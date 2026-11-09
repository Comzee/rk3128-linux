/* #587: execute the production kernel helper, never a copy of its algorithms. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "silead_rk3128_reference.h"
#include "touch_gesture.h"

static struct silead_rk3128_reference state;

static void put32(unsigned int value)
{
	unsigned char bytes[4] = {value, value >> 8, value >> 16, value >> 24};
	assert(fwrite(bytes, 1, sizeof(bytes), stdout) == sizeof(bytes));
}

static unsigned int get32(const unsigned char *p)
{
	return (unsigned int)p[0] | (unsigned int)p[1] << 8 |
	       (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}

/* Compile the actual added driver entry point against a tiny input sink. */
typedef unsigned char u8;
struct input_mt_pos { int x, y; };
struct input_dev { unsigned int seen, active; int slot, syncs, x[10], y[10]; };
struct device { int unused; };
struct i2c_client { struct device dev; };
struct touchscreen_properties { int unused; };
struct silead_ts_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct silead_rk3128_reference *reference;
	struct touchscreen_properties prop;
};
static int warnings;
#define dev_warn_ratelimited(...) (warnings++)
#define MT_TOOL_FINGER 0
#define ABS_MT_POSITION_X 53
#define ABS_MT_POSITION_Y 54
static unsigned int get_unaligned_le16(const u8 *p) { return p[0] | p[1] << 8; }
static unsigned int get_unaligned_le32(const u8 *p) { return get32(p); }
static void touchscreen_set_mt_pos(struct input_mt_pos *p,
		const struct touchscreen_properties *prop, int x, int y)
{
	(void)prop;
	p->x = x;
	p->y = y;
}
static void input_mt_slot(struct input_dev *d, int slot)
{
	assert(slot >= 0 && slot < 10);
	d->slot = slot;
}
static void input_mt_report_slot_state(struct input_dev *d, int tool, int active)
{
	(void)tool;
	assert(active);
	d->seen |= 1U << d->slot;
}
static void input_report_abs(struct input_dev *d, int code, int value)
{
	if (code == ABS_MT_POSITION_X)
		d->x[d->slot] = value;
	else {
		assert(code == ABS_MT_POSITION_Y);
		d->y[d->slot] = value;
	}
}
static void input_mt_sync_frame(struct input_dev *d) { d->active = d->seen; d->seen = 0; }
static void input_sync(struct input_dev *d) { d->syncs++; }
#include "rk3128_reference_driver.inc"

static void driver(void)
{
	struct input_dev input = {0};
	struct silead_ts_data data = {.input = &input, .reference = &state};
	/* Header, Y=320, X=512 (firmware ID nibble ignored by host tracking). */
	unsigned char packet[12] = {1, 0, 0, 0, 64, 1, 0, 0x12};
	int len, before;

	silead_rk3128_reference_reset(&state);
	silead_ts_read_rk3128_reference(&data, packet, 8);
	assert(input.active == 1 && input.x[0] == 400 && input.y[0] == 636);
	for (len = 0; len < 8; len++) {
		before = input.syncs;
		silead_ts_read_rk3128_reference(&data, packet, len);
		assert(input.active == 0 && input.syncs == before + 1);
		assert(state.valid == 0);
	}
	assert(warnings == 8);
	packet[0] = 11;
	silead_ts_read_rk3128_reference(&data, packet, sizeof(packet));
	assert(input.active == 0 && warnings == 9);
	packet[0] = 2; /* Partial second contact must not read stale bytes. */
	silead_ts_read_rk3128_reference(&data, packet, 8);
	assert(input.active == 0 && warnings == 10);
	packet[0] = 1;
	silead_ts_read_rk3128_reference(&data, packet, 8);
	assert(input.active == 1);
	packet[0] = 0;
	silead_ts_read_rk3128_reference(&data, packet, 4);
	silead_ts_read_rk3128_reference(&data, packet, 4);
	assert(input.active == 0);
	puts("RK3128 reference board production driver: packet decoding, calibrated axes, truncated/oversized reports, release PASS");
}

static int stream(int histories, int raw)
{
	unsigned char input[44];
	size_t len;

	while ((len = fread(input, 1, sizeof(input), stdin))) {
		struct silead_rk3128_reference_contact out[RK3128_REFERENCE_IDS] = {0};
		unsigned int now[RK3128_REFERENCE_IDS], header;
		int i, depth, n;

		assert(len == sizeof(input));
		header = get32(input);
		for (i = 0; i < RK3128_REFERENCE_IDS; i++)
			now[i] = get32(input + 4 + i * 4);
		if (header == UINT32_MAX) {
			silead_rk3128_reference_reset(&state);
			n = 0;
		} else if (raw) {
			n = header & 0xff;
			if (n > 5)
				n = 5;
			for (i = 0; i < n; i++) {
				out[i].x = rk3128_reference_x(now[i]);
				out[i].y = rk3128_reference_y(now[i]);
				out[i].id = i;
				silead_d708_calibrate(&out[i].x, &out[i].y);
			}
		} else {
			n = silead_rk3128_reference_process(&state, header, now, out);
		}
		assert(n >= 0 && n <= 5);
		put32(n);
		for (i = 0; i < RK3128_REFERENCE_IDS; i++) {
			if (i < n) {
				assert(out[i].id >= 0 && out[i].id < 10);
				assert(i == 0 || out[i].id > out[i - 1].id);
			}
			put32(out[i].x);
			put32(out[i].y);
			put32(out[i].id);
		}
		if (histories) {
			for (depth = 0; depth < RK3128_REFERENCE_DEPTH; depth++)
				for (i = 0; i < RK3128_REFERENCE_IDS; i++)
					put32(state.p[depth][i]);
			for (depth = 0; depth < RK3128_REFERENCE_DEPTH; depth++)
				for (i = 0; i < RK3128_REFERENCE_IDS; i++)
					put32(state.s[depth][i]);
			for (depth = 0; depth < RK3128_REFERENCE_DEPTH; depth++)
				for (i = 0; i < RK3128_REFERENCE_IDS; i++)
					put32(state.r[depth][i]);
		}
	}
	assert(!ferror(stdin));
	return 0;
}

