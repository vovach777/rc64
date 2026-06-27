/* =========================================================================
 * RANS ENCODE — 64-bit rANS, 14-bit static model
 * =========================================================================
 *
 * Integration of the user rANS codec (see rans_codec.h) into the rc64 project format.
 *
 * Usage:
 *   rans_encode <input_file> <output_file.rans>
 *
 * .rans format:
 *   [4 bytes]  signature: 'r','n', flags, rle_sym
 *              flags bit 0: is_rle
 *   [8 bytes]  uint64_t original_len (LE)
 *   --- RLE mode (is_rle=1) ---
 *              (nothing else — rle_sym is in the signature)
 *   --- rANS mode (is_rle=0) ---
 *   [512 bytes] cum[1..256] — uint16_t LE (cumulative frequencies, 14-bit)
 *              cum[0]=0 is not stored (always a constant)
 *   then consecutive blocks (fixed INPUT size BLOCK_BYTES):
 *     [4 bytes]  uint32_t flush_lo  = LOW32  of the block's final state (LE)
 *     [4 bytes]  uint32_t flush_hi  = HIGH32 of the block's final state (LE)
 *     [4*K]      uint32_t renorm words of the block, REVERSED (in the order the
 *                decoder consumes them) — the decoder reads them FORWARD
 *   The number of renorm words per block (K) IS NOT STORED IN THE STREAM — the
 *   decoder consumes them inline as needed and after block_syms symbols arrives
 *   at the next flush pair by itself.
 *
 * Header: 4 + 8 + 512 = 524 bytes (same as .rc / .rc32).
 *
 * Block scheme (lightening the decoder — all reversing is on the encoder):
 *   - Each block = an INDEPENDENT rANS: its own state = RANS64_L at the start,
 *     its own flush pair at the end. State is not carried across blocks.
 *   - Block traversal is FORWARD (block 0 = start of input, block 1 = next slice,
 *     ...). Within a block the INPUT is traversed BACKWARD (rANS LIFO): from
 *     a_hi-1 down to a_lo.
 *   - The block's renorm words are written into the buffer FROM THE END backward
 *     (buf[--buf_head]) — then they are already in decoder-consumption order
 *     (last emitted first), no separate buffer flip is needed. The slack (+1024)
 *     stays untouched AT THE FRONT and guarantees buf_head never goes negative.
 *   - The INPUT block size is fixed (BLOCK_BYTES), so boundaries are known in
 *     advance — a SINGLE pass, no pre-pass and no seek. On huge files this saves
 *     memory: a ~450 KB buffer (renorm ceiling per block) instead of ~N.
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

#define RANS_SCALE_BITS  14u   /* matches TARGET_TOTAL = 2^14 from model.h */
#define BLOCK_BYTES      (256 * 1024)   /* fixed INPUT block size */

/* Ceiling of renorm words per block (proven and confirmed empirically):
   renorm_count = (31 + n·H_cross − log2(state_final))/32, where
   H_cross is the cross-entropy (model q vs data p), H_cross ≤ 14 bpb
   (max code length at freq=1), state_final ≥ RANS64_L (invariant).
   => renorm_count ≤ BLOCK_BYTES*14/32 = 114688 (reached on adversarial
   with freq=1). +1024 slack. Buffer ~450 KB — fits in 512 KB L2. */
