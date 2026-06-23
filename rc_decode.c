/* =========================================================================
 * RC DECODE v2 — полный буфер в память, buffered output
 * =========================================================================
 *
 * Сжатый поток целиком грузится в память (random access, быстро).
 * Вывод через статический буфер 16KB, сброс на диск порциями.
 * Во время сброса таймер выключен.
 *
 * Usage:
 *   rc_decode <input_file.rc> <output_file> [original_file]
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

#define OUT_BUF_SIZE (16 * 1024)  /* 16KB выходной буфер */


int main(int argc, char **argv) {
    static uint8_t out_buf[OUT_BUF_SIZE];
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.rc> <output>\n", argv[0]);
        return 1;
    }


    zpl_file fin;
    zpl_file_error err = zpl_file_open(&fin, argv[1]);

    if (err) { perror("fopen input"); return err; }


    /* Заголовок */
    uint8_t sig[4];

    if (zpl_file_read(&fin,sig,sizeof(sig)) != 1) {
        fprintf(stderr, "read sig\n");
        zpl_file_close(&fin);
        return 1;
    }
    if (sig[0] != 'r' || sig[1] != 'c') {
        fprintf(stderr, "bad signature\n"); zpl_file_close(&fin);; return 1;
    }
    int is_rle = sig[2] & 0x01;
    uint8_t rle_sym = sig[3];

    uint64_t orig_len = 0;
    if (zpl_file_read(&fin, &orig_len, sizeof(orig_len)) != 1)  {
        fprintf(stderr, "read len\n"); zpl_file_close(&fin); return 1;
    }
    size_t n = (size_t)orig_len;
    if (n==0) {
        zpl_file_close(&fin);
        zpl_file_create(&fin, argv[2]);
        zpl_file_close(&fin);
        return 0;
    }

    /* RLE mode */
    if (is_rle) {
        zpl_file_close(&fin);
        if ( zpl_file_create(&fin, argv[2]) ) { perror("fopen output");  return 1; }
        while (n--) {
            zpl_file_write(&fin, &rle_sym, 1);
        }
        zpl_file_close(&fin);
        printf("DECODE v2 OK (RLE Mode)\n");
        return 0;
    }

    /* RC mode: чтение кумулятивных частот cum[1..256] */
    cums_t m = {0};
    zpl_file_read(&fin, m + 1, sizeof(m[0]) * ALPHABET);
    zpl_i64 ac_data_len = (zpl_file_size(&fin) - 524);

    zpl_u64 t0xx = zpl_time_rel_ms() + 100;
    zpl_u64 dddd = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 zpl_rdtsc_freq = (zpl_rdtsc() - dddd)*10;
    printf("CPU freq = %1.1f Mhz\n", zpl_rdtsc_freq / 1000000.0f);

    uint64_t ac_u32_len = ac_data_len / sizeof(uint32_t) + 4;
    uint32_t *words = (uint32_t *) malloc( ac_u32_len * sizeof(uint32_t)  );
    if ( words == NULL) {
        fprintf(stderr, "malloc words\n");
        zpl_file_close(&fin);
        return 1;
    }
    uint8_t *u8_p =(uint8_t*)words;
    int64_t u8_len = ac_data_len;
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
    if ( zpl_file_create(&fin, argv[2]) ) {
        perror("fopen output"); free(words); return 1;
    }

    /* Декодирование — чистый замер.
       Декодер пишет в out[] (в памяти). Сброс на диск порциями 16KB
       через статический буфер, во время сброса таймер выключен. */
    uint32_t* words_p = words;
    rc_dec_t rd;
    words_p += rc_dec_init(&rd, words_p);

    zpl_u64 total_ticks = 0;
    for (size_t i = 0; i != n;) {
        uint32_t block_size = zpl_min(n - i, OUT_BUF_SIZE);
        i += block_size;
        zpl_u64 t0 = zpl_rdtsc();
        for (uint32_t n=0; n < block_size; ++n) {
            uint16_t cum = rc_dec_get_cum(&rd, TARGET_TOTAL);
            uint16_t cum_lo, freq;
            uint8_t sym = model_find(m, cum, &cum_lo, &freq);
            words_p += rc_dec_step(&rd, cum_lo, freq, TARGET_TOTAL, words_p);
            out_buf[n] = sym;
        }
        total_ticks += zpl_rdtsc()-t0;
        //fwrite(out_buf, 1, block_size, fout);
        if ( zpl_file_write(&fin, out_buf, block_size) != 1 ) {
            perror("fwrite error"); zpl_file_close(&fin); free(words); return 1;
        }
        printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)", i + 1, n, 100.0 * (i + 1) / n);
    }
    printf("\n");
    uint32_t ticks_per_symbol = total_ticks / n;
    uint64_t total_dec_time = total_ticks*1000 / zpl_rdtsc_freq;

    //fclose(fout);
    zpl_file_close(&fin);
    free(words);

    printf("DECODE v2 OK\n");
    printf("  decode:  %" PRIu64 " ms  (%.1f MB/s)\n", total_dec_time, n * ( 1. / 1024.0 ) * (1. / total_dec_time));
    printf("        :  %u  ticks per symbol\n", ticks_per_symbol);
    printf("  memory:  %.2f MB\n", (float)ac_u32_len / (1024.0f * 1024.0f / 4));
    return 0;
}