static int sample(unsigned int p, struct silead_rk3128_reference_contact out[RK3128_REFERENCE_IDS])
{
	unsigned int now[RK3128_REFERENCE_IDS] = {p};
	return silead_rk3128_reference_process(&state, !!p, now, out);
}

static void gesture_step(TouchGesture *g, unsigned int now[RK3128_REFERENCE_IDS], int n, int64_t *us)
{
	struct silead_rk3128_reference_contact out[RK3128_REFERENCE_IDS];
	TouchContact contacts[TOUCH_CONTACTS];
	int i, count = silead_rk3128_reference_process(&state, n, now, out);

	for (i = 0; i < TOUCH_CONTACTS; i++)
		contacts[i] = (TouchContact){.id = -1};
	for (i = 0; i < count; i++)
		contacts[out[i].id] = (TouchContact){out[i].id,
			out[i].x * (1920.0f / 801), out[i].y * (1080.0f / 1281)};
	touch_gesture_frame(g, contacts, *us);
	*us += 20970;
}

static void gestures(void)
{
	TouchGesture g;
	int i, phase;
	int64_t us = 0;

	silead_rk3128_reference_reset(&state);
	touch_gesture_reset(&g);
	for (phase = 0; phase < 3; phase++) {
		for (i = 0; i < (phase ? 120 : 200); i++) {
			unsigned int now[RK3128_REFERENCE_IDS] = {0};

			if (!phase)
				now[0] = 512U << 16 | (320 + (i % 2 ? 8 : -8));
			else
				now[0] = (200U + i * 4U) << 16 | 320;
			if (phase == 2)
				now[1] = (300U + i * 4U) << 16 | 450;
			gesture_step(&g, now, phase == 2 ? 2 : 1, &us);
		}
		for (i = 0; i < 12; i++) {
			unsigned int now[RK3128_REFERENCE_IDS] = {0};

			gesture_step(&g, now, 0, &us);
		}
	}
	assert(g.max_contacts == 2 && g.hold && g.swipe && g.two_swipe);
	printf("RK3128 reference board filtered input -> gesture oracle: hold %.1f ms, single %.1f px, paired %.1f px PASS\n",
	       g.max_hold_ms, g.max_swipe_px, g.max_two_swipe_px);
}

static void properties(void)
{
	struct silead_rk3128_reference_contact out[RK3128_REFERENCE_IDS];
	struct silead_rk3128_reference independent;
	unsigned int raw[RK3128_REFERENCE_IDS] = {512U << 16 | 320};
	int x, y, i;

	silead_rk3128_reference_reset(&state);
	/* Vendor-confirmed: first single-finger contact is reported immediately. */
	assert(sample(raw[0], out) == 1);
	x = out[0].x;
	y = out[0].y;
	for (i = 0; i < 32; i++)
		assert(sample(raw[0], out) == 1);
	for (i = 0; i < 96; i++) {
		assert(sample((512U << 16) | (320 + (i % 2 ? 8 : -8)), out) == 1);
		assert(out[0].x == x && out[0].y == y);
	}
	/* Tracking/prediction bridges one missing sample, then releases. */
	assert(sample(0, out) == 1);
	assert(sample(0, out) == 0);
	/* Power/reset must discard all histories and report a new tap immediately. */
	silead_rk3128_reference_reset(&state);
	assert(sample(200U << 16 | 200, out) == 1);
	assert(out[0].id == 0);
	/* State is per device: another panel cannot influence this one. */
	silead_rk3128_reference_reset(&independent);
	assert(silead_rk3128_reference_process(&independent, 1, raw, out) == 1);
	assert(out[0].x == x && out[0].y == y);
	printf("RK3128 reference board: immediate single down, jitter suppression, bounded release, reset, device isolation PASS; state %zu bytes\n",
	       sizeof(state));
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--histories"))
		return stream(1, 0);
	if (argc == 2 && !strcmp(argv[1], "--stream"))
		return stream(0, 0);
	if (argc == 2 && !strcmp(argv[1], "--raw"))
		return stream(0, 1);
	assert(argc == 1);
	properties();
	gestures();
	driver();
	return 0;
}
