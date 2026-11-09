/* SPDX-License-Identifier: MIT
 * RK3128 IEP NV12 exerciser. Build: cc -O2 -Wall -Wextra -Werror
 * tools/iep-test.c -o iep-test -lm
 * No private ioctl ABI: discover the experimental spatial controls by name.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct image { uint8_t *p; size_t size; unsigned stride, width, height; };
struct buffer { void *p; size_t size; int dmafd; unsigned offset; };
struct context { int fd, owner; bool dmabuf, expect_error; struct buffer buf[2]; struct image image[2]; };
struct operation { const char *name; int control, value, filter; enum v4l2_field field; };
static unsigned spatial_amount = 8, spatial_radius = 1, spatial_threshold = 255, capture_padding = 32;
static bool rec709, full_range, spatial_gray;
static unsigned spatial_luma = 112;
static const struct operation operations[] = {
    { "copy", 0, 0, 0, V4L2_FIELD_NONE },
    { "brightness", V4L2_CID_BRIGHTNESS, 12, 0, V4L2_FIELD_NONE },
    { "contrast", V4L2_CID_CONTRAST, 96, 0, V4L2_FIELD_NONE },
    { "saturation", V4L2_CID_SATURATION, 0, 0, V4L2_FIELD_NONE },
    { "hue", V4L2_CID_HUE, 30, 0, V4L2_FIELD_NONE },
    { "deinterlace-tb", 0, 0, 0, V4L2_FIELD_INTERLACED_TB },
    { "deinterlace-bt", 0, 0, 0, V4L2_FIELD_INTERLACED_BT },
    { "denoise", 0, 0, 1, V4L2_FIELD_NONE },
    { "detail", 0, 0, 2, V4L2_FIELD_NONE },
    { "edge", 0, 0, 3, V4L2_FIELD_NONE },
};
static void die(const char *s) { perror(s); exit(1); }
static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do { r = ioctl(fd, request, arg); } while (r < 0 && errno == EINTR);
    return r;
}
static void checked(int fd, unsigned long request, void *arg, const char *name)
{
    if (xioctl(fd, request, arg) < 0) die(name);
}
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock_gettime");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static uint8_t clip(int x) { return x < 0 ? 0 : x > 255 ? 255 : x; }
static enum v4l2_buf_type type(unsigned i)
{
    return i ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
}
static void sync_buffer(struct context *c, unsigned i, bool start)
{
    if (c->dmabuf) {
        struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_RW |
            (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) };
        checked(c->buf[i].dmafd, DMA_BUF_IOCTL_SYNC, &s, "DMA_BUF_IOCTL_SYNC");
    }
}
static void format(int fd, unsigned i, unsigned width, unsigned height,
                   enum v4l2_field field, struct image *im)
{
    struct v4l2_format f = { .type = type(i) };
    f.fmt.pix.width = width;
    f.fmt.pix.height = height;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix.field = i ? V4L2_FIELD_NONE : field;
    /* V4L2 clears quantization/encoding unless extended fields are marked valid. */
    f.fmt.pix.priv = V4L2_PIX_FMT_PRIV_MAGIC;
    /* Exercise nontrivial stride, not only tightly packed pictures. */
    f.fmt.pix.bytesperline = width + (i ? capture_padding : 16);
    f.fmt.pix.colorspace = rec709 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M;
    f.fmt.pix.ycbcr_enc = rec709 ? V4L2_YCBCR_ENC_709 : V4L2_YCBCR_ENC_601;
    f.fmt.pix.quantization = full_range ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE;
    f.fmt.pix.xfer_func = V4L2_XFER_FUNC_709;
    checked(fd, VIDIOC_S_FMT, &f, "S_FMT");
    if (f.fmt.pix.width != width || f.fmt.pix.height != height ||
        f.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12 ||
        f.fmt.pix.field != (i ? V4L2_FIELD_NONE : field) ||
        f.fmt.pix.ycbcr_enc != (rec709 ? V4L2_YCBCR_ENC_709 : V4L2_YCBCR_ENC_601) ||
        f.fmt.pix.quantization != (full_range ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE) ||
        f.fmt.pix.xfer_func != V4L2_XFER_FUNC_709) {
        fprintf(stderr, "format changed unexpectedly\n"); exit(1);
    }
    *im = (struct image) { .width = width, .height = height,
        .stride = f.fmt.pix.bytesperline, .size = f.fmt.pix.sizeimage };
}
static void request(int fd, unsigned i, enum v4l2_memory mem)
{
    struct v4l2_requestbuffers r = { .count = 1, .type = type(i), .memory = mem };
    checked(fd, VIDIOC_REQBUFS, &r, "REQBUFS");
    if (r.count < 1) { fprintf(stderr, "no buffers\n"); exit(1); }
}
static void set_control(int fd, uint32_t id, int value)
{
    struct v4l2_control c = { .id = id, .value = value };
    checked(fd, VIDIOC_S_CTRL, &c, "S_CTRL");
}
static uint32_t control_by_name(int fd, const char *name)
{
    struct v4l2_query_ext_ctrl q = { .id = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND };
    while (!xioctl(fd, VIDIOC_QUERY_EXT_CTRL, &q)) {
        if (!strcmp(q.name, name)) return q.id;
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    }
    fprintf(stderr, "missing control: %s\n", name); exit(1);
}
static void open_context(struct context *c, const char *device, unsigned width,
                         unsigned height, const struct operation *op, bool dmabuf)
{
    *c = (struct context) { .fd = -1, .owner = -1, .dmabuf = dmabuf };
    c->fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (c->fd < 0) die(device);
    struct v4l2_capability cap = {0};
    checked(c->fd, VIDIOC_QUERYCAP, &cap, "QUERYCAP");
    if (strcmp((char *)cap.driver, "rockchip-iep")) {
        fprintf(stderr, "refusing device with driver %s\n", cap.driver); exit(1);
    }
    if (op->control) set_control(c->fd, op->control, op->value);
    set_control(c->fd, control_by_name(c->fd, "Spatial filter"), op->filter);
    if (op->filter) {
        set_control(c->fd, V4L2_CID_SHARPNESS, spatial_amount);
        set_control(c->fd, control_by_name(c->fd, "Spatial radius code"), spatial_radius);
        set_control(c->fd, control_by_name(c->fd, "Spatial threshold"), spatial_threshold);
    }
    if (dmabuf) {
        c->owner = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (c->owner < 0) die("open allocator");
    }
    for (unsigned i = 0; i < 2; i++) {
        int allocator = dmabuf ? c->owner : c->fd;
        format(c->fd, i, width, height, op->field, &c->image[i]);
        if (dmabuf) {
            struct image dummy;
            format(allocator, i, width, height, V4L2_FIELD_NONE, &dummy);
        }
        request(allocator, i, V4L2_MEMORY_MMAP);
        struct v4l2_buffer b = { .type = type(i), .memory = V4L2_MEMORY_MMAP };
        checked(allocator, VIDIOC_QUERYBUF, &b, "QUERYBUF");
        c->buf[i].size = b.length;
        c->buf[i].offset = b.m.offset;
        c->buf[i].dmafd = -1;
        c->buf[i].p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, allocator, b.m.offset);
        if (c->buf[i].p == MAP_FAILED) die("mmap");
        c->image[i].p = c->buf[i].p;
        if (dmabuf) {
            struct v4l2_exportbuffer e = { .type = type(i), .flags = O_CLOEXEC | O_RDWR };
            checked(allocator, VIDIOC_EXPBUF, &e, "EXPBUF");
            c->buf[i].dmafd = e.fd;
            request(c->fd, i, V4L2_MEMORY_DMABUF);
        }
    }
}
static void close_context(struct context *c)
{
    for (unsigned i = 0; i < 2; i++) {
        enum v4l2_buf_type t = type(i);
        checked(c->fd, VIDIOC_STREAMOFF, &t, "STREAMOFF");
        if (munmap(c->buf[i].p, c->buf[i].size)) die("munmap");
        if (c->buf[i].dmafd >= 0) close(c->buf[i].dmafd);
    }
    close(c->fd);
    if (c->owner >= 0) close(c->owner);
}
static void queue(struct context *c, unsigned i)
{
    struct v4l2_buffer b = { .type = type(i), .index = 0,
        .memory = c->dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP,
        .length = c->buf[i].size, .bytesused = c->image[i].size,
        .field = i ? V4L2_FIELD_NONE : V4L2_FIELD_ANY };
    if (c->dmabuf) b.m.fd = c->buf[i].dmafd;
    checked(c->fd, VIDIOC_QBUF, &b, "QBUF");
}
static void dequeue(struct context *c, unsigned i, unsigned frame)
{
    struct v4l2_buffer b = { .type = type(i),
        .memory = c->dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP };
    struct pollfd p = { .fd = c->fd, .events = i ? POLLIN : POLLOUT };
    uint64_t deadline = now_ns() + 3000000000ULL;
    for (;;) {
        if (!xioctl(c->fd, VIDIOC_DQBUF, &b)) break;
        if (errno != EAGAIN) die("DQBUF");
        if (now_ns() >= deadline) { fprintf(stderr, "DQBUF timeout\n"); exit(1); }
        int r = poll(&p, 1, 100);
        if (r < 0 && errno != EINTR) die("poll");
    }
    if (!!(b.flags & V4L2_BUF_FLAG_ERROR) != c->expect_error || b.sequence != frame ||
        (i && b.field != V4L2_FIELD_NONE)) {
        fprintf(stderr, "buffer error: queue=%u flags=%x seq=%u expected=%u field=%u\n",
            i, b.flags, b.sequence, frame, b.field); exit(1);
    }
}
/* Repeatable in-range NV12; deinterlace uses a field-invariant ramp (an
 * exact reference for both field orders, no undocumented interpolation). */
