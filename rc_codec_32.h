/* =========================================================================
 * 32-BIT IN-PLACE CARRY RANGE CODER (16-bit word granularity)
 *
 * This is the integration of an engine verified on 13+ datasets (bin/16/256-sym,
 * Zipf, AR(1), Laplace, text, urandom, etc.) into the rc64 project format.
 *
 * Interval narrowing principle per symbol:
 *     r      = range >> RC_TOTAL_BITS      // scale (division replaced by shift)
 *     low   += r * cum_freq                // lower bound shift
 *     range  = r * freq                    // new width
 *
 * Carry: low += r*cum_freq can overflow 32 bits. The carry "ripples" into
 * already emitted high words. We patch them directly in the output buffer:
 * ++out[pos-1]; if it was 0xFFFF -> became 0x0000, keep going left (ripple
 * carry) until the carry is absorbed.
 *
 * Renorm: while range < RC_TOP (2^16), the top 16 bits of low will not be
 * changed by future narrowings, so we push them out to the buffer and shift
 * low and range left by 16.
 *
 * Invariant after renorm: range >= RC_TOP  =>  r = range>>RC_TOTAL_BITS >=
 * >= 2^(16-12) = 16 > 0, therefore the division in the decoder and the
 * multiplications are correct (no division by 0).
 *
 * Parameters:
 *   RC_TOTAL_BITS = 12  (RC_TOTAL = 4096)  — frequency scale
 *   RC_TOP_BITS   = 16  (RC_TOP   = 65536) — renorm threshold
 *   Output word   = uint16_t (16 bits)
 *   range precision headroom = RC_TOP_BITS - RC_TOTAL_BITS = 4 bits
 *
 * Alphabet = 256 bytes. Static order-0 model (see model_12.h).
 * ========================================================================= */

#ifndef RC_CODEC_32_H
#define RC_CODEC_32_H

#include <stdint.h>
#include <stddef.h>

#include "model_12.h"   /* TARGET_TOTAL_12 = 2^12 = 4096 */

#define RC32_TOTAL_BITS  12u
#define RC32_TOTAL       (1u << RC32_TOTAL_BITS)   /* 4096 */
#define RC32_TOP_BITS    16u
#define RC32_TOP         (1u << RC32_TOP_BITS)     /* 0x10000 */

typedef struct {
    uint32_t  low;       /* lower bound of the interval (32 bits)          */
    uint32_t  range;     /* interval width, invariant: range >= RC_TOP     */
    uint16_t *out;       /* output buffer of words                         */
    size_t    cap;       /* buffer capacity in words                       */
    size_t    pos;       /* words written (may grow past cap)              */
} rc32_enc_t;

typedef struct {
    uint32_t        code;   /* current read code                            */
    uint32_t        range;  /* interval width                               */
    const uint16_t *in;     /* input buffer of words                        */
    size_t          len;    /* input length in words                        */
    size_t          pos;    /* words read                                   */
} rc32_dec_t;

/* ---------- ENCODER ---------- */

static inline void rc32_enc_init(rc32_enc_t *e, uint16_t *out, size_t cap) {
    e->low   = 0;
    e->range = 0xFFFFFFFFu;
    e->out   = out;
    e->cap   = cap;
    e->pos   = 0;
}

/* Carry +1 propagation backwards over already written words.
   We patch only words that actually exist (i < min(pos, cap)),
   so we do not go past the buffer on overflow. */
static inline void rc32_carry_patch(rc32_enc_t *e) {
    size_t i = (e->pos < e->cap) ? e->pos : e->cap;
    while (i != 0) {
        i--;
        /* ++out[i]; if it overflowed (0xFFFF -> 0x0000) — keep going */
        if (++e->out[i] != 0x0000u)
            return;            /* carry absorbed */
    }
    /* We only reach here on invalid input data:
       freq==0, sum freq>TOTAL, or buffer overflow.
       In a correct stream a carry past the buffer start is impossible. */
}

