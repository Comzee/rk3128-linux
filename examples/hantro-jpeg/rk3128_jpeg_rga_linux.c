// SPDX-License-Identifier: MIT
// #739 production form of the measured #730 fused Hantro -> RGA preparation.
// The owner's on-panel A/B supersedes the original numerical quality gate.
#include "rk3128_jpeg_rga.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef struct { char *why; size_t whylen; double deadline; } Rga;
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1000.0 + ts.tv_nsec/1e6;
}
static int call(int fd, unsigned long op, void *arg) {
    int result;
    do result=ioctl(fd,op,arg); while (result<0 && errno==EINTR);
    return result;
}
static int fail(Rga *r, const char *stage) {
    if (r->why && r->whylen) snprintf(r->why,r->whylen,"%s: %s",stage,strerror(errno));
    return -1;
}
static int inject(Rga *r, const char *stage) {
#ifdef RK3128_JPEG_RGA_DIAGNOSTIC
    if (rk3128_jpeg_rga_test_fail(stage)) {
        errno = !strcmp(stage,"timeout") ? ETIMEDOUT : ECANCELED;
        fail(r,stage); return 1;
    }
#else
    (void)r; (void)stage;
#endif
    return 0;
}
static int rga_open(Rga *r) {
    DIR *d = opendir("/sys/class/video4linux");
    struct dirent *e;
    int fd = -1;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "video", 5)) continue;
        char path[320];
        snprintf(path, sizeof(path), "/dev/%s", e->d_name);
        fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
        struct v4l2_capability c = {0};
        if (fd >= 0 && !call(fd, VIDIOC_QUERYCAP, &c) && !strcmp((char *)c.driver, "rockchip-rga1")) break;
        if (fd >= 0) close(fd);
        fd = -1;
    }
    if (d) closedir(d);
    if (fd < 0) { errno = ENODEV; fail(r, "RGA unavailable"); }
    return fd;
}
static int format(Rga *r, int fd, enum v4l2_buf_type type, int w, int h, int stride, uint32_t fourcc,
                  struct v4l2_format *f) {
    memset(f, 0, sizeof(*f));
    f->type = type;
    struct v4l2_pix_format_mplane *p = &f->fmt.pix_mp;
    p->width = (unsigned)w; p->height = (unsigned)h; p->pixelformat = fourcc;
    p->field = V4L2_FIELD_NONE; p->num_planes = 1;
    p->plane_fmt[0].bytesperline = (unsigned)stride;
    p->colorspace = V4L2_COLORSPACE_JPEG;
    p->ycbcr_enc = V4L2_YCBCR_ENC_601;
    p->quantization = V4L2_QUANTIZATION_FULL_RANGE;
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) p->flags = V4L2_PIX_FMT_FLAG_SET_CSC;
    if (call(fd, VIDIOC_S_FMT, f)) return fail(r, "S_FMT");
    if (p->width != (unsigned)w || p->height != (unsigned)h || p->pixelformat != fourcc ||
        p->num_planes != 1 || p->plane_fmt[0].bytesperline != (unsigned)stride ||
        p->quantization != V4L2_QUANTIZATION_FULL_RANGE || p->ycbcr_enc != V4L2_YCBCR_ENC_601) {
        errno = EINVAL; return fail(r, "normalized format/layout/colour");
    }
    return 0;
}
static int selection(Rga *r, int fd, enum v4l2_buf_type type, unsigned target, int w, int h) {
    struct v4l2_selection s = {.type = type, .target = target,
                              .r = {.width = (unsigned)w, .height = (unsigned)h}};
    if (call(fd, VIDIOC_S_SELECTION, &s)) return fail(r, "S_SELECTION");
    if (s.r.left || s.r.top || s.r.width != (unsigned)w || s.r.height != (unsigned)h) {
        errno = EINVAL; return fail(r, "normalized crop/compose");
    }
    return 0;
}
static int control(Rga *r, int fd, unsigned id, int value) {
    struct v4l2_control c = {.id = id, .value = value};
    if (call(fd, VIDIOC_S_CTRL, &c)) return fail(r, "S_CTRL");
    return 0;
}
static int request(Rga *r, int fd, enum v4l2_buf_type type, enum v4l2_memory memory) {
    struct v4l2_requestbuffers b = {.type = type, .memory = memory, .count = 1};
    if (call(fd, VIDIOC_REQBUFS, &b) || b.count != 1) { errno = ENOMEM; return fail(r, "REQBUFS"); }
    return 0;
}
static int queue(Rga *r, int fd, enum v4l2_buf_type type, int source_fd, size_t length) {
    struct v4l2_plane p = {.length = (unsigned)length};
    struct v4l2_buffer b = {.type = type, .length = 1, .m.planes = &p, .field = V4L2_FIELD_NONE,
                           .memory = source_fd >= 0 ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP};
    if (source_fd >= 0) { p.m.fd = source_fd; p.bytesused = (unsigned)length; b.timestamp.tv_sec = 1; }
    if (call(fd, VIDIOC_QBUF, &b)) return fail(r, "QBUF");
    return 0;
}
static int dequeue(Rga *r, int fd, enum v4l2_buf_type type) {
    struct v4l2_plane p = {0};
    struct v4l2_buffer b = {.type = type, .length = 1, .m.planes = &p,
        .memory = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP};
    struct pollfd pollfd = {.fd = fd, .events = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? POLLOUT : POLLIN};

    for (;;) {
        if (!call(fd, VIDIOC_DQBUF, &b)) {
            if (!(b.flags & V4L2_BUF_FLAG_ERROR)) return 0;
            errno = EIO; return fail(r, "ERROR buffer");
        }
        if (errno != EAGAIN) return fail(r, "DQBUF");
        double left = r->deadline - now_ms();
        if (left <= 0) { errno = ETIMEDOUT; return fail(r, "DMA deadline"); }
        int polled = poll(&pollfd, 1, (int)left + 1);
        if (polled < 0 && errno == EINTR) continue;
        if (polled <= 0) { if (!polled) errno = ETIMEDOUT; return fail(r, "poll"); }
    }
}


