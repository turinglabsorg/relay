#pragma once

#include <QImage>
#include <cstdint>
#include <vector>

// Apple Screen Sharing codec 1011 (0x3f3), the adaptive still-image
// encoding used on the TCP framebuffer path. The rectangle body is
// length-prefixed by the caller; a decode failure never consumes extra
// bytes, so a bad payload cannot desynchronise the session.
class AppleMvsDecoder {
public:
    AppleMvsDecoder();
    bool decode(const uchar *data, int length, int x, int y, int w, int h, QImage &image);
    int warnings() const { return m_warnings; }
    void clearWarnings() { m_warnings = 0; }
    static bool selfTest();

private:
    struct Bitstream {
        const uchar *data = nullptr;
        int length = 0;
        int bytePos = 0;
        int bitPos = 0;
        bool ok = true;
        int readBit();
        int readBits(int n);
    };

    struct AcResult { int value = 0; bool eob = false; int skip = 0; };

    void warn(const char *text);
    void ensureInit();
    void sync(QImage &image);
    void handleQuantization(const uchar *data, int length);
    bool handleFull(const uchar *data, int length, int x, int y, int w, int h, QImage &image);
    bool handlePartial(const uchar *data, int length, int x, int y, int w, int h, QImage &image);
    void decodeTwoColorBW(Bitstream &bits);
    void decodeSolidOrTwoColor(Bitstream &bits);
    void readYCbCr20(Bitstream &bits, uchar rgb[3]);
    void renderTwoColor(Bitstream &bits, const uchar *a, const uchar *b);
    bool decodeDct(Bitstream &bits, int threshold1, int threshold2, int screenIdx, int &prevDct);
    bool decodePartialDct(Bitstream &bits, int screenIdx, int crLimit, int cbLimit);
    int chromaDcDelta(Bitstream &bits, int baseline);
    bool loadCache(int index);
    void dequantAndRender();
    int transformPlane(int plane, const uchar *quant, uchar *out);
    void putTile(QImage &image, int tileX, int tileY, int tileW, int tileH);
    void copyTile(QImage &image, int srcX, int srcY, int dstX, int dstY, int tileW, int tileH);
    void copyDesc(int from, int to);
    static int leadingOnes(Bitstream &bits);
    static int decodeDcRice(Bitstream &bits);
    static void decodeAc(Bitstream &bits, int zigzagPos, int threshold, AcResult &result);
    static void decodeHuffmanAc(Bitstream &bits, int16_t *coeffs, int offset, int limit);

    int m_warnings = 0;
    bool m_inited = false;
    uchar m_lumaQuant[64]{};
    uchar m_chromaQuant[64]{};
    int16_t m_coeffs[192]{};
    int16_t m_dpcm[192]{};
    quint32 m_tile[64]{};
    int8_t m_cacheEntry[99]{};
    int32_t m_dq[64]{};
    uchar m_ySpatial[64]{};
    uchar m_cbSpatial[64]{};
    uchar m_crSpatial[64]{};
    uchar m_solid[3]{};
    uchar m_colorA[3]{};
    uchar m_colorB[3]{};
    std::vector<int8_t> m_cache;
    int m_cacheWrite = 0;
    int m_cacheLast = 0;
    int m_cacheCount = 0;
    std::vector<quint32> m_cachePixels;
    std::vector<quint32> m_cacheGen;
    quint32 m_quantGen = 1;
    const uchar *m_fbBits = nullptr;
    int m_fbW = 0;
    int m_fbH = 0;
    int m_tilesX = 0;
    int m_tilesN = 0;
    quint32 m_frameSeq = 0;
    std::vector<uchar> m_descThreshold;
    std::vector<int8_t> m_descY;
    std::vector<uchar> m_descCbOk;
    std::vector<int8_t> m_descCbDc;
    std::vector<uchar> m_descCrOk;
    std::vector<int8_t> m_descCrDc;
    std::vector<quint32> m_descSeq;
    std::vector<int32_t> m_descRef;
    std::vector<int16_t> m_descMatchX;
    std::vector<int16_t> m_descMatchY;
};
