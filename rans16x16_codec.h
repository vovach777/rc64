/* =========================================================================
 * rANS16x16 — interleaved word-rANS, states in AVX2 vectors (decode)
 * =========================================================================
 *
 * The scheme fNBLI uses (github WangXuan95/NBLI), written here from scratch:
 *   - LANES states of 32 bits (16, 32, 48 or 64), lower bound L = 2^16, renorm by
 *     one 16-bit word (at most one word per symbol: after x >>= 16 the state is
 *     < 2^16 < x_max);
 *   - static 14-bit model (model.h), symbol k is coded by state k % LANES — the
 *     same rotation as a superscalar interleave, 16 states per vector pair;
 *   - decoder: 16 states in two __m256i, symbol = gather(lut[x & 0x3fff]),
 *     (freq, start) = gather(fs[sym]), x = freq * (x >> 14) + (x & 0x3fff) - start,
 *     then the lanes with x < L shift in the next words of the stream in lane
 *     order (expand by a permutation of a 128-bit load, per 8-lane half).
 *     With LANES > 16 the P = LANES / 16 vector pairs are rotated round-robin:
 *     while one pair waits for its gathers, the next one is in flight.
 *   - encoder: scalar, division-free (ryg rans_byte reciprocal), walks the block
 *     BACKWARDS and writes words BACKWARDS, so the decoder reads them forwards
 *     in exactly the order it needs (step by step, lane 0..LANES-1).
 *
 * Block layout (uint16 words): LANES final states (32 bit, lo word first), then
 * the renorm words. The decoder stops after the block's symbols, its word pointer
 * is then at the next block (no length field, same as the rANS64 tools).
 *
 * Options: R16_LANES = 16 | 32 | 48 | 64, R16_SCALAR (scalar decode even with
 * AVX2), R16_NOGATHER (vector decode, but the two table lookups as scalar loads).
 * ========================================================================= */
#ifndef RANS16X16_CODEC_H
#define RANS16X16_CODEC_H

#include <cstdint>
#include <cstring>
#include "model.h"
#if defined(__AVX2__)
#include <immintrin.h>
#endif

#ifndef R16_LANES
#define R16_LANES 16
#endif

