/* =========================================================================
 * RC ENCODE v2 — inplace cache/FF, static 14-bit model
 * =========================================================================
 *
 * Ported to zpl.h (like rc_decode.c):
 *   - I/O via zpl_file (zpl_file_open / zpl_file_create / zpl_file_read /
 *     zpl_file_write / zpl_file_close / zpl_file_size)
 *   - Timing via zpl_rdtsc() with auto-calibration of the CPU frequency
 *   - Progress reported during encoding (in 16KB blocks), like the decoder
 *   - Isolated timing: only the encoding loop is counted in total_ticks;
 *     I/O (reading input, writing output) and the model (model_build) are
 *     NOT included in the measurement — this is symmetric to the decoder,
 *     which does not count loading the .rc file and flushing out_buf to disk.
 *
 * Usage:
 *   rc_encode <input_file> <output_file>
 *
 * .rc format:
 *   [4 bytes]  signature 'r','c', flags, rle_sym
 *   [8 bytes]  uint64_t original_len (LE)
 *   --- RLE mode ---
 *              (nothing else)
 *   --- RC mode ---
 *   [512 bytes] cum[1..256] — uint16_t LE (cumulative frequencies)
 *              cum[0]=0 is not stored (always a constant)
 *   [4*N bytes] uint32_t words LE — encoder stream
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"

#include "rc_codec.h"
#include "model.h"

/* Progress block size. The encoder works entirely in-memory, but progress is
   printed in 16KB blocks — symmetric to the decoder. */
