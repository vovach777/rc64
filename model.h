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
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))

#define ALPHABET       256
#define TARGET_TOTAL   (1u << 14)   /* 16384 = 2^14 */

/* Кумулятивные частоты: cum[0]=0, cum[256]=total.
   cum_lo(sym) = cum[sym], freq(sym) = cum[sym+1] - cum[sym].
   Всё в uint16_t — 14 бит достаточно. */
typedef  uint16_t cums_t[ALPHABET + 1];

/* Построить модель по сырым частотам.
   raw_freq[256] — счётчики байтов.
   n_active — сколько символов с raw_freq > 0.
   Если n_active <= 1: is_rle=1. */
static inline int model_build(cums_t m, const uint32_t raw_freq[ALPHABET]) {
    memset(m, 0, sizeof(cums_t));
    /* Подсчёт активных символов и общего счёта */
    int n_active = 0;
    uint64_t total_raw = 0;
    int  first_sym = 0;
    int  max_sym = 0;
    uint32_t  max_val = 1;
    for (int i = 0; i < ALPHABET; i++) {
        if (raw_freq[i] > 0) {
            n_active++;
            if (n_active == 1) first_sym = (uint8_t)i;
            total_raw += raw_freq[i];
            if (raw_freq[i] > max_val) {
                max_val = raw_freq[i];
                max_sym = i;
            }
        }
    }
    if (n_active ==0) {
        return -1;
    }


    /* RLE: один или ноль активных символов */
    if (n_active == 1) {
        m[0] = 1;
        m[1] = first_sym;
        return 1;
    }

    /* МАСШТАБИРОВАНИЕ к 14 битам */
    int16_t scaled[ALPHABET];
    memset(scaled, 0, sizeof(scaled));
    int16_t sum = 0;


    for (int i = 0; i < ALPHABET; i++) {
        if (raw_freq[i] != 0) {
            scaled[i] = (uint16_t)( raw_freq[i] * (float)TARGET_TOTAL * (1.0f / (float)total_raw) );
            scaled[i] += scaled[i] == 0;
            sum += scaled[i];
        }
    }

    /* Корректировка суммы к TARGET_TOTAL через максимальную частоту */
    scaled[max_sym] += TARGET_TOTAL - sum;

    /* Сборка кумулятивной таблицы (сброс в uint16_t) */
    m[0] = 0;
    for (int i = 0; i < ALPHABET; i++) {
        m[i + 1] = m[i] + (uint16_t)scaled[i];
    }
    /* total = cum[ALPHABET], хранится неявно */
    return (m[ALPHABET] == TARGET_TOTAL) ? 0 : -2;

}

/* Получить cum_lo и freq для символа sym. */
static inline void model_get(const cums_t m, uint8_t sym,
                             uint16_t *cum_lo, uint16_t *freq) {
    *cum_lo = m[sym];
    *freq  = m[sym + 1] - m[sym];
}

/* Найти символ по кумулятивному значению cum (для декодера).
   Возвращает символ, записывает cum_lo и freq. */
static inline uint8_t model_find(const cums_t m, uint16_t cum,
                                  uint16_t *cum_lo, uint16_t *freq) {
    const uint16_t* base = m;
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

    uint8_t s = base - m;
    *cum_lo = m[s];
    *freq  = (uint16_t)m[s + 1] - m[s];

    return (uint8_t)s;

}

#endif /* MODEL_H */