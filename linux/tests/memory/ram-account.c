// SPDX-License-Identifier: MIT
/* #536: account for every RK3128 reference board page excluded from MemTotal, without /dev/mem.
 * /proc/kpageflags supplies PageReserved; DT no-map holes are counted by union
 * because pfn_to_online_page() may hide them. CMA is reusable and is already
 * in MemTotal. The mapped boot-logo DT node outlives its release: its runtime
 * PageReserved flags, rather than its stale early-boot reservation, decide.
 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef RAM_PROC
#define RAM_PROC "/proc"
#endif
#define PAGE 4096
#define RESERVED (UINT64_C(1) << 32)
static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void die(const char *s) { perror(s); exit(1); }
static void reg(const char *path, uint32_t *base, uint32_t *size)
{
    unsigned char b[9];
    FILE *f = fopen(path, "rb");
    if (!f) die(path);
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    if (n != 8) { fprintf(stderr, "%s: expected one 32-bit base/size pair\n", path); exit(1); }
    *base = be32(b); *size = be32(b + 4);
}
static void range(uint32_t base, size_t start, size_t end, unsigned kind)
{
    printf("%08" PRIx64 "-%08" PRIx64 " %6zu KiB %s\n",
           base + (uint64_t)start * PAGE, base + (uint64_t)end * PAGE - 1,
           (end - start) * 4, kind == 3 ? "reserved+no-map" : kind == 2 ? "no-map" : "reserved");
}
int main(int argc, char **argv)
{
    bool check = argc == 2 && !strcmp(argv[1], "--check");
    if (argc > 2 || (argc == 2 && !check)) return 64;
    if (sysconf(_SC_PAGESIZE) != PAGE) { fprintf(stderr, "requires 4 KiB pages\n"); return 1; }
    uint32_t base, size;
    reg(RAM_PROC "/device-tree/memory/reg", &base, &size);
    if (!size || size > 0x40000000 || base % PAGE || size % PAGE || (uint64_t)base + size > UINT64_C(0x100000000)) return 1;
    size_t pages = size / PAGE;
    unsigned char *nomap = calloc(pages, 1);
    uint64_t *flags = calloc(pages, sizeof(*flags));
    if (!nomap || !flags) die("calloc");
    DIR *d = opendir(RAM_PROC "/device-tree/reserved-memory");
    if (!d) die("reserved-memory");
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[512];
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), RAM_PROC "/device-tree/reserved-memory/%s/no-map", e->d_name);
        if (access(p, F_OK)) continue;
        snprintf(p, sizeof(p), RAM_PROC "/device-tree/reserved-memory/%s/reg", e->d_name);
        uint32_t lo, len;
        reg(p, &lo, &len);
        if (lo < base || (uint64_t)lo + len > (uint64_t)base + size || lo % PAGE || len % PAGE) return 1;
        memset(nomap + (lo - base) / PAGE, 1, len / PAGE);
    }
    closedir(d);
    int fd = open(RAM_PROC "/kpageflags", O_RDONLY);
    if (fd < 0) die("kpageflags");
    size_t bytes = pages * sizeof(*flags), done = 0;
    while (done < bytes) {
        ssize_t n = pread(fd, (char *)flags + done, bytes - done,
                          (off_t)(base / PAGE) * 8 + (off_t)done);
        if (n <= 0) die("pread kpageflags");
        done += (size_t)n;
    }
    close(fd);
    FILE *f = fopen(RAM_PROC "/meminfo", "r");
    if (!f) die("meminfo");
    char line[256]; unsigned long total = 0;
    while (fgets(line, sizeof(line), f)) if (sscanf(line, "MemTotal: %lu kB", &total) == 1) break;
    fclose(f);
    size_t reserved = 0, holes = 0, excluded = 0, start = 0, logo_reserved = 0;
    unsigned previous = 0;
    for (size_t i = 0; i <= pages; i++) {
        unsigned kind = i == pages ? 0 : (flags[i] & RESERVED ? 1 : 0) | (nomap[i] ? 2 : 0);
        if (kind != previous) {
            if (previous) range(base, start, i, previous);
            start = i; previous = kind;
        }
        if (i == pages) break;
        reserved += !!(kind & 1); holes += !!(kind & 2); excluded += !!kind;
        uint64_t addr = base + (uint64_t)i * PAGE;
        if (addr >= 0x9dc00000 && addr < 0x9fc00000 && kind) logo_reserved++;
    }
    long long residual = (long long)size / 1024 - (long long)total - (long long)excluded * 4;
    printf("physical=%u KiB MemTotal=%lu KiB PageReserved=%zu KiB no-map=%zu KiB union=%zu KiB unexplained=%lld KiB logo-excluded=%zu KiB\n",
           size / 1024, total, reserved * 4, holes * 4, excluded * 4, residual, logo_reserved * 4);
    free(nomap); free(flags);
    if (!total) return 1;
    if (check && (total < 1023776 || residual || logo_reserved)) return 1;
    return 0;
}
