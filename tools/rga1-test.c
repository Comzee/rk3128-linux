// SPDX-License-Identifier: MIT
/* Deterministic V4L2 RGA1 verifier. Build: cc -O2 -Wall -Wextra -Werror tools/rga1-test.c -lm -o rga1-test */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
#include <time.h>
#include <unistd.h>
#include "rga1-controls.h"

struct test {
	unsigned sw, sh, dw, dh, angle, flip, matrix, full;
	uint32_t input, output;
	bool dmabuf;
};
struct memory { void *addr; size_t len; int fd; };
static unsigned failures;
static void die(const char *what) { perror(what); exit(1); }
static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
	return r;
}
static void must(int fd, unsigned long req, void *arg, const char *name)
{
	if (xioctl(fd, req, arg) < 0) die(name);
}
static double now_ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock_gettime");
	return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
struct rgb_layout { unsigned bytes, bits[4], shifts[4]; };
static struct rgb_layout layout(uint32_t fmt)
{
 switch (fmt) {
 case V4L2_PIX_FMT_RGB24: return (struct rgb_layout){3,{8,8,8,0},{0,8,16,0}};
 case V4L2_PIX_FMT_BGR24: return (struct rgb_layout){3,{8,8,8,0},{16,8,0,0}};
 case V4L2_PIX_FMT_RGBA32: case V4L2_PIX_FMT_RGBX32: return (struct rgb_layout){4,{8,8,8,8},{0,8,16,24}};
 case V4L2_PIX_FMT_ABGR32: case V4L2_PIX_FMT_XBGR32: return (struct rgb_layout){4,{8,8,8,8},{16,8,0,24}};
 case V4L2_PIX_FMT_ARGB32: case V4L2_PIX_FMT_XRGB32: return (struct rgb_layout){4,{8,8,8,8},{8,16,24,0}};
 case V4L2_PIX_FMT_BGRA32: case V4L2_PIX_FMT_BGRX32: return (struct rgb_layout){4,{8,8,8,8},{24,16,8,0}};
 case V4L2_PIX_FMT_RGB565: return (struct rgb_layout){2,{5,6,5,0},{11,5,0,0}};
 case V4L2_PIX_FMT_ARGB555: case V4L2_PIX_FMT_XRGB555: return (struct rgb_layout){2,{5,5,5,1},{10,5,0,15}};
 case V4L2_PIX_FMT_ABGR555: case V4L2_PIX_FMT_XBGR555: return (struct rgb_layout){2,{5,5,5,1},{0,5,10,15}};
 case V4L2_PIX_FMT_RGBA555: case V4L2_PIX_FMT_RGBX555: return (struct rgb_layout){2,{5,5,5,1},{11,6,1,0}};
 case V4L2_PIX_FMT_BGRA555: case V4L2_PIX_FMT_BGRX555: return (struct rgb_layout){2,{5,5,5,1},{1,6,11,0}};
 case V4L2_PIX_FMT_ARGB444: case V4L2_PIX_FMT_XRGB444: return (struct rgb_layout){2,{4,4,4,4},{8,4,0,12}};
 case V4L2_PIX_FMT_ABGR444: case V4L2_PIX_FMT_XBGR444: return (struct rgb_layout){2,{4,4,4,4},{0,4,8,12}};
 case V4L2_PIX_FMT_RGBA444: case V4L2_PIX_FMT_RGBX444: return (struct rgb_layout){2,{4,4,4,4},{12,8,4,0}};
 case V4L2_PIX_FMT_BGRA444: case V4L2_PIX_FMT_BGRX444: return (struct rgb_layout){2,{4,4,4,4},{4,8,12,0}};
 default: return (struct rgb_layout){0,{0},{0}};
 }
}
static bool yuv(uint32_t f)
{
 return f == V4L2_PIX_FMT_NV12 || f == V4L2_PIX_FMT_NV21 ||
        f == V4L2_PIX_FMT_NV16 || f == V4L2_PIX_FMT_NV61 ||
        f == V4L2_PIX_FMT_YUV420 || f == V4L2_PIX_FMT_YVU420 || f == V4L2_PIX_FMT_YUV422P || f == V4L2_PIX_FMT_YVU422M;
}
static bool y420(uint32_t f) { return f == V4L2_PIX_FMT_NV12 || f == V4L2_PIX_FMT_NV21 || f == V4L2_PIX_FMT_YUV420 || f == V4L2_PIX_FMT_YVU420; }
static bool planar(uint32_t f) { return f == V4L2_PIX_FMT_YUV420 || f == V4L2_PIX_FMT_YVU420 || f == V4L2_PIX_FMT_YUV422P || f == V4L2_PIX_FMT_YVU422M; }
static bool uvswap(uint32_t f) { return f == V4L2_PIX_FMT_NV21 || f == V4L2_PIX_FMT_NV61 || f == V4L2_PIX_FMT_YVU420 || f == V4L2_PIX_FMT_YVU422M; }
static unsigned palette_bits(uint32_t f)
{
 return f == V4L2_PIX_FMT_PAL8 ? 8 : f == V4L2_PIX_FMT_RGA1_PAL1 ? 1 :
        f == V4L2_PIX_FMT_RGA1_PAL2 ? 2 : f == V4L2_PIX_FMT_RGA1_PAL4 ? 4 : 0;
}
static unsigned bpp(uint32_t fmt) { return (yuv(fmt) || palette_bits(fmt)) ? 1 : layout(fmt).bytes; }
static unsigned align4(unsigned n) { return (n + 3) & ~3U; }
static unsigned line_bytes(uint32_t fmt, unsigned w)
{
 unsigned bits = palette_bits(fmt);
 return align4(bits ? (w * bits + 7) / 8 : w * bpp(fmt));
}
static unsigned sizeimage(uint32_t fmt, unsigned w, unsigned h)
{
 unsigned n = line_bytes(fmt, w) * h;
 return yuv(fmt) ? n + (y420(fmt) ? n / 2 : n) : n;
}
static double clamp(double v) { return fmax(0, fmin(255, v)); }
static uint8_t byte(double v) { return (uint8_t)lround(clamp(v)); }
static void convert(double y, double u, double v, const struct test *t, double rgb[3])
{
	double kr = t->matrix == 709 ? 0.2126 : 0.299;
	double kb = t->matrix == 709 ? 0.0722 : 0.114;
	double kg = 1 - kr - kb;
	double yy = t->full ? y : (y - 16) * 255 / 219;
	double uu = (u - 128) * (t->full ? 1 : 255.0 / 224);
	double vv = (v - 128) * (t->full ? 1 : 255.0 / 224);
	rgb[0] = clamp(yy + 2 * (1 - kr) * vv);
	rgb[1] = clamp(yy - 2 * kb * (1 - kb) / kg * uu - 2 * kr * (1 - kr) / kg * vv);
	rgb[2] = clamp(yy + 2 * (1 - kb) * uu);
}
static void rgb_store(uint8_t *p, uint32_t fmt, const double rgb[3])
{
 struct rgb_layout l = layout(fmt);
 uint32_t v = 0;
 for (unsigned c = 0; c < 3; c++)
  v |= ((unsigned)byte(rgb[c]) >> (8 - l.bits[c])) << l.shifts[c];
 if (l.bits[3]) v |= ((1U << l.bits[3]) - 1) << l.shifts[3];
 memcpy(p, &v, l.bytes);
}
static void rgb_read(const uint8_t *p, uint32_t fmt, double rgb[3])
{
 struct rgb_layout l = layout(fmt);
 uint32_t v = 0;
 memcpy(&v, p, l.bytes);
 for (unsigned c = 0; c < 3; c++)
  rgb[c] = ((v >> l.shifts[c]) & ((1U << l.bits[c]) - 1)) << (8 - l.bits[c]);
}
static size_t chroma_offset(uint32_t fmt, unsigned stride, unsigned h, unsigned x, unsigned y, unsigned c)
{
 unsigned cy = y420(fmt) ? y / 2 : y;
 unsigned ch = y420(fmt) ? h / 2 : h;
 c ^= uvswap(fmt);
 return stride * h + (planar(fmt) ? c * (stride / 2) * ch + cy * (stride / 2) + x / 2 : cy * stride + (x & ~1U) + c);
}
/* A smooth, asymmetric photo-sized field with useful luma and chroma gradients.
 * Every pixel is checked; no skipped borders or acceptance based on averages alone. */
