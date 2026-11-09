// Hardware JPEG decode on the RK3128's VPU (development record #395, docs/RK3128-LINUX.md "Hardware JPEG decode").
//
// The Hantro G1 decodes a baseline JPEG into semi-planar YCbCr through V4L2 (patch 395-0001, STMicroelectronics' G1 JPEG
// decoder ported to the RK3128: the decoder's video device takes V4L2_PIX_FMT_JPEG on its OUTPUT queue and writes NV12
// for 4:2:0, NV16 for 4:2:2); this helper turns that into the RGB888 buffer stb_image would have made, with stb_image's
// own chroma upsampling and colour conversion, so the only difference from a CPU decode is the IDCT. Anything the
// hardware path does not take (progressive, 12-bit, CMYK or RGB JPEGs, greyscale, 4:4:4 and other sampling factors, too
// big for the CMA pool), a busy HEVC decoder (a photo decode never takes the shared video bus from a playing video) or
// any V4L2 failure returns NULL with the reason, and the caller decodes on the CPU.
//
// Linux build only (SF_LINUX). Thread-safe: every call opens its own V4L2 context and frees it before returning, so no
// CMA memory is held between photos.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What a JPEG's headers say (rk3128_jpeg_parse).
typedef struct {
    int w, h;           // SOF dimensions
    int ncomp;          // 1 or 3 (anything else is refused)
    int hs, vs;         // the luma sampling factors (chroma are 1x1)
    int restart;        // DRI interval, 0 = none
    size_t ecs;         // offset of the entropy-coded data
} Rk3128JpegInfo;

// Parses the headers of a JPEG in memory. 0 when the hardware can decode it (the info is filled), else -1 with the
// reason in why (progressive, 12-bit, RGB/CMYK, greyscale, 4:4:4 or other sampling, size...).
int rk3128_jpeg_parse(const unsigned char *p, size_t n, Rk3128JpegInfo *info, char *why, size_t whylen);

// The decoded planes as the hardware wrote them (one buffer, luma then interleaved CbCr).
typedef struct {
    const uint8_t *y, *c;   // luma (stride ystride) and interleaved CbCr (stride cstride); NULL c: grey (host tests)
    int ystride, cstride;
    int w, h, ncomp, hs, vs;
} Rk3128JpegPlanes;

// The stb_image-exact conversion of planes to RGB888: a malloc'd buffer (free()) of w * h * 3 bytes, rows packed, or NULL
// when memory runs out (nothing is half written: every allocation is made before the work starts). threads <= 1: all on
// the caller; a thread that cannot be started has its rows done on the caller.
uint8_t *rk3128_jpeg_to_rgb(const Rk3128JpegPlanes *pl, int threads);

// Optional synchronous preparation while the dequeued capture allocation is
// still owned by the decoder. NULL requests the unchanged CPU conversion.
// A successful callback owns the shape/interpretation of its malloc'd RGB;
// w/h below still report decoded source dimensions. It MUST complete/cancel
// every hardware reader and release all exports before returning. Nothing
// from pl or capture_fd may be retained between calls.
typedef uint8_t *(*Rk3128JpegConvertCb)(const Rk3128JpegPlanes *pl, int capture_fd,
                                    size_t capture_length, void *arg);

#ifdef RK3128_JPEG_DIAGNOSTIC
// Standalone measurement builds only, never enabled by native/CMakeLists.txt. Called in place of the CPU conversion
// with the dequeued capture buffer still owned/mapped by Hantro. The diagnostic must finish/cancel every DMA reader
// before returning. capture_length includes padded rows; pl->c identifies the actual chroma offset.
uint8_t *rk3128_jpeg_diagnostic_convert(const Rk3128JpegPlanes *pl, int threads, int capture_fd, size_t capture_length);
#endif

// Where a hardware decode's time went, in ms: setup (parse, V4L2 formats, buffers, the copy in), decode (queue to
// dequeue: the VPU's work), convert (the CPU conversion or synchronous converter callback), total (the whole call,
// teardown included; the planes
// callback below and the RK3128_JPEG_TEST_FILL test hook are left out).
typedef struct {
    double setup, decode, convert, total;
} Rk3128JpegTimes;

// Decodes the JPEG at path on the VPU into a malloc'd RGB888 buffer (free()), w x h. NULL when the hardware path is not
// taken; why says why, and is empty when the image was skipped on purpose: under min_pixels (opening the VPU costs more
// than stb takes on a thumbnail) or RK3128_JPEG_HW=0 in the environment.
unsigned char *rk3128_jpeg_hw_load(const char *path, int *w, int *h, long long min_pixels, char *why, size_t whylen,
                                Rk3128JpegTimes *times);
unsigned char *rk3128_jpeg_hw_load_convert(const char *path, int *w, int *h, long long min_pixels,
    char *why, size_t whylen, Rk3128JpegTimes *times, Rk3128JpegConvertCb convert_cb, void *convert_arg);

// The same for a JPEG already in memory. With planes_cb set, it is called with the hardware's planes before the
// conversion, while they are still mapped (the probe compares them with stb_image's).
typedef void (*Rk3128JpegPlanesCb)(const Rk3128JpegPlanes *pl, void *arg);
unsigned char *rk3128_jpeg_hw_decode(const unsigned char *jpg, size_t n, int *w, int *h, char *why, size_t whylen,
                                  Rk3128JpegTimes *times, Rk3128JpegPlanesCb planes_cb, void *cb_arg);

#ifdef __cplusplus
}
#endif
