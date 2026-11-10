/* =========================================================================
 * RC22 RESERVE-AHEAD DECODE -- ONE nx pointer over the whole shared buffer.
 * ADDITIVE: no existing rc64 file is modified.
 *
 * Usage: rc22_reserve_decode <input.rc22rsv> <output> [no_progress]
 * ========================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "model_12.h"
#include "rc22_reserve.h"

static uint64_t r22_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static inline uint64_t r22_rdtsc(void) { return __builtin_ia32_rdtsc(); }
static uint64_t r22_freq(void) {
    uint64_t m0 = r22_now_ms(), c0 = r22_rdtsc();
    while (r22_now_ms() - m0 < 100) { }
    return (r22_rdtsc() - c0) * 10;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input.rc22rsv> <output> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && !strcmp(argv[3], "no_progress"));

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("open in"); return 1; }
    uint8_t sig[4];
    if (fread(sig, 1, 4, fin) != 4 || sig[0] != 'r' || sig[1] != '2' || sig[2] != 'R') {
        fprintf(stderr, "bad sig / not rc22 reserve-ahead\n"); fclose(fin); return 1;
    }
    int N = sig[3];
    if (N < 1 || N > R22_MAXN) { fprintf(stderr, "bad N\n"); fclose(fin); return 1; }
    uint64_t orig = 0;
    if (fread(&orig, 8, 1, fin) != 1) { fclose(fin); return 1; }
    size_t n = (size_t)orig;
    uint8_t flags = 0, rle_sym = 0;
    if (fread(&flags, 1, 1, fin) != 1 || fread(&rle_sym, 1, 1, fin) != 1) {
        fclose(fin); return 1;
    }

    if (n == 0) {
        fclose(fin);
        FILE *fo = fopen(argv[2], "wb"); if (fo) fclose(fo);
        return 0;
    }
    if (flags & 1) {
        fclose(fin);
        FILE *fo = fopen(argv[2], "wb");
        if (!fo) { perror("out"); return 1; }
        uint8_t *rbuf = (uint8_t *)malloc(16384);
        if (!rbuf) { fclose(fo); return 1; }
        memset(rbuf, rle_sym, 16384);
        size_t rem = n;
        while (rem) {
            size_t b = rem < 16384 ? rem : 16384;
            fwrite(rbuf, 1, b, fo); rem -= b;
        }
        free(rbuf); fclose(fo);
        printf("DECODE-RC22RSV OK (RLE)\n"); return 0;
    }

    model12_t M; memset(&M, 0, sizeof(M));
    if (fread(M.cums + 1, sizeof(M.cums[0]), ALPHABET_12, fin) != ALPHABET_12) {
        fprintf(stderr, "cum\n"); fclose(fin); return 1;
    }
    M.cums[0] = 0;
    r22_model_init(M.cums);

    uint32_t total_slots = 0;
    if (fread(&total_slots, 4, 1, fin) != 1) { fclose(fin); return 1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)total_slots + 16);
    if (!buf) { fclose(fin); return 1; }
    if (total_slots && fread(buf, 1, total_slots, fin) != total_slots) {
        fprintf(stderr, "rds\n"); fclose(fin); free(buf); return 1;
    }
    memset(buf + total_slots, 0, 16);
    fclose(fin);

    uint64_t freq = r22_freq();
    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("out"); free(buf); return 1; }

    uint8_t *out = (uint8_t *)malloc(n + 16);
    if (!out) { fclose(fout); free(buf); return 1; }

    uint32_t nx = 0;
    uint64_t renorms = 0, bypass_steps = 0;
    uint64_t t0 = r22_rdtsc();
    for (int s = 0; s < N; ++s) {
        size_t start = (size_t)s * n / (size_t)N;
        size_t end   = (size_t)(s + 1) * n / (size_t)N;
        r22_dec_t D;
        r22_dec_init(&D, buf, total_slots, &nx);
        for (size_t i = start; i < end; ++i)
            out[i] = r22_dec_decode(&D, buf, total_slots, &nx);
        renorms      += D.renorms;
        bypass_steps += D.bypass_steps;
    }
    uint64_t total_ticks = r22_rdtsc() - t0;

    if (n) fwrite(out, 1, n, fout);
    fclose(fout);

    uint32_t tps = n ? (uint32_t)(total_ticks / n) : 0;
    uint64_t ms = freq ? (uint64_t)(total_ticks * 1000 / freq) : 0;
    double mb_s = ms ? (double)n / (ms * 1048576.0 / 1000.0) : 0.0;

    printf("DECODE-RC22RSV OK\n");
    printf("  N         : %d\n", N);
    printf("  decode_ms : %" PRIu64 "\n", ms);
    printf("  decode_mbs: %.1f\n", mb_s);
    printf("  dec_ticks : %u\n", tps);
    printf("  slots     : %u\n", total_slots);
    printf("  nx_used   : %u\n", nx);
    printf("  renorms   : %" PRIu64 "\n", renorms);
    printf("  bypass_steps : %" PRIu64 "\n", bypass_steps);
    if (!no_progress) printf("  done\n");
    free(out); free(buf);
    return 0;
}
