#include "apple_mvs.h"

#include <QTextStream>

#include <algorithm>
#include <cstring>

namespace {
constexpr int kCacheSize = 65000;
constexpr int kCacheEntry = 99;
constexpr int kCb = 64;
constexpr int kCr = 128;

constexpr int kZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

constexpr uchar kLuma[64] = {
    16, 11, 11, 14, 24, 22, 24, 33, 11, 12, 11, 11, 26, 18, 19, 23,
    13, 13, 14, 24, 20, 21, 22, 28, 13, 17, 14, 21, 23, 25, 34, 34,
    17, 22, 18, 25, 31, 32, 34, 44, 24, 20, 17, 27, 35, 40, 44, 54,
    30, 35, 28, 30, 34, 44, 54, 64, 35, 30, 33, 38, 45, 55, 65, 75};
constexpr uchar kChroma[64] = {
    19, 19, 24, 47, 76, 99, 99, 99, 19, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    76, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

constexpr uchar kAcValues[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA};

struct Huff {
    int maxCode[17]{};
    int minCode[17]{};
    int valPtr[17]{};
    Huff() {
        const int counts[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125};
        std::fill(std::begin(maxCode), std::end(maxCode), -1);
        int code = 0;
        int valIdx = 0;
        for (int len = 1; len <= 16; ++len) {
            const int count = counts[len - 1];
            if (count == 0) { maxCode[len] = -1; code <<= 1; continue; }
            minCode[len] = code;
            valPtr[len] = valIdx;
            valIdx += count;
            maxCode[len] = code + count - 1;
            code = (code + count) << 1;
        }
    }
};

int32_t asI32(int64_t value) { return static_cast<int32_t>(static_cast<uint32_t>(value)); }
int8_t asI8(int value) { return static_cast<int8_t>(static_cast<uint8_t>(value)); }
int16_t asI16(int value) { return static_cast<int16_t>(static_cast<uint16_t>(value)); }
int clamp255(int value) { return value < 0 ? 0 : value > 255 ? 255 : value; }

int rangeLimit(int value) {
    value &= 0x3FF;
    if (value < 128) return value + 128;
    if (value < 512) return 255;
    if (value < 896) return 0;
    return value - 896;
}

int idctFlat(int dc) { return rangeLimit(((dc << 2) + (1 << 4)) >> 5); }

void idct8x8(const int32_t *block, uchar *output) {
    constexpr int kConst = 13;
    constexpr int kPass = 2;
    constexpr int kFix0298 = 2446, kFix0390 = 3196, kFix0541 = 4433, kFix0765 = 6270;
    constexpr int kFix0899 = 7373, kFix1175 = 9633, kFix1501 = 12299, kFix1847 = 15137;
    constexpr int kFix1961 = 16069, kFix2053 = 16819, kFix2562 = 20995, kFix3072 = 25172;
    int32_t ws[64];
    for (int c = 0; c < 8; ++c) {
        if (block[c + 8] == 0 && block[c + 16] == 0 && block[c + 24] == 0 && block[c + 32] == 0
            && block[c + 40] == 0 && block[c + 48] == 0 && block[c + 56] == 0) {
            const int32_t dc = block[c] << kPass;
            for (int row = 0; row < 8; ++row) ws[c + row * 8] = dc;
            continue;
        }
        const int64_t z2e = block[c + 16];
        const int64_t z3e = block[c + 48];
        const int64_t z1e = (z2e + z3e) * kFix0541;
        const int64_t tmp2e = z1e - z3e * kFix1847;
        const int64_t tmp3e = z1e + z2e * kFix0765;
        const int64_t tmp0e = (int64_t(block[c]) + block[c + 32]) << kConst;
        const int64_t tmp1e = (int64_t(block[c]) - block[c + 32]) << kConst;
        const int64_t tmp10 = tmp0e + tmp3e, tmp13 = tmp0e - tmp3e;
        const int64_t tmp11 = tmp1e + tmp2e, tmp12 = tmp1e - tmp2e;
        int64_t t0 = block[c + 56], t1 = block[c + 40], t2 = block[c + 24], t3 = block[c + 8];
        int64_t z1 = t0 + t3, z2 = t1 + t2, z3 = t0 + t2, z4 = t1 + t3;
        const int64_t z5 = (z3 + z4) * kFix1175;
        t0 *= kFix0298; t1 *= kFix2053; t2 *= kFix3072; t3 *= kFix1501;
        z1 *= -kFix0899; z2 *= -kFix2562;
        z3 = z3 * -kFix1961 + z5;
        z4 = z4 * -kFix0390 + z5;
        t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;
        constexpr int shift = kConst - kPass;
        constexpr int round = 1 << (shift - 1);
        ws[c] = asI32(tmp10 + t3 + round) >> shift;
        ws[c + 56] = asI32(tmp10 - t3 + round) >> shift;
        ws[c + 8] = asI32(tmp11 + t2 + round) >> shift;
        ws[c + 48] = asI32(tmp11 - t2 + round) >> shift;
        ws[c + 16] = asI32(tmp12 + t1 + round) >> shift;
        ws[c + 40] = asI32(tmp12 - t1 + round) >> shift;
        ws[c + 24] = asI32(tmp13 + t0 + round) >> shift;
        ws[c + 32] = asI32(tmp13 - t0 + round) >> shift;
    }
    for (int r = 0; r < 64; r += 8) {
        const int64_t z2e = ws[r + 2], z3e = ws[r + 6];
        const int64_t z1e = (z2e + z3e) * kFix0541;
        const int64_t tmp2e = z1e - z3e * kFix1847;
        const int64_t tmp3e = z1e + z2e * kFix0765;
        const int64_t tmp0e = (int64_t(ws[r]) + ws[r + 4]) << kConst;
        const int64_t tmp1e = (int64_t(ws[r]) - ws[r + 4]) << kConst;
        const int64_t tmp10 = tmp0e + tmp3e, tmp13 = tmp0e - tmp3e;
        const int64_t tmp11 = tmp1e + tmp2e, tmp12 = tmp1e - tmp2e;
        int64_t t0 = ws[r + 7], t1 = ws[r + 5], t2 = ws[r + 3], t3 = ws[r + 1];
        int64_t z1 = t0 + t3, z2 = t1 + t2, z3 = t0 + t2, z4 = t1 + t3;
        const int64_t z5 = (z3 + z4) * kFix1175;
        t0 *= kFix0298; t1 *= kFix2053; t2 *= kFix3072; t3 *= kFix1501;
        z1 *= -kFix0899; z2 *= -kFix2562;
        z3 = z3 * -kFix1961 + z5;
        z4 = z4 * -kFix0390 + z5;
        t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;
        constexpr int shift = kConst + kPass + 3;
        constexpr int round = 1 << (shift - 1);
        output[r] = uchar(rangeLimit(asI32(tmp10 + t3 + round) >> shift));
        output[r + 7] = uchar(rangeLimit(asI32(tmp10 - t3 + round) >> shift));
        output[r + 1] = uchar(rangeLimit(asI32(tmp11 + t2 + round) >> shift));
        output[r + 6] = uchar(rangeLimit(asI32(tmp11 - t2 + round) >> shift));
        output[r + 2] = uchar(rangeLimit(asI32(tmp12 + t1 + round) >> shift));
        output[r + 5] = uchar(rangeLimit(asI32(tmp12 - t1 + round) >> shift));
        output[r + 3] = uchar(rangeLimit(asI32(tmp13 + t0 + round) >> shift));
        output[r + 4] = uchar(rangeLimit(asI32(tmp13 - t0 + round) >> shift));
    }
}

struct Tabs {
    int crR[256]{}, cbB[256]{}, crG[256]{}, cbG[256]{};
    Tabs() {
        for (int i = 0; i < 256; ++i) {
            const int x = i - 128;
            crR[i] = (91881 * x + 32768) >> 16;
            cbB[i] = (116130 * x + 32768) >> 16;
            crG[i] = -46802 * x;
            cbG[i] = -22554 * x + 32768;
        }
    }
};
const Tabs &tabs() { static Tabs tables; return tables; }

int quantStep(int pos, int threshold) {
    if (pos <= 5) {
        if (threshold > 14) return 2;
        if (pos < threshold) return 8;
        return 16;
    }
    if (threshold > 14 && pos < threshold) return 2;
    if (threshold > 14) return 8;
    if (pos < threshold) return 8;
    return 16;
}

QByteArray packBits(const char *text) {
    QByteArray out;
    int count = 0;
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor == ' ') continue;
        if (count % 8 == 0) out.append('\0');
        if (*cursor == '1') out[count >> 3] = char(uchar(out[count >> 3]) | uchar(0x80 >> (count & 7)));
        ++count;
    }
    return out;
}

