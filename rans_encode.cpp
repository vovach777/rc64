/* =========================================================================
 * RANS ENCODE — 64-bit rANS, 14-битная статическая модель
 * =========================================================================
 *
 * Интеграция rANS-кодека пользователя (см. rans_codec.h) в формат проекта rc64.
 *
 * Usage:
 *   rans_encode <input_file> <output_file.rans>
 *
 * Формат .rans:
 *   [4 байта]  сигнатура: 'r','n', flags, rle_sym
 *              flags bit 0: is_rle
 *   [8 байт]   uint64_t original_len (LE)
 *   --- RLE mode (is_rle=1) ---
 *              (больше ничего — rle_sym в сигнатуре)
 *   --- rANS mode (is_rle=0) ---
 *   [512 байт] cum[1..256] — uint16_t LE (кумулятивные частоты, 14-бит)
 *              cum[0]=0 не хранится (всегда константа)
 *   далее подряд блоки (фиксированный размер ВХОДА BLOCK_BYTES):
 *     [4 байта]  uint32_t renorm_count — число renorm-слов блока (LE)
 *     [4 байта]  uint32_t flush_lo  = LOW32  финального state блока (LE)
 *     [4 байта]  uint32_t flush_hi  = HIGH32 финального state блока (LE)
 *     [4*K]      uint32_t renorm-слова блока, ПЕРЕВЁРНУТЫЕ (в порядке потребления
 *                декодером) — декодер читает их ВПЕРЁД
 *
 * Заголовок: 4 + 8 + 512 = 524 байта (как у .rc / .rc32).
 *
 * Блочная схема (облегчаем декодер — весь реверс на энкодере):
 *   - Каждый блок = НЕЗАВИСИМЫЙ rANS: свой state = RANS64_L в начале, своя
 *     flush-пара в конце. Между блоками состояние не переносится.
 *   - Проход по блокам ПРЯМОЙ (блок 0 = начало входа, блок 1 = следующий срез,
 *     ...). Внутри блока обход ВХОДА НАЗАД (rANS LIFO): с a_hi-1 вниз до a_lo.
 *   - renorm-слова блока пишутся в буфер С КОНЦА назад (buf[--buf_head]) —
 *     тогда они сразу в порядке потребления декодером (последняя эмиссия
 *     первой), отдельный переворот буфера не нужен. Запас (+1024) при этом
 *     остаётся СПЕРЕДИ нетронутым и гарантирует, что buf_head не уйдёт в минус.
 *   - Размер блока по ВХОДУ фиксирован (BLOCK_BYTES), поэтому границы известны
 *     заранее — ОДИН проход, без pre-pass и без seek. На огромных файлах
 *     экономим память: буфер ~450 КБ (потолок renorm на блок) вместо ~N.
 * ========================================================================= */

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <inttypes.h>

#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"

#include "rans_codec.h"
#include "model.h"

#define RANS_SCALE_BITS  14u   /* совпадает с TARGET_TOTAL = 2^14 из model.h */
#define BLOCK_BYTES      (256 * 1024)   /* фиксированный размер блока ВХОДА */

/* Потолок renorm-слов на блок (доказано и подтверждено эмпирически):
   renorm_count = (31 + n·H_cross − log2(state_final))/32, где
   H_cross — кросс-энтропия (модель q vs данные p), H_cross ≤ 14 bpb
   (макс. code length при freq=1), state_final ≥ RANS64_L (инвариант).
   => renorm_count ≤ BLOCK_BYTES*14/32 = 114688 (достигается на adversarial
   с freq=1). +1024 запас. Буфер ~450 КБ — влезает в 512 КБ L2. */
