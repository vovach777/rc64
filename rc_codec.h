/* =========================================================================
 * SCHINDLER 64-BIT RANGE CODER — INPLACE VARIANT (v2)
 * =========================================================================
 *
 * Cache and FF are written directly to the output buffer. On carry, a single
 * memset patches the FF block to 0x00000000, cache++ inplace. No deferred FF,
 * no write loops on safe emit.
 *
 * The struct holds pointers into the output buffer — memory is allocated once
 * by the caller, no reallocation.
 *
 * IMPORTANT: the placeholder word at the start of the buffer is removed. In
 * init, cache_ptr points to a static dummy (a junk variable in this file).
 * After the first safe-emit, cache_ptr is repointed to the real position in
 * the buffer. If safe-emit never happened (short file), flush writes into the
 * dummy, but in that case cache is always 0, so nothing is lost.
 * ========================================================================= */

#include <stdint.h>
#include <string.h>

#include "model.h"  /* TARGET_TOTAL = 2^14, passed as a compile-time constant */

#define SCHINDLER_BOTTOM_64 0x0000000100000000ULL

typedef struct {
    uint64_t low;
    uint64_t range;
    uint32_t cache;         /* value of the cache word */
    uint32_t *cache_ptr;    /* pointer to the cache word in the buffer (or to dummy in init) */
    uint32_t *ff_start;     /* pointer to the start of the FF block */
    uint32_t ff_count;      /* number of FF words in the block */
    uint32_t *out_ptr;      /* current write position in the buffer */
    uint32_t *out_end;      /* end of buffer (for bounds checking) */
    uint32_t *buf_start;    /* start of buffer (for backward walk) */
} rc_enc_t;

typedef struct {
    uint64_t range;
    uint64_t code;
} rc_dec_t;

/* --- Encoder --- */

static inline __attribute__((always_inline))
void rc_enc_init(rc_enc_t *rc, uint32_t *buf, size_t buf_words) {
    rc->low = 0;
    rc->range = 0xFFFFFFFFFFFFFFFFULL;
    rc->cache = 0;
    rc->out_ptr = buf;
    rc->out_end = buf + buf_words;
    rc->buf_start = buf;
    /* Junk variable for cache_ptr until the first safe-emit.
        flush writes cache here (=0, since without renorm carry is impossible).
        static — one per whole program, does not pollute the struct. */
    static uint32_t rc_enc_cache_dummy = 0;
    rc->cache_ptr = &rc_enc_cache_dummy;
    rc->ff_start = NULL;
    rc->ff_count = 0;
}

static inline __attribute__((always_inline))
void rc_enc_step(rc_enc_t *rc, uint32_t cum_lo, uint32_t freq, uint32_t total) {
    uint64_t t = rc->range / total;
    uint64_t step = t * cum_lo;

    rc->low += step;
    rc->range = t * freq;

    /* CARRY: inplace patch */
    if (rc->low < step) {
        rc->cache++;
        if (rc->cache == 0) {
            /* Cache wrapped (0xFFFFFFFF → 0x00000000).
               Carry propagation: cache → 0, walk backward
               incrementing until a non-FF word is found. */
            *rc->cache_ptr = 0;
            uint32_t *p = rc->cache_ptr;
            while (p > rc->buf_start) {
                p--;
                if (*p != 0xFFFFFFFF) {
                    (*p)++;
                    break;
                }
                *p = 0;
            }
        } else {
            *rc->cache_ptr = rc->cache;
        }
        /* FF block → 0x00000000 with a single memset */
        if (rc->ff_count > 0) {
            memset(rc->ff_start, 0x00, (size_t)rc->ff_count * sizeof(uint32_t));
        }
        rc->ff_count = 0;
    }

    /* RENORM */
    if (rc->range < SCHINDLER_BOTTOM_64) {
        uint32_t out_dword = (uint32_t)(rc->low >> 32);

        if (out_dword == 0xFFFFFFFF) {
            /* STRADDLE: write FF immediately */
            if (rc->ff_count == 0) rc->ff_start = rc->out_ptr;
            *rc->out_ptr++ = 0xFFFFFFFF;
            rc->ff_count++;
        } else {
            /* SAFE EMIT: the new word becomes the cache */
            rc->cache = out_dword;
            rc->cache_ptr = rc->out_ptr;
            *rc->out_ptr++ = out_dword;
            rc->ff_count = 0;
        }

        rc->low <<= 32;
        rc->range <<= 32;
    }
}

static inline __attribute__((always_inline))
void rc_enc_flush(rc_enc_t *rc) {
    /* Final cache is already in the buffer at cache_ptr — update it */
    *rc->cache_ptr = rc->cache;
    /* FF is already in the buffer. Append low (2 words). */
    *rc->out_ptr++ = (uint32_t)(rc->low >> 32);
    *rc->out_ptr++ = (uint32_t)(rc->low & 0xFFFFFFFF);
}

/* Number of written words = rc->out_ptr - buf_start (counted by the caller) */

/* --- Decoder --- */

static inline __attribute__((always_inline))
int rc_dec_init(rc_dec_t *rd, const uint32_t *in_buf) {
    rd->range = 0xFFFFFFFFFFFFFFFFULL;
    rd->code = ((uint64_t)in_buf[0] << 32) | in_buf[1];
    return 2;
}

static inline __attribute__((always_inline))
int rc_dec_step(rc_dec_t *rd, uint32_t cum_lo, uint32_t freq,
                uint32_t total, const uint32_t *in_buf) {
    int read_words = 0;
    uint64_t t = rd->range / total;
    uint64_t step = t * cum_lo;
    rd->code -= step;
    rd->range = t * freq;
    if (rd->range < SCHINDLER_BOTTOM_64) {
        uint32_t new_dword = in_buf[read_words++];
        rd->code = (rd->code << 32) | new_dword;
        rd->range <<= 32;
    }
    return read_words;
}

static inline __attribute__((always_inline))
uint32_t rc_dec_get_cum(const rc_dec_t *rd, uint32_t total) {
    uint64_t t = rd->range / total;
    return (uint32_t)(rd->code / t);
}