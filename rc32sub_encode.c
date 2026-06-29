/* =========================================================================
 * RC32SUB ENCODE — Subbotin carryless range coder (byte-oriented)
 *
 * Usage:
 *   rc32sub_encode <input_file> <output_file.rc32sub> [no_progress]
 *
 * .rc32sub format:
 *   [4 bytes]  signature: 'r','3','s', flags, rle_sym
 *   [8 bytes]  uint64_t original_len (LE)
 *   --- RLE mode (is_rle=1) --- (nothing else) ---
 *   --- RC mode (is_rle=0) ---
 *   [512 bytes] cum[1..256] — uint16_t LE (12-bit model)
 *   [N bytes]   uint8_t byte stream (Subbotin carryless RC)
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"

#include "rc32sub_codec.h"
#include "model_12.h"

#define PROGRESS_BLOCK (16 * 1024)

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input_file> <output_file.rc32sub> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && strcmp(argv[3], "no_progress") == 0);

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "fopen input\n"); return 1; }
    zpl_i64 fsize = zpl_file_size(&fin);
    if (fsize < 0) { fprintf(stderr, "file_size\n"); zpl_file_close(&fin); return 1; }
    uint64_t n = (uint64_t)fsize;

    uint8_t *data = NULL;
    if (n > 0) {
        data = (uint8_t *)malloc((size_t)n);
        if (!data) { fprintf(stderr, "malloc\n"); zpl_file_close(&fin); return 1; }
        uint8_t *p = data; int64_t rem = (int64_t)n;
        while (rem > 0) {
            uint32_t chunk = (uint32_t)zpl_min(rem, 0x1000000LL);
            if (!zpl_file_read(&fin, p, chunk)) { fprintf(stderr, "read\n"); zpl_file_close(&fin); free(data); return 1; }
            p += chunk; rem -= chunk;
        }
    }
    zpl_file_close(&fin);

    uint32_t raw_freq[ALPHABET_12];
    memset(raw_freq, 0, sizeof(raw_freq));
    for (size_t i = 0; i < n; i++) raw_freq[data[i]]++;

    model12_t M;
    int is_rle = model12_build(&M, raw_freq);
    if (is_rle < 0) { fprintf(stderr, "model_build\n"); free(data); return 1; }

    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); free(data); return 1; }

    uint8_t flags = !!is_rle;
    uint8_t rle_sym = (uint8_t)M.cums[1];
    uint8_t sig[4] = { 'r', 's', flags, rle_sym };
    zpl_file_write(&fout, sig, sizeof(sig));
    zpl_file_write(&fout, &n, sizeof(n));

    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 rdtsc_freq = (zpl_rdtsc() - d0) * 10;
    printf("CPU freq = %1.1f Mhz\n", rdtsc_freq / 1000000.0f);

    if (is_rle) {
        zpl_file_close(&fout);
        printf("ENCODE-RC32SUB OK (RLE, sym=0x%02X)\n", rle_sym);
        printf("  in:  %" PRIu64 " bytes\n", n);
        printf("  out: 12 bytes\n");
        free(data);
        return 0;
    }

    zpl_file_write(&fout, M.cums + 1, sizeof(M.cums[0]) * ALPHABET_12);

    size_t buf_bytes = (size_t)(n + n / 50 + 1024);
    uint8_t *buf = (uint8_t *)malloc(buf_bytes);
    if (!buf) { fprintf(stderr, "malloc buf\n"); zpl_file_close(&fout); free(data); return 1; }

    rc32sub_enc_t rc;
    rc32sub_enc_init(&rc, buf, buf_bytes);

    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i < n) {
        size_t block = zpl_min(n - i, (size_t)PROGRESS_BLOCK);
        zpl_u64 t0 = zpl_rdtsc();
        for (size_t k = 0; k < block; k++) {
            uint16_t cum_lo, freq;
            model12_get(&M, data[i + k], &cum_lo, &freq);
            rc32sub_enc_step(&rc, cum_lo, freq);
        }
        total_ticks += zpl_rdtsc() - t0;
        i += block;
        if (!no_progress) {
            printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)",
                   (uint64_t)i, n, 100.0 * (double)i / (double)n);
            fflush(stdout);
        }
    }
    rc32sub_enc_flush(&rc);
    if (!no_progress) printf("\n");

    size_t nbytes = rc32sub_enc_size(&rc);
    if (nbytes > buf_bytes) {
        fprintf(stderr, "BUFFER OVERFLOW: %zu > %zu\n", nbytes, buf_bytes);
        free(buf); zpl_file_close(&fout); free(data); return 1;
    }

    zpl_file_write(&fout, buf, sizeof(buf[0]) * nbytes);

    zpl_i64 out_bytes = zpl_file_size(&fout);
    zpl_file_close(&fout);
    free(buf); free(data);

    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;
    uint64_t total_enc_time_ms = (rdtsc_freq > 0) ? (uint64_t)(total_ticks * 1000 / rdtsc_freq) : 0;
    double ratio = (n > 0) ? (double)out_bytes / (double)n * 100.0 : 0.0;
    double bpb   = (n > 0) ? (double)out_bytes * 8.0 / (double)n : 0.0;
    double mb_s  = (total_enc_time_ms > 0) ? (double)n / (total_enc_time_ms * 1048576.0 / 1000.0) : 0.0;

    printf("ENCODE-RC32SUB OK\n");
    printf("  engine    : Subbotin carryless RC, 32-bit state, 8-bit bytes, TOTAL=4096\n");
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