static void fixture(struct image *im, const struct operation *op, bool textured)
{
    memset(im->p, 0xa5, im->size);
    for (unsigned y = 0; y < im->height; y++)
        for (unsigned x = 0; x < im->width; x++) {
            int v;
            if (op->filter && !textured) v = spatial_luma;
            else if (op->field != V4L2_FIELD_NONE) {
                if (textured) v = x < ((y & 1) ? 2 : 1) * im->width / 3 ? 64 : 192;
                else v = 32 + 160 * x / im->width;
            }
            else v = 40 + ((x / 8 + y / 8) % 12) * 12;
            if (textured && op->field == V4L2_FIELD_NONE) v += (int)((x * 37 + y * 13) % 17) - 8;
            im->p[y * im->stride + x] = clip(v);
        }
    uint8_t *uv = im->p + im->stride * im->height;
    for (unsigned y = 0; y < im->height / 2; y++)
        for (unsigned x = 0; x < im->width; x += 2) {
            bool gray = op->field != V4L2_FIELD_NONE || (op->filter && (textured || spatial_gray));
            uv[y * im->stride + x] = gray ? 128 : op->filter ? 112 : 96 + ((x / 8) % 4) * 16;
            uv[y * im->stride + x + 1] = gray ? 128 : op->filter ? 144 : 96 + ((y / 8) % 4) * 16;
        }
}
static void reference(const struct image *in, struct image *out, const struct operation *op)
{
    if (op->control == V4L2_CID_HUE) {
        double angle = op->value * M_PI / 180.0;
        int cosine = (int)(cos(angle) * 128 + 1e-7);
        double sin_q7 = sin(angle) * 128;
        int sine = (int)(sin_q7 + (sin_q7 < 0 ? -1e-7 : 1e-7));
        for (unsigned y = 0; y < in->height; y++)
            memcpy(out->p + y * out->stride, in->p + y * in->stride, in->width);
        for (unsigned y = in->height; y < in->height * 3 / 2; y++)
            for (unsigned x = 0; x < in->width; x += 2) {
                int u = in->p[y * in->stride + x] - 128;
                int v = in->p[y * in->stride + x + 1] - 128;
                out->p[y * out->stride + x] = clip(128 + (u * cosine - v * sine) / 128);
                out->p[y * out->stride + x + 1] = clip(128 + (u * sine + v * cosine) / 128);
            }
        return;
    }
    bool brightness = !strcmp(op->name, "brightness");
    bool contrast = !strcmp(op->name, "contrast");
    bool saturation = !strcmp(op->name, "saturation");
    if (!brightness && !contrast && !saturation) {
        for (unsigned y = 0; y < in->height * 3 / 2; y++)
            memcpy(out->p + y * out->stride, in->p + y * in->stride, in->width);
        return;
    }
    for (unsigned y = 0; y < in->height * 3 / 2; y++)
        for (unsigned x = 0; x < in->width; x++) {
            int v = in->p[y * in->stride + x];
            if (brightness && y < in->height) v += op->value;
            if (contrast)
                v = y < in->height ? 16 + ((v - 16) * op->value) / 128 :
                    128 + ((v - 128) * op->value) / 128;
            if (saturation && y >= in->height) v = 128 + ((v - 128) * op->value) / 128;
            out->p[y * out->stride + x] = clip(v);
        }
}
static void save_image(const char *dir, const char *name, const struct image *im, const char *suffix)
{
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s-%s.nv12", dir, name, suffix) >= (int)sizeof(path)) {
        fprintf(stderr, "output path too long\n"); exit(1);
    }
    FILE *f = fopen(path, "wb");
    if (!f) die(path);
    for (unsigned y = 0; y < im->height * 3 / 2; y++)
        if (fwrite(im->p + y * im->stride, 1, im->width, f) != im->width) die("write image");
    if (fclose(f)) die("close image");
}
static bool run(const char *device, unsigned width, unsigned height, unsigned rounds,
                const struct operation *op, bool dmabuf, const char *outdir, bool alias, bool abort_job)
{
    struct context c;
    open_context(&c, device, width, height, op, dmabuf);
    if (alias) {
        close(c.buf[1].dmafd);
        c.buf[1].dmafd = dup(c.buf[0].dmafd);
        if (c.buf[1].dmafd < 0) die("dup DMA-BUF");
        c.expect_error = true;
    }
    struct image cpu = c.image[1];
    cpu.p = malloc(cpu.size);
    if (!cpu.p) die("malloc");
    sync_buffer(&c, 0, true);
    fixture(&c.image[0], op, false);
    /* Compare computation in cached CPU memory. Reading the uncached CMA
     * mapping inside this timer would make an unfair CPU baseline.
     */
    struct image cached = c.image[0];
    cached.p = malloc(cached.size);
    if (!cached.p) die("malloc CPU input");
    memcpy(cached.p, c.image[0].p, cached.size);
    reference(&cached, &cpu, op); /* fault in output before timing */
    uint64_t ct = now_ns();
    for (unsigned n = 0; n < rounds; n++) reference(&cached, &cpu, op);
    uint64_t cpu_ns = now_ns() - ct;
    free(cached.p);
    sync_buffer(&c, 0, false);
    sync_buffer(&c, 1, true);
    memset(c.image[1].p, 0xcc, c.image[1].size);
    sync_buffer(&c, 1, false);
    uint64_t elapsed = 0;
    for (unsigned n = 0; n < rounds; n++) {
        uint64_t t = now_ns();
        queue(&c, 1);
        queue(&c, 0);
        if (!n) {
            for (unsigned i = 0; i < 2; i++) {
                enum v4l2_buf_type tp = type(i);
                checked(c.fd, VIDIOC_STREAMON, &tp, "STREAMON");
            }
        }
        if (abort_job) {
            close_context(&c);
            free(cpu.p);
            printf("{\"op\":\"%s\",\"streamoff_completed\":true,\"pass\":true}\n", op->name);
            return true;
        }
        dequeue(&c, 1, n);
        dequeue(&c, 0, n);
        elapsed += now_ns() - t;
    }
    if (alias) {
        close_context(&c);
        free(cpu.p);
        printf("{\"op\":\"%s\",\"alias_rejected\":true,\"pass\":true}\n", op->name);
        return true;
    }
    sync_buffer(&c, 1, true);
    unsigned max_error = 0;
    uint64_t error_sum = 0, mismatches = 0;
    /* CSC has fixed-point rounding. Deinterlacing may duplicate its last
     * boundary sample; test the full field-invariant fixture nevertheless. */
    unsigned tolerance = op->filter ? 3 : !strcmp(op->name, "copy") ||
        op->field != V4L2_FIELD_NONE ? 0 : 1;
    for (unsigned y = 0; y < height * 3 / 2; y++)
        for (unsigned x = 0; x < width; x++) {
            unsigned e = abs((int)c.image[1].p[y * c.image[1].stride + x] -
                             cpu.p[y * cpu.stride + x]);
            if (e > max_error) max_error = e;
            error_sum += e;
            mismatches += e > tolerance;
        }
    bool pass = !mismatches;
    printf("{\"op\":\"%s\",\"memory\":\"%s\",\"width\":%u,\"height\":%u,"
           "\"frames\":%u,\"value\":%d,\"amount\":%u,\"filter\":%d,\"radius\":%u,\"threshold\":%u,\"matrix\":%u,\"full_range\":%s,\"hw_us\":%.3f,\"cpu_reference_us\":%.3f,\"max_error\":%u,"
           "\"mae\":%.6f,\"tolerance\":%u,\"bad_samples\":%llu,\"pass\":%s}\n",
        op->name, dmabuf ? "DMABUF" : "MMAP", width, height, rounds,
        op->value, spatial_amount, op->filter, spatial_radius, spatial_threshold, rec709 ? 709 : 601, full_range ? "true" : "false",
        (double)elapsed / rounds / 1000, (double)cpu_ns / rounds / 1000, max_error,
        (double)error_sum / (width * height * 3 / 2), tolerance,
        (unsigned long long)mismatches, pass ? "true" : "false");
    if (outdir) {
        sync_buffer(&c, 0, true);
        save_image(outdir, op->name, &c.image[0], "source");
        save_image(outdir, op->name, &c.image[1], "result");
        sync_buffer(&c, 0, false);
    }
    sync_buffer(&c, 1, false);
    /* A second nonuniform fixture makes the filter outputs reviewable;
     * its exact nonlinear hardware algorithm has no public CPU oracle. */
    if ((op->filter || op->field != V4L2_FIELD_NONE) && outdir) {
        sync_buffer(&c, 0, true);
        fixture(&c.image[0], op, true);
        save_image(outdir, op->name, &c.image[0], "textured-source");
        sync_buffer(&c, 0, false);
        queue(&c, 1); queue(&c, 0);
        dequeue(&c, 1, rounds); dequeue(&c, 0, rounds);
        sync_buffer(&c, 1, true);
        save_image(outdir, op->name, &c.image[1], "textured-result");
        sync_buffer(&c, 1, false);
    }
    free(cpu.p);
    close_context(&c);
    return pass;
}
/* Queue a peer before cancelling the first context. The peer must finish
 * independently: an abort notification from the first file must not act as
 * its IRQ or return its DMA buffer early. All iterations are finite. */