QByteArray fullUpdate(int threshold1, int threshold2, const QByteArray &header, const QByteArray &body) {
    const int offset = 6 + int(header.size());
    QByteArray out;
    out.append(char(0));
    out.append(char(threshold1));
    out.append(char(threshold2));
    out.append(char((offset >> 16) & 255));
    out.append(char((offset >> 8) & 255));
    out.append(char(offset & 255));
    out.append(header);
    out.append(body);
    return out;
}

QByteArray partialUpdate(int crLimit, int cbLimit, const QByteArray &body) {
    QByteArray out;
    out.append(char(1));
    out.append(char(crLimit));
    out.append(char(cbLimit));
    out.append(body);
    return out;
}

bool rgbAt(const QImage &image, int x, int y, int r, int g, int b) {
    const QRgb pixel = image.pixel(x, y);
    return qRed(pixel) == r && qGreen(pixel) == g && qBlue(pixel) == b;
}
} // namespace

int AppleMvsDecoder::Bitstream::readBit() {
    if (!ok || bytePos >= length) { ok = false; return 0; }
    const int bit = (data[bytePos] >> (7 - bitPos)) & 1;
    if (++bitPos > 7) { bitPos = 0; ++bytePos; }
    return bit;
}

int AppleMvsDecoder::Bitstream::readBits(int n) {
    int value = 0;
    for (int i = 0; i < n; ++i) value = (value << 1) | readBit();
    return value;
}

AppleMvsDecoder::AppleMvsDecoder() { ensureInit(); }

void AppleMvsDecoder::ensureInit() {
    if (m_inited) return;
    std::memcpy(m_lumaQuant, kLuma, 64);
    std::memcpy(m_chromaQuant, kChroma, 64);
    m_cache.assign(size_t(kCacheSize) * kCacheEntry, 0);
    m_inited = true;
}

void AppleMvsDecoder::warn(const char *text) {
    ++m_warnings;
    Q_UNUSED(text);
}

