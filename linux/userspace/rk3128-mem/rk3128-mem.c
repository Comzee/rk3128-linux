/* rk3128-mem: memcpy and memset tuned for the board's Cortex-A7 (development record #429, docs/RK3128-LINUX.md "CPU and memory
 * against Android"). Debian's glibc picks its NEON/VFP memcpy on this core and a plain memset; on the board they move 8 % and
 * 14 % fewer bytes a second than the ldm/stm loops Android's bionic uses (memcpy 682 against 745 MB/s, memset 1969 against 2282
 * MB/s with the DDR at 396 MHz, 32 MB buffers). These do the same job with no libc call, no dlsym and no global state, so they
 * are safe as an LD_PRELOAD from the first instruction of a process:
 *   memcpy  n < 32     overlapping word and byte stores (loads first, so no branch on the length's low bits beyond the size class)
 *           n < 256    NEON 32 B blocks (any alignment) and a 32 B tail loaded before the first store
 *           n >= 256   ldm/stm 64 B blocks with pld 192 B ahead when both pointers are word-aligned, else NEON 64 B blocks
 *   memset  n < 32     overlapping word and byte stores
 *           n >= 32    NEON 16 B stores: 64 B blocks on a 16 B aligned destination, the head and the last 64 B stored over by unaligned stores
 * memmove and every other routine stay glibc's. memcpy's buffers must not overlap (C), which is what lets the tail be loaded first.
 * Build: -O2 -fno-builtin -fno-tree-loop-distribute-patterns (the loops must not become calls to memcpy); as a preload add
 * -DRK3128_MEM_PRELOAD -shared -fPIC, which exports memcpy and memset. */
#include <stddef.h>
#include <stdint.h>
#include <arm_neon.h>

typedef uint32_t u32;
typedef uint8_t u8;

static inline void ld16(const u8 *s, u32 *o) { /* four words, unaligned (ARMv7 ldr) */
    __asm__ volatile("ldr %0, [%4]\n\tldr %1, [%4, #4]\n\tldr %2, [%4, #8]\n\tldr %3, [%4, #12]"
                     : "=&r"(o[0]), "=&r"(o[1]), "=&r"(o[2]), "=&r"(o[3]) : "r"(s) : "memory");
}
static inline void st16(u8 *d, const u32 *o) {
    __asm__ volatile("str %1, [%0]\n\tstr %2, [%0, #4]\n\tstr %3, [%0, #8]\n\tstr %4, [%0, #12]"
                     : : "r"(d), "r"(o[0]), "r"(o[1]), "r"(o[2]), "r"(o[3]) : "memory");
}

