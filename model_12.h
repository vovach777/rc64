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
 * Поиск символа по cum-значению:
 *   - По умолчанию: прямой LUT uint8_t[TARGET_TOTAL_12+1] (4097 байт).
 *     O(1), одна cache line load. Идеально для декодера.
 *   - Если определён DISABLE_LUT: 8-шаговый binary search без ветвлений
 *     (старая реализация, 8 dependent loads из cums[257]).
 *
 * МАСШТАБИРОВАНИЕ:
 *   raw_freq[i] * 4096 / total_raw -> scaled[i]
 *   Если raw_freq[i] > 0 но scaled[i] == 0: scaled[i] = 1 (вычесть из max).
 *   Сумма scaled приводится к 4096 корректировкой максимальной частоты.
 *
 * RLE:
 *   Если только один символ встречается (n_active <= 1) — range coding
 *   бессмысленен, используем RLE.
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

/* LUT: lut[cum] -> символ s, наибольший с cum[s] <= cum.
   Размер = TARGET_TOTAL_12 + 1 = 4097 байт. */
typedef uint8_t  lut12_t[TARGET_TOTAL_12 + 1];

/* Полная модель: cums + опциональный LUT.
   LUT строится только если не определён DISABLE_LUT.
   Если LUT отключён — поле lut остаётся неиспользуемым (zero-sized через
   условную компиляцию здесь не делаем — структура всегда одинаковая,
   чтобы ABI был стабильным; экономия 4 KB памяти несущественна). */
typedef struct {
    cums12_t cums;
    lut12_t  lut;
} model12_t;

/* Построить модель по сырым частотам.
   raw_freq[256] — счётчики байтов.
   Возвращает:
      1  — RLE (только один активный символ, в cums[1] = first_sym)
      0  — OK (полная модель, LUT построен)
     -1  — пустой поток (n_active == 0)
     -2  — внутренняя ошибка (сумма не сошлась) */
static inline int model12_build(model12_t *M,
                                const uint32_t raw_freq[ALPHABET_12]) {
    cums12_t m;
    memset(m, 0, sizeof(cums12_t));
    memset(M->cums, 0, sizeof(cums12_t));
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
        m[1] = (uint16_t)first_sym;
        memcpy(M->cums, m, sizeof(cums12_t));
        return 1;
    }

    /* МАСШТАБИРОВАНИЕ к 12 битам */
    uint16_t scaled[ALPHABET_12];
    memset(scaled, 0, sizeof(scaled));
    uint32_t sum = 0;

    for (i = 0; i < ALPHABET_12; i++) {
        if (raw_freq[i] != 0) {
            uint32_t s = (uint32_t)(((uint64_t)raw_freq[i] * TARGET_TOTAL_12)
                                    / total_raw);
            if (s == 0) s = 1;
            scaled[i] = (uint16_t)s;
            sum      += s;
        }
    }

    if (sum > TARGET_TOTAL_12) {
        uint32_t over = sum - TARGET_TOTAL_12;
        if (over <= scaled[max_sym]) {
            scaled[max_sym] = (uint16_t)(scaled[max_sym] - over);
        } else {
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
    if (m[ALPHABET_12] != TARGET_TOTAL_12) {
        memcpy(M->cums, m, sizeof(cums12_t));
        return -2;
    }
    memcpy(M->cums, m, sizeof(cums12_t));

#ifndef DISABLE_LUT
    /* Построение LUT: для каждого cum-значения v найти наибольший символ s
       с cum[s] <= v. Делаем одним проходом: идём по v от 0 до TARGET_TOTAL,
       продвигаясь по s пока cum[s+1] <= v. */
    {
        int s = 0;
        unsigned v;
        for (v = 0; v <= TARGET_TOTAL_12; v++) {
            while (s < ALPHABET_12 - 1 && (int)m[s + 1] <= (int)v) s++;
            M->lut[v] = (uint8_t)s;
        }
    }
#else
    memset(M->lut, 0, sizeof(lut12_t));
#endif

    return 0;
}

/* Получить cum_lo и freq для символа sym. */
static inline void model12_get(const model12_t *M, uint8_t sym,
                               uint16_t *cum_lo, uint16_t *freq) {
    *cum_lo = M->cums[sym];
    *freq   = (uint16_t)(M->cums[sym + 1] - M->cums[sym]);
}

/* Найти символ по кумулятивному значению cum (для декодера). */
static inline uint8_t model12_find(const model12_t *M, uint16_t cum,
                                   uint16_t *cum_lo, uint16_t *freq) {
    uint8_t s;
#ifdef DISABLE_LUT
    /* 8-шаговый binary search без ветвлений (cmov). */
    const uint16_t *m = M->cums;
    const uint16_t *base = m;
    base += (cum >= base[128]) ? 128 : 0;
    base += (cum >= base[64])  ? 64  : 0;
    base += (cum >= base[32])  ? 32  : 0;
    base += (cum >= base[16])  ? 16  : 0;
    base += (cum >= base[8])   ? 8   : 0;
    base += (cum >= base[4])   ? 4   : 0;
    base += (cum >= base[2])   ? 2   : 0;
    base += (cum >= base[1])   ? 1   : 0;
    s = (uint8_t)(base - m);
#else
    /* Прямой LUT: одна cache line load. */
    s = M->lut[cum];
#endif
    *cum_lo = M->cums[s];
    *freq   = (uint16_t)(M->cums[s + 1] - M->cums[s]);
    return s;
}

#endif /* MODEL_12_H */
