/* =========================================================================
 * SCHINDLER 64-BIT RANGE CODER — INPLACE VARIANT (v2)
 * =========================================================================
 *
 * Cache и FF пишутся сразу в выходной буфер. При carry — один memset
 * патчит FF блок в 0x00000000, cache++ inplace. Нет deferred FF,
 * нет циклов записи на safe emit.
 *
 * Структура содержит указатели на выходной буфер — память выделяется
 * один раз вызывающим кодом, перевыделения нет.
 * ========================================================================= */

#include <stdint.h>
#include <string.h>

#include "model.h"  /* TARGET_TOTAL = 2^14, передаётся как compile-time constant */

#define SCHINDLER_BOTTOM_64 0x0000000100000000ULL

typedef struct {
    uint64_t low;
    uint64_t range;
    uint32_t cache;         /* значение cache слова */
    uint32_t *cache_ptr;    /* указатель на cache слово в буфере */
    uint32_t *ff_start;     /* указатель на начало блока FF */
    uint32_t ff_count;      /* кол-во FF слов в блоке */
    uint32_t *out_ptr;      /* текущая позиция записи в буфере */
    uint32_t *out_end;      /* конец буфера (для проверки) */
    uint32_t *buf_start;    /* начало буфера (для backward walk) */
} rc_enc_t;

typedef struct {
    uint64_t range;
    uint64_t code;
} rc_dec_t;

/* --- Энкодер --- */

static inline __attribute__((always_inline))
void rc_enc_init(rc_enc_t *rc, uint32_t *buf, size_t buf_words) {
    rc->low = 0;
    rc->range = 0xFFFFFFFFFFFFFFFFULL;
    rc->cache = 0;
    rc->out_ptr = buf;
    rc->out_end = buf + buf_words;
    rc->buf_start = buf;
    /* Резервируем слово под cache — запишем в flush */
    rc->cache_ptr = rc->out_ptr;
    *rc->out_ptr++ = 0;
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
               Обратное распространение: cache → 0, идём назад
               инкрементируя пока не найдём не-FF. */
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
        /* FF блок → 0x00000000 одной memset */
        if (rc->ff_count > 0) {
            memset(rc->ff_start, 0x00, (size_t)rc->ff_count * sizeof(uint32_t));
        }
        rc->ff_count = 0;
    }

    /* RENORM */
    if (rc->range < SCHINDLER_BOTTOM_64) {
        uint32_t out_dword = (uint32_t)(rc->low >> 32);

        if (out_dword == 0xFFFFFFFF) {
            /* STRADDLE: пишем FF сразу */
            if (rc->ff_count == 0) rc->ff_start = rc->out_ptr;
            *rc->out_ptr++ = 0xFFFFFFFF;
            rc->ff_count++;
        } else {
            /* SAFE EMIT: новое слово становится cache */
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
    /* Финальный cache уже в буфере на cache_ptr — обновляем */
    *rc->cache_ptr = rc->cache;
    /* FF уже в буфере. Дописываем low (2 слова). */
    *rc->out_ptr++ = (uint32_t)(rc->low >> 32);
    *rc->out_ptr++ = (uint32_t)(rc->low & 0xFFFFFFFF);
}

/* Кол-во записанных слов = rc->out_ptr - buf_start (считает вызывающий) */

/* --- Декодер --- */

static inline __attribute__((always_inline))
void rc_dec_init(rc_dec_t *rd, const uint32_t *in_buf) {
    rd->range = 0xFFFFFFFFFFFFFFFFULL;
    rd->code = ((uint64_t)in_buf[1] << 32) | in_buf[2];
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
    if (t == 0) return 0;
    return (uint32_t)(rd->code / t);
}