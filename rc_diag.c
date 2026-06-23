/* =========================================================================
 * RC DIAG — диагностический кодер со статистикой по веткам
 * =========================================================================
 *
 * Usage:
 *   rc_diag <input_file>
 *
 * Кодирует файл тем же алгоритмом что rc_encode, но НЕ пишет выход.
 * Вместо этого считает статистику по веткам кодера:
 *   - carry hits: сколько раз сработал перенос (low < step)
 *   - renorm hits: сколько раз range < BOTTOM (нужен сдвиг)
 *   - renorm miss: сколько раз сдвиг не понадобился
 *   - straddle hits: сколько раз out_dword == 0xFFFFFFFF
 *   - straddle max chain: максимальная длина цепочки зависших FF
 *   - flush carry: был ли перенос при финализации
 *
 * Также считает теоретическую энтропию order-0 и bpb.
 * ========================================================================= */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>

#include "model.h"
#include "timer.h"

#define SCHINDLER_BOTTOM_64 0x0000000100000000ULL

/* --- Состояние энкодера (копия из schindler_64_bit_automaton.c) --- */
typedef struct {
    uint64_t low;
    uint64_t range;
    uint32_t cache;
    uint32_t ff_count;
} diag_enc_t;

/* --- Счётчики --- */
typedef struct {
    uint64_t total_symbols;
    uint64_t carry_hits;       /* low += step; low < step */
    uint64_t carry_miss;       /* low += step; low >= step */
    uint64_t renorm_hits;      /* range < BOTTOM */
    uint64_t renorm_miss;      /* range >= BOTTOM */
    uint64_t straddle_hits;    /* out_dword == 0xFFFFFFFF */
    uint64_t straddle_flushed;  /* FF слова выведенные без переноса */
    uint64_t straddle_max_chain; /* макс ff_count за весь прогон */
    uint64_t flush_words;       /* слов выведенных при flush */
    uint64_t flush_carry;       /* был ли перенос в flush (cache++ из последнего step) */
} diag_stats_t;

static void diag_enc_init(diag_enc_t *rc) {
    rc->low = 0;
    rc->range = 0xFFFFFFFFFFFFFFFFULL;
    rc->cache = 0;
    rc->ff_count = 0;
}

/* Кодирование одного символа со счётчиками. out_buf не нужен — мы не пишем. */
static void diag_encode_step(diag_enc_t *rc, diag_stats_t *st,
                             uint32_t cum_lo, uint32_t freq, uint32_t total) {
    uint64_t t = rc->range / total;
    uint64_t step = t * cum_lo;

    rc->low += step;
    rc->range = t * freq;

    /* CARRY BRANCH */
    if (rc->low < step) {
        st->carry_hits++;
        rc->cache++;
        /* FF слова оборачиваются в 0 (мы их не пишем, только считаем) */
        st->straddle_flushed += rc->ff_count;
        if (rc->ff_count > st->straddle_max_chain)
            st->straddle_max_chain = rc->ff_count;
        rc->ff_count = 0;
    } else {
        st->carry_miss++;
    }

    /* RENORM BRANCH */
    if (rc->range < SCHINDLER_BOTTOM_64) {
        st->renorm_hits++;

        uint32_t out_dword = (uint32_t)(rc->low >> 32);

        if (out_dword == 0xFFFFFFFF) {
            /* STRADDLE */
            st->straddle_hits++;
            rc->ff_count++;
            if (rc->ff_count > st->straddle_max_chain)
                st->straddle_max_chain = rc->ff_count;
        } else {
            /* безопасный вывод (cache + FF) — не пишем, считаем */
            st->straddle_flushed += rc->ff_count;
            if (rc->ff_count > st->straddle_max_chain)
                st->straddle_max_chain = rc->ff_count;
            rc->ff_count = 0;
            rc->cache = out_dword;
        }

        rc->low <<= 32;
        rc->range <<= 32;
    } else {
        st->renorm_miss++;
    }
}

