/* =========================================================================
 * FSE ENCODE — order-0 tANS, block scheme (mirrors rans_encode.cpp)
 * =========================================================================
 *
 * Same architecture as the rANS engine: the input is split into fixed 256 KB
 * blocks, each an independent FSE stream; ONE global order-0 model is built
 * over the whole file and written once in the header (model construction is
 * outside the timed region, just like model_build/rfreq in rans_encode). Each
 * block is encoded with FSE_compress_usingCTable, timed per-block with
 * zpl_rdtsc(). tableLog forced to 14 to match the rc64 14-bit model.
 *
 * Usage:
 *   fse_encode <input_file> <output_file.fse>
 *
 * .fse format:
 *   [4]   signature: 'f','s', flags, rle_sym     (flags bit 0: is_rle)
 *   [8]   uint64_t original_len (LE)
 *   --- RLE mode (is_rle=1) ---
 *         (nothing else — rle_sym is in the signature)
 *   --- FSE mode (is_rle=0) ---
 *   [4]   uint32_t tableLog (LE)
 *   [4]   uint32_t maxSymbolValue (LE)
 *   [512] short normalizedCounter[256] (LE)
 *   then consecutive blocks (fixed INPUT size BLOCK_BYTES = 256 KB):
 *     [4]      uint32_t compressed_size (LE)
 *     [cs]     FSE bitstream of the block
 * ========================================================================= */

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <inttypes.h>

#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"

#define FSE_STATIC_LINKING_ONLY
#include "fse.h"

