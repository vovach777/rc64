/* =========================================================================
 * RANS DECODE — 64-bit rANS, 14-битная статическая модель
 * =========================================================================
 *
 * Декодер грузит весь renorm-поток в память (malloc), затем идёт по блокам
 * ВПЕРЁД указателем words_p. renorm внутри блока читается ВПЕРЁД по факту
 * потребления (энкодер перевернул буфер); число renorm-слов блока НЕ хранится —
 * после декодирования block_syms символов указатель сам встаёт на flush-пару
 * следующего блока (зеркальность rANS: кодировщик эмиттит K ⟺ декодер потребляет K).
 *
 * Usage:
 *   rans_decode <input_file.rans> <output_file>
 *
 * Поток .rans (см. rans_encode.cpp):
 *   [4]    sig 'r','n', flags, rle_sym
 *   [8]    uint64_t original_len (LE)
 *   [512]  cum[1..256] uint16_t LE
 *   далее подряд блоки:
 *     [4]    uint32_t flush_lo  = LOW32  state блока (LE)
 *     [4]    uint32_t flush_hi  = HIGH32 state блока (LE)
 *     [4*K]  renorm uint32_t LE — В ПОРЯДКЕ ПОТРЕБЛЕНИЯ (читаем ВПЕРЁД)
 *
 * Размер блока по ВХОДУ фиксирован (BLOCK_BYTES), поэтому число символов блока
 * декодер знает сам: min(BLOCK_BYTES, orig_len − уже_декодировано). sym_count
 * в потоке не хранится.
 *
 * Декодер:
 *   - читает flush-пару блока [flush_lo][flush_hi] (заголовка/renorm_count НЕТ),
 *   - Init({flush_lo, flush_hi}) (с обменом, как и раньше) → исходный state,
 *   - ест renorm ВПЕРЁД по потреблению (words_p += Rans64Dec(...)), идёт по выходу ВПЕРЁД.
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
#define BLOCK_BYTES      (256 * 1024)   /* фикс. размер блока ВХОДА (как у энкодера) */
/* Потолок renorm-слов на блок = BLOCK_BYTES*14/32 + запас (см. rans_encode.cpp). */
#define RENORM_BUF_WORDS (BLOCK_BYTES * 14u / 32u + 1024)
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
    if (!zpl_file_read(&fin, sig, sizeof(sig))) {
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
    if (!zpl_file_read(&fin, &orig_len, sizeof(orig_len))) {
        fprintf(stderr, "read len\n");
        zpl_file_close(&fin);
        return 1;
    }
    size_t n = (size_t)orig_len;
    if (n == 0) {
        zpl_file_close(&fin);
        zpl_file fout;
        zpl_file_create(&fout, argv[2]);
        zpl_file_close(&fout);
        return 0;
    }

    /* --- RLE mode --- */
    if (is_rle) {
        zpl_file_close(&fin);
        zpl_file fout;
        if (zpl_file_create(&fout, argv[2])) { perror("fopen output"); return 1; }
        memset(out_buf, rle_sym, OUT_BUF_SIZE);
        size_t remaining = n;
        while (remaining > 0) {
            size_t block = (remaining < OUT_BUF_SIZE) ? remaining : OUT_BUF_SIZE;
            if (!zpl_file_write(&fout, out_buf, block)) {
                perror("fwrite error");
                zpl_file_close(&fout);
                return 1;
            }
            remaining -= block;
        }
        zpl_file_close(&fout);
        printf("DECODE-RANS OK (RLE Mode, sym=0x%02X, len=%zu)\n", rle_sym, n);
        return 0;
    }

    /* --- rANS mode: чтение cum[1..256] --- */
    cums_t m = {0};
    if (!zpl_file_read(&fin, m + 1, sizeof(m[0]) * ALPHABET)) {
        fprintf(stderr, "read cum\n");
        zpl_file_close(&fin);
        return 1;
    }

    lut_t lut;
    model_build_lut(lut, m);

    /* Калибровка CPU */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100;
    zpl_u64 dddd = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 zpl_rdtsc_freq = (zpl_rdtsc() - dddd) * 10;
    printf("CPU freq = %1.1f Mhz\n", zpl_rdtsc_freq / 1000000.0f);

    zpl_i64 rans_data_len = (zpl_file_size(&fin) - 524);
    const size_t buf_words = rans_data_len/4;



    uint64_t rans_u32_len = (buf_words+1)*4;
    uint32_t *words = (uint32_t *) malloc( rans_u32_len  );
    if ( words == NULL) {
        fprintf(stderr, "malloc words\n");
        zpl_file_close(&fin);
        return 1;
    }
    uint8_t *u8_p =(uint8_t*)words;
    int64_t u8_len = rans_data_len;
    while ( u8_len> 0 ) {
        uint32_t chunk = (uint32_t)zpl_min(u8_len, 0x1000000LL);

        if (zpl_file_read (&fin,u8_p , chunk) != 1) {
           fprintf(stderr, "read error\n"); zpl_file_close(&fin);
           free(words);
           return 1;
        }
       u8_p  += chunk;
       u8_len -= chunk;
    }
    zpl_file_close(&fin);


    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) {
        free(words);
        perror("fopen output");
        return 1;
    }

    zpl_u64 total_ticks = 0;
    size_t i = 0;   /* выдано байт */

    auto words_p = words;

    while (i < n) {
        size_t block_syms = zpl_min(n - i, (size_t)BLOCK_BYTES);


        /* Init({flush_lo, flush_hi}) строит state = flush_lo | (flush_hi<<32)
           = (HIGH<<32) | LOW = исходный финальный state блока. */
        rANS::Decoder dec;
        dec.Init({words_p[0], words_p[1]});
        words_p += 2;

        size_t block_done = 0;
        while (block_done < block_syms) {
            size_t out_block = zpl_min(block_syms - block_done, (size_t)OUT_BUF_SIZE);
            zpl_u64 t0 = zpl_rdtsc();
            for (size_t k = 0; k < out_block; k++) {
                uint32_t cum = dec.Rans64DecGet(RANS_SCALE_BITS);
                uint16_t cum_lo, freq;
                uint8_t sym = model_find_lut(lut, m, (uint16_t)cum, &cum_lo, &freq);
                words_p += dec.Rans64Dec(cum_lo, freq, RANS_SCALE_BITS, *words_p);
                out_buf[k] = sym;
            }
            total_ticks += zpl_rdtsc() - t0;
            if (!zpl_file_write(&fout, out_buf, out_block)) {
                free(words);
                perror("fwrite error");
                zpl_file_close(&fout);
                return 1;
            }
            block_done += out_block;
        }

        i += block_syms;
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

    zpl_file_close(&fout);
    free(words);

    printf("DECODE-RANS OK\n");
    printf("  engine:   64-bit rANS, 32-bit renorm, scale_bits=%u\n", RANS_SCALE_BITS);
    printf("  decode:   %" PRIu64 " ms  (%.1f MB/s)\n", total_dec_time_ms, mb_s);
    printf("          : %u ticks per symbol\n", ticks_per_symbol);
    printf("  memory:   %.2f MB (block buffer only)\n",
           (float)rans_u32_len / (1024.0f * 1024.0f));
    return 0;
}