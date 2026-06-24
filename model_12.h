/* =========================================================================
 * STATIC 12-BIT FREQUENCY MODEL (для 32-битного range coder'а)
 * =========================================================================
 *
 * Фиксированные кумулятивные частоты, total = 2^12 = 4096.
 * Модель строится ОДИН РАЗ по всему входному потоку, не обновляется
 * во время кодирования (адаптация не нужна).
 *
 * Кумулятивные частоты хранятся в uint16_t (12 бит хватает: 0..4096).
 *
 * МАСШТАБИРОВАНИЕ:
 *   raw_freq[i] * 4096 / total_raw -> scaled[i]
 *   Если raw_freq[i] > 0 но scaled[i] == 0: scaled[i] = 1 (вычесть из max).
 *   Сумма scaled приводится к 4096 корректировкой максимальной частоты.
 *
 * RLE:
 *   Если только один символ встречается (n_active <= 1) — range coding
 *   бессмысленен, используем RLE.
 *
 * Это модель для 32-битного движка (rc_codec_32.h). Для 64-битного
 * движка (rc_codec.h) остаётся model.h с 14-битной шкалой.
 * ========================================================================= */

#ifndef MODEL_12_H
#define MODEL_12_H

#include <stdint.h>
#include <string.h>

#define ALPHABET_12         256
#define TARGET_TOTAL_12     (1u << 12)   /* 4096 = 2^12 */

/* Кумулятивные частоты: cum[0]=0, cum[256]=total.
   cum_lo(sym) = cum[sym], freq(sym) = cum[sym+1] - cum[sym].
   Всё в uint16_t — 12 бит достаточно. */
typedef uint16_t cums12_t[ALPHABET_12 + 1];

/* Построить модель по сырым частотам.
   raw_freq[256] — счётчики байтов.
   Возвращает:
      1  — RLE (только один активный символ, в m[1] = first_sym)
      0  — OK (полная модель)
     -1  — пустой поток (n_active == 0)
     -2  — внутренняя ошибка (сумма не сошлась) */
static inline int model12_build(cums12_t m, const uint32_t raw_freq[ALPHABET_12]) {
    memset(m, 0, sizeof(cums12_t));
    int n_active = 0;
    uint64_t total_raw = 0;
    int  first_sym = 0;
    int  max_sym = 0;
    uint32_t max_val = 1;
    int i;

    for (i = 0; i < ALPHABET_12; i++) {
        if (raw_freq[i] > 0) {
            n_active++;
            if (n_active == 1) first_sym = i;
            total_raw += raw_freq[i];
            if (raw_freq[i] > max_val) {
                max_val = raw_freq[i];
                max_sym = i;
            }
        }
    }
    if (n_active == 0) {
        return -1;
    }

    /* RLE: один активный символ */
    if (n_active == 1) {
        m[0] = 0;
        m[1] = first_sym;   /* кодируем сам символ (не частоту) */
        return 1;
    }

    /* МАСШТАБИРОВАНИЕ к 12 битам */
    uint16_t scaled[ALPHABET_12];
    memset(scaled, 0, sizeof(scaled));
    uint32_t sum = 0;

    for (i = 0; i < ALPHABET_12; i++) {
        if (raw_freq[i] != 0) {
            /* uint32_t умножение для безопасного диапазона */
            uint32_t s = (uint32_t)(((uint64_t)raw_freq[i] * TARGET_TOTAL_12)
                                    / total_raw);
            if (s == 0) s = 1;     /* нельзя 0 — декодер сломается */
            scaled[i] = (uint16_t)s;
            sum      += s;
        }
    }

    /* Корректировка суммы к TARGET_TOTAL_12 через максимальную частоту */
    if (sum > TARGET_TOTAL_12) {
        /* Из-за bump'а нулей могли перебрать — режем максимум */
        uint32_t over = sum - TARGET_TOTAL_12;
        if (over <= scaled[max_sym]) {
            scaled[max_sym] = (uint16_t)(scaled[max_sym] - over);
        } else {
            /* Тяжёлый случай: bump'ов слишком много — разделим по всем */
            scaled[max_sym] = 1;
            over -= (uint16_t)(scaled[max_sym] - 1);
            int safety = 1000000;
            while (over > 0 && safety-- > 0) {
                int kmax = -1;
                for (i = 0; i < ALPHABET_12; i++) {
                    if (raw_freq[i] != 0 && scaled[i] > 1) {
                        if (kmax < 0 || scaled[i] > scaled[kmax]) kmax = i;
                    }
                }
                if (kmax < 0) break;
                scaled[kmax]--;
                over--;
            }
        }
    } else if (sum < TARGET_TOTAL_12) {
        scaled[max_sym] = (uint16_t)(scaled[max_sym] +
                                     (TARGET_TOTAL_12 - sum));
    }

    /* Сборка кумулятивной таблицы */
    m[0] = 0;
    for (i = 0; i < ALPHABET_12; i++) {
        m[i + 1] = (uint16_t)(m[i] + scaled[i]);
    }
    return (m[ALPHABET_12] == TARGET_TOTAL_12) ? 0 : -2;
}

/* Получить cum_lo и freq для символа sym. */
static inline void model12_get(const cums12_t m, uint8_t sym,
                               uint16_t *cum_lo, uint16_t *freq) {
    *cum_lo = m[sym];
    *freq   = (uint16_t)(m[sym + 1] - m[sym]);
}

/* Найти символ по кумулятивному значению cum (для декодера).
   8 шагов binary search без ветвлений (cmov). */
static inline uint8_t model12_find(const cums12_t m, uint16_t cum,
                                   uint16_t *cum_lo, uint16_t *freq) {
    const uint16_t *base = m;
    base += (cum >= base[128]) ? 128 : 0;
    base += (cum >= base[64])  ? 64  : 0;
    base += (cum >= base[32])  ? 32  : 0;
    base += (cum >= base[16])  ? 16  : 0;
    base += (cum >= base[8])   ? 8   : 0;
    base += (cum >= base[4])   ? 4   : 0;
    base += (cum >= base[2])   ? 2   : 0;
    base += (cum >= base[1])   ? 1   : 0;

    uint8_t s = (uint8_t)(base - m);
    *cum_lo = m[s];
    *freq   = (uint16_t)(m[s + 1] - m[s]);
    return s;
}

#endif /* MODEL_12_H */
