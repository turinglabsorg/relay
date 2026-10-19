#include "session.h"
#include "apple_mvs.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QStandardPaths>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QTextStream>
#include <QPixmap>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <chrono>
#include <rfb/rfbclient.h>
#include <rfb/keysym.h>
#include <cstring>
#include <cstdlib>
#include <netdb.h>
#include <poll.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <zlib.h>
#include <algorithm>
#include <new>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

namespace {
struct DesktopNode : QSGNode {
    QSGRectangleNode *background = nullptr;
    QSGImageNode *desktop = nullptr;
    quint64 version = 0;
};
QString credentialHost(QString host) {
    host = host.trimmed().toLower();
    while (host.endsWith('.')) host.chop(1);
    return host;
}
QString credentialKey(QString host, int port, QString user) {
    const auto tuple = QJsonDocument(QJsonArray{credentialHost(host), port, user.trimmed()}).toJson(QJsonDocument::Compact);
    return "credentials/" + QString::fromLatin1(QCryptographicHash::hash(tuple, QCryptographicHash::Sha256).toHex());
}
int workerTag;
qint64 monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
qint64 socketReceivedBytes(int fd) {
    tcp_info info{};
    socklen_t length = sizeof(info);
    if (fd < 0 || ::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &length) != 0) return -1;
    return qint64(info.tcpi_bytes_received);
}
}

struct VncHooks {
    static VncWorker *worker(rfbClient *c) {
        return static_cast<VncWorker *>(rfbClientGetClientData(c, &workerTag));
    }
    static rfbBool allocate(rfbClient *c) {
        auto *w = worker(c);
        w->trackSocket(c->sock);
        // Bound server-controlled allocation; enough for a dual 8K desktop.
        if (c->width <= 0 || c->height <= 0 || c->width > 16384 || c->height > 16384
            || quint64(c->width) * c->height > 64 * 1024 * 1024)
            return FALSE;
        // Fluido negotiates 16 bpp: macOS Screen Sharing ignores Tight/JPEG
        // from VNC clients and sends lossless updates, so halving the pixel
        // size is what actually shrinks frames on slow links (measured:
        // ~160 KB instead of ~1.3 MB for a full 1920x1200 frame).
        const int bytes = w->m_lowBandwidth ? 2 : 4;
        auto *pixels = new (std::nothrow) uchar[qsizetype(c->width)*c->height*bytes]();
        if (!pixels) return FALSE;
        QImage image(pixels, c->width, c->height, c->width*bytes,
            w->m_lowBandwidth ? QImage::Format_RGB16 : QImage::Format_RGB32,
            [](void *data) { delete[] static_cast<uchar *>(data); }, pixels);
        if (image.isNull()) { delete[] pixels; return FALSE; }
        w->m_buffer = std::move(image);
        for (int i = 0; i < 3; ++i) { w->m_snapshots[i] = {}; w->m_snapshotSeq[i] = -1; }
        w->m_dirtyHistory.clear();
        w->m_presentSeq = 0;
        w->m_snapshotNext = 0;
        w->m_frameDirty = w->m_buffer.rect();
        w->m_dirty = true;
        c->frameBuffer = w->m_buffer.bits();
        c->format.bitsPerPixel = bytes*8;
        c->format.depth = w->m_lowBandwidth ? 16 : 24;
        c->format.trueColour = TRUE;
        c->format.bigEndian = Q_BYTE_ORDER == Q_BIG_ENDIAN;
        c->format.redMax = c->format.blueMax = w->m_lowBandwidth ? 31 : 255;
        c->format.greenMax = w->m_lowBandwidth ? 63 : 255;
        c->format.redShift = w->m_lowBandwidth ? 11 : 16;
        c->format.greenShift = w->m_lowBandwidth ? 5 : 8;
        c->format.blueShift = 0;
        return TRUE;
    }
    static void update(rfbClient *c, int x, int y, int w, int h) {
        auto *clientWorker = worker(c);
        clientWorker->noteDirty({x, y, w, h});
        // Flush queued input between rects so a large update cannot
        // hold mouse/key events until the whole frame has arrived.
        clientWorker->drainInputs(c);
    }
    static void cursor(rfbClient *c, int x, int y, int width, int height, int bpp) {
        QImage image;
        if (width > 0 && height > 0 && width <= 512 && height <= 512
            && (bpp == 4 || bpp == 2) && c->rcSource && c->rcMask) {
            image = QImage(c->rcSource, width, height, width*bpp,
                bpp == 2 ? QImage::Format_RGB16 : QImage::Format_RGB32).convertToFormat(QImage::Format_ARGB32);
            for (int row = 0; row < height; ++row) {
                auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(row));
                for (int col = 0; col < width; ++col)
                    pixels[col] = (pixels[col] & 0x00ffffff) | (c->rcMask[row*width+col] ? 0xff000000 : 0);
            }
        }
        emit worker(c)->cursorAvailable(image, QPoint(x, y));
    }
    static char *password(rfbClient *c) { worker(c)->trackSocket(c->sock); return strdup(worker(c)->m_password.constData()); }
    static rfbCredential *credential(rfbClient *c, int type) {
        if (type != rfbCredentialTypeUser) return nullptr;
        worker(c)->trackSocket(c->sock);
        auto *credential = static_cast<rfbCredential *>(calloc(1, sizeof(rfbCredential)));
        if (!credential) return nullptr;
        credential->userCredential.username = strdup(worker(c)->m_user.constData());
        credential->userCredential.password = strdup(worker(c)->m_password.constData());
        return credential;
    }
};

VncWorker::VncWorker(QString host, int port, QString user, QString password, bool lowBandwidth, QObject *parent)
    : QThread(parent), m_host(std::move(host)), m_port(port), m_lowBandwidth(lowBandwidth),
      m_user(user.toUtf8()), m_password(password.toUtf8()) {}
