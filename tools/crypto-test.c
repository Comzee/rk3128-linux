// SPDX-License-Identifier: MIT
/* Bounded AF_ALG KAT/benchmark client. No hardware registers or credentials. */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <linux/if_alg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LIMIT 65536
static void die(const char *s) { perror(s); exit(1); }
static unsigned unhex(const char *s, uint8_t *out, unsigned limit)
{
    size_t n = strlen(s);
    if (!strcmp(s, "-")) return 0;
    if ((n & 1) || n / 2 > limit) return UINT32_MAX;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; j++) {
            unsigned char c = (unsigned char)s[2 * i + j];
            unsigned x = c >= '0' && c <= '9' ? c - '0' :
                         c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                         c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
            if (x > 15) return UINT32_MAX;
            v = v * 16 + x;
        }
        out[i] = (uint8_t)v;
    }
    return (unsigned)n / 2;
}
static int transform(const char *type, const char *name, const uint8_t *key,
                     unsigned keylen)
{
    struct sockaddr_alg sa = { .salg_family = AF_ALG };
    if (strlen(type) >= sizeof(sa.salg_type) || strlen(name) >= sizeof(sa.salg_name)) {
        errno = EINVAL; return -1;
    }
    strcpy((char *)sa.salg_type, type);
    strcpy((char *)sa.salg_name, name);
    int fd = socket(AF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (bind(fd, (void *)&sa, sizeof(sa)) ||
        (keylen && setsockopt(fd, SOL_ALG, ALG_SET_KEY, key, keylen))) {
        int e = errno; close(fd); errno = e; return -1;
    }
    return fd;
}
static int operation(int fd, int hash, int decrypt, uint8_t *in, unsigned n,
                     uint8_t *out, unsigned outlen, const uint8_t *iv,
                     unsigned ivlen, int fragmented)
{
    union { struct cmsghdr align; unsigned char b[CMSG_SPACE(4) + CMSG_SPACE(4 + 16)]; } control;
    memset(&control, 0, sizeof(control));
    struct iovec iov[3] = {{ in, n }};
    struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 1 };
    if (fragmented && n >= 3) {
        iov[0].iov_len = 1;
        iov[1] = (struct iovec){in + 1, n - 2};
        iov[2] = (struct iovec){in + n - 1, 1};
        msg.msg_iovlen = 3;
    }
    if (!hash) {
        msg.msg_control = control.b;
        msg.msg_controllen = CMSG_SPACE(4) + (ivlen ? CMSG_SPACE(4 + ivlen) : 0);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_ALG; c->cmsg_type = ALG_SET_OP; c->cmsg_len = CMSG_LEN(4);
        uint32_t op = decrypt ? ALG_OP_DECRYPT : ALG_OP_ENCRYPT;
        memcpy(CMSG_DATA(c), &op, 4);
        if (ivlen) {
            c = CMSG_NXTHDR(&msg, c);
            c->cmsg_level = SOL_ALG; c->cmsg_type = ALG_SET_IV;
            c->cmsg_len = CMSG_LEN(4 + ivlen);
            struct af_alg_iv *a = (void *)CMSG_DATA(c);
            a->ivlen = ivlen; memcpy(a->iv, iv, ivlen);
        }
    }
    int opfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
    if (opfd < 0) return -1;
    ssize_t sent = sendmsg(opfd, &msg, 0);
    ssize_t got = -1;
    if (sent == (ssize_t)n) got = recv(opfd, out, outlen, MSG_WAITALL);
    int err = errno;
    close(opfd);
    if (sent != (ssize_t)n || got != (ssize_t)outlen) {
        errno = got < 0 ? err : EIO; return -1;
    }
    return 0;
}

