/* =========================================================================
 * RC DECODE v3 — Subbotin magic (64-bit, 16-bit shift, aligned trim)
 * =========================================================================
 *
 * Usage:
 *   rc_decode <input_file.rc> <output_file> [original_file]
 * ========================================================================= */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "rc_codec.h"
#include "model.h"

static int read_u16_le(FILE *f, uint16_t *v) {
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return -1;
    *v = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return 0;
}

static uint16_t read_u16_be_from(const uint8_t b[2]) {
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input.rc> <output> [original]\n", argv[0]);
        return 1;
    }

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("fopen input"); return 1; }

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

    double t0 = (double)clock() / CLOCKS_PER_SEC;

    uint8_t *out = malloc(n > 0 ? n : 1);
    if (!out) { fprintf(stderr, "malloc\n"); fclose(fin); return 1; }

    if (is_rle) {
        memset(out, rle_sym, n);
        fclose(fin);
        FILE *fout = fopen(argv[2], "wb");
        if (!fout) { perror("fopen output"); free(out); return 1; }
        if (n > 0 && fwrite(out, 1, n, fout) != n) {
            fprintf(stderr, "fwrite\n"); fclose(fout); free(out); return 1;
        }
        fclose(fout);
        double t1 = (double)clock() / CLOCKS_PER_SEC;
        printf("DECODE v3 OK (RLE, sym=0x%02X)\n", rle_sym);
        printf("  decoded: %zu bytes\n", n);
        printf("  time:    %.2f ms\n", (t1 - t0) * 1000.0);

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
            free(ref);
            free(out);
            return rc;
        }
        free(out);
        return 0;
    }

    /* RC mode: чтение частот */
    model_t m;
    m.is_rle = 0;
    m.cum[0] = 0;
    for (int i = 0; i < ALPHABET; i++) {
        uint16_t f;
        if (read_u16_le(fin, &f) != 0) { fprintf(stderr, "read freq\n"); free(out); fclose(fin); return 1; }
        m.cum[i + 1] = m.cum[i] + f;
    }
    m.total = m.cum[ALPHABET];

    /* Чтение потока uint16 (big-endian) */
    long header_sz = 4 + 8 + ALPHABET * 2;
    fseek(fin, 0, SEEK_END);
    long fend = ftell(fin);
    fseek(fin, header_sz, SEEK_SET);
    long body_bytes = fend - header_sz;
    if (body_bytes < 0) body_bytes = 0;
    size_t nwords = (size_t)(body_bytes / 2);
    uint16_t *words = malloc((nwords + 16) * sizeof(uint16_t));
    if (!words) { fprintf(stderr, "malloc words\n"); free(out); fclose(fin); return 1; }

    for (size_t i = 0; i < nwords; i++) {
        uint8_t b[2];
        if (fread(b, 1, 2, fin) != 2) {
            fprintf(stderr, "read word %zu\n", i); free(words); free(out); fclose(fin); return 1;
        }
        words[i] = read_u16_be_from(b);
    }
    fclose(fin);
    for (size_t i = nwords; i < nwords + 16; i++) words[i] = 0;

    /* Декодирование */
    folk_dec_t rd;
    folk_dec_init(&rd, words);
    size_t wi = 4;  /* init read 4 words */

    for (size_t i = 0; i < n; i++) {
        uint32_t cum = folk_dec_get_cum(&rd, m.total);
        uint32_t cum_lo, freq;
        uint8_t sym = model_find(&m, cum, &cum_lo, &freq);
        out[i] = sym;
        wi += folk_dec_step(&rd, &words[wi], cum_lo, freq, m.total);
    }

    double t1 = (double)clock() / CLOCKS_PER_SEC;

    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("fopen output"); free(out); free(words); return 1; }
    if (n > 0 && fwrite(out, 1, n, fout) != n) {
        fprintf(stderr, "fwrite\n"); fclose(fout); free(out); free(words); return 1;
    }
    fclose(fout);

    printf("DECODE v3 OK\n");
    printf("  input:   %s (%zu words)\n", argv[1], nwords);
    printf("  decoded: %zu bytes\n", n);
    printf("  time:    %.2f ms  (%.1f MB/s)\n",
           (t1 - t0) * 1000.0,
           n > 0 ? (double)n / ((t1 - t0) * 1048576.0) : 0.0);

    if (argc == 4) {
        FILE *fref = fopen(argv[3], "rb");
        if (!fref) { perror("fopen original"); free(out); free(words); return 1; }
        fseek(fref, 0, SEEK_END);
        long rs = ftell(fref); fseek(fref, 0, SEEK_SET);
        uint8_t *ref = malloc(rs > 0 ? (size_t)rs : 1);
        if (rs > 0 && fread(ref, 1, (size_t)rs, fref) != (size_t)rs) {
            fprintf(stderr, "read ref\n"); fclose(fref); free(ref); free(out); free(words); return 1;
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
        free(out); free(words);
        return rc;
    }

    free(out); free(words);
    return 0;
}