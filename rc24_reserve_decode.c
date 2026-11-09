/* =========================================================================
 * RC24 RESERVE-AHEAD DECODE -- one nx pointer over the whole shared buffer.
 * ADDITIVE: no existing rc64 file is modified.
 *
 * Usage: rc24_reserve_decode <input.rc24rsv> <output> [no_progress]
 * ========================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "model_12.h"
#include "rc24_reserve.h"

static uint64_t rsv_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static inline uint64_t rsv_rdtsc(void) { return __builtin_ia32_rdtsc(); }
static uint64_t rsv_freq(void) {
    uint64_t m0 = rsv_now_ms(), c0 = rsv_rdtsc();
    while (rsv_now_ms() - m0 < 100) { }
    return (rsv_rdtsc() - c0) * 10;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input.rc24rsv> <output> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && !strcmp(argv[3], "no_progress"));

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("open in"); return 1; }
    uint8_t sig[4];
    if (fread(sig, 1, 4, fin) != 4 || sig[0] != 'r' || sig[1] != '4' || sig[2] != 'R') {
        fprintf(stderr, "bad sig / not reserve-ahead\n"); fclose(fin); return 1;
    }
    int N = sig[3];
    if (N < 1 || N > RSV_MAXN) { fprintf(stderr, "bad N\n"); fclose(fin); return 1; }
    uint64_t orig = 0;
    if (fread(&orig, 8, 1, fin) != 1) { fclose(fin); return 1; }
    size_t n = (size_t)orig;
    uint8_t flags = 0, rle_sym = 0;
    if (fread(&flags, 1, 1, fin) != 1 || fread(&rle_sym, 1, 1, fin) != 1) { fclose(fin); return 1; }

    if (n == 0) { fclose(fin); FILE *fo = fopen(argv[2], "wb"); if (fo) fclose(fo); return 0; }
    if (flags & 1) {
        fclose(fin);
        FILE *fo = fopen(argv[2], "wb");
        if (!fo) { perror("out"); return 1; }
        uint8_t *buf = (uint8_t *)malloc(16384);
        memset(buf, rle_sym, 16384);
        size_t rem = n;
        while (rem) { size_t b = rem < 16384 ? rem : 16384; fwrite(buf, 1, b, fo); rem -= b; }
        free(buf); fclose(fo);
        printf("DECODE-RC24RSV OK (RLE)\n"); return 0;
    }

    model12_t M; memset(&M, 0, sizeof(M));
    if (fread(M.cums + 1, sizeof(M.cums[0]), ALPHABET_12, fin) != ALPHABET_12) {
        fprintf(stderr, "cum\n"); fclose(fin); return 1;
    }
    M.cums[0] = 0;
    uint32_t cum15[ALPHABET_12 + 1], freq15[ALPHABET_12];
    for (int s = 0; s < ALPHABET_12; ++s) {
        cum15[s]  = (uint32_t)M.cums[s] * 8u;
        freq15[s] = (uint32_t)(M.cums[s + 1] - M.cums[s]) * 8u;
    }
    cum15[ALPHABET_12] = RSV_TOTAL;
    static uint8_t sym_of[RSV_TOTAL];
    { int s = 0; for (uint32_t v = 0; v < RSV_TOTAL; ++v) {
          while (s < ALPHABET_12 - 1 && cum15[s + 1] <= v) ++s;
          sym_of[v] = (uint8_t)s;
      } }

    uint32_t total_slots = 0;
    if (fread(&total_slots, 4, 1, fin) != 1) { fclose(fin); return 1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)total_slots + 16);
    if (!buf) { fclose(fin); return 1; }
    if (total_slots && fread(buf, 1, total_slots, fin) != total_slots) {
        fprintf(stderr, "rds\n"); fclose(fin); free(buf); return 1;
    }
    memset(buf + total_slots, 0, 16);
    fclose(fin);

    uint64_t freq = rsv_freq();
    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("out"); free(buf); return 1; }

    rsv_dec_t D[RSV_MAXN];
    uint32_t nx = 0;
    for (int s = 0; s < N; ++s) rsv_dec_init(&D[s], buf, &nx);

    size_t na = ((n + (size_t)N - 1) / (size_t)N) * (size_t)N;
    uint8_t *out = (uint8_t *)malloc(na + 16);
    if (!out) { fclose(fout); free(buf); return 1; }
    uint64_t total_ticks = 0;
    uint64_t t0 = rsv_rdtsc();
    for (size_t i = 0; i < na; i += (size_t)N) {
        for (int s = 0; s < N; ++s) {
            uint32_t r = D[s].range >> RSV_SHIFT;
            uint32_t v = D[s].code / r;
            if (v >= RSV_TOTAL) v = RSV_TOTAL - 1;
            uint8_t sym = sym_of[v];
            rsv_dec_step(&D[s], cum15[sym], freq15[sym], r, buf, &nx);
            out[i + (size_t)s] = sym;
        }
    }
    total_ticks = rsv_rdtsc() - t0;
    if (n) fwrite(out, 1, n, fout);
    fclose(fout);

    uint64_t renorms = 0, cuts = 0;
    for (int s = 0; s < N; ++s) { renorms += D[s].renorms; cuts += D[s].cuts; }
    uint32_t tps = n ? (uint32_t)(total_ticks / n) : 0;
    uint64_t ms = freq ? (uint64_t)(total_ticks * 1000 / freq) : 0;
    double mb_s = ms ? (double)n / (ms * 1048576.0 / 1000.0) : 0.0;

    printf("DECODE-RC24RSV OK\n");
    printf("  N         : %d\n", N);
    printf("  decode_ms : %" PRIu64 "\n", ms);
    printf("  decode_mbs: %.1f\n", mb_s);
    printf("  dec_ticks : %u\n", tps);
    printf("  slots     : %u\n", total_slots);
    printf("  nx_used   : %u\n", nx);
    printf("  renorms   : %" PRIu64 "\n", renorms);
    printf("  cuts      : %" PRIu64 "\n", cuts);
    if (!no_progress) printf("  done\n");
    free(out); free(buf);
    return 0;
}
