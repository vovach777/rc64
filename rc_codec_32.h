/* =========================================================================
 * 32-BIT IN-PLACE CARRY RANGE CODER (16-bit word granularity)
 *
 * Это интеграция движка, проверенного на 13+ датасетах (bin/16/256-sym,
 * Zipf, AR(1), Laplace, текст, urandom и др.), в формат проекта rc64.
 *
 * Принцип сужения интервала на каждый символ:
 *     r      = range >> RC_TOTAL_BITS      // масштаб (деление заменено сдвигом)
 *     low   += r * cum_freq                // сдвиг нижней границы
 *     range  = r * freq                    // новая ширина
 *
 * Carry: low += r*cum_freq может переполнить 32 бита. Перенос «всплывает»
 * в уже выданные старшие слова. Мы патчим их прямо в выходном буфере:
 * ++out[pos-1]; если было 0xFFFF -> стало 0x0000, продолжаем влево (ripple
 * carry), пока перенос не погаснет.
 *
 * Renorm: пока range < RC_TOP (2^16) — старшие 16 бит low уже не изменятся
 * будущими сужениями, выдвигаем их в буфер и сдвигаем low и range на 16
 * влево.
 *
 * Инвариант после renorm: range >= RC_TOP  =>  r = range>>RC_TOTAL_BITS >=
 * >= 2^(16-12) = 16 > 0, поэтому деление в декодере и умножения корректны
 * (нет деления на 0).
 *
 * Параметры:
 *   RC_TOTAL_BITS = 12  (RC_TOTAL = 4096)  — частотная шкала
 *   RC_TOP_BITS   = 16  (RC_TOP   = 65536) — порог renorm
 *   Слово выхода  = uint16_t (16 бит)
 *   Запас точности range = RC_TOP_BITS - RC_TOTAL_BITS = 4 бита
 *
 * Алфавит = 256 байтов. Модель static order-0 (см. model_12.h).
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
    uint32_t  low;       /* нижняя граница интервала (32 бита)             */
    uint32_t  range;     /* ширина интервала, инвариант: range >= RC_TOP   */
    uint16_t *out;       /* выходной буфер слов                            */
    size_t    cap;       /* ёмкость буфера в словах                        */
    size_t    pos;       /* сколько слов записано (может расти за cap)      */
} rc32_enc_t;

typedef struct {
    uint32_t        code;   /* текущий прочитанный код                     */
    uint32_t        range;  /* ширина интервала                            */
    const uint16_t *in;     /* входной буфер слов                          */
    size_t          len;    /* длина входа в словах                        */
    size_t          pos;    /* сколько слов прочитано                      */
} rc32_dec_t;

/* ---------- ENCODER ---------- */

static inline void rc32_enc_init(rc32_enc_t *e, uint16_t *out, size_t cap) {
    e->low   = 0;
    e->range = 0xFFFFFFFFu;
    e->out   = out;
    e->cap   = cap;
    e->pos   = 0;
}

/* Распространение переноса +1 назад по уже записанным словам.
   Патчим только реально существующие слова (i < min(pos, cap)),
   чтобы не выйти за буфер при overflow. */
static inline void rc32_carry_patch(rc32_enc_t *e) {
    size_t i = (e->pos < e->cap) ? e->pos : e->cap;
    while (i != 0) {
        i--;
        /* ++out[i]; если переполнилось (0xFFFF -> 0x0000) — идём дальше */
        if (++e->out[i] != 0x0000u)
            return;            /* перенос погашен */
    }
    /* Сюда попадаем только при невалидных входных данных:
       freq==0, сумма freq>TOTAL, либо overflow буфера.
       В корректном потоке перенос за начало буфера невозможен. */
}

static inline void rc32_enc_step(rc32_enc_t *e, uint32_t cum_freq,
                                 uint32_t freq) {
    uint32_t r  = e->range >> RC32_TOTAL_BITS;
    uint32_t nl = e->low + r * cum_freq;

    /* детект 32-битного переполнения без 64 бит и без ветвления-исключения */
    if (nl < e->low)
        rc32_carry_patch(e);

    e->low   = nl;
    e->range = r * freq;

    /* renorm: выдвигаем старшее слово low сразу в буфер */
    while (e->range < RC32_TOP) {
        if (e->pos < e->cap)
            e->out[e->pos] = (uint16_t)(e->low >> 16);
        e->pos++;
        e->low   <<= 16;
        e->range <<= 16;
    }
}

static inline void rc32_enc_flush(rc32_enc_t *e) {
    /* вытолкнуть оставшиеся 32 бита low двумя словами */
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

/* читать одно слово (0 за концом потока) */
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
    /* первые 32 бита кода = два старших слова */
    d->code  = rc32_get(d) << 16;
    d->code |= rc32_get(d);
}

/* Сколько слов прочитано из входа после init + step-ов.
   Нужно вызывающему коду для диагностики. */
static inline size_t rc32_dec_pos(const rc32_dec_t *d) { return d->pos; }

static inline uint32_t rc32_dec_get_cum(const rc32_dec_t *d) {
    uint32_t r = d->range >> RC32_TOTAL_BITS;
    uint32_t v = d->code / r;                 /* единственное деление */
    /* clamp на случай попадания в «мёртвую зону» хвоста range */
    return (v >= RC32_TOTAL) ? (RC32_TOTAL - 1u) : v;
}

static inline void rc32_dec_step(rc32_dec_t *d, uint32_t cum_freq,
                                 uint32_t freq) {
    uint32_t r = d->range >> RC32_TOTAL_BITS;
    d->code  -= r * cum_freq;
    d->range  = r * freq;

    /* renorm — симметрично энкодеру */
    while (d->range < RC32_TOP) {
        d->code   = (d->code << 16) | rc32_get(d);
        d->range <<= 16;
    }
}

#endif /* RC_CODEC_32_H */
