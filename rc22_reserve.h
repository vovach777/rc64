/* =========================================================================
 * rc22_reserve.h -- ADDITIVE variant for rc64.  NOTHING existing is touched.
 *
 * NUMERIC CORE: faithful port of the PROVEN rc22/rc22_bitbuf.c kernel.
 *   [2] extraction | [8] shock absorber | [12] scale = 22-bit state.
 *     BITS = 22, TOP = 2^22, MAX_RANGE = TOP-1, TOTAL = 4096 (2^12),
 *     TARGET_MSB = 14, RENORM_MIN = 2^14.
 *   TRUE O(1) renorm: shift = 14 - floor(log2(range)), CLZ-based, NO loop.
 *   Fast path  : low + r*TOTAL < TOP  -> plain range update.
 *   Bypass path: zero-waste bitwise radix, range = min(TOP-low, r).
 *   Decoder is division-free: one 32x32->64 multiply with
 *     inv_r_lut[r] = floor(2^32 / r), r in [1..1023], plus a single +1
 *     correction to make the quotient EXACT (no `div`, no off-by-one).
 *
 * THE POINT (owner's insight, "22 бит range: меньше памяти для обратного
 * значения для деления таблиц"):
 *   r_max = (TOP-1)/TOTAL = 1023  ==>  EXACTLY 1024 LUT slots.
 *   inv_r_lut[1024] is uint32_t = 4 KiB.  The 24-bit state of rc64 needs
 *   the 4096-slot uint64 table + __uint128_t multiply (rc_fast_div24).
 *   Here a single `mul` high word is enough: smaller table, smaller cache
 *   footprint, no 128-bit arithmetic.
 *
 * RESERVE-AHEAD (Buf::take idiom, from llcomp/rc; additive to rc24_reserve):
 *   - each stream calls r22_take(B, R22_INITBYTES) to RESERVE its first 3
 *     slots; the first 3 emitted bytes land there;
 *   - every later byte takes ONE fresh slot in GLOBAL EVENT ORDER;
 *   - streams are processed in stream order, so each stream occupies one
 *     CONTIGUOUS run of slots and the decoder needs ONE global nx pointer --
 *     NO per-stream sizes / offsets / max_stream_len in the file;
 *   - buffer length is exactly B.next.
 *
 * WHY 3 INIT BYTES, NOT 8 (rc22_bitbuf uses an 8-byte prefetch):
 *   Let S = total renorm shift bits of a stream, T = S + 22 (encoder pushes
 *   the 22-bit flush codepoint).  Encoder emits E = ceil(T/8) bytes.
 *   Decoder init reads 3 bytes = 24 bits, takes 22 for `code`, leaving
 *   bit_count = 2; renorm consumes S bits, refilling R = ceil((S-2)/8)
 *   bytes.  E = ceil((S+22)/8) = 3 + ceil((S-2)/8) = 3 + R, EXACTLY.
 *   So nx lands precisely on the next stream's first slot.  This is the
 *   invariant that makes the single global pointer safe for N streams.
 * ========================================================================= */
#ifndef RC22_RESERVE_H
#define RC22_RESERVE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define R22_BITS        22
#define R22_TOP         (1u << R22_BITS)          /* 2^22                 */
#define R22_MAXRANGE    (R22_TOP - 1u)            /* safe init range      */
#define R22_TOTAL       4096u                     /* 2^12 static model    */
#define R22_TARGET_MSB  14
#define R22_RENORM_MIN  (1u << R22_TARGET_MSB)    /* 2^14                 */
#define R22_INITBYTES   3                         /* 3 bytes cover 22 bits */
#define R22_MAXN        8
#define R22_ALPHABET    256

/* ---- 1024-slot uint32 inverse table: floor(2^32 / r), 4 KiB ----------- */
static uint32_t r22_inv[1024]   __attribute__((aligned(128)));
static uint16_t r22_cum[257]    __attribute__((aligned(128)));
static uint32_t r22_cfreq[256]   __attribute__((aligned(128)));
static uint8_t  r22_sym_lut[4096] __attribute__((aligned(128)));

