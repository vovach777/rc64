/* =========================================================================
 * HUF (Huff0) DECODE — order-0 Huffman, per-block, STREAMING
 * =========================================================================
 *
 * Reads the .huf stream from huf_encode block by block — the whole compressed
 * stream is NEVER loaded into memory. For each block it reads the 4-byte
 * cs/marker and the cs bytes (file I/O, outside the timed region), then
 * decompresses with the HIGH-LEVEL HUF_decompress() (timed). Raw blocks
 * (cs == 0xFFFFFFFF) are read straight into the output buffer. No RLE path:
 * a single-symbol block was compressed by HUF to ~1 byte and decompresses
 * normally. Per-block zpl_rdtsc() timing, single-threaded.
 *
 * Usage:
 *   huf_decode <input_file.huf> <output_file>
 * ========================================================================= */

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <inttypes.h>

#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"

#define HUF_STATIC_LINKING_ONLY
#include "huf.h"

#define HUF_BLOCK_BYTES  (128 * 1024)
#define RAW_MARKER       0xFFFFFFFFu

static int read_exact(zpl_file *fin, uint8_t *buf, size_t want) {
    size_t got = 0;
    while (got < want) {
        uint32_t chunk = (uint32_t)zpl_min((size_t)(want - got), (size_t)0x1000000);
        if (!zpl_file_read(fin, buf + got, chunk)) return 1;
        got += chunk;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input.huf> <output> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && strcmp(argv[3], "no_progress") == 0);

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "fopen input\n"); return 1; }

    /* --- header --- */
    uint8_t sig[4];
    if (read_exact(&fin, sig, sizeof(sig))) { fprintf(stderr, "read sig\n"); zpl_file_close(&fin); return 1; }
    if (sig[0] != 'h' || sig[1] != 'u') {
        fprintf(stderr, "bad signature (expected 'hu', got %c%c)\n", sig[0], sig[1]);
        zpl_file_close(&fin); return 1;
    }
    uint64_t n = 0;
    if (read_exact(&fin, (uint8_t *)&n, sizeof(n))) { fprintf(stderr, "read len\n"); zpl_file_close(&fin); return 1; }
    size_t sz = (size_t)n;

    if (sz == 0) {
        zpl_file_close(&fin);
        zpl_file fout; zpl_file_create(&fout, argv[2]); zpl_file_close(&fout);
        return 0;
    }

    /* --- CPU frequency calibration --- */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 rdtsc_freq = (zpl_rdtsc() - d0) * 10;
    printf("CPU freq = %1.1f Mhz\n", rdtsc_freq / 1000000.0f);

    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); zpl_file_close(&fin); return 1; }

    /* --- decode: per block, read cs+stream (I/O), HUF_decompress (timed) --- */
    uint8_t *outblk  = (uint8_t *)malloc(HUF_BLOCK_BYTES);
    uint8_t *compbuf = (uint8_t *)malloc(HUF_compressBound(HUF_BLOCK_BYTES));
    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i < sz) {
        size_t bs = zpl_min(sz - i, (size_t)HUF_BLOCK_BYTES);
        uint32_t cs;
        if (read_exact(&fin, (uint8_t *)&cs, sizeof(cs))) {        /* I/O */
            fprintf(stderr, "read cs\n"); zpl_file_close(&fin); zpl_file_close(&fout); return 1;
        }
        zpl_u64 t0 = zpl_rdtsc();
        if (cs == RAW_MARKER) {
            if (read_exact(&fin, outblk, bs)) {                    /* raw block, I/O */
                total_ticks += zpl_rdtsc() - t0;
                fprintf(stderr, "read raw\n"); zpl_file_close(&fin); zpl_file_close(&fout); return 1;
            }
        } else {
            if (read_exact(&fin, compbuf, cs)) {                    /* bitstream, I/O */
                total_ticks += zpl_rdtsc() - t0;
                fprintf(stderr, "read stream\n"); zpl_file_close(&fin); zpl_file_close(&fout); return 1;
            }
            size_t ds = HUF_decompress(outblk, bs, compbuf, cs);    /* timed core */
            if (HUF_isError(ds)) {
                total_ticks += zpl_rdtsc() - t0;
                fprintf(stderr, "HUF_decompress: %s\n", HUF_getErrorName(ds));
                zpl_file_close(&fin); zpl_file_close(&fout); return 1;
            }
        }
        total_ticks += zpl_rdtsc() - t0;
        if (!zpl_file_write(&fout, outblk, bs)) {                   /* I/O */
            fprintf(stderr, "write\n"); zpl_file_close(&fin); zpl_file_close(&fout); return 1;
        }
        i += bs;

        if (!no_progress) {
            printf("\r                      \r%zu / %zu  (%3.1f%%)", i, sz, 100.0 * (double)i / (double)sz);
            fflush(stdout);
        }
    }
    if (!no_progress) printf("\n");
    zpl_file_close(&fin);
    zpl_file_close(&fout);
    free(outblk); free(compbuf);

    /* --- report --- */
    uint32_t ticks_per_symbol = (sz > 0) ? (uint32_t)(total_ticks / sz) : 0;
    uint64_t total_dec_time_ms = (rdtsc_freq > 0) ? (uint64_t)(total_ticks * 1000 / rdtsc_freq) : 0;
    double mb_s = (total_dec_time_ms > 0) ? (double)sz / (total_dec_time_ms * 1048576.0 / 1000.0) : 0.0;

    printf("DECODE-HUF OK\n");
    printf("  engine    : HUF (Huff0), block=%dKB, streaming\n", HUF_BLOCK_BYTES / 1024);
    printf("  decode_ms : %" PRIu64 "\n", total_dec_time_ms);
    printf("  decode_mbs: %.1f\n", mb_s);
    printf("  dec_ticks : %u\n", ticks_per_symbol);
    return 0;
}