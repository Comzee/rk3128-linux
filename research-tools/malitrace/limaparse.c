// limaparse: prints a command stream, render state or texture descriptor the Mali blob wrote (captured by malitrace, cut
// out by tools/malitrace/decode.py) through Mesa lima's own parser (src/gallium/drivers/lima/lima_parser.c, the code
// behind LIMA_DEBUG=dump), so the blob's streams read exactly like lima's dumps. Built on the build host against a Mesa build by
// tools/malitrace/lima-compare.sh (#345). Host tool only.
//   limaparse vs|plbu|rsw|tex|frame|fs|gp <file> <gpu va of the first byte> [offset]
//   (frame: the 23 PP frame registers; fs, gp: disassembly of PP code (to its last instruction) or GP code (the file))
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lima_parse_vs(FILE *fp, uint32_t *data, int size, uint32_t start);
void lima_parse_plbu(FILE *fp, uint32_t *data, int size, uint32_t start);
void lima_parse_render_state(FILE *fp, uint32_t *data, int size, uint32_t start);
void lima_parse_texture_descriptor(FILE *fp, uint32_t *data, int size, uint32_t start, uint32_t offset);
void lima_parse_shader(FILE *fp, uint32_t *data, int size, bool is_frag);
#include "lima_pack.h"  // genxml, from the Mesa build directory

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: limaparse vs|plbu|rsw|tex <file> <gpu va> [offset]\n");
        return 2;
    }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *d = calloc(1, (size_t)n + 64);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "limaparse: short read\n"); return 1; }
    fclose(f);
    uint32_t va = (uint32_t)strtoul(argv[3], NULL, 0), off = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 0;
    if (!strcmp(argv[1], "vs")) lima_parse_vs(stdout, d, (int)n, va);
    else if (!strcmp(argv[1], "plbu")) lima_parse_plbu(stdout, d, (int)n, va);
    else if (!strcmp(argv[1], "rsw")) lima_parse_render_state(stdout, d, (int)n, va);
    else if (!strcmp(argv[1], "tex")) lima_parse_texture_descriptor(stdout, d, (int)n, va, off);
    else if (!strcmp(argv[1], "fs")) lima_parse_shader(stdout, d, (int)n, true);
    else if (!strcmp(argv[1], "gp")) lima_parse_shader(stdout, d, (int)n, false);
    else if (!strcmp(argv[1], "frame")) {
        struct LIMA_PP_FRAME fr;
        LIMA_PP_FRAME_unpack(stdout, (const uint8_t *)d, &fr);
        LIMA_PP_FRAME_print(stdout, &fr, 8);
    }
    else { fprintf(stderr, "limaparse: unknown kind %s\n", argv[1]); return 2; }
    return 0;
}
