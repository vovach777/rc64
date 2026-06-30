/* RC24 — Single-file codec header.
 * Carryless 24-bit range coder, 12-bit model, human-readable renorm.
 * Includes scalar encoder/decoder and building blocks for 2-way interleave.
 */
#ifndef RC24_CODEC_H
#define RC24_CODEC_H
#include <stdint.h>
#include <stddef.h>
#include "model_12.h"

#define RC24_TOP    (1u << 16)
#define RC24_BOT    (1u << 16)
#define RC24_TOTAL  TARGET_TOTAL_12
#define RC24_MASK   0xFFFFFFu

/* ---- Encoder ---- */
typedef struct { uint32_t low, range; uint8_t *out; size_t cap, pos; } rc24_enc_t;
static inline void rc24_enc_init(rc24_enc_t *e, uint8_t *out, size_t cap) {
    e->low=0; e->range=RC24_MASK; e->out=out; e->cap=cap; e->pos=0; }
static inline void rc24_enc_step(rc24_enc_t *e, uint32_t cumFreq, uint32_t freq) {
    e->low  += cumFreq * (e->range /= RC24_TOTAL);
    e->range *= freq;
    while (1) {
        if ((e->low ^ (e->low + e->range)) < RC24_TOP) {
            /* top bytes match: shift out */
        } else if (e->range < RC24_BOT) {
            /* range too small: Subbotin trim, then guaranteed shift-out */
            e->range = -e->low & (RC24_BOT - 1);
        } else {
            break;
        }
        if (e->pos < e->cap) e->out[e->pos] = (uint8_t)(e->low >> 16);
        e->pos++;
        e->range <<= 8;
        e->low   = (e->low << 8) & RC24_MASK;
    }
}
static inline void rc24_enc_flush(rc24_enc_t *e) {
    int i; for (i=0;i<3;i++) {
        if (e->pos<e->cap) e->out[e->pos]=(uint8_t)(e->low>>16);
        e->pos++; e->low<<=8;
    }
}
static inline size_t rc24_enc_size(const rc24_enc_t *e) { return e->pos; }

/* ---- Decoder ---- */
typedef struct { uint32_t code, range, low; const uint8_t * __restrict in; size_t pos; } rc24_dec_t;
static inline void rc24_dec_init(rc24_dec_t *d, const uint8_t *in) {
    d->in=in; d->pos=0; d->range=RC24_MASK; d->low=0; d->code=0;
    int i; for (i=0;i<3;i++)
        d->code = ((d->code << 8) | (uint32_t)d->in[d->pos++]) & RC24_MASK;
}
static inline uint32_t rc24_dec_get_cum(rc24_dec_t *d) {
    d->range >>= 12;
    return rc_fast_div24(d->code - d->low, d->range);
}
static inline void rc24_dec_step(rc24_dec_t *d, uint32_t cumFreq, uint32_t freq) {
    uint_fast32_t low   = d->low;
    uint_fast32_t range = d->range;
    uint_fast32_t code  = d->code;
    low   += (uint_fast32_t)cumFreq * range;
    range *= (uint_fast32_t)freq;
    while (1) {
        uint_fast32_t diff = low ^ (low + range);
        if (diff < RC24_TOP) {
        } else if (range < RC24_BOT) {
            range = -low & (RC24_BOT - 1);
        } else {
            d->low = low; d->range = range; d->code = code;
            break;
        }
        code  = ((code << 8) | (uint_fast32_t)d->in[d->pos++]) & RC24_MASK;
        range <<= 8;
        low   = (low << 8) & RC24_MASK;
    }
}
#endif
