/* =========================================================================
 * RC ENCODE v2 — inplace cache/FF, статическая 14-битная модель
 * =========================================================================
 *
 * Перенесён на zpl.h (как rc_decode.c):
 *   - I/O через zpl_file (zpl_file_open / zpl_file_create / zpl_file_read /
 *     zpl_file_write / zpl_file_close / zpl_file_size)
 *   - Замер через zpl_rdtsc() с автокалибровкой частоты CPU
 *   - Прогресс по ходу кодирования (по блокам 16KB), как в декодере
 *   - Изолированный замер: только цикл кодирования считается в total_ticks;
 *     I/O (чтение входа, запись выхода) И модели (model_build) НЕ входят в
 *     замер — это симметрично тому, как декодер не учитывает загрузку .rc
 *     и сброс out_buf на диск.
 *
 * Usage:
 *   rc_encode <input_file> <output_file>
 *
 * Формат .rc:
 *   [4 байта]  сигнатура 'r','c', flags, rle_sym
 *   [8 байт]   uint64_t original_len (LE)
 *   --- RLE mode ---
 *              (больше ничего)
 *   --- RC mode ---
 *   [512 байт] cum[1..256] — uint16_t LE (кумулятивные частоты)
 *              cum[0]=0 не хранится (всегда константа)
 *   [4*N байт] uint32_t words LE — поток энкодера
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"

#include "rc_codec.h"
#include "model.h"

/* Размер блока для прогресса. Энкодер работает по полной in-памяти,
   но прогресс печатается блоками по 16KB — симметрично декодеру. */
#define PROGRESS_BLOCK (16 * 1024)

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input_file> <output_file>\n", argv[0]);
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

    /* Чтение потенциально большего файла чанками, как в декодере. */
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

    /* --- Подсчёт частот (вне замера — это часть модели) --- */
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
    uint8_t sig[4] = { 'r', 'c', flags, rle_sym };
    if (!zpl_file_write(&fout, sig, sizeof(sig))) {
        fprintf(stderr, "write sig\n");
        zpl_file_close(&fout); free(data); return 1;
    }
    if (!zpl_file_write(&fout, &n, sizeof(n))) {
        fprintf(stderr, "write len\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Калибровка частоты CPU (как в декодере) --- */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100;
    zpl_u64 dddd = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 zpl_rdtsc_freq = (zpl_rdtsc() - dddd) * 10;
    printf("CPU freq = %1.1f Mhz\n", zpl_rdtsc_freq / 1000000.0f);

    /* --- RLE mode --- */
    if (is_rle) {
        zpl_file_close(&fout);
        printf("ENCODE v2 OK (RLE, sym=0x%02X)\n", rle_sym);
        printf("  in:  %" PRIu64 " bytes\n", n);
        printf("  out: 12 bytes\n");
        free(data);
        return 0;
    }

    /* --- RC mode: запись кумулятивных частот cum[1..256] (вне замера) --- */
    if (!zpl_file_write(&fout, m + 1, sizeof(m[0]) * ALPHABET)) {
        fprintf(stderr, "write cum\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Выделение буфера потока --- */
    size_t buf_words = (size_t)(n / 4 + 256);
    uint32_t *buf = (uint32_t *)malloc(buf_words * sizeof(uint32_t));
    if (!buf) {
        fprintf(stderr, "malloc buf\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Кодирование (inplace — структура сама пишет в buf через out_ptr).
       Замер: только цикл кодирования. Прогресс печатается по блокам 16KB,
       как в декодере. Внутри блока — чистый rdtsc, без I/O. --- */
    rc_enc_t rc;
    rc_enc_init(&rc, buf, buf_words);

    zpl_u64 total_ticks = 0;
    size_t i = 0;
    while (i < n) {
        size_t block = zpl_min(n - i, (size_t)PROGRESS_BLOCK);
        zpl_u64 t0 = zpl_rdtsc();
        for (size_t k = 0; k < block; k++) {
            uint16_t cum_lo, freq;
            model_get(m, data[i + k], &cum_lo, &freq);
            rc_enc_step(&rc, cum_lo, freq, TARGET_TOTAL);
        }
        total_ticks += zpl_rdtsc() - t0;
        i += block;
        printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)",
               (uint64_t)i, n, 100.0 * (double)i / (double)n);
        fflush(stdout);
    }
    /* flush — это ~2 слова, замеряем вместе с последним блоком не нужно,
       он не относится к per-symbol hot loop. Делаем вне замера. */
    rc_enc_flush(&rc);
    printf("\n");

    size_t nwords = (size_t)(rc.out_ptr - buf);

    /* --- Запись потока (вне замера) --- */
    if (!zpl_file_write(&fout, buf, sizeof(buf[0]) * nwords)) {
        fprintf(stderr, "write word\n");
        free(buf); zpl_file_close(&fout); free(data); return 1;
    }

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

    printf("ENCODE v2 OK\n");
    printf("  input:   %s\n", argv[1]);
    printf("  in_size:  %" PRIu64 " bytes\n", n);
    printf("  out_size: %" PRIi64 " bytes\n", out_bytes);
    printf("  ratio:   %.2f%%  (%.3f bits/byte)\n", ratio, bpb);
    printf("  encode:  %" PRIu64 " ms  (%.1f MB/s)\n", total_enc_time_ms, mb_s);
    printf("        :  %u  ticks per symbol\n", ticks_per_symbol);
    return 0;
}
