/* SPDX-License-Identifier: MIT
 * Research-only one-pass exerciser; requires research-only.patch, never a product ABI.
 * Reuse the production tool's checked ioctls, DMA-BUF sync, bounded dequeue and cleanup.
 * Build from repo root: arm-linux-gnueabihf-gcc -O2 -static -Wall -Wextra -Werror
 * docs/research/iep-620/round3/exercise.c -o /var/tmp/620c/exercise -lm
 */
#define main iep_standard_main
#include "../../../tools/iep-test.c"
#undef main

static void research_format(int fd, unsigned i, unsigned w, unsigned h,
                            bool rgb, unsigned csc, struct image *im)
{
    struct v4l2_format f = { .type = type(i) };
    f.fmt.pix = (struct v4l2_pix_format) {
        .width = w, .height = h,
        /* Hardware ARGB word, little-endian B,G,R,A byte order. */
        .pixelformat = rgb ? V4L2_PIX_FMT_ABGR32 : V4L2_PIX_FMT_NV12,
        .field = V4L2_FIELD_NONE,
        .priv = V4L2_PIX_FMT_PRIV_MAGIC,
        .bytesperline = w * (rgb ? 4 : 1) + (i ? 32 : 16),
        .colorspace = csc & 2 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M,
        .ycbcr_enc = csc & 2 ? V4L2_YCBCR_ENC_709 : V4L2_YCBCR_ENC_601,
        .quantization = csc & 1 ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE,
        .xfer_func = V4L2_XFER_FUNC_709,
    };
    checked(fd, VIDIOC_S_FMT, &f, "research S_FMT");
    if (f.fmt.pix.width != w || f.fmt.pix.height != h ||
        f.fmt.pix.pixelformat != (rgb ? V4L2_PIX_FMT_ABGR32 : V4L2_PIX_FMT_NV12) ||
        f.fmt.pix.bytesperline < w * (rgb ? 4 : 1) ||
        f.fmt.pix.ycbcr_enc != (csc & 2 ? V4L2_YCBCR_ENC_709 : V4L2_YCBCR_ENC_601) ||
        f.fmt.pix.quantization != (csc & 1 ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE) ||
        f.fmt.pix.xfer_func != V4L2_XFER_FUNC_709 ||
        f.fmt.pix.sizeimage != f.fmt.pix.bytesperline * h * (rgb ? 2 : 3) / 2) {
        fputs("research format/size changed unexpectedly\n", stderr); exit(1);
    }
    *im = (struct image) { .width = w, .height = h, .stride = f.fmt.pix.bytesperline,
        .size = f.fmt.pix.sizeimage };
}
static void research_open(struct context *c, const char *dev, unsigned w, unsigned h,
                          bool srgb, bool drgb, unsigned csc, unsigned filter,
                          unsigned amount, unsigned radius, unsigned threshold, unsigned lut)
{
    *c = (struct context) { .fd = -1, .owner = -1, .dmabuf = true };
    c->fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (c->fd < 0) die(dev);
    struct v4l2_capability cap = {0};
    checked(c->fd, VIDIOC_QUERYCAP, &cap, "QUERYCAP");
    if (strcmp((char *)cap.driver, "rockchip-iep")) {
        fputs("refusing non-IEP device\n", stderr); exit(1);
    }
    set_control(c->fd, control_by_name(c->fd, "Spatial filter"), filter);
    set_control(c->fd, V4L2_CID_SHARPNESS, amount);
    set_control(c->fd, control_by_name(c->fd, "Spatial radius code"), radius);
    set_control(c->fd, control_by_name(c->fd, "Spatial threshold"), threshold);
    set_control(c->fd, control_by_name(c->fd, "Research DDE tables"), lut);
    c->owner = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (c->owner < 0) die("allocator open");
    for (unsigned i = 0; i < 2; i++) {
        struct image dummy;
        research_format(c->fd, i, w, h, i ? drgb : srgb, csc, &c->image[i]);
        research_format(c->owner, i, w, h, i ? drgb : srgb, csc, &dummy);
        request(c->owner, i, V4L2_MEMORY_MMAP);
        struct v4l2_buffer b = { .type = type(i), .memory = V4L2_MEMORY_MMAP };
        checked(c->owner, VIDIOC_QUERYBUF, &b, "QUERYBUF");
        c->buf[i] = (struct buffer) { .size = b.length, .dmafd = -1, .offset = b.m.offset };
        c->buf[i].p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, c->owner, b.m.offset);
        if (c->buf[i].p == MAP_FAILED) die("mmap");
        c->image[i].p = c->buf[i].p;
        struct v4l2_exportbuffer e = { .type = type(i), .flags = O_CLOEXEC | O_RDWR };
        checked(c->owner, VIDIOC_EXPBUF, &e, "EXPBUF");
        c->buf[i].dmafd = e.fd;
        request(c->fd, i, V4L2_MEMORY_DMABUF);
    }
}
static void raw_file(struct image *im, bool rgb, const char *path, bool write)
{
    FILE *f = fopen(path, write ? "wb" : "rb");
    if (!f) die(path);
    unsigned rows = rgb ? im->height : im->height * 3 / 2;
    unsigned columns = im->width * (rgb ? 4 : 1);
    for (unsigned y = 0; y < rows; y++) {
        size_t n = write ? fwrite(im->p + y * im->stride, 1, columns, f) :
                           fread(im->p + y * im->stride, 1, columns, f);
        if (n != columns) { fputs("short raw image read/write\n", stderr); exit(1); }
    }
    if (!write && fgetc(f) != EOF) { fputs("oversized raw image\n", stderr); exit(1); }
    if (fclose(f)) die("raw image close");
}
int main(int argc, char **argv)
{
    if (argc != 14) {
        fputs("exercise DEVICE WIDTH HEIGHT SRGB DRGB CSC FILTER ALPHA RADIUS THRESHOLD LUT INPUT OUTPUT\n", stderr);
        return 2;
    }
    unsigned w = number(argv[2], 0, UINT_MAX), h = number(argv[3], 0, UINT_MAX);
    unsigned srgb = number(argv[4], 0, UINT_MAX), drgb = number(argv[5], 0, UINT_MAX), csc = number(argv[6], 0, UINT_MAX);
    unsigned filter = number(argv[7], 0, UINT_MAX), alpha = number(argv[8], 0, UINT_MAX), radius = number(argv[9], 0, UINT_MAX);
    unsigned threshold = number(argv[10], 0, UINT_MAX), lut = number(argv[11], 0, UINT_MAX);
    if (w < 16 || w > 1920 || h < 16 || h > 1088 || w % 4 || h % 4 ||
        srgb > 1 || drgb > 1 || csc > 3 || filter > 3 || alpha > 28 || radius > 3 || threshold > 255 || lut > 4)
        return 2;
    struct context c;
    research_open(&c, argv[1], w, h, srgb, drgb, csc, filter, alpha, radius, threshold, lut);
    sync_buffer(&c, 0, true);
    memset(c.image[0].p, 0xa5, c.image[0].size);
    raw_file(&c.image[0], srgb, argv[12], false);
    sync_buffer(&c, 0, false);
    sync_buffer(&c, 1, true);
    memset(c.image[1].p, 0xcc, c.image[1].size);
    sync_buffer(&c, 1, false);
    uint64_t t = now_ns();
    queue(&c, 1); queue(&c, 0);
    for (unsigned i = 0; i < 2; i++) {
        enum v4l2_buf_type tp = type(i);
        checked(c.fd, VIDIOC_STREAMON, &tp, "STREAMON");
    }
    dequeue(&c, 1, 0); dequeue(&c, 0, 0);
    uint64_t elapsed = now_ns() - t;
    sync_buffer(&c, 1, true);
    raw_file(&c.image[1], drgb, argv[13], true);
    sync_buffer(&c, 1, false);
    printf("{\"width\":%u,\"height\":%u,\"srgb\":%u,\"drgb\":%u,\"csc\":%u,"
           "\"filter\":%u,\"alpha\":%u,\"radius\":%u,\"threshold\":%u,\"lut\":%u,"
           "\"src_stride\":%u,\"dst_stride\":%u,\"src_size\":%zu,\"dst_size\":%zu,\"hw_us\":%.3f}\n",
           w,h,srgb,drgb,csc,filter,alpha,radius,threshold,lut,c.image[0].stride,c.image[1].stride,
           c.image[0].size,c.image[1].size,elapsed/1000.0);
    close_context(&c);
    return 0;
}
