// SPDX-License-Identifier: MIT
/* #725: independent integer/numerical models and finite public-ABI proofs.
 * No register overrides or physical addresses are accepted by this program. */
#define RGA1_PROBE_MAIN rga1_original_probe_main
#include "rga1-probe.c"
#include <assert.h>
#include <limits.h>

static unsigned coefficient(unsigned a, unsigned straight)
{
    return a + (straight ? 0 : a >> 7);
}
static void pd_factors(unsigned pd, unsigned sa, unsigned da, unsigned *fs, unsigned *fd)
{
    static const unsigned unit = 256;
    switch (pd) {
    case 0:
        *fs = sa;
        *fd = unit - sa;
        break;
    case 1:
        *fs = *fd = 0;
        break;
    case 2:
        *fs = unit;
        *fd = 0;
        break;
    case 3:
        *fs = 0;
        *fd = unit;
        break;
    case 4:
        *fs = unit;
        *fd = unit - sa;
        break;
    case 5:
        *fs = unit - da;
        *fd = unit;
        break;
    case 6:
        *fs = da;
        *fd = 0;
        break;
    case 7:
        *fs = 0;
        *fd = sa;
        break;
    case 8:
        *fs = unit - da;
        *fd = 0;
        break;
    case 9:
        *fs = 0;
        *fd = unit - sa;
        break;
    case 10:
        *fs = da;
        *fd = unit - sa;
        break;
    case 11:
        *fs = unit - da;
        *fd = sa;
        break;
    case 12:
        *fs = unit - da;
        *fd = unit - sa;
        break;
    default:
        abort();
    }
}
static uint8_t sat8(unsigned n)
{
    return n > 255 ? 255 : n;
}
/* Pack the canonical test RGBA tuple using the public fourcc, independently
 * of the driver's format enum and register encoder. */
