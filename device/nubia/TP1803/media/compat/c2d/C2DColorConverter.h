/*
 * Stand-in for libc2dcolorconvert's C2DColorConverter. The real class drives
 * Adreno's proprietary C2D blitter (libC2D2), which doesn't exist on this
 * Mesa-based port, so every conversion setup fails. The OMX components then
 * only offer their native formats: decoders output NV12 (Venus layout),
 * encoders take NV12/YUV input. RGBA encoder input (surface/screen
 * recording) is the one feature this costs.
 */
#ifndef C2D_ColorConverter_H_
#define C2D_ColorConverter_H_

#include <stddef.h>
#include <stdint.h>
#include <unordered_map>

typedef std::unordered_map<int, int> ColorMapping;

#define ALIGN(num, to) (((num) + (to - 1)) & (~(to - 1)))
#define ALIGN8K 8192
#define ALIGN4K 4096
#define ALIGN2K 2048
#define ALIGN512 512
#define ALIGN256 256
#define ALIGN128 128
#define ALIGN64 64
#define ALIGN32 32
#define ALIGN16 16

enum ColorConvertFormat {
    RGB565 = 1,
    YCbCr420Tile,
    YCbCr420SP,
    YCbCr420P,
    YCrCb420P,
    RGBA8888,
    RGBA8888_UBWC,
    NV12_2K,
    NV12_512,
    NV12_128m,
    NV12_UBWC,
    TP10_UBWC,
    YCbCr420_VENUS_P010,
    CbYCrY,
    NO_COLOR_FORMAT
};

typedef struct {
    int32_t numerator;
    int32_t denominator;
} C2DBytesPerPixel;

typedef struct {
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t sliceHeight;
    int32_t lumaAlign;
    int32_t sizeAlign;
    int32_t size;
    C2DBytesPerPixel bpp;
    ColorConvertFormat yuvFormat;
} C2DBuffReq;

typedef enum {
    C2D_INPUT = 0,
    C2D_OUTPUT,
} C2D_PORT;

class C2DColorConverter {
    bool mConversionNeeded;

  public:
    C2DColorConverter() : mConversionNeeded(false) {}
    ~C2DColorConverter() {}

    bool getConversionNeeded() { return mConversionNeeded; }
    void setConversionNeeded(bool needed) { mConversionNeeded = needed; }
    bool isPropChanged(size_t, size_t, size_t, size_t, ColorConvertFormat,
                       ColorConvertFormat, int32_t, size_t) { return true; }
    bool setResolution(size_t, size_t, size_t, size_t, ColorConvertFormat,
                       ColorConvertFormat, int32_t, size_t) { return false; }
    int32_t getBuffSize(int32_t) { return 0; }
    bool getBuffFilledLen(int32_t, unsigned int &filled_length) {
        filled_length = 0;
        return false;
    }
    bool getBuffReq(int32_t, C2DBuffReq *) { return false; }
    bool convertC2D(int, void *, void *, int, void *, void *) { return false; }
};

#endif /* C2D_ColorConverter_H_ */
