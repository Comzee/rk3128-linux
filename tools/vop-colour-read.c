/* #526: read-only RK3128 colour-state snapshot. Never acquire display ownership or write MMIO.
 * Linux: /dev/mem O_RDONLY, PROT_READ. Android's 3.10 kernel lacks CONFIG_DEVMEM: build the
 * same reader as a temporary module with scripts/colour-probe-build.sh (ABI checked there).
 * The module prints one snapshot and returns -ECANCELED: no installed module, thread or node.
 * Exclusive device access required. Only sample the static, repeating colour fixtures, not arbitrary video.
 */
#ifdef VOP_ANDROID_MODULE
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;
extern void *__arm_ioremap(unsigned long, unsigned long, unsigned int);
extern void __arm_iounmap(void *);
extern void *mem_map;
extern unsigned long __pv_phys_offset;
extern int pfn_valid(unsigned long);
extern void *kmap(void *);
extern void kunmap(void *);
extern void *kmap_to_page(void *);
extern int printk(const char *, ...) __attribute__((format(printf, 1, 2)));
#define OUT(fmt, ...) printk("\0016vop-colour: " fmt, ##__VA_ARGS__)
static void *map_read(uint32_t addr, uint32_t len) { return __arm_ioremap(addr, len, 0); }
static void unmap_read(void *p, uint32_t len) { (void)len; __arm_iounmap(p); }
#else
#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#define OUT(...) printf("vop-colour: " __VA_ARGS__)
static int memfd;
static off_t file_size = -1;
static void *map_read(uint32_t addr, uint32_t len) {
    if (file_size >= 0 && (uint64_t)addr + len > (uint64_t)file_size) return 0;
    void *p = mmap(0, len, PROT_READ, MAP_SHARED, memfd, addr);
    return p == MAP_FAILED ? 0 : p;
}
static void unmap_read(void *p, uint32_t len) { munmap(p, len); }
#endif

static void *map_pixels(uint32_t addr, uint32_t len, uint32_t *span) {
    uint32_t base = addr & ~4095u, off = addr - base;
    /* Only the board's DRAM, at most one 1080p RGB frame. Reject stale/IOVA addresses. */
    if (base < 0x60000000u || base >= 0xa0000000u || len > 1920u * 1088u * 4u ||
        len > 0xa0000000u - addr) return 0;
    *span = (len + off + 4095u) & ~4095u;
#ifdef VOP_ANDROID_MODULE
    /* Normal RAM must use kmap, not ioremap (which rejects valid RAM on ARM).
     * sizeof(struct page)=32 in the shipped mali.ko's DWARF; __pv_phys_offset
     * is the actual boot-patched PHYS_OFFSET, including on a highmem kernel.
     * Only single pages are sampled, and kmap/kunmap are balanced for each one.
     */
    if (*span != 4096 || base < __pv_phys_offset || !pfn_valid(base >> 12)) return 0;
    return kmap((uint8_t *)mem_map + ((base - __pv_phys_offset) >> 12) * 32);
#else
    return map_read(base, *span);
#endif
}

static void unmap_pixels(void *p, uint32_t span) {
#ifdef VOP_ANDROID_MODULE
    (void)span; kunmap(kmap_to_page(p));
#else
    unmap_read(p, span);
#endif
}
/* Vendor rockchip_iommu_iova_to_phys (0xc05c8948), corroborated by the v1
 * rockchip-iommu.c: 1024 DTEs, 1024 PTEs, valid bit 0, physical page in 31:12.
 * Android enables the VOP MMU, so a WIN0 address is not necessarily physical.
 * Walk only the sampled pages; never touch the MMU command/zap/interrupt registers.
 */
static uint32_t translate(uint32_t addr, uint32_t directory) {
    if (!directory) return addr;
    if (directory & 4095) return 0;
    uint32_t span = 0;
    volatile const uint32_t *dt = map_pixels(directory, 4096, &span);
    if (!dt) return 0;
    uint32_t dte = dt[addr >> 22];
    unmap_pixels((void *)dt, span);
    if (!(dte & 1)) return 0;
    volatile const uint32_t *pt = map_pixels(dte & ~4095u, 4096, &span);
    if (!pt) return 0;
    uint32_t pte = pt[(addr >> 12) & 1023];
    unmap_pixels((void *)pt, span);
    return pte & 1 ? (pte & ~4095u) | (addr & 4095) : 0;
}

