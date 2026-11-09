// malitrace: an LD_PRELOAD shim that records what the vendor Mali-400 userspace blob (libGLES_mali.so, r6p0-01rel1) asks
// of the vendor kernel driver (mali.ko, /dev/mali), for development record #345 (docs/RK3128-LINUX.md "GPU: blob
// baseline"). This is the lima developers' own method: record the driver submission stream. Built with the NDK
// (scripts/mali-capture.sh, the same toolchain as scripts/build-native.sh), pushed to /data/local/tmp only, and loaded into
// an application started explicitly: LD_PRELOAD=/path/to/libmalitrace.so /path/to/application.
//
// It is a recorder, not a decoder: every ioctl on the Mali fd is written raw (the struct before and after the call, both
// times), with the mmaps of the fd, every eglSwapBuffers and the GLSL and program binaries the app hands GL. The structs
// are decoded on the build host (tools/malitrace/decode.py), so a field layout that differs between driver versions costs no
// rebuild. In the frames named by MALITRACE_DUMP it also snapshots every Mali mapping at each GP and PP job start (the
// command streams, the RSWs, the uniforms, the textures and the shader code the blob wrote), a region only when its
// content changed since its last snapshot.
//
// Environment:
//   MALITRACE_DIR    output directory (required; made 0777 by the runner script, the app drops to uid graphics)
//   MALITRACE_DUMP   "a-b,c-d,...": the frames (counted in eglSwapBuffers calls, from 1) whose GPU jobs get memory
//                    snapshots, up to 16 ranges; a lone number is a range of one (default: none)
//   MALITRACE_MAXMB  stop snapshotting after this many MB of memory data (default 400)
//   MALITRACE_PPCNT  "src0,src1": the PP performance counters every job counts (default "0,37": active clock cycles,
//                    instructions completed), read back per PP core from the finish notification; "off": none
//   MALITRACE_GPCNT  "src0,src1": the GP job's (default "1,0": source 1 counts the active cycles (it matches the job's wall
//                    time; source 0 reads 0 on this core), the second counter off)
//   The blob asks for no counters (its flag is 0), so the shim sets them in its job structs before the ioctl: the
//   hardware's own cycle counts are the GPU time of a job, without the fence wait and queueing a notification time has.
// Output: $MALITRACE_DIR/trace.bin (records, the format is "Records" below) and mem.bin.gz (snapshot data, gzip level 1:
// adb moves about 0.4 MB/s from the board; offsets in R_SNAP are into the uncompressed stream).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

// ---- Records ----
// trace.bin starts with the 8 bytes "MALITRC1". Each record: u16 type, u16 reserved, u32 payload length, u64 t (CLOCK_MONOTONIC
// ns), then the payload, all little-endian.
enum {
    R_INFO = 1,      // text: key=value lines
    R_IOCTL = 2,     // u32 fd, u32 req, s32 ret, s32 errno, u64 t_end, u32 size, in[size], out[size]
    R_MMAP = 3,      // u32 fd, u32 addr, u32 len, u32 prot, u32 flags, u64 offset, u32 result
    R_MUNMAP = 4,    // u32 addr, u32 len
    R_SWAP = 5,      // u32 frame, u64 t_end, u32 ret, u32 surface
    R_SNAP = 6,      // u32 job seq, u32 why (1 GP, 2 PP), u32 n, n x {u32 cpu, u32 len, u64 gpu_off, u64 mem_off, u64 hash}
    R_GLSRC = 7,     // u32 shader, u32 type, then the source text
    R_GLBIN = 8,     // u32 program, u32 format, s32 link status, then the binary
    R_GLATTACH = 9,  // u32 program, u32 shader
    R_EXTRA = 10,    // u32 parent req, u32 which (0 gp, 1 pp), u32 ptr, then bytes (the structs behind PP_AND_GP_START_JOB)
    R_GLUSE = 11,    // u32 program (glUseProgram in a dumped frame)
    R_GLDRAW = 12,   // u32 kind (0 arrays, 1 elements), u32 mode, u32 count, u32 program, u32 framebuffer (dumped frames)
};

#define MALI_IOC_BASE 0x82
#define MAXMAP 1024