namespace r16 {

constexpr uint32_t L = 1u << 16;
constexpr uint32_t SCALE = 14;
constexpr int LANES = R16_LANES;
static_assert(LANES % 16 == 0 && LANES <= 64, "R16_LANES = 16, 32, 48 or 64");

struct EncSym { uint32_t rcp, bias; uint16_t cmpl, shift; uint32_t freq; };

static inline void enc_sym_init(EncSym& s, uint32_t start, uint32_t freq) {
    s.freq = freq;
    s.cmpl = uint16_t((1u << SCALE) - freq);
    if (freq < 2) { s.rcp = ~0u; s.shift = 0; s.bias = start + (1u << SCALE) - 1; }
    else {
        uint32_t sh = 0; while (freq > (1u << sh)) sh++;
        s.rcp = uint32_t(((1ull << (sh + 31)) + freq - 1) / freq);
        s.shift = uint16_t(sh - 1); s.bias = start;
    }
}

/* encode one symbol into state x, words go to *--head */
static inline void enc_put(uint32_t& x, const EncSym& s, uint16_t*& head) {
    const uint64_t x_max = (uint64_t(L >> SCALE) << 16) * s.freq;
    if (x >= x_max) { *--head = uint16_t(x); x >>= 16; }
    const uint32_t q = uint32_t((uint64_t(x) * s.rcp) >> 32) >> s.shift;
    x = x + s.bias + q * s.cmpl;
}

/* Encode syms bytes; returns the new head (block = [head, old head)). */
static inline uint16_t* enc_block(const uint8_t* data, size_t syms, const EncSym* tab, uint16_t* head) {
    uint32_t x[LANES];
    for (int l = 0; l < LANES; l++) x[l] = L;
    for (size_t k = syms; k-- > 0;) enc_put(x[k % LANES], tab[data[k]], head);
    for (int l = LANES - 1; l >= 0; l--) { *--head = uint16_t(x[l] >> 16); *--head = uint16_t(x[l]); }
    return head;
}

/* decoder tables: lut[slot] -> sym (16 KB + pad), fs[sym] = freq << 16 | start */
struct DecTab {
    alignas(64) uint8_t lut[TARGET_TOTAL + 64];
    alignas(64) uint32_t fs[ALPHABET];
};
static inline void dec_tab_build(DecTab& t, const cums_t m) {
    for (int s = 0; s < ALPHABET; s++) {
        const uint32_t lo = m[s], f = uint32_t(m[s + 1] - m[s]);
        t.fs[s] = (f << 16) | lo;
        for (uint32_t v = lo; v < lo + f; v++) t.lut[v] = uint8_t(s);
    }
    memset(t.lut + TARGET_TOTAL, 0, 64);
}

static inline void dec_one(uint32_t& s, const DecTab& t, const uint16_t*& wp, uint8_t& out) {
    const uint32_t slot = s & ((1u << SCALE) - 1);
    const uint8_t sym = t.lut[slot];
    const uint32_t e = t.fs[sym];
    s = (e >> 16) * (s >> SCALE) + slot - (e & 0xffff);
    if (s < L) s = (s << 16) | *wp++;
    out = sym;
}

/* scalar decode (any CPU): same order as the vector path */
static inline const uint16_t* dec_block_scalar(uint8_t* out, size_t syms, const DecTab& t, const uint16_t* wp) {
    uint32_t x[LANES];
    for (int l = 0; l < LANES; l++) { x[l] = uint32_t(wp[0]) | (uint32_t(wp[1]) << 16); wp += 2; }
    for (size_t k = 0; k < syms; k++) dec_one(x[k % LANES], t, wp, out[k]);
    return wp;
}

#if defined(__AVX2__)
/* expand LUT: for an 8-bit mask, the source index of every lane among the loaded words */
struct ExpandLut {
    alignas(32) int32_t idx[256][8];
    ExpandLut() {
        for (int m = 0; m < 256; m++) { int k = 0; for (int l = 0; l < 8; l++) idx[m][l] = (m >> l & 1) ? k++ : 0; }
    }
};
static const ExpandLut g_expand;

/* one step of 8 states; sym_out gets the 8 symbols (low byte of each 32-bit lane) */
static inline __m256i dec_half(__m256i x, const DecTab& t, const uint16_t*& wp, __m256i& sym_out) {
    const __m256i msk14 = _mm256_set1_epi32((1 << SCALE) - 1);
    const __m256i slot = _mm256_and_si256(x, msk14);
#if defined(R16_NOGATHER)
    alignas(32) uint32_t sa[8], ea[8], ya[8]; _mm256_store_si256((__m256i*)sa, slot);
    for (int l = 0; l < 8; l++) { ya[l] = t.lut[sa[l]]; ea[l] = t.fs[ya[l]]; }
    const __m256i sym = _mm256_load_si256((const __m256i*)ya), e = _mm256_load_si256((const __m256i*)ea);
#else
    const __m256i sym = _mm256_and_si256(_mm256_i32gather_epi32((const int*)t.lut, slot, 1), _mm256_set1_epi32(0xff));
    const __m256i e = _mm256_i32gather_epi32((const int*)t.fs, sym, 4);
#endif
    const __m256i freq = _mm256_srli_epi32(e, 16), start = _mm256_and_si256(e, _mm256_set1_epi32(0xffff));
    x = _mm256_sub_epi32(_mm256_add_epi32(_mm256_mullo_epi32(freq, _mm256_srli_epi32(x, SCALE)), slot), start);
    // renorm: lanes with x < L (x is < 2^31 here, signed compare is fine)
    const __m256i need = _mm256_cmpgt_epi32(_mm256_set1_epi32(int(L)), x);
    const int m = _mm256_movemask_ps(_mm256_castsi256_ps(need));
    const __m256i w = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)wp));
    const __m256i wl = _mm256_permutevar8x32_epi32(w, _mm256_load_si256((const __m256i*)g_expand.idx[m]));
    const __m256i xs = _mm256_or_si256(_mm256_slli_epi32(x, 16), wl);
    x = _mm256_blendv_epi8(x, xs, need);
    wp += __builtin_popcount(unsigned(m));
    sym_out = sym;
    return x;
}

