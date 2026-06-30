/* =========================================================================
 * RC24 2-WAY DECODE — Two interleaved RC24 decode streams for ILP
 *
 * Usage:
 *   rc24_2way_decode <input.rc24.2w> <output> [no_progress]
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <emmintrin.h>

#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"
#include "rc24_codec.h"
#include "model_12.h"

#define OUT_BUF_SIZE (16 * 1024)

int main(int argc, char **argv) {
    static uint8_t out_buf[OUT_BUF_SIZE];
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input.rc24.2w> <output> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && strcmp(argv[3], "no_progress") == 0);

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "fopen input\n"); return 1; }

    uint8_t sig[4];
    if (!zpl_file_read(&fin, sig, sizeof(sig))) { fprintf(stderr, "read sig\n"); zpl_file_close(&fin); return 1; }
    if (sig[0] != 'r' || sig[1] != '4' || (sig[2] & 0x02) == 0) {
        fprintf(stderr, "bad signature / not 2-way (got %c%c flags=0x%02X)\n", sig[0], sig[1], sig[2]);
        zpl_file_close(&fin); return 1;
    }
    int is_rle = sig[2] & 0x01;
    uint8_t rle_sym = sig[3];

    uint64_t orig_len = 0;
    if (!zpl_file_read(&fin, &orig_len, sizeof(orig_len))) { fprintf(stderr, "read len\n"); zpl_file_close(&fin); return 1; }
    size_t n = (size_t)orig_len;

    if (n == 0) {
        zpl_file_close(&fin);
        zpl_file fout; zpl_file_create(&fout, argv[2]); zpl_file_close(&fout);
        return 0;
    }

    model12_t M;
    memset(&M, 0, sizeof(M));

    if (is_rle) {
        zpl_file_close(&fin);
        zpl_file fout;
        if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); return 1; }
        memset(out_buf, rle_sym, OUT_BUF_SIZE);
        size_t remaining = n;
        while (remaining > 0) {
            size_t blk = (remaining < OUT_BUF_SIZE) ? remaining : OUT_BUF_SIZE;
            if (!zpl_file_write(&fout, out_buf, blk)) { fprintf(stderr, "write\n"); zpl_file_close(&fout); return 1; }
            remaining -= blk;
        }
        zpl_file_close(&fout);
        printf("DECODE-RC24-2WAY OK (RLE, sym=0x%02X, len=%zu)\n", rle_sym, n);
        return 0;
    }

    if (!zpl_file_read(&fin, M.cums + 1, sizeof(M.cums[0]) * ALPHABET_12)) {
        fprintf(stderr, "read cum\n"); zpl_file_close(&fin); return 1;
    }

#ifndef DISABLE_LUT
    {
        int s = 0;
        unsigned v;
        for (v = 0; v <= TARGET_TOTAL_12; v++) {
            while (s < ALPHABET_12 - 1 && (int)M.cums[s + 1] <= (int)v) s++;
            M.lut[v] = (uint8_t)s;
        }
    }
#endif

    uint32_t stream0_len = 0, stream1_len = 0;
    if (!zpl_file_read(&fin, &stream0_len, sizeof(stream0_len)) ||
        !zpl_file_read(&fin, &stream1_len, sizeof(stream1_len))) {
        fprintf(stderr, "read lens\n"); zpl_file_close(&fin); return 1;
    }

    size_t s0_alloc = stream0_len + 1024;
    size_t s1_alloc = stream1_len + 1024;
    uint8_t *s0 = (uint8_t *)malloc(s0_alloc);
    uint8_t *s1 = (uint8_t *)malloc(s1_alloc);
    if (!s0 || !s1) { fprintf(stderr, "malloc\n"); zpl_file_close(&fin); return 1; }
    memset(s0 + stream0_len, 0, 1024);
    memset(s1 + stream1_len, 0, 1024);
    {
        uint8_t *p = s0; int64_t rem = stream0_len;
        while (rem > 0) {
            uint32_t chunk = (uint32_t)zpl_min(rem, 0x1000000LL);
            if (!zpl_file_read(&fin, p, chunk)) { fprintf(stderr, "read s0\n"); zpl_file_close(&fin); free(s0); free(s1); return 1; }
            p += chunk; rem -= chunk;
        }
    }
    {
        uint8_t *p = s1; int64_t rem = stream1_len;
        while (rem > 0) {
            uint32_t chunk = (uint32_t)zpl_min(rem, 0x1000000LL);
            if (!zpl_file_read(&fin, p, chunk)) { fprintf(stderr, "read s1\n"); zpl_file_close(&fin); free(s0); free(s1); return 1; }
            p += chunk; rem -= chunk;
        }
    }
    zpl_file_close(&fin);

    rc_div24_lut_init();

    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0_ticks = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 rdtsc_freq = (zpl_rdtsc() - d0_ticks) * 10;
    printf("CPU freq = %1.1f Mhz\n", rdtsc_freq / 1000000.0f);

    rc24_dec_t d0, d1;
    rc24_dec_init(&d0, s0);
    rc24_dec_init(&d1, s1);

    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); free(s0); free(s1); return 1; }

    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i != n) {
        uint32_t block_size = (uint32_t)zpl_min(n - i, OUT_BUF_SIZE);
        block_size &= ~1u;
        zpl_u64 t0 = zpl_rdtsc();
        for (uint32_t k = 0; k < block_size; k += 2) {
            _mm_prefetch((const char *)(d0.in + d0.pos + 64), _MM_HINT_T0);
            _mm_prefetch((const char *)(d1.in + d1.pos + 64), _MM_HINT_T0);
            uint32_t cum0 = rc24_dec_get_cum(&d0);
            uint32_t cum1 = rc24_dec_get_cum(&d1);
            uint16_t cf0, f0, cf1, f1;
            uint8_t sym0 = model12_find(&M, (uint16_t)cum0, &cf0, &f0);
            uint8_t sym1 = model12_find(&M, (uint16_t)cum1, &cf1, &f1);
            rc24_dec_step(&d0, cf0, f0);
            rc24_dec_step(&d1, cf1, f1);
            out_buf[k]     = sym0;
            out_buf[k + 1] = sym1;
        }
        total_ticks += zpl_rdtsc() - t0;
        if (!zpl_file_write(&fout, out_buf, block_size)) { fprintf(stderr, "write\n"); zpl_file_close(&fout); free(s0); free(s1); return 1; }
        i += block_size;
        if (!no_progress) {
            printf("\r                      \r%zu / %zu  (%3.1f%%)", i, n, 100.0 * (double)i / (double)n);
            fflush(stdout);
        }
    }
    if (!no_progress) printf("\n");
    zpl_file_close(&fout);
    free(s0); free(s1);

    uint64_t dec_ms = (rdtsc_freq > 0) ? (total_ticks * 1000 / rdtsc_freq) : 0;
    double mb_s = (dec_ms > 0) ? (double)n / (dec_ms * 1048576.0 / 1000.0) : 0.0;
    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;

    printf("DECODE-RC24-2WAY OK\n");
    printf("  decode_ms : %" PRIu64 "\n", dec_ms);
    printf("  decode_mbs: %.1f\n", mb_s);
    printf("  dec_ticks : %u\n", ticks_per_symbol);
    return 0;
}