static int read_sample(uint32_t addr, uint32_t directory, uint8_t *out, unsigned int n) {
    uint32_t phys = translate(addr, directory), span = 0;
    uint8_t *m = map_pixels(phys, n, &span);
    if (!m || (phys & 4095) + n > 4096) { if (m) unmap_pixels(m, span); return 0; }
    volatile const uint8_t *p = m + (phys & 4095);
    for (unsigned int i = 0; i < n; i++) out[i] = p[i];
    unmap_pixels(m, span);
    return 1;
}

static int snapshot(void) {
    volatile const uint32_t *vop = map_read(0x1010e000u, 4096);
    uint32_t r[56];
    if (!vop) { OUT("ERROR map VOP\n"); return 1; }
    OUT("BEGIN\n");
    for (unsigned int i = 0; i < 56; i++) {
        r[i] = vop[i];
        OUT("reg %03x %08x\n", i * 4, r[i]);
    }
    for (unsigned int i = 0; i < 256; i++) OUT("lut %03u %08x\n", i, vop[0xc00 / 4 + i]);
    uint32_t directory = 0;
#ifdef VOP_ANDROID_MODULE
    /* VOP MMU APB reads echo the last data (observed after the LUT read). The
     * vendor's stall/paging code also skips its status reads for rk312x_vop_mmu_base.
     * Use the physical pgtable from this boot's kernel attach log,
     * supplied at build time by the host collector while holding the board lock.
     */
    if (r[0x2c / 4] & (1u << 10)) directory = VOP_ANDROID_DIRECTORY;
#else
    if ((r[0x2c / 4] & (1u << 10)) && (vop[0x304 / 4] & 1)) directory = vop[0x300 / 4];
#endif
    OUT("mmu_directory %08x\n", directory);
    /* Windows must be full-size and unscaled for sampling; these are the fixture's centres. */
    uint32_t fmt = (r[0] >> 3) & 7, stride = (r[0x30 / 4] & 0x1fff) * 4;
    uint32_t uvstride = ((r[0x30 / 4] >> 16) & 0x1fff) * 4;
    if ((r[0] & 1) && r[0x34 / 4] == 0x0437077f && r[0x38 / 4] == 0x0437077f &&
        r[0x40 / 4] == 0x10001000 && (fmt == 4 || fmt == 0) &&
        stride >= 1920u * (fmt == 0 ? 4 : 1) && stride <= 8192u &&
        (fmt == 0 || (uvstride >= 1920 && uvstride <= 8192))) {
        for (unsigned int i = 0; i < 32; i++) {
            unsigned int x = (i % 8) * 240 + 120, row = (i / 8) * 270 + 134;
            uint8_t y[4], c[2];
            if (fmt == 4) {
                unsigned int off = (row / 2) * uvstride + x;
                if (!read_sample(r[0x20 / 4] + row * stride + x, directory, y, 1) ||
                    !read_sample(r[0x24 / 4] + off, directory, c, 2)) {
                    OUT("ERROR sample %u mapping\n", i); break;
                }
                OUT("sample %02u yuv %u %u %u\n", i, y[0], c[0], c[1]);
            } else {
                unsigned int off = row * stride + x * 4;
                if (!read_sample(r[0x20 / 4] + off, directory, y, 4)) {
                    OUT("ERROR sample %u mapping\n", i); break;
                }
                OUT("sample %02u rgb-bytes %u %u %u %u\n", i, y[0], y[1], y[2], y[3]);
            }
        }
    } else OUT("samples unavailable: win0 format/geometry (fmt=%u stride=%u uvstride=%u)\n", fmt, stride, uvstride);
    /* Report if a flip changed addresses/state during the read. Static fixtures repeat pixels. */
    OUT("stable %u\n", r[0] == vop[0] && r[1] == vop[1] && r[2] == vop[2] &&
        r[8] == vop[8] && r[9] == vop[9]);
    OUT("END\n");
    unmap_read((void *)vop, 4096);
    return 0;
}

#ifdef VOP_ANDROID_MODULE
int init_module(void) { snapshot(); return -125; /* ECANCELED: free the temporary reader immediately */ }
#else
int main(int argc, char **argv) {
    if (argc > 2) { fprintf(stderr, "usage: vop-colour-read [memory-file]\n"); return 2; }
    memfd = open(argc == 2 ? argv[1] : "/dev/mem", O_RDONLY | O_SYNC);
    if (memfd < 0) { perror("vop-colour-read"); return 1; }
    struct stat st;
    if (fstat(memfd, &st) != 0) { perror("vop-colour-read: fstat"); close(memfd); return 1; }
    if (S_ISREG(st.st_mode)) file_size = st.st_size;
    int rc = snapshot();
    close(memfd);
    return rc;
}
#endif
