/* =========================================================================
 * RANS DECODE — 64-bit rANS, 14-битная статическая модель
 * =========================================================================
 *
 * Сжатый поток целиком грузится в память. Вывод через статический буфер 16KB.
 *
 * Usage:
 *   rans_decode <input_file.rans> <output_file>
 *
 * Поток .rans (см. rans_encode.cpp):
 *   [4]    sig 'r','n', flags, rle_sym
 *   [8]    uint64_t original_len (LE)
 *   [512]  cum[1..256] uint16_t LE
 *   [4*K]  renorm uint32_t words LE (порядок эмиссии энкодера)
 *   [8]    flush pair: HIGH32, LOW32 (LE)
 *
 * Декодер:
 *   - читает flush-пару с конца потока, Init (с обменом first/second),
 *   - renorm-слова потребляет в обратном порядке (с конца буфера),
 *   - идёт по выходу ВПЕРЁД (симметрично обратному обходу энкодера).
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

#define RANS_SCALE_BITS  14u
#define OUT_BUF_SIZE     (16 * 1024)

int main(int argc, char **argv) {
    static uint8_t out_buf[OUT_BUF_SIZE];
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.rans> <output>\n", argv[0]);
        return 1;
    }

    zpl_file fin;
    zpl_file_error err = zpl_file_open(&fin, argv[1]);
    if (err) { perror("fopen input"); return err; }

    /* --- Заголовок --- */
    uint8_t sig[4];
    if (zpl_file_read(&fin, sig, sizeof(sig)) != 1) {
        fprintf(stderr, "read sig\n");
        zpl_file_close(&fin);
        return 1;
    }
    if (sig[0] != 'r' || sig[1] != 'n') {
        fprintf(stderr, "bad signature (expected 'rn', got %c%c)\n",
                sig[0], sig[1]);
        zpl_file_close(&fin);
        return 1;
    }
    int is_rle = sig[2] & 0x01;
    uint8_t rle_sym = sig[3];

    uint64_t orig_len = 0;
    if (zpl_file_read(&fin, &orig_len, sizeof(orig_len)) != 1) {
        fprintf(stderr, "read len\n");
        zpl_file_close(&fin);
        return 1;
    }
    size_t n = (size_t)orig_len;
    if (n == 0) {
        zpl_file_close(&fin);
        zpl_file_create(&fin, argv[2]);
        zpl_file_close(&fin);
        return 0;
    }

    /* --- RLE mode --- */
    if (is_rle) {
        zpl_file_close(&fin);
        if (zpl_file_create(&fin, argv[2])) { perror("fopen output"); return 1; }
        memset(out_buf, rle_sym, OUT_BUF_SIZE);
        size_t remaining = n;
        while (remaining > 0) {
            size_t block = (remaining < OUT_BUF_SIZE) ? remaining : OUT_BUF_SIZE;
            if (!zpl_file_write(&fin, out_buf, block)) {
                perror("fwrite error");
                zpl_file_close(&fin);
                return 1;
            }
            remaining -= block;
        }
        zpl_file_close(&fin);
        printf("DECODE-RANS OK (RLE Mode, sym=0x%02X, len=%zu)\n", rle_sym, n);
        return 0;
    }

    /* --- rANS mode: чтение cum[1..256] --- */
    cums_t m = {0};
    if (zpl_file_read(&fin, m + 1, sizeof(m[0]) * ALPHABET) != 1) {
        fprintf(stderr, "read cum\n");
        zpl_file_close(&fin);
        return 1;
    }

    lut_t lut;
    model_build_lut(lut, m);

    /* Размер потока = файл - заголовок 524.
       В потоке: renorm-слова (4*K байт) + flush-пара (8 байт). */
    zpl_i64 ac_data_len = zpl_file_size(&fin) - 524;
    if (ac_data_len < 8) {
        fprintf(stderr, "stream too short: %lld bytes\n", (long long)ac_data_len);
        zpl_file_close(&fin);
        return 1;
    }

    /* Калибровка CPU */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100;
    zpl_u64 dddd = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 zpl_rdtsc_freq = (zpl_rdtsc() - dddd) * 10;
    printf("CPU freq = %1.1f Mhz\n", zpl_rdtsc_freq / 1000000.0f);

    /* Чтение всего потока в память */
    size_t stream_u8_len = (size_t)ac_data_len;
    size_t stream_u32_len = stream_u8_len / sizeof(uint32_t) + 2;  /* + запас */
    uint32_t *words = (uint32_t *)malloc(stream_u32_len * sizeof(uint32_t));
    if (!words) {
        fprintf(stderr, "malloc words\n");
        zpl_file_close(&fin);
        return 1;
    }

    {
        uint8_t *u8_p = (uint8_t *)words;
        int64_t u8_len = (int64_t)stream_u8_len;
        while (u8_len > 0) {
            uint32_t chunk = (uint32_t)zpl_min(u8_len, 0x1000000LL);
            if (zpl_file_read(&fin, u8_p, chunk) != 1) {
                fprintf(stderr, "read error\n");
                zpl_file_close(&fin);
                free(words);
                return 1;
            }
            u8_p += chunk;
            u8_len -= chunk;
        }
    }
    zpl_file_close(&fin);

    /* Поток: [renorm_0 .. renorm_{K-1}] [flush_high] [flush_low]
       K = (stream_u8_len - 8) / 4 */
    size_t renorm_count = (stream_u8_len - 8) / sizeof(uint32_t);
    uint32_t flush_hi = words[renorm_count + 0];
    uint32_t flush_lo = words[renorm_count + 1];

    /* Init({a, b}) строит state = (b<<32) | a.
       flush вернул (HIGH, LOW) — нужно передать ({LOW, HIGH}), чтобы получить
       исходный state = (HIGH<<32) | LOW. Поэтому меняем местами. */
    rANS::Decoder dec;
    dec.Init({flush_lo, flush_hi});

    if (zpl_file_create(&fin, argv[2])) {
        perror("fopen output");
        free(words);
        return 1;
    }

    /* Указатель renorm: идём с конца буфера НАЗАД.
       ptr указывает на СЛЕДУЮЩЕЕ слово для потребления (после декремента —
       текущее потреблённое). Начинаем с renorm_count-1 (последнее слово). */
    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i != n) {
        size_t block_size = zpl_min(n - i, (size_t)OUT_BUF_SIZE);
        zpl_u64 t0 = zpl_rdtsc();
        for (size_t k = 0; k < block_size; k++) {
            uint32_t cum = dec.Rans64DecGet(RANS_SCALE_BITS);
            uint16_t cum_lo, freq;
            uint8_t sym = model_find_lut(lut, m, (uint16_t)cum, &cum_lo, &freq);

            /* next — следующее слово для renorm, если потребуется.
               Если буфер исчерпан (ptr == SIZE_MAX), передаём 0 — для
               валидного потока renorm в этот момент не trigger'нет. */
            uint32_t next = (renorm_count > 0) ? words[renorm_count - 1] : 0u;
            bool consumed = dec.Rans64Dec(cum_lo, freq, RANS_SCALE_BITS, next);
            if (consumed) {
                if (renorm_count == 0) {
                    fprintf(stderr, "renorm underflow\n");
                    zpl_file_close(&fin); free(words); return 1;
                }
                renorm_count--;
            }
            out_buf[k] = sym;
        }
        total_ticks += zpl_rdtsc() - t0;
        if (!zpl_file_write(&fin, out_buf, block_size)) {
            perror("fwrite error");
            zpl_file_close(&fin); free(words); return 1;
        }
        i += block_size;
        printf("\r                      \r%zu / %zu  (%3.1f%%)",
               i, n, 100.0 * (double)i / (double)n);
        fflush(stdout);
    }
    printf("\n");

    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;
    uint64_t total_dec_time_ms = (zpl_rdtsc_freq > 0)
        ? (uint64_t)(total_ticks * 1000 / zpl_rdtsc_freq)
        : 0;
    double mb_s = (total_dec_time_ms > 0)
        ? (double)n / (total_dec_time_ms * 1048576.0 / 1000.0)
        : 0.0;

    zpl_file_close(&fin);
    free(words);

    printf("DECODE-RANS OK\n");
    printf("  engine:   64-bit rANS, 32-bit renorm, scale_bits=%u\n", RANS_SCALE_BITS);
    printf("  decode:   %" PRIu64 " ms  (%.1f MB/s)\n", total_dec_time_ms, mb_s);
    printf("          : %u ticks per symbol\n", ticks_per_symbol);
    printf("  memory:   %.2f MB\n",
           (float)stream_u32_len / (1024.0f * 1024.0f / 4));
    return 0;
}