/* =========================================================================
 * FOLK CODEC — 64-битный micro-Subbotin (16-битный сдвиг, aligned trim)
 * =========================================================================
 *
 * 64-битные low/range, 16-битный сдвиг, BOTTOM = 2^48.
 * Subbotin aligned trim: range = (-low) & 0xFFFF.
 *
 * ИДЕЯ:
 *   64-битный low: 48 бит точности сверху + 16 снизу.
 *   16-битный сдвиг: выводим top 16 бит как uint16.
 *   Trim к 2^16-блоку: range = (-low) & 0xFFFF.
 *   Обрезка убирает только нижние 16 бит — минимальная потеря.
 *   range после trim >= 1, shift=16 поднимает до 2^16 >= BOTTOM? Нет:
 *   BOTTOM = 2^48, shift=16 поднимает range на 2^16, нужен renorm цикл.
 *
 * Сравнение с 32-битным сдвигом:
 *   32-bit shift, naive trim:  +1.21% overhead (clip к 2^64)
 *   16-bit shift, Subbotin trim: ? (clip к 2^16, меньше потеря)
 *
 * Вывод: uint16 слова (big-endian).
 * ========================================================================= */

#include <stdint.h>
#include <string.h>

#define FOLK_BOTTOM  (1ULL << 32)    /* 2^32 — безопасно для t*total */
#define FOLK_MASK16  0xFFFFULL       /* 2^16 - 1 */

typedef struct {
    uint64_t low;
    uint64_t range;
} folk_enc_t;

typedef struct {
    uint64_t low;
    uint64_t range;
    uint64_t code;
} folk_dec_t;

static inline __attribute__((always_inline))
void folk_enc_init(folk_enc_t *rc) {
    rc->low = 0;
    rc->range = 0xFFFFFFFFFFFFFFFFULL;
}

/* Декодер: читаем 4 слова (8 байт) в code (64 бита) */
static inline __attribute__((always_inline))
void folk_dec_init(folk_dec_t *rd, const uint16_t *in_buf) {
    rd->low = 0;
    rd->range = 0xFFFFFFFFFFFFFFFFULL;
    rd->code = ((uint64_t)in_buf[0] << 48) | ((uint64_t)in_buf[1] << 32) |
               ((uint64_t)in_buf[2] << 16) | (uint64_t)in_buf[3];
}

/* Энкодер: renorm с Subbotin aligned trim (16-битный) */
static inline __attribute__((always_inline))
int folk_enc_renorm(folk_enc_t *rc, uint16_t *out) {
    int n = 0;
    while (rc->range < FOLK_BOTTOM) {
        /* Выводим top 16 бит */
        out[n++] = (uint16_t)(rc->low >> 48);
        rc->low <<= 16;
        rc->range <<= 16;
        /* SUBBOTIN ALIGNED TRIM:
           range = (-low) & 0xFFFF — расстояние от low до границы 2^16.
           Интервал [low, low+range] остаётся в одном 2^16-блоке.
           Никакого "плывания" — математически точная траектория. */
        if (rc->low + rc->range < rc->low) {
            rc->range = (0ULL - rc->low) & FOLK_MASK16;
            if (rc->range == 0) rc->range = FOLK_MASK16;
        }
    }
    return n;
}

static inline __attribute__((always_inline))
void folk_enc_step(folk_enc_t *rc, uint16_t *out, int *nout,
                   uint32_t cum_lo, uint32_t freq, uint32_t total) {
    uint64_t t = rc->range / total;
    rc->low += t * cum_lo;
    rc->range = t * freq;
    *nout = folk_enc_renorm(rc, out);
}

static inline __attribute__((always_inline))
int folk_enc_flush(folk_enc_t *rc, uint16_t *out) {
    /* Выводим 4 слова (64 бита low) */
    out[0] = (uint16_t)(rc->low >> 48);
    out[1] = (uint16_t)(rc->low >> 32);
    out[2] = (uint16_t)(rc->low >> 16);
    out[3] = (uint16_t)(rc->low);
    return 4;
}

/* Декодер: renorm с Subbotin aligned trim (зеркально) */
static inline __attribute__((always_inline))
int folk_dec_renorm(folk_dec_t *rd, const uint16_t *in_buf) {
    int n = 0;
    while (rd->range < FOLK_BOTTOM) {
        rd->range <<= 16;
        rd->low <<= 16;
        rd->code = (rd->code << 16) | in_buf[n++];
        if (rd->low + rd->range < rd->low) {
            rd->range = (0ULL - rd->low) & FOLK_MASK16;
            if (rd->range == 0) rd->range = FOLK_MASK16;
        }
    }
    return n;
}

static inline __attribute__((always_inline))
int folk_dec_step(folk_dec_t *rd, const uint16_t *in_buf,
                   uint32_t cum_lo, uint32_t freq, uint32_t total) {
    uint64_t t = rd->range / total;
    rd->low += t * cum_lo;
    rd->range = t * freq;
    return folk_dec_renorm(rd, in_buf);
}

static inline __attribute__((always_inline))
uint32_t folk_dec_get_cum(const folk_dec_t *rd, uint32_t total) {
    uint64_t t = rd->range / total;
    if (t == 0) return 0;
    return (uint32_t)((rd->code - rd->low) / t);
}