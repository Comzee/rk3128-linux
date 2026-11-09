// SPDX-License-Identifier: MIT
/* Diagnostic v1 PIO only. Exclusive device access held, runtime PM forced on, no crypto requests.
 * Never run beside an active hardware RNG driver. No DMA and no clock/reset writes.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static volatile uint32_t *reg;
static void wr(unsigned off, uint32_t v) { reg[off / 4] = v; }
static uint32_t rd(unsigned off) { return reg[off / 4]; }
static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
static int poll_clear(unsigned off, uint32_t mask, uint64_t ns)
{
    uint64_t end = now() + ns;
    do { if (!(rd(off) & mask)) return 0; } while (now() < end);
    fprintf(stderr, "timeout off=%x val=%08x mask=%08x\n", off, rd(off), mask);
    return -1;
}
static int hex(const char *s, uint8_t *b, unsigned n)
{
    if (strlen(s) != 2 * n) return -1;
    for (unsigned i = 0; i < n; i++) {
        unsigned x = 0;
        for (unsigned j = 0; j < 2; j++) {
            char c = s[2 * i + j];
            unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                         c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 99;
            if (d > 15) return -1;
            x = 16 * x + d;
        }
        b[i] = (uint8_t)x;
    }
    return 0;
}
static void operand(unsigned off, uint8_t *v, unsigned n)
{
    for (unsigned i = 0; i < 256; i += 4) {
        uint32_t w = 0;
        for (unsigned j = 0; j < 4; j++)
            if (i + j < n) w |= (uint32_t)v[n - i - j - 1] << (8 * j);
        wr(off + i, w);
    }
}
static int pka(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    char line[3000], s[5][513]; unsigned bits, rows = 0, failures = 0;
    uint8_t v[5][256], got[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%u %512s %512s %512s %512s %512s", &bits,
                   s[0], s[1], s[2], s[3], s[4]) != 6 ||
            (bits != 512 && bits != 1024 && bits != 2048)) return 2;
        unsigned n = bits / 8;
        for (unsigned i = 0; i < 5; i++) if (hex(s[i], v[i], n)) return 2;
        if (++rows > 256) { fclose(f); return 2; }
        printf("PKA start row=%u bits=%u\n", rows, bits); fflush(stdout);
        wr(4, 0); wr(0, 0x3f);
        wr(8, 0x00800080); wr(8, 0x00800000);
        wr(0x280, bits == 512 ? 0 : bits == 1024 ? 1 : 2);
        operand(0x400, v[0], n); operand(0x500, v[1], n);
        operand(0x600, v[2], n); operand(0x700, v[3], n);
        uint64_t start = now();
        wr(8, 0x00100010);
        if (poll_clear(8, 16, 1000000000)) { fclose(f); return 3; }
        for (unsigned i = 0; i < n; i += 4) {
            uint32_t w = rd(0x400 + i);
            for (unsigned j = 0; j < 4; j++) got[n - i - j - 1] = (uint8_t)(w >> (8 * j));
        }
        int equal = !memcmp(got, v[4], n);
        printf("PKA %s bits=%u ns=%" PRIu64 " status=%08x\n", equal ? "PASS" : "FAIL", bits, now() - start, rd(0));
        fflush(stdout);
        if (!equal) {
            printf("PKA got=");
            for (unsigned j = 0; j < n; j++) printf("%02x", got[j]);
            putchar('\n'); fflush(stdout);
        }
        failures += !equal;
    }
    fclose(f);
    for (unsigned i = 0x400; i < 0x800; i += 4) wr(i, 0);
    printf("PKA rows=%u failures=%u\n", rows, failures);
    return !rows || failures ? 1 : 0;
}
static int rng_block(uint8_t b[32])
{
    wr(8, 0x01000100);
    if (poll_clear(8, 0x100, 10000000)) return -1;
    for (unsigned j = 0; j < 32; j += 4) {
        uint32_t w = rd(0x204 + j);
        memcpy(b + j, &w, 4);
    }
    return 0;
}
static int rng(const char *path, unsigned int period, unsigned int bytes, int restarts)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { perror(path); return 1; }
    uint8_t b[32];
    uint64_t start = now();
    wr(4, 0);
    unsigned rows = restarts ? 1000 : 1, row_bytes = restarts ? 128 : bytes;
    for (unsigned row = 0; row < rows; row++) {
        wr(0x200, 0x10000 | period);
        /* Run the independent oscillator before retaining data: 1024 bits. */
        for (unsigned i = 0; i < 4; i++) {
            if (rng_block(b)) { wr(0x200, 0); close(fd); return 3; }
        }
        for (unsigned i = 0; i < row_bytes / 32; i++) {
            if (rng_block(b)) { wr(0x200, 0); close(fd); return 3; }
            if (write(fd, b, sizeof(b)) != sizeof(b)) { perror("write sample"); wr(0x200, 0); close(fd); return 3; }
        }
        wr(0x200, 0);
    }
    wr(0x200, 0); wr(8, 0x01000000);
    close(fd);
    printf("TRNG bytes=%u period=%u restarts=%u startup_discard_bits=1024 ns=%" PRIu64 "\n",
           rows * row_bytes, period, restarts ? rows : 0, now() - start);
    return 0;
}
static int sys_is(const char *path, const char *value)
{
    FILE *f = fopen(path, "r");
    char text[64];
    if (!f) return 0;
    int ok = fgets(text, sizeof(text), f) && !strcmp(text, value);
    fclose(f);
    return ok;
}
static int block_active(void)
{
    char driver[256];
    ssize_t n = readlink("/sys/bus/platform/devices/100fc000.crypto/driver",
                         driver, sizeof(driver) - 1);
    if (n < 0) return 0;
    driver[n] = 0;
    const char *name = strrchr(driver, '/');
    return name && !strcmp(name + 1, "rk3128-crypto") &&
        sys_is("/sys/bus/platform/devices/100fc000.crypto/power/control", "on\n") &&
        sys_is("/sys/bus/platform/devices/100fc000.crypto/power/runtime_status", "active\n") &&
        !sys_is("/sys/class/misc/hw_random/rng_current", "rk3128-trng\n");
}
/* Always-on GRF/CRU only: never touch the possibly clock-gated crypto window. */
static int grf_status(void)
{
    int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0) { perror("devmem"); return 1; }
    volatile uint32_t *grf = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x20008000);
    volatile uint32_t *cru = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x20000000);
    close(fd);
    if (grf == MAP_FAILED || cru == MAP_FAILED) {
        perror("mmap GRF/CRU");
        if (grf != MAP_FAILED) munmap((void *)grf, 4096);
        if (cru != MAP_FAILED) munmap((void *)cru, 4096);
        return 1;
    }
    uint32_t con = grf[0x148 / 4], status = grf[0x14c / 4];
    printf("GRF_SOC_CON2=%08x GRF_SOC_STATUS0=%08x crypto_idlereq=%u idle=%u idleack=%u\n",
           con, status, (con >> 15) & 1, (status >> 29) & 1, (status >> 21) & 1);
    printf("CRU_CLKSEL_CON24=%08x CLKGATE_CON0=%08x CLKGATE_CON3=%08x SOFTRST_CON3=%08x\n",
           cru[0xa0 / 4], cru[0xd0 / 4], cru[0xdc / 4], cru[0x11c / 4]);
    munmap((void *)grf, 4096);
    munmap((void *)cru, 4096);
    return 0;
}
int main(int argc, char **argv)
{
    unsigned int period = 1000, bytes = 1024 * 1024;
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("crypto-probe --grf-status (read-only GRF/CRU) | --rng NEW_SAMPLE [PERIOD BYTES] | --rng-restarts NEW_SAMPLE PERIOD | --pka FIXTURES: root, exclusive device access, block idle, PM on; never beside an RNG driver");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--grf-status")) return grf_status();
    int restarts = argc == 4 && !strcmp(argv[1], "--rng-restarts");
    if (restarts) {
        char *end;
        unsigned long p = strtoul(argv[3], &end, 10);
        if (*end || !p || p > 65535) return 2;
        period = p;
    }
    if (argc == 5 && !strcmp(argv[1], "--rng")) {
        char *end;
        unsigned long p = strtoul(argv[3], &end, 10);
        if (*end || !p || p > 65535) return 2;
        unsigned long n = strtoul(argv[4], &end, 10);
        if (*end || !n || n > 4 * 1024 * 1024 || n % 32) return 2;
        period = p; bytes = n;
    }
    if (!restarts && ((argc != 3 && argc != 5) || (strcmp(argv[1], "--rng") && strcmp(argv[1], "--pka")) ||
        (argc == 5 && strcmp(argv[1], "--rng")))) {
        fprintf(stderr, "usage: crypto-probe --rng NEW_SAMPLE | --pka FIXTURES (diagnostic root only, block idle, PM on, exclusive device access held)\n");
        return 2;
    }
    if (!block_active()) {
        fputs("refusing MMIO: require bound rk3128-crypto, PM on/active, and no crypto TRNG source\n", stderr);
        return 1;
    }
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) { perror("devmem"); return 1; }
    reg = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x100fc000);
    close(fd);
    if (reg == MAP_FAILED) { perror("mmap"); return 1; }
    printf("v1 ctrl=%08x conf=%08x rng=%08x pka=%08x\n", rd(8), rd(12), rd(0x200), rd(0x280)); fflush(stdout);
    int rc = !strcmp(argv[1], "--rng") || restarts ? rng(argv[2], period, bytes, restarts) : pka(argv[2]);
    munmap((void *)reg, 4096);
    return rc;
}