void AppleMvsDecoder::sync(QImage &image) {
    // The baseline is per screen size. A detached QImage keeps the same
    // dimensions and must keep the descriptors partial updates are coded
    // against; keying this on the pixel-buffer address wiped them.
    if (m_tilesN > 0 && image.width() == m_fbW && image.height() == m_fbH) return;
    m_fbBits = image.constBits();
    m_fbW = image.width();
    m_fbH = image.height();
    m_tilesX = (m_fbW + 7) / 8;
    m_tilesN = m_tilesX * ((m_fbH + 7) / 8);
    m_descThreshold.assign(m_tilesN, 0);
    m_descY.assign(size_t(m_tilesN) * 64, 0);
    m_descCbOk.assign(m_tilesN, 0);
    m_descCbDc.assign(m_tilesN, 0);
    m_descCrOk.assign(m_tilesN, 0);
    m_descCrDc.assign(m_tilesN, 0);
    m_descSeq.assign(m_tilesN, 0);
    m_descRef.assign(m_tilesN, -1);
    m_descMatchX.assign(m_tilesN, 0);
    m_descMatchY.assign(m_tilesN, 0);
}

bool AppleMvsDecoder::decode(const uchar *data, int length, int x, int y, int w, int h, QImage &image) {
    ensureInit();
    if (!data || length < 1 || image.format() != QImage::Format_RGB32) return false;
    sync(image);
    const int type = data[0];
    if (type != 0x02 && (x < 0 || y < 0 || x + w > m_fbW || y + h > m_fbH)) {
        warn("bad rectangle");
        return true;
    }
    if (type == 0x02) { handleQuantization(data, length); return true; }
    if (type == 0x00) return handleFull(data, length, x, y, w, h, image);
    if (type == 0x01) return handlePartial(data, length, x, y, w, h, image);
    warn("unknown update");
    return true;
}

void AppleMvsDecoder::handleQuantization(const uchar *data, int length) {
    if (length != 129) { warn("quantization size"); return; }
    std::memcpy(m_lumaQuant, data + 1, 64);
    std::memcpy(m_chromaQuant, data + 65, 64);
    ++m_quantGen;
}

bool AppleMvsDecoder::handleFull(const uchar *data, int length, int x, int y, int w, int h, QImage &image) {
    if (length < 6) return false;
    const int threshold1 = data[1], threshold2 = data[2];
    const int bodyOffset = (int(data[3]) << 16) | (int(data[4]) << 8) | data[5];
    if (bodyOffset < 6 || bodyOffset > length) return false;
    const int tilesX = (w + 7) / 8, tilesY = (h + 7) / 8, total = tilesX * tilesY;
    Bitstream header{data + 6, bodyOffset - 6, 0, 0, true};
    Bitstream body{data + bodyOffset, length - bodyOffset, 0, 0, true};
    header.readBit();
    ++m_frameSeq;
    std::fill(std::begin(m_dpcm), std::end(m_dpcm), 0);
    std::fill(std::begin(m_solid), std::end(m_solid), 0);
    std::fill(std::begin(m_colorA), std::end(m_colorA), 255);
    m_colorB[0] = 181; m_colorB[1] = 213; m_colorB[2] = 254;
    int prevDct = -1, prevX = x, prevY = y;
    for (int tile = 0; tile < total && header.ok;) {
        const int tileType = header.readBits(3);
        int count = 1;
        if (header.readBit()) {
            const int v4 = header.readBits(4);
            if (v4 < 15) count = v4 + 2;
            else {
                const int byte1 = header.readBits(8);
                int varint = byte1;
                if (byte1 & 0x80) {
                    const int byte2 = header.readBits(8);
                    if ((byte2 & 0x80) == 0) varint = (byte2 << 7) | (byte1 & 0x7f);
                    else {
                        const int byte3 = header.readBits(8);
                        varint = (byte3 << 14) | ((byte2 & 0x7f) << 7) | (byte1 & 0x7f);
                    }
                }
                count = varint + 17;
            }
        }
        for (; count > 0 && tile < total; --count, ++tile) {
            const int col = tile % tilesX, row = tile / tilesX;
            const int tileX = x + col * 8, tileY = y + row * 8;
            const int tileW = std::min(8, x + w - tileX), tileH = std::min(8, y + h - tileY);
            const int screen = (tileY >> 3) * m_tilesX + (tileX >> 3);
            if (screen < 0 || screen >= m_tilesN) return false;
            m_descSeq[screen] = m_frameSeq;
            m_descRef[screen] = -1;
            bool rendered = true;
            switch (tileType) {
            case 0: std::fill(std::begin(m_tile), std::end(m_tile), qRgb(255, 255, 255)); break;
            case 1:
                m_descRef[screen] = screen - 1;
                m_descMatchX[screen] = int16_t(prevX);
                m_descMatchY[screen] = int16_t(prevY);
                copyTile(image, prevX, prevY, tileX, tileY, tileW, tileH);
                rendered = false;
                break;
            case 2:
                m_descRef[screen] = screen - m_tilesX;
                m_descMatchX[screen] = int16_t(tileX);
                m_descMatchY[screen] = int16_t(tileY - 8);
                copyTile(image, tileX, tileY - 8, tileX, tileY, tileW, tileH);
                rendered = false;
                break;
            case 3: decodeTwoColorBW(body); break;
            case 4: decodeSolidOrTwoColor(body); break;
            case 5:
                if (!decodeDct(body, threshold1, threshold2, screen, prevDct)) return false;
                prevDct = screen;
                break;
            case 6: rendered = loadCache(body.readBits(16)); break;
            case 7: rendered = loadCache((m_cacheLast + 1) & 0xFFFF); break;
            default: break;
            }
            if (rendered) putTile(image, tileX, tileY, tileW, tileH);
            prevX = tileX;
            prevY = tileY;
        }
    }
    const int headerEnd = header.readBits(8);
    const int bodyEnd = body.readBits(8);
    if (header.ok && headerEnd != 0x6d) warn("header marker");
    if (body.ok && bodyEnd != 0x6d) warn("body marker");
    return header.ok && body.ok;
}

