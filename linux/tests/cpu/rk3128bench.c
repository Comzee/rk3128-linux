/* rk3128bench: the CPU and memory benchmarks of docs/HARDWARE.md "CPU" and "RAM", as one static armhf binary that runs
 * unchanged under Android's 3.10 kernel and under Linux (development record #429). The kernels are the ones behind
 * the Android numbers, byte for byte where it matters:
 *   int   = scripts/native/cpubase.c's xorshift32 + read-modify-write of a 4 KB array, 11 integer ops per iteration
 *   float = 8 independent multiply-add chains, 16 flops per iteration
 *   copy / set / read = app/src/main/cpp/statsbench.c's memcpy, memset and uint32_t-sum loops over 32 MB buffers,
 *           calibrated with one iteration, then the median of 5 runs of about 0.6 s
 * Modes (every one prints key=value lines, nothing else):
 *   rk3128bench par <int|float> <nproc> <seconds>   nproc processes start together; one line each, and the sum
 *   rk3128bench thr <int|float> <nthreads> <seconds> the same in threads of one process, each with its own state
 *   rk3128bench sus <seconds> <nproc> [t]           nproc int workers (processes, or threads with t); a line per second with the frequencies, then the
 *                                                  peak (first 10 s), sustained (last 30 s), ratio and min/max freq
 *   rk3128bench ramp <nproc>                        sleep 2 s (idle), then load nproc cores: the work rate and cpu0 clock
 *                                                  each millisecond until both have settled
 *   rk3128bench mem [copy|set|read|all|x]           the three bandwidths, MB/s of 1e6 bytes; x: hand-written loops (diagnosis)
 *   rk3128bench temp                                every thermal zone and hwmon temperature that exists
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define INT_OPS 11
#define FLOAT_OPS 16
#define CPU_SYS "/sys/devices/system/cpu"
#define MAXP 8

static volatile unsigned sink;

static long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long) t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* ---- the kernels (cpubase.c, unchanged) ---- */
static unsigned arr[1024]; /* the processes' array; every thread has its own (struct tw) */

static void int_chunk(unsigned *arr, unsigned *xs, unsigned *accs, int iters) {
    unsigned x = *xs, acc = *accs;
    for (int i = 0; i < iters; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        unsigned idx = x & 1023;
        unsigned v = arr[idx] + x;
        arr[idx] = v;
        acc += v ^ (unsigned) i;
    }
    *xs = x; *accs = acc;
}

/* The float kernel as the instruction stream scripts/native/cpubase.c compiles to under arm-none-eabi-gcc (HARDWARE.md's 198 and 698
 * MFLOPS): per chain vmov s5 <- c; vfma s5 += a * m; vmov a <- s5, eight chains, no other instruction in the loop. Written out
 * in asm because a different compiler (the Debian gcc 15 that builds this) allocates the same C with nine more register copies
 * and runs 12 % slower on this core, which is the compiler's loop, not the board's speed (#429). */
static void float_chunk(float *s, int iters) {
    float a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3], a4 = s[4], a5 = s[5], a6 = s[6], a7 = s[7];
    const float m = 0.999999f, c = 1e-7f;
    float t;
    __asm__ volatile(
        "1:\n\t"
        "vmov.f32 %[t], %[c]\n\tvfma.f32 %[t], %[a0], %[m]\n\tvmov.f32 %[a0], %[t]\n\t"
        "vmov.f32 %[t], %[c]\n\tvfma.f32 %[t], %[a1], %[m]\n\tvmov.f32 %[a1], %[t]\n\t"
        "vmov.f32 %[t], %[c]\n\tvfma.f32 %[t], %[a2], %[m]\n\tvmov.f32 %[a2], %[t]\n\t"
        "vmov.f32 %[t], %[c]\n\tvfma.f32 %[t], %[a3], %[m]\n\tvmov.f32 %[a3], %[t]\n\t"
        "vmov.f32 %[t], %[c]\n\tvfma.f32 %[t], %[a4], %[m]\n\tvmov.f32 %[a4], %[t]\n\t"
        "vmov.f32 %[t], %[c]\n\tvfma.f32 %[t], %[a5], %[m]\n\tvmov.f32 %[a5], %[t]\n\t"
        "vmov.f32 %[t], %[c]\n\tvfma.f32 %[t], %[a6], %[m]\n\tvmov.f32 %[a6], %[t]\n\t"
        "vmov.f32 %[t], %[c]\n\tvfma.f32 %[t], %[a7], %[m]\n\tvmov.f32 %[a7], %[t]\n\t"
        "subs %[n], %[n], #1\n\t"
        "bne 1b\n\t"
        : [a0] "+t"(a0), [a1] "+t"(a1), [a2] "+t"(a2), [a3] "+t"(a3), [a4] "+t"(a4), [a5] "+t"(a5), [a6] "+t"(a6), [a7] "+t"(a7),
          [t] "=&t"(t), [n] "+r"(iters)
        : [c] "t"(c), [m] "t"(m) : "cc");
    s[0] = a0; s[1] = a1; s[2] = a2; s[3] = a3; s[4] = a4; s[5] = a5; s[6] = a6; s[7] = a7;
}

