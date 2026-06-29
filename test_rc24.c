/* RC24 PoC — original Subbotin carryless rangecoder (1999), verbatim core.
   TOP=2^24, BOT=2^16, 32-bit state, 8-bit shift, carry-free trim. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "model_12.h"

#define TOP (1u<<24)
#define BOT (1u<<16)

/* ---- ENCODER (verbatim Subbotin) ---- */
typedef struct { uint32_t low, range; } rc_enc_t;

static inline void rc_enc_init(rc_enc_t *e) { e->low = 0; e->range = 0xFFFFFFFFu; }

static inline int rc_enc_step(rc_enc_t *e, uint8_t *out,
                              uint32_t cumFreq, uint32_t freq, uint32_t totFreq) {
    int n = 0;
    e->low  += cumFreq * (e->range /= totFreq);
    e->range *= freq;
    while ((e->low ^ (e->low + e->range)) < TOP ||
           (e->range < BOT && ((e->range = -e->low & (BOT-1)), 1))) {
        out[n++] = (uint8_t)(e->low >> 24);
        e->range <<= 8;
        e->low   <<= 8;
    }
    return n;
}

static inline int rc_enc_flush(rc_enc_t *e, uint8_t *out) {
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)(e->low >> 24);
        e->low <<= 8;
    }
    return 4;
}

/* ---- DECODER (verbatim Subbotin) ---- */
typedef struct { uint32_t low, range, code; } rc_dec_t;

static inline void rc_dec_init(rc_dec_t *d, const uint8_t *in) {
    d->low = 0; d->range = 0xFFFFFFFFu; d->code = 0;
    for (int i = 0; i < 4; i++)
        d->code = (d->code << 8) | in[i];
}

static inline uint32_t rc_dec_get_freq(rc_dec_t *d, uint32_t totFreq) {
    return (d->code - d->low) / (d->range /= totFreq);
}

static inline int rc_dec_step(rc_dec_t *d, const uint8_t *in,
                              uint32_t cumFreq, uint32_t freq, uint32_t totFreq) {
    int n = 0;
    d->low  += cumFreq * d->range;   /* range already /= totFreq in get_freq */
    d->range *= freq;
    while ((d->low ^ (d->low + d->range)) < TOP ||
           (d->range < BOT && ((d->range = -d->low & (BOT-1)), 1))) {
        d->code  = (d->code << 8) | in[n++];
        d->range <<= 8;
        d->low   <<= 8;
    }
    return n;
}

/* ---- roundtrip test ---- */
static int test_dataset(const char *name, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *data = malloc(n); fread(data, 1, n, f); fclose(f);

    uint32_t raw[256] = {0};
    for (long i = 0; i < n; i++) raw[data[i]]++;
    model12_t M;
    if (model12_build(&M, raw) != 0) { printf("  %-8s: RLE\n", name); free(data); return 1; }

    /* encode */
    uint8_t *comp = malloc(n + 4096);
    size_t cl = 0;
    rc_enc_t enc;
    rc_enc_init(&enc);
    for (long i = 0; i < n; i++) {
        uint16_t clo, fq; model12_get(&M, data[i], &clo, &fq);
        uint8_t buf[8];
        int nw = rc_enc_step(&enc, buf, clo, fq, 4096);
        for (int j = 0; j < nw; j++) comp[cl++] = buf[j];
    }
    { uint8_t buf[4]; int nw = rc_enc_flush(&enc, buf);
      for (int j = 0; j < nw; j++) comp[cl++] = buf[j]; }

    /* decode */
    uint8_t *dec = malloc(n);
    rc_dec_t dstate;
    rc_dec_init(&dstate, comp);
    size_t ci = 4;  /* skip 4 init bytes */
    for (long i = 0; i < n; i++) {
        uint32_t cum = rc_dec_get_freq(&dstate, 4096);
        if (cum >= 4096) cum = 4095;
        uint16_t clo, fq; uint8_t sym = model12_find(&M, (uint16_t)cum, &clo, &fq);
        dec[i] = sym;
        int nc = rc_dec_step(&dstate, comp + ci, clo, fq, 4096);
        ci += nc;
    }

    int ok = (memcmp(dec, data, n) == 0);
    printf("  %-8s: %ld -> %zu  (%.2f%%, %.3f bpb)  %s\n",
           name, n, cl, 100.0*cl/n, 8.0*cl/n, ok ? "OK" : "*** FAIL ***");
    free(comp); free(dec); free(data);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    printf("=== RC24 Subbotin carryless (TOP=2^24, BOT=2^16) - PoC roundtrip ===\n\n");
    if (argc < 7) { fprintf(stderr, "usage: %s d0..d5\n", argv[0]); return 1; }
    const char *names[] = {"lorem","ccode","english","russian","repeat","random"};
    int fails = 0;
    for (int i = 0; i < 6; i++) fails += test_dataset(names[i], argv[i+1]);
    printf("\n%s\n", fails ? "*** FAIL ***" : "=== ALL PASSED ===");
    return fails ? 1 : 0;
}