static int vectors(const char *path, int generic)
{
    FILE *f = fopen(path, "r");
    if (!f) die("vectors");
    char *line = malloc(4 * LIMIT + 514); unsigned count = 0, row = 0;
    size_t total = 0;
    uint8_t *in = malloc(LIMIT + 32), *expected = malloc(LIMIT + 32), *out = malloc(LIMIT + 32);
    if (!line || !in || !expected || !out) die("malloc");
    while (fgets(line, 4 * LIMIT + 514, f)) {
        size_t bytes = strlen(line);
        total += bytes;
        if (++row > 512 || total > 20 * 1024 * 1024) {
            fprintf(stderr, "oversized vector file\n"); return 2;
        }
        if (bytes > 4 * LIMIT + 512) { fprintf(stderr, "oversized vector line\n"); return 2; }
        if (line[0] == '#' || line[0] == '\n') continue;
        char *v[8], *save = NULL; unsigned nv = 0;
        for (char *p = strtok_r(line, " \t\r\n", &save); p && nv < 8; p = strtok_r(NULL, " \t\r\n", &save))
            v[nv++] = p;
        if (nv != 7) { fprintf(stderr, "invalid row %u\n", row); return 2; }
        uint8_t key[32], iv[16];
        unsigned kl = unhex(v[3], key, 32), il = unhex(v[4], iv, 16);
        unsigned n = unhex(v[5], in, LIMIT), want = unhex(v[6], expected, LIMIT);
        if (kl == UINT32_MAX || il == UINT32_MAX || n == UINT32_MAX || want == UINT32_MAX ||
            (strcmp(v[0], "hash") && strcmp(v[0], "skcipher"))) return 2;
        const char *name = generic ? v[1] : v[2];
        int fd = transform(v[0], name, key, kl);
        if (fd < 0) { fprintf(stderr, "bind %s: ", name); die("AF_ALG"); }
        int hash = !strcmp(v[0], "hash");
        for (int fragmented = 0; fragmented < 2; fragmented++) {
            memset(out, 0xa5, LIMIT + 32);
            if (operation(fd, hash, 0, in, n, out, want, iv, il, fragmented)) die(name);
            if (memcmp(out, expected, want) || out[want] != 0xa5) {
                fprintf(stderr, "FAIL row=%u alg=%s len=%u fragmented=%d\n", row, name, n, fragmented);
                for (unsigned j = 0; j < (want < 32 ? want : 32); j++) fprintf(stderr, "%02x", out[j]);
                fprintf(stderr, "\n"); return 1;
            }
            count++;
            if (!hash) {
                /* Receive into the buffer just sent: same user address, no second fixture. */
                memcpy(out, expected, want);
                if (operation(fd, 0, 1, out, want, out, n, iv, il, fragmented)) die(name);
                if (memcmp(out, in, n)) { fprintf(stderr, "FAIL decrypt row=%u %s\n", row, name); return 1; }
                count++;
            }
        }
        close(fd);
    }
    if (ferror(f)) die("read vectors");
    fclose(f); free(in); free(expected); free(out); free(line);
    printf("PASS vectors=%u operations=%u driver=%s\n", row, count, generic ? "generic" : "explicit");
    return count ? 0 : 2;
}

static unsigned number(const char *s, unsigned max)
{
    char *end; errno = 0; unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || s[0] == '-' || n == 0 || n > max) { fprintf(stderr, "invalid number: %s\n", s); exit(2); }
    return (unsigned)n;
}
static double stamp(void)
{
    struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock");
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static int bench(const char *type, const char *name, unsigned n, unsigned loops)
{
    uint8_t in[LIMIT], out[LIMIT], key[32], iv[16] = {0};
    for (unsigned i = 0; i < sizeof(in); i++) in[i] = (uint8_t)(i * 73 + 19);
    for (unsigned i = 0; i < sizeof(key); i++) key[i] = (uint8_t)(i * 19 + 7);
    int hash = !strcmp(type, "hash");
    if (!hash && strcmp(type, "skcipher")) return 2;
    unsigned kl = hash ? 0 : strstr(name, "des3") ? 24 : strstr(name, "des") ? 8 : 16;
    unsigned il = hash || !strncmp(name, "ecb", 3) ? 0 : strstr(name, "des") ? 8 : 16;
    unsigned ol = hash ? strstr(name, "sha256") ? 32 : strstr(name, "sha1") ? 20 : 16 : n;
    int fd = transform(type, name, key, kl);
    if (fd < 0) die(name);
    double start = stamp();
    for (unsigned i = 0; i < loops; i++)
        if (operation(fd, hash, 0, in, n, out, ol, iv, il, 0)) die(name);
    double elapsed = stamp() - start;
    close(fd);
    printf("BENCH %s bytes=%u loops=%u seconds=%.6f MiB_s=%.3f\n", name, n, loops,
           elapsed, (double)n * loops / elapsed / (1024 * 1024));
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--selftest")) {
        uint8_t b[2];
        if (unhex("a50F", b, 2) != 2 || b[0] != 0xa5 || b[1] != 15 ||
            unhex("00", b, 0) != UINT32_MAX || unhex("0g", b, 2) != UINT32_MAX ||
            unhex("f", b, 2) != UINT32_MAX || unhex("-", b, 2) != 0) return 1;
        puts("PASS bounded hex parser"); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--vectors")) return vectors(argv[2], 0);
    if (argc == 3 && !strcmp(argv[1], "--generic-vectors")) return vectors(argv[2], 1);
    if (argc == 6 && !strcmp(argv[1], "--bench"))
        return bench(argv[2], argv[3], number(argv[4], LIMIT), number(argv[5], 10000));
    fprintf(stderr, "usage: crypto-test --selftest | --[generic-]vectors FILE | --bench hash|skcipher NAME BYTES LOOPS\n");
    return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
}
