/* =========================================================================
 * RANS DECODE — 64-bit rANS, 14-bit static model
 * =========================================================================
 *
 * The decoder loads the entire renorm stream into memory (malloc), then walks
 * the blocks FORWARD with the words_p pointer. renorm within a block is read
 * FORWARD as it is consumed (the encoder flipped the buffer); the number of
 * renorm words per block is NOT stored — after decoding block_syms symbols the
 * pointer lands on the next block's flush pair by itself (rANS symmetry: encoder
 * emits K ⟺ decoder consumes K).
 *
 * Usage:
 *   rans_decode <input_file.rans> <output_file>
 *
 * .rans stream (see rans_encode.cpp):
 *   [4]    sig 'r','n', flags, rle_sym
 *   [8]    uint64_t original_len (LE)
 *   [512]  cum[1..256] uint16_t LE
 *   then consecutive blocks:
 *     [4]    uint32_t flush_lo  = LOW32  of the block's state (LE)
 *     [4]    uint32_t flush_hi  = HIGH32 of the block's state (LE)
 *     [4*K]  renorm uint32_t LE — IN CONSUMPTION ORDER (read FORWARD)
 *
 * The INPUT block size is fixed (BLOCK_BYTES), so the decoder knows the number
 * of symbols per block itself: min(BLOCK_BYTES, orig_len − already_decoded).
 * sym_count is not stored in the stream.
 *
 * Decoder:
 *   - reads the block's flush pair [flush_lo][flush_hi] (NO header/renorm_count),
 *   - Init({flush_lo, flush_hi}) (with a swap, as before) → the initial state,
 *   - consumes renorm FORWARD as needed (words_p += Rans64Dec(...)), walks the
 *     output FORWARD.
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

#define RANS_SCALE_BITS  14u
#define BLOCK_BYTES      (256 * 1024)   /* fixed INPUT block size (same as encoder) */
/* Ceiling of renorm words per block = BLOCK_BYTES*14/32 + slack (see rans_encode.cpp). */
#define RENORM_BUF_WORDS (BLOCK_BYTES * 14u / 32u + 1024)


