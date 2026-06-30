/* =========================================================================
 * RC24 2-WAY DECODE — Two interleaved RC24 streams in one shared buffer
 *
 * Usage:
 *   rc24_2way_decode <input.rc24s2w> <output> [no_progress]
 *
 * Format matches rc24s_encode.c with N=2 (shared interleaved buffer).
 * Even output positions come from stream 0, odd from stream 1.
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"
#include "rc24_codec.h"
#include "model_12.h"

#define N 2
#define OUT_BUF_SIZE (16 * 1024)

int main(int argc, char **argv) {
    static uint8_t out_buf[OUT_BUF_SIZE];
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input.rc24s2w> <output> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && !strcmp(argv[3], "no_progress"));

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "open\n"); return 1; }
    uint8_t sig[4];
    if (!zpl_file_read(&fin, sig, 4)) { fprintf(stderr, "sig\n"); zpl_file_close(&fin); return 1; }
    if (sig[0] != 'r' || sig[1] != '4' || (sig[2] & 0x02) == 0) {
        fprintf(stderr, "bad sig / not 2-way\n"); zpl_file_close(&fin); return 1;
    }
    int is_rle = sig[3] & 1;
    uint8_t rle_sym = sig[3];

    uint64_t orig = 0;
    if (!zpl_file_read(&fin, &orig, 8)) { fprintf(stderr, "len\n"); zpl_file_close(&fin); return 1; }
    size_t n = (size_t)orig;
    if (n == 0) { zpl_file_close(&fin); zpl_file fo; zpl_file_create(&fo, argv[2]); zpl_file_close(&fo); return 0; }

    if (is_rle) {
        zpl_file_close(&fin); zpl_file fo;
        if (zpl_file_create(&fo, argv[2])) { fprintf(stderr, "out\n"); return 1; }
        memset(out_buf, rle_sym, OUT_BUF_SIZE);
        size_t rem = n;
        while (rem > 0) { size_t b = rem < OUT_BUF_SIZE ? rem : OUT_BUF_SIZE; if (!zpl_file_write(&fo, out_buf, b)) { fprintf(stderr, "wr\n"); zpl_file_close(&fo); return 1; } rem -= b; }
        zpl_file_close(&fo);
        printf("DECODE-RC24S2W OK (RLE)\n"); return 0;
    }

    model12_t M; memset(&M, 0, sizeof(M));
    if (!zpl_file_read(&fin, M.cums + 1, sizeof(M.cums[0]) * ALPHABET_12)) { fprintf(stderr, "cum\n"); zpl_file_close(&fin); return 1; }
#ifndef DISABLE_LUT
    { int s = 0; for (unsigned v = 0; v <= TARGET_TOTAL_12; v++) { while (s < ALPHABET_12 - 1 && (int)M.cums[s + 1] <= (int)v) s++; M.lut[v] = (uint8_t)s; } }
#endif

    rc_div24_lut_init();

    uint8_t dec_sym_tbl[TARGET_TOTAL_12];
    uint16_t dec_cum_tbl[TARGET_TOTAL_12], dec_freq_tbl[TARGET_TOTAL_12];
    {
        int s = 0;
        for (unsigned v = 0; v < TARGET_TOTAL_12; v++) {
            while (s < ALPHABET_12 - 1 && (int)M.cums[s + 1] <= (int)v) s++;
            dec_sym_tbl[v]  = (uint8_t)s;
            dec_cum_tbl[v]  = M.cums[s];
            dec_freq_tbl[v] = (uint16_t)(M.cums[s + 1] - M.cums[s]);
        }
    }

    uint32_t max_len = 0;
    if (!zpl_file_read(&fin, &max_len, 4)) { fprintf(stderr, "maxlen\n"); zpl_file_close(&fin); return 1; }

    uint8_t *shared = malloc((size_t)max_len * N + 16);
    if (!shared) { fprintf(stderr, "malloc\n"); zpl_file_close(&fin); return 1; }
    memset(shared + (size_t)max_len * N, 0, 16);
    if (max_len > 0) {
        uint8_t *p = shared; int64_t r = (int64_t)max_len * N;
        while (r > 0) { uint32_t c = (uint32_t)zpl_min(r, 0x1000000LL); if (!zpl_file_read(&fin, p, c)) { fprintf(stderr, "rds\n"); return 1; } p += c; r -= c; }
    }
    zpl_file_close(&fin);

    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0_ticks = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 freq = (zpl_rdtsc() - d0_ticks) * 10;
    printf("CPU freq = %1.1f Mhz\n", freq / 1000000.0f);

    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "out\n"); free(shared); return 1; }

    uint32_t dlow[N], drange[N], dcode[N];
    size_t dpos[N];
    for (int s = 0; s < N; s++) {
        dlow[s] = 0; drange[s] = RC24_MASK; dcode[s] = 0; dpos[s] = 0;
        for (int j = 0; j < 3; j++) {
            dcode[s] = ((dcode[s] << 8) | (uint32_t)shared[dpos[s] * N + s]) & RC24_MASK;
            dpos[s]++;
        }
    }

    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i != n) {
        uint32_t blk = (uint32_t)zpl_min((zpl_i64)(n - i), (zpl_i64)OUT_BUF_SIZE);
        blk &= ~1u;
        zpl_u64 t0 = zpl_rdtsc();
        for (uint32_t k = 0; k < blk; k += 2) {
            drange[0] /= RC24_TOTAL;
            drange[1] /= RC24_TOTAL;
            uint32_t cum0 = rc_fast_div24(dcode[0] - dlow[0], drange[0]);
            uint32_t cum1 = rc_fast_div24(dcode[1] - dlow[1], drange[1]);
            out_buf[k]     = dec_sym_tbl[cum0];
            out_buf[k + 1] = dec_sym_tbl[cum1];

            uint32_t low0 = dlow[0] + dec_cum_tbl[cum0] * drange[0];
            uint32_t range0 = drange[0] * dec_freq_tbl[cum0];
            while (1) {
                uint32_t diff = low0 ^ (low0 + range0);
                if (diff < RC24_TOP) {
                } else if (range0 < RC24_BOT) {
                    range0 = -low0 & (RC24_BOT - 1);
                } else {
                    dlow[0] = low0; drange[0] = range0;
                    break;
                }
                dcode[0] = ((dcode[0] << 8) | (uint32_t)shared[dpos[0] * N + 0]) & RC24_MASK;
                range0 <<= 8; low0 = (low0 << 8) & RC24_MASK;
                dpos[0]++;
            }

            uint32_t low1 = dlow[1] + dec_cum_tbl[cum1] * drange[1];
            uint32_t range1 = drange[1] * dec_freq_tbl[cum1];
            while (1) {
                uint32_t diff = low1 ^ (low1 + range1);
                if (diff < RC24_TOP) {
                } else if (range1 < RC24_BOT) {
                    range1 = -low1 & (RC24_BOT - 1);
                } else {
                    dlow[1] = low1; drange[1] = range1;
                    break;
                }
                dcode[1] = ((dcode[1] << 8) | (uint32_t)shared[dpos[1] * N + 1]) & RC24_MASK;
                range1 <<= 8; low1 = (low1 << 8) & RC24_MASK;
                dpos[1]++;
            }
        }
        total_ticks += zpl_rdtsc() - t0;
        if (!zpl_file_write(&fout, out_buf, blk)) { fprintf(stderr, "wr\n"); zpl_file_close(&fout); free(shared); return 1; }
        i += blk;
        if (!no_progress) { printf("\r                      \r%zu / %zu  (%3.1f%%)", i, n, 100.0 * (double)i / (double)n); fflush(stdout); }
    }
    if (!no_progress) printf("\n");

    zpl_file_close(&fout); free(shared);

    uint32_t tps = n ? (uint32_t)(total_ticks / n) : 0;
    uint64_t ms = freq ? (uint64_t)(total_ticks * 1000 / freq) : 0;
    double mb_s = ms ? (double)n / (ms * 1048576.0 / 1000.0) : 0.0;

    printf("DECODE-RC24S2W OK\n");
    printf("  decode_ms : %" PRIu64 "\n", ms);
    printf("  decode_mbs: %.1f\n", mb_s);
    printf("  dec_ticks : %u\n", tps);
    return 0;
}