static void pack_tuple(uint8_t *out, uint32_t format, const uint8_t rgba[4])
{
    struct rgb_layout l = layout(format);
    uint32_t word = 0;
    for (unsigned c = 0; c < 4; c++)
        if (l.bits[c])
            word |= ((uint32_t)rgba[c] >> (8 - l.bits[c])) << l.shifts[c];
    memcpy(out, &word, l.bytes);
}
static void alpha_grid(const char *node)
{
    const uint32_t destinations[] = {V4L2_PIX_FMT_RGBA32, V4L2_PIX_FMT_ABGR32, V4L2_PIX_FMT_ARGB32,
                                     V4L2_PIX_FMT_BGRA32, V4L2_PIX_FMT_RGB24};
    struct test t = {
        .sw = 256, .sh = 256, .dw = 256, .dh = 256, .matrix = 601, .input = V4L2_PIX_FMT_RGBA32};
    uint8_t *src = malloc(262144), *bg = malloc(262144), *initial = malloc(262144),
            *e = malloc(262144);
    if (!src || !bg || !initial || !e)
        die("alpha grid allocation");
    for (unsigned i = 0; i < 65536; i++) {
        unsigned sa = i & 255, da = i >> 8;
        src[4 * i] = sa * 3 / 4;
        src[4 * i + 1] = sa / 2;
        src[4 * i + 2] = sa / 4;
        src[4 * i + 3] = sa;
        bg[4 * i] = da / 4;
        bg[4 * i + 1] = da / 2;
        bg[4 * i + 2] = da * 3 / 4;
        bg[4 * i + 3] = da;
    }
    for (unsigned destination = 0; destination < 5; destination++) {
        t.output = destinations[destination];
        unsigned db = bpp(t.output), opaque = destination == 4;
        for (unsigned i = 0; i < 65536; i++)
            pack_tuple(initial + i * db, t.output, bg + 4 * i);
        for (unsigned calc = 0; calc < 2; calc++)
            for (unsigned mode = 1; mode <= 3; mode++)
                for (unsigned pd = 0; pd <= 12; pd++) {
                    unsigned global = 153;
                    for (unsigned i = 0; i < 65536; i++) {
                        unsigned raw_sa = src[4 * i + 3], sa = mode == 1   ? global
                                                               : mode == 3 ? (raw_sa * global) >> 8
                                                                           : raw_sa;
                        unsigned da = opaque ? global : bg[4 * i + 3], fs, fd;
                        uint8_t rgba[4];
                        pd_factors(pd, coefficient(sa, calc), coefficient(da, calc), &fs, &fd);
                        for (unsigned c = 0; c < 4; c++) {
                            unsigned sc = c == 3 && pd ? sa : src[4 * i + c],
                                     dc = c == 3 ? da : bg[4 * i + c];
                            rgba[c] = c == 3 && !pd ? raw_sa : sat8((sc * fs + dc * fd) >> 8);
                        }
                        pack_tuple(e + i * db, t.output, rgba);
                    }
                    struct setting ss[] = {{.id = V4L2_CID_RGA1_ALPHA_MODE, .value = mode},
                                           {.id = V4L2_CID_RGA1_PORTER_DUFF, .value = pd},
                                           {.id = V4L2_CID_ALPHA_COMPONENT, .value = global},
                                           {.id = V4L2_CID_RGA1_ALPHA_CALC, .value = calc}};
                    uint8_t *a = job(node, &t, src, initial, ss, 4, NULL, NULL, false);
                    char label[110];
                    snprintf(label, sizeof(label),
                             "65536 As/Ad pairs %.4s alpha-mode %u PD %u straight %u",
                             (char *)&t.output, mode, pd, calc);
                    exact(label, a, e, 65536 * db, 0);
                    free(a);
                }
    }
    for (unsigned destination = 0; destination < 4; destination++) {
        t.output = destinations[destination];
        for (unsigned i = 0; i < 65536; i++)
            pack_tuple(initial + 4 * i, t.output, bg + 4 * i);
        for (unsigned calc = 0; calc < 2; calc++)
            for (unsigned mode = 1; mode <= 3; mode++) {
                for (unsigned i = 0; i < 65536; i++) {
                    unsigned da = coefficient(bg[4 * i + 3], calc);
                    uint8_t rgba[4];
                    for (unsigned c = 0; c < 3; c++)
                        rgba[c] = sat8((src[4 * i + c] * (256 - da) + bg[4 * i + c] * da) >> 8);
                    rgba[3] = src[4 * i + 3];
                    pack_tuple(e + 4 * i, t.output, rgba);
                }
                struct setting ss[] = {{.id = V4L2_CID_RGA1_ALPHA_MODE, .value = mode},
                                       {.id = V4L2_CID_RGA1_ALPHA_CALC, .value = calc},
                                       {.id = V4L2_CID_RGA1_MIX_DEST, .value = 1}};
                uint8_t *a = job(node, &t, src, initial, ss, 3, NULL, NULL, false);
                char label[110];
                snprintf(label, sizeof(label),
                         "65536 destination-selected %.4s mix pairs mode %u straight %u",
                         (char *)&t.output, mode, calc);
                exact(label, a, e, 262144, 0);
                free(a);
            }
    }
    free(src);
    free(bg);
    free(initial);
    free(e);
}
static int clip16(int n)
{
    return n < 0 ? 0 : n > 65535 ? 65535 : n;
}
static void gradient_wrap(const char *node)
{
    struct test t = {.sw = 64,
                     .sh = 64,
                     .dw = 64,
                     .dh = 64,
                     .matrix = 601,
                     .input = V4L2_PIX_FMT_RGBA32,
                     .output = V4L2_PIX_FMT_RGBA32};
    uint8_t src[16384] = {0}, e[16384];
    const int steps[] = {-32768, -769, -256, -255, -1, 0, 1, 255, 256, 769, 32767};
    for (unsigned j = 0; j < 11; j++)
        for (unsigned wrap = 0; wrap < 2; wrap++) {
            int32_t gr[8];
            for (unsigned c = 0; c < 4; c++) {
                gr[2 * c] = steps[j];
                gr[2 * c + 1] = steps[10 - j];
            }
            for (unsigned y = 0; y < 64; y++)
                for (unsigned x = 0; x < 64; x++) {
                    int row = 32768 + (int)y * steps[10 - j];
                    unsigned v = wrap ? (unsigned)(row + (int)x * steps[j]) & 65535
                                      : (unsigned)clip16(clip16(row) + (int)x * steps[j]);
                    memset(e + (y * 64 + x) * 4, v >> 8, 4);
                }
            struct setting ss[] = {{.id = V4L2_CID_RGA1_OPERATION, .value = 1},
                                   {.id = V4L2_CID_BG_COLOR, .value = 0x808080},
                                   {.id = V4L2_CID_ALPHA_COMPONENT, .value = 128},
                                   {.id = V4L2_CID_RGA1_GRADIENT, .data = gr, .size = sizeof(gr)},
                                   {.id = V4L2_CID_RGA1_GRADIENT_WRAP, .value = wrap}};
            uint8_t *a = job(node, &t, src, NULL, ss, 5, NULL, NULL, false);
            char label[100];
            snprintf(label, sizeof(label), "8.8 gradient crossing basis %u wrap %u including alpha",
                     j, wrap);
            exact(label, a, e, sizeof(e), 0);
            free(a);
        }
}
static uint8_t boolean3(unsigned code, uint8_t p, uint8_t s, uint8_t d)
{
    unsigned r = 0;
    for (unsigned b = 0; b < 8; b++)
        r |= ((code >> ((((p >> b) & 1) << 2) | (((s >> b) & 1) << 1) | ((d >> b) & 1))) & 1) << b;
    return r;
}
static int mask_buffer(const uint8_t *mask, size_t len)
{
    int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap < 0)
        die("mask heap");
    struct dma_heap_allocation_data a = {.len = (len + 4095) & ~4095U,
                                         .fd_flags = O_RDWR | O_CLOEXEC};
    must(heap, DMA_HEAP_IOCTL_ALLOC, &a, "mask allocation");
    close(heap);
    struct memory m = {.fd = (int)a.fd, .len = a.len};
    m.addr = mmap(NULL, m.len, PROT_READ | PROT_WRITE, MAP_SHARED, m.fd, 0);
    if (m.addr == MAP_FAILED)
        die("mask mmap");
    sync_mem(&m, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    memset(m.addr, 0, m.len);
    memcpy(m.addr, mask, len);
    sync_mem(&m, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    munmap(m.addr, m.len);
    return m.fd;
}
static void rop_masks(const char *node)
{
    int device = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (device < 0)
        die(node);
    struct rga1_mask mask = {.fd = -1};
    must(device, VIDIOC_RGA1_SET_MASK, &mask, "empty mask release");
    const struct {
        int fd;
        unsigned flags;
        int error;
    } invalid[] = {{-2, 0, EINVAL}, {INT_MAX, 0, EBADF}, {-1, 1, EINVAL}, {device, 0, EINVAL}};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        mask.fd = invalid[i].fd;
        mask.flags = invalid[i].flags;
        if (xioctl(device, VIDIOC_RGA1_SET_MASK, &mask) != -1 || errno != invalid[i].error) {
            fprintf(stderr, "invalid mask import accepted or wrong error\n");
            exit(1);
        }
    }
    close(device);
    puts("{\"proof\":\"fixed-width mask ABI rejects invalid fd/flags/non-DMA-BUF\",\"pass\":true}");
    struct test t = {.sw = 64,
                     .sh = 8,
                     .dw = 64,
                     .dh = 8,
                     .matrix = 601,
                     .input = V4L2_PIX_FMT_RGB24,
                     .output = V4L2_PIX_FMT_RGB24};
    uint8_t src[1536], bg[1536], e[1536], pattern[1024] = {0};
    for (unsigned i = 0; i < 1536; i++) {
        src[i] = (i * 31 + i / 7) & 255;
        bg[i] = (i * 17 + i / 11) & 255;
    }
    pattern[0] = 0x96;
    pattern[1] = 0x69;
    pattern[2] = 0xa5;
    for (unsigned code = 0; code < 16; code++) {
        for (unsigned i = 0; i < 1536; i++) {
            unsigned r = 0;
            for (unsigned b = 0; b < 8; b++)
                r |= ((code >> ((((pattern[i % 3] >> b) & 1) << 1) | ((bg[i] >> b) & 1))) & 1) << b;
            e[i] = r;
        }
        struct setting ss[] = {{.id = V4L2_CID_RGA1_ROP, .value = code + 1},
                               {.id = V4L2_CID_RGA1_ROP_MODE, .value = 0},
                               {.id = V4L2_CID_RGA1_PATTERN, .data = pattern, .size = 1024}};
        uint8_t *a = job(node, &t, src, bg, ss, 3, NULL, NULL, false);
        char label[80];
        snprintf(label, sizeof(label), "raw ROP2 P,D truth table %x", code);
        exact(label, a, e, sizeof(e), 0);
        free(a);
    }
    for (unsigned basis = 0; basis < 513; basis++) {
        uint32_t words[16] = {0};
        if (basis < 512)
            words[basis / 32] = 1U << (basis % 32);
        else
            for (unsigned i = 0; i < 16; i++)
                words[i] = 0x69a5965aU ^ (i * 0x1020104U);
        unsigned lo = 0x96, hi = 0x69;
        for (unsigned y = 0; y < 8; y++)
            for (unsigned x = 0; x < 64; x++)
                for (unsigned c = 0; c < 3; c++) {
                    unsigned i = (y * 64 + x) * 3 + c,
                             one = (words[y * 2 + x / 32] >> (31 - (x % 32))) & 1;
                    e[i] = boolean3(one ? lo : hi, pattern[c], src[i], bg[i]);
                }
        int fd = mask_buffer((uint8_t *)words, sizeof(words));
        struct setting ss[] = {{.id = V4L2_CID_RGA1_ROP, .value = (hi << 8) + lo + 1},
                               {.id = V4L2_CID_RGA1_ROP_MODE, .value = 2},
                               {.id = VIDIOC_RGA1_SET_MASK, .value = fd, .close_after = true},
                               {.id = V4L2_CID_RGA1_PATTERN, .data = pattern, .size = 1024}};
        uint8_t *a = job(node, &t, src, bg, ss, 4, NULL, NULL, basis == 512);
        char label[100];
        snprintf(label, sizeof(label), "ROP4 mask %s %u, imported fd closed before DMA",
                 basis == 512 ? "nonconstant" : "one-hot", basis);
        exact(label, a, e, sizeof(e), 0);
        free(a);
    }
    const unsigned widths[] = {2, 17, 33, 65};
    const uint32_t formats[] = {V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_RGBX32, V4L2_PIX_FMT_RGB565};
    for (unsigned f = 0; f < 3; f++)
        for (unsigned w = 0; w < 4; w++) {
            struct test padded = {.sw = widths[w],
                                  .sh = 7,
                                  .dw = widths[w],
                                  .dh = 7,
                                  .matrix = 601,
                                  .input = formats[f],
                                  .output = formats[f]};
            uint8_t source[2048], background[2048], expected[2048];
            uint32_t words[21];
            unsigned stride = line_bytes(padded.input, padded.sw), db = bpp(padded.input),
                     n = stride * 7;
            unsigned rowwords = (padded.sw + 31) / 32;
            for (unsigned i = 0; i < rowwords * 7; i++)
                words[i] = 0xa59669a5U ^ (i * 0x13017023U);
            for (unsigned i = 0; i < n; i++) {
                source[i] = (i * 31 + i / 7) & 255;
                background[i] = (i * 17 + i / 11) & 255;
            }
            memcpy(expected, background, n);
            for (unsigned y = 0; y < 7; y++)
                for (unsigned x = 0; x < padded.sw; x++)
                    for (unsigned c = 0; c < db; c++) {
                        unsigned one = (words[y * rowwords + x / 32] >> (31 - x % 32)) & 1,
                                 i = y * stride + x * db + c;
                        expected[i] =
                            boolean3(one ? 0x96 : 0x69, pattern[c], source[i], background[i]);
                    }
            int fd = mask_buffer((uint8_t *)words, rowwords * 7 * 4);
            struct setting ss[] = {{.id = V4L2_CID_RGA1_ROP, .value = 0x6997},
                                   {.id = V4L2_CID_RGA1_ROP_MODE, .value = 2},
                                   {.id = VIDIOC_RGA1_SET_MASK, .value = fd, .close_after = true},
                                   {.id = V4L2_CID_RGA1_PATTERN, .data = pattern, .size = 1024}};
            uint8_t *a = job(node, &padded, source, background, ss, 4, NULL, NULL, f == 1);
            char label[120];
            snprintf(label, sizeof(label),
                     "ROP4 %.4s width %u, nonzero mask padding and closed original fd%s",
                     (char *)&padded.input, padded.sw, f == 1 ? ", RGB contract" : "");
            if (f == 1) {
                /* X is undefined by this fourcc. Compare every RGB byte;
                 * these four-byte pixels have no row padding. The job helper
                 * also checks the complete source and both allocation tails. */
                uint8_t actual_rgb[2048], expected_rgb[2048];
                unsigned offset = 0;
                for (unsigned y = 0; y < 7; y++)
                    for (unsigned x = 0; x < padded.sw; x++)
                        for (unsigned c = 0; c < 3; c++) {
                            actual_rgb[offset] = a[y * stride + x * 4 + c];
                            expected_rgb[offset++] = expected[y * stride + x * 4 + c];
                        }
                exact(label, actual_rgb, expected_rgb, offset, 0);
            } else
                exact(label, a, expected, n, 0);
            free(a);
        }
}
static void pattern_phase(const char *node)
{
    struct test t = {.sw = 64,
                     .sh = 48,
                     .dw = 64,
                     .dh = 48,
                     .matrix = 601,
                     .input = V4L2_PIX_FMT_RGB24,
                     .output = V4L2_PIX_FMT_RGB24};
    uint8_t src[9216] = {0}, e[9216], pattern[1024] = {0};
    for (unsigned i = 0; i < 1024; i++)
        pattern[i] = (i * 37 + i / 3) & 255;
    struct v4l2_rect compose = {.left = 7, .top = 5, .width = 43, .height = 31};
    for (unsigned oy = 0; oy < 3; oy++)
        for (unsigned ox = 0; ox < 5; ox++) {
            memset(e, 0xa5, sizeof(e));
            for (unsigned y = 0; y < 31; y++)
                for (unsigned x = 0; x < 43; x++)
                    memcpy(e + ((y + 5) * 64 + x + 7) * 3,
                           pattern + (((y + oy) % 3) * 5 + (x + ox) % 5) * 3, 3);
            struct setting ss[] = {{.id = V4L2_CID_RGA1_OPERATION, .value = 5},
                                   {.id = V4L2_CID_RGA1_PATTERN_SIZE, .value = (3 << 16) | 5},
                                   {.id = V4L2_CID_RGA1_PATTERN_ORIGIN, .value = (oy << 16) | ox},
                                   {.id = V4L2_CID_RGA1_PATTERN, .data = pattern, .size = 1024}};
            uint8_t *a = job(node, &t, src, NULL, ss, 4, NULL, &compose, false);
            char label[100];
            snprintf(label, sizeof(label), "5x3 pattern phase %u,%u with untouched compose borders",
                     ox, oy);
            exact(label, a, e, sizeof(e), 0);
            free(a);
        }
}
/* Fixed-point coefficient quantization is part of the hardware's filter,
 * independently expressed from the Keys cubic polynomial (a=-1). */