int main(int argc, char **argv) {
    static uint8_t out_buf[BLOCK_BYTES];
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input.rans> <output> [no_progress]\n", argv[0]);
        return 1;
    }
    int no_progress = (argc == 4 && strcmp(argv[3], "no_progress") == 0);

    zpl_file fin;
    zpl_file_error err = zpl_file_open(&fin, argv[1]);
    if (err) { perror("fopen input"); return err; }

    /* --- Header --- */
    uint8_t sig[4];
    if (!zpl_file_read(&fin, sig, sizeof(sig))) {
        fprintf(stderr, "read sig\n");
        zpl_file_close(&fin);
        return 1;
    }
    if (sig[0] != 'r' || sig[1] != 'n') {
        fprintf(stderr, "bad signature (expected 'rn', got %c%c)\n",
                sig[0], sig[1]);
        zpl_file_close(&fin);
        return 1;
    }
    int is_rle = sig[2] & 0x01;
    uint8_t rle_sym = sig[3];

    uint64_t orig_len = 0;
    if (!zpl_file_read(&fin, &orig_len, sizeof(orig_len))) {
        fprintf(stderr, "read len\n");
        zpl_file_close(&fin);
        return 1;
    }
    size_t n = (size_t)orig_len;
    if (n == 0) {
        zpl_file_close(&fin);
        zpl_file fout;
        zpl_file_create(&fout, argv[2]);
        zpl_file_close(&fout);
        return 0;
    }

    /* --- RLE mode --- */
    if (is_rle) {
        zpl_file_close(&fin);
        zpl_file fout;
        if (zpl_file_create(&fout, argv[2])) { perror("fopen output"); return 1; }
        memset(out_buf, rle_sym, BLOCK_BYTES);
        size_t remaining = n;
        while (remaining > 0) {
            size_t block = (remaining < BLOCK_BYTES) ? remaining : BLOCK_BYTES;
            if (!zpl_file_write(&fout, out_buf, block)) {
                perror("fwrite error");
                zpl_file_close(&fout);
                return 1;
            }
            remaining -= block;
        }
        zpl_file_close(&fout);
        printf("DECODE-RANS OK (RLE Mode, sym=0x%02X, len=%zu)\n", rle_sym, n);
        return 0;
    }

    /* --- rANS mode: reading cum[1..256] --- */
    cums_t m = {0};
    if (!zpl_file_read(&fin, m + 1, sizeof(m[0]) * ALPHABET)) {
        fprintf(stderr, "read cum\n");
        zpl_file_close(&fin);
        return 1;
    }

    lut_t lut;
    model_build_lut(lut, m);

    /* CPU calibration */
    zpl_u64 t0xx = zpl_time_rel_ms() + 100;
    zpl_u64 dddd = zpl_rdtsc();
    while (zpl_time_rel_ms() < t0xx);
    zpl_u64 zpl_rdtsc_freq = (zpl_rdtsc() - dddd) * 10;
    printf("CPU freq = %1.1f Mhz\n", zpl_rdtsc_freq / 1000000.0f);

    zpl_i64 rans_data_len = (zpl_file_size(&fin) - 524);
    const size_t buf_words = rans_data_len/4;



    uint64_t rans_u32_len = (buf_words+2)*4;
    uint32_t *words = (uint32_t *) malloc( rans_u32_len  );
    if ( words == NULL) {
        fprintf(stderr, "malloc words\n");
        zpl_file_close(&fin);
        return 1;
    }
    uint8_t *u8_p =(uint8_t*)words;
    int64_t u8_len = rans_data_len;
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


    zpl_file fout;
    if (zpl_file_create(&fout, argv[2])) {
        free(words);
        perror("fopen output");
        return 1;
    }

    zpl_u64 total_ticks = 0;
    size_t i = 0;   /* bytes emitted */

    auto words_p = words;

    while (i < n) {
        size_t block_syms = zpl_min(n - i, (size_t)BLOCK_BYTES);


        /* Four interleaved states (matches the encoder's 4-way rotation).
           Read 4 flush pairs forward: [s0.lo,s0.hi, s1.lo,s1.hi,
           s2.lo,s2.hi, s3.lo,s3.hi] (the encoder pushed encs[3..0]). Each
           Init({lo,hi}) rebuilds state = lo | (hi<<32). */
        rANS::Decoder decs[4];
        decs[0].Init({words_p[0], words_p[1]});
        decs[1].Init({words_p[2], words_p[3]});
        decs[2].Init({words_p[4], words_p[5]});
        decs[3].Init({words_p[6], words_p[7]});
        words_p += 8;

        zpl_u64 t0 = zpl_rdtsc();
        for (size_t k = 0; k < block_syms; k++) {
            auto dec = decs[0];
            uint32_t cum = dec.Rans64DecGet(RANS_SCALE_BITS);
            uint16_t cum_lo, freq;
            uint8_t sym = model_find_lut(lut, m, (uint16_t)cum, &cum_lo, &freq);
            words_p += dec.Rans64Dec(cum_lo, freq, RANS_SCALE_BITS, *words_p);
            out_buf[k] = sym;
            decs[0] = decs[1];
            decs[1] = decs[2];
            decs[2] = decs[3];
            decs[3] = dec;
        }
        total_ticks += zpl_rdtsc() - t0;
        if (!zpl_file_write(&fout, out_buf, block_syms)) {
            free(words);
            perror("fwrite error");
            zpl_file_close(&fout);
            return 1;
        }
        i += block_syms;
        if (!no_progress) {
            printf("\r                      \r%zu / %zu  (%3.1f%%)",
                   i, n, 100.0 * (double)i / (double)n);
            fflush(stdout);
        }
    }
    if (!no_progress) printf("\n");

    uint32_t ticks_per_symbol = (n > 0) ? (uint32_t)(total_ticks / n) : 0;
    uint64_t total_dec_time_ms = (zpl_rdtsc_freq > 0)
        ? (uint64_t)(total_ticks * 1000 / zpl_rdtsc_freq)
        : 0;
    double mb_s = (total_dec_time_ms > 0)
        ? (double)n / (total_dec_time_ms * 1048576.0 / 1000.0)
        : 0.0;

    zpl_file_close(&fout);
    free(words);

    printf("DECODE-RANS OK\n");
    printf("  engine    : 64-bit rANS, 32-bit renorm, scale_bits=%u, N=4\n", RANS_SCALE_BITS);
    printf("  decode_ms : %" PRIu64 "\n", total_dec_time_ms);
    printf("  decode_mbs: %.1f\n", mb_s);
    printf("  dec_ticks : %u\n", ticks_per_symbol);
    return 0;
}