typedef struct {
    uintptr_t cpu;
    size_t len;
    uint64_t off;   // the mmap offset: the GPU virtual address on this driver's memory system (checked in decode.py)
    uint64_t hash;  // content hash at the last snapshot (0: never)
    int prot;
} Map;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int out_fd = -1, inited, broken;
static gzFile mem_gz;
static uint64_t mem_off;
static unsigned char wbuf[1 << 18];
static size_t wlen;
static uint64_t last_flush;
static int mali_fds[64];  // 1: /dev/mali, 2: something else, 0: not looked at
static Map maps[MAXMAP];
static int nmaps;
static unsigned frame;    // eglSwapBuffers calls so far
static unsigned dump_a[16], dump_b[16], ndump, dump_last;  // the ranges; dump_last: the end of the last one (0: none)
static unsigned job_seq;
static uint64_t max_mem = 400ull << 20;
static int pp_cnt = 1, gp_cnt = 1;
static uint32_t pp_src[2] = {0, 37}, gp_src[2] = {1, 0};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void write_all(int fd, const void *p, size_t n) {
    const unsigned char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { broken = 1; return; }
        c += w;
        n -= (size_t)w;
    }
}

static void flush_locked(void) {
    if (wlen && out_fd >= 0) write_all(out_fd, wbuf, wlen);
    wlen = 0;
    last_flush = now_ns();
}

static void put_locked(const void *p, size_t n) {
    if (out_fd < 0 || broken) return;
    if (n > sizeof(wbuf)) { flush_locked(); write_all(out_fd, p, n); return; }
    if (wlen + n > sizeof(wbuf)) flush_locked();
    memcpy(wbuf + wlen, p, n);
    wlen += n;
}

// one record from up to three pieces, under the lock
static void rec3(int type, uint64_t t, const void *a, size_t na, const void *b, size_t nb, const void *c, size_t nc) {
    struct { uint16_t type, res; uint32_t len; uint64_t t; } __attribute__((packed)) h = {(uint16_t)type, 0, (uint32_t)(na + nb + nc), t};
    put_locked(&h, sizeof(h));
    if (na) put_locked(a, na);
    if (nb) put_locked(b, nb);
    if (nc) put_locked(c, nc);
    // Written at once except for the per-job records (an app killed after a quiet minute keeps its set-up and shaders)
    if ((type != R_IOCTL && type != R_EXTRA && type != R_GLDRAW && type != R_GLUSE) || now_ns() - last_flush > 500000000ull)
        flush_locked();
}

static void rec(int type, uint64_t t, const void *a, size_t na, const void *b, size_t nb, const void *c, size_t nc) {
    pthread_mutex_lock(&mu);
    rec3(type, t, a, na, b, nb, c, nc);
    pthread_mutex_unlock(&mu);
}

static void info(const char *fmt, ...) {
    char s[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s, sizeof(s), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(s)) n = sizeof(s) - 1;
    rec(R_INFO, now_ns(), s, (size_t)n, NULL, 0, NULL, 0);
}

static void finish(void) {
    pthread_mutex_lock(&mu);
    flush_locked();
    if (mem_gz) { gzclose(mem_gz); mem_gz = NULL; }
    pthread_mutex_unlock(&mu);
}

