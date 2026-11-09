// touchinj: scripted touches on the board's touchscreen for the GPU scenes of scripts/mali-capture.sh (#345). One process
// writes the whole gesture script as struct input_event records into /dev/input/event0, in the gslX680's own protocol B
// form (ABS_MT_SLOT 0, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X/Y, SYN_REPORT; no BTN_TOUCH; the input mapping), so whoever holds
// the grab (the launcher, which forwards to the app in front) gets them; one process per gesture, not sendevent, which
// double-types (GOTCHAS.md "Demo on the Mali-400"). Built with the NDK as a PIE.
//   touchinj <device> <command>...   commands, in screen pixels (1920x1080) and milliseconds:
//     tap X Y            down, 60 ms, up
//     swipe X0 Y0 X1 Y1 MS   down at (X0,Y0), a move every 16 ms to (X1,Y1) over MS, up
//     sleep MS
// Raw = screen * (max + 1) / screen size, the inverse input mapping (raw 0-800 x 0-1280).
// SIGTERM, SIGINT or SIGHUP mid-gesture lifts the finger before it exits, so the next owner of the touchscreen (the
// launcher, started again by the capture's cleanup) never inherits a contact that is still down.
#include <fcntl.h>
#include <signal.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int fd, tid = 100;
static volatile sig_atomic_t touching;

static void ev(int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof(e));
    gettimeofday(&e.time, NULL);
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    if (write(fd, &e, sizeof(e)) != (ssize_t)sizeof(e)) perror("touchinj: write");
}

static int rx(double x) { return (int)(x * 801.0 / 1920.0); }
static int ry(double y) { return (int)(y * 1281.0 / 1080.0); }
static void ms(int n) { struct timespec t = {n / 1000, (n % 1000) * 1000000L}; nanosleep(&t, NULL); }

static void down(double x, double y) {
    touching = 1;
    ev(EV_ABS, ABS_MT_SLOT, 0);
    ev(EV_ABS, ABS_MT_TRACKING_ID, tid++);
    ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 40);
    ev(EV_ABS, ABS_MT_POSITION_X, rx(x));
    ev(EV_ABS, ABS_MT_POSITION_Y, ry(y));
    ev(EV_SYN, SYN_REPORT, 0);
}
static void move(double x, double y) {
    ev(EV_ABS, ABS_MT_POSITION_X, rx(x));
    ev(EV_ABS, ABS_MT_POSITION_Y, ry(y));
    ev(EV_SYN, SYN_REPORT, 0);
}
static void up(void) {
    ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ev(EV_SYN, SYN_REPORT, 0);
    touching = 0;
}

// async-signal-safe: plain write(2)s of zero-time records (the kernel stamps injected events itself)
static void on_signal(int sig) {
    (void)sig;
    if (touching) {
        struct input_event e[3];
        memset(e, 0, sizeof(e));
        e[0].type = EV_ABS, e[0].code = ABS_MT_SLOT, e[0].value = 0;
        e[1].type = EV_ABS, e[1].code = ABS_MT_TRACKING_ID, e[1].value = -1;
        e[2].type = EV_SYN, e[2].code = SYN_REPORT, e[2].value = 0;
        if (write(fd, e, sizeof(e)) < 0) _exit(1);
    }
    _exit(0);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: touchinj <device> tap X Y | swipe X0 Y0 X1 Y1 MS | sleep MS ...\n"); return 2; }
    fd = open(argv[1], O_WRONLY | O_CLOEXEC);
    if (fd < 0) { perror(argv[1]); return 1; }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigfillset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    for (int i = 2; i < argc;) {
        if (!strcmp(argv[i], "tap") && i + 2 < argc) {
            down(atof(argv[i + 1]), atof(argv[i + 2]));
            ms(60);
            up();
            i += 3;
        } else if (!strcmp(argv[i], "swipe") && i + 5 < argc) {
            double x0 = atof(argv[i + 1]), y0 = atof(argv[i + 2]), x1 = atof(argv[i + 3]), y1 = atof(argv[i + 4]);
            int d = atoi(argv[i + 5]), steps = d / 16 > 0 ? d / 16 : 1;
            down(x0, y0);
            for (int s = 1; s <= steps; s++) {
                ms(16);
                move(x0 + (x1 - x0) * s / steps, y0 + (y1 - y0) * s / steps);
            }
            up();
            i += 6;
        } else if (!strcmp(argv[i], "sleep") && i + 1 < argc) {
            ms(atoi(argv[i + 1]));
            i += 2;
        } else {
            fprintf(stderr, "touchinj: bad command at %s\n", argv[i]);
            return 2;
        }
    }
    close(fd);
    return 0;
}