static inline void rc32_enc_step(rc32_enc_t *e, uint32_t cum_freq,
                                 uint32_t freq) {
    uint32_t r  = e->range >> RC32_TOTAL_BITS;
    uint32_t nl = e->low + r * cum_freq;

    /* detect 32-bit overflow without 64-bit math and without exception branching */
    if (nl < e->low)
        rc32_carry_patch(e);

    e->low   = nl;
    e->range = r * freq;

    /* renorm: push the high word of low out to the buffer immediately */
    while (e->range < RC32_TOP) {
        if (e->pos < e->cap)
            e->out[e->pos] = (uint16_t)(e->low >> 16);
        e->pos++;
        e->low   <<= 16;
        e->range <<= 16;
    }
}

static inline void rc32_enc_flush(rc32_enc_t *e) {
    /* push the remaining 32 bits of low out as two words */
    int i;
    for (i = 0; i < 2; i++) {
        if (e->pos < e->cap)
            e->out[e->pos] = (uint16_t)(e->low >> 16);
        e->pos++;
        e->low <<= 16;
    }
}

static inline size_t rc32_enc_size(const rc32_enc_t *e) { return e->pos; }

/* ---------- DECODER ---------- */

/* read one word (0 past end of stream) */
static inline uint32_t rc32_get(rc32_dec_t *d) {
    uint32_t w = (d->pos < d->len) ? d->in[d->pos] : 0u;
    d->pos++;
    return w;
}

static inline void rc32_dec_init(rc32_dec_t *d, const uint16_t *in, size_t len) {
    d->in    = in;
    d->len   = len;
    d->pos   = 0;
    d->range = 0xFFFFFFFFu;
    /* first 32 bits of code = two high words */
    d->code  = rc32_get(d) << 16;
    d->code |= rc32_get(d);
}

/* How many words were read from the input after init + steps.
   Needed by the caller for diagnostics. */
static inline size_t rc32_dec_pos(const rc32_dec_t *d) { return d->pos; }

/* Compute v = code / r (floor division).
   Default: integer divl (~23 cycles on Skylake).
   When USE_FLOAT_DIV is defined: via double (~13 cycles):
       v = (uint32_t)((double)code / (double)r)
   FP division rounds to nearest, so the result may be 1 greater than
   the true floor. We correct with a single imull+cmp+sbb (~3 cycles),
   for a total of ~16 cycles instead of 23.

   Safety: code <= 0xFFFFFFFF and r >= 16 are exactly representable in
   double (53-bit mantissa), as is their ratio (<= 2^28). */
static inline uint32_t rc32_dec_get_cum(const rc32_dec_t *d) {
    uint32_t r = d->range >> RC32_TOTAL_BITS;
#ifdef USE_FLOAT_DIV
    /* FP division + off-by-one-high correction. */
    uint32_t v = (uint32_t)((double)d->code / (double)r);
    /* If FP rounded up (v * r > code), decrement by 1.
       Branchless: subtract the (v*r > code) flag via sbb. */
    v -= (uint32_t)((uint64_t)v * r > d->code);
#else
    uint32_t v = d->code / r;             /* integer divl */
#endif
    /* clamp in case of landing in the "dead zone" of the range tail */
    return (v >= RC32_TOTAL) ? (RC32_TOTAL - 1u) : v;
}

static inline void rc32_dec_step(rc32_dec_t *d, uint32_t cum_freq,
                                 uint32_t freq) {
    uint32_t r = d->range >> RC32_TOTAL_BITS;
    d->code  -= r * cum_freq;
    d->range  = r * freq;

    /* renorm — symmetric to the encoder */
    while (d->range < RC32_TOP) {
        d->code   = (d->code << 16) | rc32_get(d);
        d->range <<= 16;
    }
}

#endif /* RC_CODEC_32_H */
