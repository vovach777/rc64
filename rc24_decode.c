/* =========================================================================
 * RC24 DECODE — Subbotin carryless range coder (byte-oriented)
 *
 * Usage:
 *   rc24_decode <input_file.rc24> <output_file> [no_progress]
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

#define OUT_BUF_SIZE (16 * 1024)

int main(int argc, char **argv) {
    static uint8_t out_buf[OUT_BUF_SIZE];
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input.rc24> <output> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && strcmp(argv[3], "no_progress") == 0);

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "fopen input\n"); return 1; }

    uint8_t sig[4];
    if (!zpl_file_read(&fin, sig, sizeof(sig))) { fprintf(stderr, "read sig\n"); zpl_file_close(&fin); return 1; }
    if (sig[0] != 'r' || sig[1] != '4') {
        fprintf(stderr, "bad signature (expected 'r4', got %c%c)\n", sig[0], sig[1]);
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
        printf("DECODE-RC24 OK (RLE, sym=0x%02X, len=%zu)\n", rle_sym, n);
        return 0;
    }

    model12_t M;
    memset(&M, 0, sizeof(M));
    if (!zpl_file_read(&fin, M.cums + 1, sizeof(M.cums[0]) * ALPHABET_12)) { fprintf(stderr, "read cum\n"); zpl_file_close(&fin); return 1; }

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

    rc_div24_lut_init();

    zpl_i64 ac_data_len = zpl_file_size(&fin) - 524;

    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 rdtsc_freq = (zpl_rdtsc() - d0) * 10;
    printf("CPU freq = %1.1f Mhz\n", rdtsc_freq / 1000000.0f);

    /* load stream into memory */
    size_t stream_len = (size_t)ac_data_len + 1024;
    uint8_t *stream = (uint8_t *)malloc(stream_len);
    if (!stream) { fprintf(stderr, "malloc\n"); zpl_file_close(&fin); return 1; }
    memset(stream + ac_data_len, 0, 1024);  /* padding for safe reads past end */
    {
        uint8_t *p = stream; int64_t rem = ac_data_len;
        while (rem > 0) {
            uint32_t chunk = (uint32_t)zpl_min(rem, 0x1000000LL);
            if (!zpl_file_read(&fin, p, chunk)) { fprintf(stderr, "read stream\n"); zpl_file_close(&fin); free(stream); return 1; }
            p += chunk; rem -= chunk;
        }
    }
    zpl_file_close(&fin);

    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); free(stream); return 1; }

    rc24_dec_t rd;
    rc24_dec_init(&rd, stream);

    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i != n) {
        uint32_t block_size = (uint32_t)zpl_min(n - i, OUT_BUF_SIZE);
        zpl_u64 t0 = zpl_rdtsc();
        for (uint32_t k = 0; k < block_size; k++) {
            uint16_t cum = (uint16_t)rc24_dec_get_cum(&rd);
            if (cum >= RC24_TOTAL) cum = RC24_TOTAL - 1;
            uint16_t cum_lo, freq;
            uint8_t sym = model12_find(&M, cum, &cum_lo, &freq);
            rc24_dec_step(&rd, cum_lo, freq);
            out_buf[k] = sym;
        }
        total_ticks += zpl_rdtsc() - t0;
        if (!zpl_file_write(&fout, out_buf, block_size)) { fprintf(stderr, "write\n"); zpl_file_close(&fout); free(stream); return 1; }
        i += block_size;
        if (!no_progress) {
            printf("\r                      \r%zu / %zu  (%3.1f%%)", i, n, 100.0 * (double)i / (double)n);
            fflush(stdout);
        }
    }
    if (!no_progress) printf("\n");

    zpl_file_close(&fout);
    free(stream);

    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;
    uint64_t total_dec_time_ms = (rdtsc_freq > 0) ? (uint64_t)(total_ticks * 1000 / rdtsc_freq) : 0;
    double mb_s = (total_dec_time_ms > 0) ? (double)n / (total_dec_time_ms * 1048576.0 / 1000.0) : 0.0;

    printf("DECODE-RC24 OK\n");
    printf("  engine    : Subbotin carryless RC, 24-bit state, 8-bit bytes\n");
    printf("  decode_ms : %" PRIu64 "\n", total_dec_time_ms);
    printf("  decode_mbs: %.1f\n", mb_s);
    printf("  dec_ticks : %u\n", ticks_per_symbol);
    return 0;
}
