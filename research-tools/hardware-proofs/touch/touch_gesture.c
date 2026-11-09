#include "touch_gesture.h"
#include <math.h>
#include <string.h>

static int inside(const TouchContact *c) { return c->x >= 64 && c->x < 1856 && c->y >= 500 && c->y < 770; }
void touch_gesture_reset(TouchGesture *g) {
    memset(g, 0, sizeof(*g));
    touch_gesture_cancel(g);
}
void touch_gesture_cancel(TouchGesture *g) {
    g->count = g->hold_ok = 0;
    g->hold_ms = 0;
    for (int i = 0; i < TOUCH_CONTACTS; i++)
        g->ids[i] = -1;
    g->pair[0] = g->pair[1] = -1;
    g->message = "Hold still, swipe once, then swipe with two fingers";
}
void touch_gesture_frame(TouchGesture *g, const TouchContact c[TOUCH_CONTACTS], int64_t us) {
    int n = 0, slots[TOUCH_CONTACTS];
    for (int i = 0; i < TOUCH_CONTACTS; i++) {
        if (c[i].id < 0)
            continue;
        slots[n++] = i;
        if (c[i].id != g->ids[i]) {
            g->start_x[i] = c[i].x;
            g->start_y[i] = c[i].y;
        }
    }
    if (n > g->max_contacts)
        g->max_contacts = n;
    if (!g->count && n) {
        g->multi = 0;
        g->hold_ok = n == 1 && inside(&c[slots[0]]);
        g->down_us = us;
    }
    g->hold_ms = 0;
    if (n > 1) {
        g->multi = 1;
        g->hold_ok = 0;
    }
    if (!n && g->count == 1 && g->hold_ok && us >= g->down_us) {
        double held = (us - g->down_us) / 1000.0;
        if (held > g->max_hold_ms)
            g->max_hold_ms = held;
        if (held >= 3000) {
            g->hold = 1;
            g->message = "Hold recorded. Lift, then try the swipes";
        }
    }
    if (n == 1) {
        int k = slots[0];
        double distance = hypot(c[k].x - g->start_x[k], c[k].y - g->start_y[k]);
        if ((g->count && c[k].id != g->ids[k]) || !inside(&c[k]) || distance > 32)
            g->hold_ok = 0;
        if (!g->multi && inside(&(TouchContact){.x = g->start_x[k], .y = g->start_y[k]})) {
            if (distance > g->max_swipe_px)
                g->max_swipe_px = distance;
            if (distance >= 100) {
                g->swipe = 1;
                g->message = "One-finger swipe recorded";
            } else if (g->hold_ok && us >= g->down_us) {
                g->hold_ms = (us - g->down_us) / 1000.0;
                if (g->hold_ms > g->max_hold_ms)
                    g->max_hold_ms = g->hold_ms;
                if (g->hold_ms >= 3000) {
                    g->hold = 1;
                    g->message = "Hold recorded. Lift, then try the swipes";
                }
            } else
                g->message = "Finger moved. Swipe, or lift and hold still";
        }
    }
    if (n == 2) {
        int same = 1;
        for (int j = 0; j < 2; j++)
            if (g->pair[j] != slots[j] || g->pair_id[j] != c[slots[j]].id)
                same = 0;
        if (!same) {
            for (int j = 0; j < 2; j++) {
                int k = slots[j];
                g->pair[j] = inside(&c[k]) ? k : -1;
                g->pair_id[j] = c[k].id;
                g->pair_x[j] = c[k].x;
                g->pair_y[j] = c[k].y;
            }
            g->message = "Two contacts. Swipe both fingers together";
        } else if (g->pair[0] >= 0 && g->pair[1] >= 0) {
            double dx[2], dy[2], d[2];
            for (int j = 0; j < 2; j++) {
                int k = slots[j];
                dx[j] = c[k].x - g->pair_x[j];
                dy[j] = c[k].y - g->pair_y[j];
                d[j] = hypot(dx[j], dy[j]);
            }
            // Each contact must move, in a shared direction. Pinches do not count.
            if (d[0] >= 100 && d[1] >= 100 && dx[0] * dx[1] + dy[0] * dy[1] >= .5 * d[0] * d[1]) {
                g->two_swipe = 1;
                g->message = "Two-finger swipe recorded";
                if (fmin(d[0], d[1]) > g->max_two_swipe_px)
                    g->max_two_swipe_px = fmin(d[0], d[1]);
            }
        }
    } else {
        g->pair[0] = g->pair[1] = -1;
        if (n > 2)
            g->message = "More than two contacts. Lift and try two fingers";
    }
    for (int i = 0; i < TOUCH_CONTACTS; i++)
        g->ids[i] = c[i].id;
    g->count = n;
}
