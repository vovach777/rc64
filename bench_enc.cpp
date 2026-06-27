/* =========================================================================
 * bench_enc — rANS N-way interleave ENCODE benchmark
 * =========================================================================
 *
 * A parameterized clone of rans_encode.cpp: the interleave factor N is a
 * compile-time constant (-DNINTER=n). It emits the SAME .rans format as the
 * production tools for that N (N flush pairs per block), so a bench_dec<n>
 * decodes and verifies a bench_enc<n> stream. Default NINTER=4 (matches the
 * production codec).
 *
 *   build: clang++ -O3 -std=c++17 [-DNINTER=4] -DNDEBUG -I. bench_enc.cpp
 *   usage: bench_enc <input_file> <output.rans>
 *
 * Timed region = the interleave/encode loop only (I/O and model build excluded).
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
constexpr uint32_t RENORM_BUF_WORDS = (BLOCK_BYTES * 14u / 32u + 1024 + 0xff) & ~0xffu;

#ifndef NINTER
#define NINTER 4
#endif
constexpr int N = NINTER;

/* Encode one block into buf (writing renorm words + N flush pairs from the
   end). States are ROTATED (not k&N-indexed) so they stay in GPRs for N<=5. */
template<int NN>
static void enc_block(const uint8_t *data, size_t base, size_t syms,
                      const cums_t m, const rfreq_tab_t rf,
                      rANS::Encoder (&e)[NN], uint32_t *&head)
{
    for (int k = (int)syms - 1; k >= 0; --k) {
        rANS::Encoder enc = e[NN - 1];
        uint16_t lo, f;
        model_get(m, data[base + k], &lo, &f);
        auto res = enc.Rans64EncPut_no_div(lo, f, rf[data[base + k]], RANS_SCALE_BITS);
        if (res) *(--head) = *res;
        if constexpr (NN >= 2) e[NN - 1] = e[NN - 2];
        if constexpr (NN >= 3) e[NN - 2] = e[NN - 3];
        if constexpr (NN >= 4) e[NN - 3] = e[NN - 4];
        if constexpr (NN >= 5) e[NN - 4] = e[NN - 5];
        if constexpr (NN >= 6) e[NN - 5] = e[NN - 6];
        if constexpr (NN >= 7) e[NN - 6] = e[NN - 7];
        if constexpr (NN >= 8) e[NN - 7] = e[NN - 8];
        e[0] = enc;
    }
    for (int s = NN - 1; s >= 0; --s) {
        auto fp = e[s].flush();
        *(--head) = fp.first;
        *(--head) = fp.second;
    }
}

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
    rfreq_tab_t rf; model_build_rfreq(rf, m);

    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) { fprintf(stderr, "fopen output\n"); free(data); return 1; }
    uint8_t sig[4] = { 'r', 'n', 0, 0 };
    zpl_file_write(&fout, sig, 4);
    zpl_file_write(&fout, &n, 8);
    zpl_file_write(&fout, m + 1, sizeof(m[0]) * ALPHABET);

    /* CPU frequency calibration */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100, d0 = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 freq = (zpl_rdtsc() - d0) * 10;

    static uint32_t buf[RENORM_BUF_WORDS];
    rANS::Encoder encs[N];
    zpl_u64 tt = 0;
    size_t i = 0; auto rem = n;
    while (rem) {
        for (int s = 0; s < N; s++) encs[s] = rANS::Encoder{};
        size_t syms = zpl_min((size_t)BLOCK_BYTES, rem);
        uint32_t *head = buf + RENORM_BUF_WORDS;
        zpl_u64 t0 = zpl_rdtsc();
        enc_block<N>(data, i, syms, m, rf, encs, head);
        tt += zpl_rdtsc() - t0;
        i += syms; rem -= syms;
        size_t cnt = buf + RENORM_BUF_WORDS - head;
        zpl_file_write(&fout, head, sizeof(buf[0]) * cnt);
    }
    zpl_file_close(&fout);
    free(data);

    double ms  = freq ? (double)(tt * 1000.0 / freq) : 0;
    double mbs = ms ? (double)n / (ms * 1048576.0 / 1000.0) : 0;
    printf("ENC N=%d  in=%" PRIu64 "  ticks/sym=%.2f  %.1f MB/s  (%.0f ms)\n",
           N, n, (double)tt / n, mbs, ms);
    return 0;
}
