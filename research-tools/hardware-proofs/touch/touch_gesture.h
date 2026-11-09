// Recorded-touch gesture oracle: complete broker snapshots, monotonic microseconds.
#ifndef TOUCH_GESTURE_H
#define TOUCH_GESTURE_H
#include <stdint.h>
#define TOUCH_CONTACTS 10
typedef struct {
    int id;
    float x, y;
} TouchContact;
typedef struct {
    int ids[TOUCH_CONTACTS], count, multi, hold_ok, pair[2], pair_id[2];
    float start_x[TOUCH_CONTACTS], start_y[TOUCH_CONTACTS], pair_x[2], pair_y[2];
    int64_t down_us;
    double hold_ms, max_hold_ms, max_swipe_px, max_two_swipe_px;
    int hold, swipe, two_swipe, max_contacts;
    const char *message;
} TouchGesture;
void touch_gesture_reset(TouchGesture *g);
void touch_gesture_cancel(TouchGesture *g);
void touch_gesture_frame(TouchGesture *g, const TouchContact c[TOUCH_CONTACTS], int64_t us);
#endif
