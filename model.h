/* =========================================================================
 * STATIC 14-BIT FREQUENCY MODEL
 * =========================================================================
 *
 * Фиксированные кумулятивные частоты, total = 2^14 = 16384.
 * Модель строится ОДИН РАЗ по всему входному потоку, не обновляется
 * во время кодирования (адаптация не нужна).
 *
 * Кумулятивные частоты хранятся в uint16_t (14 бит хватает: 0..16384).
 *
 * МАСШТАБИРОВАНИЕ:
 *   raw_freq[i] * 16384 / total_raw -> scaled[i]
 *   Если raw_freq[i] > 0 но scaled[i] == 0: scaled[i] = 1 (вычесть из max).
 *   Сумма scaled приводится к 16384 корректировкой максимальной частоты.
 *
 * RLE:
 *   Если только один символ встречается (n_active <= 1) — range coding
 *   бессмысленен, используем RLE.
 * ========================================================================= */

#ifndef MODEL_H
#define MODEL_H

#include <stdint.h>
#include <string.h>

#define ALPHABET       256
#define TARGET_TOTAL   (1u << 14)   /* 16384 = 2^14 */

/* Кумулятивные частоты: cum[0]=0, cum[256]=total.
   cum_lo(sym) = cum[sym], freq(sym) = cum[sym+1] - cum[sym].
   Всё в uint16_t — 14 бит достаточно. */
typedef struct {
    uint16_t cum[ALPHABET + 1];
    uint32_t total;            /* всегда TARGET_TOTAL после build */
    int      is_rle;            /* 1 = RLE mode (один символ) */
    uint8_t  rle_sym;           /* символ для RLE */
} model_t;

/* Построить модель по сырым частотам.
   raw_freq[256] — счётчики байтов.
   n_active — сколько символов с raw_freq > 0.
   Если n_active <= 1: is_rle=1. */
static inline void model_build(model_t *m, const uint32_t raw_freq[ALPHABET]) {
    memset(m->cum, 0, sizeof(m->cum));
    m->total = TARGET_TOTAL;
    m->is_rle = 0;
    m->rle_sym = 0;

    /* Подсчёт активных символов и общего счёта */
    int n_active = 0;
    uint32_t total_raw = 0;
    uint8_t  first_sym = 0;
    for (int i = 0; i < ALPHABET; i++) {
        if (raw_freq[i] > 0) {
            n_active++;
            if (n_active == 1) first_sym = (uint8_t)i;
            total_raw += raw_freq[i];
        }
    }

    /* RLE: один или ноль активных символов */
    if (n_active <= 1) {
        m->is_rle = 1;
        m->rle_sym = first_sym;
        return;
    }

    /* МАСШТАБИРОВАНИЕ к 14 битам */
    int32_t scaled[ALPHABET];
    memset(scaled, 0, sizeof(scaled));
    int32_t sum = 0;
    int max_sym = -1;
    int32_t max_scaled = 0;

    for (int i = 0; i < ALPHABET; i++) {
        if (raw_freq[i] == 0) {
            scaled[i] = 0;
            continue;
        }
        /* Целочисленное масштабирование */
        scaled[i] = (int32_t)((uint64_t)raw_freq[i] * TARGET_TOTAL / total_raw);
        if (scaled[i] == 0) {
            /* Частота обратилась в ноль — вернуть единичку */
            scaled[i] = 1;
        }
        /* Отслеживаем максимум */
        if (scaled[i] > max_scaled) {
            max_scaled = scaled[i];
            max_sym = i;
        }
        sum += scaled[i];
    }

    /* Корректировка суммы к TARGET_TOTAL через максимальную частоту */
    int32_t diff = (int32_t)TARGET_TOTAL - sum;
    if (max_sym >= 0) {
        scaled[max_sym] += diff;
        /* Безопасность: частота не должна уйти в ноль или ниже */
        if (scaled[max_sym] < 1) scaled[max_sym] = 1;
    }

    /* Сборка кумулятивной таблицы (сброс в uint16_t) */
    m->cum[0] = 0;
    for (int i = 0; i < ALPHABET; i++) {
        m->cum[i + 1] = m->cum[i] + (uint16_t)scaled[i];
    }
    m->total = m->cum[ALPHABET];   /* должно быть TARGET_TOTAL */
}

/* Получить cum_lo и freq для символа sym. */
static inline void model_get(const model_t *m, uint8_t sym,
                             uint32_t *cum_lo, uint32_t *freq) {
    *cum_lo = m->cum[sym];
    *freq  = (uint32_t)m->cum[sym + 1] - m->cum[sym];
}

/* Найти символ по кумулятивному значению cum (для декодера).
   Возвращает символ, записывает cum_lo и freq. */
static inline uint8_t model_find(const model_t *m, uint32_t cum,
                                  uint32_t *cum_lo, uint32_t *freq) {
    const uint16_t* base = m->cum;
    // Компилятор (GCC/Clang) превратит тернарные операторы в инструкции
    // условной пересылки (MOVEQZ / MOVNEZ для Xtensa),
    // полностью исключая ветвления!
    /* 8 шагов (128..1) покрывают все 256 индексов: 128+64+32+16+8+4+2+1 = 255.
       base сдвигается, пока cum >= cum[base+k]; итоговый base указывает на
       cum[s], где s — наибольший индекс с cum[s] <= cum_value. */
    base += (cum >= base[128]) ? 128 : 0;
    base += (cum >= base[64]) ? 64 : 0;
    base += (cum >= base[32]) ? 32 : 0;
    base += (cum >= base[16]) ? 16 : 0;
    base += (cum >= base[8]) ? 8 : 0;
    base += (cum >= base[4]) ? 4 : 0;
    base += (cum >= base[2]) ? 2 : 0;
    base += (cum >= base[1]) ? 1 : 0;

    uint32_t s = base - m->cum;
    *cum_lo = m->cum[s];
    *freq  = (uint32_t)m->cum[s + 1] - m->cum[s];

    return (uint8_t)s;

}

#endif /* MODEL_H */