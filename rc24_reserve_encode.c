/* =========================================================================
 * RC24 RESERVE-AHEAD ENCODE -- N interleaved streams, one dynamically
 * reserved shared buffer (llcomp/rc Buf::take idiom).  ADDITIVE: no existing
 * rc64 file is modified.
 *
 * Usage: rc24_reserve_encode <input> <output.rc24rsv> [N] [no_progress]
 *
 * .rc24rsv format:
 *   [4]  'r','4','R', N
 *   [8]  uint64 original_len LE
 *   [1]  flags (bit0 = RLE)
 *   [1]  rle_sym
 *   [512] cum[1..256] uint16 LE   (only when !RLE; 12-bit model)
 *   [4]  uint32 total_slots LE    (== Buf.next, exact)
 *   [total_slots] shared buffer   (no max_stream_len, no per-stream sizes)
 * ========================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "model_12.h"
#include "rc24_reserve.h"

static uint64_t rsv_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static inline uint64_t rsv_rdtsc(void) { return __builtin_ia32_rdtsc(); }
static uint64_t rsv_freq(void) {
    uint64_t m0 = rsv_now_ms(), c0 = rsv_rdtsc();
    while (rsv_now_ms() - m0 < 100) { }
    return (rsv_rdtsc() - c0) * 10;   /* ticks per second */
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "Usage: %s <input> <output.rc24rsv> [N=1..8] [no_progress]\n", argv[0]);
        return 1;
    }
    int N = (argc >= 4) ? atoi(argv[3]) : 4;
    if (N < 1 || N > RSV_MAXN) N = 4;
    int no_progress = (argc == 5 && !strcmp(argv[4], "no_progress"));

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("open in"); return 1; }
    if (fseek(fin, 0, SEEK_END)) { fclose(fin); return 1; }
    long fs = ftell(fin);
    if (fs < 0) { fclose(fin); return 1; }
    rewind(fin);
    size_t n = (size_t)fs;
    uint8_t *data = (uint8_t *)malloc(n + RSV_MAXN + 16);
    if (!data) { fclose(fin); return 1; }
    if (n && fread(data, 1, n, fin) != n) { fclose(fin); free(data); return 1; }
    fclose(fin);
    memset(data + n, 0, RSV_MAXN + 16);

    uint32_t raw[256] = {0};
    for (size_t i = 0; i < n; ++i) ++raw[data[i]];
    model12_t M;
    int is_rle = model12_build(&M, raw);
    if (is_rle < 0) { fprintf(stderr, "model\n"); free(data); return 1; }

    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("open out"); free(data); return 1; }
    uint8_t sig[4] = { 'r', '4', 'R', (uint8_t)N };
    fwrite(sig, 1, 4, fout);
    fwrite(&n, 8, 1, fout);
    uint8_t flags = (uint8_t)(!!is_rle);
    uint8_t rle_sym = (uint8_t)(is_rle ? M.cums[1] : 0);
    fwrite(&flags, 1, 1, fout);
    fwrite(&rle_sym, 1, 1, fout);

    uint64_t freq = rsv_freq();
    if (is_rle) {
        fclose(fout);
        printf("ENCODE-RC24RSV OK (RLE)\n  in_size   : %" PRIu64 "\n  out_size  : 14\n", (uint64_t)n);
        free(data); return 0;
    }
    fwrite(M.cums + 1, sizeof(M.cums[0]), ALPHABET_12, fout);

    uint32_t cum15[ALPHABET_12 + 1], freq15[ALPHABET_12];
    for (int s = 0; s < ALPHABET_12; ++s) {
        cum15[s]  = (uint32_t)M.cums[s] * 8u;
        freq15[s] = (uint32_t)(M.cums[s + 1] - M.cums[s]) * 8u;
    }
    cum15[ALPHABET_12] = RSV_TOTAL;

    rsv_buf_t B;
    rsv_buf_init(&B, n / 2 + n / 50 + 4096);
    rsv_enc_t E[RSV_MAXN];
    for (int s = 0; s < N; ++s) rsv_enc_init(&E[s], &B);

    size_t na = ((n + (size_t)N - 1) / (size_t)N) * (size_t)N;
    uint64_t total_ticks = 0;
    uint64_t t0 = rsv_rdtsc();
    for (size_t i = 0; i < na; i += (size_t)N) {
        for (int s = 0; s < N; ++s) {
            uint32_t sym = data[i + (size_t)s];
            rsv_enc_put(&E[s], &B, cum15[sym], freq15[sym]);
        }
    }
    total_ticks = rsv_rdtsc() - t0;
    for (int s = 0; s < N; ++s) rsv_enc_flush(&E[s], &B);

    uint32_t total_slots = (uint32_t)B.next;
    fwrite(&total_slots, 4, 1, fout);
    if (B.next) fwrite(B.b, 1, B.next, fout);
    long out_bytes = ftell(fout);
    fclose(fout);

    uint64_t renorms = 0, cuts = 0;
    for (int s = 0; s < N; ++s) { renorms += E[s].renorms; cuts += E[s].cuts; }
    uint32_t tps = n ? (uint32_t)(total_ticks / n) : 0;
    uint64_t ms = freq ? (uint64_t)(total_ticks * 1000 / freq) : 0;
    double mb_s = ms ? (double)n / (ms * 1048576.0 / 1000.0) : 0.0;
    double per1m = renorms ? (double)cuts * 1000000.0 / (double)renorms : 0.0;

    printf("ENCODE-RC24RSV OK\n");
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
    printf("  cuts      : %" PRIu64 "\n", cuts);
    printf("  cuts_per_1M_renorms : %.4f\n", per1m);
    if (!no_progress) printf("  done\n");
    rsv_buf_free(&B); free(data);
    return 0;
}
