/* =========================================================================
 * rc24_reserve.h -- ADDITIVE variant for rc64.
 *
 * Reserve-ahead shared buffer, ported from llcomp/rc (llcomp_rc.hpp:222-249).
 * Nothing here touches rc24_codec.h / rc24_2way_* / any existing target.
 *
 * Difference from the existing fixed layout shared[k*N+s]:
 *   - slots are handed out by rsv_take() in GLOBAL EVENT ORDER, not by a
 *     data-independent (k,s) formula;
 *   - encoder init() reserves RSV_PENDING slots per stream but does NOT fill
 *     them; the delayed-carry FIFO fills them later (flush() writes the tail);
 *   - decoder reads the whole shared buffer with ONE nx pointer, in exactly
 *     the same global order, so no per-stream sizes / offsets / max_len are
 *     stored in the file;
 *   - buffer length is exactly B.next (no max_stream_len * N padding).
 *
 * Numerical core is llcomp/rc verbatim: 64-bit low, 32-bit range, TOP=2^24,
 * 4 pending bytes, hybrid carry with the rare Subbotin trim. The rc64 12-bit
 * static model is scaled by 8 into a 2^15 CDF so no constant changes.
 * ========================================================================= */
#ifndef RC24_RESERVE_H
#define RC24_RESERVE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RSV_TOP      (1u << 24)
#define RSV_SHIFT    15                    /* llcomp CDF total = 2^15 */
#define RSV_TOTAL    (1u << RSV_SHIFT)
#define RSV_PENDING  4                     /* delayed-carry bytes */
#define RSV_MAXN     8

/* ---- shared buffer with dynamic slot reservation (Buf::take) ---- */
typedef struct {
    uint8_t *b;
    size_t   next, cap;
    uint64_t takes;      /* number of take() calls (diagnostics) */
    uint64_t cuts;       /* number of effective range trims (diagnostics) */
} rsv_buf_t;

static inline void rsv_buf_init(rsv_buf_t *B, size_t cap) {
    B->cap = cap ? cap : 64;
    B->next = 0; B->takes = 0; B->cuts = 0;
    B->b = (uint8_t *)calloc(B->cap, 1);
    if (!B->b) abort();
}
static inline void rsv_buf_free(rsv_buf_t *B) {
    free(B->b); B->b = 0; B->next = 0; B->cap = 0;
}
static inline size_t rsv_take(rsv_buf_t *B, size_t n) {
    size_t s = B->next;
    B->next += n; B->takes++;
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

/* ---- encoder: 4 pending bytes, reserved slots, flush back-fill ---- */
typedef struct {
    uint64_t low;
    uint32_t range;
    int      p;
    uint32_t fifo[16];
    uint32_t head, tail;
    uint64_t renorms, cuts;
} rsv_enc_t;

static inline void rsv_enc_init(rsv_enc_t *E, rsv_buf_t *B) {
    E->low = 0; E->range = 0xFFFFFFFFu; E->p = 0;
    E->head = 0; E->tail = 0; E->renorms = 0; E->cuts = 0;
    for (int i = 0; i < RSV_PENDING; ++i)
        E->fifo[E->tail++ & 15] = (uint32_t)rsv_take(B, 1);
}
static inline void rsv_enc_put(rsv_enc_t *E, rsv_buf_t *B, uint32_t c, uint32_t f) {
    uint32_t r = E->range >> RSV_SHIFT;
    E->low  += (uint64_t)c * r;
    E->range = f * r;
    while (E->range < RSV_TOP) {
        if (E->p == RSV_PENDING)
            B->b[E->fifo[E->head++ & 15]] = (uint8_t)(E->low >> 56);
        else
            ++E->p;
        E->fifo[E->tail++ & 15] = (uint32_t)rsv_take(B, 1);
        E->low <<= 8; E->range <<= 8;
        ++E->renorms;
        if ((uint32_t)(E->low >> 32) == 0xFFFFFFFFu) {
            uint64_t lim = (1ull << 32) - (uint32_t)E->low;
            if (E->range > lim) {
                E->range = (uint32_t)lim;
                ++E->cuts; ++B->cuts;
            }
        }
    }
}
static inline void rsv_enc_flush(rsv_enc_t *E, rsv_buf_t *B) {
    uint64_t l = E->low << (8 * (RSV_PENDING - E->p));
    for (int i = 0; i < E->p + RSV_PENDING; ++i) {
        B->b[E->fifo[E->head++ & 15]] = (uint8_t)(l >> 56);
        l <<= 8;
    }
}

/* ---- decoder: one nx pointer for all streams ---- */
typedef struct {
    uint64_t low;
    uint32_t range, code;
    uint64_t renorms, cuts;
} rsv_dec_t;

static inline void rsv_dec_init(rsv_dec_t *D, const uint8_t *b, uint32_t *nx) {
    D->low = 0; D->range = 0xFFFFFFFFu; D->code = 0;
    D->renorms = 0; D->cuts = 0;
    for (int i = 0; i < RSV_PENDING; ++i)
        D->code = (D->code << 8) | b[(*nx)++];
}
static inline void rsv_dec_step(rsv_dec_t *D, uint32_t c, uint32_t f, uint32_t r,
                                const uint8_t *b, uint32_t *nx) {
    D->code -= c * r;
    D->low  += (uint64_t)c * r;
    D->range = f * r;
    while (D->range < RSV_TOP) {
        D->code = (D->code << 8) | b[(*nx)++];
        D->low <<= 8; D->range <<= 8;
        ++D->renorms;
        if ((uint32_t)(D->low >> 32) == 0xFFFFFFFFu) {
            uint64_t lim = (1ull << 32) - (uint32_t)D->low;
            if (D->range > lim) {
                D->range = (uint32_t)lim;
                ++D->cuts;
            }
        }
    }
}
#endif /* RC24_RESERVE_H */