#define RENORM_BUF_WORDS (BLOCK_BYTES * 14u / 32u + 1024)

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input_file> <output_file.rans>\n", argv[0]);
        return 1;
    }

    /* --- Чтение входного файла (вне замера) --- */
    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) {
        fprintf(stderr, "fopen input\n");
        return 1;
    }

    zpl_i64 fsize = zpl_file_size(&fin);
    if (fsize <= 0) {
        fprintf(stderr, "file_size failed\n");
        zpl_file_close(&fin);
        return 1;
    }
    uint64_t n = (uint64_t)fsize;

    uint8_t *data = (uint8_t *)malloc((size_t)n);
    if (!data) {
        fprintf(stderr, "malloc data\n");
        zpl_file_close(&fin);
        return 1;
    }

    {
        uint8_t *p = data;
        int64_t remaining = (int64_t)n;
        while (remaining > 0) {
            uint32_t chunk = (uint32_t)zpl_min(remaining, 0x1000000LL);
            if (!zpl_file_read(&fin, p, chunk)) {
                fprintf(stderr, "read error\n");
                zpl_file_close(&fin);
                free(data);
                return 1;
            }
            p += chunk;
            remaining -= chunk;
        }
    }
    zpl_file_close(&fin);

    /* --- Подсчёт частот и построение модели (вне замера) --- */
    uint32_t raw_freq[ALPHABET];
    memset(raw_freq, 0, sizeof(raw_freq));
    for (size_t i = 0; i < n; i++) raw_freq[data[i]]++;

    cums_t m;
    int is_rle = model_build(m, raw_freq);
    if (is_rle < 0) {
        fprintf(stderr, "model_build failed\n");
        free(data);
        return 1;
    }

    /* --- Открытие выходного файла и запись заголовка (вне замера) --- */
    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) {
        fprintf(stderr, "fopen output\n");
        free(data);
        return 1;
    }

    uint8_t flags = !!m[0];
    uint8_t rle_sym = (uint8_t)m[1];
    uint8_t sig[4] = { 'r', 'n', flags, rle_sym };
    if (!zpl_file_write(&fout, sig, sizeof(sig))) {
        fprintf(stderr, "write sig\n");
        zpl_file_close(&fout); free(data); return 1;
    }
    if (!zpl_file_write(&fout, &n, sizeof(n))) {
        fprintf(stderr, "write len\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Калибровка частоты CPU --- */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100;
    zpl_u64 dddd = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 zpl_rdtsc_freq = (zpl_rdtsc() - dddd) * 10;
    printf("CPU freq = %1.1f Mhz\n", zpl_rdtsc_freq / 1000000.0f);

    /* --- RLE mode --- */
    if (is_rle) {
        zpl_file_close(&fout);
        printf("ENCODE-RANS OK (RLE, sym=0x%02X)\n", rle_sym);
        printf("  in:  %" PRIu64 " bytes\n", n);
        printf("  out: 12 bytes\n");
        free(data);
        return 0;
    }

    /* --- rANS mode: запись cum[1..256] --- */
    if (!zpl_file_write(&fout, m + 1, sizeof(m[0]) * ALPHABET)) {
        fprintf(stderr, "write cum\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Выделение renorm-буфера под ОДИН блок (переиспользуется).
       Размер = доказанный потолок renorm-слов на блок (см. RENORM_BUF_WORDS). --- */
    size_t buf_words = (size_t)RENORM_BUF_WORDS;
    uint32_t *buf = (uint32_t *)malloc(buf_words * sizeof(uint32_t));
    if (!buf) {
        fprintf(stderr, "malloc buf\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Кодирование: проход по блокам ПРЯМОЙ, внутри блока — НАЗАД (LIFO).
       Замер: только цикл Rans64EncPut, без I/O и без переворота. --- */
    rANS::Encoder enc;
    zpl_u64 total_ticks = 0;
    size_t total_renorm = 0;
    size_t blocks = 0;
    size_t i = 0;   /* сколько байт входа уже закодировано (от начала) */

    while (i < n) {
        size_t a_lo = i;
        size_t a_hi = zpl_min(i + (size_t)BLOCK_BYTES, n);
        size_t block_syms = a_hi - a_lo;

        enc = rANS::Encoder{};   /* свежий независимый state = RANS64_L */

        /* Пишем renorm с КОНЦА буфера назад (buf[--buf_head]). Тогда слова
           сразу ложатся в порядке потребления декодером (последняя эмиссия —
           первой), и отдельный переворот не нужен. Запас (+n/50+1024) при
           этом остаётся СПЕРЕДИ, нетронутым; для текста тронутый хвост буфера
           мал и укладывается в L2. */
        size_t buf_head = buf_words;

        zpl_u64 t0 = zpl_rdtsc();
        for (size_t k = 0, pos = a_hi; k < block_syms; k++) {
            pos--;   /* обход среза НАЗАД: a_hi-1 ... a_lo */
            uint16_t cum_lo, freq;
            model_get(m, data[pos], &cum_lo, &freq);
            auto res = enc.Rans64EncPut(cum_lo, freq, RANS_SCALE_BITS);
            if (res) {
                if (buf_head == 0) {
                    fprintf(stderr, "renorm buffer overflow\n");
                    free(buf); zpl_file_close(&fout); free(data); return 1;
                }
                buf[--buf_head] = *res;
            }
        }
        total_ticks += zpl_rdtsc() - t0;

        auto flush_pair = enc.flush();
        uint32_t flush_hi = (uint32_t)flush_pair.first;   /* HIGH32 state */
        uint32_t flush_lo = (uint32_t)flush_pair.second;  /* LOW32  state */

        size_t renorm_count = buf_words - buf_head;

        /* Заголовок блока: [renorm_count][flush_lo][flush_hi].
           flush в паре (LOW, HIGH) — декодер Init({flush_lo, flush_hi})
           соберёт state = flush_lo | (flush_hi<<32) = исходный state.
           renorm пишем как есть (с buf_head) — порядок уже декодерный. */
        uint32_t blk_hdr[3] = { (uint32_t)renorm_count, flush_lo, flush_hi };
        if (!zpl_file_write(&fout, blk_hdr, sizeof(blk_hdr))) {
            fprintf(stderr, "write block hdr\n");
            free(buf); zpl_file_close(&fout); free(data); return 1;
        }
        if (renorm_count > 0) {
            if (!zpl_file_write(&fout, buf + buf_head, sizeof(buf[0]) * renorm_count)) {
                fprintf(stderr, "write renorm\n");
                free(buf); zpl_file_close(&fout); free(data); return 1;
            }
        }

        total_renorm += renorm_count;
        blocks++;
        i = a_hi;
        printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)",
               (uint64_t)i, n, 100.0 * (double)i / (double)n);
        fflush(stdout);
    }
    printf("\n");

    zpl_i64 out_bytes = zpl_file_size(&fout);
    zpl_file_close(&fout);
    free(buf);
    free(data);

    /* --- Отчёт --- */
    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;
    uint64_t total_enc_time_ms = (zpl_rdtsc_freq > 0)
        ? (uint64_t)(total_ticks * 1000 / zpl_rdtsc_freq)
        : 0;

    double ratio = (n > 0) ? (double)out_bytes / (double)n * 100.0 : 0.0;
    double bpb   = (n > 0) ? (double)out_bytes * 8.0 / (double)n : 0.0;
    double mb_s  = (total_enc_time_ms > 0)
        ? (double)n / (total_enc_time_ms * 1048576.0 / 1000.0)
        : 0.0;

    printf("ENCODE-RANS OK\n");
    printf("  engine:   64-bit rANS, 32-bit renorm, scale_bits=%u\n", RANS_SCALE_BITS);
    printf("  input:    %s\n", argv[1]);
    printf("  in_size:  %" PRIu64 " bytes\n", n);
    printf("  out_size: %" PRIi64 " bytes\n", out_bytes);
    printf("  blocks:   %zu  (block_bytes=%d)\n", blocks, BLOCK_BYTES);
    printf("  renorm:   %zu words\n", total_renorm);
    printf("  ratio:    %.2f%%  (%.3f bits/byte)\n", ratio, bpb);
    printf("  encode:   %" PRIu64 " ms  (%.1f MB/s)\n", total_enc_time_ms, mb_s);
    printf("          : %u ticks per symbol\n", ticks_per_symbol);
    return 0;
}