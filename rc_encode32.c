/* =========================================================================
 * RC32 ENCODE — 32-bit in-place carry range coder (16-bit words)
 *
 * Integration of the verified engine (see rc_codec_32.h) into the rc64 project.
 *
 * Usage:
 *   rc_encode32 <input_file> <output_file.rc32>
 *
 * .rc32 format:
 *   [4 bytes]  signature: 'r','3', flags, rle_sym
 *              flags bit 0: is_rle
 *   [8 bytes]  uint64_t original_len (LE)
 *   --- RLE mode (is_rle=1) ---
 *              (nothing else — rle_sym is in the signature)
 *   --- RC mode (is_rle=0) ---
 *   [512 bytes] cum[1..256] — uint16_t LE (cumulative frequencies, 12-bit)
 *              cum[0]=0 is not stored (constant)
 *   [2*N bytes] uint16_t words LE — encoder stream
 *
 * Header: 4 + 8 + 512 = 524 bytes (same as .rc, only signature is 'r3' not 'rc').
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

/* Block size for progress reporting. */
#define PROGRESS_BLOCK (16 * 1024)

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input_file> <output_file.rc32>\n", argv[0]);
        return 1;
    }

    /* --- Reading the input file --- */
    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) {
        fprintf(stderr, "fopen input\n");
        return 1;
    }
    zpl_i64 fsize = zpl_file_size(&fin);
    if (fsize < 0) {
        fprintf(stderr, "file_size failed\n");
        zpl_file_close(&fin);
        return 1;
    }
    uint64_t n = (uint64_t)fsize;

    uint8_t *data = NULL;
    if (n > 0) {
        data = (uint8_t *)malloc((size_t)n);
        if (!data) {
            fprintf(stderr, "malloc data\n");
            zpl_file_close(&fin);
            return 1;
        }
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

    /* --- Counting frequencies --- */
    uint32_t raw_freq[ALPHABET_12];
    memset(raw_freq, 0, sizeof(raw_freq));
    for (size_t i = 0; i < n; i++) raw_freq[data[i]]++;

    model12_t M;
    int is_rle = model12_build(&M, raw_freq);
    if (is_rle < 0) {
        fprintf(stderr, "model_build failed\n");
        free(data);
        return 1;
    }

    /* --- Opening the output file --- */
    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) {
        fprintf(stderr, "fopen output\n");
        free(data);
        return 1;
    }

    /* Signature: 'r','3' (differs from 'r','c' of the 64-bit version) */
    uint8_t flags = !!is_rle;
    uint8_t rle_sym = (uint8_t)M.cums[1];
    uint8_t sig[4] = { 'r', '3', flags, rle_sym };
    if (!zpl_file_write(&fout, sig, sizeof(sig))) {
        fprintf(stderr, "write sig\n");
        zpl_file_close(&fout); free(data); return 1;
    }
    if (!zpl_file_write(&fout, &n, sizeof(n))) {
        fprintf(stderr, "write len\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- CPU frequency calibration --- */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100;
    zpl_u64 dddd = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 zpl_rdtsc_freq = (zpl_rdtsc() - dddd) * 10;
    printf("CPU freq = %1.1f Mhz\n", zpl_rdtsc_freq / 1000000.0f);

    /* --- RLE mode --- */
    if (is_rle) {
        zpl_file_close(&fout);
        printf("ENCODE32 OK (RLE, sym=0x%02X)\n", rle_sym);
        printf("  in:  %" PRIu64 " bytes\n", n);
        printf("  out: 12 bytes\n");
        free(data);
        return 0;
    }

    /* --- RC mode: writing cum[1..256] --- */
    if (!zpl_file_write(&fout, M.cums + 1, sizeof(M.cums[0]) * ALPHABET_12)) {
        fprintf(stderr, "write cum\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Allocating the stream buffer (16-bit words) ---
       Upper bound: 1 word (16 bits) per 1 input byte (worst case
       for very skewed distributions) + headroom for flush. */
    size_t buf_words = (size_t)(n + n / 50 + 1024);
    uint16_t *buf = (uint16_t *)malloc(buf_words * sizeof(uint16_t));
    if (!buf) {
        fprintf(stderr, "malloc buf\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Encoding --- */
    rc32_enc_t rc;
    rc32_enc_init(&rc, buf, buf_words);

    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i < n) {
        size_t block = zpl_min(n - i, (size_t)PROGRESS_BLOCK);
        zpl_u64 t0 = zpl_rdtsc();
        for (size_t k = 0; k < block; k++) {
            uint16_t cum_lo, freq;
            model12_get(&M, data[i + k], &cum_lo, &freq);
            rc32_enc_step(&rc, cum_lo, freq);
        }
        total_ticks += zpl_rdtsc() - t0;
        i += block;
        printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)",
               (uint64_t)i, n, 100.0 * (double)i / (double)n);
        fflush(stdout);
    }
    rc32_enc_flush(&rc);
    printf("\n");

    size_t nwords = rc32_enc_size(&rc);

    if (nwords > buf_words) {
        fprintf(stderr, "BUFFER OVERFLOW: wrote %zu words, cap=%zu\n",
                nwords, buf_words);
        free(buf); zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Writing the stream --- */
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

    printf("ENCODE32 OK\n");
    printf("  engine:   32-bit in-place carry RC, 16-bit words, TOTAL_BITS=12\n");
    printf("  input:    %s\n", argv[1]);
    printf("  in_size:  %" PRIu64 " bytes\n", n);
    printf("  out_size: %" PRIi64 " bytes\n", out_bytes);
    printf("  ratio:    %.2f%%  (%.3f bits/byte)\n", ratio, bpb);
    printf("  encode:   %" PRIu64 " ms  (%.1f MB/s)\n", total_enc_time_ms, mb_s);
    printf("          : %u ticks per symbol\n", ticks_per_symbol);
    return 0;
}