VncWorker::~VncWorker() { stop(); wait(); m_password.fill('\0'); }
void VncWorker::stop() {
    m_stop.store(true);
    QMutexLocker lock(&m_socketMutex);
    // Wake a partial-frame read so Cancel/Close does not wait for network traffic.
    if (m_socket >= 0) ::shutdown(m_socket, SHUT_RDWR);
}
void VncWorker::trackSocket(int socket) {
    QMutexLocker lock(&m_socketMutex);
    // Keep a duplicate descriptor so library cleanup cannot make cancellation
    // accidentally shut down a reused descriptor in another thread.
    if (m_socket < 0) m_socket = ::dup(socket);
    if (m_stop && m_socket >= 0) ::shutdown(m_socket, SHUT_RDWR);
}
bool VncWorker::sendNow(const InputEvent &event) {
    if (m_appleMode) {
        if (!m_sendFailed.load() && !appleSendInput(event)) m_sendFailed.store(true);
        return true;
    }
    auto *client = m_client.load();
    if (!client || m_sendFailed.load()) return false;
    // Input must not wait behind a frame that takes seconds to arrive on a
    // slow link: send it immediately. Tiny writes on the blocking socket,
    // serialised with the worker's queued sends by m_sendMutex.
    QMutexLocker lock(&m_sendMutex);
    if (m_client.load() != client) return false;
    const rfbBool sent = event.type == InputEvent::Pointer
        ? SendPointerEvent(client, event.x, event.y, event.mask)
        : SendKeyEvent(client, event.key, event.down);
    if (!sent) m_sendFailed.store(true);
    return true;
}
void VncWorker::enqueue(InputEvent event) {
    event.queuedAt = monotonicMs();
    if (sendNow(event)) return;
    QMutexLocker lock(&m_queueMutex);
    // Coalesce motion without discarding button transitions or key events.
    if (event.type == InputEvent::Pointer && !m_queue.empty()
        && m_queue.back().type == InputEvent::Pointer && m_queue.back().mask == event.mask)
        m_queue.back() = std::move(event);
    else m_queue.push_back(std::move(event));
}
QImage VncWorker::latestFrame() {
    QMutexLocker lock(&m_frameMutex);
    m_notificationPending = false;
    return m_latest;
}
QRect unitedDirtyRect(QRect current, QRect addition, QSize bounds) {
    if (addition.isEmpty() || bounds.isEmpty()) return current;
    addition = addition.intersected(QRect(QPoint(), bounds));
    if (addition.isEmpty()) return current;
    return current.isEmpty() ? addition : current.united(addition);
}
bool copyImageRegion(QImage &dst, const QImage &src, QRect rect) {
    if (dst.isNull() || dst.size() != src.size() || dst.format() != src.format()) return false;
    rect = rect.intersected(src.rect());
    if (rect.isEmpty()) return true;
    const int bytesPerPixel = src.depth() / 8;
    if (bytesPerPixel <= 0) return false;
    const qsizetype rowBytes = qsizetype(rect.width()) * bytesPerPixel;
    for (int row = 0; row < rect.height(); ++row) {
        auto *target = dst.scanLine(rect.y() + row) + qsizetype(rect.x()) * bytesPerPixel;
        const auto *source = src.constScanLine(rect.y() + row) + qsizetype(rect.x()) * bytesPerPixel;
        std::memcpy(target, source, size_t(rowBytes));
    }
    return true;
}
void VncWorker::noteDirty(QRect rect) {
    if (m_buffer.isNull()) return;
    const QRect clipped = unitedDirtyRect({}, rect, m_buffer.size());
    if (clipped.isEmpty()) return;
    m_dirty = true;
    m_frameDirty = unitedDirtyRect(m_frameDirty, clipped, m_buffer.size());
    ++m_statsRects;
}
bool VncWorker::drainInputs(rfbClient *client) {
    if (!client || m_sendFailed.load()) return !m_sendFailed.load();
    std::deque<InputEvent> events;
    { QMutexLocker lock(&m_queueMutex); events.swap(m_queue); }
    QMutexLocker sendLock(&m_sendMutex);
    for (auto &event : events) {
        m_maxInputDelayMs = std::max(m_maxInputDelayMs, monotonicMs() - event.queuedAt);
        const rfbBool sent = event.type == InputEvent::Pointer
            ? SendPointerEvent(client, event.x, event.y, event.mask)
            : SendKeyEvent(client, event.key, event.down);
        if (!sent) { m_sendFailed.store(true); return false; }
    }
    return true;
}
bool VncWorker::presentFrame() {
    if (m_buffer.isNull()) return false;
    const qint64 sequence = ++m_presentSeq;
    m_dirtyHistory.emplace_back(sequence, m_frameDirty);
    const int index = m_snapshotNext;
    m_snapshotNext = (m_snapshotNext + 1) % 3;
    QImage &snapshot = m_snapshots[index];
    const bool full = snapshot.size() != m_buffer.size()
        || snapshot.format() != m_buffer.format() || m_snapshotSeq[index] < 0;
    QRect region = m_buffer.rect();
    if (!full) {
        region = {};
        for (const auto &entry : m_dirtyHistory)
            if (entry.first > m_snapshotSeq[index])
                region = unitedDirtyRect(region, entry.second, m_buffer.size());
        if (region.isEmpty()) region = m_frameDirty;
    }
    if (full || !snapshot.isDetached()) snapshot = m_buffer.copy();
    else copyImageRegion(snapshot, m_buffer, region);
    m_snapshotSeq[index] = sequence;
    qint64 oldest = sequence;
    for (qint64 seen : m_snapshotSeq) if (seen >= 0) oldest = std::min(oldest, seen);
    while (!m_dirtyHistory.empty() && m_dirtyHistory.front().first <= oldest)
        m_dirtyHistory.pop_front();
    m_statsDirtyArea += qint64(m_frameDirty.width()) * m_frameDirty.height();
    ++m_statsPresented;
    bool notify = false;
    {
        QMutexLocker lock(&m_frameMutex);
        m_latest = snapshot;
        if (!m_notificationPending) { m_notificationPending = true; notify = true; }
    }
    m_dirty = false;
    m_frameDirty = {};
    return notify;
}
// ---------------------------------------------------------------------------
// Apple RFB 003.889 (Screen Sharing's own dialect), Standard mode, cleartext.
// Apple's daemon answers bare incremental FramebufferUpdateRequests on a
// physical display seconds late by design; a session that arms
// AutoFrameBufferUpdate instead gets each change pushed as captured, paced by
// how fast the socket drains. See docs/apple-889.md for the measured formats.
// ---------------------------------------------------------------------------
bool AppleReader::fill() {
    while (!stop->load()) {
        pollfd pfd{fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 100);
        if (ready < 0) { if (errno == EINTR) continue; return false; }
        if (ready == 0) continue;
        char chunk[65536];
        const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
        if (got <= 0) return false;
        buf.append(chunk, qsizetype(got));
        total += quint64(got);
        return true;
    }
    return false;
}
void AppleReader::compact() {
    if (at == 0) return;
    if (at >= buf.size()) { buf.clear(); at = 0; return; }
    if (at >= 65536) { buf.remove(0, at); at = 0; }
}
bool AppleReader::read(void *out, qsizetype size) {
    while (buf.size() - at < size) if (!fill()) return false;
    memcpy(out, buf.constData() + at, size_t(size));
    at += size;
    compact();
    return true;
}
bool AppleReader::skip(qsizetype size) {
    while (size > 0) {
        if (at >= buf.size() && !fill()) return false;
        const qsizetype take = std::min(size, buf.size() - at);
        at += take;
        size -= take;
        compact();
    }
    return true;
}
bool AppleReader::u8(quint8 &value) { return read(&value, 1); }
bool AppleReader::u16(quint16 &value) { uchar b[2]; if (!read(b, 2)) return false; value = quint16((b[0] << 8) | b[1]); return true; }
bool AppleReader::u32(quint32 &value) { uchar b[4]; if (!read(b, 4)) return false; value = (quint32(b[0]) << 24) | (quint32(b[1]) << 16) | (quint32(b[2]) << 8) | b[3]; return true; }
bool AppleReader::i32(qint32 &value) { quint32 v; if (!u32(v)) return false; value = qint32(v); return true; }
namespace {
bool sendAllFd(int fd, const char *data, qsizetype size) {
    while (size > 0) {
        const ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
        if (sent < 0) { if (errno == EINTR) continue; return false; }
        if (sent == 0) return false;
        data += sent; size -= sent;
    }
    return true;
}
quint16 be16(const QByteArray &data, qsizetype at) {
    return quint16((quint8(data[at]) << 8) | quint8(data[at + 1]));
}
quint32 be32(const QByteArray &data, qsizetype at) {
    return (quint32(quint8(data[at])) << 24) | (quint32(quint8(data[at + 1])) << 16)
        | (quint32(quint8(data[at + 2])) << 8) | quint32(quint8(data[at + 3]));
}
double beF64(const QByteArray &data, qsizetype at) {
    const quint64 bits = (quint64(be32(data, at)) << 32) | be32(data, at + 4);
    double value;
    memcpy(&value, &bits, 8);
    return value;
}
QByteArray appleArm(quint16 width, quint16 height) {
    QByteArray msg;
    msg.append(char(0x09)); msg.append(char(0));
    msg.append(char(0)); msg.append(char(1));            // version 1
    msg.append(char(0)); msg.append(char(0)); msg.append(char(0)); msg.append(char(0)); // interval 0
    const quint16 rect[4] = {0, 0, width, height};
    for (quint16 v : rect) { msg.append(char(v >> 8)); msg.append(char(v)); }
    return msg;
}
QByteArray appleUpdateRequest(bool incremental, quint16 width, quint16 height) {
    QByteArray msg;
    msg.append(char(3)); msg.append(char(incremental ? 1 : 0));
    const quint16 rect[4] = {0, 0, width, height};
    for (quint16 v : rect) { msg.append(char(v >> 8)); msg.append(char(v)); }
    return msg;
}
} // namespace

bool VncWorker::appleWriteAll(const QByteArray &data) {
    QMutexLocker lock(&m_sendMutex);
    return m_appleFd >= 0 && sendAllFd(m_appleFd, data.constData(), data.size());
}
bool VncWorker::appleSendInput(const InputEvent &event) {
    QByteArray msg;
    if (event.type == InputEvent::Pointer) {
        // On 003.889 the daemon reads the mask as CGMouseButton numbers:
        // right and middle trade places against the RFB convention.
        int mask = (event.mask & 1) | (event.mask & 2 ? 4 : 0) | (event.mask & 4 ? 2 : 0) | (event.mask & ~7);
        const int x = std::clamp(int(std::lround(event.x / m_appleScale)), 0, 65535);
        const int y = std::clamp(int(std::lround(event.y / m_appleScale)), 0, 65535);
        msg.append(char(5)); msg.append(char(mask));
        msg.append(char(x >> 8)); msg.append(char(x)); msg.append(char(y >> 8)); msg.append(char(y));
    } else {
        quint32 keysym = event.key;
        // The daemon maps Alt keysyms to Command; Option is Meta there.
        if (keysym == XK_Alt_L) keysym = XK_Meta_L;
        if (keysym == XK_Alt_R) keysym = XK_Meta_R;
        msg.append(char(4)); msg.append(char(event.down ? 1 : 0)); msg.append(char(0)); msg.append(char(0));
        msg.append(char(keysym >> 24)); msg.append(char(keysym >> 16)); msg.append(char(keysym >> 8)); msg.append(char(keysym));
    }
    return appleWriteAll(msg);
}

bool VncWorker::tryApple() {
    // Probe the greeting on our own connection: only a Mac answers 003.889.
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    const QByteArray hostUtf8 = m_host.toUtf8();
    if (::getaddrinfo(hostUtf8.constData(), QByteArray::number(m_port).constData(), &hints, &addresses) != 0)
        return false;
    int fd = -1;
    for (addrinfo *info = addresses; info && fd < 0; info = info->ai_next) {
        fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, info->ai_addr, info->ai_addrlen) != 0) { ::close(fd); fd = -1; }
    }
    ::freeaddrinfo(addresses);
    if (fd < 0) return false;
    const int noDelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
    AppleReader reader;
    reader.fd = fd;
    reader.stop = &m_stop;
    char greeting[12];
    const bool isApple = reader.read(greeting, 12)
        && QByteArray(greeting, 12) == "RFB 003.889\n";
    if (!isApple || m_stop) {
        // Not an Apple server: close fully (no tracked duplicate exists yet)
        // and let the LibVNC path make its own connection.
        ::close(fd);
        return false;
    }
    QTextStream(stderr) << "Relay: Apple 003.889 server, using Screen Sharing dialect\n";
    return appleSession(fd);
}