void *fm_memcpy(void *dst, const void *src, size_t n) {
    u8 *d = dst;
    const u8 *s = src;
    if (n < 32) {
        if (n >= 16) {
            u32 a[4], b[4];
            ld16(s, a); ld16(s + n - 16, b);
            st16(d, a); st16(d + n - 16, b);
        } else if (n >= 8) {
            u32 a0, a1, b0, b1;
            __asm__ volatile("ldr %0, [%4]\n\tldr %1, [%4, #4]\n\tldr %2, [%5]\n\tldr %3, [%5, #4]"
                             : "=&r"(a0), "=&r"(a1), "=&r"(b0), "=&r"(b1) : "r"(s), "r"(s + n - 8) : "memory");
            __asm__ volatile("str %0, [%2]\n\tstr %1, [%2, #4]" : : "r"(a0), "r"(a1), "r"(d) : "memory");
            __asm__ volatile("str %0, [%2]\n\tstr %1, [%2, #4]" : : "r"(b0), "r"(b1), "r"(d + n - 8) : "memory");
        } else if (n >= 4) {
            u32 a, b;
            __asm__ volatile("ldr %0, [%2]\n\tldr %1, [%3]" : "=&r"(a), "=&r"(b) : "r"(s), "r"(s + n - 4) : "memory");
            __asm__ volatile("str %0, [%2]\n\tstr %1, [%3]" : : "r"(a), "r"(b), "r"(d), "r"(d + n - 4) : "memory");
        } else if (n) {
            u8 a = s[0], b = s[n >> 1], c = s[n - 1];
            d[0] = a; d[n >> 1] = b; d[n - 1] = c;
        }
        return dst;
    }
    if (n < 256) {
        size_t blocks = n / 32;
        const u8 *tail_s = s + n - 32;
        u8 *tail_d = d + n - 32;
        __asm__ volatile(
            "vld1.8 {d4-d7}, [%0]\n\t"        /* the last 32 B first: the tail is stored over the end after the loop */
            "1:\n\t"
            "vld1.8 {d0-d3}, [%1]!\n\t"
            "vst1.8 {d0-d3}, [%2]!\n\t"
            "subs %3, %3, #1\n\t"
            "bne 1b\n\t"
            "vst1.8 {d4-d7}, [%4]\n\t"
            : "+r"(tail_s), "+r"(s), "+r"(d), "+r"(blocks) : "r"(tail_d) : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "memory", "cc");
        return dst;
    }
    {
        size_t blocks = n / 64;
        const u8 *tail_s = s + n - 64;
        u8 *tail_d = d + n - 64;
        if ((((uintptr_t) s | (uintptr_t) d) & 3) == 0) {
            __asm__ volatile(
                "vld1.8 {d0-d3}, [%0]!\n\t"
                "vld1.8 {d4-d7}, [%0]\n\t"      /* the last 64 B */
                "2:\n\t"
                "pld [%1, #192]\n\t"
                "ldmia %1!, {r4-r11}\n\t"
                "stmia %2!, {r4-r11}\n\t"
                "ldmia %1!, {r4-r11}\n\t"
                "stmia %2!, {r4-r11}\n\t"
                "subs %3, %3, #1\n\t"
                "bne 2b\n\t"
                "vst1.8 {d0-d3}, [%4]!\n\t"
                "vst1.8 {d4-d7}, [%4]\n\t"
                : "+r"(tail_s), "+r"(s), "+r"(d), "+r"(blocks), "+r"(tail_d)
                : : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "memory", "cc");
        } else {
            __asm__ volatile(
                "vld1.8 {d0-d3}, [%0]!\n\t"
                "vld1.8 {d4-d7}, [%0]\n\t"
                "3:\n\t"
                "pld [%1, #192]\n\t"
                "vld1.8 {d16-d19}, [%1]!\n\t"
                "vld1.8 {d20-d23}, [%1]!\n\t"
                "vst1.8 {d16-d19}, [%2]!\n\t"
                "vst1.8 {d20-d23}, [%2]!\n\t"
                "subs %3, %3, #1\n\t"
                "bne 3b\n\t"
                "vst1.8 {d0-d3}, [%4]!\n\t"
                "vst1.8 {d4-d7}, [%4]\n\t"
                : "+r"(tail_s), "+r"(s), "+r"(d), "+r"(blocks), "+r"(tail_d)
                : : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "memory", "cc");
        }
    }
    return dst;
}

void *fm_memset(void *dst, int c, size_t n) {
    u8 *d = dst;
    u32 v = (u8) c * 0x01010101u;
    if (n < 32) {
        if (n >= 16) {
            u32 a[4] = {v, v, v, v};
            st16(d, a); st16(d + n - 16, a);
        } else if (n >= 8) {
            __asm__ volatile("str %0, [%1]\n\tstr %0, [%1, #4]\n\tstr %0, [%2]\n\tstr %0, [%2, #4]" : : "r"(v), "r"(d), "r"(d + n - 8) : "memory");
        } else if (n >= 4) {
            __asm__ volatile("str %0, [%1]\n\tstr %0, [%2]" : : "r"(v), "r"(d), "r"(d + n - 4) : "memory");
        } else if (n) {
            d[0] = (u8) c; d[n >> 1] = (u8) c; d[n - 1] = (u8) c;
        }
        return dst;
    }
    uint8x16_t q = vdupq_n_u8((u8) c);
    u8 *end = d + n;
    if (n < 64) { /* 32 B at each end: they overlap and cover everything */
        vst1q_u8(d, q); vst1q_u8(d + 16, q); vst1q_u8(end - 32, q); vst1q_u8(end - 16, q);
        return dst;
    }
    /* head 16 B unaligned, then 64 B blocks from the first 16 B boundary at or after d (at most d + 15, so the head reaches it),
     * then the last 64 B stored over whatever the blocks left */
    vst1q_u8(d, q);
    u8 *p = (u8 *) (((uintptr_t) d + 15) & ~(uintptr_t) 15);
    size_t blocks = (size_t) (end - p) / 64;
    while (blocks--) {
        vst1q_u8(__builtin_assume_aligned(p, 16), q);
        vst1q_u8(__builtin_assume_aligned(p + 16, 16), q);
        vst1q_u8(__builtin_assume_aligned(p + 32, 16), q);
        vst1q_u8(__builtin_assume_aligned(p + 48, 16), q);
        p += 64;
    }
    vst1q_u8(end - 64, q); vst1q_u8(end - 48, q); vst1q_u8(end - 32, q); vst1q_u8(end - 16, q);
    return dst;
}

#ifdef RK3128_MEM_PRELOAD
void *memcpy(void *d, const void *s, size_t n) { return fm_memcpy(d, s, n); }
void *memset(void *d, int c, size_t n) { return fm_memset(d, c, n); }
#endif