#define PROGRESS_BLOCK (16 * 1024)

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input_file> <output_file> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && strcmp(argv[3], "no_progress") == 0);

    /* --- Reading the input file (outside the measurement) --- */
    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) {
        fprintf(stderr, "fopen input\n");
        return 1;
    }

    zpl_i64 fsize = zpl_file_size(&fin);
    if (fsize <= 0) {
        fprintf(stderr, "file_size failed\n");
        zpl_file_close(&fin);
        return 1;
    }
    uint64_t n = (uint64_t)fsize;

    uint8_t *data = (uint8_t *)malloc((size_t)n);
    if (!data) {
        fprintf(stderr, "malloc data\n");
        zpl_file_close(&fin);
        return 1;
    }

    /* Reading a potentially large file in chunks, like the decoder. */
    {
        uint8_t *p = data;
        int64_t remaining = (int64_t)n;
        while (remaining > 0) {
            uint32_t chunk = (uint32_t)zpl_min(remaining, 0x1000000LL);
            if (!zpl_file_read(&fin, p, chunk)) {
                fprintf(stderr, "read error\n");
                zpl_file_close(&fin);
                free(data);
                return 1;
            }
            p += chunk;
            remaining -= chunk;
        }
    }
    zpl_file_close(&fin);

    /* --- Frequency counting (outside the measurement — part of the model) --- */
    uint32_t raw_freq[ALPHABET];
    memset(raw_freq, 0, sizeof(raw_freq));
    for (size_t i = 0; i < n; i++) raw_freq[data[i]]++;

    cums_t m;
    int is_rle = model_build(m, raw_freq);
    if (is_rle < 0) {
        fprintf(stderr, "model_build failed\n");
        free(data);
        return 1;
    }

    /* --- Opening the output file and writing the header (outside the measurement) --- */
    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) {
        fprintf(stderr, "fopen output\n");
        free(data);
        return 1;
    }

    uint8_t flags = !!m[0];
    uint8_t rle_sym = (uint8_t)m[1];
    uint8_t sig[4] = { 'r', 'c', flags, rle_sym };
    if (!zpl_file_write(&fout, sig, sizeof(sig))) {
        fprintf(stderr, "write sig\n");
        zpl_file_close(&fout); free(data); return 1;
    }
    if (!zpl_file_write(&fout, &n, sizeof(n))) {
        fprintf(stderr, "write len\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- CPU frequency calibration (like in the decoder) --- */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100;
    zpl_u64 dddd = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 zpl_rdtsc_freq = (zpl_rdtsc() - dddd) * 10;
    printf("CPU freq = %1.1f Mhz\n", zpl_rdtsc_freq / 1000000.0f);

    /* --- RLE mode --- */
    if (is_rle) {
        zpl_file_close(&fout);
        printf("ENCODE v2 OK (RLE, sym=0x%02X)\n", rle_sym);
        printf("  in:  %" PRIu64 " bytes\n", n);
        printf("  out: 12 bytes\n");
        free(data);
        return 0;
    }

    /* --- RC mode: writing cumulative frequencies cum[1..256] (outside the measurement) --- */
    if (!zpl_file_write(&fout, m + 1, sizeof(m[0]) * ALPHABET)) {
        fprintf(stderr, "write cum\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Allocating the stream buffer --- */
    size_t buf_words = (size_t)(n / 4 + n / 200 + 1024);
    uint32_t *buf = (uint32_t *)malloc(buf_words * sizeof(uint32_t));
    if (!buf) {
        fprintf(stderr, "malloc buf\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Encoding (inplace — the struct writes into buf itself via out_ptr).
       Measurement: only the encoding loop. Progress is printed in 16KB blocks,
       like in the decoder. Inside a block — pure rdtsc, no I/O. --- */
    rc_enc_t rc;
    rc_enc_init(&rc, buf, buf_words);

    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i < n) {
        size_t block = zpl_min(n - i, (size_t)PROGRESS_BLOCK);
        zpl_u64 t0 = zpl_rdtsc();
        for (size_t k = 0; k < block; k++) {
            uint16_t cum_lo, freq;
            model_get(m, data[i + k], &cum_lo, &freq);
            rc_enc_step(&rc, cum_lo, freq, TARGET_TOTAL);
        }
        total_ticks += zpl_rdtsc() - t0;
        i += block;
        if (!no_progress) {
            printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)",
                   (uint64_t)i, n, 100.0 * (double)i / (double)n);
            fflush(stdout);
        }
    }
    if (!no_progress) printf("\n");
    /* flush is ~2 words; no need to measure it together with the last block,
       it is not part of the per-symbol hot loop. Done outside the measurement. */
    rc_enc_flush(&rc);
    printf("\n");

    size_t nwords = (size_t)(rc.out_ptr - buf);

    /* --- Writing the stream (outside the measurement) --- */
    if (!zpl_file_write(&fout, buf, sizeof(buf[0]) * nwords)) {
        fprintf(stderr, "write word\n");
        free(buf); zpl_file_close(&fout); free(data); return 1;
    }

    zpl_i64 out_bytes = zpl_file_size(&fout);
    zpl_file_close(&fout);
    free(buf);
    free(data);

    /* --- Report --- */
    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;
    uint64_t total_enc_time_ms = (zpl_rdtsc_freq > 0)
        ? (uint64_t)(total_ticks * 1000 / zpl_rdtsc_freq)
        : 0;

    double ratio = (n > 0) ? (double)out_bytes / (double)n * 100.0 : 0.0;
    double bpb   = (n > 0) ? (double)out_bytes * 8.0 / (double)n : 0.0;
    double mb_s  = (total_enc_time_ms > 0)
        ? (double)n / (total_enc_time_ms * 1048576.0 / 1000.0)
        : 0.0;

    printf("ENCODE-RC OK\n");
    printf("  engine    : 64-bit Schindler range coder, 14-bit model\n");
    printf("  input     : %s\n", argv[1]);
    printf("  in_size   : %" PRIu64 " bytes\n", n);
    printf("  out_size  : %" PRIi64 " bytes\n", out_bytes);
    printf("  bpb       : %.3f\n", bpb);
    printf("  ratio_pct : %.2f\n", ratio);
    printf("  encode_ms : %" PRIu64 "\n", total_enc_time_ms);
    printf("  encode_mbs: %.1f\n", mb_s);
    printf("  enc_ticks : %u\n", ticks_per_symbol);
    return 0;
}