bool VncWorker::appleSession(int fd) {
    AppleReader reader;
    reader.fd = fd;
    reader.stop = &m_stop;
    bool authed = false;
    auto finish = [&](const QString &reason) {
        m_appleMode = false;
        m_appleFd = -1;
        if (m_appleInflate) {
            inflateEnd(static_cast<z_stream *>(m_appleInflate));
            delete static_cast<z_stream *>(m_appleInflate);
            m_appleInflate = nullptr;
        }
        { QMutexLocker lock(&m_socketMutex); if (m_socket >= 0) { ::close(m_socket); m_socket = -1; } }
        ::close(fd);
        if (authed) emit ended(m_stop ? QString() : reason);
        return authed;
    };
    if (!sendAllFd(fd, "RFB 003.889\n", 12)) return finish({});
    quint8 typeCount = 0;
    if (!reader.u8(typeCount) || typeCount == 0 || typeCount > 64) return finish({});
    QByteArray types(typeCount, 0);
    if (!reader.read(types.data(), typeCount) || !types.contains(char(30))) return finish({});
    if (!sendAllFd(fd, "\x1e", 1)) return finish({});

    // Diffie-Hellman (security type 30), as measured from Apple's viewer.
    quint16 generator = 0, keyLen = 0;
    if (!reader.u16(generator) || !reader.u16(keyLen) || keyLen < 64 || keyLen > 1024)
        return finish({});
    QByteArray prime(keyLen, 0), peerPublic(keyLen, 0);
    if (!reader.read(prime.data(), keyLen) || !reader.read(peerPublic.data(), keyLen))
        return finish({});
    QByteArray wrapKey;
    {
        using BigNum = std::unique_ptr<BIGNUM, decltype(&::BN_free)>;
        BigNum modulus(BN_bin2bn(reinterpret_cast<const uchar *>(prime.constData()), keyLen, nullptr), &::BN_free);
        BigNum peer(BN_bin2bn(reinterpret_cast<const uchar *>(peerPublic.constData()), keyLen, nullptr), &::BN_free);
        BigNum gen(BN_new(), &::BN_free);
        BigNum priv(BN_new(), &::BN_free);
        QByteArray privBytes(2 * keyLen, 0), publicPad(keyLen, 0), secretPad(keyLen, 0);
        bool ok = modulus && peer && gen && priv
            && BN_set_word(gen.get(), generator) == 1
            && RAND_bytes(reinterpret_cast<uchar *>(privBytes.data()), privBytes.size()) == 1
            && BN_bin2bn(reinterpret_cast<const uchar *>(privBytes.constData()), privBytes.size(), priv.get()) != nullptr;
        std::unique_ptr<BN_CTX, decltype(&::BN_CTX_free)> ctx(BN_CTX_new(), &::BN_CTX_free);
        BigNum pub(BN_new(), &::BN_free), secret(BN_new(), &::BN_free);
        ok = ok && ctx && pub && secret
            && BN_mod_exp(pub.get(), gen.get(), priv.get(), modulus.get(), ctx.get()) == 1
            && BN_mod_exp(secret.get(), peer.get(), priv.get(), modulus.get(), ctx.get()) == 1
            && BN_bn2binpad(pub.get(), reinterpret_cast<uchar *>(publicPad.data()), keyLen) == keyLen
            && BN_bn2binpad(secret.get(), reinterpret_cast<uchar *>(secretPad.data()), keyLen) == keyLen;
        if (!ok) return finish({});
        wrapKey.resize(16);
        size_t mdLen = 0;
        if (EVP_Q_digest(nullptr, "MD5", nullptr, secretPad.constData(), keyLen,
                         reinterpret_cast<uchar *>(wrapKey.data()), &mdLen) != 1 || mdLen != 16)
            return finish({});
        // Credential block: user at 0, password at 64, random filler elsewhere.
        QByteArray blob(128, 0);
        if (RAND_bytes(reinterpret_cast<uchar *>(blob.data()), blob.size()) != 1) return finish({});
        auto place = [&](const QByteArray &field, qsizetype offset) {
            memcpy(blob.data() + offset, field.constData(), field.size());
            blob[offset + field.size()] = 0;
        };
        if (m_user.size() >= 64 || m_password.size() >= 64) return finish({});
        place(m_user, 0);
        place(m_password, 64);
        // AES-128-ECB under MD5(shared secret).
        std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> cipher(EVP_CIPHER_CTX_new(), &::EVP_CIPHER_CTX_free);
        QByteArray encrypted(128, 0);
        int outLen = 0;
        ok = cipher
            && EVP_EncryptInit_ex(cipher.get(), EVP_aes_128_ecb(), nullptr,
                                  reinterpret_cast<const uchar *>(wrapKey.constData()), nullptr) == 1
            && EVP_CIPHER_CTX_set_padding(cipher.get(), 0) == 1
            && EVP_EncryptUpdate(cipher.get(), reinterpret_cast<uchar *>(encrypted.data()), &outLen,
                                 reinterpret_cast<const uchar *>(blob.constData()), blob.size()) == 1
            && outLen == blob.size();
        if (!ok) return finish({});
        if (!sendAllFd(fd, encrypted.constData(), encrypted.size())
            || !sendAllFd(fd, publicPad.constData(), publicPad.size()))
            return finish({});
    }
    quint32 result = 1;
    if (!reader.u32(result) || result != 0) {
        QTextStream(stderr) << "Relay: Apple authentication refused (" << result << ")\n";
        return finish({});
    }
    if (!sendAllFd(fd, "\x81", 1)) return finish({});
    quint16 deskW = 0, deskH = 0;
    uchar formatIgnored[16];
    quint32 nameLen = 0;
    if (!reader.u16(deskW) || !reader.u16(deskH) || deskW == 0 || deskH == 0
        || !reader.read(formatIgnored, 16) || !reader.u32(nameLen) || nameLen > (1u << 20))
        return finish({});
    QByteArray deskName(nameLen, 0);
    if (nameLen && !reader.read(deskName.data(), nameLen)) return finish({});
    QTextStream(stderr) << "Relay: Apple desktop " << deskW << 'x' << deskH << '\n';

    m_appleFd = fd;
    // Prelude, byte for byte as measured from Apple's viewer (cleartext form).
    QByteArray viewerInfo(66, 0);
    viewerInfo[0] = char(0x21);
    viewerInfo[2] = 0; viewerInfo[3] = 62;
    viewerInfo[4] = 0; viewerInfo[5] = 1;
    viewerInfo[9] = 2;
    viewerInfo[13] = 6; viewerInfo[17] = 1;
    viewerInfo[25] = 26; viewerInfo[29] = 6; viewerInfo[33] = 2;
    viewerInfo[34] = char(0xb0); viewerInfo[36] = 0x0c; viewerInfo[37] = 0x03;
    viewerInfo[38] = char(0x90); viewerInfo[44] = 0x40;
    const uchar setMode[4] = {0x0a, 0, 0, 1};
    if (!appleWriteAll(viewerInfo) || !appleWriteAll(QByteArray(reinterpret_cast<const char *>(setMode), 4)))
        return finish({});

    // Pixel format in the negotiated profile, as in the libVNC path.
    const int bytesPerPixel = m_lowBandwidth ? 2 : 4;
    QByteArray pixelFormat(20, 0);
    pixelFormat[0] = 0;
    pixelFormat[4] = char(bytesPerPixel * 8);
    pixelFormat[5] = char(m_lowBandwidth ? 16 : 24);
    pixelFormat[7] = 1;
    auto put16 = [&](int at, quint16 v) { pixelFormat[at] = char(v >> 8); pixelFormat[at + 1] = char(v); };
    put16(8, m_lowBandwidth ? 31 : 255);
    put16(10, m_lowBandwidth ? 63 : 255);
    put16(12, m_lowBandwidth ? 31 : 255);
    pixelFormat[14] = char(m_lowBandwidth ? 11 : 16);
    pixelFormat[15] = char(m_lowBandwidth ? 5 : 8);
    pixelFormat[16] = 0;
    if (!appleWriteAll(pixelFormat)) return finish({});

    // The Mac treats this list as a preference order and will send raw for
    // the whole desktop when raw is first. Qualità asks for Apple's adaptive
    // DCT codec (1011) first: that is what makes Screen Sharing small on a
    // photographic page. Fluido stays lossless and asks for ZRLE before the
    // 16-bit codec, because that codec is deflate level 1 and larger on a
    // slow link. Both fall through to CopyRect, flat zlib and raw.
    qint32 encodings[16];
    int encodingCount = 0;
    if (!m_lowBandwidth) encodings[encodingCount++] = 0x3f3;
    encodings[encodingCount++] = 16;
    if (m_lowBandwidth) encodings[encodingCount++] = 0x3ea;
    for (qint32 encoding : {qint32(1), qint32(6), qint32(0x44c), qint32(0x44d), qint32(0x451),
            qint32(0x453), qint32(0x455), qint32(-223), qint32(-224), qint32(0)})
        encodings[encodingCount++] = encoding;
    QTextStream(stderr) << "Relay: Apple encodings"
        << (m_lowBandwidth ? " Fluido (ZRLE, RGB555, CopyRect, zlib)"
                           : " Qualità (adaptive, ZRLE, CopyRect, zlib)")
        << ", raw last\n";
    QByteArray setEncodings;
    setEncodings.append(char(2)); setEncodings.append(char(0));
    setEncodings.append(char(0)); setEncodings.append(char(encodingCount));
    for (int index = 0; index < encodingCount; ++index) {
        const qint32 encoding = encodings[index];
        setEncodings.append(char(encoding >> 24)); setEncodings.append(char(encoding >> 16));
        setEncodings.append(char(encoding >> 8)); setEncodings.append(char(encoding));
    }
    if (!appleWriteAll(setEncodings)) return finish({});
    if (!appleAllocBuffer(deskW, deskH)) return finish({});
    trackSocket(fd);
    m_appleMode = true;
    authed = true;
    emit authenticated();
    if (!appleWriteAll(appleUpdateRequest(false, deskW, deskH))
        || !appleWriteAll(appleArm(deskW, deskH)))
        return finish("Il collegamento con il Mac è stato interrotto. Puoi riconnetterti.");
    appleReadLoop(reader);
    return finish("Il collegamento con il Mac è stato interrotto. Puoi riconnetterti.");
}

bool VncWorker::appleAllocBuffer(int width, int height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return false;
    const int bytes = m_lowBandwidth ? 2 : 4;
    auto *pixels = new (std::nothrow) uchar[qsizetype(width) * height * bytes]();
    if (!pixels) return false;
    QImage image(pixels, width, height, width * bytes,
        m_lowBandwidth ? QImage::Format_RGB16 : QImage::Format_RGB32,
        [](void *data) { delete[] static_cast<uchar *>(data); }, pixels);
    if (image.isNull()) { delete[] pixels; return false; }
    m_buffer = image;
    m_frameDirty = m_buffer.rect();
    m_dirty = true;
    for (auto &snapshot : m_snapshots) snapshot = {};
    for (auto &sequence : m_snapshotSeq) sequence = -1;
    m_dirtyHistory.clear();
    return true;
}

