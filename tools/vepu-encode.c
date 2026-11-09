// SPDX-License-Identifier: GPL-2.0
/* Bounded V4L2 encode probe. Runs only on explicit input; no product integration.
 * cc -O2 -Wall -Wextra -Werror tools/vepu-encode.c -o vepu-encode
 * vepu-encode DEVICE WIDTH HEIGHT FRAMES INPUT.yuv OUTPUT nv12|yuv420 [QP [CAPTURE_BYTES [h264|vp8]]]
 * Optional final fault test: invalid-offset|short-input|mismatched-geometry.
 * Input contains tightly packed visible frames. The probe pads the MB storage.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct mapping { void *addr; size_t size; };
struct queue {
    enum v4l2_buf_type type;
    unsigned planes;
    struct mapping map[VIDEO_MAX_PLANES];
    struct v4l2_format fmt;
    int streaming;
    unsigned fault;
};

static void fail(const char *what) { perror(what); exit(1); }
static int xioctl(int fd, unsigned long cmd, void *arg) {
    int rc;
    do { rc = ioctl(fd, cmd, arg); } while (rc < 0 && errno == EINTR);
    return rc;
}
static void checked(int fd, unsigned long cmd, void *arg, const char *what) {
    if (xioctl(fd, cmd, arg)) fail(what);
}
static unsigned number(const char *s, unsigned min, unsigned max) {
    char *end; unsigned long v;
    errno = 0; v = strtoul(s, &end, 10);
    if (errno || !*s || *end || v < min || v > max) {
        fprintf(stderr, "invalid number: %s (range %u..%u)\n", s, min, max); exit(2);
    }
    return (unsigned)v;
}
static double now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) fail("clock_gettime");
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void format(int fd, struct queue *q, unsigned w, unsigned h,
                   unsigned fourcc, unsigned size) {
    q->fmt.type = q->type;
    q->fmt.fmt.pix_mp.width = w;
    q->fmt.fmt.pix_mp.height = h;
    q->fmt.fmt.pix_mp.pixelformat = fourcc;
    q->fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    q->fmt.fmt.pix_mp.plane_fmt[0].sizeimage = size;
    checked(fd, VIDIOC_S_FMT, &q->fmt, "S_FMT");
    if (q->fmt.fmt.pix_mp.pixelformat != fourcc ||
        q->fmt.fmt.pix_mp.width != w || q->fmt.fmt.pix_mp.height != h) {
        fprintf(stderr, "driver changed requested format/geometry\n"); exit(1);
    }
    q->planes = q->fmt.fmt.pix_mp.num_planes;
    if (!q->planes || q->planes > VIDEO_MAX_PLANES) { errno = EPROTO; fail("planes"); }
}
static void buffers(int fd, struct queue *q) {
    struct v4l2_requestbuffers req = { .count = 1, .type = q->type,
                                     .memory = V4L2_MEMORY_MMAP };
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}};
    struct v4l2_buffer b = { .type = q->type, .memory = V4L2_MEMORY_MMAP,
                            .length = q->planes, .m.planes = planes };
    checked(fd, VIDIOC_REQBUFS, &req, "REQBUFS");
    if (!req.count) { errno = ENOMEM; fail("REQBUFS count"); }
    if (q->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
        (req.capabilities & V4L2_BUF_CAP_SUPPORTS_DMABUF)) {
        errno = EPROTO; fail("video capture must use coherent MMAP buffers");
    }
    if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
        (req.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS)) {
        errno = EPROTO; fail("stateful output must not advertise media requests");
    }
    checked(fd, VIDIOC_QUERYBUF, &b, "QUERYBUF");
    for (unsigned p = 0; p < q->planes; p++) {
        q->map[p].size = planes[p].length;
        q->map[p].addr = mmap(NULL, planes[p].length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, planes[p].m.mem_offset);
        if (q->map[p].addr == MAP_FAILED) fail("mmap");
    }
}
static void format_busy(int fd, struct queue *q) {
    struct v4l2_format f = q->fmt;
    if (!xioctl(fd, VIDIOC_S_FMT, &f) || errno != EBUSY) {
        errno = EPROTO; fail("format change must fail while buffers exist");
    }
}
static void format_kinds(int fd, struct queue *src, struct queue *dst) {
    struct v4l2_format f = dst->fmt;
    f.fmt.pix_mp.pixelformat = src->fmt.fmt.pix_mp.pixelformat;
    checked(fd, VIDIOC_TRY_FMT, &f, "TRY_FMT coded queue");
    if (f.fmt.pix_mp.pixelformat == src->fmt.fmt.pix_mp.pixelformat) {
        errno = EPROTO; fail("coded queue accepted raw format");
    }
    f = src->fmt;
    f.fmt.pix_mp.pixelformat = dst->fmt.fmt.pix_mp.pixelformat;
    checked(fd, VIDIOC_TRY_FMT, &f, "TRY_FMT raw queue");
    if (f.fmt.pix_mp.pixelformat == dst->fmt.fmt.pix_mp.pixelformat) {
        errno = EPROTO; fail("raw queue accepted coded format");
    }
}
static void format_limits(int fd, struct queue *dst,
                          const struct v4l2_selection *crop, int vp8) {
    struct v4l2_format f = dst->fmt;
    f.fmt.pix_mp.width = 96; f.fmt.pix_mp.height = 32;
    checked(fd, VIDIOC_TRY_FMT, &f, "TRY_FMT video minimum");
    if (f.fmt.pix_mp.width != (vp8 ? 144U : 96U) || f.fmt.pix_mp.height != 96) {
        errno = EPROTO; fail("video minimum geometry");
    }
    for (unsigned i = 0; i < 3; i++) {
        struct v4l2_selection bad = *crop;
        if (i == 0) bad.r.height = 94;
        if (i == 1) bad.r.width = (vp8 ? 132 : 96) - 4;
        if (i == 2) bad.r.height = 97;
        if (!xioctl(fd, VIDIOC_S_SELECTION, &bad) || errno != EINVAL) {
            errno = EPROTO; fail("invalid video crop must fail");
        }
    }
    struct v4l2_selection selected = { .type = crop->type, .target = crop->target };
    checked(fd, VIDIOC_G_SELECTION, &selected, "G_SELECTION after rejected crop");
    if (selected.r.width != crop->r.width || selected.r.height != crop->r.height) {
        errno = EPROTO; fail("rejected crop changed geometry");
    }
}
static void enqueue(int fd, struct queue *q, int source, unsigned frame) {
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}};
    struct v4l2_buffer b = { .type = q->type, .memory = V4L2_MEMORY_MMAP,
                            .length = q->planes, .m.planes = planes,
                            .timestamp = { .tv_sec = frame + 1 } };
    for (unsigned p = 0; p < q->planes; p++) {
        planes[p].length = q->map[p].size;
        if (source) planes[p].bytesused = q->fmt.fmt.pix_mp.plane_fmt[p].sizeimage;
        if (source && !p && q->fault == 1) planes[p].data_offset = 1;
        if (source && !p && q->fault == 2) planes[p].bytesused--;
    }
    checked(fd, VIDIOC_QBUF, &b, "QBUF");
}
static unsigned dequeue(int fd, struct queue *q, unsigned frame) {
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}};
    struct v4l2_buffer b = { .type = q->type, .memory = V4L2_MEMORY_MMAP,
                            .length = q->planes, .m.planes = planes };
    double deadline = now() + 5;
    for (;;) {
        if (!xioctl(fd, VIDIOC_DQBUF, &b)) break;
        if (errno != EAGAIN) fail("DQBUF");
        int ms = (int)((deadline - now()) * 1000);
        if (ms <= 0) { errno = ETIMEDOUT; fail("DQBUF deadline"); }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int rc = poll(&p, 1, ms);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) { if (!rc) errno = ETIMEDOUT; fail("poll"); }
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EIO; fail("poll device"); }
    }
    if (b.flags & V4L2_BUF_FLAG_ERROR) {
        fprintf(stderr, "ERROR buffer payload=%u\n", planes[0].bytesused);
        errno = EIO; fail("encoded buffer ERROR");
    }
    if (b.index || b.sequence != frame) { errno = EPROTO; fail("buffer identity"); }
    if (q->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        if (!planes[0].bytesused || planes[0].bytesused > q->map[0].size ||
            planes[0].data_offset) { errno = EPROTO; fail("payload bounds"); }
        return planes[0].bytesused;
    }
    return 0;
}
static void stop(int fd, struct queue *q) {
    if (q->streaming) checked(fd, VIDIOC_STREAMOFF, &q->type, "STREAMOFF");
    for (unsigned p = 0; p < q->planes; p++)
        if (q->map[p].addr && munmap(q->map[p].addr, q->map[p].size)) fail("munmap");
}
static void load(FILE *input, struct queue *src, unsigned w, unsigned h, int nv12) {
    unsigned expected = nv12 ? 2 : 3;
    if (src->planes != expected) { errno = EPROTO; fail("raw planes"); }
    for (unsigned p = 0; p < src->planes; p++) {
        unsigned row = p && !nv12 ? w / 2 : w;
        unsigned rows = p ? h / 2 : h;
        unsigned stride = src->fmt.fmt.pix_mp.plane_fmt[p].bytesperline;
        unsigned stored = p ? src->fmt.fmt.pix_mp.height / 2 : src->fmt.fmt.pix_mp.height;
        unsigned char *dest = src->map[p].addr;
        if (stride < row || (uint64_t)stride * stored > src->map[p].size) {
            errno = EPROTO; fail("raw plane bounds");
        }
        memset(dest, p ? 128 : 16, src->map[p].size);
        for (unsigned y = 0; y < rows; y++) {
            if (fread(dest + y * stride, 1, row, input) != row) {
                fprintf(stderr, "short input frame\n"); exit(1);
            }
            if (p && nv12) {
                for (unsigned x = row; x < stride; x++)
                    dest[y * stride + x] = dest[y * stride + row - 2 + (x & 1)];
            } else if (stride > row) {
                memset(dest + y * stride + row, dest[y * stride + row - 1], stride - row);
            }
        }
        for (unsigned y = rows; y < stored; y++)
            memcpy(dest + y * stride, dest + (rows - 1) * stride, stride);
    }
}
int main(int argc, char **argv) {
    if (argc < 8 || argc > 12) {
        fprintf(stderr, "usage: %s DEVICE WIDTH HEIGHT FRAMES INPUT OUTPUT nv12|yuv420 [QP [CAPTURE_BYTES [h264|vp8 [invalid-offset|short-input|mismatched-geometry]]]]\n", argv[0]); return 2;
    }
    int vp8 = argc > 10 && !strcmp(argv[10], "vp8");
    unsigned w = number(argv[2], vp8 ? 132 : 96, 1920), h = number(argv[3], 96, 1088);
    if (argc > 10 && strcmp(argv[10], "h264") && !vp8) { fprintf(stderr, "codec must be h264|vp8\n"); return 2; }
    unsigned count = number(argv[4], 1, 120), qp = argc > 8 ? number(argv[8], 0, vp8 ? 127 : 51) : (vp8 ? 40 : 26);
    unsigned capsize = argc > 9 ? number(argv[9], 0, 16 * 1024 * 1024) : 0;
    unsigned fault = 0;
    if (argc > 11) {
        if (!strcmp(argv[11], "invalid-offset")) fault = 1;
        else if (!strcmp(argv[11], "short-input")) fault = 2;
        else if (!strcmp(argv[11], "mismatched-geometry")) fault = 3;
        else { fprintf(stderr, "unknown fault test\n"); return 2; }
    }
    int nv12 = !strcmp(argv[7], "nv12");
    if ((!nv12 && strcmp(argv[7], "yuv420")) || w % 4 || h % 2) {
        fprintf(stderr, "format must be nv12|yuv420, width divisible by 4, height even\n"); return 2;
    }
    FILE *input = fopen(argv[5], "rb"); if (!input) fail("input");
    FILE *output = fopen(argv[6], "wb"); if (!output) fail("output");
    int fd = open(argv[1], O_RDWR | O_NONBLOCK | O_CLOEXEC); if (fd < 0) fail("device");
    struct v4l2_capability capability = {0};
    checked(fd, VIDIOC_QUERYCAP, &capability, "QUERYCAP");
    if (!(capability.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE)) { errno = ENODEV; fail("M2M device"); }
    unsigned aw = (w + 15) & ~15U, ah = (h + 15) & ~15U;
    struct queue src = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .fault = fault };
    struct queue dst = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    format(fd, &dst, aw, ah, vp8 ? V4L2_PIX_FMT_VP8 : V4L2_PIX_FMT_H264, capsize);
    unsigned sw = fault == 3 ? (aw == 1920 ? aw - 16 : aw + 16) : aw;
    format(fd, &src, sw, ah, nv12 ? V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_YUV420M, 0);
    struct v4l2_selection crop = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .target = V4L2_SEL_TGT_CROP, .r = { .width = w, .height = h } };
    if (fault == 3) {
        crop.r.width = aw; crop.r.height = ah;
    } else {
        checked(fd, VIDIOC_S_SELECTION, &crop, "S_SELECTION");
        if ((unsigned)crop.r.width != w || (unsigned)crop.r.height != h) { errno = EPROTO; fail("crop"); }
    }
    struct v4l2_control quality = { .id = vp8 ? V4L2_CID_MPEG_VIDEO_VPX_I_FRAME_QP : V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, .value = qp };
    checked(fd, VIDIOC_S_CTRL, &quality, "S_CTRL QP");
    format_kinds(fd, &src, &dst);
    format_limits(fd, &dst, &crop, vp8);
    /* Exercise both allocation orders and both format-freeze directions. */
    struct queue *first = vp8 ? &src : &dst, *second = vp8 ? &dst : &src;
    buffers(fd, first);
    format_busy(fd, &dst); format_busy(fd, &src);
    buffers(fd, second);
    if (vp8) {
        unsigned char ivf[32] = { 'D','K','I','F',0,0,32,0,'V','P','8','0' };
        ivf[12] = w; ivf[13] = w >> 8; ivf[14] = h; ivf[15] = h >> 8;
        ivf[16] = 30; ivf[20] = 1; ivf[24] = count;
        if (fwrite(ivf, 1, sizeof(ivf), output) != sizeof(ivf)) fail("IVF header");
    }
    double elapsed = 0; uint64_t bytes = 0;
    for (unsigned frame = 0; frame < count; frame++) {
        load(input, &src, w, h, nv12);
        double start = now();
        enqueue(fd, &dst, 0, frame); enqueue(fd, &src, 1, frame);
        if (!frame) {
            checked(fd, VIDIOC_STREAMON, &dst.type, "STREAMON capture"); dst.streaming = 1;
            checked(fd, VIDIOC_STREAMON, &src.type, "STREAMON output"); src.streaming = 1;
        }
        unsigned size = dequeue(fd, &dst, frame);
        dequeue(fd, &src, frame);
        elapsed += now() - start;
        if (vp8) {
            unsigned char tag[12] = {0};
            for (unsigned i = 0; i < 4; i++) tag[i] = size >> (i * 8);
            tag[4] = frame; /* bounded to 120 frames */
            if (fwrite(tag, 1, sizeof(tag), output) != sizeof(tag)) fail("IVF frame");
        }
        if (fwrite(dst.map[0].addr, 1, size, output) != size) fail("output write");
        bytes += size;
    }
    if (fgetc(input) != EOF) { fprintf(stderr, "input has extra frames\n"); return 1; }
    stop(fd, &src); stop(fd, &dst);
    if (close(fd) || fclose(input) || fclose(output)) fail("close");
    printf("{\"codec\":\"%s\",\"width\":%u,\"height\":%u,\"frames\":%u,\"qp\":%u,\"bytes\":%llu,\"encode_seconds\":%.6f,\"fps\":%.3f,\"mpixel_s\":%.3f}\n",
           vp8 ? "vp8" : "h264", w, h, count, qp, (unsigned long long)bytes, elapsed, count / elapsed, w * (double)h * count / elapsed / 1e6);
    return 0;
}