static void init_once(void) {
    if (inited) return;
    pthread_mutex_lock(&mu);
    if (inited) { pthread_mutex_unlock(&mu); return; }
    const char *dir = getenv("MALITRACE_DIR");
    if (dir && *dir) {
        char p[512];
        snprintf(p, sizeof(p), "%s/trace.bin", dir);
        out_fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        snprintf(p, sizeof(p), "%s/mem.bin.gz", dir);
        int mfd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (mfd >= 0) mem_gz = gzdopen(mfd, "wb1");
        if (out_fd >= 0) write_all(out_fd, "MALITRC1", 8);
    }
    const char *d = getenv("MALITRACE_DUMP");
    const char *dspec = d;
    if (d && *d) {
        while (*d && ndump < 16) {
            unsigned a = 0, b = 0;
            int n = 0;
            if (sscanf(d, "%u-%u%n", &a, &b, &n) == 2 && a <= b) {
            } else if (sscanf(d, "%u%n", &a, &n) == 1) {
                b = a;
            } else {
                break;
            }
            dump_a[ndump] = a;
            dump_b[ndump++] = b;
            if (b > dump_last) dump_last = b;
            d += n;
            if (*d != ',') break;
            d++;
        }
    }
    const char *c = getenv("MALITRACE_PPCNT");
    if (c && !strcmp(c, "off")) pp_cnt = 0;
    else if (c) sscanf(c, "%u,%u", &pp_src[0], &pp_src[1]);
    c = getenv("MALITRACE_GPCNT");
    if (c && !strcmp(c, "off")) gp_cnt = 0;
    else if (c) sscanf(c, "%u,%u", &gp_src[0], &gp_src[1]);
    const char *m = getenv("MALITRACE_MAXMB");
    if (m && *m) max_mem = (uint64_t)strtoull(m, NULL, 10) << 20;
    last_flush = now_ns();
    inited = 1;
    pthread_mutex_unlock(&mu);
    char cmd[256] = "";
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        for (ssize_t i = 0; i < n; i++) if (!cmd[i]) cmd[i] = ' ';
        if (n > 0) cmd[n] = 0;
    }
    info("pid=%d uid=%d cmdline=%s dump=%s maxmb=%llu ppcnt=%d:%u,%u gpcnt=%d:%u,%u", (int)getpid(), (int)getuid(), cmd,
         dspec ? dspec : "", (unsigned long long)(max_mem >> 20), pp_cnt, pp_src[0], pp_src[1], gp_cnt, gp_src[0], gp_src[1]);
    atexit(finish);
}

__attribute__((constructor)) static void malitrace_ctor(void) { init_once(); }
__attribute__((destructor)) static void malitrace_dtor(void) { finish(); }

static int is_mali(int fd) {
    if (fd < 0) return 0;
    if (fd < 64 && mali_fds[fd]) return mali_fds[fd] == 1;
    char p[32], t[64];
    snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, t, sizeof(t) - 1);
    int r = 0;
    if (n > 0) { t[n] = 0; r = strcmp(t, "/dev/mali") == 0; }
    if (fd < 64) mali_fds[fd] = r ? 1 : 2;
    return r;
}

// A copy of len bytes at addr that never faults: through a pipe of our own, whose write() returns EFAULT for a bad
// address instead of a SIGSEGV (process_vm_readv on ourselves fails on the board's 3.10). Bytes copied.
static int cpipe[2] = {-1, -1};
static pthread_mutex_t cmu = PTHREAD_MUTEX_INITIALIZER;
static size_t safe_copy(void *dst, uintptr_t addr, size_t len) {
    size_t done = 0;
    pthread_mutex_lock(&cmu);
    if (cpipe[0] < 0 && pipe2(cpipe, O_CLOEXEC | O_NONBLOCK) != 0) cpipe[0] = cpipe[1] = -1;
    while (cpipe[0] >= 0 && done < len) {
        size_t n = len - done > 4096 ? 4096 : len - done;
        ssize_t w = write(cpipe[1], (const void *)(addr + done), n);
        if (w <= 0) break;
        ssize_t r = read(cpipe[0], (char *)dst + done, (size_t)w);
        if (r != w) break;
        done += (size_t)w;
    }
    pthread_mutex_unlock(&cmu);
    return done;
}