static void generate(uint8_t *raw, const struct test *t)
{
	unsigned stride = align4(t->sw * bpp(t->input));
	double *fxs = malloc(t->sw * sizeof(*fxs)), *sines = malloc(t->sw * sizeof(*sines));
	if (!fxs || !sines) die("malloc");
	for (unsigned x = 0; x < t->sw; x++) {
		fxs[x] = (double)x / (t->sw - 1);
		sines[x] = sin(fxs[x] * 12.0);
	}
	memset(raw, 0, sizeimage(t->input, t->sw, t->sh));
	for (unsigned y = 0; y < t->sh; y++) {
		double fy = (double)y / (t->sh - 1), cosine = cos(fy * 9.0);
		for (unsigned x = 0; x < t->sw; x++) {
			double fx = fxs[x];
			if (yuv(t->input)) {
				raw[y * stride + x] = byte(48 + 100 * fx + 50 * fy + 10 * sines[x] * cosine);
			} else {
				double rgb[3] = { 35 + 180 * fx, 25 + 140 * fy, 50 + 80 * fx + 70 * fy };
				rgb_store(raw + y * stride + x * bpp(t->input), t->input, rgb);
			}
		}
	}
	free(fxs); free(sines);
 if (yuv(t->input)) {
  unsigned ch = y420(t->input) ? t->sh / 2 : t->sh;
  for (unsigned y = 0; y < ch; y++) for (unsigned x = 0; x < t->sw; x += 2) {
   unsigned ly = y * (y420(t->input) ? 2 : 1);
   raw[chroma_offset(t->input, stride, t->sh, x, ly, 0)] = byte(96 + 52.0 * y / (ch - 1));
   raw[chroma_offset(t->input, stride, t->sh, x, ly, 1)] = byte(108 + 44.0 * x / (t->sw - 2));
  }
 }

}
static unsigned preshift(unsigned src, unsigned dst)
{
	unsigned n = 0;
	while (n < 3 && ((src >> n) >= 2 * dst - 1)) n++;
	return n;
}
static uint8_t *prescale(const uint8_t *src, struct test *t)
{
	unsigned uw = t->angle % 180 ? t->dh : t->dw;
	unsigned uh = t->angle % 180 ? t->dw : t->dh;
	unsigned hx = preshift(t->sw, uw), vy = preshift(t->sh, uh);
	if (hx || vy) {
		while ((t->sw >> hx) > 2048 && hx < 3) hx++;
		while ((t->sh >> vy) > 2048 && vy < 3) vy++;
	}
	unsigned ax = 1U << hx, ay = 1U << vy;
	unsigned nw = t->sw / ax, nh = t->sh / ay;
	unsigned stride = align4(t->sw * bpp(t->input)), ns = align4(nw * bpp(t->input));
	uint8_t *dst = calloc(1, sizeimage(t->input, nw, nh));
	if (!dst) die("calloc");
	if (!hx && !vy) {
		memcpy(dst, src, sizeimage(t->input, t->sw, t->sh));
		return dst;
	}
	for (unsigned y = 0; y < nh; y++) {
		for (unsigned x = 0; x < nw; x++) {
			if (yuv(t->input)) {
				unsigned sum = 0;
				for (unsigned dy = 0; dy < ay; dy++) for (unsigned dx = 0; dx < ax; dx++)
					sum += src[(y * ay + dy) * stride + x * ax + dx];
				dst[y * ns + x] = (sum + ax * ay / 2) / (ax * ay);
			} else {
				double sum[3] = {0};
				for (unsigned dy = 0; dy < ay; dy++) for (unsigned dx = 0; dx < ax; dx++) {
					double rgb[3];
					rgb_read(src + (y * ay + dy) * stride + (x * ax + dx) * bpp(t->input), t->input, rgb);
					for (unsigned c = 0; c < 3; c++) sum[c] += rgb[c] / (ax * ay);
				}
				rgb_store(dst + y * ns + x * bpp(t->input), t->input, sum);
			}
		}
	} if (yuv(t->input)) {
  unsigned ch = y420(t->input) ? nh / 2 : nh;
  for (unsigned y = 0; y < ch; y++) for (unsigned x = 0; x < nw; x += 2) {
   for (unsigned c = 0; c < 2; c++) {
    unsigned sum = 0;
    for (unsigned dy = 0; dy < ay; dy++) for (unsigned dx = 0; dx < ax; dx++) {
     unsigned sy = (y * ay + dy) * (y420(t->input) ? 2 : 1);
     sum += src[chroma_offset(t->input, stride, t->sh, x * ax + 2 * dx, sy, c)];
    }
    dst[chroma_offset(t->input, ns, nh, x, y * (y420(t->input) ? 2 : 1), c)] = (sum + ax * ay / 2) / (ax * ay);
   }
  }
 }

	t->sw = nw; t->sh = nh;
	return dst;
}
static void source_pixel(const uint8_t *src, const struct test *t, unsigned x, unsigned y, double rgb[3])
{
	unsigned stride = align4(t->sw * bpp(t->input));
	if (!yuv(t->input)) {
		rgb_read(src + y * stride + x * bpp(t->input), t->input, rgb);
		return;
	}
 unsigned u = src[chroma_offset(t->input, stride, t->sh, x, y, 0)];
 unsigned v = src[chroma_offset(t->input, stride, t->sh, x, y, 1)];
 convert(src[y * stride + x], u, v, t, rgb);
}
static void reference(const uint8_t *src, uint8_t *dst, const struct test *orig)
{
	struct test t = *orig;
	unsigned uw = t.angle % 180 ? t.dh : t.dw, uh = t.angle % 180 ? t.dw : t.dh;
	uint8_t *mid = prescale(src, &t);
	double xs = (double)(((t.sw - 1) * 65536U) / (uw - 1)) / 65536;
	double ys = (double)(((t.sh - 1) * 65536U) / (uh - 1)) / 65536;
	unsigned stride = align4(t.dw * bpp(t.output));
	for (unsigned y = 0; y < t.dh; y++) for (unsigned x = 0; x < t.dw; x++) {
		unsigned ox = t.flip & 1 ? t.dw - 1 - x : x;
		unsigned oy = t.flip & 2 ? t.dh - 1 - y : y;
		double sx, sy;
		switch (t.angle) {
		case 90: sx = oy * xs; sy = (t.dw - 1 - ox) * ys; break;
		case 180: sx = (t.dw - 1 - ox) * xs; sy = (t.dh - 1 - oy) * ys; break;
		case 270: sx = (t.dh - 1 - oy) * xs; sy = ox * ys; break;
		default: sx = ox * xs; sy = oy * ys; break;
		}
		unsigned x0 = (unsigned)sx, y0 = (unsigned)sy;
		unsigned x1 = x0 + 1 < t.sw ? x0 + 1 : x0, y1 = y0 + 1 < t.sh ? y0 + 1 : y0;
		double fracx = sx - x0, fracy = sy - y0, p[4][3], rgb[3];
		source_pixel(mid, &t, x0, y0, p[0]); source_pixel(mid, &t, x1, y0, p[1]);
		source_pixel(mid, &t, x0, y1, p[2]); source_pixel(mid, &t, x1, y1, p[3]);
		for (unsigned c = 0; c < 3; c++)
			rgb[c] = (p[0][c] * (1 - fracx) + p[1][c] * fracx) * (1 - fracy) +
				 (p[2][c] * (1 - fracx) + p[3][c] * fracx) * fracy;
		rgb_store(dst + y * stride + x * bpp(t.output), t.output, rgb);
	}
	free(mid);
}
static void set_fmt(int fd, enum v4l2_buf_type type, const struct test *t)
{
	struct v4l2_format f = { .type = type };
	bool out = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	f.fmt.pix_mp.width = out ? t->sw : t->dw;
	f.fmt.pix_mp.height = out ? t->sh : t->dh;
	f.fmt.pix_mp.pixelformat = out ? t->input : t->output;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.colorspace = t->matrix == 709 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M;
	f.fmt.pix_mp.ycbcr_enc = t->matrix == 709 ? V4L2_YCBCR_ENC_709 : V4L2_YCBCR_ENC_601;
	f.fmt.pix_mp.flags = out ? 0 : V4L2_PIX_FMT_FLAG_SET_CSC;
	f.fmt.pix_mp.quantization = (out ? (!yuv(t->input) || t->full) : (!yuv(t->output) || t->full)) ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE;
	must(fd, VIDIOC_S_FMT, &f, "VIDIOC_S_FMT");
	if (f.fmt.pix_mp.width != (out ? t->sw : t->dw) || f.fmt.pix_mp.height != (out ? t->sh : t->dh) ||
	    f.fmt.pix_mp.pixelformat != (out ? t->input : t->output) || f.fmt.pix_mp.num_planes != 1 ||
	    f.fmt.pix_mp.plane_fmt[0].bytesperline != line_bytes(out ? t->input : t->output, out ? t->sw : t->dw) ||
	    (out && yuv(t->input) && f.fmt.pix_mp.quantization != (t->full ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE))) {
		fprintf(stderr, "driver adjusted required dimensions/format/colorimetry\n"); exit(1);
	}
}
/* Check the ABI restriction as well as positive pixel cases. */
static void check_formats(const char *node)
{
	const uint32_t inputs[] = {
 V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_BGR24, V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV16,
 V4L2_PIX_FMT_RGBA32,V4L2_PIX_FMT_RGBX32,V4L2_PIX_FMT_ABGR32,V4L2_PIX_FMT_XBGR32,V4L2_PIX_FMT_ARGB32,V4L2_PIX_FMT_XRGB32,V4L2_PIX_FMT_BGRA32,V4L2_PIX_FMT_BGRX32,
 V4L2_PIX_FMT_RGBA555,V4L2_PIX_FMT_RGBX555,V4L2_PIX_FMT_BGRA555,V4L2_PIX_FMT_BGRX555,
 V4L2_PIX_FMT_ARGB444,V4L2_PIX_FMT_XRGB444,V4L2_PIX_FMT_ABGR444,V4L2_PIX_FMT_XBGR444,V4L2_PIX_FMT_RGBA444,V4L2_PIX_FMT_RGBX444,V4L2_PIX_FMT_BGRA444,V4L2_PIX_FMT_BGRX444,
 V4L2_PIX_FMT_NV21,V4L2_PIX_FMT_NV61,V4L2_PIX_FMT_YUV420,V4L2_PIX_FMT_YVU420,V4L2_PIX_FMT_YUV422P, V4L2_PIX_FMT_YVU422M, V4L2_PIX_FMT_RGA1_PAL1, V4L2_PIX_FMT_RGA1_PAL2, V4L2_PIX_FMT_RGA1_PAL4 };
 uint32_t outputs[sizeof(inputs) / sizeof(inputs[0])];
 unsigned output_count = 0;
 for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); i++)
  if (!planar(inputs[i]) && !palette_bits(inputs[i]) &&
      inputs[i] != V4L2_PIX_FMT_ARGB444 && inputs[i] != V4L2_PIX_FMT_XRGB444 &&
      inputs[i] != V4L2_PIX_FMT_ABGR444 && inputs[i] != V4L2_PIX_FMT_XBGR444) outputs[output_count++] = inputs[i];
	int fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) die("open format check");
	for (unsigned side = 0; side < 2; side++) {
		const uint32_t *expected = side ? outputs : inputs;
		unsigned count = side ? output_count : sizeof(inputs) / sizeof(inputs[0]);
		struct v4l2_fmtdesc e = { .type = side ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
		for (e.index = 0; e.index < count; e.index++) {
			must(fd, VIDIOC_ENUM_FMT, &e, "VIDIOC_ENUM_FMT");
			if (e.pixelformat != expected[e.index]) { fprintf(stderr, "unexpected advertised format\n"); exit(1); }
		}
		if (xioctl(fd, VIDIOC_ENUM_FMT, &e) != -1 || errno != EINVAL) { fprintf(stderr, "unexpected extra format\n"); exit(1); }
	}
 close(fd);
 fprintf(stderr, "formats: %zu inputs, %u outputs PASS\n", sizeof(inputs)/sizeof(inputs[0]), output_count);

}
static void configure(int fd, const struct test *t)
{
	struct v4l2_control c = { .id = V4L2_CID_ROTATE, .value = (int)t->angle };
	set_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, t);
	set_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, t);
	must(fd, VIDIOC_S_CTRL, &c, "rotate");
	c.id = V4L2_CID_HFLIP; c.value = t->flip & 1; must(fd, VIDIOC_S_CTRL, &c, "hflip");
	c.id = V4L2_CID_VFLIP; c.value = !!(t->flip & 2); must(fd, VIDIOC_S_CTRL, &c, "vflip");
}
static struct memory alloc_mmap(int fd, enum v4l2_buf_type type)
{
	struct v4l2_requestbuffers r = { .type = type, .memory = V4L2_MEMORY_MMAP, .count = 1 };
	struct v4l2_plane plane = {0};
	struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &plane };
	struct v4l2_exportbuffer e = { .type = type, .flags = O_RDWR | O_CLOEXEC };
	struct memory m;
	must(fd, VIDIOC_REQBUFS, &r, "VIDIOC_REQBUFS MMAP");
	must(fd, VIDIOC_QUERYBUF, &b, "VIDIOC_QUERYBUF");
	m.len = plane.length;
	m.addr = mmap(NULL, m.len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
	if (m.addr == MAP_FAILED) die("mmap");
	must(fd, VIDIOC_EXPBUF, &e, "VIDIOC_EXPBUF"); m.fd = e.fd;
	return m;
}
static void sync_mem(const struct memory *m, uint64_t flags)
{
	struct dma_buf_sync s = { .flags = flags };
	must(m->fd, DMA_BUF_IOCTL_SYNC, &s, "DMA_BUF_IOCTL_SYNC");
}
static void queue(int fd, enum v4l2_buf_type type, const struct memory *m, const struct test *t)
{
	bool out = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	struct v4l2_plane p = { .length = (unsigned)m->len };
	struct v4l2_buffer b = { .type = type, .length = 1, .m.planes = &p,
		.memory = t->dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP, .field = V4L2_FIELD_NONE };
	if (t->dmabuf) {
		struct v4l2_requestbuffers r = { .type = type, .memory = V4L2_MEMORY_DMABUF, .count = 1 };
		must(fd, VIDIOC_REQBUFS, &r, "VIDIOC_REQBUFS DMABUF");
		p.m.fd = m->fd;
	}
	if (out) {
		p.bytesused = sizeimage(t->input, t->sw, t->sh);
		b.timestamp.tv_sec = 1;
	}
	must(fd, VIDIOC_QBUF, &b, "VIDIOC_QBUF");
}
static void dequeue(int fd, enum v4l2_buf_type type, bool dmabuf)
{
	struct v4l2_plane p = {0};
	struct v4l2_buffer b = { .type = type, .length = 1, .m.planes = &p,
		.memory = dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP };
	struct pollfd pollfd = { .fd = fd, .events = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? POLLOUT : POLLIN };
	double deadline = now_ms() + 3000;
	for (;;) {
		if (xioctl(fd, VIDIOC_DQBUF, &b) == 0) break;
		if (errno != EAGAIN) die("VIDIOC_DQBUF");
		if (now_ms() >= deadline || poll(&pollfd, 1, (int)(deadline - now_ms()) + 1) <= 0) { fprintf(stderr, "buffer timeout\n"); exit(1); }
	}
	if (b.flags & V4L2_BUF_FLAG_ERROR) { fprintf(stderr, "hardware returned ERROR buffer\n"); exit(1); }
}
static bool compare(const uint8_t *actual, const uint8_t *expected, const struct test *t,
		    double *mean, double *rmse, unsigned *maxerr)
{
	double sum = 0, square = 0;
	unsigned stride = align4(t->dw * bpp(t->output));
	*maxerr = 0;
	for (unsigned y = 0; y < t->dh; y++) for (unsigned x = 0; x < t->dw; x++) {
		double a[3], e[3];
		rgb_read(actual + y * stride + x * bpp(t->output), t->output, a);
		rgb_read(expected + y * stride + x * bpp(t->output), t->output, e);
		for (unsigned c = 0; c < 3; c++) {
			unsigned d = (unsigned)ceil(fabs(a[c] - e[c]));
			sum += d; square += d * d;
			if (d > *maxerr) *maxerr = d;
		}
	}
	*mean = sum / ((double)t->dw * t->dh * 3);
	*rmse = sqrt(square / ((double)t->dw * t->dh * 3));
	/* Fixed before hardware testing. RGB565 includes quantization. */
	return *maxerr <= ((t->input == V4L2_PIX_FMT_RGB565 || t->output == V4L2_PIX_FMT_RGB565) ? 10U : (bpp(t->input) == 2 || bpp(t->output) == 2) ? 16U : 6U) && *mean <= 2.0;
}
static void run(const char *device, const struct test *t)
{
	enum v4l2_buf_type output = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	int fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC), donor;
	if (fd < 0) die(device);
	configure(fd, t);
	donor = t->dmabuf ? open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC) : fd;
	if (donor < 0) die("donor open");
	if (t->dmabuf) configure(donor, t);
	struct memory src = alloc_mmap(donor, output), dst = alloc_mmap(donor, capture);
	uint8_t *raw = malloc(sizeimage(t->input, t->sw, t->sh));
	uint8_t *ref = calloc(1, sizeimage(t->output, t->dw, t->dh));
	uint8_t *actual = malloc(sizeimage(t->output, t->dw, t->dh));
	if (!raw || !ref || !actual) die("malloc");
	generate(raw, t);
	double upload_start = now_ms();
	sync_mem(&src, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
	memcpy(src.addr, raw, sizeimage(t->input, t->sw, t->sh));
	sync_mem(&src, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
	double upload = now_ms() - upload_start;
	sync_mem(&dst, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE); memset(dst.addr, 0xa5, dst.len);
	sync_mem(&dst, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
	queue(fd, output, &src, t); queue(fd, capture, &dst, t);
	double start = now_ms();
	must(fd, VIDIOC_STREAMON, &output, "STREAMON output");
	must(fd, VIDIOC_STREAMON, &capture, "STREAMON capture");
	dequeue(fd, output, t->dmabuf); dequeue(fd, capture, t->dmabuf);
	double hardware = now_ms() - start;
	must(fd, VIDIOC_STREAMOFF, &output, "STREAMOFF output");
	must(fd, VIDIOC_STREAMOFF, &capture, "STREAMOFF capture");
	start = now_ms();
	sync_mem(&dst, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
	memcpy(actual, dst.addr, sizeimage(t->output, t->dw, t->dh));
	sync_mem(&dst, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
	double readback = now_ms() - start;
	start = now_ms(); reference(raw, ref, t); double cpu = now_ms() - start;
	double mean, rmse; unsigned maxerr;
	bool pass = compare(actual, ref, t, &mean, &rmse, &maxerr);
	printf("{\"input\":\"%.4s\",\"output\":\"%.4s\",\"source\":[%u,%u],\"destination\":[%u,%u],\"rotate\":%u,\"flip\":%u,\"matrix\":%u,\"full\":%u,\"dmabuf\":%s,\"hardware_ms\":%.3f,\"upload_ms\":%.3f,\"readback_ms\":%.3f,\"total_cpu_visible_ms\":%.3f,\"cpu_reference_ms\":%.3f,\"max_error\":%u,\"mean_error\":%.4f,\"rmse\":%.4f,\"pass\":%s}\n",
	       (const char *)&t->input, (const char *)&t->output, t->sw, t->sh, t->dw, t->dh,
	       t->angle, t->flip, t->matrix, t->full, t->dmabuf ? "true" : "false", hardware, upload, readback, upload + hardware + readback, cpu,
	       maxerr, mean, rmse, pass ? "true" : "false"); fflush(stdout);
	if (!pass) failures++;
	free(raw); free(ref); free(actual);
	munmap(src.addr, src.len); munmap(dst.addr, dst.len); close(src.fd); close(dst.fd);
	if (t->dmabuf) close(donor);
	close(fd);
}
static void selftest(void)
{
	struct test t = { .sw = 32, .sh = 24, .dw = 24, .dh = 32,
		.input = V4L2_PIX_FMT_RGB24, .output = V4L2_PIX_FMT_RGB24, .matrix = 601 };
	uint8_t raw[32 * 24 * 3], ref[32 * 24 * 3];
	generate(raw, &t);
	for (t.angle = 0; t.angle <= 270; t.angle += 90) for (t.flip = 0; t.flip < 4; t.flip++) {
		t.dw = t.angle % 180 ? 24 : 32; t.dh = t.angle % 180 ? 32 : 24;
		reference(raw, ref, &t);
		for (unsigned y = 0; y < t.dh; y++) for (unsigned x = 0; x < t.dw; x++) {
			unsigned ox = t.flip & 1 ? t.dw - 1 - x : x, oy = t.flip & 2 ? t.dh - 1 - y : y;
			unsigned sx = 0, sy = 0;
			switch (t.angle) {
			case 0: sx = ox; sy = oy; break;
			case 90: sx = oy; sy = t.sh - 1 - ox; break;
			case 180: sx = t.sw - 1 - ox; sy = t.sh - 1 - oy; break;
			case 270: sx = t.sw - 1 - oy; sy = ox; break;
			}
			if (memcmp(ref + (y * t.dw + x) * 3, raw + (sy * t.sw + sx) * 3, 3)) {
				fprintf(stderr, "CPU transform selftest failed\n"); exit(1);
			}
		}
	}
	double rgb[3]; t.full = 0; convert(16, 128, 128, &t, rgb);
	if (rgb[0] != 0 || rgb[1] != 0 || rgb[2] != 0) exit(1);
	convert(235, 128, 128, &t, rgb);
	if (byte(rgb[0]) != 255 || byte(rgb[1]) != 255 || byte(rgb[2]) != 255) exit(1);
	puts("CPU reference: 16 exact rotations/reflections and limited-range black/white PASS");
}
#ifdef RGA1_GPU
/* Separate, offscreen GLES2 path for the same input. Includes CPU upload and
 * readback, so its timing has the same CPU-visible destination as V4L2. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
static GLuint shader(GLenum type, const char *source)
{
	GLuint s = glCreateShader(type);
	GLint ok;
	glShaderSource(s, 1, &source, NULL); glCompileShader(s); glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof(log), NULL, log); fprintf(stderr, "%s\n", log); exit(1); }
	return s;
}
static void gpu_run(void)
{
	struct test t = { .sw = 2560, .sh = 1440, .dw = 1920, .dh = 1080,
		.input = V4L2_PIX_FMT_NV12, .output = V4L2_PIX_FMT_RGB24, .matrix = 601 };
	uint8_t *raw = malloc(sizeimage(t.input, t.sw, t.sh));
	uint8_t *ref = malloc(sizeimage(t.output, t.dw, t.dh));
	uint8_t *rgba = malloc((size_t)t.dw * t.dh * 4), *rgb = malloc((size_t)t.dw * t.dh * 3);
	if (!raw || !ref || !rgba || !rgb) die("malloc");
	generate(raw, &t);
	double start = now_ms(); reference(raw, ref, &t); double cpu = now_ms() - start;
	int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (fd < 0) die("renderD128");
	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); exit(1); }
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_display) { fprintf(stderr, "EGL platform entry missing\n"); exit(1); }
	EGLDisplay dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	if (!eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) { fprintf(stderr, "eglInitialize failed\n"); exit(1); }
	EGLint attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE, 0, EGL_NONE };
	EGLConfig cfg; EGLint count;
	if (!eglChooseConfig(dpy, attrs, &cfg, 1, &count) || !count) { fprintf(stderr, "EGL config missing\n"); exit(1); }
	EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { fprintf(stderr, "EGL context failed\n"); exit(1); }
	const char *vs = "attribute vec2 position; void main(){gl_Position=vec4(position,0.,1.);}";
	const char *fs = "precision mediump float; uniform sampler2D lum, chroma;"
		"void main(){vec2 p=(gl_FragCoord.xy-vec2(.5))/vec2(1919.,1079.);"
		"vec2 tc=(p*vec2(2559.,1439.)+vec2(.5))/vec2(2560.,1440.);"
		"float y=(texture2D(lum,tc).r-16./255.)*255./219.;"
		"vec4 uv=texture2D(chroma,tc); float u=(uv.r-128./255.)*255./224.;"
		"float v=(uv.a-128./255.)*255./224.;"
		"gl_FragColor=vec4(y+1.402*v,y-.344136*u-.714136*v,y+1.772*u,1.);}";
	GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs), prog = glCreateProgram();
	glAttachShader(prog, v); glAttachShader(prog, f); glBindAttribLocation(prog, 0, "position"); glLinkProgram(prog);
	GLint linked; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) { fprintf(stderr, "GL link failed\n"); exit(1); }
	glUseProgram(prog); glUniform1i(glGetUniformLocation(prog, "lum"), 0); glUniform1i(glGetUniformLocation(prog, "chroma"), 1);
	GLuint tex[3], fbo;
	glGenTextures(3, tex);
	for (unsigned i = 0; i < 3; i++) {
		glActiveTexture(GL_TEXTURE0 + (i == 1 ? 1 : 0)); glBindTexture(GL_TEXTURE_2D, tex[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, i == 0 ? GL_LINEAR : GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, i == 0 ? GL_LINEAR : GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_2D, tex[2]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.dw, t.dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[2], 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "GL FBO incomplete\n"); exit(1); }
	const GLfloat vertices[] = {-1,-1, 1,-1, -1,1, 1,1};
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices); glEnableVertexAttribArray(0); glViewport(0, 0, t.dw, t.dh);
	for (unsigned pass = 0; pass < 3; pass++) {
		start = now_ms();
		glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex[0]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, t.sw, t.sh, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, raw);
		glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tex[1]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, t.sw / 2, t.sh / 2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, raw + t.sw * t.sh);
		double uploaded = now_ms(); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glFinish(); double drawn = now_ms();
		glReadPixels(0, 0, t.dw, t.dh, GL_RGBA, GL_UNSIGNED_BYTE, rgba); double read = now_ms();
		if (glGetError() != GL_NO_ERROR) { fprintf(stderr, "GL job error\n"); exit(1); }
		for (unsigned i = 0; i < t.dw * t.dh; i++) memcpy(rgb + i * 3, rgba + i * 4, 3);
		double mean, rmse; unsigned maxerr; bool ok = compare(rgb, ref, &t, &mean, &rmse, &maxerr);
		printf("{\"path\":\"GLES2 NV12 upload/scale/CSC/readback\",\"pass_index\":%u,\"upload_ms\":%.3f,\"draw_ms\":%.3f,\"readback_ms\":%.3f,\"total_ms\":%.3f,\"cpu_reference_ms\":%.3f,\"max_error\":%u,\"mean_error\":%.4f,\"pass\":%s}\n",
			pass, uploaded-start, drawn-uploaded, read-drawn, read-start, cpu, maxerr, mean, ok?"true":"false");
		if (!ok) failures++;
	}
	glDeleteFramebuffers(1, &fbo); glDeleteTextures(3, tex); glDeleteProgram(prog); glDeleteShader(v); glDeleteShader(f);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroyContext(dpy, ctx); eglTerminate(dpy);
	gbm_device_destroy(gbm); close(fd); free(raw); free(ref); free(rgba); free(rgb);
}
#endif

int main(int argc, char **argv)
{
#ifdef RGA1_GPU
	if (argc == 2 && !strcmp(argv[1], "--gpu")) { gpu_run(); return failures ? 1 : 0; }
#endif
	if (argc == 2 && !strcmp(argv[1], "--selftest")) { selftest(); return 0; }
	if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--suite") && strcmp(argv[2], "--dmabuf"))) {
		fprintf(stderr, "usage: %s --selftest | /dev/videoN [--suite|--dmabuf]\n", argv[0]); return 2;
	}
	struct test t = { .sw = 2560, .sh = 1440, .dw = 1920, .dh = 1080,
		.input = V4L2_PIX_FMT_NV12, .output = V4L2_PIX_FMT_RGB24, .matrix = 601 };
	if (argc == 3 && !strcmp(argv[2], "--dmabuf")) t.dmabuf = true;
	if (argc == 3 && !strcmp(argv[2], "--suite")) {
		t.sw = 640; t.sh = 480; t.dw = 480; t.dh = 320;
		check_formats(argv[1]);
		for (unsigned fmt = 0; fmt < 4; fmt++) {
			t.input = fmt == 0 ? V4L2_PIX_FMT_NV12 : fmt == 1 ? V4L2_PIX_FMT_NV16 : fmt == 2 ? V4L2_PIX_FMT_RGB24 :
			V4L2_PIX_FMT_BGR24;
			for (t.angle = 0; t.angle <= 270; t.angle += 90)
				for (t.flip = 0; t.flip < 4; t.flip++) run(argv[1], &t);
		}
		/* Cover every transform into RGB565 as well. */
		t.input = V4L2_PIX_FMT_RGB24; t.output = V4L2_PIX_FMT_RGB565;
		for (t.angle = 0; t.angle <= 270; t.angle += 90)
			for (t.flip = 0; t.flip < 4; t.flip++) run(argv[1], &t);
		t.output = V4L2_PIX_FMT_RGB24;
		t.sw = 2560; t.sh = 1440; t.dw = 1920; t.dh = 1080;
		t.input = V4L2_PIX_FMT_NV12; t.flip = 0;
		for (t.angle = 0; t.angle <= 270; t.angle += 90) run(argv[1], &t);
		t.angle = 0;
		t.full = 1; run(argv[1], &t); t.full = 0;
		t.matrix = 709; run(argv[1], &t); t.matrix = 601;
		t.output = V4L2_PIX_FMT_BGR24; run(argv[1], &t);
		t.output = V4L2_PIX_FMT_RGB565; run(argv[1], &t);
		t.output = V4L2_PIX_FMT_RGB24; t.dmabuf = true; run(argv[1], &t);
		t.sw = 3840; t.sh = 2160; run(argv[1], &t);
        /* Partial tiles, padded strides and exact maximum endpoint enlargement. */
        t.dmabuf = false; t.sw = 638; t.sh = 478; t.dw = 479; t.dh = 317; run(argv[1], &t);
        t.input = V4L2_PIX_FMT_RGB24; t.sw = t.sh = 2; t.dw = t.dh = 9; run(argv[1], &t);
        t.sw = t.dw = 640; t.sh = t.dh = 480; run(argv[1], &t);
        t.output = V4L2_PIX_FMT_RGB565; run(argv[1], &t);
        t.input = V4L2_PIX_FMT_NV12; t.output = V4L2_PIX_FMT_RGB24;
        t.sw = t.sh = 2048; t.dw = t.dh = 320; run(argv[1], &t); /* /4 prescale */
        t.dw = t.dh = 160; run(argv[1], &t); /* /8 prescale */
	} else run(argv[1], &t);
	return failures ? 1 : 0;
}