bool AppleMvsDecoder::handlePartial(const uchar *data, int length, int x, int y, int w, int h, QImage &image) {
    if (length < 3) return false;
    const int crLimit = std::min(int(data[1]), 64), cbLimit = std::min(int(data[2]), 64);
    const int tilesX = (w + 7) / 8, tilesY = (h + 7) / 8, total = tilesX * tilesY;
    Bitstream bits{data + 3, length - 3, 0, 0, true};
    for (int tile = 0; tile < total; ++tile) {
        const int col = tile % tilesX, row = tile / tilesX;
        const int tileX = x + col * 8, tileY = y + row * 8;
        const int tileW = std::min(8, x + w - tileX), tileH = std::min(8, y + h - tileY);
        const int screen = (tileY >> 3) * m_tilesX + (tileX >> 3);
        if (screen < 0 || screen >= m_tilesN) return false;
        switch (bits.readBits(2)) {
        case 0: break;
        case 1:
            if (!decodePartialDct(bits, screen, crLimit, cbLimit)) return false;
            putTile(image, tileX, tileY, tileW, tileH);
            break;
        case 2: {
            const int ref = m_descRef[screen];
            if (ref >= 0 && ref < m_tilesN && m_descSeq[screen] == m_descSeq[ref])
                copyTile(image, m_descMatchX[screen], m_descMatchY[screen], tileX, tileY, tileW, tileH);
            break;
        }
        case 3: {
            const int index = bits.readBit() ? (m_cacheLast + 1) & 0xFFFF : bits.readBits(16);
            if (loadCache(index)) putTile(image, tileX, tileY, tileW, tileH);
            break;
        }
        default: break;
        }
    }
    const int m = bits.readBits(8), v = bits.readBits(8), s = bits.readBits(8);
    if (bits.ok && (m != 0x6d || v != 0x76 || s != 0x73)) warn("partial marker");
    return bits.ok;
}

void AppleMvsDecoder::decodeTwoColorBW(Bitstream &bits) {
    const int rows = bits.readBits(8);
    for (int row = 0; row < 8; ++row) {
        const int whiteRow = (rows >> (7 - row)) & 1;
        const int pixels = whiteRow ? 0xFF : bits.readBits(8);
        for (int col = 0; col < 8; ++col) {
            const int white = whiteRow || ((pixels >> (7 - col)) & 1);
            m_tile[row * 8 + col] = qRgb(white ? 255 : 0, white ? 255 : 0, white ? 255 : 0);
        }
    }
}

void AppleMvsDecoder::decodeSolidOrTwoColor(Bitstream &bits) {
    const int two = bits.readBit(), reuse = bits.readBit();
    if (!two) {
        if (!reuse) readYCbCr20(bits, m_solid);
        std::fill(std::begin(m_tile), std::end(m_tile), qRgb(m_solid[0], m_solid[1], m_solid[2]));
        return;
    }
    if (!reuse) { readYCbCr20(bits, m_colorA); readYCbCr20(bits, m_colorB); }
    renderTwoColor(bits, m_colorA, m_colorB);
}

void AppleMvsDecoder::readYCbCr20(Bitstream &bits, uchar rgb[3]) {
    const int y = bits.readBits(8);
    const int cb = bits.readBits(6) << 2;
    const int cr = bits.readBits(6) << 2;
    const Tabs &color = tabs();
    rgb[0] = uchar(clamp255(y + color.crR[cr]));
    rgb[1] = uchar(clamp255(y + ((color.cbG[cb] + color.crG[cr]) >> 16)));
    rgb[2] = uchar(clamp255(y + color.cbB[cb]));
}

void AppleMvsDecoder::renderTwoColor(Bitstream &bits, const uchar *a, const uchar *b) {
    const int rows = bits.readBits(8);
    for (int row = 0; row < 8; ++row) {
        const int pixels = ((rows >> (7 - row)) & 1) ? 0xFF : bits.readBits(8);
        for (int col = 0; col < 8; ++col) {
            const uchar *color = ((pixels >> (7 - col)) & 1) ? a : b;
            m_tile[row * 8 + col] = qRgb(color[0], color[1], color[2]);
        }
    }
}

int AppleMvsDecoder::leadingOnes(Bitstream &bits) {
    int count = 0;
    while (bits.readBit() == 1) ++count;
    return count;
}

int AppleMvsDecoder::decodeDcRice(Bitstream &bits) {
    const int ones = leadingOnes(bits);
    if (!bits.ok) return 0;
    if (ones == 0) {
        if (bits.readBit() == 0) return 0;
        return bits.readBit() ? -1 : 1;
    }
    int magnitude;
    if (ones == 1) magnitude = 2 + bits.readBits(1);
    else if (ones == 2) magnitude = 4 + bits.readBits(2);
    else magnitude = ((ones << 3) | bits.readBits(3)) - 16;
    return bits.readBit() ? -magnitude : magnitude;
}

