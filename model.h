/* =========================================================================
 * STATIC 14-BIT FREQUENCY MODEL
 * =========================================================================
 *
 * Fixed cumulative frequencies, total = 2^14 = 16384.
 * The model is built ONCE over the entire input stream and is not updated
 * during coding (no adaptation needed).
 *
 * Cumulative frequencies are stored in uint16_t (14 bits suffice: 0..16384).
 *
 * Symbol lookup by cum value:
 *   - Default: direct LUT uint8_t[TARGET_TOTAL+1] (16385 bytes).
 *     O(1), a single cache line load. Ideal for the decoder.
 *   - If DISABLE_LUT is defined: 8-step branchless binary search
 *     (old implementation, 8 dependent loads from cums[257]).
 *
 * SCALING:
 *   raw_freq[i] * 16384 / total_raw -> scaled[i]
 *   If raw_freq[i] > 0 but scaled[i] == 0: scaled[i] = 1 (subtract from max).
 *   The sum of scaled is brought to 16384 by adjusting the maximum frequency.
 *
 * RLE:
 *   If only one symbol occurs (n_active <= 1) — range coding is pointless,
 *   use RLE.
 * ========================================================================= */

#ifndef MODEL_H
#define MODEL_H

#include <stdint.h>
#include <string.h>
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))

#define ALPHABET       256
#define TARGET_TOTAL   (1u << 14)   /* 16384 = 2^14 */

/* Cumulative frequencies: cum[0]=0, cum[256]=total.
   cum_lo(sym) = cum[sym], freq(sym) = cum[sym+1] - cum[sym].
   All in uint16_t — 14 bits suffice. */
typedef  uint16_t cums_t[ALPHABET + 1];

/* LUT: lut[cum] -> symbol s, the largest with cum[s] <= cum.
   Size = TARGET_TOTAL + 1 = 16385 bytes (~16 KB, fits in L1). */
typedef  uint8_t  lut_t[TARGET_TOTAL + 1];

/* Build the model from raw frequencies.
   raw_freq[256] — byte counters.
   n_active — number of symbols with raw_freq > 0.
   If n_active <= 1: is_rle=1. */
static inline int model_build(cums_t m, const uint32_t raw_freq[ALPHABET]) {
    memset(m, 0, sizeof(cums_t));
    /* Count active symbols and the total count */
    int n_active = 0;
    uint64_t total_raw = 0;
    int  first_sym = 0;
    int  max_sym = 0;
    uint32_t  max_val = 1;
    for (int i = 0; i < ALPHABET; i++) {
        if (raw_freq[i] > 0) {
            n_active++;
            if (n_active == 1) first_sym = (uint8_t)i;
            total_raw += raw_freq[i];
            if (raw_freq[i] > max_val) {
                max_val = raw_freq[i];
                max_sym = i;
            }
        }
    }
    if (n_active ==0) {
        return -1;
    }


    /* RLE: one or zero active symbols */
    if (n_active == 1) {
        m[0] = 1;
        m[1] = first_sym;
        return 1;
    }

    /* SCALING to 14 bits */
    int16_t scaled[ALPHABET];
    memset(scaled, 0, sizeof(scaled));
    int16_t sum = 0;


    for (int i = 0; i < ALPHABET; i++) {
        if (raw_freq[i] != 0) {
            scaled[i] = (uint16_t)( raw_freq[i] * (float)TARGET_TOTAL * (1.0f / (float)total_raw) );
            scaled[i] += scaled[i] == 0;
            sum += scaled[i];
        }
    }

    /* Adjust the sum to TARGET_TOTAL via the maximum frequency */
    scaled[max_sym] += TARGET_TOTAL - sum;

    /* Assemble the cumulative table (cast to uint16_t) */
    m[0] = 0;
    for (int i = 0; i < ALPHABET; i++) {
        m[i + 1] = m[i] + (uint16_t)scaled[i];
    }
    /* total = cum[ALPHABET], stored implicitly */
    return (m[ALPHABET] == TARGET_TOTAL) ? 0 : -2;

}

/* Build the LUT from a ready cumulative table.
   Called once after model_build (if LUT is not disabled).
   For an RLE model the LUT is not needed — it simply won't be used. */
static inline void model_build_lut(lut_t lut, const cums_t m) {
#ifndef DISABLE_LUT
    int s = 0;
    unsigned v;
    for (v = 0; v <= TARGET_TOTAL; v++) {
        while (s < ALPHABET - 1 && (int)m[s + 1] <= (int)v) s++;
        lut[v] = (uint8_t)s;
    }
#else
    (void)lut; (void)m;  /* unused */
#endif
}

/* Get cum_lo and freq for symbol sym. */
static inline void model_get(const cums_t m, uint8_t sym,
                             uint16_t *cum_lo, uint16_t *freq) {
    *cum_lo = m[sym];
    *freq  = m[sym + 1] - m[sym];
}

/* Per-symbol 64-bit reciprocal table for the division-free rANS encoder.
   rf[sym] = floor((2^64 - 1) / freq(sym)). With this magic number,
   (state * rf[sym]) >> 64  ==  state / freq  or  state / freq - 1,
   so the encoder needs at most one correction (see Rans64EncPut_no_div).
   Built once after model_build(); unused for RLE. */
typedef uint64_t rfreq_tab_t[ALPHABET];

static inline void model_build_rfreq(rfreq_tab_t rf, const cums_t m) {
    for (int i = 0; i < ALPHABET; i++) {
        uint16_t freq = (uint16_t)(m[i + 1] - m[i]);
        rf[i] = (freq == 0) ? 0 : (uint64_t)(0xFFFFFFFFFFFFFFFFULL / freq);
    }
}

/* Find the symbol by cumulative value cum (for the decoder).
   LUT version — O(1) direct read. */
static inline uint8_t model_find_lut(const lut_t lut, const cums_t m,
                                     uint16_t cum,
                                     uint16_t *cum_lo, uint16_t *freq) {
    uint8_t s = lut[cum];
    *cum_lo = m[s];
    *freq   = (uint16_t)(m[s + 1] - m[s]);
    return s;
}

/* Find the symbol by cumulative value cum (for the decoder).
   No-LUT version — 8-step branchless binary search (cmov). */
static inline uint8_t model_find_nolut(const cums_t m, uint16_t cum,
                                       uint16_t *cum_lo, uint16_t *freq) {
    const uint16_t* base = m;
    base += (cum >= base[128]) ? 128 : 0;
    base += (cum >= base[64]) ? 64 : 0;
    base += (cum >= base[32]) ? 32 : 0;
    base += (cum >= base[16]) ? 16 : 0;
    base += (cum >= base[8]) ? 8 : 0;
    base += (cum >= base[4]) ? 4 : 0;
    base += (cum >= base[2]) ? 2 : 0;
    base += (cum >= base[1]) ? 1 : 0;

    uint8_t s = (uint8_t)(base - m);
    *cum_lo = m[s];
    *freq  = (uint16_t)m[s + 1] - m[s];
    return s;
}

/* Generic model_find: selects the default implementation.
   If a LUT is available (non-NULL passed) — it is used.
   Otherwise fallback to binary search. */
static inline uint8_t model_find(const cums_t m, uint16_t cum,
                                  uint16_t *cum_lo, uint16_t *freq) {
#ifdef DISABLE_LUT
    return model_find_nolut(m, cum, cum_lo, freq);
#else
    /* This variant is called when the caller does not pass a LUT.
       Fall back to binary search — slower, but compatible with the old API.
       New code should use model_find_lut(). */
    return model_find_nolut(m, cum, cum_lo, freq);
#endif
}

#endif /* MODEL_H */