static bool peer_abort(const char *device, unsigned width, unsigned height, unsigned rounds)
{
    unsigned max_error = 0;
    for (unsigned n = 0; n < rounds; n++) {
        struct context a, b;
        open_context(&a, device, 16, 16, &operations[0], false);
        open_context(&b, device, width, height, &operations[0], false);
        fixture(&a.image[0], &operations[0], false);
        fixture(&b.image[0], &operations[0], false);
        memset(b.image[1].p, 0xcc, b.image[1].size);
        struct context *contexts[] = { &a, &b };
        for (unsigned j = 0; j < 2; j++) {
            queue(contexts[j], 1); queue(contexts[j], 0);
            for (unsigned i = 0; i < 2; i++) {
                enum v4l2_buf_type t = type(i);
                checked(contexts[j]->fd, VIDIOC_STREAMON, &t, "peer STREAMON");
            }
        }
        close_context(&a);
        dequeue(&b, 1, 0); dequeue(&b, 0, 0);
        for (unsigned y = 0; y < height * 3 / 2; y++)
            for (unsigned x = 0; x < width; x++) {
                unsigned error = abs((int)b.image[1].p[y * b.image[1].stride + x] -
                                     b.image[0].p[y * b.image[0].stride + x]);
                if (error > max_error) max_error = error;
            }
        close_context(&b);
    }
    printf("{\"peer_abort_iterations\":%u,\"max_error\":%u,\"pass\":%s}\n",
           rounds, max_error, max_error ? "false" : "true");
    return !max_error;
}

