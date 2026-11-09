/* fbtest: draw a test pattern on /dev/fb0 (or argv[1]) and print what the framebuffer is.
 * Eight colour bars over the top 3/4, a grey ramp under them, a white 4 px frame and a
 * red/green/blue/white square in the four corners (orientation). Works for 16/24/32 bpp
 * true colour by reading the bit fields the driver reports; 8 bpp is reported and skipped.
 * Static armhf binary built by scripts/linux-build.sh; runs from the test initramfs.
 *   fbtest <dev> target X Y [X Y ...]   instead: a black screen with a touch target (a green square with a white
 *                                       centre, 1/6 of the screen height) centred at each X% Y% of the screen
 *                                       (#357's guided touch test: the owner touches what the panel shows)
 */
#include <fcntl.h>
#include <stdlib.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static struct fb_var_screeninfo v;
static struct fb_fix_screeninfo f;
static uint8_t *fb;

static uint32_t pack(unsigned r, unsigned g, unsigned b)
{
	return ((r >> (8 - v.red.length)) << v.red.offset) |
	       ((g >> (8 - v.green.length)) << v.green.offset) |
	       ((b >> (8 - v.blue.length)) << v.blue.offset);
}

static void put(unsigned x, unsigned y, uint32_t px)
{
	uint8_t *p = fb + (size_t)(y + v.yoffset) * f.line_length + (size_t)(x + v.xoffset) * (v.bits_per_pixel / 8);

	switch (v.bits_per_pixel) {
	case 16: *(uint16_t *)p = px; break;
	case 24: p[0] = px; p[1] = px >> 8; p[2] = px >> 16; break;
	case 32: *(uint32_t *)p = px; break;
	}
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
	int fd = open(dev, O_RDWR);
	unsigned x, y;
	static const uint8_t bar[8][3] = {
		{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
		{255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0},
	};

	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
		printf("fbtest: cannot open or query %s\n", dev);
		return 1;
	}
	printf("fbtest: %s id=%s %ux%u (virtual %ux%u) %u bpp stride %u smem 0x%lx len %u "
	       "R%u/%u G%u/%u B%u/%u A%u/%u\n", dev, f.id, v.xres, v.yres, v.xres_virtual, v.yres_virtual,
	       v.bits_per_pixel, f.line_length, f.smem_start, f.smem_len,
	       v.red.offset, v.red.length, v.green.offset, v.green.length,
	       v.blue.offset, v.blue.length, v.transp.offset, v.transp.length);
	if (v.bits_per_pixel != 16 && v.bits_per_pixel != 24 && v.bits_per_pixel != 32) {
		printf("fbtest: %u bpp not drawn\n", v.bits_per_pixel);
		return 2;
	}
	fb = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		printf("fbtest: mmap failed\n");
		return 3;
	}
	if (argc > 4 && !strcmp(argv[2], "target")) {
		unsigned half = v.yres / 12, dot = half / 4;

		for (y = 0; y < v.yres; y++)
			for (x = 0; x < v.xres; x++)
				put(x, y, 0);
		for (int a = 3; a + 1 < argc; a += 2) {
			unsigned cx = (unsigned)atoi(argv[a]) * (v.xres - 1) / 100, cy = (unsigned)atoi(argv[a + 1]) * (v.yres - 1) / 100;

			cx = cx < half ? half : cx > v.xres - 1 - half ? v.xres - 1 - half : cx;
			cy = cy < half ? half : cy > v.yres - 1 - half ? v.yres - 1 - half : cy;
			for (y = cy - half; y <= cy + half; y++)
				for (x = cx - half; x <= cx + half; x++) {
					int in = x + dot >= cx && x <= cx + dot && y + dot >= cy && y <= cy + dot;

					put(x, y, in ? pack(255, 255, 255) : pack(0, 200, 0));
				}
			printf("fbtest: target at %u,%u of %ux%u (%s%% %s%%), %u px square\n", cx, cy, v.xres, v.yres, argv[a],
			       argv[a + 1], 2 * half + 1);
		}
		munmap(fb, f.smem_len);
		return 0;
	}
	for (y = 0; y < v.yres; y++) {
		for (x = 0; x < v.xres; x++) {
			uint32_t px;

			if (y < v.yres * 3 / 4) {
				const uint8_t *c = bar[x * 8 / v.xres];

				px = pack(c[0], c[1], c[2]);
			} else {
				unsigned g = x * 255 / (v.xres - 1);

				px = pack(g, g, g);
			}
			if (x < 4 || y < 4 || x >= v.xres - 4 || y >= v.yres - 4)
				px = pack(255, 255, 255);
			put(x, y, px);
		}
	}
	for (y = 8; y < 56; y++) {
		for (x = 8; x < 56; x++) {
			put(x, y, pack(255, 0, 0));				/* top left: red */
			put(v.xres - 1 - x, y, pack(0, 255, 0));		/* top right: green */
			put(x, v.yres - 1 - y, pack(0, 0, 255));		/* bottom left: blue */
			put(v.xres - 1 - x, v.yres - 1 - y, pack(255, 255, 255)); /* bottom right: white */
		}
	}
	munmap(fb, f.smem_len);
	printf("fbtest: drawn\n");
	return 0;
}
