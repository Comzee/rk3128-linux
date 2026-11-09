// Read-only DRM owner-handover timing: a non-master fd opened by a parent, first/repeated VERSION in its child.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
static double ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <display-server card>\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    // Require an existing master. A successful is-master check means the server is absent: stop without any modeset.
    if (drmIsMaster(fd)) {
        fprintf(stderr, "refusing: display server is not holding DRM master\n");
        close(fd);
        return 1;
    }
    pid_t p = fork();
    if (p < 0) {
        perror("fork");
        close(fd);
        return 1;
    }
    if (!p) {
        for (int i = 0; i < 2; i++) {
            double t = ms();
            drmVersionPtr v = drmGetVersion(fd);
            double elapsed = ms() - t;
            if (!v) {
                perror("VERSION");
                _exit(1);
            }
            printf("drm-handoff VERSION=%d ms=%.3f driver=%s\n", i + 1, elapsed, v->name);
            drmFreeVersion(v);
        }
        fflush(stdout);
        close(fd);
        _exit(0);
    }
    close(fd);
    int st;
    if (waitpid(p, &st, 0) != p)
        return 1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