void AppleMvsDecoder::decodeAc(Bitstream &bits, int pos, int threshold, AcResult &result) {
    result = {};
    if (bits.readBit()) {
        const int ones = leadingOnes(bits);
        int magnitude;
        if (pos <= 5) {
            if (ones <= 3) magnitude = 2 + ones * 4 + bits.readBits(2);
            else magnitude = 18 + (ones - 4) * 8 + bits.readBits(3);
        } else if (ones == 0) magnitude = 2 + bits.readBits(1);
        else if (ones == 1) magnitude = 4 + bits.readBits(2);
        else magnitude = ((ones << 3) | bits.readBits(3)) - 8;
        result.value = (bits.readBit() ? -magnitude : magnitude) * quantStep(pos, threshold);
        return;
    }
    switch (bits.readBits(2)) {
    case 0: break;
    case 1: {
        if (bits.readBit() == 0) { result.eob = true; break; }
        const int ext = bits.readBits(2);
        if (ext < 3) { result.skip = ext + 3; break; }
        int run = 6;
        while (true) {
            const int chunk = bits.readBits(3);
            run += chunk;
            if (chunk < 7 || run + pos >= 64) break;
        }
        result.skip = run;
        break;
    }
    case 2: result.value = quantStep(pos, threshold); break;
    case 3: result.value = -quantStep(pos, threshold); break;
    default: break;
    }
}

void AppleMvsDecoder::decodeHuffmanAc(Bitstream &bits, int16_t *coeffs, int offset, int limit) {
    static const Huff table;
    int consumed = 0;
    while (consumed < limit && bits.ok) {
        int code = 0, symbol = -1;
        for (int len = 1; len <= 16; ++len) {
            code = (code << 1) | bits.readBit();
            if (table.maxCode[len] >= 0 && code <= table.maxCode[len]) {
                symbol = kAcValues[table.valPtr[len] + (code - table.minCode[len])];
                break;
            }
        }
        if (symbol < 0) { bits.ok = false; return; }
        const int run = symbol >> 4, valueBits = symbol & 0x0f;
        if (valueBits == 0) {
            if (run != 15) return;
            consumed += 16;
            continue;
        }
        consumed += run;
        const int raw = bits.readBits(valueBits);
        const int value = raw < (1 << (valueBits - 1)) ? raw - (1 << valueBits) + 1 : raw;
        coeffs[offset + std::min(consumed + 1, 63)] = asI16(value);
        ++consumed;
    }
}

bool AppleMvsDecoder::decodeDct(Bitstream &bits, int threshold1, int threshold2, int screen, int &prevDct) {
    std::fill(std::begin(m_coeffs), std::end(m_coeffs), 0);
    if (bits.readBit()) {
        if (prevDct >= 0) {
            for (int i = 0; i < 64; ++i) m_coeffs[i] = m_dpcm[i];
            m_coeffs[kCb] = m_dpcm[kCb];
            m_coeffs[kCr] = m_dpcm[kCr];
            copyDesc(prevDct, screen);
        } else warn("repeat without a DCT tile");
    } else {
        const int dcMode = bits.readBit();
        const int threshold = bits.readBit() ? threshold2 : threshold1;
        if (dcMode == 0) {
            const int cbRice = decodeDcRice(bits);
            const int cbHalf = (m_dpcm[kCb] + (m_dpcm[kCb] < 0 ? 1 : 0)) >> 1;
            m_coeffs[kCb] = asI16((cbHalf - cbRice) * 2);
            const int crRice = decodeDcRice(bits);
            const int crHalf = (m_dpcm[kCr] + (m_dpcm[kCr] < 0 ? 1 : 0)) >> 1;
            m_coeffs[kCr] = asI16((crHalf - crRice) * 2);
        } else {
            m_coeffs[kCb] = m_dpcm[kCb];
            m_coeffs[kCr] = m_dpcm[kCr];
        }
        m_coeffs[0] = asI16(m_dpcm[0] - decodeDcRice(bits));
        AcResult ac;
        for (int pos = 1; pos < 64;) {
            decodeAc(bits, pos, threshold, ac);
            if (!bits.ok || ac.eob) break;
            if (ac.skip > 0) pos += ac.skip;
            else { m_coeffs[pos] = asI16(ac.value); ++pos; }
        }
        m_descThreshold[screen] = uchar(threshold);
        for (int i = 0; i < 64; ++i) m_descY[size_t(screen) * 64 + i] = asI8(m_coeffs[i]);
        m_descCbOk[screen] = 1;
        m_descCbDc[screen] = asI8(m_coeffs[kCb]);
        m_descCrOk[screen] = 1;
        m_descCrDc[screen] = asI8(m_coeffs[kCr]);
    }
    std::copy(std::begin(m_coeffs), std::end(m_coeffs), std::begin(m_dpcm));
    dequantAndRender();
    return bits.ok;
}

int AppleMvsDecoder::chromaDcDelta(Bitstream &bits, int baseline) {
    const int bit = bits.readBit();
    if (baseline != 0) return asI8(baseline >= 0 ? baseline + bit : baseline - bit);
    if (bit) return bits.readBit() ? -1 : 1;
    return 0;
}

