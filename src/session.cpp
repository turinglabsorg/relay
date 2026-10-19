#include "session.h"
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
void VncWorker::run() {
    if (m_stop) { emit ended({}); return; }
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
    c->appData.encodingsString = "copyrect tight zrle zlib hextile raw";
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
