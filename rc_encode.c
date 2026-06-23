/* =========================================================================
 * RC ENCODE v2 — inplace cache/FF, статическая 14-битная модель
 * =========================================================================
 *
 * Usage:
 *   rc_encode_v2 <input_file> <output_file>
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

//#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "rc_codec.h"
#include "model.h"
#include "timer.h"

static int write_u16_le(FILE *f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };
    return fwrite(b, 1, 2, f) == 2 ? 0 : -1;
}

static int write_u32_le(FILE *f, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
        (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF),
    };
    return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}

static int write_u64_le(FILE *f, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        if (fputc((int)((v >> (8 * i)) & 0xFF), f) == EOF) return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input_file> <output_file>\n", argv[0]);
        return 1;
    }

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("fopen input"); return 1; }
    fseek(fin, 0, SEEK_END);
    long fsize = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    if (fsize <= 0) { fprintf(stderr, "ftell failed\n"); fclose(fin); return 1; }
    uint64_t n = (size_t)fsize;
    uint8_t *data = malloc(fsize);
    if (!data) { fprintf(stderr, "malloc failed\n"); fclose(fin); return 1; }
    if (fread(data, 1, n, fin) != n) {
        fprintf(stderr, "fread failed\n"); free(data); fclose(fin); return 1;
    }
    fclose(fin);

    /* Подсчёт частот */
    uint32_t raw_freq[ALPHABET];
    memset(raw_freq, 0, sizeof(raw_freq));
    for (size_t i = 0; i < n; i++) raw_freq[data[i]]++;

    cums_t m;
    int is_rle = model_build(m, raw_freq);
    if (is_rle < 0 ) {
        fprintf(stderr, "model_build failed\n"); free(data); return 1;
    }

    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("fopen output"); free(data); return 1; }

    /* Заголовок */
    uint8_t flags = !!m[0];
    uint8_t rle_sym = m[1];
    uint8_t sig[4] = { 'r', 'c', flags, rle_sym };
    if (fwrite(sig, 1, 4, fout) != 4) {
        fprintf(stderr, "write sig\n"); free(data); fclose(fout); return 1;
    }
    if (fwrite(&n,sizeof(n), 1, fout) != 1) {
        fprintf(stderr, "write len\n"); free(data); fclose(fout); return 1;
    }

    int64_t freq = timer_freq();
    int64_t t0 = timer_ticks();

    /* RLE mode */
    if (is_rle) {
        int64_t t1 = timer_ticks();
        long out_bytes = ftell(fout);
        fclose(fout);
        double enc_ms = (double)(t1 - t0) * 1000.0 / (double)freq;
        printf("ENCODE v2 OK (RLE, sym=0x%02X)\n", rle_sym);
        printf("  in:  %zu bytes\n", n);
        printf("  out: %ld bytes\n", out_bytes);
        printf("  time: %.2f ms\n", enc_ms);
        free(data);
        return 0;
    }

    /* RC mode: запись кумулятивных частот cum[1..256] (256 значений).
    cum[0]=0 не хранится — декодер ставит сам. */
    if ( fwrite(m+1,sizeof(m[0]),ALPHABET,fout) != ALPHABET) {
        fprintf(stderr, "write cum\n"); free(data); fclose(fout); return 1;
    }

    /* Выделение буфера потока */
    size_t buf_words = n / 4 + 256;
    uint32_t *buf = malloc(buf_words*4);
    if (!buf) { fprintf(stderr, "malloc buf\n"); free(data); fclose(fout); return 1; }

    /* Кодирование (inplace — структура сама пишет в buf через out_ptr) */
    rc_enc_t rc;
    rc_enc_init(&rc, buf, buf_words);

    for (size_t i = 0; i < n; i++) {
        uint16_t cum_lo, freq;
        model_get(&m, data[i], &cum_lo, &freq);
        rc_enc_step(&rc, cum_lo, freq, TARGET_TOTAL);
    }
    rc_enc_flush(&rc);

    size_t nwords = (size_t)(rc.out_ptr - buf);

    int64_t t1 = timer_ticks();

    /* Запись потока */
    if (fwrite(buf, sizeof(buf[0]), nwords, fout) != nwords) {
            fprintf(stderr, "write word\n");
            free(buf); free(data); fclose(fout); return 1;
    }

    long out_bytes = ftell(fout);
    fclose(fout);
    free(buf);

    double enc_ms = (double)(t1 - t0) * 1000.0 / (double)freq;
    double ratio = (n > 0) ? (double)out_bytes / (double)n * 100.0 : 0.0;
    double bpb = (n > 0) ? (double)out_bytes * 8.0 / (double)n : 0.0;

    printf("ENCODE v2 OK\n");
    printf("  input:   %s\n", argv[1]);
    printf("  in_size:  %zu bytes\n", n);
    printf("  out_size: %ld bytes\n", out_bytes);
    printf("  ratio:   %.2f%%  (%.3f bits/byte)\n", ratio, bpb);
    printf("  time:    %.2f ms  (%.1f MB/s)\n",
           enc_ms, n > 0 ? (double)n / (enc_ms * 1048576.0 / 1000.0) : 0.0);

    free(data);
    return 0;
}