static uint64_t hash_words(const uint32_t *w, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= w[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

// A snapshot of every readable Mali mapping (changed content only goes to mem.bin), at a GP or PP job start.
static void snapshot(int why) {
    if (!mem_gz || broken) return;
    pthread_mutex_lock(&mu);
    if (mem_off >= max_mem) { pthread_mutex_unlock(&mu); return; }
    unsigned seq = ++job_seq;
    struct { uint32_t cpu, len; uint64_t gpu, moff, hash; } __attribute__((packed)) e[MAXMAP];
    int n = 0;
    static uint32_t *tmp;
    static size_t tmp_len;
    for (int i = 0; i < nmaps && n < MAXMAP; i++) {
        Map *m = &maps[i];
        if (!(m->prot & PROT_READ)) continue;
        if (tmp_len < m->len) {
            free(tmp);
            tmp = malloc(m->len);
            tmp_len = tmp ? m->len : 0;
            if (!tmp) continue;
        }
        memcpy(tmp, (const void *)m->cpu, m->len);  // mapped and readable: munmap waits for this lock
        uint64_t h = hash_words(tmp, m->len / 4);
        e[n].cpu = (uint32_t)m->cpu;
        e[n].len = (uint32_t)m->len;
        e[n].gpu = m->off;
        e[n].hash = h;
        if (h != m->hash) {
            e[n].moff = mem_off;
            if (gzwrite(mem_gz, tmp, (unsigned)m->len) != (int)m->len) broken = 1;
            mem_off += m->len;
            m->hash = h;
        } else {
            e[n].moff = UINT64_MAX;  // same bytes as this region's last snapshot
        }
        n++;
    }
    uint32_t hd[3] = {seq, (uint32_t)why, (uint32_t)n};
    rec3(R_SNAP, now_ns(), hd, sizeof(hd), e, sizeof(e[0]) * (size_t)n, NULL, 0);
    flush_locked();
    gzflush(mem_gz, Z_SYNC_FLUSH);
    pthread_mutex_unlock(&mu);
}

static int dumping(void) {
    for (unsigned i = 0; i < ndump; i++)
        if (frame + 1 >= dump_a[i] && frame + 1 <= dump_b[i]) return 1;
    return 0;
}

// ---- ioctl ----
int ioctl(int fd, int req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    unsigned type = ((unsigned)req >> 8) & 0xff, size = ((unsigned)req >> 16) & 0x3fff, nr = (unsigned)req & 0xff;
    if (type < MALI_IOC_BASE || type > MALI_IOC_BASE + 5 || !is_mali(fd))
        return (int)syscall(__NR_ioctl, fd, req, arg);
    init_once();
    unsigned char in[4096], out[4096];
    if (size > sizeof(in)) size = sizeof(in);
    if (arg && size) memcpy(in, arg, size);  // the kernel reads the same bytes: a bad pointer is the blob's own fault
    // job starts: GP_START_JOB (GP subsystem nr 0), PP_START_JOB (PP nr 0), PP_AND_GP_START_JOB (PP nr 4)
    int gp = type == MALI_IOC_BASE + 3 && nr == 0, pp = type == MALI_IOC_BASE + 2 && nr == 0,
        both = type == MALI_IOC_BASE + 2 && nr == 4;
    if (dumping() && (gp || pp || both)) snapshot(gp ? 1 : pp ? 2 : 3);
    // the counters: GP struct u32 perf_counter_flag/src0/src1 at byte 44, PP struct at byte 332 (decode.py has the layout)
    {
        uint32_t *g = NULL, *q = NULL;
        if (gp && arg && size >= 56) g = (uint32_t *)((char *)arg + 44);
        if (pp && arg && size >= 344) q = (uint32_t *)((char *)arg + 332);
        if (both && arg && size >= 24) {
            uint32_t gpa, ppa;
            memcpy(&gpa, (char *)arg + 8, 4);
            memcpy(&ppa, (char *)arg + 16, 4);
            if (gpa) g = (uint32_t *)(uintptr_t)(gpa + 44);
            if (ppa) q = (uint32_t *)(uintptr_t)(ppa + 332);
        }
        if (g && gp_cnt && g[0] == 0) { g[0] = 3; g[1] = gp_src[0]; g[2] = gp_src[1]; }
        if (q && pp_cnt && q[0] == 0) { q[0] = 3; q[1] = pp_src[0]; q[2] = pp_src[1]; }
        if (arg && size) memcpy(in, arg, size);  // the record shows what the kernel got
    }
    if (both && size >= 24) {  // u64 ctx, u64 gp_args, u64 pp_args: the structs behind the pointers
        uint32_t gpa, ppa;
        memcpy(&gpa, in + 8, 4);
        memcpy(&ppa, in + 16, 4);
        unsigned char b[1024];
        uint32_t h[3] = {(uint32_t)req, 0, gpa};
        size_t n = gpa ? safe_copy(b, gpa, 256) : 0;
        if (n) rec(R_EXTRA, now_ns(), h, sizeof(h), b, n, NULL, 0);
        h[1] = 1;
        h[2] = ppa;
        n = ppa ? safe_copy(b, ppa, sizeof(b)) : 0;
        if (n) rec(R_EXTRA, now_ns(), h, sizeof(h), b, n, NULL, 0);
    }
    uint64_t t0 = now_ns();
    int ret = (int)syscall(__NR_ioctl, fd, req, arg);
    int err = errno;
    uint64_t t1 = now_ns();
    if (arg && size) memcpy(out, arg, size);
    struct { uint32_t fd, req; int32_t ret, err; uint64_t t1; uint32_t size; } __attribute__((packed)) h =
        {(uint32_t)fd, (uint32_t)req, ret, ret < 0 ? err : 0, t1, size};
    rec(R_IOCTL, t0, &h, sizeof(h), in, size, out, size);
    errno = err;
    return ret;
}

// ---- mmap ----
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int (*real_munmap)(void *, size_t);

static void note_map(int fd, void *addr, size_t len, int prot, int flags, uint64_t off, void *r) {
    struct { uint32_t fd, addr, len, prot, flags; uint64_t off; uint32_t res; } __attribute__((packed)) h =
        {(uint32_t)fd, (uint32_t)(uintptr_t)addr, (uint32_t)len, (uint32_t)prot, (uint32_t)flags, off, (uint32_t)(uintptr_t)r};
    pthread_mutex_lock(&mu);
    if (r != MAP_FAILED && nmaps < MAXMAP)
        maps[nmaps++] = (Map){(uintptr_t)r, len, off, 0, prot};
    rec3(R_MMAP, now_ns(), &h, sizeof(h), NULL, 0, NULL, 0);
    pthread_mutex_unlock(&mu);
}

// Only mmap: libGLES_mali.so imports mmap and munmap (not mmap64), and a __thread guard against bionic's own
// mmap -> mmap64 call would be emulated TLS on android-23, which allocates (and maps) inside the hook.
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
    void *r = real_mmap(addr, len, prot, flags, fd, off);
    if (fd >= 0 && is_mali(fd)) { init_once(); note_map(fd, addr, len, prot, flags, (uint64_t)(uint32_t)off, r); }
    return r;
}

int munmap(void *addr, size_t len) {
    if (!real_munmap) real_munmap = dlsym(RTLD_NEXT, "munmap");
    int hit = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nmaps; i++) {
        if (maps[i].cpu == (uintptr_t)addr) { maps[i] = maps[--nmaps]; hit = 1; break; }
    }
    if (hit) {
        uint32_t h[2] = {(uint32_t)(uintptr_t)addr, (uint32_t)len};
        rec3(R_MUNMAP, now_ns(), h, sizeof(h), NULL, 0, NULL, 0);
    }
    pthread_mutex_unlock(&mu);
    return real_munmap(addr, len);
}

