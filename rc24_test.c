/* Test RC24 with 12-bit frequency total (4096) on random synthetic data. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <immintrin.h>

#define R24_TOTAL  4096u
#define R24_TOP    (1u << 16)
#define R24_BOT    (1u << 16)
#define R24_MASK   0xFFFFFFu

typedef struct { uint32_t low, range; uint8_t *out; size_t cap, pos; } r24_enc_t;
static inline void r24_enc_init(r24_enc_t *e, uint8_t *out, size_t cap) {
    e->low = 0; e->range = R24_MASK; e->out = out; e->cap = cap; e->pos = 0;
}
static inline void r24_enc_step(r24_enc_t *e, uint32_t cumFreq, uint32_t freq) {
    e->low  += cumFreq * (e->range /= R24_TOTAL);
    e->range *= freq;
    while ((e->low ^ (e->low + e->range)) < R24_TOP ||
           (e->range < R24_BOT && ((e->range = -e->low & (R24_BOT - 1)), 1))) {
        if (e->pos < e->cap) e->out[e->pos] = (uint8_t)(e->low >> 16);
        e->pos++;
        e->range <<= 8;
        e->low   = (e->low << 8) & R24_MASK;
    }
}
static inline void r24_enc_flush(r24_enc_t *e) {
    for (int i = 0; i < 3; i++) {
        if (e->pos < e->cap) e->out[e->pos] = (uint8_t)(e->low >> 16);
        e->pos++;
        e->low <<= 8;
    }
}
static inline size_t r24_enc_size(const r24_enc_t *e) { return e->pos; }

typedef struct { uint32_t code, range, low; const uint8_t *in; size_t len, pos; } r24_dec_t;
static inline void r24_dec_init(r24_dec_t *d, const uint8_t *in, size_t len) {
    d->in = in; d->len = len; d->pos = 0;
    d->range = R24_MASK; d->low = 0; d->code = 0;
    for (int i = 0; i < 3; i++)
        d->code = ((d->code << 8) | (uint32_t)((d->pos < d->len) ? d->in[d->pos++] : 0)) & R24_MASK;
}
static inline uint32_t r24_dec_get_cum(r24_dec_t *d) {
    d->range >>= 12;                              /* 12-bit TOTAL = 4096 */
    uint32_t diff = d->code - d->low;
    assert(diff <= R24_MASK);
    __m128 fr  = _mm_set_ss((float)d->range);
    __m128 rcp = _mm_rcp_ss(fr);
    __m128 tmp = _mm_sub_ss(_mm_set_ss(2.0f), _mm_mul_ss(fr, rcp));
    rcp        = _mm_mul_ss(rcp, tmp);
    __m128 fd  = _mm_set_ss((float)diff);
    __m128 fq  = _mm_mul_ss(fd, rcp);
    uint32_t q = (uint32_t)_mm_cvttss_si32(fq);
    q += (uint32_t)((uint64_t)(q + 1) * d->range <= diff);
    return q;
}
static inline void r24_dec_step(r24_dec_t *d, uint32_t cumFreq, uint32_t freq) {
    d->low   += cumFreq * d->range;
    d->range *= freq;
    while ((d->low ^ (d->low + d->range)) < R24_TOP ||
           (d->range < R24_BOT && ((d->range = -d->low & (R24_BOT - 1)), 1))) {
        d->code  = ((d->code << 8) | (uint32_t)((d->pos < d->len) ? d->in[d->pos++] : 0)) & R24_MASK;
        d->range <<= 8;
        d->low   = (d->low << 8) & R24_MASK;
    }
}

static uint32_t find_sym(const uint32_t *cum, const uint32_t *freq, uint32_t n, uint32_t q) {
    for (uint32_t i = 0; i < n; i++)
        if (q >= cum[i] && q < cum[i] + freq[i]) return i;
    return n - 1;
}