uint8_t *rk3128_jpeg_rga_prepare(const Rk3128JpegPlanes *pl, int hantro_fd, size_t length,
    const Rk3128JpegRgaTarget *target, char *why, size_t whylen) {
    if (why && whylen) why[0]=0;
    // No orientation-only/identity or small-image bet: #730 measured no wall win
    // there. Preserve exact CPU pixels instead. Header races also take fallback.
    if (!pl || !target || pl->w != target->source_w || pl->h != target->source_h ||
        pl->w>8176 || pl->h>8176 || pl->ncomp != 3 || pl->hs != 2 || (pl->vs != 1 && pl->vs != 2) ||
        (long long)pl->w*pl->h < 1000000 || target->w<1 || target->h<1 ||
        target->w>pl->w || target->h>pl->h || (target->w==pl->w && target->h==pl->h) ||
        !pl->y || !pl->c ||
        pl->ystride<pl->w || pl->ystride>8192 || pl->ystride!=pl->cstride || (pl->w&1) || (pl->h&1)) return NULL;
    int ow=target->w, oh=target->h;
    if (ow<2 || oh<2 || ow>2048 || oh>2048) return NULL;
    ptrdiff_t chroma=pl->c-pl->y;
    if (chroma<0 || chroma%pl->ystride) return NULL;
    int padded_h=(int)(chroma/pl->ystride);
    if (padded_h<pl->h || padded_h>8192) return NULL;
    size_t expected=(size_t)pl->ystride*padded_h*(pl->vs==2?3:4)/2;
    if (expected>length || expected>(48u<<20)) return NULL;
    Rga state={.why=why,.whylen=whylen};
    Rga *r=&state;
    int fd = -1, src = -1, dst = -1, streaming[2] = {0}, sync_started = 0, ok = 0;
    size_t maplen = 0;
    void *map = MAP_FAILED;
    uint8_t *rgb = NULL;
    struct v4l2_exportbuffer exp = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .flags = O_CLOEXEC | O_RDWR};
    if (call(hantro_fd, VIDIOC_EXPBUF, &exp)) { fail(r, "Hantro EXPBUF"); goto out; }
    src = exp.fd;
    if (inject(r, "export")) goto out;
    fd = rga_open(r);
    if (fd < 0) goto out;
    struct v4l2_format fo, fc;
    int stride = (ow * 3 + 3) & ~3;
    if (format(r, fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, (pl->w + 15) & ~15, padded_h, pl->ystride,
               pl->vs == 2 ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_NV16, &fo) ||
        format(r, fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, ow, oh, stride, V4L2_PIX_FMT_RGB24, &fc)) goto out;
    if (fo.fmt.pix_mp.plane_fmt[0].sizeimage != expected ||
        fc.fmt.pix_mp.plane_fmt[0].sizeimage != (unsigned)stride*oh) {
        errno = EINVAL; fail(r, "source sizeimage"); goto out;
    }
    // Hardware scale/transform combinations have orientation-dependent filter
    // rounding. Return one reduced unrotated image; Photos applies its existing
    // exact EXIF transform after the decoder/export allocation has been freed.
    if (control(r, fd, V4L2_CID_ROTATE, 0) ||
        control(r, fd, V4L2_CID_HFLIP, 0) || control(r, fd, V4L2_CID_VFLIP, 0) ||
        selection(r, fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_SEL_TGT_CROP, pl->w, pl->h) ||
        selection(r, fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_SEL_TGT_COMPOSE, ow, oh)) goto out;
    if (inject(r, "formats") || request(r, fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_DMABUF) ||
        request(r, fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP)) goto out;
    struct v4l2_plane plane = {0};
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
                           .length = 1, .m.planes = &plane};
    if (call(fd, VIDIOC_QUERYBUF, &b)) { fail(r, "QUERYBUF"); goto out; }
    maplen = plane.length;
    if (maplen < (size_t)stride * oh) { errno = EINVAL; fail(r, "short capture"); goto out; }
    map = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
    if (map == MAP_FAILED) { fail(r, "mmap"); goto out; }
    exp = (struct v4l2_exportbuffer){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .flags = O_CLOEXEC | O_RDWR};
    if (call(fd, VIDIOC_EXPBUF, &exp)) { fail(r, "RGA EXPBUF"); goto out; }
    dst = exp.fd;
    rgb = malloc((size_t)ow * oh * 3);
    if (!rgb) { fail(r, "cached RGB allocation"); goto out; }
    if (inject(r, "buffers") || queue(r, fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, src, expected) ||
        queue(r, fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, -1, maplen)) goto out;
    for (int q = 0; q < 2; q++) {
        int type = q ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (call(fd, VIDIOC_STREAMON, &type)) { fail(r, "STREAMON"); goto out; }
        streaming[q] = 1;
        if (inject(r, q ? "streaming" : "output-streaming")) goto out;
    }
    r->deadline = now_ms() + 3000;
    if (inject(r,"timeout")) goto out;
    if (dequeue(r, fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) || dequeue(r, fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)) goto out;
    if (inject(r, "dequeued")) goto out;
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    if (call(dst, DMA_BUF_IOCTL_SYNC, &sync)) { fail(r, "SYNC START"); goto out; }
    sync_started = 1;
    for (int y = 0; y < oh; y++) memcpy(rgb + (size_t)y * ow * 3, (uint8_t *)map + (size_t)y * stride, (size_t)ow * 3);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (call(dst, DMA_BUF_IOCTL_SYNC, &sync)) { fail(r, "SYNC END"); goto out; }
    sync_started = 0;
    ok = 1;
out:
    if (sync_started) {
        struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
        if (call(dst, DMA_BUF_IOCTL_SYNC, &sync)) { fail(r, "cleanup SYNC END"); ok = 0; }
    }
    // Close the importing context BEFORE releasing either export or returning to Hantro's job_close.
    // STREAMOFF acknowledges cancellation even if the job never dequeued; close is the final driver lifetime barrier.
    for (int q = 0; q < 2; q++) if (streaming[q]) {
        int type = q ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (call(fd, VIDIOC_STREAMOFF, &type)) { fail(r, "STREAMOFF"); ok = 0; }
    }
    if (map != MAP_FAILED) munmap(map, maplen);
    if (fd >= 0) close(fd);
    if (dst >= 0) close(dst);
    if (src >= 0) close(src);
    if (!ok) { free(rgb); rgb = NULL; }
    return rgb;
}