// ---- EGL and GL ----
// The app .so and its libEGL/libGLESv2 are loaded RTLD_LOCAL by the runner, so RTLD_NEXT does not see them from here:
// the real functions come from the already loaded libraries by handle.
static void *lib(const char *name) {
    void *h = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    return h ? h : dlopen(name, RTLD_NOW);
}
static void *gles_sym(const char *s) { static void *h; if (!h) h = lib("libGLESv2.so"); return h ? dlsym(h, s) : NULL; }
static void *egl_sym(const char *s) { static void *h; if (!h) h = lib("libEGL.so"); return h ? dlsym(h, s) : NULL; }

EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    static EGLBoolean (*real)(EGLDisplay, EGLSurface);
    if (!real) real = egl_sym("eglSwapBuffers");
    init_once();
    uint64_t t0 = now_ns();
    EGLBoolean r = real ? real(d, s) : EGL_FALSE;
    uint64_t t1 = now_ns();
    pthread_mutex_lock(&mu);
    uint32_t f = ++frame;
    struct { uint32_t frame; uint64_t t1; uint32_t ret, surf; } __attribute__((packed)) h = {f, t1, (uint32_t)r, (uint32_t)(uintptr_t)s};
    rec3(R_SWAP, t0, &h, sizeof(h), NULL, 0, NULL, 0);
    flush_locked();  // every frame: an app that is killed (or idles for a minute, then is killed) keeps its records
    pthread_mutex_unlock(&mu);
    return r;
}