static int test_r24_12bit_total(unsigned seed, uint32_t nsym, uint32_t n, uint8_t *buf, size_t bufsz) {
    static uint32_t cum[4096], freq[4096];
    srand(seed);
    uint32_t k = 2 + (rand() % (nsym - 1));
    static uint32_t w[4096];
    for (uint32_t i = 0; i < k; i++) w[i] = 1 + (rand() % 64);
    uint32_t sum = 0; for (uint32_t i = 0; i < k; i++) sum += w[i];
    uint32_t acc = 0;
    for (uint32_t i = 0; i < k; i++) {
        uint32_t f = (uint32_t)((uint64_t)w[i] * R24_TOTAL / sum); if (f == 0) f = 1;
        freq[i] = f; cum[i] = acc; acc += f;
    }
    if (acc != R24_TOTAL) freq[k-1] += (R24_TOTAL - acc);

    static uint16_t sym[200000];
    srand(seed ^ 0x5A5A5A5Au);
    for (uint32_t i = 0; i < n; i++) sym[i] = (uint16_t)(rand() % k);

    r24_enc_t e; r24_enc_init(&e, buf, bufsz);
    for (uint32_t i = 0; i < n; i++) r24_enc_step(&e, cum[sym[i]], freq[sym[i]]);
    r24_enc_flush(&e);
    size_t sz = r24_enc_size(&e);

    r24_dec_t d; r24_dec_init(&d, buf, sz);
    uint32_t errs = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t q = r24_dec_get_cum(&d);
        uint32_t s = find_sym(cum, freq, k, q);
        r24_dec_step(&d, cum[s], freq[s]);
        if (s != sym[i]) {
            if (errs < 3) printf("    [R24-12bit] mismatch i=%u orig=%u dec=%u q=%u range=%u diff=%u\n",
                                 i, sym[i], s, q, d.range, d.code - d.low);
            errs++;
        }
    }
    if (errs) printf("  [FAIL] R24+TOTAL=4096 seed=%u k=%u n=%u errs=%u\n", seed, k, n, errs);
    else      printf("  [OK]   R24+TOTAL=4096 seed=%u k=%u n=%u enc=%zu (%.2f bps)\n",
                     seed, k, n, sz, (double)sz*8/n);
    return errs == 0;
}

int main(void) {
    static uint8_t buf[2000000];
    int total = 0, passed = 0;
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("=================================================================\n");
    printf("Test: RC24 with 12-bit TOTAL (4096)\n");
    printf("=================================================================\n");
    for (unsigned s = 1; s <= 20; s++) {
        total++; passed += test_r24_12bit_total(s, 256, 5000, buf, sizeof(buf));
    }
    /* skewed */
    {
        uint32_t cum[3] = {0, 4000, 4095};
        uint32_t freq[3] = {4000, 95, 1};
        r24_enc_t e; r24_enc_init(&e, buf, sizeof(buf));
        srand(77);
        static uint16_t sym[30000];
        for (int i = 0; i < 30000; i++) sym[i] = (rand() % 100 == 0) ? 1 : 0;
        for (int i = 0; i < 30000; i++) r24_enc_step(&e, cum[sym[i]], freq[sym[i]]);
        r24_enc_flush(&e);
        size_t sz = r24_enc_size(&e);
        r24_dec_t d; r24_dec_init(&d, buf, sz);
        int errs = 0;
        for (int i = 0; i < 30000; i++) {
            uint32_t q = r24_dec_get_cum(&d);
            uint32_t s = (q < 4000) ? 0 : (q < 4095 ? 1 : 2);
            r24_dec_step(&d, cum[s], freq[s]);
            if (s != sym[i]) errs++;
        }
        total++;
        if (errs == 0) { passed++; printf("  [OK]   R24+TOTAL=4096 skewed 30k (enc=%zu)\n", sz); }
        else            printf("  [FAIL] R24+TOTAL=4096 skewed 30k: %d errors\n", errs);
    }

    printf("\n=================================================================\n");
    printf("Total: %d / %d passed\n", passed, total);
    printf("=================================================================\n");
    return (passed == total) ? 0 : 1;
}
