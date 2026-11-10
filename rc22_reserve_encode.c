/* =========================================================================
 * RC22 RESERVE-AHEAD ENCODE -- 22-bit [2][8][12] coder + dynamically
 * reserved shared buffer (llcomp/rc Buf::take idiom).  ADDITIVE: no existing
 * rc64 file is modified.
 *
 * Usage: rc22_reserve_encode <input> <output.rc22rsv> [N=1..8] [no_progress]
 *
 * N splits the input into N contiguous SEGMENTS, each with its own 22-bit
 * encoder state but ONE shared buffer.  Slots are handed out by r22_take()
 * in global event order, so each segment occupies one contiguous run and the
 * decoder needs a single nx pointer (no per-stream sizes stored).
 *
 * .rc22rsv format:
 *   [4]  'r','2','R', N
 *   [8]  uint64 original_len LE
 *   [1]  flags (bit0 = RLE)
 *   [1]  rle_sym
 *   [512] cum[1..256] uint16 LE   (only when !RLE; static 12-bit model)
 *   [4]  uint32 total_slots LE    (== Buf.next, exact)
 *   [total_slots] shared buffer
 * ========================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "model_12.h"
#include "rc22_reserve.h"

static uint64_t r22_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static inline uint64_t r22_rdtsc(void) { return __builtin_ia32_rdtsc(); }
static uint64_t r22_freq(void) {
    uint64_t m0 = r22_now_ms(), c0 = r22_rdtsc();
    while (r22_now_ms() - m0 < 100) { }
    return (r22_rdtsc() - c0) * 10;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "Usage: %s <input> <output.rc22rsv> [N=1..8] [no_progress]\n", argv[0]);
        return 1;
    }
    int N = (argc >= 4) ? atoi(argv[3]) : 1;
    if (N < 1 || N > R22_MAXN) N = 1;
    int no_progress = (argc == 5 && !strcmp(argv[4], "no_progress"));

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("open in"); return 1; }
    if (fseek(fin, 0, SEEK_END)) { fclose(fin); return 1; }
    long fs = ftell(fin);
    if (fs < 0) { fclose(fin); return 1; }
    rewind(fin);
    size_t n = (size_t)fs;
    uint8_t *data = (uint8_t *)malloc(n + R22_MAXN + 16);
    if (!data) { fclose(fin); return 1; }
    if (n && fread(data, 1, n, fin) != n) { fclose(fin); free(data); return 1; }
    fclose(fin);
    memset(data + n, 0, R22_MAXN + 16);

    /* Empty input: model12_build() returns -1 (no active symbol), so it
       cannot be used here.  Emit only the 14-byte header; the decoder stops
       right after flags/rle_sym when orig_len == 0 and creates an empty file.
       This is handled locally (no existing rc64 file is modified). */
    if (n == 0) {
        FILE *fe = fopen(argv[2], "wb");
        if (!fe) { perror("open out"); free(data); return 1; }
        uint8_t hdr[14] = { 'r', '2', 'R', (uint8_t)N,
                            0, 0, 0, 0, 0, 0, 0, 0,   /* orig_len = 0 LE */
                            0, 0 };                    /* flags = 0, rle_sym = 0 */
        fwrite(hdr, 1, sizeof(hdr), fe);
        fclose(fe);
        printf("ENCODE-RC22RSV OK (EMPTY)\n");
        printf("  N         : %d\n", N);
        printf("  in_size   : 0\n");
        printf("  out_size  : 14\n");
        free(data);
        return 0;
    }

    uint32_t raw[256] = {0};
    for (size_t i = 0; i < n; ++i) ++raw[data[i]];
    model12_t M;
    int is_rle = model12_build(&M, raw);
    if (is_rle < 0) { fprintf(stderr, "model\n"); free(data); return 1; }

    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("open out"); free(data); return 1; }
    uint8_t sig[4] = { 'r', '2', 'R', (uint8_t)N };
    fwrite(sig, 1, 4, fout);
    fwrite(&n, 8, 1, fout);
    uint8_t flags = (uint8_t)(!!is_rle);
    uint8_t rle_sym = (uint8_t)(is_rle ? M.cums[1] : 0);
    fwrite(&flags, 1, 1, fout);
    fwrite(&rle_sym, 1, 1, fout);

    if (is_rle) {
        fclose(fout);
        printf("ENCODE-RC22RSV OK (RLE)\n  in_size   : %" PRIu64 "\n  out_size  : 14\n", (uint64_t)n);
        free(data); return 0;
    }
    fwrite(M.cums + 1, sizeof(M.cums[0]), ALPHABET_12, fout);
    r22_model_init(M.cums);

    uint64_t freq = r22_freq();
    r22_buf_t B;
    r22_buf_init(&B, n / 2 + n / 50 + 4096);

    uint64_t total_ticks = 0;
    uint64_t t0 = r22_rdtsc();
    uint64_t renorms = 0, bypass_cnt = 0, bypass_steps = 0;
    for (int s = 0; s < N; ++s) {
        size_t start = (size_t)s * n / (size_t)N;
        size_t end   = (size_t)(s + 1) * n / (size_t)N;
        r22_enc_t E;
        r22_enc_init(&E, &B);
        for (size_t i = start; i < end; ++i)
            r22_enc_encode(&E, &B, data[i]);
        r22_enc_flush(&E, &B);
        renorms      += E.renorms;
        bypass_cnt   += E.bypass_cnt;
        bypass_steps += E.bypass_steps;
    }
    total_ticks = r22_rdtsc() - t0;

    uint32_t total_slots = (uint32_t)B.next;
    fwrite(&total_slots, 4, 1, fout);
    if (B.next) fwrite(B.b, 1, B.next, fout);
    long out_bytes = ftell(fout);
    fclose(fout);

    uint32_t tps = n ? (uint32_t)(total_ticks / n) : 0;
    uint64_t ms = freq ? (uint64_t)(total_ticks * 1000 / freq) : 0;
    double mb_s = ms ? (double)n / (ms * 1048576.0 / 1000.0) : 0.0;
    double bypass_per_sym = n ? (double)bypass_cnt / (double)n : 0.0;

    printf("ENCODE-RC22RSV OK\n");
    printf("  N         : %d\n", N);
    printf("  in_size   : %" PRIu64 "\n", (uint64_t)n);
    printf("  out_size  : %ld\n", out_bytes);
    printf("  bpb       : %.3f\n", (double)(out_bytes * 8) / (double)(n ? n : 1));
    printf("  ratio_pct : %.2f\n", 100.0 * (double)out_bytes / (double)(n ? n : 1));
    printf("  encode_ms : %" PRIu64 "\n", ms);
    printf("  encode_mbs: %.1f\n", mb_s);
    printf("  enc_ticks : %u\n", tps);
    printf("  slots     : %u\n", total_slots);
    printf("  takes     : %" PRIu64 "\n", B.takes);
    printf("  renorms   : %" PRIu64 "\n", renorms);
    printf("  bypass    : %" PRIu64 " (%.6f per sym)\n", bypass_cnt, bypass_per_sym);
    printf("  bypass_steps : %" PRIu64 "\n", bypass_steps);
    printf("  inv_lut   : 1024 x uint32 = 4 KiB\n");
    if (!no_progress) printf("  done\n");
    r22_buf_free(&B); free(data);
    return 0;
}
