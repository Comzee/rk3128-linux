// Hardware JPEG decode on the RK3128's VPU (development record #395): the header check, the V4L2 job on Hantro's
// decoder device and the stb_image-exact YCbCr to RGB conversion. rk3128_jpeg.h has the contract.
#include "rk3128_jpeg.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// The datasheet's range (RK3128-SOC.md), and what we let one photo take of the shared 160 MiB CMA pool (the decoded
// planes): 48 MiB is a 32 MP 4:2:0 photo; bigger ones go to the CPU, as do files over the 16 MiB stream register.
#define HW_MIN_SIDE 48
#define HW_MAX_SIDE 8176
#define HW_MAX_PLANES_BYTES (48u << 20)
#define HW_MAX_STREAM ((16u << 20) - 64)
#define HW_TIMEOUT_MS 3000

static void say(char *why, size_t n, const char *fmt, ...) {
    if (!why || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ---- headers ----

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }

int rk3128_jpeg_parse(const unsigned char *p, size_t n, Rk3128JpegInfo *info, char *why, size_t whylen) {
    memset(info, 0, sizeof(*info));
    if (n < 4 || p[0] != 0xff || p[1] != 0xd8) {
        say(why, whylen, "not a JPEG");
        return -1;
    }
    int jfif = 0, adobe_tf = -1, rgb_ids = 0, have_sof = 0;
    unsigned char ids[3] = {0};
    size_t i = 2;
    while (i + 4 <= n) {
        if (p[i] != 0xff) {
            say(why, whylen, "corrupt marker at %zu", i);
            return -1;
        }
        unsigned m = p[i + 1];
        if (m == 0xff) {  // fill byte
            i++;
            continue;
        }
        if (m == 0xd8 || m == 0x01 || (m >= 0xd0 && m <= 0xd7)) {
            i += 2;
            continue;
        }
        if (m == 0xd9) break;
        unsigned len = be16(p + i + 2);
        if (len < 2 || i + 2 + len > n) {
            say(why, whylen, "truncated segment 0x%02x", m);
            return -1;
        }
        const unsigned char *s = p + i + 4;  // payload, len - 2 bytes
        unsigned sl = len - 2;
        if (m == 0xc0 || m == 0xc1) {
            if (sl < 6 || s[0] != 8) {
                say(why, whylen, "%u-bit samples", sl ? s[0] : 0);
                return -1;
            }
            info->h = (int)be16(s + 1);
            info->w = (int)be16(s + 3);
            info->ncomp = s[5];
            if ((info->ncomp != 1 && info->ncomp != 3) || sl != 6 + 3u * info->ncomp) {
                say(why, whylen, "%d components", info->ncomp);
                return -1;
            }
            for (int c = 0; c < info->ncomp; c++) {
                int hv = s[7 + 3 * c];
                if (c == 0) {
                    info->hs = hv >> 4;
                    info->vs = hv & 15;
                } else if (hv != 0x11) {
                    say(why, whylen, "chroma sampling %dx%d", hv >> 4, hv & 15);
                    return -1;
                }
                ids[c] = s[6 + 3 * c];
            }
            have_sof = 1;
        } else if ((m >= 0xc2 && m <= 0xcf) && m != 0xc4 && m != 0xc8 && m != 0xcc) {
            say(why, whylen, m == 0xc2 ? "progressive" : "SOF%u", m - 0xc0);
            return -1;
        } else if (m == 0xdb) {
            for (unsigned k = 0; k < sl;) {
                if (s[k] >> 4) {
                    say(why, whylen, "16-bit quantisation table");
                    return -1;
                }
                k += 65;
            }
        } else if (m == 0xe0 && sl >= 5 && !memcmp(s, "JFIF", 5)) {
            jfif = 1;
        } else if (m == 0xee && sl >= 12 && !memcmp(s, "Adobe", 6)) {
            adobe_tf = s[11];
        } else if (m == 0xdd && sl >= 2) {
            info->restart = (int)be16(s);
        } else if (m == 0xda) {
            if (!have_sof) {
                say(why, whylen, "scan before the frame header");
                return -1;
            }
            if (sl < 1 || s[0] != info->ncomp) {
                say(why, whylen, "a scan of %d of %d components", sl ? s[0] : 0, info->ncomp);
                return -1;
            }
            info->ecs = i + 2 + len;
            break;
        }
        i += 2 + len;
    }
    if (!info->ecs) {
        say(why, whylen, "no scan");
        return -1;
    }
    if (info->ncomp == 3) {
        rgb_ids = ids[0] == 'R' && ids[1] == 'G' && ids[2] == 'B';
        // stb_image's own rule for RGB-coded JPEGs: no colour conversion, which the hardware path would apply
        if (rgb_ids || (adobe_tf == 0 && !jfif)) {
            say(why, whylen, "RGB-coded JPEG");
            return -1;
        }
        // the kernel's G1 decoder (patch 395-0001, ST's port) writes 4:2:0 as NV12 and 4:2:2 as NV16, nothing else
        if (!((info->hs == 2 && info->vs == 2) || (info->hs == 2 && info->vs == 1))) {
            say(why, whylen, info->hs == 1 && info->vs == 1 ? "4:4:4" : "luma sampling %dx%d", info->hs, info->vs);
            return -1;
        }
    } else {
        say(why, whylen, "greyscale");
        return -1;
    }
    if (info->w < HW_MIN_SIDE || info->h < HW_MIN_SIDE || info->w > HW_MAX_SIDE || info->h > HW_MAX_SIDE) {
        say(why, whylen, "%dx%d outside %d..%d", info->w, info->h, HW_MIN_SIDE, HW_MAX_SIDE);
        return -1;
    }
    return 0;
}

// ---- the conversion: stb_image v2.30's resampling and YCbCr_to_RGB_row, scalar, bit for bit ----

#define DIV4(x) ((uint8_t)((x) >> 2))
#define DIV16(x) ((uint8_t)((x) >> 4))

static void up_h2(uint8_t *out, const uint8_t *in, int w) {
    if (w == 1) {
        out[0] = out[1] = in[0];
        return;
    }
    int i;
    out[0] = in[0];
    out[1] = DIV4(in[0] * 3 + in[1] + 2);
    for (i = 1; i < w - 1; ++i) {
        int n = 3 * in[i] + 2;
        out[i * 2 + 0] = DIV4(n + in[i - 1]);
        out[i * 2 + 1] = DIV4(n + in[i + 1]);
    }
    out[i * 2 + 0] = DIV4(in[w - 2] * 3 + in[w - 1] + 2);
    out[i * 2 + 1] = in[w - 1];
}

static void up_hv2(uint8_t *out, const uint8_t *near, const uint8_t *far, int w) {
    if (w == 1) {
        out[0] = out[1] = DIV4(3 * near[0] + far[0] + 2);
        return;
    }
    int t0, t1 = 3 * near[0] + far[0];
    out[0] = DIV4(t1 + 2);
    for (int i = 1; i < w; ++i) {
        t0 = t1;
        t1 = 3 * near[i] + far[i];
        out[i * 2 - 1] = DIV16(3 * t0 + t1 + 8);
        out[i * 2] = DIV16(3 * t1 + t0 + 8);
    }
    out[w * 2 - 1] = DIV4(t1 + 2);
}

#define F2F(x) (((int)((x) * 4096.0f + 0.5f)) << 8)

static void ycc_row(uint8_t *out, const uint8_t *y, const uint8_t *pcb, const uint8_t *pcr, int count) {
    for (int i = 0; i < count; ++i) {
        int y_fixed = (y[i] << 20) + (1 << 19);
        int cr = pcr[i] - 128, cb = pcb[i] - 128;
        int r = y_fixed + cr * F2F(1.40200f);
        int g = y_fixed + (cr * -F2F(0.71414f)) + ((cb * -F2F(0.34414f)) & 0xffff0000);
        int b = y_fixed + cb * F2F(1.77200f);
        r >>= 20, g >>= 20, b >>= 20;
        if ((unsigned)r > 255) r = r < 0 ? 0 : 255;
        if ((unsigned)g > 255) g = g < 0 ? 0 : 255;
        if ((unsigned)b > 255) b = b < 0 ? 0 : 255;
        out[0] = (uint8_t)r, out[1] = (uint8_t)g, out[2] = (uint8_t)b;
        out += 3;
    }
}

typedef struct {
    const Rk3128JpegPlanes *pl;
    uint8_t *rgb;
    uint8_t *tmp;  // this band's row buffers (band_bytes), allocated by the caller: a band cannot fail
    int y0, y1;
} Band;

// The planes may be the VPU's buffer itself, which user space maps uncached (a coherent DMA buffer on this SoC): every
// row is read once, whole, with memcpy (bursts) into a cached row buffer, never sample by sample. A chroma row serves two
// output rows in 4:2:0, so the last two are kept.
typedef struct {
    int idx;
    uint8_t *cb, *cr;
} CRow;

static const CRow *chroma_row(const Rk3128JpegPlanes *pl, CRow slot[2], int idx, int keep, uint8_t *raw, int cw) {
    if (slot[0].idx == idx) return &slot[0];
    if (slot[1].idx == idx) return &slot[1];
    CRow *r = slot[0].idx == keep ? &slot[1] : &slot[0];
    memcpy(raw, pl->c + (size_t)idx * pl->cstride, (size_t)cw * 2);
    for (int i = 0; i < cw; i++) r->cb[i] = raw[2 * i], r->cr[i] = raw[2 * i + 1];
    r->idx = idx;
    return r;
}

static size_t band_bytes(const Rk3128JpegPlanes *pl) {
    int hs = pl->ncomp == 3 ? pl->hs : 1;
    size_t w = (size_t)pl->w, cw = (size_t)((pl->w + hs - 1) / hs);
    return (w + cw * 6 + w * 2 + 16 + 63) & ~(size_t)63;  // a cache line apart, so two bands never share one
}

static void *band_run(void *arg) {
    const Band *b = arg;
    const Rk3128JpegPlanes *pl = b->pl;
    int w = pl->w;
    int hs = pl->ncomp == 3 ? pl->hs : 1, vs = pl->ncomp == 3 ? pl->vs : 1;
    int cw = (w + hs - 1) / hs, chh = (pl->h + vs - 1) / vs;  // stb's w_lores and the chroma component's rows
    uint8_t *yrow = b->tmp, *raw = yrow + w, *cb = raw + cw * 2, *cr = cb + w + 4;
    CRow slot[2] = {{-1, cr + w + 4, cr + w + 4 + cw}, {-1, cr + w + 4 + 2 * cw, cr + w + 4 + 3 * cw}};
    for (int j = b->y0; j < b->y1; j++) {
        uint8_t *o = b->rgb + (size_t)j * w * 3;
        memcpy(yrow, pl->y + (size_t)j * pl->ystride, (size_t)w);
        if (pl->ncomp == 1) {
            for (int i = 0; i < w; i++, o += 3) o[0] = o[1] = o[2] = yrow[i];
            continue;
        }
        // the chroma rows stb's resampler pairs with output row j (its line0/line1 walk, in closed form)
        int near, far;
        if (vs == 2) {
            int k = j >> 1;
            near = k;
            far = (j & 1) ? k + 1 : (k ? k - 1 : 0);
        } else {
            near = far = j;
        }
        if (near > chh - 1) near = chh - 1;
        if (far > chh - 1) far = chh - 1;
        const CRow *n = chroma_row(pl, slot, near, far, raw, cw);
        const CRow *f = chroma_row(pl, slot, far, near, raw, cw);
        const uint8_t *pcb = n->cb, *pcr = n->cr;
        if (hs == 2 && vs == 2) {
            up_hv2(cb, n->cb, f->cb, cw);
            up_hv2(cr, n->cr, f->cr, cw);
            pcb = cb, pcr = cr;
        } else if (hs == 2) {
            up_h2(cb, n->cb, cw);
            up_h2(cr, n->cr, cw);
            pcb = cb, pcr = cr;
        }
        ycc_row(o, yrow, pcb, pcr, w);
    }
    return NULL;
}

uint8_t *rk3128_jpeg_to_rgb(const Rk3128JpegPlanes *pl, int threads) {
    if (threads < 1) threads = 1;
    if (threads > 8) threads = 8;
    if (pl->w < 1 || pl->h < 1) return NULL;
    // Everything the conversion needs is allocated here, before any thread starts, so a failure is all or nothing: NULL,
    // and never a buffer with rows nobody wrote.
    size_t bb = band_bytes(pl);
    uint8_t *rgb = malloc((size_t)pl->w * pl->h * 3);
    uint8_t *tmp = malloc(bb * (size_t)threads);
    if (!rgb || !tmp) {
        free(rgb);
        free(tmp);
        return NULL;
    }
    Band bands[8];
    pthread_t th[8];
    int started[8] = {0};
    int per = (pl->h + threads - 1) / threads;
    if (pl->vs == 2) per = (per + 1) & ~1;
    for (int t = 0; t < threads; t++) {
        bands[t].pl = pl, bands[t].rgb = rgb, bands[t].tmp = tmp + bb * (size_t)t;
        bands[t].y0 = t * per < pl->h ? t * per : pl->h;
        bands[t].y1 = (t + 1) * per < pl->h ? (t + 1) * per : pl->h;
    }
    for (int t = 1; t < threads; t++)
        if (bands[t].y0 < bands[t].y1) started[t] = !pthread_create(&th[t], NULL, band_run, &bands[t]);
    band_run(&bands[0]);
    for (int t = 1; t < threads; t++) {
        if (started[t])
            pthread_join(th[t], NULL);  // cannot fail: a joinable thread of ours, joined once
        else if (bands[t].y0 < bands[t].y1)
            band_run(&bands[t]);  // no thread: do it here
    }
    free(tmp);
    return rgb;
}

// ---- the V4L2 job ----

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

// Hantro's decoder device: the hantro-vpu node whose OUTPUT queue takes JPEG. Looked up once.
static int find_device(char *path, size_t n) {
    static char found[32];
    static int done;  // racy only in doing the lookup twice
    if (__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        if (!found[0]) return -1;
        snprintf(path, n, "%s", found);
        return 0;
    }
    char p[32];
    for (int i = 0; i < 16; i++) {
        snprintf(p, sizeof(p), "/dev/video%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct v4l2_capability cap = {0};
        int ok = 0;
        if (!xioctl(fd, VIDIOC_QUERYCAP, &cap) && !strcmp((char *)cap.driver, "hantro-vpu")) {
            for (unsigned k = 0; !ok; k++) {
                struct v4l2_fmtdesc d = {.index = k, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
                if (xioctl(fd, VIDIOC_ENUM_FMT, &d)) break;
                ok = d.pixelformat == V4L2_PIX_FMT_JPEG;
            }
        }
        close(fd);
        if (ok) {
            snprintf(found, sizeof(found), "%s", p);
            break;
        }
    }
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    if (!found[0]) return -1;
    snprintf(path, n, "%s", found);
    return 0;
}

// The HEVC decoder (rkvdec) shares the VPU's bus (GRF_SOC_CON1 bit 15, patch 301-0002): while it is runtime-active a
// video is playing, and a photo job would make the arbiter take the bus from it between two frames. Leave it alone.
static int hevc_busy(void) {
    DIR *d = opendir("/sys/class/video4linux");
    if (!d) return 0;
    struct dirent *e;
    int busy = 0;
    while (!busy && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[320], link[256];
        snprintf(p, sizeof(p), "/sys/class/video4linux/%s/device/driver", e->d_name);
        ssize_t l = readlink(p, link, sizeof(link) - 1);
        if (l <= 0) continue;
        link[l] = 0;
        const char *base = strrchr(link, '/');
        if (strcmp(base ? base + 1 : link, "rkvdec")) continue;
        snprintf(p, sizeof(p), "/sys/class/video4linux/%s/device/power/runtime_status", e->d_name);
        FILE *f = fopen(p, "re");
        char st[32] = {0};
        if (f) {
            if (fgets(st, sizeof(st), f)) busy = !strncmp(st, "active", 6) || !strncmp(st, "resuming", 8);
            fclose(f);
        }
    }
    closedir(d);
    return busy;
}

typedef struct {
    int fd;
    void *map[2];
    size_t maplen[2];
    int streaming;
} Job;

static void job_close(Job *j) {
    if (j->fd >= 0) {
        if (j->streaming) {
            int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            xioctl(j->fd, VIDIOC_STREAMOFF, &t);
            t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            xioctl(j->fd, VIDIOC_STREAMOFF, &t);
        }
        for (int q = 0; q < 2; q++)
            if (j->map[q] && j->map[q] != MAP_FAILED) munmap(j->map[q], j->maplen[q]);
        close(j->fd);  // frees the buffers (CMA) with the context
    }
}

// REQBUFS 1, QUERYBUF, mmap. Coherent buffers (the CMA pool; uncached in user space): a cached one
// (V4L2_MEMORY_FLAG_NON_COHERENT) would come from dma_alloc_pages, which takes no highmem page on 32-bit ARM, so not
// from the CMA pool, and fails over the buddy allocator's 4 MiB (patch 395-0001 offers no cache hints for that reason).
static int job_buffer(Job *j, int q, enum v4l2_buf_type type, char *why, size_t whylen) {
    struct v4l2_requestbuffers rb = {.count = 1, .type = type, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(j->fd, VIDIOC_REQBUFS, &rb) || rb.count < 1) {
        say(why, whylen, "REQBUFS %s: %s", q ? "capture" : "output", strerror(errno));
        return -1;
    }
    struct v4l2_plane pl[1] = {0};
    struct v4l2_buffer b = {.type = type, .memory = V4L2_MEMORY_MMAP, .index = 0, .m.planes = pl, .length = 1};
    if (xioctl(j->fd, VIDIOC_QUERYBUF, &b)) {
        say(why, whylen, "QUERYBUF: %s", strerror(errno));
        return -1;
    }
    j->maplen[q] = pl[0].length;
    j->map[q] = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, j->fd, pl[0].m.mem_offset);
    if (j->map[q] == MAP_FAILED) {
        j->map[q] = NULL;
        say(why, whylen, "mmap: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int job_queue(Job *j, enum v4l2_buf_type type, size_t used) {
    struct v4l2_plane pl[1] = {{.bytesused = (unsigned)used}};
    struct v4l2_buffer b = {.type = type, .memory = V4L2_MEMORY_MMAP, .index = 0, .m.planes = pl, .length = 1};
    return xioctl(j->fd, VIDIOC_QBUF, &b);
}

static int job_dequeue(Job *j, enum v4l2_buf_type type, unsigned *flags) {
    struct v4l2_plane pl[1] = {0};
    struct v4l2_buffer b = {.type = type, .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1};
    int r = xioctl(j->fd, VIDIOC_DQBUF, &b);
    if (!r && flags) *flags = b.flags;
    return r;
}

static int conv_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : n > 4 ? 4 : (int)n;
}

static unsigned char *decode(const unsigned char *jpg, size_t n, int *w, int *h, char *why, size_t whylen,
    Rk3128JpegTimes *times, Rk3128JpegPlanesCb planes_cb, void *cb_arg, Rk3128JpegConvertCb convert_cb, void *convert_arg) {
    Rk3128JpegTimes t = {0};
    if (times) *times = t;
    double t0 = now_ms(), tx = 0;  // tx: the test fill and the planes callback, left out of the times
    Rk3128JpegInfo in;
    if (rk3128_jpeg_parse(jpg, n, &in, why, whylen)) return NULL;
    int aw = (in.w + 15) & ~15, ah = (in.h + 15) & ~15;
    uint32_t fourcc = in.vs == 2 ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_NV16;
    size_t planes = (size_t)aw * ah * (fourcc == V4L2_PIX_FMT_NV12 ? 3 : 4) / 2;
    if (planes > HW_MAX_PLANES_BYTES) {
        say(why, whylen, "%dx%d needs %zu MiB of CMA", in.w, in.h, planes >> 20);
        return NULL;
    }
    // A run of 0xff fill bytes before the EOI upsets the decoder (MPP's jpegd_vdpu_tail_0xFF_patch): drop them.
    size_t len = n;
    while (len >= 2 && !(jpg[len - 2] == 0xff && jpg[len - 1] == 0xd9) && len > in.ecs) len--;  // trailing junk
    size_t keep = len;
    if (len >= 3 && jpg[len - 2] == 0xff && jpg[len - 1] == 0xd9) {
        keep = len - 2;
        while (keep > in.ecs && jpg[keep - 1] == 0xff) keep--;
    }
    // test hook (linux/tests/jpeg "fill" step): zero bytes after the EOI, counted in the stream
    size_t pad = getenv("RK3128_JPEG_TEST_PAD") ? (size_t)strtoul(getenv("RK3128_JPEG_TEST_PAD"), NULL, 10) : 0;
    if (pad > 4096) pad = 4096;
    if (keep + 2 + pad > HW_MAX_STREAM) {
        say(why, whylen, "%zu-byte file over the stream limit", n);
        return NULL;
    }
    if (hevc_busy()) {
        say(why, whylen, "the HEVC decoder is in use");
        return NULL;
    }
    char dev[32];
    if (find_device(dev, sizeof(dev))) {
        say(why, whylen, "no VPU JPEG decoder");
        return NULL;
    }
    Job j = {.fd = open(dev, O_RDWR | O_CLOEXEC | O_NONBLOCK)};
    unsigned char *rgb = NULL;
    if (j.fd < 0) {
        say(why, whylen, "%s: %s", dev, strerror(errno));
        return NULL;
    }
    struct v4l2_format fo = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
    fo.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
    fo.fmt.pix_mp.width = (unsigned)in.w, fo.fmt.pix_mp.height = (unsigned)in.h;
    fo.fmt.pix_mp.num_planes = 1;
    fo.fmt.pix_mp.plane_fmt[0].sizeimage = (unsigned)((keep + 2 + pad + 4095) & ~(size_t)4095);
    struct v4l2_format fc = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    fc.fmt.pix_mp.pixelformat = fourcc;
    fc.fmt.pix_mp.width = (unsigned)in.w, fc.fmt.pix_mp.height = (unsigned)in.h;
    if (xioctl(j.fd, VIDIOC_S_FMT, &fo) || fo.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_JPEG) {
        say(why, whylen, "S_FMT output: %s", strerror(errno));
        goto out;
    }
    if (xioctl(j.fd, VIDIOC_S_FMT, &fc) || fc.fmt.pix_mp.pixelformat != fourcc ||
        (int)fc.fmt.pix_mp.width != aw || (int)fc.fmt.pix_mp.height != ah) {
        say(why, whylen, "S_FMT capture: %s (%ux%u)", strerror(errno), fc.fmt.pix_mp.width, fc.fmt.pix_mp.height);
        goto out;
    }
    if (job_buffer(&j, 0, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, why, whylen) ||
        job_buffer(&j, 1, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, why, whylen))
        goto out;
    if (j.maplen[0] < keep + 2 + pad) {
        say(why, whylen, "output buffer %zu < %zu", j.maplen[0], keep + 2 + pad);
        goto out;
    }
    memcpy(j.map[0], jpg, keep);
    ((unsigned char *)j.map[0])[keep] = 0xff;
    ((unsigned char *)j.map[0])[keep + 1] = 0xd9;
    memset((unsigned char *)j.map[0] + keep + 2, 0, pad);
    t.setup = now_ms() - t0;
    if (getenv("RK3128_JPEG_TEST_FILL")) {  // test hook (linux/tests/jpeg): a sample the VPU never wrote stays 0x55
        double a = now_ms();
        memset(j.map[1], 0x55, j.maplen[1]);
        tx += now_ms() - a;
    }
    double t1 = now_ms();
    if (job_queue(&j, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0)) {
        say(why, whylen, "QBUF capture: %s", strerror(errno));
        goto out;
    }
    if (job_queue(&j, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, keep + 2 + pad)) {
        say(why, whylen, "the VPU refused the JPEG: %s", strerror(errno));
        goto out;
    }
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(j.fd, VIDIOC_STREAMON, &type)) {
        say(why, whylen, "STREAMON: %s", strerror(errno));
        goto out;
    }
    j.streaming = 1;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(j.fd, VIDIOC_STREAMON, &type)) {
        say(why, whylen, "STREAMON: %s", strerror(errno));
        goto out;
    }
    struct pollfd pfd = {.fd = j.fd, .events = POLLIN};
    unsigned flags = 0;
    for (;;) {
        int r = poll(&pfd, 1, HW_TIMEOUT_MS);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            say(why, whylen, "no picture in %d ms", HW_TIMEOUT_MS);
            goto out;
        }
        if (!job_dequeue(&j, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &flags)) break;
        if (errno != EAGAIN) {
            say(why, whylen, "DQBUF: %s", strerror(errno));
            goto out;
        }
    }
    job_dequeue(&j, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, NULL);
    t.decode = now_ms() - t1;
    if (flags & V4L2_BUF_FLAG_ERROR) {
        say(why, whylen, "the VPU refused or failed it (a video decode on the VPU has priority)");
        goto out;
    }
    Rk3128JpegPlanes pl = {.y = j.map[1], .ystride = (int)fc.fmt.pix_mp.plane_fmt[0].bytesperline,
                        .w = in.w, .h = in.h, .ncomp = in.ncomp, .hs = in.hs, .vs = in.vs};
    pl.c = (const uint8_t *)j.map[1] + (size_t)pl.ystride * ah;
    pl.cstride = pl.ystride;
    if (planes_cb) {
        double a = now_ms();
        planes_cb(&pl, cb_arg);
        tx += now_ms() - a;
    }
    double t2 = now_ms();
    if (convert_cb) {
        rgb = convert_cb(&pl, j.fd, j.maplen[1], convert_arg);
        if (!rgb) rgb = rk3128_jpeg_to_rgb(&pl, conv_threads());
    } else {
#ifdef RK3128_JPEG_DIAGNOSTIC
        rgb = rk3128_jpeg_diagnostic_convert(&pl, conv_threads(), j.fd, j.maplen[1]);
#else
        rgb = rk3128_jpeg_to_rgb(&pl, conv_threads());
#endif
    }
    if (!rgb) {
        say(why, whylen, "out of memory for the conversion to RGB");
        goto out;
    }
    t.convert = now_ms() - t2;
    *w = in.w, *h = in.h;
out:
    job_close(&j);
    t.total = now_ms() - t0 - tx;
    if (times) *times = t;
    return rgb;
}

