/* =========================================================================
 * STATIC 12-BIT FREQUENCY MODEL (for the 32-bit range coder)
 * =========================================================================
 *
 * Fixed cumulative frequencies, total = 2^12 = 4096.
 * The model is built ONCE over the entire input stream and is not updated
 * during coding (no adaptation needed).
 *
 * Cumulative frequencies are stored in uint16_t (12 bits suffice: 0..4096).
 *
 * Symbol lookup by cum value:
 *   - Default: direct LUT uint8_t[TARGET_TOTAL_12+1] (4097 bytes).
 *     O(1), a single cache line load. Ideal for the decoder.
 *   - If DISABLE_LUT is defined: 8-step branchless binary search
 *     (old implementation, 8 dependent loads from cums[257]).
 *
 * SCALING:
 *   raw_freq[i] * 4096 / total_raw -> scaled[i]
 *   If raw_freq[i] > 0 but scaled[i] == 0: scaled[i] = 1 (subtract from max).
 *   The sum of scaled is brought to 4096 by adjusting the maximum frequency.
 *
 * RLE:
 *   If only one symbol occurs (n_active <= 1) — range coding is pointless,
 *   use RLE.
 * ========================================================================= */

#ifndef MODEL_12_H
#define MODEL_12_H

#include <stdint.h>
#include <string.h>

#define ALPHABET_12         256
#define TARGET_TOTAL_12     (1u << 12)   /* 4096 = 2^12 */

/* Cumulative frequencies: cum[0]=0, cum[256]=total.
   cum_lo(sym) = cum[sym], freq(sym) = cum[sym+1] - cum[sym].
   All in uint16_t — 12 bits suffice. */
typedef uint16_t cums12_t[ALPHABET_12 + 1];

/* LUT: lut[cum] -> symbol s, the largest with cum[s] <= cum.
   Size = TARGET_TOTAL_12 + 1 = 4097 bytes. */
typedef uint8_t  lut12_t[TARGET_TOTAL_12 + 1];

/* Full model: cums + optional LUT.
   The LUT is built only if DISABLE_LUT is not defined.
   If the LUT is disabled — the lut field remains unused (we do not make it
   zero-sized via conditional compilation here — the structure is always the
   same to keep the ABI stable; saving 4 KB of memory is negligible). */
typedef struct {
    cums12_t cums;
    lut12_t  lut;
} model12_t;

/* Build the model from raw frequencies.
   raw_freq[256] — byte counters.
   Returns:
      1  — RLE (only one active symbol, in cums[1] = first_sym)
      0  — OK (full model, LUT built)
     -1  — empty stream (n_active == 0)
     -2  — internal error (sum did not converge) */