bool VncWorker::appleLayout(const QByteArray &payload) {
    if (payload.size() < 0x14) return true;
    const int fbW = be16(payload, 6), fbH = be16(payload, 8);
    const int count = be16(payload, 0x12);
    if (fbW > 0 && fbH > 0 && (fbW != m_buffer.width() || fbH != m_buffer.height())) {
        if (!appleAllocBuffer(fbW, fbH)) return false;
        // The repaint requested below fills the new buffer; never present
        // the blank allocation in between.
        m_dirty = false;
        m_frameDirty = {};
    }
    if (count >= 1 && payload.size() >= 0x14 + 0x38) {
        const double scale = beF64(payload, 0x14 + 8);
        if (scale > 0 && scale <= 1) m_appleScale = scale;
    }
    // Fluido asks the Mac itself to render a smaller framebuffer; the local
    // display is untouched and pointer coordinates are unscaled on the way in.
    if (m_lowBandwidth && !m_appleScaleSent && count >= 1 && m_buffer.width() > 1440) {
        const double factor = 1440.0 / m_buffer.width();
        quint64 bits;
        memcpy(&bits, &factor, 8);
        QByteArray msg;
        msg.append(char(0x08)); msg.append(char(0));
        for (int i = 7; i >= 0; --i) msg.append(char(bits >> (8 * i)));
        if (!appleWriteAll(msg)) return false;
        m_appleScaleSent = true;
        QTextStream(stderr) << "Relay: Apple server scaling " << factor << '\n';
    }
    // A layout drops the Mac's arming: re-arm and ask for a repaint.
    return appleWriteAll(appleArm(quint16(m_buffer.width()), quint16(m_buffer.height())))
        && appleWriteAll(appleUpdateRequest(false, quint16(m_buffer.width()), quint16(m_buffer.height())));
}

namespace {
void copyFrameRect(QImage &image, int x, int y, int w, int h, int sx, int sy) {
    const QRect dest = QRect(x, y, w, h).intersected(image.rect());
    if (dest.isEmpty() || image.isNull()) return;
    const QRect source(sx + (dest.x() - x), sy + (dest.y() - y), dest.width(), dest.height());
    if (!image.rect().contains(source)) return;
    const int bytesPerPixel = image.depth() / 8;
    if (bytesPerPixel <= 0) return;
    const qsizetype rowBytes = qsizetype(source.width()) * bytesPerPixel;
    QByteArray taken(rowBytes * source.height(), 0);
    for (int row = 0; row < source.height(); ++row)
        memcpy(taken.data() + row * rowBytes,
            image.constScanLine(source.y() + row) + qsizetype(source.x()) * bytesPerPixel, size_t(rowBytes));
    for (int row = 0; row < source.height(); ++row)
        memcpy(image.scanLine(dest.y() + row) + qsizetype(dest.x()) * bytesPerPixel,
            taken.constData() + row * rowBytes, size_t(rowBytes));
}
// Apple codec 1002. noVNC-ARD's live decoder reads this as big-endian RGB555,
// two bytes per pixel, on the same zlib stream as encoding 6.
void blitRgb555(QImage &image, int x, int y, int w, int h, const QByteArray &pixels) {
    const QRect target = QRect(x, y, w, h).intersected(image.rect());
    if (target.isEmpty() || pixels.size() < w * h * 2) return;
    const bool rgb16 = image.format() == QImage::Format_RGB16;
    for (int row = target.top(); row <= target.bottom(); ++row) {
        const auto *src = reinterpret_cast<const uchar *>(pixels.constData())
            + (qsizetype(row - y) * w + (target.left() - x)) * 2;
        if (rgb16) {
            auto *dst = reinterpret_cast<quint16 *>(image.scanLine(row)) + target.left();
            for (int col = 0; col < target.width(); ++col, src += 2) {
                const int word = (int(src[0]) << 8) | src[1];
                const int r5 = (word >> 10) & 31, g5 = (word >> 5) & 31, b5 = word & 31;
                const int g6 = (g5 << 1) | (g5 >> 4);
                dst[col] = quint16((r5 << 11) | (g6 << 5) | b5);
            }
        } else {
            auto *dst = reinterpret_cast<QRgb *>(image.scanLine(row)) + target.left();
            for (int col = 0; col < target.width(); ++col, src += 2) {
                const int word = (int(src[0]) << 8) | src[1];
                const int r5 = (word >> 10) & 31, g5 = (word >> 5) & 31, b5 = word & 31;
                dst[col] = qRgb((r5 << 3) | (r5 >> 2), (g5 << 3) | (g5 >> 2), (b5 << 3) | (b5 >> 2));
            }
        }
    }
}

struct ZrleIn {
    const uchar *cursor = nullptr;
    const uchar *end = nullptr;
    bool take(void *out, int size) {
        if (size < 0 || end - cursor < size) return false;
        if (size) std::memcpy(out, cursor, size_t(size));
        cursor += size;
        return true;
    }
    bool u8(quint8 &value) { if (cursor >= end) return false; value = *cursor++; return true; }
};

bool zrleRun(ZrleIn &in, int &run) {
    run = 1;
    quint8 byte = 0;
    do {
        if (!in.u8(byte)) return false;
        run += byte;
    } while (byte == 255);
    return run > 0;
}

bool zrlePixel(ZrleIn &in, bool low, quint16 &packed, QRgb &color) {
    if (low) {
        uchar bytes[2];
        if (!in.take(bytes, 2)) return false;
        packed = quint16(bytes[0] | (bytes[1] << 8));
        return true;
    }
    uchar bytes[3];
    if (!in.take(bytes, 3)) return false;
    color = qRgb(bytes[2], bytes[1], bytes[0]);
    return true;
}

void zrlePut(QImage &image, int x, int y, bool low, quint16 packed, QRgb color) {
    if (!image.rect().contains(x, y)) return;
    if (low) reinterpret_cast<quint16 *>(image.scanLine(y))[x] = packed;
    else reinterpret_cast<QRgb *>(image.scanLine(y))[x] = color;
}

bool zrleTile(ZrleIn &in, QImage &image, int x, int y, int w, int h, bool low) {
    quint8 sub = 0;
    if (!in.u8(sub)) return false;
    const int paletteSize = sub & 127;
    const bool rle = sub & 128;
    const int total = w * h;
    auto paint = [&](int index, quint16 packed, QRgb color) {
        zrlePut(image, x + index % w, y + index / w, low, packed, color);
    };
    if (!rle && paletteSize == 0) {
        for (int i = 0; i < total; ++i) {
            quint16 packed = 0; QRgb color = 0;
            if (!zrlePixel(in, low, packed, color)) return false;
            paint(i, packed, color);
        }
        return true;
    }
    if (!rle && paletteSize == 1) {
        quint16 packed = 0; QRgb color = 0;
        if (!zrlePixel(in, low, packed, color)) return false;
        for (int i = 0; i < total; ++i) paint(i, packed, color);
        return true;
    }
    if (!rle && paletteSize >= 2 && paletteSize <= 16) {
        quint16 packedPalette[16];
        QRgb colors[16];
        for (int i = 0; i < paletteSize; ++i)
            if (!zrlePixel(in, low, packedPalette[i], colors[i])) return false;
        const int bits = paletteSize == 2 ? 1 : paletteSize <= 4 ? 2 : 4;
        const int rowBytes = (w * bits + 7) / 8;
        const int mask = (1 << bits) - 1;
        for (int row = 0; row < h; ++row) {
            uchar raw[64];
            if (rowBytes > 64 || !in.take(raw, rowBytes)) return false;
            int bit = 0;
            for (int col = 0; col < w; ++col) {
                const int index = (raw[bit / 8] >> (8 - bits - (bit % 8))) & mask;
                bit += bits;
                if (index >= paletteSize) return false;
                zrlePut(image, x + col, y + row, low, packedPalette[index], colors[index]);
            }
        }
        return true;
    }
    if (rle && paletteSize == 0) {
        for (int filled = 0; filled < total;) {
            quint16 packed = 0; QRgb color = 0;
            int run = 0;
            if (!zrlePixel(in, low, packed, color) || !zrleRun(in, run) || filled + run > total) return false;
            for (int i = 0; i < run; ++i) paint(filled + i, packed, color);
            filled += run;
        }
        return true;
    }
    if (rle && paletteSize >= 2 && paletteSize <= 127) {
        quint16 packedPalette[128];
        QRgb colors[128];
        for (int i = 0; i < paletteSize; ++i)
            if (!zrlePixel(in, low, packedPalette[i], colors[i])) return false;
        for (int filled = 0; filled < total;) {
            quint8 lead = 0;
            int run = 1;
            if (!in.u8(lead)) return false;
            const int index = lead & 127;
            if (index >= paletteSize) return false;
            if ((lead & 128) && !zrleRun(in, run)) return false;
            if (filled + run > total) return false;
            for (int i = 0; i < run; ++i) paint(filled + i, packedPalette[index], colors[index]);
            filled += run;
        }
        return true;
    }
    return false;
}

bool inflateChunk(z_stream *stream, const uchar *input, unsigned length, QByteArray &out) {
    stream->next_in = const_cast<Bytef *>(input);
    stream->avail_in = length;
    out.clear();
    while (stream->avail_in > 0) {
        const int have = int(out.size());
        out.resize(have + 65536);
        stream->next_out = reinterpret_cast<Bytef *>(out.data() + have);
        stream->avail_out = 65536;
        const int ret = inflate(stream, Z_NO_FLUSH);
        out.resize(have + int(65536 - stream->avail_out));
        if (ret != Z_OK && ret != Z_STREAM_END) return false;
        if (out.size() > (32 << 20)) return false;
        if (ret == Z_STREAM_END) break;
    }
    return stream->avail_in == 0;
}

bool decodeZrle(z_stream *stream, const QByteArray &chunk, QImage &image, int x, int y, int w, int h, bool low) {
    if (w <= 0 || h <= 0) return true;
    QByteArray raw;
    if (!inflateChunk(stream, reinterpret_cast<const uchar *>(chunk.constData()), unsigned(chunk.size()), raw))
        return false;
    ZrleIn in{reinterpret_cast<const uchar *>(raw.constData()),
        reinterpret_cast<const uchar *>(raw.constData()) + raw.size()};
    for (int tileY = 0; tileY < h; tileY += 64) {
        const int tileH = std::min(64, h - tileY);
        for (int tileX = 0; tileX < w; tileX += 64) {
            const int tileW = std::min(64, w - tileX);
            if (!zrleTile(in, image, x + tileX, y + tileY, tileW, tileH, low)) return false;
        }
    }
    return true;
}

QByteArray deflateSync(z_stream &stream, const QByteArray &raw) {
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(raw.constData()));
    stream.avail_in = uInt(raw.size());
    QByteArray out;
    int ret = Z_OK;
    do {
        const int have = int(out.size());
        out.resize(have + int(raw.size()) + 64);
        stream.next_out = reinterpret_cast<Bytef *>(out.data() + have);
        stream.avail_out = uInt(out.size() - have);
        ret = deflate(&stream, Z_SYNC_FLUSH);
        out.resize(have + int(out.size() - have - stream.avail_out));
    } while (ret == Z_OK && stream.avail_out == 0);
    return ret == Z_OK ? out : QByteArray();
}

