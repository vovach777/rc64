/* =========================================================================
 * RC32 DECODE — 32-bit in-place carry range coder (16-bit words)
 *
 * The compressed stream is loaded entirely into memory. Output goes through
 * a static 16KB buffer, flushed to disk in chunks. The timer is off during
 * the flush.
 *
 * Usage:
 *   rc_decode32 <input_file.rc32> <output_file>
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"
#include "rc_codec_32.h"
#include "model_12.h"

#define OUT_BUF_SIZE (16 * 1024)  /* 16KB output buffer */

int main(int argc, char **argv) {
    static uint8_t out_buf[OUT_BUF_SIZE];
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.rc32> <output>\n", argv[0]);
        return 1;
    }

    zpl_file fin;
    zpl_file_error err = zpl_file_open(&fin, argv[1]);
    if (err) { perror("fopen input"); return err; }

    /* Header */
    uint8_t sig[4];
    if (zpl_file_read(&fin, sig, sizeof(sig)) != 1) {
        fprintf(stderr, "read sig\n");
        zpl_file_close(&fin);
        return 1;
    }
    if (sig[0] != 'r' || sig[1] != '3') {
        fprintf(stderr, "bad signature (expected 'r3', got %c%c)\n",
                sig[0], sig[1]);
        zpl_file_close(&fin);
        return 1;
    }
    int is_rle = sig[2] & 0x01;
    uint8_t rle_sym = sig[3];

    uint64_t orig_len = 0;
    if (zpl_file_read(&fin, &orig_len, sizeof(orig_len)) != 1) {
        fprintf(stderr, "read len\n");
        zpl_file_close(&fin);
        return 1;
    }
    size_t n = (size_t)orig_len;
    if (n == 0) {
        zpl_file_close(&fin);
        zpl_file_create(&fin, argv[2]);
        zpl_file_close(&fin);
        return 0;
    }

    /* RLE mode */
    if (is_rle) {
        zpl_file_close(&fin);
        if (zpl_file_create(&fin, argv[2])) {
            perror("fopen output");
            return 1;
        }
        /* Fill the buffer with rle_sym and write in chunks */
        memset(out_buf, rle_sym, OUT_BUF_SIZE);
        size_t remaining = n;
        while (remaining > 0) {
            size_t block = (remaining < OUT_BUF_SIZE) ? remaining : OUT_BUF_SIZE;
            if (!zpl_file_write(&fin, out_buf, block)) {
                perror("fwrite error");
                zpl_file_close(&fin);
                return 1;
            }
            remaining -= block;
        }
        zpl_file_close(&fin);
        printf("DECODE32 OK (RLE Mode, sym=0x%02X, len=%zu)\n", rle_sym, n);
        return 0;
    }

    /* RC mode: reading cumulative frequencies cum[1..256] */
    model12_t M;
    memset(&M, 0, sizeof(M));
    if (zpl_file_read(&fin, M.cums + 1, sizeof(M.cums[0]) * ALPHABET_12) != 1) {
        fprintf(stderr, "read cum\n");
        zpl_file_close(&fin);
        return 1;
    }

    /* LUT is built once from the ready cums table (unless DISABLE_LUT) */
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

    zpl_i64 ac_data_len = zpl_file_size(&fin) - 524;  /* 4+8+512 */

    /* CPU calibration */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100;
    zpl_u64 dddd = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 zpl_rdtsc_freq = (zpl_rdtsc() - dddd) * 10;
    printf("CPU freq = %1.1f Mhz\n", zpl_rdtsc_freq / 1000000.0f);

    /* Reading the stream (16-bit words) */
    size_t ac_u16_len = (size_t)(ac_data_len / sizeof(uint16_t)) + 4;
    uint16_t *words = (uint16_t *)malloc(ac_u16_len * sizeof(uint16_t));
    if (!words) {
        fprintf(stderr, "malloc words\n");
        zpl_file_close(&fin);
        return 1;
    }

    uint8_t *u8_p = (uint8_t *)words;
    int64_t u8_len = ac_data_len;
    while (u8_len > 0) {
        uint32_t chunk = (uint32_t)zpl_min(u8_len, 0x1000000LL);
        if (zpl_file_read(&fin, u8_p, chunk) != 1) {
            fprintf(stderr, "read error\n");
            zpl_file_close(&fin);
            free(words);
            return 1;
        }
        u8_p  += chunk;
        u8_len -= chunk;
    }
    zpl_file_close(&fin);

    if (zpl_file_create(&fin, argv[2])) {
        perror("fopen output");
        free(words);
        return 1;
    }

    /* Decoding */
    size_t words_total = (size_t)(ac_data_len / sizeof(uint16_t));
    rc32_dec_t rd;
    rc32_dec_init(&rd, words, words_total);

    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i != n) {
        uint32_t block_size = (uint32_t)zpl_min(n - i, OUT_BUF_SIZE);
        zpl_u64 t0 = zpl_rdtsc();
        for (uint32_t k = 0; k < block_size; k++) {
            uint16_t cum = (uint16_t)rc32_dec_get_cum(&rd);
            uint16_t cum_lo, freq;
            uint8_t sym = model12_find(&M, cum, &cum_lo, &freq);
            rc32_dec_step(&rd, cum_lo, freq);
            out_buf[k] = sym;
        }
        total_ticks += zpl_rdtsc() - t0;
        if (!zpl_file_write(&fin, out_buf, block_size)) {
            perror("fwrite error");
            zpl_file_close(&fin);
            free(words);
            return 1;
        }
        i += block_size;
        printf("\r                      \r%zu / %zu  (%3.1f%%)",
               i, n, 100.0 * (double)i / (double)n);
        fflush(stdout);
    }
    printf("\n");

    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;
    uint64_t total_dec_time_ms = (zpl_rdtsc_freq > 0)
        ? (uint64_t)(total_ticks * 1000 / zpl_rdtsc_freq)
        : 0;
    double mb_s = (total_dec_time_ms > 0)
        ? (double)n / (total_dec_time_ms * 1048576.0 / 1000.0)
        : 0.0;

    zpl_file_close(&fin);
    free(words);

    printf("DECODE32 OK\n");
    printf("  engine:   32-bit in-place carry RC, 16-bit words, TOTAL_BITS=12\n");
    printf("  decode:   %" PRIu64 " ms  (%.1f MB/s)\n", total_dec_time_ms, mb_s);
    printf("          : %u ticks per symbol\n", ticks_per_symbol);
    printf("  memory:   %.2f MB\n",
           (float)ac_u16_len / (1024.0f * 1024.0f / 2));
    return 0;
}