static inline int model12_build(model12_t *M,
                                const uint32_t raw_freq[ALPHABET_12]) {
    cums12_t m;
    memset(m, 0, sizeof(cums12_t));
    memset(M->cums, 0, sizeof(cums12_t));
    int n_active = 0;
    uint64_t total_raw = 0;
    int  first_sym = 0;
    int  max_sym = 0;
    uint32_t max_val = 1;
    int i;

    for (i = 0; i < ALPHABET_12; i++) {
        if (raw_freq[i] > 0) {
            n_active++;
            if (n_active == 1) first_sym = i;
            total_raw += raw_freq[i];
            if (raw_freq[i] > max_val) {
                max_val = raw_freq[i];
                max_sym = i;
            }
        }
    }
    if (n_active == 0) {
        return -1;
    }

    /* RLE: one active symbol */
    if (n_active == 1) {
        m[0] = 0;
        m[1] = (uint16_t)first_sym;
        memcpy(M->cums, m, sizeof(cums12_t));
        return 1;
    }

    /* SCALING to 12 bits */
    uint16_t scaled[ALPHABET_12];
    memset(scaled, 0, sizeof(scaled));
    uint32_t sum = 0;

    for (i = 0; i < ALPHABET_12; i++) {
        if (raw_freq[i] != 0) {
            uint32_t s = (uint32_t)(((uint64_t)raw_freq[i] * TARGET_TOTAL_12)
                                    / total_raw);
            if (s == 0) s = 1;
            scaled[i] = (uint16_t)s;
            sum      += s;
        }
    }

    if (sum > TARGET_TOTAL_12) {
        uint32_t over = sum - TARGET_TOTAL_12;
        if (over <= scaled[max_sym]) {
            scaled[max_sym] = (uint16_t)(scaled[max_sym] - over);
        } else {
            scaled[max_sym] = 1;
            over -= (uint16_t)(scaled[max_sym] - 1);
            int safety = 1000000;
            while (over > 0 && safety-- > 0) {
                int kmax = -1;
                for (i = 0; i < ALPHABET_12; i++) {
                    if (raw_freq[i] != 0 && scaled[i] > 1) {
                        if (kmax < 0 || scaled[i] > scaled[kmax]) kmax = i;
                    }
                }
                if (kmax < 0) break;
                scaled[kmax]--;
                over--;
            }
        }
    } else if (sum < TARGET_TOTAL_12) {
        scaled[max_sym] = (uint16_t)(scaled[max_sym] +
                                     (TARGET_TOTAL_12 - sum));
    }

    /* Assemble the cumulative table */
    m[0] = 0;
    for (i = 0; i < ALPHABET_12; i++) {
        m[i + 1] = (uint16_t)(m[i] + scaled[i]);
    }
    if (m[ALPHABET_12] != TARGET_TOTAL_12) {
        memcpy(M->cums, m, sizeof(cums12_t));
        return -2;
    }
    memcpy(M->cums, m, sizeof(cums12_t));

#ifndef DISABLE_LUT
    /* Build the LUT: for each cum value v find the largest symbol s
       with cum[s] <= v. Done in a single pass: walk v from 0 to TARGET_TOTAL,
       advancing s while cum[s+1] <= v. */
    {
        int s = 0;
        unsigned v;
        for (v = 0; v <= TARGET_TOTAL_12; v++) {
            while (s < ALPHABET_12 - 1 && (int)m[s + 1] <= (int)v) s++;
            M->lut[v] = (uint8_t)s;
        }
    }
#else
    memset(M->lut, 0, sizeof(lut12_t));
#endif

    return 0;
}

/* Get cum_lo and freq for symbol sym. */
static inline void model12_get(const model12_t *M, uint8_t sym,
                               uint16_t *cum_lo, uint16_t *freq) {
    *cum_lo = M->cums[sym];
    *freq   = (uint16_t)(M->cums[sym + 1] - M->cums[sym]);
}

/* Find the symbol by cumulative value cum (for the decoder). */
static inline uint8_t model12_find(const model12_t *M, uint16_t cum,
                                   uint16_t *cum_lo, uint16_t *freq) {
    uint8_t s;
#ifdef DISABLE_LUT
    /* 8-step branchless binary search (cmov). */
    const uint16_t *m = M->cums;
    const uint16_t *base = m;
    base += (cum >= base[128]) ? 128 : 0;
    base += (cum >= base[64])  ? 64  : 0;
    base += (cum >= base[32])  ? 32  : 0;
    base += (cum >= base[16])  ? 16  : 0;
    base += (cum >= base[8])   ? 8   : 0;
    base += (cum >= base[4])   ? 4   : 0;
    base += (cum >= base[2])   ? 2   : 0;
    base += (cum >= base[1])   ? 1   : 0;
    s = (uint8_t)(base - m);
#else
    /* Direct LUT: a single cache line load. */
    s = M->lut[cum];
#endif
    *cum_lo = M->cums[s];
    *freq   = (uint16_t)(M->cums[s + 1] - M->cums[s]);
    return s;
}

#endif /* MODEL_12_H */
