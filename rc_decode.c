/* =========================================================================
 * RC DECODE v2 — streaming с кольцевым буфером (cache-friendly)
 * =========================================================================
 *
 * Сжатый поток читается блоками по 16KB (4096 uint32 слов).
 * Декодер берёт слова из кольцевого буфера, refill когда исчерпан.
 * Весь поток не загружается в память — работает на файлах любого размера.
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

#define STREAM_WORDS 4096  /* 16KB кольцевой буфер (L1 friendly) */

static int read_u16_le(FILE *f, uint16_t *v) {
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return -1;
    *v = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return 0;
}

static uint32_t read_u32_le(FILE *f) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Streaming контекст для чтения сжатого потока */
typedef struct {
    FILE *fin;
    uint32_t buf[STREAM_WORDS + 8];  /* +8 для padding */
    size_t buf_pos;   /* текущая позиция в буфере */
    size_t buf_len;   /* сколько слов в буфере */
    int eof;          /* достигнут конец файла */
} stream_t;

static void stream_init(stream_t *s, FILE *f) {
    s->fin = f;
    s->buf_pos = 0;
    s->buf_len = 0;
    s->eof = 0;
}

/* Refill буфера. Читаем блок uint32 слов одним fread. */
static int stream_refill(stream_t *s) {
    if (s->eof) {
        s->buf_len = 0;
        s->buf_pos = 0;
        for (int i = 0; i < 8; i++) s->buf[i] = 0;
        s->buf_len = 8;
        return 0;
    }

    /* Читаем сырые байты одним fread, потом конвертируем LE → uint32 */
    size_t to_read_bytes = STREAM_WORDS * 4;
    uint8_t *raw = (uint8_t *)s->buf;  /* alias: buf и raw指向 одну память */
    size_t got = fread(raw, 1, to_read_bytes, s->fin);
    size_t got_words = got / 4;

    /* Конвертиация LE → uint32 (in-place, безопасно: 4 байта → 1 uint32) */
    for (size_t i = 0; i < got_words; i++) {
        uint8_t *b = raw + i * 4;
        s->buf[i] = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    }

    /* Padding */
    for (size_t i = got_words; i < got_words + 8 && i < STREAM_WORDS + 8; i++)
        s->buf[i] = 0;
    s->buf_len = got_words;
    s->buf_pos = 0;

    if (got < to_read_bytes) s->eof = 1;
    return 0;
}

