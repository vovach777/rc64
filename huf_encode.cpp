/* =========================================================================
 * HUF (Huff0) ENCODE — order-0 Huffman, per-block, STREAMING
 * =========================================================================
 *
 * Uses the HIGH-LEVEL HUF_compress() API per block (HUF_BLOCKSIZE_MAX = 128 KB
 * is the largest block HUF accepts). The input is read 128 KB at a time from
 * the file — the whole file is NEVER loaded into memory. Each block is
 * self-contained: HUF builds its own Huffman table inside the one timed
 * HUF_compress() call (table construction is part of the measurement, by
 * design — no tree extraction). A single-symbol block compresses to 1 byte
 * (degenerate tree), so no RLE path is needed. If a block is incompressible
 * HUF_compress returns 0, and that block is stored raw (marker 0xFFFFFFFF +
 * raw bytes). zpl_rdtsc() timing per block, identical to rc_encode; file I/O is
 * outside the timed region.
 *
 * No threads are spawned — HUF/FSE are single-threaded; "4X" is 4 interleaved
 * bitstreams within one thread (ILP), not OS threads.
 *
 * Usage:
 *   huf_encode <input_file> <output_file.huf>
 *
 * .huf format:
 *   [4]   signature: 'h','u', 0, 0
 *   [8]   uint64_t original_len (LE)
 *   then per block (INPUT size HUF_BLOCK_BYTES = 128 KB):
 *     [4]   uint32_t cs   (compressed size; 0xFFFFFFFF = raw/incompressible)
 *     [cs]  HUF bitstream, OR [bs] raw bytes if cs == 0xFFFFFFFF
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

#define HUF_BLOCK_BYTES  (128 * 1024)          /* == HUF_BLOCKSIZE_MAX */
#define RAW_MARKER       0xFFFFFFFFu

/* read exactly want bytes from fin into buf (untimed I/O) */
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
        fprintf(stderr, "Usage: %s <input_file> <output_file.huf> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && strcmp(argv[3], "no_progress") == 0);

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "fopen input\n"); return 1; }
    zpl_i64 fsize = zpl_file_size(&fin);
    if (fsize <= 0) { fprintf(stderr, "file_size failed\n"); zpl_file_close(&fin); return 1; }
    uint64_t n = (uint64_t)fsize;

    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); zpl_file_close(&fin); return 1; }

    /* --- header --- */
    uint8_t sig[4] = { 'h', 'u', 0, 0 };
    zpl_file_write(&fout, sig, sizeof(sig));
    zpl_file_write(&fout, &n, sizeof(n));

    /* --- CPU frequency calibration --- */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 rdtsc_freq = (zpl_rdtsc() - d0) * 10;
    printf("CPU freq = %1.1f Mhz\n", rdtsc_freq / 1000000.0f);

    /* --- encode: read 128KB block from file, compress (timed), write --- */
    uint8_t *blockbuf = (uint8_t *)malloc(HUF_BLOCK_BYTES);
    uint8_t *blkdst   = (uint8_t *)malloc(HUF_compressBound(HUF_BLOCK_BYTES));
    zpl_u64 total_ticks = 0;
    size_t blocks = 0, raw_blocks = 0;
    uint64_t off = 0;
    while (off < n) {
        size_t bs = zpl_min((size_t)HUF_BLOCK_BYTES, (size_t)(n - off));
        if (read_exact(&fin, blockbuf, bs)) {        /* I/O, outside timing */
            fprintf(stderr, "read block\n");
            zpl_file_close(&fin); zpl_file_close(&fout); return 1;
        }
        zpl_u64 t0 = zpl_rdtsc();
        size_t cs = HUF_compress(blkdst, HUF_compressBound(bs), blockbuf, bs);
        total_ticks += zpl_rdtsc() - t0;
        if (HUF_isError(cs)) {
            fprintf(stderr, "HUF_compress: %s\n", HUF_getErrorName(cs));
            zpl_file_close(&fin); zpl_file_close(&fout); return 1;
        }
        if (cs == 0 || cs >= bs) {
            uint32_t m = RAW_MARKER;                  /* incompressible -> raw */
            zpl_file_write(&fout, &m, sizeof(m));
            zpl_file_write(&fout, blockbuf, bs);
            raw_blocks++;
        } else {
            uint32_t cs32 = (uint32_t)cs;
            zpl_file_write(&fout, &cs32, sizeof(cs32));
            zpl_file_write(&fout, blkdst, cs);
        }
        off += bs; blocks++;

        if (!no_progress) {
            printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)",
                   off, n, 100.0 * (double)off / (double)n);
            fflush(stdout);
        }
    }
    if (!no_progress) printf("\n");
    zpl_file_close(&fin);

    zpl_i64 out_bytes = zpl_file_size(&fout);
    zpl_file_close(&fout);
    free(blockbuf); free(blkdst);

    /* --- report --- */
    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;
    uint64_t total_enc_time_ms = (rdtsc_freq > 0) ? (uint64_t)(total_ticks * 1000 / rdtsc_freq) : 0;
    double ratio = (n > 0) ? (double)out_bytes / (double)n * 100.0 : 0.0;
    double bpb   = (n > 0) ? (double)out_bytes * 8.0 / (double)n : 0.0;
    double mb_s  = (total_enc_time_ms > 0) ? (double)n / (total_enc_time_ms * 1048576.0 / 1000.0) : 0.0;

    printf("ENCODE-HUF OK\n");
    printf("  engine    : HUF (Huff0), block=%dKB, per-block table, streaming\n", HUF_BLOCK_BYTES / 1024);
    printf("  input     : %s\n", argv[1]);
    printf("  in_size   : %" PRIu64 " bytes\n", n);
    printf("  out_size  : %" PRIi64 " bytes\n", out_bytes);
    printf("  blocks    : %zu  (block_bytes=%d, raw=%zu)\n", blocks, HUF_BLOCK_BYTES, raw_blocks);
    printf("  bpb       : %.3f\n", bpb);
    printf("  ratio_pct : %.2f\n", ratio);
    printf("  encode_ms : %" PRIu64 "\n", total_enc_time_ms);
    printf("  encode_mbs: %.1f\n", mb_s);
    printf("  enc_ticks : %u\n", ticks_per_symbol);
    return 0;
}