bool zrleSelfTest() {
    auto fail = [](const char *name) {
        QTextStream(stderr) << "ZRLE self-test failed: " << name << '\n';
        return false;
    };
    z_stream inflating{};
    z_stream deflating{};
    if (inflateInit(&inflating) != Z_OK || deflateInit(&deflating, 6) != Z_OK) return fail("zlib");
    const auto finish = [&] { inflateEnd(&inflating); deflateEnd(&deflating); };
    QImage image(65, 2, QImage::Format_RGB32);
    image.fill(Qt::black);
    QByteArray solid;
    solid.append(char(1));
    solid.append(char(0xAB)); solid.append(char(0x34)); solid.append(char(0x12));
    solid.append(char(1));
    solid.append(char(0x10)); solid.append(char(0x20)); solid.append(char(0x30));
    const QByteArray first = deflateSync(deflating, solid);
    if (!decodeZrle(&inflating, first, image, 0, 0, 65, 1, false)) { finish(); return fail("solid"); }
    if (image.pixelColor(0, 0) != QColor("#1234ab") || image.pixelColor(64, 0) != QColor("#302010")) {
        finish(); return fail("solid pixels");
    }
    QByteArray packed;
    packed.append(char(2));
    packed.append(char(0x00)); packed.append(char(0x00)); packed.append(char(0xFF));
    packed.append(char(0xAB)); packed.append(char(0x34)); packed.append(char(0x12));
    packed.append(char(0x50));
    const QByteArray second = deflateSync(deflating, packed);
    if (!decodeZrle(&inflating, second, image, 0, 1, 4, 1, false)) { finish(); return fail("palette"); }
    if (image.pixelColor(0, 1) != QColor("#ff0000") || image.pixelColor(1, 1) != QColor("#1234ab")
        || image.pixelColor(2, 1) != QColor("#ff0000")) {
        finish(); return fail("palette pixels");
    }
    QByteArray runs;
    runs.append(char(128));
    runs.append(char(0x00)); runs.append(char(0x00)); runs.append(char(0x00));
    runs.append(char(2));
    runs.append(char(0x00)); runs.append(char(0xFF)); runs.append(char(0x00));
    runs.append(char(0));
    if (!decodeZrle(&inflating, deflateSync(deflating, runs), image, 0, 0, 4, 1, false)) { finish(); return fail("rle"); }
    if (image.pixelColor(0, 0) != QColor("#000000") || image.pixelColor(2, 0) != QColor("#000000")
        || image.pixelColor(3, 0) != QColor("#00ff00")) {
        finish(); return fail("rle pixels");
    }
    QImage low(3, 1, QImage::Format_RGB16);
    low.fill(Qt::black);
    z_stream lowIn{};
    z_stream lowDef{};
    if (inflateInit(&lowIn) != Z_OK || deflateInit(&lowDef, 6) != Z_OK) { finish(); return fail("low zlib"); }
    QByteArray lowRaw;
    lowRaw.append(char(1));
    lowRaw.append(char(0x34)); lowRaw.append(char(0x12));
    const bool lowOk = decodeZrle(&lowIn, deflateSync(lowDef, lowRaw), low, 0, 0, 3, 1, true);
    const quint16 stored = reinterpret_cast<const quint16 *>(low.constScanLine(0))[2];
    inflateEnd(&lowIn); deflateEnd(&lowDef); finish();
    if (!lowOk || stored != 0x1234) return fail("rgb565");
    return true;
}
} // namespace

bool relayCodecSelfTest() { return zrleSelfTest() && AppleMvsDecoder::selfTest(); }

bool VncWorker::appleReadLoop(AppleReader &reader) {
    auto *zs = new z_stream{};
    if (inflateInit(zs) != Z_OK) { delete zs; return false; }
    m_appleInflate = zs;
    z_stream zrle{};
    if (inflateInit(&zrle) != Z_OK) return false;
    AppleMvsDecoder mvs;
    const int bytesPerPixel = m_lowBandwidth ? 2 : 4;
    QElapsedTimer frameClock; frameClock.start();
    QElapsedTimer statsClock; statsClock.start();
    QElapsedTimer responseClock; responseClock.start();
    qint64 maxDecodeMs = 0, maxResponseMs = 0;
    quint64 lastBytes = reader.total;
    int updates = 0;
    qint32 seenEnc[8]{};
    int seenCount[8]{};
    quint64 seenBytes[8]{};
    int seenN = 0;
    auto noteEnc = [&](qint32 encoding, quint64 bytes) {
        for (int index = 0; index < seenN; ++index) {
            if (seenEnc[index] != encoding) continue;
            ++seenCount[index];
            seenBytes[index] += bytes;
            return;
        }
        if (seenN < 8) {
            seenEnc[seenN] = encoding;
            seenCount[seenN] = 1;
            seenBytes[seenN] = bytes;
            ++seenN;
        }
    };
    bool ok = true;
    auto blit = [&](int x, int y, int w, int h, const QByteArray &pixels) {
        const QRect target = QRect(x, y, w, h).intersected(m_buffer.rect());
        if (target.isEmpty()) return;
        const qsizetype rowBytes = qsizetype(w) * bytesPerPixel;
        for (int row = target.top(); row <= target.bottom(); ++row)
            memcpy(m_buffer.scanLine(row) + qsizetype(target.left()) * bytesPerPixel,
                   pixels.constData() + qsizetype(row - y) * rowBytes + qsizetype(target.left() - x) * bytesPerPixel,
                   qsizetype(target.width()) * bytesPerPixel);
        noteDirty(target);
    };
    auto periodic = [&] {
        if (m_dirty) {
            const qint64 framePixels = qint64(m_buffer.width()) * m_buffer.height();
            const qint64 dirtyPixels = qint64(m_frameDirty.width()) * m_frameDirty.height();
            const qint64 interval = framePixels > 0 && dirtyPixels * 4 <= framePixels ? 8 : 16;
            if (frameClock.elapsed() >= interval) {
                const bool notify = presentFrame();
                frameClock.restart();
                if (notify) emit frameAvailable();
            }
        }
        if (statsClock.elapsed() >= 5000) {
            const qint64 framePixels = qint64(m_buffer.width()) * m_buffer.height();
            const qint64 dirtyPercent = m_statsPresented > 0 && framePixels > 0
                ? m_statsDirtyArea * 100 / (qint64(m_statsPresented) * framePixels) : 0;
            const qint64 receivedDelta = qint64(reader.total - lastBytes);
            lastBytes = reader.total;
            QString codecs;
            for (int index = 0; index < seenN; ++index) {
                if (!codecs.isEmpty()) codecs += QStringLiteral(", ");
                codecs += QString::number(seenEnc[index]) + QLatin1Char('x') + QString::number(seenCount[index])
                    + QLatin1Char('/') + QString::number(seenBytes[index]) + QLatin1Char('B');
            }
            QTextStream(stderr) << "Relay performance (Apple): " << updates << " messages, "
                << m_statsPresented << " presented, " << m_statsRects << " rects / "
                << statsClock.elapsed() << " ms; dirty ~" << dirtyPercent
                << "%/frame; received " << receivedDelta << " B; max response " << maxResponseMs
                << " ms; max receive/decode " << maxDecodeMs << " ms"
                << (codecs.isEmpty() ? QString() : QStringLiteral("; enc ") + codecs) << '\n';
            statsClock.restart(); updates = 0; maxDecodeMs = 0; maxResponseMs = 0;
            m_statsRects = 0; m_statsPresented = 0; m_statsDirtyArea = 0;
            seenN = 0;
        }
    };
    while (ok && !m_stop) {
        if (reader.at >= reader.buf.size()) {
            pollfd pfd{reader.fd, POLLIN, 0};
            const int ready = ::poll(&pfd, 1, 10);
            if (ready == 0) { periodic(); continue; }
            if (ready < 0 && errno != EINTR) break;
            if (ready < 0) continue;
        }
        quint8 type = 0;
        if (!reader.u8(type)) break;
        if (m_sendFailed.load()) break;
        if (type == 2) continue;
        if (type == 3) {
            quint32 len = 0;
            if (!reader.skip(3) || !reader.u32(len) || !reader.skip(len)) break;
            continue;
        }
        if (type == 0x14) {
            quint8 pad = 0;
            quint16 len = 0;
            QByteArray body;
            if (!reader.u8(pad) || !reader.u16(len)) break;
            body.resize(len);
            if (len && !reader.read(body.data(), len)) break;
            if (len >= 4 && be16(body, 2) == 1) { QTextStream(stderr) << "Relay: Apple session ended by the Mac\n"; break; }
            continue;
        }
        if (type != 0) {
            QTextStream(stderr) << "Relay: Apple unknown server message " << int(type) << '\n';
            break;
        }
        QElapsedTimer decodeClock; decodeClock.start();
        quint8 pad = 0;
        quint16 nRects = 0;
        if (!reader.u8(pad) || !reader.u16(nRects)) break;
        for (int i = 0; i < nRects && ok; ++i) {
            quint16 x = 0, y = 0, w = 0, h = 0;
            qint32 enc = 0;
            if (!reader.u16(x) || !reader.u16(y) || !reader.u16(w) || !reader.u16(h) || !reader.i32(enc)) { ok = false; break; }
            if (enc == 0 || enc == 6 || enc == 0x3ea) {
                const int pixelBytes = enc == 0x3ea ? 2 : bytesPerPixel;
                const quint64 need = quint64(w) * h * quint64(pixelBytes);
                if (need > (64ull << 20)) { ok = false; break; }
                QByteArray pixels(qsizetype(need), 0);
                quint32 compressed = 0;
                if (enc == 0) {
                    ok = need == 0 || reader.read(pixels.data(), pixels.size());
                } else {
                    QByteArray chunk;
                    if (!reader.u32(compressed) || compressed > (64u << 20)) { ok = false; break; }
                    chunk.resize(compressed);
                    if (compressed && !reader.read(chunk.data(), compressed)) { ok = false; break; }
                    zs->next_in = reinterpret_cast<Bytef *>(chunk.data());
                    zs->avail_in = compressed;
                    zs->next_out = reinterpret_cast<Bytef *>(pixels.data());
                    zs->avail_out = uInt(pixels.size());
                    const int ret = inflate(zs, Z_NO_FLUSH);
                    ok = (ret == Z_OK || ret == Z_STREAM_END) && zs->avail_in == 0 && zs->avail_out == 0;
                    if (!ok) QTextStream(stderr) << "Relay: Apple compressed rectangle " << enc
                        << " failed (" << ret << ")\n";
                }
                if (!ok) break;
                noteEnc(enc, enc == 0 ? quint64(pixels.size()) : compressed);
                if (enc == 0x3ea) {
                    blitRgb555(m_buffer, x, y, w, h, pixels);
                    noteDirty(QRect(x, y, w, h));
                } else blit(x, y, w, h, pixels);
            } else if (enc == 16 || enc == 0x3f3) {
                quint32 compressed = 0;
                QByteArray chunk;
                if (!reader.u32(compressed) || compressed > (32u << 20)) { ok = false; break; }
                chunk.resize(compressed);
                if (compressed && !reader.read(chunk.data(), compressed)) { ok = false; break; }
                noteEnc(enc, compressed);
                if (enc == 16) {
                    ok = decodeZrle(&zrle, chunk, m_buffer, x, y, w, h, m_lowBandwidth);
                    if (!ok) QTextStream(stderr) << "Relay: Apple ZRLE rectangle failed\n";
                } else if (m_buffer.format() == QImage::Format_RGB32
                    && !mvs.decode(reinterpret_cast<const uchar *>(chunk.constData()), int(chunk.size()), x, y, w, h, m_buffer)) {
                    QTextStream(stderr) << "Relay: Apple adaptive rectangle failed\n";
                }
                if (ok) noteDirty(QRect(x, y, w, h));
            } else if (enc == 1) {
                quint16 sx = 0, sy = 0;
                if (!reader.u16(sx) || !reader.u16(sy)) { ok = false; break; }
                noteEnc(1, 4);
                copyFrameRect(m_buffer, x, y, w, h, sx, sy);
                noteDirty(QRect(x, y, w, h));
            } else if (enc == 0x451) {
                quint16 declared = 0;
                QByteArray payload;
                if (!reader.u16(declared)) { ok = false; break; }
                payload.resize(declared);
                if (declared && !reader.read(payload.data(), declared)) { ok = false; break; }
                ok = appleLayout(payload);
            } else if (enc == 0x44c || enc == -224) {
                // Cursor position / last rectangle: no payload.
            } else if (enc == -223) {
                if (!appleAllocBuffer(w, h)) { ok = false; break; }
            } else if (enc == 0x44d) {
                uchar head[10];
                if (!reader.read(head, 10)) { ok = false; break; }
                ok = reader.skip(qsizetype(be16(QByteArray(reinterpret_cast<char *>(head), 10), 8)) * 0x1c);
            } else if (enc == 0x453) {
                quint16 followed = 0;
                if (!reader.u16(followed)) { ok = false; break; }
                ok = reader.skip(followed);
            } else if (enc == 0x455) {
                quint16 followed = 0;
                if (!reader.u16(followed)) { ok = false; break; }
                ok = reader.skip(followed);
            } else {
                QTextStream(stderr) << "Relay: Apple unexpected encoding " << enc << '\n';
                ok = false;
            }
        }
        if (!ok) break;
        ++updates;
        maxDecodeMs = std::max(maxDecodeMs, decodeClock.elapsed());
        maxResponseMs = std::max(maxResponseMs, responseClock.restart());
        periodic();
    }
    inflateEnd(&zrle);
    return ok;
}

