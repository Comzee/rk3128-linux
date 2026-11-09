/* Diagnostic only: caller pins IEP runtime PM with power/control=on first.
 * Reads the IEP's own command/address registers, never writes MMIO. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void)
{
    char state[32] = {0};
    FILE *f = fopen("/sys/devices/platform/10108000.iep/power/runtime_status", "r");
    if (!f || !fgets(state, sizeof(state), f)) return 2;
    fclose(f);
    if (strcmp(state, "active\n")) { fputs("IEP runtime PM must be held active\n", stderr); return 2; }
    int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    volatile uint32_t *regs = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x10108000);
    if (regs == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    for (unsigned off = 0; off <= 0x5fc; off += 4) {
        if (off >= 8 && off < 0x20) continue;
        if (off > 0x54 && off < 0x80) continue;
        printf("%03x %08x\n", off, regs[off / 4]);
    }
    munmap((void *)regs, 4096); close(fd); return 0;
}