bool AppleMvsDecoder::decodePartialDct(Bitstream &bits, int screen, int crLimit, int cbLimit) {
    if (!m_descCrOk[screen] || !m_descCbOk[screen]) { warn("partial without baseline"); return false; }
    std::fill(std::begin(m_coeffs), std::end(m_coeffs), 0);
    std::fill(std::begin(m_cacheEntry), std::end(m_cacheEntry), 0);
    const int threshold = m_descThreshold[screen];
    const int base = screen * 64;
    m_coeffs[0] = m_descY[base];
    m_cacheEntry[0] = m_descY[base];
    const int acCount = bits.readBits(6);
    const int end = acCount + 1;
    int descPos = base + 1, entryPos = 1;
    const int phaseEnd = std::min(end, threshold);
    auto previous = [&] {
        const int value = descPos < base + 64 ? m_descY[descPos] : 0;
        ++descPos;
        return value;
    };
    for (int pos = 1; pos < phaseEnd; ++pos) {
        const int prev = previous();
        int value = 0;
        if (threshold < 15) {
            if (prev == 0) value = asI8(decodeDcRice(bits));
            else {
                const int delta = bits.readBits(3);
                value = asI8(prev >= 0 ? prev + delta : prev - delta);
            }
        } else if (prev == 0) {
            if (bits.readBit()) value = bits.readBit() ? -1 : 1;
        } else {
            const int delta = bits.readBit();
            value = asI8(prev >= 0 ? prev + delta : prev - delta);
        }
        m_coeffs[pos] = asI16(value);
        m_cacheEntry[entryPos++] = asI8(value);
    }
    if (acCount >= threshold) {
        const int deltaBits = threshold < 15 ? 4 : 3;
        for (int pos = threshold; pos < end; ++pos) {
            const int prev = previous();
            int value;
            if (prev == 0) value = asI8(decodeDcRice(bits));
            else {
                const int delta = bits.readBits(deltaBits);
                value = asI8(prev >= 0 ? prev + delta : prev - delta);
            }
            m_coeffs[pos] = asI16(value);
            m_cacheEntry[entryPos++] = asI8(value);
        }
    }
    m_coeffs[kCr] = asI16(chromaDcDelta(bits, m_descCrDc[screen]));
    decodeHuffmanAc(bits, m_coeffs, kCr, crLimit);
    for (int i = 0; i < 15; ++i) m_cacheEntry[64 + i] = asI8(m_coeffs[kCr + i]);
    m_coeffs[kCb] = asI16(chromaDcDelta(bits, m_descCbDc[screen]));
    decodeHuffmanAc(bits, m_coeffs, kCb, cbLimit);
    for (int i = 0; i < 20; ++i) m_cacheEntry[79 + i] = asI8(m_coeffs[kCb + i]);
    dequantAndRender();
    if (++m_cacheWrite == kCacheSize) m_cacheWrite = 1;
    ++m_cacheCount;
    std::memcpy(m_cache.data() + size_t(m_cacheWrite) * kCacheEntry, m_cacheEntry, kCacheEntry);
    if (!m_cacheGen.empty()) m_cacheGen[m_cacheWrite] = 0;
    return bits.ok;
}

bool AppleMvsDecoder::loadCache(int index) {
    if (index == 0 || index > kCacheSize || index > m_cacheCount) {
        warn("cache index");
        return false;
    }
    if (m_cachePixels.empty()) {
        m_cachePixels.assign(size_t(kCacheSize + 1) * 64, 0);
        m_cacheGen.assign(kCacheSize + 1, 0);
    }
    const size_t pixelBase = size_t(index) * 64;
    if (m_cacheGen[index] == m_quantGen) {
        std::memcpy(m_tile, m_cachePixels.data() + pixelBase, 64 * sizeof(quint32));
    } else {
        std::fill(std::begin(m_coeffs), std::end(m_coeffs), 0);
        const int8_t *entry = m_cache.data() + size_t(index) * kCacheEntry;
        for (int i = 0; i < 64; ++i) m_coeffs[i] = entry[i];
        for (int i = 0; i < 15; ++i) m_coeffs[kCr + i] = entry[64 + i];
        for (int i = 0; i < 20; ++i) m_coeffs[kCb + i] = entry[79 + i];
        dequantAndRender();
        std::memcpy(m_cachePixels.data() + pixelBase, m_tile, 64 * sizeof(quint32));
        m_cacheGen[index] = m_quantGen;
    }
    m_cacheLast = index;
    return true;
}

