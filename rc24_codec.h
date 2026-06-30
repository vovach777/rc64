/* RC24 — 24-bit Carryless RC, TOP=BOT=2^16. 12-bit model.
 * Decoder division is selectable:
 *   - default: direct 12-bit LUT (rc_fast_div24) — fastest on Haswell.
 *   - -DUSE_INT_DIV24: plain integer division (code-low)/range.
 *   - -DUSE_SSE_RCP24: SSE _mm_rcp_ss Newton-Raphson.
 * Subbotin carryless property makes N-way interleave trivial: no merge. */
#ifndef RC24_CODEC_H
#define RC24_CODEC_H
#include <stdint.h>
#include <stddef.h>
#include "model_12.h"

#define RC24_TOP    (1u << 16)
#define RC24_BOT    (1u << 16)
#define RC24_TOTAL  TARGET_TOTAL_12
#define RC24_MASK   0xFFFFFFu

typedef struct { uint32_t low, range; uint8_t *out; size_t cap, pos; } rc24_enc_t;
static inline void rc24_enc_init(rc24_enc_t *e, uint8_t *out, size_t cap) {
    e->low=0; e->range=RC24_MASK; e->out=out; e->cap=cap; e->pos=0; }
static inline void rc24_enc_step(rc24_enc_t *e, uint32_t cumFreq, uint32_t freq) {
    e->low  += cumFreq * (e->range /= RC24_TOTAL);
    e->range *= freq;
    while ((e->low ^ (e->low + e->range)) < RC24_TOP ||
           (e->range < RC24_BOT && ((e->range = -e->low & (RC24_BOT - 1)), 1))) {
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

typedef struct { uint32_t code, range, low; const uint8_t *in; size_t len, pos; } rc24_dec_t;
static inline void rc24_dec_init(rc24_dec_t *d, const uint8_t *in, size_t len) {
    d->in=in; d->len=len; d->pos=0; d->range=RC24_MASK; d->low=0; d->code=0;
    int i; for (i=0;i<3;i++)
        d->code = ((d->code << 8) | (uint32_t)((d->pos<d->len)?d->in[d->pos++]:0)) & RC24_MASK;
}
static inline uint32_t rc24_dec_get_cum(rc24_dec_t *d) {
    d->range >>= 12;   /* range / 4096 */
#if defined(USE_INT_DIV24)
    return (d->code - d->low) / d->range;
#elif defined(USE_SSE_RCP24)
    return rc_fast_div24_rcp(d->code - d->low, d->range);
#else
    return rc_fast_div24(d->code - d->low, d->range);
#endif
}
static inline void rc24_dec_step(rc24_dec_t *d, uint32_t cumFreq, uint32_t freq) {
    d->low  += cumFreq * d->range;
    d->range *= freq;
    while ((d->low ^ (d->low + d->range)) < RC24_TOP ||
           (d->range < RC24_BOT && ((d->range = -d->low & (RC24_BOT - 1)), 1))) {
        d->code  = ((d->code << 8) | (uint32_t)((d->pos<d->len)?d->in[d->pos++]:0)) & RC24_MASK;
        d->range <<= 8;
        d->low   = (d->low << 8) & RC24_MASK;
    }
}
#endif