static inline void r22_inv_init(void) {
    r22_inv[0] = 0;
    for (uint32_t r = 1; r < 1024; ++r)
        r22_inv[r] = 0xFFFFFFFFu / r;            /* floor(2^32 / r) */
}

/* Build the static 12-bit model, the symbol LUT and the inverse LUT.
   cums[] is the 257-entry cumulative table from model_12.h. */
static inline void r22_model_init(const uint16_t cums[257]) {
    for (int i = 0; i <= R22_ALPHABET; ++i) r22_cum[i] = cums[i];
    for (int i = 0; i < R22_ALPHABET; ++i)
        r22_cfreq[i] = (uint32_t)(cums[i + 1] - cums[i]);
    for (int s = 0; s < R22_ALPHABET; ++s)
        if (r22_cfreq[s]) memset(r22_sym_lut + r22_cum[s], (uint8_t)s, r22_cfreq[s]);
    r22_inv_init();
}

static inline int r22_fast_log2(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return 31 - __builtin_clz(x | 1u);           /* |1 guards x==0 */
#else
    int k = 0; while (x >>= 1) ++k; return k;
#endif
}

/* EXACT floor(x/r) for r in [1,1023] and x < 2^22.
   floor(2^32/r) underestimates 2^32/r by < 1, so the raw high word is
   either floor(x/r) or floor(x/r)-1; at most ONE +1 fix is needed. */
static inline uint32_t r22_fastdiv(uint32_t x, uint32_t r) {
    if (x >= R22_TOP) x = R22_TOP - 1u;          /* defensive clamp */
    uint32_t v = (uint32_t)(((uint64_t)x * r22_inv[r]) >> 32);
    if ((uint64_t)(v + 1u) * r <= x) ++v;
    else if ((uint64_t)v * r > x) --v;
    return v;
}

/* ---- shared buffer with dynamic slot reservation (Buf::take) ----------- */
typedef struct {
    uint8_t *b;
    size_t   next, cap;
    uint64_t takes;
} r22_buf_t;

static inline void r22_buf_init(r22_buf_t *B, size_t cap) {
    B->cap = cap ? cap : 64;
    B->next = 0; B->takes = 0;
    B->b = (uint8_t *)calloc(B->cap, 1);
    if (!B->b) abort();
}
static inline void r22_buf_free(r22_buf_t *B) {
    free(B->b); B->b = 0; B->next = 0; B->cap = 0;
}
static inline size_t r22_take(r22_buf_t *B, size_t n) {
    size_t s = B->next;
    B->next += n; ++B->takes;
    if (B->next + 16 > B->cap) {
        size_t nc = B->cap * 2 + 64;
        if (nc < B->next + 16) nc = B->next + 16 + 64;
        uint8_t *nb = (uint8_t *)realloc(B->b, nc);
        if (!nb) abort();
        memset(nb + B->cap, 0, nc - B->cap);
        B->b = nb; B->cap = nc;
    }
    return s;
}

/* ---- encoder: 22-bit state + bit packer + reserved slots --------------- */
typedef struct {
    uint32_t low, range;
    uint64_t bit_buf;          /* < 22 pending bits, MSB-first */
    int      bit_count;        /* 0..7 between renorms */
    uint32_t slot_base;        /* first R22_INITBYTES reserved slots */
    int      local_len;        /* bytes already written into reserved slots */
    uint64_t renorms, bypass_cnt, bypass_steps;
} r22_enc_t;

/* Emit ONE byte: first R22_INITBYTES go into the reserved slots, every
 * later byte takes a fresh slot in global event order.
 *
 * CRITICAL: r22_take() may REALLOC B->b.  Writing `B->b[r22_take(B,1)] = v`
 * is unsequenced in C -- a compiler may load the OLD B->b before the call,
 * so `v` lands in freed memory and the fresh slot stays zero.  Always take
 * the slot index FIRST, then dereference the (possibly new) B->b. */
static inline void r22_enc_byte(r22_enc_t *E, r22_buf_t *B, uint8_t v) {
    if (E->local_len < R22_INITBYTES) {
        B->b[E->slot_base + (uint32_t)E->local_len++] = v;
    } else {
        size_t s = r22_take(B, 1);
        B->b[s] = v;
    }
}