void AppleMvsDecoder::dequantAndRender() {
    const int yFlat = transformPlane(0, m_lumaQuant, m_ySpatial);
    if (yFlat >= 0) std::fill(std::begin(m_ySpatial), std::end(m_ySpatial), uchar(yFlat));
    const int cbFlat = transformPlane(kCb, m_chromaQuant, m_cbSpatial);
    const int crFlat = transformPlane(kCr, m_chromaQuant, m_crSpatial);
    const Tabs &color = tabs();
    if (cbFlat >= 0 && crFlat >= 0) {
        const int rAdd = color.crR[crFlat];
        const int gAdd = (color.cbG[cbFlat] + color.crG[crFlat]) >> 16;
        const int bAdd = color.cbB[cbFlat];
        for (int i = 0; i < 64; ++i)
            m_tile[i] = qRgb(clamp255(m_ySpatial[i] + rAdd), clamp255(m_ySpatial[i] + gAdd),
                clamp255(m_ySpatial[i] + bAdd));
        return;
    }
    if (cbFlat >= 0) std::fill(std::begin(m_cbSpatial), std::end(m_cbSpatial), uchar(cbFlat));
    if (crFlat >= 0) std::fill(std::begin(m_crSpatial), std::end(m_crSpatial), uchar(crFlat));
    for (int i = 0; i < 64; ++i) {
        const int y = m_ySpatial[i], cb = m_cbSpatial[i], cr = m_crSpatial[i];
        m_tile[i] = qRgb(clamp255(y + color.crR[cr]), clamp255(y + ((color.cbG[cb] + color.crG[cr]) >> 16)),
            clamp255(y + color.cbB[cb]));
    }
}

int AppleMvsDecoder::transformPlane(int plane, const uchar *quant, uchar *out) {
    int ac = 0;
    for (int i = 1; i < 64; ++i) {
        const int natural = kZigzag[i];
        const int value = m_coeffs[plane + i];
        ac |= value;
        m_dq[natural] = value * int(quant[natural]);
    }
    const int dc = m_coeffs[plane] * int(quant[0]);
    if (ac == 0) return idctFlat(dc);
    m_dq[0] = dc;
    idct8x8(m_dq, out);
    return -1;
}

void AppleMvsDecoder::putTile(QImage &image, int tileX, int tileY, int tileW, int tileH) {
    for (int row = 0; row < tileH; ++row) {
        auto *dst = reinterpret_cast<QRgb *>(image.scanLine(tileY + row)) + tileX;
        std::memcpy(dst, m_tile + row * 8, size_t(tileW) * sizeof(QRgb));
    }
}

void AppleMvsDecoder::copyTile(QImage &image, int srcX, int srcY, int dstX, int dstY, int tileW, int tileH) {
    if (srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0) return;
    for (int row = 0; row < tileH; ++row) {
        if (srcY + row >= image.height() || dstY + row >= image.height()) break;
        const int copyW = std::min(tileW, std::min(image.width() - srcX, image.width() - dstX));
        if (copyW <= 0) break;
        auto *dst = reinterpret_cast<QRgb *>(image.scanLine(dstY + row)) + dstX;
        const auto *src = reinterpret_cast<const QRgb *>(image.constScanLine(srcY + row)) + srcX;
        std::memmove(dst, src, size_t(copyW) * sizeof(QRgb));
    }
}

void AppleMvsDecoder::copyDesc(int from, int to) {
    m_descThreshold[to] = m_descThreshold[from];
    std::memcpy(m_descY.data() + size_t(to) * 64, m_descY.data() + size_t(from) * 64, 64);
    m_descCbOk[to] = m_descCbOk[from];
    m_descCbDc[to] = m_descCbDc[from];
    m_descCrOk[to] = m_descCrOk[from];
    m_descCrDc[to] = m_descCrDc[from];
    m_descSeq[to] = m_descSeq[from];
    m_descRef[to] = m_descRef[from];
    m_descMatchX[to] = m_descMatchX[from];
    m_descMatchY[to] = m_descMatchY[from];
}

