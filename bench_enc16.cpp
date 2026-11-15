/* =========================================================================
 * bench_enc16 — rANS16x16 ENCODE benchmark (16 interleaved word-rANS states)
 * =========================================================================
 *
 * The fNBLI scheme (see rans16x16_codec.h): 32-bit states, 16-bit renorm, 14-bit
 * static model, symbol k on state k & 15. Same container as bench_enc (signature
 * 'r','x'), so the min-of-5 harness in bench_rans.sh reads it the same way.
 *
 *   build: clang++ -O3 -std=c++17 -march=native -DNDEBUG -I. bench_enc16.cpp
 *   usage: bench_enc16 <input_file> <output.r16>
 *
 * Timed region = the encode loop only (I/O and model build excluded).
 * ========================================================================= */
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <inttypes.h>

#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"

#include "rans16x16_codec.h"
#include "model.h"

#define BLOCK_BYTES      (256 * 1024)
constexpr size_t BUF_WORDS = BLOCK_BYTES + 4096;

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s <in> <out.rans>\n", argv[0]); return 1; }

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "fopen input\n"); return 1; }
    zpl_i64 fs = zpl_file_size(&fin);
    if (fs <= 0) { fprintf(stderr, "file_size\n"); return 1; }
    uint64_t n = (uint64_t)fs;
    uint8_t *data = (uint8_t *)malloc((size_t)n);
    {
        uint8_t *p = data; int64_t r = (int64_t)n;
        while (r > 0) { uint32_t c = (uint32_t)zpl_min(r, 0x1000000LL); zpl_file_read(&fin, p, c); p += c; r -= c; }
    }
    zpl_file_close(&fin);

    uint32_t raw[ALPHABET]; memset(raw, 0, sizeof raw);
    for (size_t i = 0; i < n; i++) raw[data[i]]++;
    cums_t m;
    int is_rle = model_build(m, raw);
    if (is_rle != 0) { fprintf(stderr, "bench expects non-RLE input\n"); free(data); return 1; }
    r16::EncSym tab[ALPHABET];
    for (int s = 0; s < ALPHABET; s++) r16::enc_sym_init(tab[s], m[s], uint32_t(m[s + 1] - m[s]));

    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); free(data); return 1; }
    uint8_t sig[4] = { 'r', 'x', 0, 0 };
    zpl_file_write(&fout, sig, 4);
    zpl_file_write(&fout, &n, 8);
    zpl_file_write(&fout, m + 1, sizeof(m[0]) * ALPHABET);

    /* CPU frequency calibration */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 freq = (zpl_rdtsc() - d0) * 10;

    static uint16_t buf[BUF_WORDS];
    zpl_u64 tt = 0;
    size_t i = 0; auto rem = n;
    while (rem) {
        size_t syms = zpl_min((size_t)BLOCK_BYTES, rem);
        uint16_t *head = buf + BUF_WORDS;
        zpl_u64 t0 = zpl_rdtsc();
        head = r16::enc_block(data + i, syms, tab, head);
        tt += zpl_rdtsc() - t0;
        i += syms; rem -= syms;
        size_t cnt = buf + BUF_WORDS - head;
        zpl_file_write(&fout, head, sizeof(buf[0]) * cnt);
    }
    zpl_file_close(&fout);
    free(data);

    double ms  = freq ? (double)(tt * 1000.0 / freq) : 0;
    double mbs = ms ? (double)n / (ms * 1048576.0 / 1000.0) : 0;
    printf("ENC x%d  in=%" PRIu64 "  ticks/sym=%.2f  %.1f MB/s  (%.0f ms)\n",
           r16::LANES, n, (double)tt / n, mbs, ms);
    return 0;
}