static double keys(double x)
{
    x = fabs(x);
    return x <= 1 ? x * x * x - 2 * x * x + 1 : x < 2 ? -x * x * x + 5 * x * x - 8 * x + 4 : 0;
}
static void weights(unsigned phase, unsigned mode, int w[4])
{
    if (mode == 1) {
        w[0] = 0;
        w[1] = 64 - (int)phase;
        w[2] = phase;
        w[3] = 0;
        return;
    }
    double t = (phase >> 1) / 32.0;
    for (unsigned j = 0; j < 4; j++)
        w[j] = (int)floor(keys(t - ((int)j - 1)) * 64 + .5);
}
static unsigned endpoint(unsigned i, unsigned src, unsigned dst)
{
    unsigned ratio = ((src - 1) * 65536U) / (dst - 1);
    return (i / 8) * (ratio >> 3) * 16 + (i % 8) * (ratio >> 2);
}
static unsigned bounded(int n, unsigned max)
{
    return n < 0 ? 0 : (unsigned)n >= max ? max - 1 : (unsigned)n;
}
static void sample_model(const uint8_t *src, uint8_t *e, const struct test *t, unsigned mode)
{
    unsigned ss = line_bytes(t->input, t->sw), ds = line_bytes(t->output, t->dw);
    memset(e, 0xa5, sizeimage(t->output, t->dw, t->dh));
    for (unsigned y = 0; y < t->dh; y++)
        for (unsigned x = 0; x < t->dw; x++) {
            unsigned sx = endpoint(x, t->sw, t->dw), sy = endpoint(y, t->sh, t->dh);
            if (!mode) {
                unsigned ix = bounded((sx + 8192) >> 14, t->sw),
                         iy = bounded((sy + 8192) >> 14, t->sh);
                memcpy(e + y * ds + x * 3, src + iy * ss + ix * 3, 3);
                continue;
            }
            int wx[4], wy[4];
            weights((sx >> 8) & 63, mode, wx);
            weights((sy >> 8) & 63, mode, wy);
            for (unsigned c = 0; c < 3; c++) {
                int v = 0;
                for (unsigned k = 0; k < 4; k++) {
                    int h = 0;
                    unsigned ix = bounded((int)(sx >> 14) + (int)k - 1, t->sw);
                    for (unsigned j = 0; j < 4; j++) {
                        unsigned iy = bounded((int)(sy >> 14) + (int)j - 1, t->sh);
                        h += src[iy * ss + ix * 3 + c] * wy[j];
                    }
                    if (h < 0)
                        h = 0;
                    if (h > 255 * 64)
                        h = 255 * 64;
                    if (mode == 2)
                        h = (h / 64) * 64;
                    v += h * wx[k];
                }
                e[y * ds + x * 3 + c] = v < 0 ? 0 : v >= 255 * 4096 ? 255 : v / 4096;
            }
        }
}
static void sampling_nonconstant(const char *node)
{
    const unsigned sizes[][4] = {{32, 24, 53, 37}, {17, 13, 65, 49}, {33, 25, 257, 193}};
    for (unsigned shape = 0; shape < 3; shape++)
        for (unsigned fixture = 0; fixture < 3; fixture++)
            for (unsigned mode = 0; mode < 3; mode++) {
                struct test t = {.sw = sizes[shape][0],
                                 .sh = sizes[shape][1],
                                 .dw = sizes[shape][2],
                                 .dh = sizes[shape][3],
                                 .matrix = 601,
                                 .input = V4L2_PIX_FMT_RGB24,
                                 .output = V4L2_PIX_FMT_RGB24};
                unsigned sn = sizeimage(t.input, t.sw, t.sh), dn = sizeimage(t.output, t.dw, t.dh);
                uint8_t *src = calloc(1, sn), *e = malloc(dn);
                if (!src || !e)
                    die("sampling allocation");
                for (unsigned y = 0; y < t.sh; y++)
                    for (unsigned x = 0; x < t.sw; x++)
                        for (unsigned c = 0; c < 3; c++) {
                            unsigned v =
                                fixture == 0
                                    ? (c == 0   ? x * 217 / (t.sw - 1)
                                       : c == 1 ? y * 207 / (t.sh - 1)
                                                : (x == t.sw / 2 && y == t.sh / 2 ? 255 : 0))
                                : fixture == 1 ? (x * 173 + y * 117 + c * 61 + x * y * 31) & 255
                                : ((x + y + c) & 1) ? 255
                                                    : 0;
                            src[y * line_bytes(t.input, t.sw) + 3 * x + c] = v;
                        }
                sample_model(src, e, &t, mode);
                struct setting st = {.id = V4L2_CID_RGA1_SAMPLING, .value = mode};
                uint8_t *a = job(node, &t, src, NULL, &st, 1, NULL, NULL, false);
                char label[120];
                snprintf(
                    label, sizeof(label),
                    "nonconstant sampling %u shape %u fixture %u, independent fixed-point model",
                    mode, shape, fixture);
                exact(label, a, e, dn, mode ? 1 : 0);
                free(src);
                free(e);
                free(a);
            }
}