static inline void r22_enc_init(r22_enc_t *E, r22_buf_t *B) {
    E->low = 0; E->range = R22_MAXRANGE;
    E->bit_buf = 0; E->bit_count = 0;
    E->slot_base = (uint32_t)r22_take(B, R22_INITBYTES);
    E->local_len = 0;
    E->renorms = 0; E->bypass_cnt = 0; E->bypass_steps = 0;
}

/* Push `shift` bits into the packer, flushing whole bytes to slots.
 * bit_count stays < 8, shift <= 14, so bit_count+shift < 22: no overflow. */
static inline void r22_enc_push(r22_enc_t *E, r22_buf_t *B,
                                uint32_t bits, int shift) {
    E->bit_buf = (E->bit_buf << shift) | bits;
    E->bit_count += shift;
    while (E->bit_count >= 8) {
        E->bit_count -= 8;
        uint8_t out = (uint8_t)(E->bit_buf >> E->bit_count);
        E->bit_buf &= (1ull << E->bit_count) - 1u;
        r22_enc_byte(E, B, out);
    }
}

static inline void r22_enc_renorm(r22_enc_t *E, r22_buf_t *B) {
    if (E->range < R22_RENORM_MIN) {
        int shift = R22_TARGET_MSB - r22_fast_log2(E->range);
        uint32_t bits = (E->low >> (R22_BITS - shift)) & ((1u << shift) - 1u);
        r22_enc_push(E, B, bits, shift);
        E->low = (E->low << shift) & (R22_TOP - 1u);
        E->range <<= shift;
        ++E->renorms;
    }
}

static inline void r22_enc_encode(r22_enc_t *E, r22_buf_t *B, uint8_t sym) {
    uint32_t r = E->range / R22_TOTAL;
    if (E->low + r * R22_TOTAL < R22_TOP) {
        /* Fast path (99.9%) */
        uint32_t c = r22_cum[sym];
        E->low  += r * c;
        E->range = r * r22_cfreq[sym];
        r22_enc_renorm(E, B);
    } else {
        /* Zero-waste bitwise bypass */
        ++E->bypass_cnt;
        int val = sym, bits_encoded = 0;
        while (bits_encoded < 8) {
            ++E->bypass_steps;
            r = E->range / R22_TOTAL;
            uint32_t gap_m1 = R22_TOP - E->low - 1u;
            uint32_t A = gap_m1 + r;
            int k = r22_fast_log2(A) - r22_fast_log2(r);
            if ((r << k) > A) --k;
            int bits = k;
            if (bits > 8 - bits_encoded) bits = 8 - bits_encoded;
            uint32_t P = 1u << bits, V = 0;
            if (bits > 0) {
                V = (uint32_t)val & (P - 1u);
                val >>= bits;
                bits_encoded += bits;
                E->low += r * V;
            }
            uint32_t rem = R22_TOP - E->low;
            E->range = (rem < r) ? rem : r;
            r22_enc_renorm(E, B);
        }
    }
}

/* Push the 22-bit codepoint, then flush all remaining bytes (zero-padded). */
static inline void r22_enc_flush(r22_enc_t *E, r22_buf_t *B) {
    uint32_t cp = E->low + E->range - 1u;
    E->bit_buf = (E->bit_buf << R22_BITS) | cp;
    E->bit_count += R22_BITS;
    while (E->bit_count >= 8) {
        E->bit_count -= 8;
        uint8_t out = (uint8_t)(E->bit_buf >> E->bit_count);
        E->bit_buf &= (1ull << E->bit_count) - 1u;
        r22_enc_byte(E, B, out);
    }
    if (E->bit_count > 0) {
        r22_enc_byte(E, B, (uint8_t)(E->bit_buf << (8 - E->bit_count)));
        E->bit_count = 0;
    }
}

/* ---- decoder: ONE nx pointer over the whole shared buffer -------------- */
typedef struct {
    uint32_t low, range, code;
    uint64_t bit_buf;          /* unconsumed future bits, MSB-first */
    int      bit_count;
    uint64_t renorms, bypass_steps;
} r22_dec_t;