constexpr uint32_t RENORM_BUF_WORDS = (BLOCK_BYTES * 14u / 32u + 1024 +0xff) & ~0xff;

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input_file> <output_file.rans>\n", argv[0]);
        return 1;
    }

    /* --- Reading the input file (outside the timed region) --- */
    zpl_file fin;
    if (zpl_file_open(&fin, argv[1])) {
        fprintf(stderr, "fopen input\n");
        return 1;
    }

    zpl_i64 fsize = zpl_file_size(&fin);
    if (fsize <= 0) {
        fprintf(stderr, "file_size failed\n");
        zpl_file_close(&fin);
        return 1;
    }
    uint64_t n = (uint64_t)fsize;

    uint8_t *data = (uint8_t *)malloc((size_t)n);
    if (!data) {
        fprintf(stderr, "malloc data\n");
        zpl_file_close(&fin);
        return 1;
    }

    {
        uint8_t *p = data;
        int64_t remaining = (int64_t)n;
        while (remaining > 0) {
            uint32_t chunk = (uint32_t)zpl_min(remaining, 0x1000000LL);
            if (!zpl_file_read(&fin, p, chunk)) {
                fprintf(stderr, "read error\n");
                zpl_file_close(&fin);
                free(data);
                return 1;
            }
            p += chunk;
            remaining -= chunk;
        }
    }
    zpl_file_close(&fin);

    /* --- Frequency counting and model building (outside the timed region) --- */
    uint32_t raw_freq[ALPHABET];
    memset(raw_freq, 0, sizeof(raw_freq));
    for (size_t i = 0; i < n; i++) raw_freq[data[i]]++;

    cums_t m;
    int is_rle = model_build(m, raw_freq);
    if (is_rle < 0) {
        fprintf(stderr, "model_build failed\n");
        free(data);
        return 1;
    }

    /* --- Opening the output file and writing the header (outside the timed region) --- */
    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) {
        fprintf(stderr, "fopen output\n");
        free(data);
        return 1;
    }

    uint8_t flags = !!m[0];
    uint8_t rle_sym = (uint8_t)m[1];
    uint8_t sig[4] = { 'r', 'n', flags, rle_sym };
    if (!zpl_file_write(&fout, sig, sizeof(sig))) {
        fprintf(stderr, "write sig\n");
        zpl_file_close(&fout); free(data); return 1;
    }
    if (!zpl_file_write(&fout, &n, sizeof(n))) {
        fprintf(stderr, "write len\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- CPU frequency calibration --- */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100;
    zpl_u64 dddd = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 zpl_rdtsc_freq = (zpl_rdtsc() - dddd) * 10;
    printf("CPU freq = %1.1f Mhz\n", zpl_rdtsc_freq / 1000000.0f);

    /* --- RLE mode --- */
    if (is_rle) {
        zpl_file_close(&fout);
        printf("ENCODE-RANS OK (RLE, sym=0x%02X)\n", rle_sym);
        printf("  in:  %" PRIu64 " bytes\n", n);
        printf("  out: 12 bytes\n");
        free(data);
        return 0;
    }

    /* --- rANS mode: writing cum[1..256] --- */
    if (!zpl_file_write(&fout, m + 1, sizeof(m[0]) * ALPHABET)) {
        fprintf(stderr, "write cum\n");
        zpl_file_close(&fout); free(data); return 1;
    }

    /* --- Division-free encoder: precompute the 64-bit reciprocal per symbol. */
    rfreq_tab_t rfreq_tab;
    model_build_rfreq(rfreq_tab, m);

    /* --- renorm buffer for ONE block (reused). Size is fixed — the proven
       ceiling of renorm words per block (see RENORM_BUF_WORDS), hence
       static: no malloc/free, lives in BSS. --- */
    static uint32_t buf[RENORM_BUF_WORDS];

    /* --- Encoding: FORWARD pass over blocks, BACKWARD within a block (LIFO).
       Timed: only the Rans64EncPut loop, excluding I/O and the flip. --- */
    rANS::Encoder enc;
    zpl_u64 total_ticks = 0;
    size_t total_renorm = 0;
    size_t blocks = 0;
    size_t i = 0;   /* how many input bytes have been encoded (from the start) */
    auto remaining = n;

    while (remaining) {
        enc = rANS::Encoder{};   /* fresh independent state = RANS64_L */
        const size_t block_syms = zpl_min((size_t)BLOCK_BYTES, remaining);
        auto buf_head = buf +  RENORM_BUF_WORDS;
        zpl_u64 t0 = zpl_rdtsc();
        for (int k=block_syms-1; k >= 0; --k) {
            uint16_t cum_lo, freq;
            model_get(m, data[i+k], &cum_lo, &freq);
            auto res = enc.Rans64EncPut_no_div(cum_lo, freq, rfreq_tab[data[i+k]], RANS_SCALE_BITS);
            if (res) {
                assert(buf_head > buf+16 && "renorm buffer overflow");
                // if (buf_head == 0) {
                //     fprintf(stderr, "renorm buffer overflow\n");
                //     zpl_file_close(&fout); free(data); return 1;
                // }
                *(--buf_head) = *res;
            }
        }
        total_ticks += zpl_rdtsc() - t0;
        auto flush_pair = enc.flush();
        *(--buf_head) = flush_pair.first;
        *(--buf_head) = flush_pair.second;

        i += block_syms;
        remaining -= block_syms;

        size_t renorm_count = buf + RENORM_BUF_WORDS - buf_head;

        assert(renorm_count > 0);
        if (!zpl_file_write(&fout, buf_head, sizeof(buf[0]) * renorm_count)) {
            fprintf(stderr, "write renorm\n");
            zpl_file_close(&fout); free(data); return 1;
        }

        total_renorm += renorm_count;
        blocks++;
        printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)",
               (uint64_t)i, n, 100.0 * (double)i / (double)n);
        fflush(stdout);
    }
    printf("\n");

    zpl_i64 out_bytes = zpl_file_size(&fout);
    zpl_file_close(&fout);
    free(data);

    /* --- Report --- */
    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;
    uint64_t total_enc_time_ms = (zpl_rdtsc_freq > 0)
        ? (uint64_t)(total_ticks * 1000 / zpl_rdtsc_freq)
        : 0;

    double ratio = (n > 0) ? (double)out_bytes / (double)n * 100.0 : 0.0;
    double bpb   = (n > 0) ? (double)out_bytes * 8.0 / (double)n : 0.0;
    double mb_s  = (total_enc_time_ms > 0)
        ? (double)n / (total_enc_time_ms * 1048576.0 / 1000.0)
        : 0.0;

    printf("ENCODE-RANS OK\n");
    printf("  engine:   64-bit rANS, 32-bit renorm, scale_bits=%u\n", RANS_SCALE_BITS);
    printf("  input:    %s\n", argv[1]);
    printf("  in_size:  %" PRIu64 " bytes\n", n);
    printf("  out_size: %" PRIi64 " bytes\n", out_bytes);
    printf("  blocks:   %zu  (block_bytes=%d)\n", blocks, BLOCK_BYTES);
    printf("  renorm:   %zu words\n", total_renorm);
    printf("  ratio:    %.2f%%  (%.3f bits/byte)\n", ratio, bpb);
    printf("  encode:   %" PRIu64 " ms  (%.1f MB/s)\n", total_enc_time_ms, mb_s);
    printf("          : %u ticks per symbol\n", ticks_per_symbol);
    return 0;
}