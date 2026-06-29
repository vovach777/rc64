/* =========================================================================
 * RC24S ENCODE — 24-bit carryless RC, N=4 interleaved streams, SHARED BUFFER.
 *
 * Usage: rc24s_encode <input> <output.rc24s> [no_progress]
 *
 * .rc24s format:
 *   [4]   signature: 'r','4','s', flags
 *   [8]   uint64_t original_len (LE)
 *   [512] cum[1..256] uint16_t LE
 *   [4]   uint32_t max_stream_len LE — max bytes emitted by any stream
 *   [max_stream_len * N] shared interleaved buffer
 *
 * Byte k of stream i lives at shared[k * N + i].  No merge, no swizzle.
 * ========================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#  define ZPL_NANO
#define ZPL_IMPLEMENTATION
#include "zpl.h"
#include "rc24_codec.h"
#include "model_12.h"

#define N 4
#define PROGRESS_BLOCK (16*1024)

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) { fprintf(stderr, "Usage: %s <in> <out.rc24s> [no_progress]\n", argv[0]); return 1; }
    int no_progress = (argc==4 && !strcmp(argv[3],"no_progress"));

    zpl_file fin; if (zpl_file_open(&fin, argv[1])) { fprintf(stderr,"open\n"); return 1; }
    zpl_i64 fs = zpl_file_size(&fin); if (fs<0) { fprintf(stderr,"size\n"); zpl_file_close(&fin); return 1; }
    uint64_t n = (uint64_t)fs;
    uint8_t *data = malloc(n);
    { uint8_t *p=data; int64_t r=(int64_t)n; while(r>0){uint32_t c=(uint32_t)zpl_min(r,0x1000000LL); if(!zpl_file_read(&fin,p,c)){fprintf(stderr,"read\n");return 1;} p+=c; r-=c;} }
    zpl_file_close(&fin);

    uint32_t raw[256]={0}; for(size_t i=0;i<n;i++) raw[data[i]]++;
    model12_t M; int is_rle = model12_build(&M, raw);
    if (is_rle < 0) { fprintf(stderr,"model\n"); free(data); return 1; }

    zpl_file fout; if (zpl_file_create(&fout, argv[2])) { fprintf(stderr,"open out\n"); free(data); return 1; }
    uint8_t sig[4] = {'r','4','s',(uint8_t)!!is_rle};
    zpl_file_write(&fout, sig, 4);
    zpl_file_write(&fout, &n, 8);

    zpl_u64 t0xx=zpl_time_rel_ms()+100, d0=zpl_rdtsc(); while(zpl_time_rel_ms()<t0xx);
    zpl_u64 freq=(zpl_rdtsc()-d0)*10;
    printf("CPU freq = %1.1f Mhz\n", freq/1000000.0f);

    if (is_rle) {
        zpl_file_close(&fout);
        printf("ENCODE-RC24S OK (RLE)\n  in: %" PRIu64 "\n  out: 12\n", n);
        free(data); return 0;
    }

    zpl_file_write(&fout, M.cums+1, sizeof(M.cums[0])*ALPHABET_12);

    /* Precompute per-symbol cum/freq tables to remove model12_get from hot loop. */
    uint16_t cum_tbl[256], freq_tbl[256];
    for (int sym = 0; sym < 256; sym++) {
        cum_tbl[sym]  = M.cums[sym];
        freq_tbl[sym] = (uint16_t)(M.cums[sym + 1] - M.cums[sym]);
    }

    /* Single shared buffer sized by max possible bytes per stream. */
    size_t cap = n/N + n/50 + 4096;
    uint8_t *shared = calloc(cap * N + 16, 1);
    if (!shared) { fprintf(stderr,"malloc\n"); free(data); return 1; }
    size_t spos[N]; uint32_t elow[N], erange[N];
    for (int s=0; s<N; s++) { spos[s]=0; elow[s]=0; erange[s]=RC24_MASK; }

    zpl_u64 total_ticks=0; size_t i=0;
    while (i < n) {
        size_t blk = (size_t)zpl_min((zpl_i64)(n-i), (zpl_i64)PROGRESS_BLOCK);
        zpl_u64 t0 = zpl_rdtsc();
        for (size_t k=0; k<blk; k++) {
            int s = (int)((i+k) % N);
            uint8_t sym = data[i+k];
            uint16_t cum = cum_tbl[sym];
            uint16_t fq  = freq_tbl[sym];
            elow[s] += cum * (erange[s] /= RC24_TOTAL);
            erange[s] *= fq;
            while ((elow[s]^(elow[s]+erange[s])) < RC24_TOP ||
                   (erange[s]<RC24_BOT && ((erange[s]=-elow[s]&(RC24_BOT-1)),1))) {
                if (erange[s]==0) erange[s]=RC24_BOT-1;
                shared[spos[s] * N + s] = (uint8_t)(elow[s]>>16);
                spos[s]++;
                erange[s] <<= 8;
                elow[s] = (elow[s]<<8) & RC24_MASK;
            }
        }
        total_ticks += zpl_rdtsc()-t0;
        i += blk;
        if (!no_progress) { printf("\r                      \r%" PRIu64 " / %" PRIu64 "  (%3.1f%%)",(uint64_t)i,n,100.0*(double)i/(double)n); fflush(stdout); }
    }
    if (!no_progress) printf("\n");

    /* flush: 3 bytes per stream */
    for (int s=0; s<N; s++) {
        for (int j=0; j<3; j++) {
            shared[spos[s] * N + s] = (uint8_t)(elow[s]>>16);
            spos[s]++; elow[s]<<=8;
        }
    }

    size_t max_len = 0;
    for (int s=0; s<N; s++) if (spos[s] > max_len) max_len = spos[s];

    uint32_t max_len_u32 = (uint32_t)max_len;
    zpl_file_write(&fout, &max_len_u32, 4);
    zpl_file_write(&fout, shared, max_len * N);

    zpl_i64 out_bytes = zpl_file_size(&fout);
    zpl_file_close(&fout);
    free(shared); free(data);

    uint32_t tps = n ? (uint32_t)(total_ticks/n) : 0;
    uint64_t ms = freq ? (uint64_t)(total_ticks*1000/freq) : 0;
    double bpb = n ? (double)out_bytes*8.0/n : 0;
    double mbs = ms ? (double)n/(ms*1048576.0/1000.0) : 0;

    printf("ENCODE-RC24S OK\n");
    printf("  engine    : 24-bit carryless RC, N=%d super-interleaved, TOTAL=4096\n", N);
    printf("  input     : %s\n", argv[1]);
    printf("  in_size   : %" PRIu64 " bytes\n", n);
    printf("  out_size  : %" PRIi64 " bytes\n", out_bytes);
    printf("  bpb       : %.3f\n", bpb);
    printf("  ratio_pct : %.2f\n", n ? 100.0*out_bytes/n : 0);
    printf("  encode_ms : %" PRIu64 "\n", ms);
    printf("  encode_mbs: %.1f\n", mbs);
    printf("  enc_ticks : %u\n", tps);
    return 0;
}
