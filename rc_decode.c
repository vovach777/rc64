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

#include "rc_codec.h"
#include "model.h"
#include "timer.h"

#define OUT_BUF_SIZE (16 * 1024)  /* 16KB выходной буфер */

static int read_u16_le(FILE *f, uint16_t *v) {
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return -1;
    *v = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.rc> <output>\n", argv[0]);
        return 1;
    }

    int64_t freq = timer_freq();
    int64_t t_total0 = timer_ticks();

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("fopen input"); return 1; }

    /* Заголовок */
    uint8_t sig[4];
    if (fread(sig, 1, 4, fin) != 4) { fprintf(stderr, "read sig\n"); fclose(fin); return 1; }
    if (sig[0] != 'r' || sig[1] != 'c') {
        fprintf(stderr, "bad signature\n"); fclose(fin); return 1;
    }
    int is_rle = sig[2] & 0x01;
    uint8_t rle_sym = sig[3];

    uint64_t orig_len = 0;
    for (int i = 0; i < 8; i++) {
        int c = fgetc(fin);
        if (c == EOF) { fprintf(stderr, "read len\n"); fclose(fin); return 1; }
        orig_len |= (uint64_t)(uint8_t)c << (8 * i);
    }
    size_t n = (size_t)orig_len;

    uint8_t *out = malloc(n > 0 ? n : 1);
    if (!out) { fprintf(stderr, "malloc\n"); fclose(fin); return 1; }

    /* RLE mode */
    if (is_rle) {
        int64_t t0 = timer_ticks();
        memset(out, rle_sym, n);
        int64_t t1 = timer_ticks();

        fclose(fin);
        FILE *fout = fopen(argv[2], "wb");
        if (!fout) { perror("fopen output"); free(out); return 1; }
        if (n > 0 && fwrite(out, 1, n, fout) != n) {
            fprintf(stderr, "fwrite\n"); fclose(fout); free(out); return 1;
        }
        fclose(fout);

        double dec_ms = (double)(t1 - t0) * 1000.0 / (double)freq;
        printf("DECODE v2 OK (RLE, sym=0x%02X)\n", rle_sym);
        printf("  decoded: %zu bytes\n", n);
        printf("  decode:  %.2f ms\n", dec_ms);

        free(out);
        return 0;
    }

    /* RC mode: чтение кумулятивных частот cum[1..256] */
    model_t m;
    m.is_rle = 0;
    m.cum[0] = 0;
    for (int i = 1; i <= ALPHABET; i++) {
        uint16_t c;
        if (read_u16_le(fin, &c) != 0) { fprintf(stderr, "read cum\n"); free(out); fclose(fin); return 1; }
        m.cum[i] = c;
    }
    /* cum[256] прочитан из файла — это total */

    /* Чтение всего сжатого потока в память */
    long header_sz = 4 + 8 + (long)(ALPHABET * 2);
    fseek(fin, 0, SEEK_END);
    long fend = ftell(fin);
    fseek(fin, header_sz, SEEK_SET);
    long body_bytes = fend - header_sz;
    if (body_bytes < 0) body_bytes = 0;
    size_t nwords = (size_t)(body_bytes / 4);
    uint32_t *words = malloc((nwords + 8) * sizeof(uint32_t));
    if (!words) { fprintf(stderr, "malloc words\n"); free(out); fclose(fin); return 1; }

    /* Bulk read: читаем сырые байты, конвертируем LE → uint32 */
    {
        size_t got = fread(words, 1, nwords * 4, fin);
        (void)got;
        fclose(fin);
        /* in-place LE → uint32 (endian-safe) */
        uint8_t *raw = (uint8_t *)words;
        for (size_t i = 0; i < nwords; i++) {
            uint8_t *b = raw + i * 4;
            words[i] = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        }
    }
    for (size_t i = nwords; i < nwords + 8; i++) words[i] = 0;

    /* Декодирование — чистый замер.
       Декодер пишет в out[] (в памяти). Сброс на диск порциями 16KB
       через статический буфер, во время сброса таймер выключен. */
    rc_dec_t rd;
    rc_dec_init(&rd, words);
    size_t wi = 3;

    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("fopen output"); free(out); free(words); return 1; }

    int64_t t_dec0 = timer_ticks();
    size_t flushed = 0;  /* сколько байт уже сброшено на диск */
    for (size_t i = 0; i < n; i++) {
        uint32_t cum = rc_dec_get_cum(&rd, TARGET_TOTAL);
        uint32_t cum_lo, freq;
        uint8_t sym = model_find(&m, cum, &cum_lo, &freq);
        out[i] = sym;

        /* rc_dec_step inline */
        uint64_t t = rd.range / TARGET_TOTAL;
        uint64_t step = t * cum_lo;
        rd.code -= step;
        rd.range = t * freq;
        if (rd.range < SCHINDLER_BOTTOM_64) {
            uint32_t new_dword = words[wi++];
            rd.code = (rd.code << 32) | new_dword;
            rd.range <<= 32;
        }

        /* Сброс 16KB порции — ВНЕ замера */
        size_t ready = i + 1 - flushed;
        if (ready >= OUT_BUF_SIZE) {
            int64_t t_pause = timer_ticks();
            fwrite(out + flushed, 1, OUT_BUF_SIZE, fout);
            int64_t t_resume = timer_ticks();
            t_dec0 += (t_resume - t_pause);  /* вычитаем время I/O */
            flushed += OUT_BUF_SIZE;
        }
    }
    int64_t t_dec1 = timer_ticks();

    /* Дописываем остаток — вне замера */
    if (flushed < n) {
        fwrite(out + flushed, 1, n - flushed, fout);
    }
    fclose(fout);
    int64_t t_total1 = timer_ticks();

    free(words);

    double dec_ms = (double)(t_dec1 - t_dec0) * 1000.0 / (double)freq;
    double total_ms = (double)(t_total1 - t_total0) * 1000.0 / (double)freq;
    printf("DECODE v2 OK\n");
    printf("  input:   %s (%zu words)\n", argv[1], nwords);
    printf("  decoded: %zu bytes\n", n);
    printf("  decode:  %.2f ms  (%.1f MB/s)\n",
           dec_ms, n > 0 ? (double)n / (dec_ms * 1048576.0 / 1000.0) : 0.0);
    printf("  total:   %.2f ms  (incl. I/O)\n", total_ms);

    free(out);
    return 0;
}