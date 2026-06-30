/* =========================================================================
 * RC24 2-WAY ENCODE — Two interleaved RC24 streams in one shared buffer
 *
 * Usage:
 *   rc24_2way_encode <input_file> <output_file.rc24s2w> [no_progress]
 *
 * .rc24s2w format (same shared-buffer layout as RC24S, N=2):
 *   [4 bytes]   signature: 'r','4', flags, rle_sym
 *               flags bit 1: 2-way interleave
 *   [8 bytes]   uint64_t original_len (LE)
 *   [512 bytes] cum[1..256] — uint16_t LE (12-bit model)
 *   [4 bytes]   uint32_t max_stream_len LE
 *   [max_stream_len * 2 bytes] shared interleaved buffer
 *
 * Byte k of stream i lives at shared[k * 2 + i].  Even input positions go to
 * stream 0, odd positions to stream 1.  No merge, no swizzle.
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
#define PROGRESS_BLOCK (16 * 1024)

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input> <output.rc24s2w> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && !strcmp(argv[3], "no_progress"));

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "open\n"); return 1; }
    zpl_i64 fs = zpl_file_size(&fin);
    if (fs < 0) { fprintf(stderr, "size\n"); zpl_file_close(&fin); return 1; }
    uint64_t n = (uint64_t)fs;

    uint8_t *data = malloc((size_t)n + 1);
    if (!data) { fprintf(stderr, "malloc\n"); zpl_file_close(&fin); return 1; }
    {
        uint8_t *p = data; int64_t r = (int64_t)n;
        while (r > 0) {
            uint32_t c = (uint32_t)zpl_min(r, 0x1000000LL);
            if (!zpl_file_read(&fin, p, c)) { fprintf(stderr, "read\n"); zpl_file_close(&fin); free(data); return 1; }
            p += c; r -= c;
        }
    }
    data[n] = 0;  /* padding for last odd symbol */
    zpl_file_close(&fin);

    uint32_t raw[256] = {0};
    for (size_t i = 0; i < n; i++) raw[data[i]]++;
    model12_t M;
    int is_rle = model12_build(&M, raw);
    if (is_rle < 0) { fprintf(stderr, "model\n"); free(data); return 1; }

    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "open out\n"); free(data); return 1; }
    uint8_t flags = (uint8_t)((!!is_rle) | 0x02);
    uint8_t rle_sym = (uint8_t)M.cums[1];
    uint8_t sig[4] = { 'r', '4', flags, rle_sym };
    zpl_file_write(&fout, sig, 4);
    zpl_file_write(&fout, &n, 8);

    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 freq = (zpl_rdtsc() - d0) * 10;
    printf("CPU freq = %1.1f Mhz\n", freq / 1000000.0f);

    if (is_rle) {
        zpl_file_close(&fout);
        printf("ENCODE-RC24S2W OK (RLE)\n  in: %" PRIu64 "\n  out: 12\n", n);
        free(data); return 0;
    }

    zpl_file_write(&fout, M.cums + 1, sizeof(M.cums[0]) * ALPHABET_12);

    uint16_t cum_tbl[256], freq_tbl[256];
    for (int sym = 0; sym < 256; sym++) {
        cum_tbl[sym]  = M.cums[sym];
        freq_tbl[sym] = (uint16_t)(M.cums[sym + 1] - M.cums[sym]);
    }

    size_t cap = n / N + n / 50 + 4096;
    uint8_t *shared = calloc(cap * N + 16, 1);
    if (!shared) { fprintf(stderr, "malloc\n"); free(data); return 1; }
    size_t spos[N];
    uint32_t elow[N], erange[N];
    for (int s = 0; s < N; s++) { spos[s] = 0; elow[s] = 0; erange[s] = RC24_MASK; }

    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i < n) {
        size_t blk = (size_t)zpl_min((zpl_i64)(n - i), (zpl_i64)PROGRESS_BLOCK);
        blk &= ~1u;
        zpl_u64 t0 = zpl_rdtsc();
        for (size_t k = 0; k < blk; k += 2) {
            uint8_t sym0 = data[i + k];
            uint8_t sym1 = data[i + k + 1];
            uint16_t c0 = cum_tbl[sym0], f0 = freq_tbl[sym0];
            uint16_t c1 = cum_tbl[sym1], f1 = freq_tbl[sym1];

            elow[0] += c0 * (erange[0] /= RC24_TOTAL);
            erange[0] *= f0;
            while (1) {
                uint32_t diff = elow[0] ^ (elow[0] + erange[0]);
                if (diff < RC24_TOP) {
                } else if (erange[0] < RC24_BOT) {
                    erange[0] = -elow[0] & (RC24_BOT - 1);
                } else {
                    break;
                }
                shared[spos[0] * N + 0] = (uint8_t)(elow[0] >> 16);
                spos[0]++;
                erange[0] <<= 8;
                elow[0] = (elow[0] << 8) & RC24_MASK;
            }

            elow[1] += c1 * (erange[1] /= RC24_TOTAL);
            erange[1] *= f1;
            while (1) {
                uint32_t diff = elow[1] ^ (elow[1] + erange[1]);
                if (diff < RC24_TOP) {
                } else if (erange[1] < RC24_BOT) {
                    erange[1] = -elow[1] & (RC24_BOT - 1);
                } else {
                    break;
                }
                shared[spos[1] * N + 1] = (uint8_t)(elow[1] >> 16);
                spos[1]++;
                erange[1] <<= 8;
                elow[1] = (elow[1] << 8) & RC24_MASK;
            }
        }
        total_ticks += zpl_rdtsc() - t0;
        i += blk;
        if (!no_progress) {
            printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)",
                   (uint64_t)i, n, 100.0 * (double)i / (double)n);
            fflush(stdout);
        }
    }
    if (!no_progress) printf("\n");

    for (int s = 0; s < N; s++) {
        for (int j = 0; j < 3; j++) {
            shared[spos[s] * N + s] = (uint8_t)(elow[s] >> 16);
            spos[s]++; elow[s] <<= 8;
        }
    }

    size_t max_len = 0;
    for (int s = 0; s < N; s++) if (spos[s] > max_len) max_len = spos[s];

    uint32_t max_len_u32 = (uint32_t)max_len;
    zpl_file_write(&fout, &max_len_u32, 4);
    zpl_file_write(&fout, shared, max_len * N);

    zpl_i64 out_bytes = zpl_file_size(&fout);
    zpl_file_close(&fout);
    free(shared); free(data);

    uint32_t tps = n ? (uint32_t)(total_ticks / n) : 0;
    uint64_t ms = freq ? (uint64_t)(total_ticks * 1000 / freq) : 0;
    double mb_s = ms ? (double)n / (ms * 1048576.0 / 1000.0) : 0.0;

    printf("ENCODE-RC24S2W OK\n");
    printf("  input     : %s\n", argv[1]);
    printf("  in_size   : %" PRIu64 " bytes\n", n);
    printf("  out_size  : %" PRIi64 " bytes\n", out_bytes);
    printf("  bpb       : %.3f\n", (double)(out_bytes * 8) / (double)n);
    printf("  ratio_pct : %.2f\n", 100.0 * (double)out_bytes / (double)n);
    printf("  encode_ms : %" PRIu64 "\n", ms);
    printf("  encode_mbs: %.1f\n", mb_s);
    printf("  enc_ticks : %u\n", tps);
    return 0;
}