unsigned char *rk3128_jpeg_hw_decode(const unsigned char *jpg, size_t n, int *w, int *h, char *why, size_t whylen,
                                  Rk3128JpegTimes *times, Rk3128JpegPlanesCb planes_cb, void *cb_arg) {
    return decode(jpg,n,w,h,why,whylen,times,planes_cb,cb_arg,NULL,NULL);
}

unsigned char *rk3128_jpeg_hw_load_convert(const char *path, int *w, int *h, long long min_pixels,
    char *why, size_t whylen, Rk3128JpegTimes *times, Rk3128JpegConvertCb convert_cb, void *convert_arg) {
    if (times) memset(times, 0, sizeof(*times));
    if (getenv("RK3128_JPEG_HW") && !strcmp(getenv("RK3128_JPEG_HW"), "0")) {
        say(why, whylen, "%s", "");
        return NULL;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        say(why, whylen, "%s", strerror(errno));
        return NULL;
    }
    struct stat st;
    unsigned char *buf = NULL, *rgb = NULL;
    if (fstat(fd, &st) || st.st_size < 4 || st.st_size > (off_t)HW_MAX_STREAM) {
        say(why, whylen, "file size %lld", (long long)st.st_size);
        goto out;
    }
    // the headers first: a small photo is not worth reading in full here
    unsigned char head[4096];
    ssize_t hn = pread(fd, head, sizeof(head), 0);
    Rk3128JpegInfo in;
    if (hn > 4 && !rk3128_jpeg_parse(head, (size_t)hn, &in, NULL, 0) && (long long)in.w * in.h < min_pixels) {
        say(why, whylen, "%s", "");
        goto out;
    }
    buf = malloc((size_t)st.st_size);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t r = pread(fd, buf + got, (size_t)st.st_size - got, (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (!buf || got != (size_t)st.st_size) {
        say(why, whylen, buf ? "short read" : "out of memory");
        goto out;
    }
    rgb = decode(buf,got,w,h,why,whylen,times,NULL,NULL,convert_cb,convert_arg);
out:
    free(buf);
    close(fd);
    return rgb;
}

unsigned char *rk3128_jpeg_hw_load(const char *path, int *w, int *h, long long min_pixels, char *why, size_t whylen,
                                Rk3128JpegTimes *times) {
    return rk3128_jpeg_hw_load_convert(path,w,h,min_pixels,why,whylen,times,NULL,NULL);
}