/* Changing a spatial control after STREAMON must not launch an unsupported
 * I2O1+RGB job. An errored job must leave the next ordinary field job usable. */
static bool mixed_filter_rejected(const char *device)
{
    struct context c;
    const struct operation *op = &operations[5];
    open_context(&c, device, 68, 36, op, true);
    sync_buffer(&c, 0, true);
    fixture(&c.image[0], op, false);
    sync_buffer(&c, 0, false);
    uint32_t filter = control_by_name(c.fd, "Spatial filter");
    for (unsigned n = 0; n < 3; n++) {
        if (n) set_control(c.fd, filter, n == 1 ? 1 : 0);
        c.expect_error = n == 1;
        queue(&c, 1); queue(&c, 0);
        if (!n) for (unsigned i = 0; i < 2; i++) {
            enum v4l2_buf_type tp = type(i);
            checked(c.fd, VIDIOC_STREAMON, &tp, "mixed STREAMON");
        }
        dequeue(&c, 1, n); dequeue(&c, 0, n);
    }
    unsigned maximum = 0;
    sync_buffer(&c, 0, true); sync_buffer(&c, 1, true);
    for (unsigned y = 0; y < c.image[0].height * 3 / 2; y++)
        for (unsigned x = 0; x < c.image[0].width; x++) {
            unsigned e = abs((int)c.image[0].p[y*c.image[0].stride+x] -
                             c.image[1].p[y*c.image[1].stride+x]);
            if (e > maximum) maximum = e;
        }
    sync_buffer(&c, 0, false); sync_buffer(&c, 1, false);
    close_context(&c);
    printf("{\"mixed_filter_rejected\":true,\"recovered_max_error\":%u,\"pass\":%s}\n",
           maximum, maximum ? "false" : "true");
    return maximum == 0;
}

