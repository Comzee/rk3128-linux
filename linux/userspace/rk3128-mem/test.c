/* randomized check of fm_memcpy and fm_memset against byte loops: every size 0..600 at every src and dst offset 0..7 (and 16), guard bytes
 * either side. Run under qemu-arm. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
void *fm_memcpy(void *, const void *, size_t);
void *fm_memset(void *, int, size_t);
int main(void) {
    enum { MAXN = 700, PAD = 64 };
    static uint8_t src[MAXN + 2 * PAD + 32], dst[MAXN + 2 * PAD + 32], ref[MAXN + 2 * PAD + 32];
    unsigned seed = 12345;
    long checks = 0;
    for (size_t n = 0; n <= 600; n++) {
        for (int so = 0; so < 9; so++) for (int dof = 0; dof < 9; dof++) {
            int sof = so == 8 ? 16 : so, dof2 = dof == 8 ? 16 : dof;
            for (size_t i = 0; i < sizeof src; i++) { seed = seed * 1103515245u + 12345u; src[i] = seed >> 16; dst[i] = ref[i] = 0xA5; }
            for (size_t i = 0; i < n; i++) ref[PAD + dof2 + i] = src[PAD + sof + i];
            void *r = fm_memcpy(dst + PAD + dof2, src + PAD + sof, n);
            if (r != dst + PAD + dof2 || memcmp(dst, ref, sizeof dst)) { printf("memcpy FAIL n=%zu so=%d do=%d\n", n, sof, dof2); return 1; }
            int c = (int) (seed >> 8) & 0xff;
            memset(dst, 0x5A, sizeof dst); memset(ref, 0x5A, sizeof ref);
            for (size_t i = 0; i < n; i++) ref[PAD + dof2 + i] = (uint8_t) c;
            r = fm_memset(dst + PAD + dof2, c, n);
            if (r != dst + PAD + dof2 || memcmp(dst, ref, sizeof dst)) { printf("memset FAIL n=%zu do=%d c=%d\n", n, dof2, c); return 1; }
            checks += 2;
        }
    }
    printf("ok %ld checks\n", checks);
    return 0;
}
