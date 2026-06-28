/* =========================================================================
 * FSE DECODE — order-0 tANS, block scheme (mirrors rans_decode.cpp)
 * =========================================================================
 *
 * Reads the .fse stream produced by fse_encode, rebuilds the DTable from the
 * header model (outside the timed region, like model_build_lut), then decodes
 * each 256 KB block with FSE_decompress_usingDTable, timed per-block with
 * zpl_rdtsc(). Block size is fixed (BLOCK_BYTES), so the decoder derives the
 * number of symbols per block itself; only the per-block compressed size is
 * stored in the stream.
 *
 * Usage:
 *   fse_decode <input_file.fse> <output_file>
 *
 * See fse_encode.cpp for the .fse format.
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

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input.fse> <output> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && strcmp(argv[3], "no_progress") == 0);

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "fopen input\n"); return 1; }

    /* --- header: signature + length --- */
    uint8_t sig[4];
    if (!zpl_file_read(&fin, sig, sizeof(sig))) { fprintf(stderr, "read sig\n"); zpl_file_close(&fin); return 1; }
    if (sig[0] != 'f' || sig[1] != 's') {
        fprintf(stderr, "bad signature (expected 'fs', got %c%c)\n", sig[0], sig[1]);
        zpl_file_close(&fin); return 1;
    }
    int is_rle = sig[2] & 0x01;
    uint8_t rle_sym = sig[3];

    uint64_t n = 0;
    if (!zpl_file_read(&fin, &n, sizeof(n))) { fprintf(stderr, "read len\n"); zpl_file_close(&fin); return 1; }
    size_t sz = (size_t)n;
    if (sz == 0) {
        zpl_file_close(&fin);
        zpl_file fout; zpl_file_create(&fout, argv[2]); zpl_file_close(&fout);
        return 0;
    }

    /* --- RLE mode --- */
    if (is_rle) {
        zpl_file_close(&fin);
        zpl_file fout;
        if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); return 1; }
        static uint8_t rle_buf[64 * 1024];
        memset(rle_buf, rle_sym, sizeof(rle_buf));
        size_t remaining = sz;
        while (remaining > 0) {
            size_t blk = (remaining < sizeof(rle_buf)) ? remaining : sizeof(rle_buf);
            if (!zpl_file_write(&fout, rle_buf, blk)) { fprintf(stderr, "write\n"); zpl_file_close(&fout); return 1; }
            remaining -= blk;
        }
        zpl_file_close(&fout);
        printf("DECODE-FSE OK (RLE, sym=0x%02X, len=%zu)\n", rle_sym, sz);
        return 0;
    }

    /* --- read model: tableLog, maxSymbolValue, normalizedCounter --- */
    uint32_t tl32 = 0, msv32 = 0;
    if (!zpl_file_read(&fin, &tl32, sizeof(tl32))) { fprintf(stderr, "read tl\n"); zpl_file_close(&fin); return 1; }
    if (!zpl_file_read(&fin, &msv32, sizeof(msv32))) { fprintf(stderr, "read msv\n"); zpl_file_close(&fin); return 1; }
    short norm[256]; memset(norm, 0, sizeof(norm));
    if (!zpl_file_read(&fin, norm, sizeof(norm))) { fprintf(stderr, "read norm\n"); zpl_file_close(&fin); return 1; }
    unsigned tl = tl32, maxsv = msv32;

    /* --- build DTable (outside timed region) --- */
    FSE_DTable *dt = (FSE_DTable *)malloc(FSE_DTABLE_SIZE(tl) * sizeof(FSE_DTable));
    if (FSE_isError(FSE_buildDTable(dt, norm, maxsv, tl))) {
        fprintf(stderr, "buildDTable failed\n"); zpl_file_close(&fin); return 1;
    }

    /* --- CPU frequency calibration --- */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 rdtsc_freq = (zpl_rdtsc() - d0) * 10;
    printf("CPU freq = %1.1f Mhz\n", rdtsc_freq / 1000000.0f);

    /* --- load the block stream into memory --- */
    const uint64_t HEADER = 4 + 8 + 4 + 4 + 512;   /* sig+len+tl+msv+norm */
    zpl_i64 stream_len = zpl_file_size(&fin) - (zpl_i64)HEADER;
    if (stream_len <= 0) { fprintf(stderr, "no block data\n"); zpl_file_close(&fin); return 1; }
    uint8_t *stream = (uint8_t *)malloc((size_t)stream_len + 16);
    {
        uint8_t *p = stream; int64_t remaining = stream_len;
        while (remaining > 0) {
            uint32_t chunk = (uint32_t)zpl_min(remaining, 0x1000000LL);
            if (!zpl_file_read(&fin, p, chunk)) { fprintf(stderr, "read stream\n"); zpl_file_close(&fin); return 1; }
            p += chunk; remaining -= chunk;
        }
    }
    zpl_file_close(&fin);

    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); free(stream); return 1; }

    /* --- decode: per block, zpl_rdtsc timed --- */
    uint8_t *outblk = (uint8_t *)malloc(BLOCK_BYTES);
    zpl_u64 total_ticks = 0;
    size_t i = 0;
    uint8_t *sp = stream;
    while (i < sz) {
        size_t bs = zpl_min(sz - i, (size_t)BLOCK_BYTES);
        uint32_t cs;
        memcpy(&cs, sp, 4); sp += 4;
        zpl_u64 t0 = zpl_rdtsc();
        size_t ds = FSE_decompress_usingDTable(outblk, bs, sp, cs, dt);
        total_ticks += zpl_rdtsc() - t0;
        if (FSE_isError(ds)) {
            fprintf(stderr, "decompress block: %s\n", FSE_getErrorName(ds));
            zpl_file_close(&fout); free(stream); free(outblk); return 1;
        }
        sp += cs;
        if (!zpl_file_write(&fout, outblk, bs)) { fprintf(stderr, "write\n"); zpl_file_close(&fout); return 1; }
        i += bs;

        if (!no_progress) {
            printf("\r                      \r%zu / %zu  (%3.1f%%)", i, sz, 100.0 * (double)i / (double)sz);
            fflush(stdout);
        }
    }
    if (!no_progress) printf("\n");

    zpl_file_close(&fout);
    free(stream); free(outblk); free(dt);

    /* --- report --- */
    uint32_t ticks_per_symbol = (sz > 0) ? (uint32_t)(total_ticks / sz) : 0;
    uint64_t total_dec_time_ms = (rdtsc_freq > 0) ? (uint64_t)(total_ticks * 1000 / rdtsc_freq) : 0;
    double mb_s = (total_dec_time_ms > 0) ? (double)sz / (total_dec_time_ms * 1048576.0 / 1000.0) : 0.0;

    printf("DECODE-FSE OK\n");
    printf("  engine    : FSE tANS, block=%dKB, tableLog=%u\n", BLOCK_BYTES / 1024, tl);
    printf("  decode_ms : %" PRIu64 "\n", total_dec_time_ms);
    printf("  decode_mbs: %.1f\n", mb_s);
    printf("  dec_ticks : %u\n", ticks_per_symbol);
    return 0;
}