static unsigned number(const char *s, unsigned lo, unsigned hi)
{
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || n < lo || n > hi) {
        fprintf(stderr, "invalid number: %s\n", s); exit(2);
    }
    return n;
}
static int signed_number(const char *s)
{
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || !*s || *end || n < INT_MIN || n > INT_MAX) {
        fprintf(stderr, "invalid signed number: %s\n", s); exit(2);
    }
    return n;
}
static void selftest(void)
{
    struct image in = { .width = 32, .height = 16, .stride = 48, .size = 48 * 24 };
    struct image out = { .width = 32, .height = 16, .stride = 64, .size = 64 * 24 };
    in.p = malloc(in.size); out.p = malloc(out.size);
    if (!in.p || !out.p) die("malloc");
    for (unsigned i = 0; i < sizeof(operations) / sizeof(*operations); i++) {
        fixture(&in, &operations[i], false);
        memset(out.p, 0x6b, out.size);
        reference(&in, &out, &operations[i]);
        for (unsigned y = 0; y < 24; y++) {
            for (unsigned x = out.width; x < out.stride; x++)
                if (out.p[y * out.stride + x] != 0x6b) abort();
        }
        if (!strcmp(operations[i].name, "brightness") && out.p[0] != in.p[0] + 12) abort();
        if (!strcmp(operations[i].name, "saturation") && out.p[out.stride * 16] != 128) abort();
        if (!strcmp(operations[i].name, "contrast") &&
            (out.p[0] != 34 || out.p[out.stride * 16] != 104)) abort();
        if (!strcmp(operations[i].name, "hue") &&
            (out.p[out.stride * 16] != 117 || out.p[out.stride * 16 + 1] != 85)) abort();
    }
    struct operation negative = operations[4];
    negative.value = -30;
    fixture(&in, &negative, false); reference(&in, &out, &negative);
    if (out.p[out.stride * 16] != 85 || out.p[out.stride * 16 + 1] != 117) abort();
    free(in.p); free(out.p);
    puts("PASS: ten NV12 CPU fixtures, independent stride, reference bounds and known samples");
}
int main(int argc, char **argv)
{
    const char *device = NULL, *opname = "all", *outdir = NULL;
    unsigned width = 640, height = 480, rounds = 4, force_filter = UINT_MAX;
    int override_value = 0;
    bool value_set = false, mixed = false;
    bool dmabuf = false, found = false, pass = true, alias = false, abort_job = false, peer = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) { selftest(); return 0; }
        if (!strcmp(argv[i], "--709")) { rec709 = true; continue; }
        if (!strcmp(argv[i], "--full-range")) { full_range = true; continue; }
        if (!strcmp(argv[i], "--mixed-filter")) { mixed = true; continue; }
        if (!strcmp(argv[i], "--peer-abort")) { peer = true; continue; }
        if (!strcmp(argv[i], "--alias")) { alias = true; dmabuf = true; continue; }
        if (!strcmp(argv[i], "--abort")) { abort_job = true; continue; }
        if (!strcmp(argv[i], "--dmabuf")) { dmabuf = true; continue; }
        if (!strcmp(argv[i], "--help")) {
            puts("iep-test --device /dev/videoN [--width 640 --height 480 --frames 4]\n"
                 "         [--op all|copy|brightness|contrast|saturation|hue|deinterlace-tb|deinterlace-bt|denoise|detail|edge]\n"
                 "         [--value INTEGER (BCSH --op only)] [--dmabuf] [--709] [--full-range] [--filter 0..3 (combine with BCSH)] [--amount 0..127] [--radius 0..1] [--threshold 0..255] [--dc-luma 16..235] [--output DIRECTORY] [--alias|--abort|--peer-abort|--mixed-filter]\n"
                 "iep-test --selftest  (CPU-only; no device)\n"
                 "JSON lines include numeric CPU reference error, tolerance and end-to-end job time.\n"
                 "Spatial references use a uniform colour frame; textured outputs need visual review.");
            return 0;
        }
        if (i + 1 == argc) { fprintf(stderr, "missing value\n"); return 2; }
        const char *key = argv[i++], *v = argv[i];
        if (!strcmp(key, "--device")) device = v;
        else if (!strcmp(key, "--op")) opname = v;
        else if (!strcmp(key, "--output")) outdir = v;
        else if (!strcmp(key, "--width")) width = number(v, 16, 1920);
        else if (!strcmp(key, "--height")) height = number(v, 16, 1088);
        else if (!strcmp(key, "--frames")) rounds = number(v, 1, 256);
        else if (!strcmp(key, "--value")) { override_value = signed_number(v); value_set = true; }
        else if (!strcmp(key, "--amount")) spatial_amount = number(v, 0, 127);
        else if (!strcmp(key, "--filter")) force_filter = number(v, 0, 3);
        else if (!strcmp(key, "--radius")) spatial_radius = number(v, 0, 1);
        else if (!strcmp(key, "--threshold")) spatial_threshold = number(v, 0, 255);
        else if (!strcmp(key, "--dc-luma")) { spatial_luma = number(v, 16, 235); spatial_gray = true; }
        else { fprintf(stderr, "unknown option: %s\n", key); return 2; }
    }
    if (!device || width % 4 || height % 4) {
        fprintf(stderr, "supply --device; dimensions must be multiples of four\n"); return 2;
    }
    if (value_set) {
        int lo = 0, hi = 255;
        if (!strcmp(opname, "brightness")) { lo = -32; hi = 31; }
        else if (!strcmp(opname, "hue")) { lo = -30; hi = 30; }
        else if (strcmp(opname, "contrast") && strcmp(opname, "saturation")) {
            fprintf(stderr, "--value requires a BCSH --op\n"); return 2;
        }
        if (override_value < lo || override_value > hi) {
            fprintf(stderr, "--value outside operation range\n"); return 2;
        }
    }
    if (alias) capture_padding = 16; /* same exported allocation must fit both queues */
    if (mixed) return mixed_filter_rejected(device) ? 0 : 1;
    if (peer) return peer_abort(device, width, height, rounds) ? 0 : 1;
    if (outdir && mkdir(outdir, 0755) && errno != EEXIST) die("mkdir output");
    for (unsigned i = 0; i < sizeof(operations) / sizeof(*operations); i++) {
        if (strcmp(opname, "all") && strcmp(opname, operations[i].name)) continue;
        found = true;
        struct operation selected = operations[i];
        if (value_set) selected.value = override_value;
        if (force_filter != UINT_MAX) selected.filter = force_filter;
        if (!run(device, width, height, rounds, &selected, dmabuf, outdir, alias, abort_job)) pass = false;
    }
    if (!found) { fprintf(stderr, "unknown operation: %s\n", opname); return 2; }
    return pass ? 0 : 1;
}
