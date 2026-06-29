/* =========================================================================
 * RC32SUB — Carryless (Subbotin) Range Coder, byte-oriented, 32-bit state
 * =========================================================================
 *
 * Original algorithm by Dmitry Subbotin (1999), public domain.
 * 32-bit low/range/code state, TOP=2^24, BOT=2^16, 8-bit byte output.
 * No carry propagation: the renorm loop trims range to a 2^16 boundary
 * instead of patching already-written output bytes. This makes it safe
 * for interleaving (no backward writes).
 *
 * 12-bit model (TARGET_TOTAL_12 = 4096). Output: uint8_t bytes (not words).
 *
 * Format note: produces a byte stream (not uint16_t words like RC32).
 * The .rc32sub file is structurally identical to .rc32 (524-byte header +
 * stream), except: signature 'r','4' and the stream is bytes not words.
 * ========================================================================= */

#ifndef RC32SUB_CODEC_H
#define RC32SUB_CODEC_H

#include <stdint.h>
#include <stddef.h>

#include "model_12.h"

#define RC32SUB_TOP    (1u << 24)
#define RC32SUB_BOT    (1u << 16)
#define RC32SUB_TOTAL  TARGET_TOTAL_12

/* ---------- ENCODER ---------- */

typedef struct {
    uint32_t  low;
    uint32_t  range;
    uint8_t  *out;     /* output buffer of bytes */
    size_t    cap;     /* buffer capacity in bytes */
    size_t    pos;     /* bytes written */
} rc32sub_enc_t;

static inline void rc32sub_enc_init(rc32sub_enc_t *e, uint8_t *out, size_t cap) {
    e->low   = 0;
    e->range = 0xFFFFFFFFu;
    e->out   = out;
    e->cap   = cap;
    e->pos   = 0;
}

static inline void rc32sub_enc_step(rc32sub_enc_t *e, uint32_t cumFreq,
                                 uint32_t freq) {
    e->low  += cumFreq * (e->range /= RC32SUB_TOTAL);
    e->range *= freq;
    while ((e->low ^ (e->low + e->range)) < RC32SUB_TOP ||
           (e->range < RC32SUB_BOT && ((e->range = -e->low & (RC32SUB_BOT-1)), 1))) {
        if (e->pos < e->cap)
            e->out[e->pos] = (uint8_t)(e->low >> 24);
        e->pos++;
        e->range <<= 8;
        e->low   <<= 8;
    }
}

static inline void rc32sub_enc_flush(rc32sub_enc_t *e) {
    int i;
    for (i = 0; i < 4; i++) {
        if (e->pos < e->cap)
            e->out[e->pos] = (uint8_t)(e->low >> 24);
        e->pos++;
        e->low <<= 8;
    }
}

static inline size_t rc32sub_enc_size(const rc32sub_enc_t *e) { return e->pos; }

/* ---------- DECODER ---------- */

typedef struct {
    uint32_t        code;
    uint32_t        range;
    uint32_t        low;
    const uint8_t  *in;
    size_t          len;
    size_t          pos;
} rc32sub_dec_t;

static inline void rc32sub_dec_init(rc32sub_dec_t *d, const uint8_t *in, size_t len) {
    int i;
    d->in    = in;
    d->len   = len;
    d->pos   = 0;
    d->range = 0xFFFFFFFFu;
    d->low   = 0;
    d->code  = 0;
    for (i = 0; i < 4; i++)
        d->code = (d->code << 8) | ((d->pos < d->len) ? d->in[d->pos++] : 0);
}

static inline uint32_t rc32sub_dec_get_cum(rc32sub_dec_t *d) {
    d->range /= RC32SUB_TOTAL; return (d->code - d->low) / d->range;
}

static inline void rc32sub_dec_step(rc32sub_dec_t *d, uint32_t cumFreq,
                                 uint32_t freq) {
    d->low  += cumFreq * d->range;
    d->range *= freq;
    while ((d->low ^ (d->low + d->range)) < RC32SUB_TOP ||
           (d->range < RC32SUB_BOT && ((d->range = -d->low & (RC32SUB_BOT-1)), 1))) {
        d->code  = (d->code << 8) | ((d->pos < d->len) ? d->in[d->pos++] : 0);
        d->range <<= 8;
        d->low   <<= 8;
    }
}

#endif /* RC32SUB_CODEC_H */