/* Читать следующее слово из потока */
static inline uint32_t stream_next(stream_t *s) {
    if (s->buf_pos >= s->buf_len) {
        stream_refill(s);
    }
    return s->buf[s->buf_pos++];
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input.rc> <output> [original]\n", argv[0]);
        return 1;
    }

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

    double t0 = timer_sec();

    uint8_t *out = malloc(n > 0 ? n : 1);
    if (!out) { fprintf(stderr, "malloc\n"); fclose(fin); return 1; }

    /* RLE mode */
    if (is_rle) {
        double tdec0 = timer_sec();
        memset(out, rle_sym, n);
        double tdec1 = timer_sec();

        fclose(fin);
        FILE *fout = fopen(argv[2], "wb");
        if (!fout) { perror("fopen output"); free(out); return 1; }
        if (n > 0 && fwrite(out, 1, n, fout) != n) {
            fprintf(stderr, "fwrite\n"); fclose(fout); free(out); return 1;
        }
        fclose(fout);

        printf("DECODE v2 OK (RLE, sym=0x%02X)\n", rle_sym);
        printf("  decoded: %zu bytes\n", n);
        printf("  decode:  %.2f ms\n", (tdec1 - tdec0) * 1000.0);

        if (argc == 4) {
            FILE *fref = fopen(argv[3], "rb");
            if (!fref) { perror("fopen original"); free(out); return 1; }
            fseek(fref, 0, SEEK_END);
            long rs = ftell(fref); fseek(fref, 0, SEEK_SET);
            uint8_t *ref = malloc(rs > 0 ? (size_t)rs : 1);
            if (rs > 0 && fread(ref, 1, (size_t)rs, fref) != (size_t)rs) {
                fprintf(stderr, "read ref\n"); fclose(fref); free(ref); free(out); return 1;
            }
            fclose(fref);
            int rc = 0;
            if ((size_t)rs != n) { printf("  ROUNDTRIP MISMATCH: len\n"); rc = 2; }
            else if (n > 0 && memcmp(ref, out, n) != 0) { printf("  ROUNDTRIP MISMATCH\n"); rc = 2; }
            else printf("  ROUNDTRIP OK: %zu bytes\n", n);
            free(ref); free(out);
            return rc;
        }
        free(out);
        return 0;
    }

    /* RC mode: чтение кумулятивных частот cum[1..256] (256 значений).
       cum[0]=0 — константа, не хранится. */
    model_t m;
    m.is_rle = 0;
    m.cum[0] = 0;
    for (int i = 1; i <= ALPHABET; i++) {
        uint16_t c;
        if (read_u16_le(fin, &c) != 0) { fprintf(stderr, "read cum\n"); free(out); fclose(fin); return 1; }
        m.cum[i] = c;
    }
    m.total = m.cum[ALPHABET];

    /* Streaming: 16KB кольцевой буфер для сжатого потока */
    stream_t st;
    stream_init(&st, fin);
    stream_refill(&st);

    rc_dec_t rd;
    rd.range = 0xFFFFFFFFFFFFFFFFULL;
    rd.code = ((uint64_t)st.buf[1] << 32) | st.buf[2];
    st.buf_pos = 3;  /* первые 3 слова прочитаны */

    /* Декодирование — чистый замер.
       rc_dec_step читает 0 или 1 слово из in_buf.
       in_buf = &st.buf[st.buf_pos], после чтения buf_pos++. */
    double tdec0 = timer_sec();
    for (size_t i = 0; i < n; i++) {
        uint32_t cum = rc_dec_get_cum(&rd, TARGET_TOTAL);
        uint32_t cum_lo, freq;
        uint8_t sym = model_find(&m, cum, &cum_lo, &freq);
        out[i] = sym;

        /* rc_dec_step inline: читает 0 или 1 слово из streaming буфера */
        uint64_t t = rd.range / TARGET_TOTAL;
        uint64_t step = t * cum_lo;
        rd.code -= step;
        rd.range = t * freq;
        if (rd.range < SCHINDLER_BOTTOM_64) {
            if (st.buf_pos >= st.buf_len) stream_refill(&st);
            uint32_t new_dword = st.buf[st.buf_pos++];
            rd.code = (rd.code << 32) | new_dword;
            rd.range <<= 32;
        }
    }
    double tdec1 = timer_sec();

    fclose(fin);

    /* Запись — вне замера */
    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("fopen output"); free(out); return 1; }
    if (n > 0 && fwrite(out, 1, n, fout) != n) {
        fprintf(stderr, "fwrite\n"); fclose(fout); free(out); return 1;
    }
    fclose(fout);
    double ttotal1 = timer_sec();

    double dec_ms = (tdec1 - tdec0) * 1000.0;
    printf("DECODE v2 OK\n");
    printf("  input:   %s\n", argv[1]);
    printf("  decoded: %zu bytes\n", n);
    printf("  decode:  %.2f ms  (%.1f MB/s)\n",
           dec_ms, n > 0 ? (double)n / (dec_ms * 1048576.0 / 1000.0) : 0.0);
    printf("  total:   %.2f ms  (incl. I/O)\n", (ttotal1 - t0) * 1000.0);

    /* Verify */
    if (argc == 4) {
        FILE *fref = fopen(argv[3], "rb");
        if (!fref) { perror("fopen original"); free(out); return 1; }
        fseek(fref, 0, SEEK_END);
        long rs = ftell(fref); fseek(fref, 0, SEEK_SET);
        uint8_t *ref = malloc(rs > 0 ? (size_t)rs : 1);
        if (rs > 0 && fread(ref, 1, (size_t)rs, fref) != (size_t)rs) {
            fprintf(stderr, "read ref\n"); fclose(fref); free(ref); free(out); return 1;
        }
        fclose(fref);
        int rc = 0;
        if ((size_t)rs != n) { printf("  ROUNDTRIP MISMATCH: len\n"); rc = 2; }
        else if (n > 0 && memcmp(ref, out, n) != 0) {
            size_t pos = 0;
            while (pos < n && ref[pos] == out[pos]) pos++;
            printf("  ROUNDTRIP MISMATCH at %zu: orig=0x%02X dec=0x%02X\n", pos, ref[pos], out[pos]);
            rc = 2;
        } else printf("  ROUNDTRIP OK: %zu bytes\n", n);
        free(ref);
        free(out);
        return rc;
    }

    free(out);
    return 0;
}