bool AppleMvsDecoder::selfTest() {
    auto fail = [](const char *name) {
        QTextStream(stderr) << "MVS self-test failed: " << name << '\n';
        return false;
    };
    for (int dc = -40000; dc <= 40000; dc += 97) {
        int32_t block[64]{};
        uchar samples[64];
        block[0] = dc;
        idct8x8(block, samples);
        const int flat = idctFlat(dc);
        if (samples[0] != flat || samples[63] != flat) return fail("idct flat");
    }
    const QByteArray header = packBits("0 101 1 0000 01101101");
    const QByteArray fullBody = packBits("0 0 0 00 00 1110 010 1 0 01 0 1 01101101");
    const QByteArray full = fullUpdate(15, 25, header, fullBody);
    const QByteArray fullAcBody = packBits("0 0 0 00 00 1110 010 1 0 10 0 01 0 1 01101101");
    const QByteArray fullAc = fullUpdate(15, 25, header, fullAcBody);
    const char *eob = "1010";
    const QByteArray crUp = partialUpdate(14, 19, packBits(
        QByteArray("01 000000 1 0 ").append(eob).append(" 0 ").append(eob).append(" 00 01101101 01110110 01110011").constData()));
    const QByteArray unchanged = partialUpdate(14, 19, packBits(
        QByteArray("01 000000 0 ").append(eob).append(" 0 ").append(eob).append(" 00 01101101 01110110 01110011").constData()));
    auto partialAc = [&](const char *step) {
        return partialUpdate(14, 19, packBits(QByteArray("01 000001 ").append(step).append(" 0 ").append(eob)
            .append(" 0 ").append(eob).append(" 00 01101101 01110110 01110011").constData()));
    };
    const QByteArray cache1 = partialUpdate(14, 19, packBits("00 11 0 0000000000000001 01101101 01110110 01110011"));
    const QByteArray cacheNext = partialUpdate(14, 19, packBits("00 11 1 01101101 01110110 01110011"));

    AppleMvsDecoder decoder;
    QImage image(16, 8, QImage::Format_RGB32);
    image.fill(Qt::black);
    if (!decoder.decode(reinterpret_cast<const uchar *>(full.constData()), int(full.size()), 0, 0, 16, 8, image))
        return fail("full decode");
    if (decoder.warnings() != 0 || !rgbAt(image, 0, 0, 148, 148, 148) || !rgbAt(image, 8, 0, 148, 148, 148))
        return fail("full color");

    QByteArray tables(129, char(8));
    tables[0] = 2;
    decoder.clearWarnings();
    if (!decoder.decode(reinterpret_cast<const uchar *>(tables.constData()), int(tables.size()), 0, 0, 0, 0, image))
        return fail("quant");
    image.fill(Qt::black);
    if (!decoder.decode(reinterpret_cast<const uchar *>(full.constData()), int(full.size()), 0, 0, 16, 8, image)
        || decoder.warnings() != 0 || !rgbAt(image, 0, 0, 138, 138, 138))
        return fail("quant color");

    AppleMvsDecoder chroma;
    QImage screen(16, 8, QImage::Format_RGB32);
    screen.fill(Qt::black);
    if (!chroma.decode(reinterpret_cast<const uchar *>(full.constData()), int(full.size()), 0, 0, 16, 8, screen)
        || !chroma.decode(reinterpret_cast<const uchar *>(crUp.constData()), int(crUp.size()), 0, 0, 16, 8, screen))
        return fail("chroma decode");
    if (chroma.warnings() != 0 || !rgbAt(screen, 0, 0, 151, 147, 148) || !rgbAt(screen, 8, 0, 148, 148, 148))
        return fail("chroma color");
    if (!chroma.decode(reinterpret_cast<const uchar *>(unchanged.constData()), int(unchanged.size()), 0, 0, 16, 8, screen)
        || !rgbAt(screen, 0, 0, 148, 148, 148))
        return fail("baseline reset");

    AppleMvsDecoder ac;
    QImage acImage(16, 8, QImage::Format_RGB32);
    acImage.fill(Qt::black);
    const QByteArray stepped = partialAc("1"), restored = partialAc("0");
    if (!ac.decode(reinterpret_cast<const uchar *>(fullAc.constData()), int(fullAc.size()), 0, 0, 16, 8, acImage))
        return fail("ac full");
    QImage original = acImage;
    if (!ac.decode(reinterpret_cast<const uchar *>(stepped.constData()), int(stepped.size()), 0, 0, 16, 8, acImage))
        return fail("ac step");
    if (acImage == original) return fail("ac changed nothing");
    if (!ac.decode(reinterpret_cast<const uchar *>(restored.constData()), int(restored.size()), 0, 0, 16, 8, acImage)
        || acImage != original)
        return fail("ac restore");

    AppleMvsDecoder cache;
    QImage cached(16, 8, QImage::Format_RGB32);
    cached.fill(Qt::black);
    if (!cache.decode(reinterpret_cast<const uchar *>(full.constData()), int(full.size()), 0, 0, 16, 8, cached)
        || !cache.decode(reinterpret_cast<const uchar *>(crUp.constData()), int(crUp.size()), 0, 0, 16, 8, cached)
        || !cache.decode(reinterpret_cast<const uchar *>(unchanged.constData()), int(unchanged.size()), 0, 0, 16, 8, cached)
        || !cache.decode(reinterpret_cast<const uchar *>(cache1.constData()), int(cache1.size()), 0, 0, 16, 8, cached))
        return fail("cache decode");
    if (cache.warnings() != 0 || !rgbAt(cached, 8, 0, 151, 147, 148)) return fail("cache slot");
    if (!cache.decode(reinterpret_cast<const uchar *>(cacheNext.constData()), int(cacheNext.size()), 0, 0, 16, 8, cached)
        || !rgbAt(cached, 8, 0, 148, 148, 148))
        return fail("cache next");

    AppleMvsDecoder missing;
    QImage untouched(16, 8, QImage::Format_RGB32);
    untouched.fill(Qt::black);
    if (!missing.decode(reinterpret_cast<const uchar *>(full.constData()), int(full.size()), 0, 0, 16, 8, untouched)
        || !missing.decode(reinterpret_cast<const uchar *>(cache1.constData()), int(cache1.size()), 0, 0, 16, 8, untouched))
        return fail("missing cache decode");
    if (missing.warnings() != 1 || !rgbAt(untouched, 8, 0, 148, 148, 148)) return fail("missing cache");

    AppleMvsDecoder placed;
    QImage wide(32, 16, QImage::Format_RGB32);
    wide.fill(Qt::black);
    if (!placed.decode(reinterpret_cast<const uchar *>(full.constData()), int(full.size()), 16, 3, 16, 8, wide))
        return fail("offset decode");
    if (placed.warnings() != 0 || !rgbAt(wide, 16, 3, 148, 148, 148) || !rgbAt(wide, 31, 10, 148, 148, 148)
        || !rgbAt(wide, 16, 2, 0, 0, 0) || !rgbAt(wide, 15, 3, 0, 0, 0))
        return fail("offset pixels");
    AppleMvsDecoder rejected;
    QImage overflow(32, 16, QImage::Format_RGB32);
    overflow.fill(Qt::black);
    if (!rejected.decode(reinterpret_cast<const uchar *>(full.constData()), int(full.size()), 24, 3, 16, 8, overflow)
        || rejected.warnings() != 1 || !rgbAt(overflow, 24, 3, 0, 0, 0))
        return fail("rejected rectangle");
    return true;
}
