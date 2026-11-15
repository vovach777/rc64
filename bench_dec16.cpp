/* =========================================================================
 * bench_dec16 — rANS16x16 DECODE benchmark (AVX2: 16 states in two vectors)
 * =========================================================================
 *
 * Decodes a bench_enc16 stream and verifies it against the original. With AVX2
 * the 16 states are decoded together (gathers + lane expand); without AVX2, or
 * with -DR16_SCALAR, the same stream is decoded by a scalar loop.
 *
 *   build: clang++ -O3 -std=c++17 -march=native -DNDEBUG -I. bench_dec16.cpp
 *   usage: bench_dec16 <input.r16> <output> <original_to_verify>
 *
 * Timed region = the decode loop only (I/O excluded).
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

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: %s <in.rans> <out> <orig>\n", argv[0]); return 1; }

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "fopen input\n"); return 1; }
    uint8_t sig[4]; zpl_file_read(&fin, sig, 4);
    if (sig[0] != 'r' || sig[1] != 'x') { fprintf(stderr, "bad sig\n"); return 1; }
    uint64_t n; zpl_file_read(&fin, &n, 8);
    size_t sz = (size_t)n;
    cums_t m; zpl_file_read(&fin, m + 1, sizeof(m[0]) * ALPHABET);
    static r16::DecTab tabd; r16::dec_tab_build(tabd, m);

    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 freq = (zpl_rdtsc() - d0) * 10;

    zpl_i64 rlen = zpl_file_size(&fin) - 524;
    size_t ww = rlen / 2;
    uint16_t *words = (uint16_t *)malloc((ww + 64) * 2);   /* + pad: the vector path loads 8 words ahead */
    memset(words + ww, 0, 64 * 2);
    {
        uint8_t *p = (uint8_t *)words; int64_t r = rlen;
        while (r > 0) { uint32_t c = (uint32_t)zpl_min(r, 0x1000000LL); zpl_file_read(&fin, p, c); p += c; r -= c; }
    }
    zpl_file_close(&fin);

    uint8_t *out = (uint8_t *)malloc(sz);
    const uint16_t *wp = words;
    zpl_u64 tt = 0;
    size_t i = 0;
    while (i < sz) {
        size_t syms = zpl_min(sz - i, (size_t)BLOCK_BYTES);
        zpl_u64 t0 = zpl_rdtsc();
        wp = r16::dec_block(out + i, syms, tabd, wp);
        tt += zpl_rdtsc() - t0;
        i += syms;
    }

    int ok = 1;
    FILE *orig = fopen(argv[3], "rb");
    if (orig) {
        uint8_t tmp[65536]; size_t got = 0, r;
        while ((r = fread(tmp, 1, sizeof tmp, orig)) > 0) {
            if (got + r > sz || memcmp(out + got, tmp, r)) { ok = 0; break; }
            got += r;
        }
        if (ok && got != sz) ok = 0;
        fclose(orig);
    }

    double ms  = freq ? (double)(tt * 1000.0 / freq) : 0;
    double mbs = ms ? (double)sz / (ms * 1048576.0 / 1000.0) : 0;
    printf("DEC x%d  out=%zu  ticks/sym=%.2f  %.1f MB/s  (%.0f ms)  %s\n",
           r16::LANES, sz, (double)tt / sz, mbs, ms, ok ? "VERIFIED" : "*** MISMATCH ***");
    free(words); free(out);
    return ok ? 0 : 2;
}