static inline uint8_t r22_dec_byte(const uint8_t *b, uint32_t total, uint32_t *nx) {
    return (*nx < total) ? b[(*nx)++] : (uint8_t)0;   /* zero padding past EOF */
}

static inline void r22_dec_init(r22_dec_t *D, const uint8_t *b,
                                uint32_t total, uint32_t *nx) {
    D->low = 0; D->range = R22_MAXRANGE; D->code = 0;
    D->bit_buf = 0; D->bit_count = 0;
    D->renorms = 0; D->bypass_steps = 0;
    for (int i = 0; i < R22_INITBYTES; ++i) {
        D->bit_buf = (D->bit_buf << 8) | r22_dec_byte(b, total, nx);
        D->bit_count += 8;
    }
    D->bit_count -= R22_BITS;                      /* == 2 */
    D->code = (uint32_t)(D->bit_buf >> D->bit_count) & (R22_TOP - 1u);
    D->bit_buf &= (1ull << D->bit_count) - 1u;
}

static inline void r22_dec_renorm(r22_dec_t *D, const uint8_t *b,
                                  uint32_t total, uint32_t *nx) {
    if (D->range < R22_RENORM_MIN) {
        int shift = R22_TARGET_MSB - r22_fast_log2(D->range);
        D->range <<= shift;
        D->low = (D->low << shift) & (R22_TOP - 1u);
        while (D->bit_count < shift) {
            D->bit_buf = (D->bit_buf << 8) | r22_dec_byte(b, total, nx);
            D->bit_count += 8;
        }
        /* bit_buf keeps its significant bits in the LOW lanes (we mask it
         * after every step), and fresh bytes are appended at the bottom.
         * So the NEXT bits to consume are the HIGH bits of bit_buf: take
         * them BEFORE shrinking bit_count. Shifting by the already-reduced
         * count would always yield 0 and silently feed zeros into `code`. */
        uint32_t bits = (uint32_t)(D->bit_buf >> (D->bit_count - shift))
                        & ((1u << shift) - 1u);
        D->bit_count -= shift;
        D->code = ((D->code << shift) | bits) & (R22_TOP - 1u);
        D->bit_buf &= (1ull << D->bit_count) - 1u;
        ++D->renorms;
    }
}

static inline uint8_t r22_dec_decode(r22_dec_t *D, const uint8_t *b,
                                     uint32_t total, uint32_t *nx) {
    uint32_t r = D->range / R22_TOTAL;
    if (D->low + r * R22_TOTAL < R22_TOP) {
        /* Fast path */
        uint32_t target = r22_fastdiv(D->code - D->low, r);
        if (target >= R22_TOTAL) target = R22_TOTAL - 1u;
        uint8_t s = r22_sym_lut[target];
        uint32_t c = r22_cum[s];
        D->low  += r * c;
        D->range = r * r22_cfreq[s];
        r22_dec_renorm(D, b, total, nx);
        return s;
    } else {
        /* Zero-waste bitwise bypass */
        int val = 0, bits_decoded = 0;
        while (bits_decoded < 8) {
            ++D->bypass_steps;
            r = D->range / R22_TOTAL;
            uint32_t gap_m1 = R22_TOP - D->low - 1u;
            uint32_t A = gap_m1 + r;
            int k = r22_fast_log2(A) - r22_fast_log2(r);
            if ((r << k) > A) --k;
            int bits = k;
            if (bits > 8 - bits_decoded) bits = 8 - bits_decoded;
            uint32_t P = 1u << bits, V = 0;
            if (bits > 0) {
                uint32_t target = r22_fastdiv(D->code - D->low, r);
                V = target;
                if (V >= P) V = P - 1u;
                val |= (int)(V << bits_decoded);
                bits_decoded += bits;
                D->low += r * V;
            }
            uint32_t rem = R22_TOP - D->low;
            D->range = (rem < r) ? rem : r;
            r22_dec_renorm(D, b, total, nx);
        }
        return (uint8_t)val;
    }
}
#endif /* RC22_RESERVE_H */
