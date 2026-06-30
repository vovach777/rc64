/* =========================================================================
 * RC24 2-WAY ENCODE — Two interleaved RC24 streams
 *
 * Usage:
 *   rc24_2way_encode <input_file> <output_file.rc24.2w> [no_progress]
 *
 * .rc24.2w format:
 *   [4 bytes]   signature: 'r','4', flags, rle_sym
 *               flags bit 1: 2-way interleave
 *   [8 bytes]   uint64_t original_len (LE)
 *   [512 bytes] cum[1..256] — uint16_t LE (12-bit model)
 *   [4 bytes]   uint32_t stream0_len (LE)
 *   [4 bytes]   uint32_t stream1_len (LE)
 *   [stream0_len bytes] stream 0 (even-position symbols)
 *   [stream1_len bytes] stream 1 (odd-position symbols)
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

#define PROGRESS_BLOCK (16 * 1024)

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input_file> <output_file.rc24.2w> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && strcmp(argv[3], "no_progress") == 0);

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "fopen input\n"); return 1; }
    zpl_i64 fsize = zpl_file_size(&fin);
    if (fsize < 0) { fprintf(stderr, "file_size\n"); zpl_file_close(&fin); return 1; }
    uint64_t n64 = (uint64_t)fsize;
    size_t n = (size_t)n64;

    uint8_t *in = (uint8_t *)malloc(n + 1);
    if (!in) { fprintf(stderr, "malloc\n"); zpl_file_close(&fin); return 1; }
    {
        uint8_t *p = in; int64_t rem = (int64_t)n;
        while (rem > 0) {
            uint32_t chunk = (uint32_t)zpl_min(rem, 0x1000000LL);
            if (!zpl_file_read(&fin, p, chunk)) { fprintf(stderr, "read\n"); zpl_file_close(&fin); free(in); return 1; }
            p += chunk; rem -= chunk;
        }
    }
    in[n] = 0;  /* padding for the last odd symbol */
    zpl_file_close(&fin);

    uint32_t raw_freq[ALPHABET_12];
    memset(raw_freq, 0, sizeof(raw_freq));
    for (size_t i = 0; i < n; i++) raw_freq[in[i]]++;

    model12_t M;
    int is_rle = model12_build(&M, raw_freq);
    if (is_rle < 0) { fprintf(stderr, "model_build\n"); free(in); return 1; }

    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); free(in); return 1; }

    uint8_t flags = (uint8_t)((!!is_rle) | 0x02);
    uint8_t rle_sym = (uint8_t)M.cums[1];
    uint8_t sig[4] = { 'r', '4', (uint8_t)flags, rle_sym };
    zpl_file_write(&fout, sig, sizeof(sig));
    zpl_file_write(&fout, &n64, sizeof(n64));

    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 rdtsc_freq = (zpl_rdtsc() - d0) * 10;
    printf("CPU freq = %1.1f Mhz\n", rdtsc_freq / 1000000.0f);

    if (is_rle) {
        zpl_file_close(&fout);
        printf("ENCODE-RC24-2WAY OK (RLE, sym=0x%02X)\n", rle_sym);
        printf("  in:  %zu bytes\n", n);
        printf("  out: 12 bytes\n");
        free(in);
        return 0;
    }

    zpl_file_write(&fout, M.cums + 1, sizeof(M.cums[0]) * ALPHABET_12);

    size_t cap0 = n / 2 + 1024;
    size_t cap1 = n / 2 + 1024;
    uint8_t *s0 = (uint8_t *)malloc(cap0);
    uint8_t *s1 = (uint8_t *)malloc(cap1);
    if (!s0 || !s1) { fprintf(stderr, "malloc\n"); zpl_file_close(&fout); free(in); return 1; }

    rc24_enc_t e0, e1;
    rc24_enc_init(&e0, s0, cap0);
    rc24_enc_init(&e1, s1, cap1);

    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i < n) {
        size_t block = zpl_min(n - i, (size_t)PROGRESS_BLOCK);
        block &= ~1u;  /* keep even */
        zpl_u64 t0 = zpl_rdtsc();
        for (size_t k = 0; k < block; k += 2) {
            uint8_t sym0 = in[i + k];
            uint8_t sym1 = in[i + k + 1];
            uint16_t c0 = M.cums[sym0], f0 = (uint16_t)(M.cums[sym0 + 1] - M.cums[sym0]);
            uint16_t c1 = M.cums[sym1], f1 = (uint16_t)(M.cums[sym1 + 1] - M.cums[sym1]);
            rc24_enc_step(&e0, c0, f0);
            rc24_enc_step(&e1, c1, f1);
        }
        total_ticks += zpl_rdtsc() - t0;
        i += block;
        if (!no_progress) {
            printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)",
                   (uint64_t)i, (uint64_t)n, 100.0 * (double)i / (double)n);
            fflush(stdout);
        }
    }
    rc24_enc_flush(&e0);
    rc24_enc_flush(&e1);
    if (!no_progress) printf("\n");

    uint32_t stream0_len = (uint32_t)rc24_enc_size(&e0);
    uint32_t stream1_len = (uint32_t)rc24_enc_size(&e1);

    zpl_file_write(&fout, &stream0_len, sizeof(stream0_len));
    zpl_file_write(&fout, &stream1_len, sizeof(stream1_len));
    zpl_file_write(&fout, s0, stream0_len);
    zpl_file_write(&fout, s1, stream1_len);

    zpl_i64 out_bytes = zpl_file_size(&fout);
    zpl_file_close(&fout);
    free(in); free(s0); free(s1);

    uint64_t enc_ms = (rdtsc_freq > 0) ? (total_ticks * 1000 / rdtsc_freq) : 0;
    double mb_s = (enc_ms > 0) ? (double)n / (enc_ms * 1048576.0 / 1000.0) : 0.0;
    double bpb = (double)(out_bytes * 8) / (double)n;
    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;

    printf("ENCODE-RC24-2WAY OK\n");
    printf("  input     : %s\n", argv[1]);
    printf("  in_size   : %zu bytes\n", n);
    printf("  out_size  : %" PRIi64 " bytes\n", out_bytes);
    printf("  bpb       : %.3f\n", bpb);
    printf("  ratio_pct : %.2f\n", 100.0 * (double)out_bytes / (double)n);
    printf("  encode_ms : %" PRIu64 "\n", enc_ms);
    printf("  encode_mbs: %.1f\n", mb_s);
    printf("  enc_ticks : %u\n", ticks_per_symbol);
    return 0;
}