#define BLOCK_BYTES   (256 * 1024)
#define REQ_TABLELOG  14u

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input_file> <output_file.fse> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && strcmp(argv[3], "no_progress") == 0);

    /* --- read input (outside timed region) --- */
    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "fopen input\n"); return 1; }
    zpl_i64 fsize = zpl_file_size(&fin);
    if (fsize <= 0) { fprintf(stderr, "file_size failed\n"); zpl_file_close(&fin); return 1; }
    uint64_t n = (uint64_t)fsize;
    uint8_t *data = (uint8_t *)malloc((size_t)n);
    if (!data) { fprintf(stderr, "malloc data\n"); zpl_file_close(&fin); return 1; }
    {
        uint8_t *p = data; int64_t remaining = (int64_t)n;
        while (remaining > 0) {
            uint32_t chunk = (uint32_t)zpl_min(remaining, 0x1000000LL);
            if (!zpl_file_read(&fin, p, chunk)) {
                fprintf(stderr, "read error\n"); zpl_file_close(&fin); free(data); return 1;
            }
            p += chunk; remaining -= chunk;
        }
    }
    zpl_file_close(&fin);

    /* --- frequency counting --- */
    uint32_t raw_freq[256];
    memset(raw_freq, 0, sizeof(raw_freq));
    for (size_t i = 0; i < n; i++) raw_freq[data[i]]++;
    int n_active = 0, first_sym = 0;
    for (int i = 0; i < 256; i++) if (raw_freq[i]) { n_active++; if (n_active == 1) first_sym = i; }
    if (n_active == 0) { fprintf(stderr, "empty input\n"); free(data); return 1; }

    /* --- open output, write signature + length --- */
    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); free(data); return 1; }

    /* --- CPU frequency calibration --- */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 rdtsc_freq = (zpl_rdtsc() - d0) * 10;
    printf("CPU freq = %1.1f Mhz\n", rdtsc_freq / 1000000.0f);

    /* --- RLE mode --- */
    if (n_active == 1) {
        uint8_t sig[4] = { 'f', 's', 1, (uint8_t)first_sym };
        zpl_file_write(&fout, sig, sizeof(sig));
        zpl_file_write(&fout, &n, sizeof(n));
        zpl_file_close(&fout);
        printf("ENCODE-FSE OK (RLE, sym=0x%02X)\n", first_sym);
        printf("  in:  %" PRIu64 " bytes\n", n);
        printf("  out: 12 bytes\n");
        free(data);
        return 0;
    }

    /* --- global model (outside timed region) --- */
    unsigned maxsv = 255; while (maxsv > 0 && raw_freq[maxsv] == 0) maxsv--;
    short norm[256]; memset(norm, 0, sizeof(norm));
    size_t tlr = FSE_normalizeCount(norm, REQ_TABLELOG, raw_freq, (unsigned)n, maxsv);
    if (FSE_isError(tlr)) {
        fprintf(stderr, "normalizeCount: %s\n", FSE_getErrorName(tlr));
        zpl_file_close(&fout); free(data); return 1;
    }
    unsigned tl = (unsigned)tlr;
    FSE_CTable *ct = (FSE_CTable *)malloc(FSE_CTABLE_SIZE(tl, maxsv) * sizeof(FSE_CTable));
    if (FSE_isError(FSE_buildCTable(ct, norm, maxsv, tl))) {
        fprintf(stderr, "buildCTable failed\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- write header: sig, len, tableLog, maxSymbolValue, normalizedCounter --- */
    uint8_t sig[4] = { 'f', 's', 0, 0 };
    zpl_file_write(&fout, sig, sizeof(sig));
    zpl_file_write(&fout, &n, sizeof(n));
    uint32_t tl32 = tl, msv32 = maxsv;
    zpl_file_write(&fout, &tl32, sizeof(tl32));
    zpl_file_write(&fout, &msv32, sizeof(msv32));
    zpl_file_write(&fout, norm, sizeof(norm));   /* 512 bytes */

    /* --- encode: per block, zpl_rdtsc timed --- */
    uint8_t *blkdst = (uint8_t *)malloc(FSE_compressBound(BLOCK_BYTES));
    zpl_u64 total_ticks = 0;
    size_t blocks = 0;
    uint64_t off = 0;
    while (off < n) {
        size_t bs = zpl_min((size_t)BLOCK_BYTES, (size_t)(n - off));
        zpl_u64 t0 = zpl_rdtsc();
        size_t cs = FSE_compress_usingCTable(blkdst, FSE_compressBound(bs), data + off, bs, ct);
        total_ticks += zpl_rdtsc() - t0;
        if (FSE_isError(cs)) {
            fprintf(stderr, "compress block: %s\n", FSE_getErrorName(cs));
            zpl_file_close(&fout); free(data); free(blkdst); free(ct); return 1;
        }
        uint32_t cs32 = (uint32_t)cs;
        zpl_file_write(&fout, &cs32, sizeof(cs32));
        zpl_file_write(&fout, blkdst, cs);
        off += bs; blocks++;

        if (!no_progress) {
            printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)",
                   off, n, 100.0 * (double)off / (double)n);
            fflush(stdout);
        }
    }
    if (!no_progress) printf("\n");

    zpl_i64 out_bytes = zpl_file_size(&fout);
    zpl_file_close(&fout);
    free(data); free(blkdst); free(ct);

    /* --- report --- */
    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;
    uint64_t total_enc_time_ms = (rdtsc_freq > 0) ? (uint64_t)(total_ticks * 1000 / rdtsc_freq) : 0;
    double ratio = (n > 0) ? (double)out_bytes / (double)n * 100.0 : 0.0;
    double bpb   = (n > 0) ? (double)out_bytes * 8.0 / (double)n : 0.0;
    double mb_s  = (total_enc_time_ms > 0) ? (double)n / (total_enc_time_ms * 1048576.0 / 1000.0) : 0.0;

    printf("ENCODE-FSE OK\n");
    printf("  engine    : FSE tANS, block=%dKB, tableLog=%u\n", BLOCK_BYTES / 1024, tl);
    printf("  input     : %s\n", argv[1]);
    printf("  in_size   : %" PRIu64 " bytes\n", n);
    printf("  out_size  : %" PRIi64 " bytes\n", out_bytes);
    printf("  blocks    : %zu  (block_bytes=%d)\n", blocks, BLOCK_BYTES);
    printf("  bpb       : %.3f\n", bpb);
    printf("  ratio_pct : %.2f\n", ratio);
    printf("  encode_ms : %" PRIu64 "\n", total_enc_time_ms);
    printf("  encode_mbs: %.1f\n", mb_s);
    printf("  enc_ticks : %u\n", ticks_per_symbol);
    return 0;
}