/* one worker: runs the kernel in chunks of 100000 iterations until `stop` (a deadline in ns, or 0 and *flag set),
 * publishing the chunks done in *count when it is non-NULL; returns the iterations and the elapsed ns */
struct run { long long iters, ns; };
static struct run worker(int is_int, long long end_ns, volatile unsigned *count, volatile int *flag, unsigned *arr) {
    unsigned x = 0x2545F491u, acc = 0;
    float s[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    long long t0 = now_ns(), iters = 0, t;
    const int chunk = 100000;
    do {
        if (is_int) int_chunk(arr, &x, &acc, chunk); else float_chunk(s, chunk);
        iters += chunk;
        if (count) *count = (unsigned) (iters / chunk); /* chunks: a 32-bit word is written atomically, a 64-bit one is not */
        t = now_ns();
    } while ((end_ns ? t < end_ns : !*flag));
    sink = acc ^ x ^ (unsigned) s[0];
    return (struct run){iters, t - t0};
}

static long long rd_ll(const char *path) {
    char b[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    return atoll(b);
}

static long long cpu_freq(int cpu) {
    char p[96];
    snprintf(p, sizeof p, CPU_SYS "/cpu%d/cpufreq/scaling_cur_freq", cpu);
    return rd_ll(p);
}

static int ncpu_online(void) {
    int n = (int) sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : n;
}

static void pin(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu % ncpu_online(), &s);
    sched_setaffinity(0, sizeof s, &s);
}

/* ---- par ---- */
static int cmd_par(int argc, char **argv) {
    if (argc < 5) return 2;
    int is_int = argv[2][0] == 'i', np = atoi(argv[3]), secs = atoi(argv[4]);
    if (np < 1 || np > MAXP) return 2;
    struct run *res = mmap(0, sizeof(struct run) * MAXP, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    long long end = now_ns() + 300000000LL + (long long) secs * 1000000000LL; /* every child ends at the same instant */
    for (int i = 0; i < np; i++) {
        if (fork() == 0) {
            struct run r = worker(is_int, end, 0, 0, arr);
            res[i] = r;
            _exit(0);
        }
    }
    while (wait(0) > 0) {}
    double total = 0;
    for (int i = 0; i < np; i++) {
        double ips = res[i].iters * 1e9 / res[i].ns;
        total += ips;
        printf("kind=%s proc=%d iters_per_s=%lld\n", is_int ? "int" : "float", i, (long long) ips);
    }
    printf("kind=%s procs=%d iters_per_s=%lld %s=%.1f\n", is_int ? "int" : "float", np, (long long) total,
           is_int ? "Mops" : "MFLOPS", total * (is_int ? INT_OPS : FLOAT_OPS) / 1e6);
    return 0;
}


/* ---- threads: the same kernels in threads of one process, each with its own state and array (Android's 4-thread method) ---- */
struct tw {
    int is_int;
    long long end_ns;
    volatile unsigned count;
    volatile int *flag;
    struct run r;
    pthread_t th;
    unsigned arr[1024] __attribute__((aligned(64)));
};
static void *tw_main(void *p) {
    struct tw *w = p;
    w->r = worker(w->is_int, w->end_ns, &w->count, w->flag, w->arr);
    return 0;
}
static struct tw *tw_start(int n, int is_int, long long end_ns, volatile int *flag) {
    struct tw *w = aligned_alloc(64, sizeof(struct tw) * n);
    if (!w) return 0;
    memset(w, 0, sizeof(struct tw) * n);
    for (int i = 0; i < n; i++) {
        w[i].is_int = is_int; w[i].end_ns = end_ns; w[i].flag = flag;
        if (pthread_create(&w[i].th, 0, tw_main, &w[i])) { fprintf(stderr, "pthread_create failed\n"); exit(1); }
    }
    return w;
}

static int cmd_thr(int argc, char **argv) {
    if (argc < 5) return 2;
    int is_int = argv[2][0] == 'i', nt = atoi(argv[3]), secs = atoi(argv[4]);
    if (nt < 1 || nt > MAXP) return 2;
    long long end = now_ns() + 100000000LL + (long long) secs * 1000000000LL; /* every thread ends at the same instant */
    struct tw *w = tw_start(nt, is_int, end, 0);
    if (!w) return 1;
    double total = 0;
    for (int i = 0; i < nt; i++) pthread_join(w[i].th, 0);
    for (int i = 0; i < nt; i++) {
        double ips = w[i].r.iters * 1e9 / w[i].r.ns;
        total += ips;
        printf("kind=%s thread=%d iters_per_s=%lld\n", is_int ? "int" : "float", i, (long long) ips);
    }
    printf("kind=%s threads=%d iters_per_s=%lld %s=%.1f\n", is_int ? "int" : "float", nt, (long long) total,
           is_int ? "Mops" : "MFLOPS", total * (is_int ? INT_OPS : FLOAT_OPS) / 1e6);
    return 0;
}

/* ---- sus: the Android method (CpuSuite.sustained): a total every second and the clocks at each sample ---- */
static int cmd_sus(int argc, char **argv) {
    if (argc < 4) return 2;
    int secs = atoi(argv[2]), np = atoi(argv[3]);
    if (secs < 1 || np < 1 || np > MAXP) return 2;
    int threads = argc > 4 && argv[4][0] == 't';
    volatile unsigned *cnt = mmap(0, sizeof(unsigned) * 16, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    volatile int *stop = (volatile int *) (cnt + 12);
    pid_t pids[MAXP];
    struct tw *tws = 0;
    if (threads) {
        tws = tw_start(np, 1, 0, stop);
        if (!tws) return 1;
    } else {
        for (int i = 0; i < np; i++) {
            pids[i] = fork();
            if (pids[i] == 0) { worker(1, 0, &cnt[i], stop, arr); _exit(0); }
        }
    }
    double *tp = calloc(secs, sizeof *tp);
    long long *fmax = calloc(secs, sizeof *fmax);
    unsigned prev[MAXP] = {0};
    long long t0 = now_ns(), prev_t = t0, fmin_all = 1LL << 40, fmax_all = 0;
    int ncpu = ncpu_online();
    for (int t = 1; t <= secs; t++) {
        long long target = t0 + (long long) t * 1000000000LL, n = now_ns();
        if (target > n) { struct timespec ts = {(target - n) / 1000000000LL, (target - n) % 1000000000LL}; nanosleep(&ts, 0); }
        n = now_ns();
        double dt = (n - prev_t) / 1e9, total = 0;
        prev_t = n;
        for (int i = 0; i < np; i++) { unsigned c = threads ? tws[i].count : cnt[i]; total += (double) (unsigned) (c - prev[i]) * 100000.0 / dt; prev[i] = c; }
        tp[t - 1] = total;
        printf("t=%d iters_per_s=%lld mops=%.2f freq_khz=", t, (long long) total, total * INT_OPS / 1e6);
        long long fm = 0;
        for (int c = 0; c < ncpu; c++) {
            long long f = cpu_freq(c);
            printf("%s%lld", c ? "," : "", f);
            if (f > fm) fm = f;
            if (f > 0 && f < fmin_all) fmin_all = f;
            if (f > fmax_all) fmax_all = f;
        }
        fmax[t - 1] = fm;
        printf("\n");
        fflush(stdout);
    }
    *stop = 1;
    for (int i = 0; i < np; i++) { if (threads) pthread_join(tws[i].th, 0); else waitpid(pids[i], 0, 0); }
    int pn = secs < 10 ? secs : 10, sn = secs / 3 < 30 ? secs / 3 : 30;
    if (sn < 1) sn = 1;
    double peak = 0, sus = 0;
    for (int i = 0; i < pn; i++) peak += tp[i];
    for (int i = secs - sn; i < secs; i++) sus += tp[i];
    peak /= pn; sus /= sn;
    printf("sustained_s=%d %s=%d peak_mops=%.2f sustained_mops=%.2f ratio=%.4f freq_khz_min=%lld freq_khz_max=%lld\n", secs, threads ? "threads" : "procs", np,
           peak * INT_OPS / 1e6, sus * INT_OPS / 1e6, peak > 0 ? sus / peak : 0.0, fmin_all == 1LL << 40 ? -1 : fmin_all, fmax_all);
    return 0;
}

/* ---- ramp: how long the governor takes from idle to the top clock under a sudden load ---- */
#define RAMP_MAX 6000
static int cmd_ramp(int argc, char **argv) {
    int np = argc > 2 ? atoi(argv[2]) : 1;
    if (np < 1 || np > MAXP) return 2;
    volatile int *stop = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int gopipe[2];
    if (pipe(gopipe)) return 1;
    for (int i = 1; i < np; i++) {
        if (fork() == 0) {
            char c;
            if (read(gopipe[0], &c, 1) == 1) worker(1, 0, 0, stop, arr); /* asleep in read() until the load starts */
            _exit(0);
        }
    }
    pin(0);
    struct timespec idle = {2, 0};
    nanosleep(&idle, 0); /* every core idle: the governor settles at its lowest clock */
    long long f_idle = cpu_freq(0);
    char fp[96];
    snprintf(fp, sizeof fp, CPU_SYS "/cpu0/cpufreq/scaling_cur_freq");
    int ffd = open(fp, O_RDONLY);
    static long long tt[RAMP_MAX], it[RAMP_MAX], fr[RAMP_MAX];
    unsigned x = 0x2545F491u, acc = 0;
    long long t0 = now_ns(), end = t0 + 1500000000LL;
    { char g[MAXP]; memset(g, 1, sizeof g); if (write(gopipe[1], g, np - 1) < 0) return 1; }
    int n = 0;
    const int chunk = 2000; /* ~30 us at 1 GHz: finer than the 1 ms windows below */
    long long win_iters = 0, win_start = t0;
    while (n < RAMP_MAX) {
        int_chunk(arr, &x, &acc, chunk);
        win_iters += chunk;
        long long t = now_ns();
        if (t - win_start >= 1000000LL) { /* a 1 ms window */
            long long f = -1;
            if (ffd >= 0) { char b[32]; ssize_t r = pread(ffd, b, sizeof b - 1, 0); if (r > 0) { b[r] = 0; f = atoll(b); } }
            tt[n] = t - t0; it[n] = win_iters * 1000000LL / (t - win_start); fr[n] = f; /* iterations per ms */
            n++;
            win_iters = 0; win_start = t;
        }
        if (t >= end) break;
    }
    *stop = 1;
    sink = acc ^ x;
    while (wait(0) > 0) {}
    /* settled rate: the median of the windows from 1.0 s to the end; settled clock: the largest reading */
    long long fmax = 0;
    for (int i = 0; i < n; i++) if (fr[i] > fmax) fmax = fr[i];
    int a = 0;
    while (a < n && tt[a] < 1000000000LL) a++;
    long long best, sum = 0; int cnt = 0;
    for (int i = a; i < n; i++) { sum += it[i]; cnt++; }
    best = cnt ? sum / cnt : 0;
    /* "settled" = this window and the next four are all at or above the level (one lucky window does not count) */
    long long t_rate90 = -1, t_rate95 = -1, t_fmax = -1;
    for (int i = 0; i + 5 <= n; i++) {
        int ok90 = 1, ok95 = 1, okf = fmax > 0;
        for (int j = i; j < i + 5; j++) {
            if (it[j] * 100 < best * 90) ok90 = 0;
            if (it[j] * 100 < best * 95) ok95 = 0;
            if (fr[j] < fmax) okf = 0;
        }
        if (t_rate90 < 0 && ok90) t_rate90 = tt[i];
        if (t_rate95 < 0 && ok95) t_rate95 = tt[i];
        if (t_fmax < 0 && okf) t_fmax = tt[i];
    }
    for (int i = 0; i < n && i < 400; i++) printf("ms=%lld iters_per_ms=%lld freq_khz=%lld\n", (tt[i] + 500000) / 1000000, it[i], fr[i]);
    printf("ramp procs=%d idle_freq_khz=%lld top_freq_khz=%lld settled_iters_per_ms=%lld ms_to_90pct_rate=%.1f ms_to_95pct_rate=%.1f ms_to_top_freq_reading=%.1f\n",
           np, f_idle, fmax, best, t_rate90 / 1e6, t_rate95 / 1e6, t_fmax / 1e6);
    return 0;
}

/* ---- mem: statsbench.c's loops ---- */
static double bw_copy(size_t n, int iterations) {
    uint8_t *src = malloc(n), *dst = malloc(n);
    if (!src || !dst) { free(src); free(dst); return -1; }
    memset(src, 0x5a, n);
    memset(dst, 0xa5, n);
    long long t0 = now_ns();
    for (int i = 0; i < iterations; i++) {
        src[i & 63] = (uint8_t) i;
        memcpy(dst, src, n);
        sink += dst[n - 1 - (i & 63)];
    }
    double dt = (now_ns() - t0) / 1e9;
    free(src); free(dst);
    return dt > 0 ? (double) n * iterations / dt / 1e6 : -1;
}

static double bw_set(size_t n, int iterations) {
    uint8_t *dst = malloc(n);
    if (!dst) return -1;
    memset(dst, 0, n);
    long long t0 = now_ns();
    for (int i = 0; i < iterations; i++) {
        memset(dst, i & 0xff, n);
        sink += dst[(size_t) i * 4099 % n];
    }
    double dt = (now_ns() - t0) / 1e9;
    free(dst);
    return dt > 0 ? (double) n * iterations / dt / 1e6 : -1;
}

static double bw_read(size_t bytes, int iterations) {
    size_t words = bytes / 4;
    uint32_t *buf = malloc(words * 4);
    if (!buf) return -1;
    for (size_t i = 0; i < words; i++) buf[i] = (uint32_t) i;
    uint32_t acc = 0;
    long long t0 = now_ns();
    for (int it = 0; it < iterations; it++) {
        const uint32_t *p = buf;
        uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        for (size_t i = 0; i < words; i += 4) {
            a0 += p[i]; a1 += p[i + 1]; a2 += p[i + 2]; a3 += p[i + 3];
        }
        acc += a0 ^ a1 ^ a2 ^ a3;
    }
    double dt = (now_ns() - t0) / 1e9;
    sink += acc;
    free(buf);
    return dt > 0 ? (double) words * 4 * iterations / dt / 1e6 : -1;
}


/* ---- diagnosis variants (mem x): other ways to move the same bytes, to tell a slow libc routine from a slow DDR ---- */
typedef void (*copy_fn)(void *dst, const void *src, size_t n);
typedef void (*set_fn)(void *dst, int c, size_t n);
typedef uint32_t (*read_fn)(const uint32_t *p, size_t words);

/* ldm/stm of 8 registers (32 B) twice a loop with a pld 192 B ahead: what bionic's Cortex-A7/A15 memcpy does */
static void copy_ldm(void *dst, const void *src, size_t n) {
    uint32_t *d = dst; const uint32_t *s = src;
    size_t blocks = n / 64;
    __asm__ volatile(
        "1:\n\t"
        "pld [%1, #192]\n\t"
        "ldmia %1!, {r2-r9}\n\t"
        "stmia %0!, {r2-r9}\n\t"
        "ldmia %1!, {r2-r9}\n\t"
        "stmia %0!, {r2-r9}\n\t"
        "subs %2, %2, #1\n\t"
        "bne 1b\n\t"
        : "+r"(d), "+r"(s), "+r"(blocks) : : "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "memory", "cc");
}

/* NEON 64 B a loop with a pld 192 B ahead */
static void copy_neon(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    size_t blocks = n / 64;
    __asm__ volatile(
        "1:\n\t"
        "pld [%1, #192]\n\t"
        "vld1.8 {d0-d3}, [%1]!\n\t"
        "vld1.8 {d4-d7}, [%1]!\n\t"
        "vst1.8 {d0-d3}, [%0]!\n\t"
        "vst1.8 {d4-d7}, [%0]!\n\t"
        "subs %2, %2, #1\n\t"
        "bne 1b\n\t"
        : "+r"(d), "+r"(s), "+r"(blocks) : : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "memory", "cc");
}

static void set_stm(void *dst, int c, size_t n) {
    uint32_t *d = dst, v = (uint8_t) c * 0x01010101u;
    size_t blocks = n / 64;
    register uint32_t r2 __asm__("r2") = v, r3 __asm__("r3") = v, r4 __asm__("r4") = v, r5 __asm__("r5") = v;
    register uint32_t r6 __asm__("r6") = v, r7 __asm__("r7") = v, r8 __asm__("r8") = v, r9 __asm__("r9") = v;
    __asm__ volatile(
        "1:\n\t"
        "stmia %0!, {r2-r9}\n\t"
        "stmia %0!, {r2-r9}\n\t"
        "subs %1, %1, #1\n\t"
        "bne 1b\n\t"
        : "+r"(d), "+r"(blocks) : "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r6), "r"(r7), "r"(r8), "r"(r9) : "memory", "cc");
}

static void set_neon(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    size_t blocks = n / 64;
    __asm__ volatile(
        "vdup.8 q0, %2\n\t"
        "vmov q1, q0\n\t"
        "vmov q2, q0\n\t"
        "vmov q3, q0\n\t"
        "1:\n\t"
        "vst1.8 {d0-d3}, [%0]!\n\t"
        "vst1.8 {d4-d7}, [%0]!\n\t"
        "subs %1, %1, #1\n\t"
        "bne 1b\n\t"
        : "+r"(d), "+r"(blocks) : "r"(c) : "q0", "q1", "q2", "q3", "memory", "cc");
}

static uint32_t read_neon(const uint32_t *p, size_t words) {
    size_t blocks = words / 16;
    uint32_t out[4];
    __asm__ volatile(
        "veor q4, q4\n\t"
        "1:\n\t"
        "pld [%0, #192]\n\t"
        "vld1.32 {d0-d3}, [%0]!\n\t"
        "vld1.32 {d4-d7}, [%0]!\n\t"
        "vadd.i32 q4, q4, q0\n\t"
        "vadd.i32 q4, q4, q1\n\t"
        "vadd.i32 q4, q4, q2\n\t"
        "vadd.i32 q4, q4, q3\n\t"
        "subs %1, %1, #1\n\t"
        "bne 1b\n\t"
        "vst1.32 {d8-d9}, [%2]\n\t"
        : "+r"(p), "+r"(blocks) : "r"(out) : "q0", "q1", "q2", "q3", "q4", "memory", "cc");
    return out[0] ^ out[1] ^ out[2] ^ out[3];
}

static copy_fn g_copy;
static set_fn g_set;
static read_fn g_read;

static double bwv_copy(size_t n, int iterations) {
    uint8_t *src = malloc(n), *dst = malloc(n);
    if (!src || !dst) { free(src); free(dst); return -1; }
    memset(src, 0x5a, n);
    memset(dst, 0xa5, n);
    long long t0 = now_ns();
    for (int i = 0; i < iterations; i++) {
        src[i & 63] = (uint8_t) i;
        g_copy(dst, src, n);
        sink += dst[n - 1 - (i & 63)];
    }
    double dt = (now_ns() - t0) / 1e9;
    free(src); free(dst);
    return dt > 0 ? (double) n * iterations / dt / 1e6 : -1;
}

static double bwv_set(size_t n, int iterations) {
    uint8_t *dst = malloc(n);
    if (!dst) return -1;
    memset(dst, 0, n);
    long long t0 = now_ns();
    for (int i = 0; i < iterations; i++) {
        g_set(dst, i & 0xff, n);
        sink += dst[(size_t) i * 4099 % n];
    }
    double dt = (now_ns() - t0) / 1e9;
    free(dst);
    return dt > 0 ? (double) n * iterations / dt / 1e6 : -1;
}

static double bwv_read(size_t bytes, int iterations) {
    size_t words = bytes / 4;
    uint32_t *buf = malloc(words * 4);
    if (!buf) return -1;
    for (size_t i = 0; i < words; i++) buf[i] = (uint32_t) i;
    uint32_t acc = 0;
    long long t0 = now_ns();
    for (int it = 0; it < iterations; it++) acc += g_read(buf, words);
    double dt = (now_ns() - t0) / 1e9;
    sink += acc;
    free(buf);
    return dt > 0 ? (double) words * 4 * iterations / dt / 1e6 : -1;
}

static void libc_copy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
/* linux/userspace/rk3128-mem/rk3128-mem.c, linked in as a separate object */
extern void *fm_memcpy(void *, const void *, size_t);
extern void *fm_memset(void *, int, size_t);
static void fm_copy(void *d, const void *s, size_t n) { fm_memcpy(d, s, n); }
static void fm_set(void *d, int c, size_t n) { fm_memset(d, c, n); }

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y;
}

static void measure(const char *label, double (*fn)(size_t, int)) {
    const size_t N = 32u << 20;
    double first = fn(N, 1);
    if (first <= 0) { printf("%s=alloc-failed\n", label); return; }
    int iters = (int) (0.6 * first * 1e6 / N);
    if (iters < 1) iters = 1;
    double runs[5];
    for (int i = 0; i < 5; i++) runs[i] = fn(N, iters);
    double sorted[5];
    memcpy(sorted, runs, sizeof sorted);
    qsort(sorted, 5, sizeof sorted[0], cmp_d);
    printf("%s_mbps=%.1f buffer_mb=32 iterations_per_run=%d runs=%.1f,%.1f,%.1f,%.1f,%.1f max=%.1f\n", label, sorted[2], iters,
           runs[0], runs[1], runs[2], runs[3], runs[4], sorted[4]);
    fflush(stdout);
}

static int cmd_mem(int argc, char **argv) {
    const char *w = argc > 2 ? argv[2] : "all";
    int all = !strcmp(w, "all");
    if (all || !strcmp(w, "copy")) measure("memcpy", bw_copy);
    if (all || !strcmp(w, "set")) measure("memset", bw_set);
    if (all || !strcmp(w, "read")) measure("read", bw_read);
    if (!strcmp(w, "x")) { /* diagnosis: libc and hand-written loops side by side */
        g_copy = libc_copy; measure("copy_libc", bwv_copy);
        g_copy = copy_ldm; measure("copy_ldm", bwv_copy);
        g_copy = copy_neon; measure("copy_neon", bwv_copy);
        g_set = (set_fn) memset; measure("set_libc", bwv_set);
        g_set = set_stm; measure("set_stm", bwv_set);
        g_set = set_neon; measure("set_neon", bwv_set);
        g_copy = fm_copy; measure("copy_rk3128-mem", bwv_copy);
        g_set = fm_set; measure("set_rk3128-mem", bwv_set);
        g_read = read_neon; measure("read_neon", bwv_read);
    }
    return 0;
}

/* ---- temp ---- */
static int cmd_temp(void) {
    int found = 0;
    char p[320], b[64];
    DIR *d = opendir("/sys/class/thermal");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12)) continue;
        snprintf(p, sizeof p, "/sys/class/thermal/%s/temp", e->d_name);
        long long v = rd_ll(p);
        snprintf(p, sizeof p, "/sys/class/thermal/%s/type", e->d_name);
        int fd = open(p, O_RDONLY); ssize_t n = fd >= 0 ? read(fd, b, sizeof b - 1) : 0;
        if (fd >= 0) close(fd);
        if (n > 0 && b[n - 1] == '\n') n--;
        b[n > 0 ? n : 0] = 0;
        printf("thermal_zone=%s type=%s temp_mC=%lld\n", e->d_name, b, v);
        found++;
    }
    if (d) closedir(d);
    printf("zones=%d\n", found);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) goto usage;
    if (!strcmp(argv[1], "par")) return cmd_par(argc, argv);
    if (!strcmp(argv[1], "thr")) return cmd_thr(argc, argv);
    if (!strcmp(argv[1], "sus")) return cmd_sus(argc, argv);
    if (!strcmp(argv[1], "ramp")) return cmd_ramp(argc, argv);
    if (!strcmp(argv[1], "mem")) return cmd_mem(argc, argv);
    if (!strcmp(argv[1], "temp")) return cmd_temp();
usage:
    fprintf(stderr, "usage: rk3128bench par|thr <int|float> <n> <s> | sus <s> <n> [t] | ramp [nproc] | mem [copy|set|read|all] | temp\n");
    return 2;
}