void glShaderSource(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len) {
    static void (*real)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    if (!real) real = gles_sym("glShaderSource");
    init_once();
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++) total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    char *all = malloc(total + 1);
    if (all) {
        size_t o = 0;
        for (GLsizei i = 0; i < count; i++) {
            size_t n = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
            memcpy(all + o, str[i], n);
            o += n;
        }
        static void (*getiv)(GLuint, GLenum, GLint *);
        if (!getiv) getiv = gles_sym("glGetShaderiv");
        GLint type = 0;
        if (getiv) getiv(sh, GL_SHADER_TYPE, &type);
        uint32_t h[2] = {sh, (uint32_t)type};
        rec(R_GLSRC, now_ns(), h, sizeof(h), all, total, NULL, 0);
        free(all);
    }
    if (real) real(sh, count, str, len);
}

void glAttachShader(GLuint prog, GLuint sh) {
    static void (*real)(GLuint, GLuint);
    if (!real) real = gles_sym("glAttachShader");
    init_once();
    uint32_t h[2] = {prog, sh};
    rec(R_GLATTACH, now_ns(), h, sizeof(h), NULL, 0, NULL, 0);
    if (real) real(prog, sh);
}

// After the real link: the program binary (GL_OES_get_program_binary; the Mali blob hands out its MBS1 container, the
// compiled GP and PP code with their symbol tables), so each blob shader is tied to the GLSL it came from.
void glLinkProgram(GLuint prog) {
    static void (*real)(GLuint);
    static void (*getiv)(GLuint, GLenum, GLint *);
    static PFNGLGETPROGRAMBINARYOESPROC getbin;
    if (!real) real = gles_sym("glLinkProgram");
    if (!getiv) getiv = gles_sym("glGetProgramiv");
    if (!getbin) {
        getbin = (PFNGLGETPROGRAMBINARYOESPROC)gles_sym("glGetProgramBinaryOES");
        if (!getbin) {
            __eglMustCastToProperFunctionPointerType (*gpa)(const char *) = egl_sym("eglGetProcAddress");
            if (gpa) getbin = (PFNGLGETPROGRAMBINARYOESPROC)gpa("glGetProgramBinaryOES");
        }
    }
    init_once();
    if (real) real(prog);
    GLint ok = 0, n = 0;
    if (getiv) {
        getiv(prog, GL_LINK_STATUS, &ok);
        getiv(prog, GL_PROGRAM_BINARY_LENGTH_OES, &n);
    }
    void *bin = n > 0 ? malloc((size_t)n) : NULL;
    GLsizei got = 0;
    GLenum fmt = 0;
    if (bin && getbin) getbin(prog, n, &got, &fmt, bin);
    uint32_t h[3] = {prog, fmt, (uint32_t)ok};
    rec(R_GLBIN, now_ns(), h, sizeof(h), bin, bin ? (size_t)got : 0, NULL, 0);
    free(bin);
}

static GLuint cur_prog;
void glUseProgram(GLuint prog) {
    static void (*real)(GLuint);
    if (!real) real = gles_sym("glUseProgram");
    cur_prog = prog;
    if (dumping()) {
        uint32_t h[1] = {prog};
        rec(R_GLUSE, now_ns(), h, sizeof(h), NULL, 0, NULL, 0);
    }
    if (real) real(prog);
}

// The draws of a dumped frame, in order, with the program and framebuffer they use: decode.py matches them to the draws
// it finds in the blob's PLBU streams (one RSW per draw), which ties each captured shader binary to its GLSL.
static GLuint cur_fbo;

static void note_draw(uint32_t kind, GLenum mode, GLsizei count) {
    uint32_t h[5] = {kind, mode, (uint32_t)count, cur_prog, cur_fbo};
    rec(R_GLDRAW, now_ns(), h, sizeof(h), NULL, 0, NULL, 0);
}

void glBindFramebuffer(GLenum target, GLuint fb) {
    static void (*real)(GLenum, GLuint);
    if (!real) real = gles_sym("glBindFramebuffer");
    cur_fbo = fb;
    if (real) real(target, fb);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    static void (*real)(GLenum, GLint, GLsizei);
    if (!real) real = gles_sym("glDrawArrays");
    if (dumping()) note_draw(0, mode, count);
    if (real) real(mode, first, count);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    static void (*real)(GLenum, GLsizei, GLenum, const void *);
    if (!real) real = gles_sym("glDrawElements");
    if (dumping()) note_draw(1, mode, count);
    if (real) real(mode, count, type, idx);
}