static void diag_enc_flush(diag_enc_t *rc, diag_stats_t *st) {
    /* cache + ff_count + 2 слова low */
    st->flush_words = 1 + rc->ff_count + 2;
    /* Проверка: был ли перенос в последнем step (cache инкрементирован) */
    /* Мы не можем точно знать, но если ff_count > 0 при flush,
       значит цепочка не разрешилась переносом */
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }

    /* --- Чтение входного файла --- */
    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("fopen input"); return 1; }
    fseek(fin, 0, SEEK_END);
    long fsize = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    if (fsize < 0) { fprintf(stderr, "ftell failed\n"); fclose(fin); return 1; }
    size_t n = (size_t)fsize;
    uint8_t *data = (uint8_t *)malloc(n > 0 ? n : 1);
    if (!data) { fprintf(stderr, "malloc %zu failed\n", n); fclose(fin); return 1; }
    if (n > 0 && fread(data, 1, n, fin) != n) {
        fprintf(stderr, "fread failed\n"); free(data); fclose(fin); return 1;
    }
    fclose(fin);

    /* --- Подсчёт частот --- */
    uint32_t raw_freq[ALPHABET];
    memset(raw_freq, 0, sizeof(raw_freq));
    for (size_t i = 0; i < n; i++) raw_freq[data[i]]++;

    /* --- Энтропия order-0 --- */
    double entropy = 0.0;
    for (int i = 0; i < ALPHABET; i++) {
        if (raw_freq[i] > 0) {
            double p = (double)raw_freq[i] / (double)n;
            entropy += -p * log2(p);
        }
    }
    double theory_bpb = entropy;  /* bits per byte */

    /* --- Построение модели --- */
    cums_t m;
    int res = model_build(m, raw_freq);
    if (res < 0) {
        fprintf(stderr, "model_build failed: %d\n", res); free(data); return 1;
    }

    /* --- Кодирование со счётчиками --- */
    diag_enc_t rc;
    diag_enc_init(&rc);
    diag_stats_t st;
    memset(&st, 0, sizeof(st));
    st.total_symbols = n;

    int64_t tfreq = timer_freq();
    int64_t t0 = timer_ticks();

    if (res) {
        /* RLE: кодер не работает, выход = 12 байт */
        printf("=== RC DIAG: %s ===\n", argv[1]);
        printf("\n");
        printf("RLE mode (single symbol 0x%02X)\n", m[1]);
        printf("  input:       %zu bytes\n", n);
        printf("  output:      12 bytes (header only)\n");
        printf("  ratio:       %.6f%%\n", n > 0 ? 12.0 / (double)n * 100.0 : 0.0);
        printf("  theory bpb:  %.6f (entropy=0, single symbol)\n", 0.0);
        printf("  carry hits:  0 (RLE, no encoding)\n");
        free(data);
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        uint16_t cum_lo, freq;
        model_get(m, data[i], &cum_lo, &freq);
        diag_encode_step(&rc, &st, cum_lo, freq, TARGET_TOTAL);
    }
    diag_enc_flush(&rc, &st);

    int64_t t1 = timer_ticks();
    double elapsed_ms = (double)(t1 - t0) * 1000.0 / (double)tfreq;

    /* --- Оценка выходного размера --- */
    /* header: 4 + 8 = 12. freq table: 512. total = 524.
       Поток слов: считаем из статы.
       carry_hits * (ff_count_at_time) + renorm_hits * (1 + ff_at_time) ...
       Проще: выведенных слов = (renorm_hits - straddle текущих) + flush_words.
       Но точный подсчёт сложен. Оценим по факту: */
    /* На каждый renorm_hit без straddle: 1 cache + ff_count_на_тот_момент слов.
       На straddle: 0 слов (только ff_count++).
       На carry: ff_count слов (оборачиваются в 0) + 1 cache (при следующем выводе).
       Итого: stream_words = renorm_hits - straddle_hits + straddle_flushed + flush_words.
       Нет, точнее: каждый renorm hit выводит 1 (cache) + ff_count (если не straddle).
       straddle не выводит ничего.
       carry выводит ff_count (в нули).
       Упрощаем: stream_words ≈ straddle_flushed + carry_hits + (renorm_hits - straddle_hits) + flush_words */
    /* Точнее не считаем — просто оценка для диагностики. */
    uint64_t stream_words_approx = st.renorm_hits - st.straddle_hits + st.straddle_flushed + st.flush_words;
    uint64_t out_bytes_approx = 12 + 512 + stream_words_approx * 4;
    double approx_bpb = (n > 0) ? (double)out_bytes_approx * 8.0 / (double)n : 0.0;
    double approx_ratio = (n > 0) ? (double)out_bytes_approx / (double)n * 100.0 : 0.0;

    /* --- Отчёт --- */
    printf("=== RC DIAG: %s ===\n", argv[1]);
    printf("\n");
    printf("--- ВХОД ---\n");
    printf("  file:          %s\n", argv[1]);
    printf("  size:          %zu bytes (%.1f MB)\n", n, (double)n / (1048576.0));
    printf("  symbols:       %zu\n", st.total_symbols);
    printf("  active bytes:  ");
    int active = 0;
    for (int i = 0; i < ALPHABET; i++) if (raw_freq[i] > 0) active++;
    printf("%d / 256\n", active);
    printf("\n");
    printf("--- МОДЕЛЬ ---\n");
    printf("  total:         %u (2^%.0f)\n", TARGET_TOTAL, log2((double)TARGET_TOTAL));
    printf("  entropy H:     %.6f bits/symbol\n", entropy);
    printf("  theory bpb:    %.6f\n", theory_bpb);
    printf("  RLE:           %s\n", !!m[0] ? "yes" : "no");
    printf("\n");
    printf("--- ВЕТКИ КОДЕРА ---\n");
    printf("  CARRY (low < step):\n");
    printf("    hit:          %llu  (%.4f%% of symbols)\n",
           (unsigned long long)st.carry_hits,
           st.total_symbols > 0 ? (double)st.carry_hits / (double)st.total_symbols * 100.0 : 0.0);
    printf("    miss:         %llu  (%.4f%%)\n",
           (unsigned long long)st.carry_miss,
           st.total_symbols > 0 ? (double)st.carry_miss / (double)st.total_symbols * 100.0 : 0.0);
    printf("\n");
    printf("  RENORM (range < 2^32):\n");
    printf("    hit:          %llu  (%.4f%%)\n",
           (unsigned long long)st.renorm_hits,
           st.total_symbols > 0 ? (double)st.renorm_hits / (double)st.total_symbols * 100.0 : 0.0);
    printf("    miss:         %llu  (%.4f%%)\n",
           (unsigned long long)st.renorm_miss,
           st.total_symbols > 0 ? (double)st.renorm_miss / (double)st.total_symbols * 100.0 : 0.0);
    printf("\n");
    printf("  STRADDLE (out_dword == 0xFFFFFFFF):\n");
    printf("    hits:         %llu  (%.4f%% of renorm hits)\n",
           (unsigned long long)st.straddle_hits,
           st.renorm_hits > 0 ? (double)st.straddle_hits / (double)st.renorm_hits * 100.0 : 0.0);
    printf("    flushed FF:   %llu  (FF-слова выведенные в поток)\n",
           (unsigned long long)st.straddle_flushed);
    printf("    max chain:    %llu  (самая длинная цепочка зависших FF)\n",
           (unsigned long long)st.straddle_max_chain);
    printf("\n");
    printf("--- FLUSH ---\n");
    printf("  flush words:    %llu  (cache + FF + low_hi + low_lo)\n",
           (unsigned long long)st.flush_words);
    printf("\n");
    printf("--- ОЦЕНКА ВЫХОДА ---\n");
    printf("  approx stream words: %llu\n", (unsigned long long)stream_words_approx);
    printf("  approx output:  %llu bytes (%.1f MB)\n",
           (unsigned long long)out_bytes_approx, (double)out_bytes_approx / 1048576.0);
    printf("  approx ratio:   %.4f%%\n", approx_ratio);
    printf("  approx bpb:     %.6f\n", approx_bpb);
    printf("  overhead vs H:  %.4f%%\n",
           theory_bpb > 0 ? (approx_bpb / theory_bpb - 1.0) * 100.0 : 0.0);
    printf("\n");
    printf("--- ВРЕМЯ ---\n");
    printf("  elapsed:        %.2f ms (%.1f MB/s)\n",
           elapsed_ms, n > 0 ? (double)n / (elapsed_ms * 1048576.0 / 1000.0) : 0.0);

    free(data);
    return 0;
}