/* =========================================================================
 * bench_dec — rANS N-way interleave DECODE benchmark
 * =========================================================================
 *
 * Parameterized clone of rans_decode.cpp. Decodes a bench_enc<n>-produced
 * stream (N flush pairs per block) and verifies the result against the
 * original. Default NINTER=4.
 *
 *   build: clang++ -O3 -std=c++17 [-DNINTER=4] -DNDEBUG -I. bench_dec.cpp
 *   usage: bench_dec <input.rans> <output> <original_to_verify>
 *
 * Timed region = the interleave/decode loop only (I/O excluded).
 * ========================================================================= */

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <inttypes.h>

#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"

#include "rans_codec.h"
#include "model.h"

#define RANS_SCALE_BITS  14u
#define BLOCK_BYTES      (256 * 1024)

#ifndef NINTER
#define NINTER 4
#endif
constexpr int N = NINTER;

/* Decode one block: N states Inited from N flush pairs, then round-robin by
   position (state = k & N). Rotated, not indexed, so states stay in GPRs. */
template<int NN>
static void dec_block(uint8_t *out, size_t syms, const lut_t lut, const cums_t m,
                      rANS::Decoder (&d)[NN], const uint32_t *&wp)
{
    for (size_t k = 0; k < syms; k++) {
        rANS::Decoder dec = d[0];
        uint32_t cum = dec.Rans64DecGet(RANS_SCALE_BITS);
        uint16_t lo, f;
        uint8_t sym = model_find_lut(lut, m, (uint16_t)cum, &lo, &f);
        wp += dec.Rans64Dec(lo, f, RANS_SCALE_BITS, *wp);
        out[k] = sym;
        if constexpr (NN >= 2) d[0] = d[1];
        if constexpr (NN >= 3) d[1] = d[2];
        if constexpr (NN >= 4) d[2] = d[3];
        if constexpr (NN >= 5) d[3] = d[4];
        if constexpr (NN >= 6) d[4] = d[5];
        if constexpr (NN >= 7) d[5] = d[6];
        if constexpr (NN >= 8) d[6] = d[7];
        d[NN - 1] = dec;
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: %s <in.rans> <out> <orig>\n", argv[0]); return 1; }

    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) { fprintf(stderr, "fopen input\n"); return 1; }
    uint8_t sig[4]; zpl_file_read(&fin, sig, 4);
    if (sig[0] != 'r' || sig[1] != 'n') { fprintf(stderr, "bad sig\n"); return 1; }
    uint64_t n; zpl_file_read(&fin, &n, 8);
    size_t sz = (size_t)n;
    cums_t m; zpl_file_read(&fin, m + 1, sizeof(m[0]) * ALPHABET);
    lut_t lut; model_build_lut(lut, m);

    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 freq = (zpl_rdtsc() - d0) * 10;

    zpl_i64 rlen = zpl_file_size(&fin) - 524;
    size_t ww = rlen / 4;
    uint32_t *words = (uint32_t *)malloc((ww + 2) * 4);
    {
        uint8_t *p = (uint8_t *)words; int64_t r = rlen;
        while (r > 0) { uint32_t c = (uint32_t)zpl_min(r, 0x1000000LL); zpl_file_read(&fin, p, c); p += c; r -= c; }
    }
    zpl_file_close(&fin);

    uint8_t *out = (uint8_t *)malloc(sz);
    const uint32_t *wp = words;
    rANS::Decoder decs[N];
    zpl_u64 tt = 0;
    size_t i = 0;
    while (i < sz) {
        size_t syms = zpl_min(sz - i, (size_t)BLOCK_BYTES);
        for (int s = 0; s < N; s++) decs[s].Init({wp[2 * s], wp[2 * s + 1]});
        wp += 2 * N;
        zpl_u64 t0 = zpl_rdtsc();
        dec_block<N>(out + i, syms, lut, m, decs, wp);
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
    printf("DEC N=%d  out=%zu  ticks/sym=%.2f  %.1f MB/s  (%.0f ms)  %s\n",
           N, sz, (double)tt / sz, mbs, ms, ok ? "VERIFIED" : "*** MISMATCH ***");
    free(words); free(out);
    return ok ? 0 : 2;
}