struct multi_memory {
    struct memory p[3];
    unsigned n, size[3];
    bool imported;
};
static struct multi_memory multi_alloc(int fd, enum v4l2_buf_type type,
                                       const struct v4l2_pix_format_mplane *pix, bool imported)
{
    struct multi_memory m = {.n = pix->num_planes, .imported = imported};
    struct v4l2_requestbuffers r = {
        .type = type, .memory = imported ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP, .count = 1};
    must(fd, VIDIOC_REQBUFS, &r, "multiplane REQBUFS");
    struct v4l2_plane planes[3] = {0};
    struct v4l2_buffer b = {.type = type, .memory = r.memory, .length = m.n, .m.planes = planes};
    if (!imported)
        must(fd, VIDIOC_QUERYBUF, &b, "multiplane QUERYBUF");
    for (unsigned p = 0; p < m.n; p++) {
        m.size[p] = pix->plane_fmt[p].sizeimage;
        if (imported) {
            uint8_t *zero = calloc(1, m.size[p]);
            if (!zero)
                die("plane allocation");
            m.p[p].fd = mask_buffer(zero, m.size[p]);
            free(zero);
            m.p[p].len = (m.size[p] + 4095) & ~4095U;
        } else {
            struct v4l2_exportbuffer e = {.type = type, .plane = p, .flags = O_RDWR | O_CLOEXEC};
            must(fd, VIDIOC_EXPBUF, &e, "multiplane EXPBUF");
            m.p[p].fd = e.fd;
            m.p[p].len = planes[p].length;
        }
        m.p[p].addr = mmap(NULL, m.p[p].len, PROT_READ | PROT_WRITE, MAP_SHARED,
                           imported ? m.p[p].fd : fd, imported ? 0 : planes[p].m.mem_offset);
        if (m.p[p].addr == MAP_FAILED)
            die("multiplane mmap");
    }
    return m;
}
static void multi_queue(int fd, enum v4l2_buf_type type, const struct multi_memory *m)
{
    struct v4l2_plane p[3] = {0};
    struct v4l2_buffer b = {.type = type,
                            .memory = m->imported ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP,
                            .length = m->n,
                            .m.planes = p};
    for (unsigned i = 0; i < m->n; i++) {
        p[i].length = m->p[i].len;
        p[i].bytesused = V4L2_TYPE_IS_OUTPUT(type) ? m->size[i] : 0;
        if (m->imported)
            p[i].m.fd = m->p[i].fd;
    }
    must(fd, VIDIOC_QBUF, &b, "multiplane QBUF");
}
static void multi_dequeue(int fd, enum v4l2_buf_type type, const struct multi_memory *m)
{
    struct v4l2_plane p[3] = {0};
    struct v4l2_buffer b = {.type = type,
                            .memory = m->imported ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP,
                            .length = m->n,
                            .m.planes = p};
    double deadline = now_ms() + 3000;
    struct pollfd pollfd = {.fd = fd, .events = V4L2_TYPE_IS_OUTPUT(type) ? POLLOUT : POLLIN};
    for (;;) {
        if (!xioctl(fd, VIDIOC_DQBUF, &b))
            break;
        if (errno != EAGAIN)
            die("multiplane DQBUF");
        if (now_ms() >= deadline || poll(&pollfd, 1, (int)(deadline - now_ms()) + 1) <= 0)
            die("multiplane deadline");
    }
    if (b.flags & V4L2_BUF_FLAG_ERROR) {
        fprintf(stderr, "multiplane DMA returned an error\n");
        exit(1);
    }
}
static uint8_t *multi_job(const char *node, const struct test *t, const uint8_t *raw,
                          bool prescale_op, bool imported)
{
    int fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        die(node);
    enum v4l2_buf_type type[2] = {V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    struct v4l2_control c = {.id = V4L2_CID_RGA1_OPERATION, .value = prescale_op ? 6 : 0};
    must(fd, VIDIOC_S_CTRL, &c, "multiplane operation");
    struct v4l2_format f[2] = {{.type = type[0]}, {.type = type[1]}};
    for (unsigned side = 0; side < 2; side++) {
        struct v4l2_pix_format_mplane *q = &f[side].fmt.pix_mp;
        q->width = side ? t->dw : t->sw;
        q->height = side ? t->dh : t->sh;
        q->pixelformat =
            side ? (prescale_op ? V4L2_PIX_FMT_YVU422M : V4L2_PIX_FMT_RGB24) : V4L2_PIX_FMT_YVU422M;
        q->colorspace = V4L2_COLORSPACE_SMPTE170M;
        q->ycbcr_enc = V4L2_YCBCR_ENC_601;
        q->quantization =
            side && !prescale_op ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE;
        q->flags = side ? V4L2_PIX_FMT_FLAG_SET_CSC : 0;
        must(fd, VIDIOC_S_FMT, &f[side], "YVU422M format");
        if (q->num_planes != (side && !prescale_op ? 1 : 3) ||
            q->pixelformat != (side && !prescale_op ? V4L2_PIX_FMT_RGB24 : V4L2_PIX_FMT_YVU422M)) {
            fprintf(stderr, "YVU422M negotiation changed\n");
            exit(1);
        }
    }
    c.id = V4L2_CID_ROTATE;
    c.value = t->angle;
    must(fd, VIDIOC_S_CTRL, &c, "multiplane rotation");
    c.id = V4L2_CID_HFLIP;
    c.value = t->flip & 1;
    must(fd, VIDIOC_S_CTRL, &c, "multiplane reflection");
    c.id = V4L2_CID_VFLIP;
    c.value = !!(t->flip & 2);
    must(fd, VIDIOC_S_CTRL, &c, "multiplane reflection");
    struct multi_memory m[2] = {multi_alloc(fd, type[0], &f[0].fmt.pix_mp, imported),
                                multi_alloc(fd, type[1], &f[1].fmt.pix_mp, imported)};
    size_t offsets[3] = {0, t->sw * t->sh * 3 / 2, t->sw * t->sh};
    for (unsigned side = 0; side < 2; side++)
        for (unsigned p = 0; p < m[side].n; p++) {
            sync_mem(&m[side].p[p], DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            memset(m[side].p[p].addr, 0xa5, m[side].p[p].len);
            if (!side)
                memcpy(m[side].p[p].addr, raw + offsets[p], m[side].size[p]);
            sync_mem(&m[side].p[p], DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        }
    multi_queue(fd, type[0], &m[0]);
    multi_queue(fd, type[1], &m[1]);
    must(fd, VIDIOC_STREAMON, &type[0], "multiplane output start");
    must(fd, VIDIOC_STREAMON, &type[1], "multiplane capture start");
    multi_dequeue(fd, type[0], &m[0]);
    multi_dequeue(fd, type[1], &m[1]);
    must(fd, VIDIOC_STREAMOFF, &type[0], "multiplane output stop");
    must(fd, VIDIOC_STREAMOFF, &type[1], "multiplane capture stop");
    unsigned n = prescale_op ? t->dw * t->dh * 2 : sizeimage(V4L2_PIX_FMT_RGB24, t->dw, t->dh);
    uint8_t *result = malloc(n);
    if (!result)
        die("multiplane result");
    for (unsigned side = 0; side < 2; side++)
        for (unsigned p = 0; p < m[side].n; p++) {
            struct memory *mp = &m[side].p[p];
            sync_mem(mp, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            if (!side && memcmp(mp->addr, raw + offsets[p], m[side].size[p])) {
                fprintf(stderr, "multiplane source changed\n");
                exit(1);
            }
            for (size_t i = m[side].size[p]; i < mp->len; i++)
                if (((uint8_t *)mp->addr)[i] != 0xa5) {
                    fprintf(stderr, "multiplane allocation tail changed\n");
                    exit(1);
                }
            if (side) {
                unsigned offset = prescale_op ? (p == 0   ? 0
                                                 : p == 1 ? t->dw * t->dh * 3 / 2
                                                          : t->dw * t->dh)
                                              : 0;
                memcpy(result + offset, mp->addr, m[side].size[p]);
            }
            sync_mem(mp, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            munmap(mp->addr, mp->len);
            close(mp->fd);
        }
    close(fd);
    return result;
}
static void multiplane_suite(const char *node)
{
    struct test t = {.sw = 64,
                     .sh = 48,
                     .dw = 64,
                     .dh = 48,
                     .matrix = 601,
                     .input = V4L2_PIX_FMT_YUV422P,
                     .output = V4L2_PIX_FMT_RGB24};
    uint8_t raw[16384], e[16384];
    for (unsigned y = 0; y < 48; y++)
        for (unsigned x = 0; x < 64; x++) {
            raw[y * 64 + x] = 64 + x + y;
            raw[3072 + y * 32 + x / 2] = 96;
            raw[4608 + y * 32 + x / 2] = 176;
        }
    for (unsigned imported = 0; imported < 2; imported++)
        for (t.angle = 0; t.angle < 360; t.angle += 90)
            for (t.flip = 0; t.flip < 4; t.flip++) {
                memset(e, 0xa5, sizeof(e));
                reference(raw, e, &t);
                uint8_t *a = multi_job(node, &t, raw, false, imported);
                char label[100];
                snprintf(label, sizeof(label), "YVU422M three %s planes rotation %u reflection %u",
                         imported ? "imported" : "MMAP", t.angle, t.flip);
                uint8_t *one = job(node, &t, raw, NULL, NULL, 0, NULL, NULL, false);
                exact(label, a, one, 9216, 0);
                double mean, rmse;
                unsigned maxerr;
                if (!compare(one, e, &t, &mean, &rmse, &maxerr)) {
                    fprintf(stderr, "established YUV transform reference failed max=%u\n", maxerr);
                    failures++;
                }
                free(one);
                free(a);
            }
    t.angle = t.flip = 0;
    t.sw = t.sh = 64;
    for (unsigned y = 0; y < 64; y++)
        for (unsigned x = 0; x < 64; x++) {
            raw[y * 64 + x] = 32 + 2 * x + y;
            raw[4096 + y * 32 + x / 2] = 32 + (x / 2 % 4) * 8 + (y % 4) * 4;
            raw[6144 + y * 32 + x / 2] = 192 + (x / 2 % 4) * 4 - (y % 4) * 8;
        }
    for (unsigned hx = 0; hx < 4; hx++)
        for (unsigned vy = 0; vy < 4; vy++) {
            t.dw = 64 >> hx;
            t.dh = 64 >> vy;
            unsigned ax = 1 << hx, ay = 1 << vy;
            for (unsigned plane = 0; plane < 3; plane++) {
                unsigned sw = plane ? 32 : 64, dw = plane ? t.dw / 2 : t.dw,
                         srcbase = plane ? 4096 + (plane - 1) * 2048 : 0,
                         dstbase = plane ? t.dw * t.dh + (plane - 1) * t.dw * t.dh / 2 : 0;
                for (unsigned y = 0; y < t.dh; y++)
                    for (unsigned x = 0; x < dw; x++) {
                        unsigned v = 0;
                        for (unsigned oy = 0; oy < ay; oy++) {
                            unsigned row = 0;
                            for (unsigned ox = 0; ox < ax; ox++)
                                row += raw[srcbase + (y * ay + oy) * sw + x * ax + ox];
                            v += row / ax;
                        }
                        e[dstbase + y * dw + x] = v / ay;
                    }
            }
            uint8_t *a = multi_job(node, &t, raw, true, !!((hx + vy) & 1));
            char label[100];
            snprintf(label, sizeof(label), "YVU422M three-plane prescale 1/%u by 1/%u", ax, ay);
            exact(label, a, e, t.dw * t.dh * 2, 1);
            free(a);
        }
}
static void standalone_prescale(const char *node)
{
    const uint32_t formats[] = {V4L2_PIX_FMT_RGB24,   V4L2_PIX_FMT_BGR24,   V4L2_PIX_FMT_RGB565,
                                V4L2_PIX_FMT_RGBA32,  V4L2_PIX_FMT_ABGR32,  V4L2_PIX_FMT_ARGB32,
                                V4L2_PIX_FMT_BGRA32,  V4L2_PIX_FMT_RGBA555, V4L2_PIX_FMT_BGRA555,
                                V4L2_PIX_FMT_RGBA444, V4L2_PIX_FMT_BGRA444, V4L2_PIX_FMT_NV12,
                                V4L2_PIX_FMT_NV21,    V4L2_PIX_FMT_NV16,    V4L2_PIX_FMT_NV61,
                                V4L2_PIX_FMT_YUV420,  V4L2_PIX_FMT_YVU420,  V4L2_PIX_FMT_YUV422P};
    uint8_t raw[65536], e[65536];
    for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++)
        for (unsigned hx = 0; hx < 4; hx++)
            for (unsigned vy = 0; vy < 4; vy++) {
                struct test t = {.sw = 128,
                                 .sh = 128,
                                 .dw = 128 >> hx,
                                 .dh = 128 >> vy,
                                 .matrix = 601,
                                 .input = formats[f],
                                 .output = formats[f]};
                unsigned ax = 1 << hx, ay = 1 << vy, db = bpp(t.input);
                unsigned n = sizeimage(t.input, 128, 128);
                for (unsigned i = 0; i < n; i++)
                    raw[i] = (i * 31 + i / 59) & 255;
                memset(e, 0xa5, sizeof(e));
                for (unsigned y = 0; y < t.dh; y++)
                    for (unsigned x = 0; x < t.dw; x++) {
                        if (yuv(t.input)) {
                            unsigned v = 0;
                            for (unsigned oy = 0; oy < ay; oy++) {
                                unsigned row = 0;
                                for (unsigned ox = 0; ox < ax; ox++)
                                    row += raw[(y * ay + oy) * 128 + x * ax + ox];
                                v += row / ax;
                            }
                            e[y * t.dw + x] = v / ay;
                        } else {
                            struct rgb_layout l = layout(t.input);
                            uint32_t word = 0;
                            for (unsigned c = 0; c < 4; c++) {
                                if (!l.bits[c])
                                    continue;
                                unsigned mask = (1U << l.bits[c]) - 1, code = 0;
                                if (c == 3) {
                                    uint32_t last = 0;
                                    memcpy(&last,
                                           raw + ((y * ay + ay - 1) * 128 + x * ax + ax - 1) * db,
                                           db);
                                    code = (last >> l.shifts[c]) & mask;
                                } else {
                                    unsigned v = 0;
                                    for (unsigned oy = 0; oy < ay; oy++) {
                                        unsigned row = 0;
                                        for (unsigned ox = 0; ox < ax; ox++) {
                                            uint32_t value = 0;
                                            memcpy(&value,
                                                   raw + ((y * ay + oy) * 128 + x * ax + ox) * db,
                                                   db);
                                            row += ((value >> l.shifts[c]) & mask)
                                                   << (8 - l.bits[c]);
                                        }
                                        v += row / ax;
                                    }
                                    code = (v / ay) >> (8 - l.bits[c]);
                                }
                                word |= code << l.shifts[c];
                            }
                            memcpy(e + (y * t.dw + x) * db, &word, db);
                        }
                    }
                if (yuv(t.input))
                    for (unsigned component = 0; component < 2; component++) {
                        unsigned ch = y420(t.input) ? t.dh / 2 : t.dh;
                        for (unsigned y = 0; y < ch; y++)
                            for (unsigned x = 0; x < t.dw / 2; x++) {
                                unsigned v = 0;
                                for (unsigned oy = 0; oy < ay; oy++) {
                                    unsigned row = 0;
                                    for (unsigned ox = 0; ox < ax; ox++)
                                        row += raw[chroma_offset(
                                            t.input, 128, 128, 2 * (x * ax + ox),
                                            (y * ay + oy) * (y420(t.input) ? 2 : 1), component)];
                                    v += row / ax;
                                }
                                e[chroma_offset(t.output, t.dw, t.dh, 2 * x,
                                                y * (y420(t.input) ? 2 : 1), component)] = v / ay;
                            }
                    }
                struct setting st = {.id = V4L2_CID_RGA1_OPERATION, .value = 6};
                uint8_t *a = job(node, &t, raw, NULL, &st, 1, NULL, NULL, false);
                char label[100];
                snprintf(label, sizeof(label),
                         "standalone %.4s prescale 1/%u by 1/%u incl alpha/planes",
                         (char *)&t.input, ax, ay);
                exact(label, a, e, sizeimage(t.output, t.dw, t.dh), db == 2 ? 0 : 1);
                free(a);
            }
}

/* Affine reference uses mathematical inverse coordinates and explicit register
 * precision, not the driver's emitter or its address/tiling implementation. */
static int64_t floor_shift(int64_t v, unsigned bits)
{
    int64_t scale = INT64_C(1) << bits;
    return v >= 0 ? v / scale : -((-v + scale - 1) / scale);
}
static void affine_model(const uint8_t *raw, uint8_t *e, const struct test *t, unsigned aa,
                         unsigned sampling)
{
    int64_t sn = llround(sin(t->angle * M_PI / 180) * 65536),
            cs = llround(cos(t->angle * M_PI / 180) * 65536);
    unsigned uw = t->angle == 90 || t->angle == 270 ? t->dh : t->dw,
             uh = t->angle == 90 || t->angle == 270 ? t->dw : t->dh;
    int64_t xs = ((t->sw - 1) * 65536U) / (uw - 1), ys = ((t->sh - 1) * 65536U) / (uh - 1);
    int64_t a = xs * cs, b = xs * sn, c = -ys * sn, d = ys * cs;
    if (t->flip & 1) {
        a = -a;
        c = -c;
    }
    if (t->flip & 2) {
        b = -b;
        d = -d;
    }
    int64_t tx = (int64_t)(t->sw - 1) * 2147483648LL - (a * (t->dw - 1) + b * (t->dh - 1)) / 2;
    int64_t ty = (int64_t)(t->sh - 1) * 2147483648LL - (c * (t->dw - 1) + d * (t->dh - 1)) / 2;
    if (!(t->angle % 90)) {
        tx = (a < 0 ? -a * (t->dw - 1) : 0) + (b < 0 ? -b * (t->dh - 1) : 0);
        ty = (c < 0 ? -c * (t->dw - 1) : 0) + (d < 0 ? -d * (t->dh - 1) : 0);
    }
    int64_t xmin = tx + (a < 0 ? 7 * a : 0) + (b < 0 ? 7 * b : 0) -
                   ((aa || sampling == 2 || t->angle % 90) ? 4294967296LL : 0);
    int64_t ymin = ty + (c < 0 ? 7 * c : 0) + (d < 0 ? 7 * d : 0) -
                   ((aa || sampling == 2 || t->angle % 90) ? 4294967296LL : 0);
    int64_t ox = floor_shift(xmin, 29) * 2048 + floor_shift(tx - xmin, 18);
    int64_t oy = floor_shift(ymin, 29) * 2048 + floor_shift(ty - ymin, 18);
    unsigned ss = line_bytes(t->input, t->sw), ds = line_bytes(t->output, t->dw);
    memset(e, 0xa5, sizeimage(t->output, t->dw, t->dh));
    for (unsigned y = 0; y < t->dh; y++)
        for (unsigned x = 0; x < t->dw; x++) {
            int64_t sx = ox + (x / 8) * floor_shift(8 * a, 22) * 16 +
                         (y / 8) * floor_shift(8 * b, 22) * 16 + (x % 8) * floor_shift(a, 18) +
                         (y % 8) * floor_shift(b, 18);
            int64_t sy = oy + (x / 8) * floor_shift(8 * c, 22) * 16 +
                         (y / 8) * floor_shift(8 * d, 22) * 16 + (x % 8) * floor_shift(c, 18) +
                         (y % 8) * floor_shift(d, 18);
            int ix = floor_shift(sx, 14), iy = floor_shift(sy, 14);
            unsigned fx = (unsigned)floor_shift(sx, 8) & 63, fy = (unsigned)floor_shift(sy, 8) & 63;
            double value[3] = {0};
            if (ix >= -1 && iy >= -1 && ix < (int)t->sw && iy < (int)t->sh) {
                uint64_t covx = sx < 0                               ? sx + 16384
                                : sx >= (int64_t)(t->sw - 1) * 16384 ? (int64_t)t->sw * 16384 - sx
                                                                     : 16384;
                uint64_t covy = sy < 0                               ? sy + 16384
                                : sy >= (int64_t)(t->sh - 1) * 16384 ? (int64_t)t->sh * 16384 - sy
                                                                     : 16384;
                unsigned coverage = (255 * covx * covy + (UINT64_C(1) << 27)) >> 28;
                unsigned al = aa && coverage < 255 ? coefficient((coverage * 255) >> 8, 0) : 256;
                if (!sampling) {
                    unsigned px = bounded(floor_shift(sx + 8192, 14), t->sw);
                    unsigned py = bounded(floor_shift(sy + 8192, 14), t->sh);
                    rgb_read(raw + py * ss + px * bpp(t->input), t->input, value);
                } else {
                    int wx[4], wy[4];
                    weights(fx, sampling, wx);
                    weights(fy, sampling, wy);
                    for (unsigned xx = 0; xx < 4; xx++) {
                        double vertical[3] = {0};
                        for (unsigned yy = 0; yy < 4; yy++) {
                            double color[3];
                            unsigned px = bounded(ix + (int)xx - 1, t->sw),
                                     py = bounded(iy + (int)yy - 1, t->sh);
                            rgb_read(raw + py * ss + px * bpp(t->input), t->input, color);
                            for (unsigned k = 0; k < 3; k++)
                                vertical[k] += color[k] * wy[yy];
                        }
                        for (unsigned k = 0; k < 3; k++) {
                            double v = fmax(0, fmin(255 * 64, vertical[k]));
                            value[k] += (int)(v / 64) * wx[xx];
                        }
                    }
                    for (unsigned k = 0; k < 3; k++)
                        value[k] = fmax(0, fmin(255, floor(value[k] / 64)));
                }
                for (unsigned k = 0; k < 3; k++)
                    value[k] = (unsigned)value[k] * al / 256;
            }
            rgb_store(e + y * ds + x * bpp(t->output), t->output, value);
        }
}
static void arbitrary_bounds(const char *node)
{
    const unsigned shapes[][4] = {
        {2048, 4, 2048, 4}, {4, 2048, 4, 2048}, {2048, 1024, 1026, 514}, {96, 72, 129, 97}};
    const unsigned angles[] = {45, 135, 315};
    for (unsigned shape = 0; shape < 4; shape++)
        for (unsigned j = 0; j < 3; j++)
            for (unsigned aa = 0; aa < 2; aa++) {
                struct test t = {.sw = shapes[shape][0],
                                 .sh = shapes[shape][1],
                                 .dw = shapes[shape][2],
                                 .dh = shapes[shape][3],
                                 .matrix = 601,
                                 .input = V4L2_PIX_FMT_RGB24,
                                 .output = V4L2_PIX_FMT_RGB24,
                                 .angle = angles[j]};
                unsigned sn = sizeimage(t.input, t.sw, t.sh), dn = sizeimage(t.output, t.dw, t.dh);
                uint8_t *raw = calloc(1, sn), *e = malloc(dn);
                if (!raw || !e)
                    die("affine bounds allocation");
                for (unsigned y = 0; y < t.sh; y++)
                    for (unsigned x = 0; x < t.sw; x++) {
                        uint8_t *pixel = raw + y * line_bytes(t.input, t.sw) + x * 3;
                        pixel[0] = x * 197 / (t.sw - 1);
                        pixel[1] = y * 197 / (t.sh - 1);
                        pixel[2] = 77;
                    }
                affine_model(raw, e, &t, aa, 1);
                struct setting st = {.id = V4L2_CID_RGA1_ROTATE_AA, .value = aa};
                uint8_t *a = job(node, &t, raw, NULL, &st, 1, NULL, NULL, false);
                char label[120];
                snprintf(label, sizeof(label), "affine DMA extreme %ux%u to %ux%u angle %u AA %u",
                         t.sw, t.sh, t.dw, t.dh, t.angle, aa);
                exact(label, a, e, dn, 1);
                free(a);
                free(raw);
                free(e);
            }
    struct test t = {.sw = 64,
                     .sh = 48,
                     .dw = 80,
                     .dh = 64,
                     .matrix = 601,
                     .input = V4L2_PIX_FMT_RGB24,
                     .output = V4L2_PIX_FMT_RGB24};
    struct v4l2_rect crop = {.left = 8, .top = 4, .width = 48, .height = 40},
                     compose = {.left = 7, .top = 5, .width = 61, .height = 49};
    uint8_t raw[9216], cropped[5760], rectangle[9016], e[15360];
    for (unsigned y = 0; y < 48; y++)
        for (unsigned x = 0; x < 64; x++) {
            raw[(y * 64 + x) * 3] = x * 3;
            raw[(y * 64 + x) * 3 + 1] = y * 3;
            raw[(y * 64 + x) * 3 + 2] = 77;
        }
    for (unsigned y = 0; y < 40; y++)
        memcpy(cropped + y * 144, raw + (y + 4) * 192 + 24, 144);
    const unsigned selected[] = {1, 30, 45, 89, 91, 123, 179, 181, 225, 269, 271, 315, 359};
    for (unsigned j = 0; j < 13; j++)
        for (unsigned flip = 0; flip < 4; flip++)
            for (unsigned aa = 0; aa < 2; aa++) {
                t.angle = selected[j];
                t.flip = flip;
                struct test ref = t;
                ref.sw = 48;
                ref.sh = 40;
                ref.dw = 61;
                ref.dh = 49;
                affine_model(cropped, rectangle, &ref, aa, 1);
                memset(e, 0xa5, sizeof(e));
                for (unsigned y = 0; y < 49; y++)
                    memcpy(e + (y + 5) * 240 + 21, rectangle + y * 184, 183);
                struct setting st = {.id = V4L2_CID_RGA1_ROTATE_AA, .value = aa};
                uint8_t *a = job(node, &t, raw, NULL, &st, 1, &crop, &compose, false);
                char label[120];
                snprintf(label, sizeof(label),
                         "cropped affine translated odd COMPOSE angle %u reflection %u AA %u",
                         t.angle, flip, aa);
                exact(label, a, e, sizeof(e), 1);
                free(a);
            }
}
/* The canvas-clear pass is a packed RGB fill. Refusal must happen at the
 * first STREAMON, before either the clear or affine DMA can be submitted. */
static void arbitrary_yuv_refusals(const char *node)
{
    const uint32_t formats[] = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV21, V4L2_PIX_FMT_NV16,
                                V4L2_PIX_FMT_NV61};
    const unsigned angles[] = {45, 89, 315};
    for (unsigned f = 0; f < 4; f++)
        for (unsigned angle = 0; angle < 3; angle++) {
            struct test t = {.sw = 2048,
                             .sh = 2,
                             .dw = 2048,
                             .dh = 2,
                             .matrix = 601,
                             .input = V4L2_PIX_FMT_RGB24,
                             .output = formats[f],
                             .angle = angles[angle]};
            rejected_stream(node, t, false);
            printf("{\"proof\":\"arbitrary RGB24 to %.4s 2048x2 angle %u refused before "
                   "DMA\",\"pass\":true}\n",
                   (char *)&t.output, t.angle);
        }
}
static void palette_affine_bounds(const char *node)
{
    const uint32_t formats[] = {V4L2_PIX_FMT_RGA1_PAL1, V4L2_PIX_FMT_RGA1_PAL2,
                                V4L2_PIX_FMT_RGA1_PAL4};
    const unsigned shapes[][4] = {
        {2048, 4, 2048, 4}, {4, 2048, 4, 2048}, {2048, 16, 1024, 8}, {4, 2048, 4, 1024}};
    const unsigned angles[] = {89, 271, 45, 315};
    uint8_t lut[1024];
    for (unsigned i = 0; i < 256; i++) {
        lut[4 * i] = 32 + (i & 15) * 4;
        lut[4 * i + 1] = 64 + (i & 15) * 4;
        lut[4 * i + 2] = 96 + (i & 15) * 4;
        lut[4 * i + 3] = 255;
    }
    for (unsigned f = 0; f < 3; f++)
        for (unsigned shape = 0; shape < 4; shape++)
            for (unsigned angle = 0; angle < 4; angle++)
                for (unsigned flip = 0; flip < 2; flip++)
                    for (unsigned aa = 0; aa < 2; aa++) {
                        struct test t = {.sw = shapes[shape][0],
                                         .sh = shapes[shape][1],
                                         .dw = shapes[shape][2],
                                         .dh = shapes[shape][3],
                                         .matrix = 601,
                                         .input = formats[f],
                                         .output = V4L2_PIX_FMT_RGB24,
                                         .angle = angles[angle],
                                         .flip = flip ? 3 : 0};
                        unsigned bits = palette_bits(t.input), ss = line_bytes(t.input, t.sw),
                                 rs = line_bytes(V4L2_PIX_FMT_RGB24, t.sw);
                        unsigned sn = sizeimage(t.input, t.sw, t.sh),
                                 rn = sizeimage(V4L2_PIX_FMT_RGB24, t.sw, t.sh),
                                 dn = sizeimage(t.output, t.dw, t.dh);
                        uint8_t *raw = calloc(1, sn), *rgb = calloc(1, rn), *e = malloc(dn);
                        if (!raw || !rgb || !e)
                            die("palette affine allocation");
                        for (unsigned y = 0; y < t.sh; y++)
                            for (unsigned x = 0; x < t.sw; x++) {
                                unsigned index = (x / 8 + y / 8) & ((1U << bits) - 1);
                                raw[y * ss + x * bits / 8] |= index << (8 - bits - (x * bits % 8));
                                for (unsigned c = 0; c < 3; c++)
                                    rgb[y * rs + x * 3 + c] =
                                        bits == 1 ? (index ? 96 : 32) + c * 32 : lut[4 * index + c];
                            }
                        struct test ref = t;
                        ref.input = V4L2_PIX_FMT_RGB24;
                        /* Average precedes the affine pass without exchanging axes.
                         * prescale() also serves quarter turns; angle zero selects
                         * the unrotated average geometry needed at these angles. */
                        ref.angle = 0;
                        uint8_t *mid = prescale(rgb, &ref);
                        ref.angle = t.angle;
                        affine_model(mid, e, &ref, aa, 1);
                        struct setting settings[] = {
                            {.id = V4L2_CID_RGA1_LUT, .data = lut, .size = sizeof(lut)},
                            {.id = V4L2_CID_BG_COLOR, .value = 0x204060},
                            {.id = V4L2_CID_RGA1_FG_COLOR, .value = 0x6080a0},
                            {.id = V4L2_CID_RGA1_ROTATE_AA, .value = aa}};
                        uint8_t *a = job(node, &t, raw, NULL, settings, 4, NULL, NULL, false);
                        char label[150];
                        snprintf(label, sizeof(label),
                                 "expanded PAL%u affine %ux%u to %ux%u angle %u reflection %u AA "
                                 "%u bounds",
                                 bits, t.sw, t.sh, t.dw, t.dh, t.angle, t.flip, aa);
                        exact(label, a, e, dn, 1);
                        free(a);
                        free(mid);
                        free(raw);
                        free(rgb);
                        free(e);
                    }
}
static void arbitrary_suite(const char *node)
{
    const unsigned angles[] = {1, 15, 30, 45, 75, 89, 91, 135, 179, 181, 225, 269, 271, 315, 359};
    const uint32_t fmts[] = {V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_BGR24, V4L2_PIX_FMT_RGB565};
    for (unsigned fmt = 0; fmt < 6; fmt++)
        for (unsigned i = 0; i < 15; i++)
            for (unsigned aa = fmt == 3 ? 1 : 0; aa < 2; aa++)
                for (unsigned flip = 0; flip < 4; flip++) {
                    struct test t = {.sw = 64,
                                     .sh = 64,
                                     .dw = 64,
                                     .dh = 64,
                                     .matrix = 601,
                                     .input = fmts[fmt < 3 ? fmt : 0],
                                     .output = V4L2_PIX_FMT_RGB24,
                                     .angle = angles[i],
                                     .flip = flip};
                    uint8_t raw[16384], e[12288];
                    for (unsigned y = 0; y < 64; y++)
                        for (unsigned x = 0; x < 64; x++) {
                            double rgb[3] = {fmt == 3 ? 255 : x * 3, fmt == 3 ? 255 : y * 3,
                                             fmt == 3 ? 255 : 77};
                            rgb_store(raw + y * line_bytes(t.input, 64) + x * bpp(t.input), t.input,
                                      rgb);
                        }
                    unsigned sampling = fmt == 4 ? 0 : fmt == 5 ? 2 : 1;
                    affine_model(raw, e, &t, aa, sampling);
                    struct setting st[] = {{.id = V4L2_CID_RGA1_ROTATE_AA, .value = aa},
                                           {.id = V4L2_CID_RGA1_SAMPLING, .value = sampling}};
                    uint8_t *a = job(node, &t, raw, NULL, st, 2, NULL, NULL, false);
                    char label[110];
                    snprintf(
                        label, sizeof(label),
                        "affine %.4s %s angle %u reflection %u AA %u sampling %u black clipping",
                        (char *)&t.input, fmt == 3 ? "white" : "ramp", t.angle, flip, aa, sampling);
                    exact(label, a, e, sizeof(e), 1);
                    free(a);
                }
    arbitrary_bounds(node);
    arbitrary_yuv_refusals(node);
}
static void palette_transforms(const char *node)
{
    uint8_t lut[1024], raw[65536], rgba[262144], e[262144];
    for (unsigned i = 0; i < 256; i++) {
        lut[4 * i] = i * 7;
        lut[4 * i + 1] = i * 11;
        lut[4 * i + 2] = i * 13;
        lut[4 * i + 3] = 255 - i * 3;
    }
    const uint32_t fmts[] = {V4L2_PIX_FMT_RGA1_PAL2, V4L2_PIX_FMT_RGA1_PAL4};
    for (unsigned f = 0; f < 2; f++)
        for (unsigned pre = 0; pre < 2; pre++)
            for (unsigned angle = 0; angle < 360; angle += 90)
                for (unsigned flip = 0; flip < 4; flip++) {
                    struct test t = {.sw = pre ? 256 : 64,
                                     .sh = pre ? 192 : 48,
                                     .dw = 96,
                                     .dh = 64,
                                     .matrix = 601,
                                     .input = fmts[f],
                                     .output = V4L2_PIX_FMT_RGB24,
                                     .angle = angle,
                                     .flip = flip};
                    unsigned bits = palette_bits(t.input), ss = line_bytes(t.input, t.sw),
                             rs = line_bytes(V4L2_PIX_FMT_RGBA32, t.sw);
                    memset(raw, 0, sizeof(raw));
                    for (unsigned y = 0; y < t.sh; y++)
                        for (unsigned x = 0; x < t.sw; x++) {
                            unsigned index = (x / 8 + y / 8) % (1U << bits);
                            raw[y * ss + x * bits / 8] |= index << (8 - bits - (x * bits % 8));
                            memcpy(rgba + y * rs + x * 4, lut + index * 4, 4);
                        }
                    struct test ref = t;
                    ref.input = V4L2_PIX_FMT_RGBA32;
                    memset(e, 0xa5, sizeof(e));
                    uint8_t *mid = prescale(rgba, &ref);
                    affine_model(mid, e, &ref, 0, 1);
                    free(mid);
                    struct setting st = {.id = V4L2_CID_RGA1_LUT, .data = lut, .size = 1024};
                    uint8_t *a = job(node, &t, raw, NULL, &st, 1, NULL, NULL, false);
                    char label[110];
                    snprintf(label, sizeof(label),
                             "palette %u-bit hardware expansion%s rotation %u reflection %u", bits,
                             pre ? " + prescale" : "", angle, flip);
                    exact(label, a, e, sizeimage(t.output, t.dw, t.dh), 1);
                    free(a);
                }
    /* Palette alpha blending follows expansion, so it has the same independently
     * modelled RGBA arithmetic as ordinary blits. */
    struct test t = {.sw = 64,
                     .sh = 48,
                     .dw = 64,
                     .dh = 48,
                     .matrix = 601,
                     .input = V4L2_PIX_FMT_RGA1_PAL4,
                     .output = V4L2_PIX_FMT_RGBA32};
    uint8_t bg[12288];
    for (unsigned i = 0; i < 3072; i++) {
        unsigned idx = i & 15;
        raw[i / 2] = (i & 1) ? (raw[i / 2] & 0xf0) | idx : idx << 4;
        memcpy(rgba + i * 4, lut + idx * 4, 4);
        bg[4 * i] = 32;
        bg[4 * i + 1] = 64;
        bg[4 * i + 2] = 96;
        bg[4 * i + 3] = 191;
        unsigned ca = coefficient(lut[4 * idx + 3], 0);
        for (unsigned c = 0; c < 3; c++)
            e[4 * i + c] = (lut[4 * idx + c] * ca + bg[4 * i + c] * (256 - ca)) >> 8;
        e[4 * i + 3] = lut[4 * idx + 3];
    }
    struct setting ss[] = {{.id = V4L2_CID_RGA1_LUT, .data = lut, .size = 1024},
                           {.id = V4L2_CID_RGA1_ALPHA_MODE, .value = 2}};
    uint8_t *a = job(node, &t, raw, bg, ss, 2, NULL, NULL, false);
    exact("palette LUT alpha through hardware expansion and RGBA mix", a, e, sizeof(bg), 0);
    free(a);
    palette_affine_bounds(node);
}
/* Native fill's RGB565 formatter normalizes against 31/63 before its Bayer
 * thresholds. Blit conversion instead truncates to zero-fill 5/6-bit values. */
static uint16_t fill_dither_pixel(const unsigned rgb[3], unsigned x, unsigned y)
{
    static const unsigned bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    unsigned d = bayer[(y & 3) * 4 + (x & 3)];
    unsigned r = (rgb[0] * 31 + 31 + (d >> 1) * 32) >> 8;
    unsigned g = (rgb[1] * 63 + 63 + (d >> 2) * 64) >> 8;
    unsigned b = (rgb[2] * 31 + 31 + (d >> 1) * 32) >> 8;
    return (r << 11) | (g << 5) | b;
}
static void fill_combinations(const char *node)
{
    struct test t = {.sw = 64,
                     .sh = 64,
                     .dw = 64,
                     .dh = 64,
                     .matrix = 601,
                     .input = V4L2_PIX_FMT_RGB24,
                     .output = V4L2_PIX_FMT_RGBA32};
    uint8_t raw[12288] = {0}, bg[16384], e[16384];
    int32_t gr[8] = {1024, -256, 0, 0, 0, 0, 0, 0};
    for (unsigned i = 0; i < 4096; i++) {
        bg[4 * i] = 32;
        bg[4 * i + 1] = 64;
        bg[4 * i + 2] = 96;
        bg[4 * i + 3] = (i / 64) * 4;
    }
    for (unsigned mode = 1; mode <= 3; mode++)
        for (unsigned pd = 0; pd <= 12; pd++)
            for (unsigned calc = 0; calc < 2; calc++) {
                for (unsigned y = 0; y < 64; y++)
                    for (unsigned x = 0; x < 64; x++) {
                        unsigned idx = y * 64 + x,
                                 stored =
                                     clip16(clip16(128 * 256 - (int)y * 256) + (int)x * 1024) >> 8,
                                 sa = mode == 1   ? 128
                                      : mode == 3 ? (stored * 128) >> 8
                                                  : stored,
                                 da = bg[4 * idx + 3], fs, fd;
                        pd_factors(pd, coefficient(sa, calc), coefficient(da, calc), &fs, &fd);
                        const unsigned color[4] = {34, 102, 170, sa};
                        for (unsigned c = 0; c < 4; c++)
                            e[4 * idx + c] =
                                c == 3 && !pd ? stored
                                              : sat8((color[c] * fs + bg[4 * idx + c] * fd) >> 8);
                    }
                struct setting ss[] = {
                    {.id = V4L2_CID_RGA1_OPERATION, .value = 1},
                    {.id = V4L2_CID_BG_COLOR, .value = 0x2266aa},
                    {.id = V4L2_CID_ALPHA_COMPONENT, .value = 128},
                    {.id = V4L2_CID_RGA1_GRADIENT, .data = gr, .size = sizeof(gr)},
                    {.id = V4L2_CID_RGA1_ALPHA_MODE, .value = mode},
                    {.id = V4L2_CID_RGA1_PORTER_DUFF, .value = pd},
                    {.id = V4L2_CID_RGA1_ALPHA_CALC, .value = calc}};
                uint8_t *a = job(node, &t, raw, bg, ss, 7, NULL, NULL, false);
                char label[100];
                snprintf(label, sizeof(label), "gradient fill alpha mode %u PD %u straight %u",
                         mode, pd, calc);
                exact(label, a, e, sizeof(e), 0);
                free(a);
            }
    t.output = V4L2_PIX_FMT_RGB24;
    memset(bg, 0x64, 12288);
    for (unsigned code = 0; code < 16; code++) {
        for (unsigned i = 0; i < 12288; i++) {
            unsigned p = i % 3 == 0 ? 34 : i % 3 == 1 ? 102 : 170, r = 0;
            for (unsigned b = 0; b < 8; b++)
                r |= ((code >> ((((p >> b) & 1) << 1) | ((bg[i] >> b) & 1))) & 1) << b;
            e[i] = r;
        }
        struct setting ss[] = {{.id = V4L2_CID_RGA1_OPERATION, .value = 1},
                               {.id = V4L2_CID_BG_COLOR, .value = 0x2266aa},
                               {.id = V4L2_CID_RGA1_ROP_MODE, .value = 0},
                               {.id = V4L2_CID_RGA1_ROP, .value = code + 1}};
        uint8_t *a = job(node, &t, raw, bg, ss, 4, NULL, NULL, false);
        char label[80];
        snprintf(label, sizeof(label), "solid fill ROP2 pen truth table %x", code);
        exact(label, a, e, 12288, 0);
        free(a);
    }
    t.output = V4L2_PIX_FMT_RGB565;
    unsigned errors = 0;
    for (unsigned value = 0; value < 256; value++) {
        struct setting ss[] = {
            {.id = V4L2_CID_RGA1_OPERATION, .value = 1},
            {.id = V4L2_CID_BG_COLOR, .value = value | (value << 8) | (value << 16)},
            {.id = V4L2_CID_RGA1_DITHER, .value = 1}};
        uint8_t *a = job(node, &t, raw, NULL, ss, 3, NULL, NULL, false);
        for (unsigned y = 0; y < 64; y++)
            for (unsigned x = 0; x < 64; x++) {
                const unsigned colors[3] = {value, value, value};
                uint16_t pixel = fill_dither_pixel(colors, x, y);
                errors += a[(y * 64 + x) * 2] != (pixel & 255);
                errors += a[(y * 64 + x) * 2 + 1] != (pixel >> 8);
            }
        free(a);
    }
    printf("{\"proof\":\"native RGB565 fill dither all 256 levels, independent normalized Bayer "
           "model\",\"pixels\":1048576,\"errors\":%u,\"pass\":%s}\n",
           errors, errors ? "false" : "true");
    if (errors)
        failures++;
    int32_t gradients[8] = {0, 0, -769, 255, 255, -769, 769, -255};
    for (unsigned wrap = 0; wrap < 2; wrap++) {
        for (unsigned y = 0; y < 64; y++)
            for (unsigned x = 0; x < 64; x++) {
                const unsigned initial[3] = {34, 102, 170};
                unsigned colors[3];
                for (unsigned c = 0; c < 3; c++) {
                    unsigned index = 6 - 2 * c;
                    int row = initial[c] * 256 + (int)y * gradients[index + 1];
                    int v = wrap ? row + (int)x * gradients[index]
                                 : clip16(clip16(row) + (int)x * gradients[index]);
                    colors[c] = ((unsigned)v & 65535) >> 8;
                }
                uint16_t pixel = fill_dither_pixel(colors, x, y);
                e[(y * 64 + x) * 2] = pixel;
                e[(y * 64 + x) * 2 + 1] = pixel >> 8;
            }
        struct setting ss[] = {
            {.id = V4L2_CID_RGA1_OPERATION, .value = 1},
            {.id = V4L2_CID_BG_COLOR, .value = 0x2266aa},
            {.id = V4L2_CID_RGA1_DITHER, .value = 1},
            {.id = V4L2_CID_RGA1_GRADIENT, .data = gradients, .size = sizeof(gradients)},
            {.id = V4L2_CID_RGA1_GRADIENT_WRAP, .value = wrap}};
        uint8_t *a = job(node, &t, raw, NULL, ss, 5, NULL, NULL, false);
        exact(wrap ? "native wrapping gradient plus Bayer dither"
                   : "native clipped gradient plus Bayer dither",
              a, e, 8192, 0);
        free(a);
    }
    const struct v4l2_rect rectangles[] = {{.left = 63, .top = 63, .width = 1, .height = 1},
                                           {.left = 7, .top = 5, .width = 3, .height = 5}};
    for (unsigned r = 0; r < 2; r++) {
        struct setting ss[] = {{.id = V4L2_CID_RGA1_OPERATION, .value = 1},
                               {.id = V4L2_CID_BG_COLOR, .value = 0x2266aa},
                               {.id = V4L2_CID_RGA1_DITHER, .value = 1}};
        const struct v4l2_rect *box = &rectangles[r];
        memset(e, 0xa5, 8192);
        for (unsigned y = 0; y < (unsigned)box->height; y++)
            for (unsigned x = 0; x < (unsigned)box->width; x++) {
                const unsigned colors[3] = {34, 102, 170};
                uint16_t pixel = fill_dither_pixel(colors, x, y);
                unsigned offset = ((y + box->top) * 64 + x + box->left) * 2;
                e[offset] = pixel;
                e[offset + 1] = pixel >> 8;
            }
        uint8_t *a = job(node, &t, raw, NULL, ss, 3, NULL, box, false);
        char label[100];
        snprintf(label, sizeof(label), "native RGB565 dither %ux%u COMPOSE, exact border bytes",
                 box->width, box->height);
        exact(label, a, e, 8192, 0);
        free(a);
    }
}
static void selftest_rest(void)
{
    for (unsigned p = 0; p < 64; p++) {
        int w[4];
        weights(p, 2, w);
        assert(w[0] + w[1] + w[2] + w[3] == 64);
    }
    unsigned fs, fd;
    pd_factors(12, 256, 256, &fs, &fd);
    assert(!fs && !fd);
    puts("RGA1 rest reference models: cubic normalization and coefficient endpoints PASS");
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--selftest")) {
        selftest_rest();
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr,
                "usage: %s NODE "
                "--alpha-grid|--gradient-wrap|--rop-masks|--phase|--sampling|--mplane|--prescale|--"
                "arbitrary|--palette-transforms|--fill-combinations\n",
                argv[0]);
        return 64;
    }
    if (!strcmp(argv[2], "--alpha-grid"))
        alpha_grid(argv[1]);
    else if (!strcmp(argv[2], "--gradient-wrap"))
        gradient_wrap(argv[1]);
    else if (!strcmp(argv[2], "--rop-masks"))
        rop_masks(argv[1]);
    else if (!strcmp(argv[2], "--phase"))
        pattern_phase(argv[1]);
    else if (!strcmp(argv[2], "--sampling"))
        sampling_nonconstant(argv[1]);
    else if (!strcmp(argv[2], "--mplane"))
        multiplane_suite(argv[1]);
    else if (!strcmp(argv[2], "--prescale"))
        standalone_prescale(argv[1]);
    else if (!strcmp(argv[2], "--arbitrary"))
        arbitrary_suite(argv[1]);
    else if (!strcmp(argv[2], "--palette-transforms"))
        palette_transforms(argv[1]);
    else if (!strcmp(argv[2], "--fill-combinations"))
        fill_combinations(argv[1]);
    else
        return 64;
    return failures ? 1 : 0;
}