void VncWorker::run() {
    if (m_stop) { emit ended({}); return; }
    if (!m_user.isEmpty() && tryApple()) return;
    auto *c = rfbGetClient(8, 3, 4);
    if (!c) { emit ended("Memoria insufficiente per aprire la sessione."); return; }
    rfbClientSetClientData(c, &workerTag, this);
    free(c->serverHost);
    c->serverHost = strdup(m_host.toUtf8().constData());
    c->serverPort = m_port;
    c->connectTimeout = 5;
    c->readTimeout = 15;
    c->MallocFrameBuffer = VncHooks::allocate;
    c->GotFrameBufferUpdate = VncHooks::update;
    c->GotCursorShape = VncHooks::cursor;
    c->GetPassword = VncHooks::password;
    c->GetCredential = VncHooks::credential;
    c->canHandleNewFBSize = TRUE;
    c->appData.shareDesktop = TRUE;
    c->appData.useRemoteCursor = TRUE;
    c->appData.enableJPEG = TRUE;
    c->appData.qualityLevel = m_lowBandwidth ? 3 : 7;
    c->appData.compressLevel = m_lowBandwidth ? 6 : 3;
    // Fluido forces ZRLE only: measured against macOS Screen Sharing, its
    // Tight/Hextile paths produce larger lossless rects for window drags.
    c->appData.encodingsString = m_lowBandwidth
        ? "copyrect zrle zlib raw"
        : "copyrect tight zrle zlib hextile raw";
    const bool initialized = rfbInitClient(c, nullptr, nullptr);
    m_password.fill('\0'); m_password.clear();
    if (!initialized) {
        { QMutexLocker lock(&m_socketMutex); if (m_socket >= 0) ::close(m_socket); m_socket = -1; }
        // rfbInitClient already frees the client on failure; the image belongs to us.
        emit ended(m_stop ? QString() : "Connessione non riuscita. Controlla Tailscale, Condivisione schermo e le credenziali del Mac.");
        return;
    }
    trackSocket(c->sock);
    // Pointer/key packets are tiny: Nagle would hold them behind delayed
    // ACKs and add tens of ms of input latency on Wi-Fi/Tailscale paths.
    const int noDelay = 1;
    ::setsockopt(c->sock, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
#ifdef TCP_QUICKACK
    ::setsockopt(c->sock, IPPROTO_TCP, TCP_QUICKACK, &noDelay, sizeof(noDelay));
#endif
    // LibVNCClient counts partial-read retries, not wall-clock inactivity.
    // A large frame over a relay can exceed that count despite making progress.
    // Streaming reads are interrupted explicitly via shutdown() on cancellation.
    c->readTimeout = 0;
    QTextStream(stderr) << "Relay profile: " << (m_lowBandwidth ? "Fluido / RGB565" : "Qualità / RGB888")
        << "; desktop " << c->width << 'x' << c->height << '\n';
    emit authenticated();
    m_client.store(c);
    QElapsedTimer frameClock; frameClock.start();
    QElapsedTimer statsClock; statsClock.start();
    QElapsedTimer responseClock; responseClock.start();
    qint64 maxDecodeMs = 0, maxResponseMs = 0;
    qint64 lastReceivedBytes = socketReceivedBytes(c->sock);
    int updates = 0;
    bool healthy = true;
    while (!m_stop && healthy) {
        if (!drainInputs(c)) break;
        // ReadFromRFBServer can prefetch several messages in one socket read.
        // select() cannot see those bytes; waiting only on the socket can leave
        // the last frame buffered forever and stop the update-request cycle.
        const int ready = c->buffered > 0 ? 1 : WaitForMessage(c, 1000);
        QElapsedTimer decodeClock; decodeClock.start();
        if (ready < 0 || (ready > 0 && !HandleRFBServerMessage(c))) { healthy = false; break; }
        if (m_sendFailed.load()) { healthy = false; break; }
        if (ready > 0) {
            ++updates;
            maxDecodeMs = std::max(maxDecodeMs, decodeClock.elapsed());
            maxResponseMs = std::max(maxResponseMs, responseClock.restart());
        }
        if (m_dirty) {
            // Present small changes almost immediately and cap full-screen
            // changes at ~60 fps: copy and texture conversion of a
            // 1920x1200 frame measure well under 1 ms on this class of
            // hardware, so a 33 ms hold only adds visible input latency.
            const qint64 framePixels = qint64(m_buffer.width()) * m_buffer.height();
            const qint64 dirtyPixels = qint64(m_frameDirty.width()) * m_frameDirty.height();
            const qint64 interval = framePixels > 0 && dirtyPixels * 4 <= framePixels ? 8 : 16;
            if (frameClock.elapsed() >= interval) {
                const bool notify = presentFrame();
                frameClock.restart();
                if (notify) emit frameAvailable();
            }
        }
        if (statsClock.elapsed() >= 5000) {
            const qint64 framePixels = qint64(m_buffer.width()) * m_buffer.height();
            const qint64 dirtyPercent = m_statsPresented > 0 && framePixels > 0
                ? m_statsDirtyArea * 100 / (qint64(m_statsPresented) * framePixels) : 0;
            const qint64 receivedBytes = socketReceivedBytes(c->sock);
            const qint64 receivedDelta = receivedBytes >= 0 && lastReceivedBytes >= 0
                ? receivedBytes - lastReceivedBytes : -1;
            lastReceivedBytes = receivedBytes;
            QTextStream(stderr) << "Relay performance: " << updates << " messages, "
                << m_statsPresented << " presented, " << m_statsRects << " rects / "
                << statsClock.elapsed() << " ms; dirty ~" << dirtyPercent
                << "%/frame; received " << receivedDelta << " B; max response " << maxResponseMs
                << " ms; max receive/decode " << maxDecodeMs
                << " ms; max input queue " << m_maxInputDelayMs << " ms\n";
            statsClock.restart(); updates = 0; maxDecodeMs = 0; maxResponseMs = 0;
            m_maxInputDelayMs = 0; m_statsRects = 0; m_statsPresented = 0; m_statsDirtyArea = 0;
        }
    }
    m_client.store(nullptr);
    c->frameBuffer = nullptr;
    { QMutexLocker lock(&m_socketMutex); if (m_socket >= 0) ::close(m_socket); m_socket = -1; }
    rfbClientCleanup(c);
    emit ended(m_stop ? QString() : "Il collegamento con il Mac è stato interrotto. Puoi riconnetterti.");
}

Session::Session(bool probeEnabled, QObject *parent) : QObject(parent), m_probeEnabled(probeEnabled) {
    connect(&m_credentials, &Credentials::completed, this,
        [this](int operation, QString host, int port, QString user, QString password, bool found, bool success) {
            const auto key = credentialKey(host, port, user);
            if (operation == Credentials::Store) m_storingCredentials.remove(key);
            if (!success) {
                emit credentialError("Portachiavi non disponibile. Puoi inserire la password manualmente.");
                return;
            }
            if (operation == Credentials::Store) {
                m_settings.setValue(key, true);
                emit preferencesChanged();
                showNotice("Password salvata nel portachiavi.");
            } else if (operation == Credentials::Clear) {
                m_settings.remove(key);
                emit preferencesChanged();
                showNotice("Password dimenticata.");
            } else {
                if (!found) { m_settings.remove(key); emit preferencesChanged(); }
                if (credentialHost(this->host()) == host && this->port() == port)
                    emit passwordLoaded(user, password);
            }
        });
    m_probeTimeout.setSingleShot(true);
    m_probeTimeout.setInterval(5000);
    connect(&m_probe, &QTcpSocket::connected, this, [this] {
        m_probeTimeout.stop(); m_online = true; emit onlineChanged(); m_probe.abort();
    });
    connect(&m_probe, &QTcpSocket::errorOccurred, this, [this] {
        m_probeTimeout.stop(); m_online = false; emit onlineChanged();
    });
    connect(&m_probeTimeout, &QTimer::timeout, this, [this] {
        m_probe.abort(); m_online = false; emit onlineChanged();
    });
    connect(&m_probeInterval, &QTimer::timeout, this, &Session::probe);
    if (probeEnabled) m_probeInterval.start(30000);
    m_noticeTimeout.setSingleShot(true);
    connect(&m_noticeTimeout, &QTimer::timeout, this, [this] { m_notice.clear(); emit noticeChanged(); });
    if (probeEnabled) QTimer::singleShot(0, this, &Session::probe);
}
Session::~Session() { m_pendingPassword.fill('\0'); if (m_worker) { m_worker->stop(); m_worker->wait(); } }
QString Session::host() const { return m_settings.value("host").toString(); }
QString Session::username() const { return m_settings.value("username").toString(); }
QString Session::displayName() const { return m_settings.value("displayName", "Mac").toString(); }
void Session::setDisplayName(QString name) { m_settings.setValue("displayName", name.trimmed().isEmpty() ? "Mac" : name.trimmed()); emit preferencesChanged(); }
int Session::port() const { return m_settings.value("port", 5900).toInt(); }
bool Session::dark() const { return m_settings.value("dark", true).toBool(); }
bool Session::macShortcuts() const { return m_settings.value("macShortcuts", true).toBool(); }
bool Session::lowBandwidth() const { return m_settings.value("lowBandwidth", true).toBool(); }
void Session::setDark(bool enabled) { m_settings.setValue("dark", enabled); emit preferencesChanged(); }
void Session::setLowBandwidth(bool enabled) { m_settings.setValue("lowBandwidth", enabled); emit preferencesChanged(); }
void Session::setMacShortcuts(bool enabled) { m_settings.setValue("macShortcuts", enabled); emit preferencesChanged(); }
void Session::setState(QString state, QString message) {
    m_state = std::move(state); m_message = std::move(message); emit stateChanged();
}
void Session::probe() {
    if (!m_probeEnabled || host().isEmpty() || m_probe.state() != QAbstractSocket::UnconnectedState || m_worker) return;
    m_probe.connectToHost(host(), port()); m_probeTimeout.start();
}
void Session::savePreferences(QString host, int port, QString user, bool dark) {
    host = host.trimmed();
    if (host.isEmpty() || host.contains('/') || host.contains(' ') || port < 1 || port > 65535) return;
    m_settings.setValue("host", host);
    m_settings.setValue("port", port);
    m_settings.setValue("username", user.trimmed());
    m_settings.setValue("dark", dark);
    m_settings.sync();
    emit preferencesChanged();
    m_probe.abort(); probe();
}
void Session::showNotice(QString message) {
    m_notice = std::move(message); emit noticeChanged(); m_noticeTimeout.start(5000);
}
bool Session::remembersPassword(QString user) const {
    return m_settings.value(credentialKey(host(), port(), user), false).toBool();
}
void Session::loadPassword(QString user) {
    user = user.trimmed();
    if (remembersPassword(user)) m_credentials.request(Credentials::Lookup, credentialHost(host()), port(), user);
}
void Session::forgetPassword(QString user) {
    m_credentials.request(Credentials::Clear, credentialHost(host()), port(), user.trimmed());
}
void Session::connectRemote(QString user, QString password, bool remember) {
    if (m_worker || host().isEmpty()) return;
    user = user.trimmed();
    m_settings.setValue("username", user); emit preferencesChanged();
    m_pendingPassword.fill('\0'); m_pendingPassword.clear();
    m_rememberOnAuthentication = remember;
    m_pendingUser = user;
    if (remember) m_pendingPassword = password.toUtf8();
    else if (remembersPassword(user) || m_storingCredentials.contains(credentialKey(host(), port(), user))) forgetPassword(user);
    connectEndpoint(host(), port(), std::move(user), std::move(password));
}
void Session::connectEndpoint(QString host, int port, QString user, QString password) {
    if (m_worker) return;
    m_lastFrame = {}; m_resolution.clear(); emit resolutionChanged();
    m_probe.abort(); m_probeTimeout.stop();
    setState("connecting");
    const auto endpointHost = credentialHost(host);
    const int endpointPort = port;
    auto *worker = new VncWorker(std::move(host), port, std::move(user), std::move(password), lowBandwidth(), this);
    m_worker = worker;
    connect(worker, &VncWorker::authenticated, this, [this, endpointHost, endpointPort] {
        if (m_state != "connecting") return;
        if (m_rememberOnAuthentication && !m_pendingPassword.isEmpty()) {
            m_storingCredentials.insert(credentialKey(endpointHost, endpointPort, m_pendingUser));
            m_credentials.request(Credentials::Store, endpointHost, endpointPort, m_pendingUser, m_pendingPassword);
        }
        m_pendingPassword.fill('\0'); m_pendingPassword.clear(); m_rememberOnAuthentication = false;
        setState("loading");
    });
    connect(worker, &VncWorker::cursorAvailable, this, &Session::cursorReady);
    connect(worker, &VncWorker::frameAvailable, this, [this, worker] {
        const QImage image = worker->latestFrame();
        if (m_worker != worker || m_state == "disconnecting") return;
        m_lastFrame = image;
        QString size = QString::number(image.width()) + " × " + QString::number(image.height());
        if (size != m_resolution) { m_resolution = size; emit resolutionChanged(); }
        if (m_state != "connected") setState("connected");
        emit frameReady(image);
    });
    connect(worker, &VncWorker::ended, this, [this, worker](const QString &reason) {
        if (m_worker != worker) return;
        m_pendingPassword.fill('\0'); m_pendingPassword.clear(); m_rememberOnAuthentication = false;
        // Input is no longer accepted while the thread finishes cleanup.
        setState(reason.isEmpty() ? "idle" : "error", reason);
        emit cursorReady({}, {});
    });
    connect(worker, &QThread::finished, this, [this, worker] {
        if (m_worker == worker) m_worker = nullptr;
        worker->deleteLater();
        if (m_state == "disconnecting") setState("idle");
    });
    worker->start();
}
void Session::disconnectRemote() {
    if (m_worker) { setState("disconnecting"); m_worker->stop(); }
    else setState("idle");
}
void Session::pointer(int x, int y, int mask) {
    if (m_worker && m_state == "connected") {
        InputEvent e; e.type=InputEvent::Pointer; e.x=x; e.y=y; e.mask=mask; m_worker->enqueue(std::move(e));
    }
}
void Session::key(quint32 key, bool down) {
    if (m_worker && m_state == "connected" && key) {
        InputEvent e; e.type=InputEvent::Key; e.key=key; e.down=down; m_worker->enqueue(std::move(e));
    }
}
void Session::saveScreenshot() {
    if (m_lastFrame.isNull()) return;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/Relay";
    QDir().mkpath(dir);
    QString path = dir + "/Mac-Pro-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + ".png";
    m_notice = m_lastFrame.save(path) ? "Schermata salvata in Immagini/Relay" : "Non è stato possibile salvare la schermata.";
    emit noticeChanged(); m_noticeTimeout.start(5000);
}

quint32 remoteKey(int key, const QString &text) {
    switch (key) {
    case Qt::Key_Shift: return XK_Shift_L;
    case Qt::Key_Control: return XK_Control_L;
    case Qt::Key_Alt: return XK_Alt_L;
    case Qt::Key_Meta: return XK_Super_L;
    case Qt::Key_AltGr: return XK_ISO_Level3_Shift;
    case Qt::Key_Escape: return XK_Escape;
    case Qt::Key_Return: case Qt::Key_Enter: return XK_Return;
    case Qt::Key_Tab: case Qt::Key_Backtab: return XK_Tab;
    case Qt::Key_Backspace: return XK_BackSpace;
    case Qt::Key_Delete: return XK_Delete;
    case Qt::Key_Insert: return XK_Insert;
    case Qt::Key_Home: return XK_Home;
    case Qt::Key_End: return XK_End;
    case Qt::Key_PageUp: return XK_Page_Up;
    case Qt::Key_PageDown: return XK_Page_Down;
    case Qt::Key_Left: return XK_Left;
    case Qt::Key_Right: return XK_Right;
    case Qt::Key_Up: return XK_Up;
    case Qt::Key_Down: return XK_Down;
    case Qt::Key_CapsLock: return XK_Caps_Lock;
    default: break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35) return XK_F1 + key - Qt::Key_F1;
    const auto chars = text.toUcs4();
    if (!chars.isEmpty() && chars[0] >= 32)
        return chars[0] <= 255 ? chars[0] : 0x01000000u | chars[0];
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return 'a' + key - Qt::Key_A;
    if (key >= 32 && key <= 126) return key;
    return 0;
}
QRectF fittedRect(QSize image, QSizeF viewport) {
    if (image.isEmpty() || viewport.isEmpty()) return {};
    QSizeF fitted = QSizeF(image).scaled(viewport, Qt::KeepAspectRatio);
    return {(viewport.width()-fitted.width())/2, (viewport.height()-fitted.height())/2, fitted.width(), fitted.height()};
}
RemoteView::RemoteView(QQuickItem *parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    setFlag(ItemIsFocusScope, true);
    setAntialiasing(false);
    setFlag(ItemHasContents, true);
}
void RemoteView::setSession(Session *session) {
    if (m_session == session) return;
    if (m_session) disconnect(m_session, nullptr, this, nullptr);
    m_session = session;
    if (session) {
        connect(session, &Session::frameReady, this, [this](const QImage &image) { m_image = image; ++m_frameVersion; update(); });
        connect(session, &Session::cursorReady, this, [this](const QImage &image, QPoint hotspot) {
            setCursor(image.isNull() ? QCursor(Qt::ArrowCursor)
                : QCursor(QPixmap::fromImage(image), hotspot.x(), hotspot.y()));
        });
    }
    emit sessionChanged();
}
QSGNode *RemoteView::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
    auto *node = static_cast<DesktopNode *>(oldNode);
    if (!node) {
        node = new DesktopNode;
        node->background = window()->createRectangleNode();
        node->background->setColor(QColor("#080d0d"));
        node->appendChildNode(node->background);
    }
    node->background->setRect(boundingRect());
    if (!m_image.isNull() && (!node->desktop || node->version != m_frameVersion)) {
        auto *texture = window()->createTextureFromImage(m_image, QQuickWindow::TextureIsOpaque);
        if (texture) {
            if (!node->desktop) {
                node->desktop = window()->createImageNode();
                node->desktop->setOwnsTexture(true);
                node->desktop->setFiltering(QSGTexture::Linear);
                node->appendChildNode(node->desktop);
            }
            auto *previous = node->desktop->texture();
            // The software node also deletes owned textures in setTexture().
            // Temporarily take ownership so replacement works on both backends.
            node->desktop->setOwnsTexture(false);
            node->desktop->setTexture(texture);
            node->desktop->setOwnsTexture(true);
            delete previous;
            node->desktop->setSourceRect(QRectF(QPointF(), m_image.size()));
            node->version = m_frameVersion;
        }
    }
    if (node->desktop) node->desktop->setRect(fittedRect(m_image.size(), size()));
    return node;
}
void RemoteView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    update();
}
bool RemoteView::sendPointer(QPointF position, Qt::MouseButtons buttons, int wheel) {
    if (!m_session || m_image.isNull()) return false;
    QRectF rect = fittedRect(m_image.size(), QSizeF(width(), height()));
    if (!rect.contains(position) && !buttons) return false;
    int x = std::clamp(int((position.x()-rect.x())/rect.width()*m_image.width()), 0, m_image.width()-1);
    int y = std::clamp(int((position.y()-rect.y())/rect.height()*m_image.height()), 0, m_image.height()-1);
    m_lastPointer = {x,y};
    int mask = (buttons.testFlag(Qt::LeftButton)?1:0) | (buttons.testFlag(Qt::MiddleButton)?2:0) | (buttons.testFlag(Qt::RightButton)?4:0);
    m_session->pointer(x,y,mask | wheel);
    if (wheel) m_session->pointer(x,y,mask);
    return true;
}
quint32 RemoteView::mappedRemoteKey(int qtKey, const QString &text) {
    quint32 key = remoteKey(qtKey, text);
    if (m_session && m_session->macShortcuts()) {
        if (qtKey == Qt::Key_Control) key = XK_Super_L;
        if (qtKey == Qt::Key_Meta) key = XK_Control_L;
    }
    return key;
}
QSet<quint32> RemoteView::intendedModifiers(Qt::KeyboardModifiers modifiers, int selfKey) {
    QSet<quint32> keep;
    if (modifiers & Qt::ShiftModifier) keep.insert(mappedRemoteKey(Qt::Key_Shift, {}));
    if (modifiers & Qt::ControlModifier) keep.insert(mappedRemoteKey(Qt::Key_Control, {}));
    if (modifiers & Qt::AltModifier) keep.insert(mappedRemoteKey(Qt::Key_Alt, {}));
    if (modifiers & Qt::MetaModifier) keep.insert(mappedRemoteKey(Qt::Key_Meta, {}));
    if (selfKey == Qt::Key_Shift || selfKey == Qt::Key_Control || selfKey == Qt::Key_Alt
        || selfKey == Qt::Key_Meta || selfKey == Qt::Key_AltGr)
        keep.insert(mappedRemoteKey(selfKey, {}));
    return keep;
}
void RemoteView::releaseModifiersExcept(const QSet<quint32> &keep) {
    if (!m_session) return;
    // A modifier release can be swallowed by the compositor (e.g. Super used
    // for a window shortcut), and local tracking can then disagree with the
    // Mac. Left down remotely it turns clicks into Ctrl+clicks and letters
    // into shortcuts. Sending a release for a key that is not held is a
    // no-op on the server, so force-release every modifier this event does
    // not intend to hold, tracked or not.
    static const quint32 modifierKeysyms[] = {XK_Shift_L, XK_Control_L, XK_Alt_L, XK_Super_L, XK_ISO_Level3_Shift};
    for (quint32 keysym : modifierKeysyms)
        if (!keep.contains(keysym)) m_session->key(keysym, false);
    for (auto it = m_pressedKeys.begin(); it != m_pressedKeys.end();) {
        bool isModifier = false;
        for (quint32 keysym : modifierKeysyms) isModifier = isModifier || it.value() == keysym;
        if (isModifier && !keep.contains(it.value())) {
            QTextStream(stderr) << "Relay input: released stale modifier " << it.value() << '\n';
            it = m_pressedKeys.erase(it);
        } else ++it;
    }
}
void RemoteView::mousePressEvent(QMouseEvent *e) { forceActiveFocus(); releaseModifiersExcept(intendedModifiers(e->modifiers())); sendPointer(e->position(), e->buttons()); e->accept(); }
void RemoteView::mouseReleaseEvent(QMouseEvent *e) {
    // Send exactly one release. A release outside the letterboxed image must
    // still reach the Mac, at the last in-image position.
    if (!sendPointer(e->position(), e->buttons()) && m_session)
        m_session->pointer(m_lastPointer.x(), m_lastPointer.y(),
            (e->buttons().testFlag(Qt::LeftButton)?1:0) | (e->buttons().testFlag(Qt::MiddleButton)?2:0) | (e->buttons().testFlag(Qt::RightButton)?4:0));
    e->accept();
}
void RemoteView::mouseMoveEvent(QMouseEvent *e) { sendPointer(e->position(), e->buttons()); e->accept(); }
void RemoteView::hoverMoveEvent(QHoverEvent *e) { sendPointer(e->position(), Qt::NoButton); e->accept(); }
void RemoteView::wheelEvent(QWheelEvent *e) {
    int delta = e->angleDelta().y();
    if (!delta) delta = e->pixelDelta().y();
    if (delta) for (int n=0;n<std::clamp(std::abs(delta)/120,1,8);n++) sendPointer(e->position(),e->buttons(),delta>0?8:16);
    e->accept();
}
void RemoteView::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_F11 && m_pressedKeys.isEmpty()) { e->ignore(); return; }
    releaseModifiersExcept(intendedModifiers(e->modifiers(), e->key()));
    const quint32 key = mappedRemoteKey(e->key(), e->text());
    if (key && m_session) { m_pressedKeys[e->key()] = key; m_session->key(key,true); }
    e->accept();
}
void RemoteView::keyReleaseEvent(QKeyEvent *e) {
    if (!e->isAutoRepeat()) {
        const quint32 key = m_pressedKeys.take(e->key());
        if (m_session && key) m_session->key(key,false);
    }
    e->accept();
}
void RemoteView::releaseInput() {
    if (m_session) {
        for (auto key : std::as_const(m_pressedKeys)) m_session->key(key,false);
        releaseModifiersExcept({});
        m_session->pointer(m_lastPointer.x(),m_lastPointer.y(),0);
    }
    m_pressedKeys.clear();
}
void RemoteView::focusOutEvent(QFocusEvent *e) { releaseInput(); QQuickItem::focusOutEvent(e); }