/* vector decode: the states sit in P = LANES / 16 pairs of __m256i, rotated round-robin like the
   states of a superscalar interleave (symbol k -> state k % LANES -> pair (k / 16) % P); while one
   pair waits for its gathers the next pair is in flight. Then the scalar tail on the same states. */
static inline const uint16_t* dec_block_avx2(uint8_t* out, size_t syms, const DecTab& t, const uint16_t* wp) {
    constexpr int P = LANES / 16;
    alignas(32) uint32_t xa[LANES];
    for (int l = 0; l < LANES; l++) { xa[l] = uint32_t(wp[0]) | (uint32_t(wp[1]) << 16); wp += 2; }
    __m256i x[2 * P];
    for (int q = 0; q < 2 * P; q++) x[q] = _mm256_load_si256((const __m256i*)(xa + 8 * q));
    const size_t full = syms / LANES * LANES;
    const __m256i pack = _mm256_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                          0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    for (size_t k = 0; k < full; k += LANES) {
        for (int q = 0; q < P; q++) {                        // P is a constant: the compiler unrolls it
            __m256i s0, s1;
            x[2 * q] = dec_half(x[2 * q], t, wp, s0);
            x[2 * q + 1] = dec_half(x[2 * q + 1], t, wp, s1);
            const __m256i p0 = _mm256_shuffle_epi8(s0, pack), p1 = _mm256_shuffle_epi8(s1, pack);
            const uint64_t a = uint32_t(_mm256_extract_epi32(p0, 0)) | (uint64_t(uint32_t(_mm256_extract_epi32(p0, 4))) << 32);
            const uint64_t b = uint32_t(_mm256_extract_epi32(p1, 0)) | (uint64_t(uint32_t(_mm256_extract_epi32(p1, 4))) << 32);
            memcpy(out + k + 16 * q, &a, 8); memcpy(out + k + 16 * q + 8, &b, 8);
        }
    }
    for (int q = 0; q < 2 * P; q++) _mm256_store_si256((__m256i*)(xa + 8 * q), x[q]);
    for (size_t k = full; k < syms; k++) dec_one(xa[k % LANES], t, wp, out[k]);
    return wp;
}
#endif

#if defined(__AVX512F__)
/* R16_ROT: 16 states in ONE zmm register, one symbol at a time, the register rotated by one lane
   per symbol like the states of a superscalar interleave (lane 0 = current state, the updated state
   goes back in at lane 15). No mask, no lane expand: the renorm word is read by the scalar code. */
static inline const uint16_t* dec_block_rot(uint8_t* out, size_t syms, const DecTab& t, const uint16_t* wp) {
    static_assert(LANES == 16, "R16_ROT needs R16_LANES = 16");
    alignas(64) uint32_t xa[16];
    for (int l = 0; l < 16; l++) { xa[l] = uint32_t(wp[0]) | (uint32_t(wp[1]) << 16); wp += 2; }
    __m512i v = _mm512_load_si512(xa);
    for (size_t k = 0; k < syms; k++) {
        uint32_t s = uint32_t(_mm_cvtsi128_si32(_mm512_castsi512_si128(v)));
        v = _mm512_alignr_epi32(v, v, 1);                        // rotate: lane l <- lane l + 1
        dec_one(s, t, wp, out[k]);
        v = _mm512_mask_set1_epi32(v, __mmask16(0x8000), int(s));  // updated state back at lane 15
    }
    return wp;
}
#endif

static inline const uint16_t* dec_block(uint8_t* out, size_t syms, const DecTab& t, const uint16_t* wp) {
#if defined(R16_ROT) && defined(__AVX512F__)
    return dec_block_rot(out, syms, t, wp);
#elif defined(__AVX2__) && !defined(R16_SCALAR)
    return dec_block_avx2(out, syms, t, wp);
#else
    return dec_block_scalar(out, syms, t, wp);
#endif
}

} // namespace r16
#endif
