# rc64 — Schindler 64-bit Range Coder

Static order-0 range coder based on Mikhail Schindler's algorithm.
64-bit state, 32-bit shift, carry via uint64 overflow.
Inplace cache/FF — no deferred FF, no write loops on safe emit.

The repository also contains a 32-bit range-coder engine and a 64-bit rANS
engine (see the dedicated sections below).

## Build

```
mkdir build && cd build
cmake ..
make
make roundtrip      # 64-bit engine (14-bit model, uint32_t words)
make roundtrip32    # 32-bit engine (12-bit model, uint16_t words)
make roundtrip_rans # rANS engine (64-bit state, 32-bit renorm, 14-bit model)
make roundtrip_fse  # FSE engine (vendored tANS, 14-bit model, block scheme)
make roundtrip_huf  # HUF engine (vendored Huff0, per-block table, 128 KB blocks)
make bench_rans     # rANS N-way interleave benchmark (enc+dec, N=1..5)
```

## Usage

### Encoder (64-bit range coder)
```
./rc_encode <input_file> <output_file.rc>
```

### Decoder (64-bit range coder)
```
./rc_decode <input_file.rc> <output_file>
```

### 32-bit engine (RC_TOTAL_BITS=12, 16-bit words)
```
./rc_encode32 <input_file> <output_file.rc32>
./rc_decode32 <input_file.rc32> <output_file>
```

### rANS engine (64-bit state, 32-bit renorm, 14-bit model)
```
./rans_encode <input_file> <output_file.rans>
./rans_decode <input_file.rans> <output_file>
```

### FSE engine (vendored tANS, block scheme, 14-bit model)
```
./fse_encode <input_file> <output_file.fse>
./fse_decode <input_file.fse> <output_file>
```

### HUF engine (vendored Huff0, per-block table, 128 KB blocks)
```
./huf_encode <input_file> <output_file.huf>
./huf_decode <input_file.huf> <output_file>
```

### Roundtrip test (all 6 datasets)
```
make roundtrip      # 64-bit range coder
make roundtrip32    # 32-bit range coder
make roundtrip_rans # rANS engine
```

### Diagnostics (encoder branch statistics, no output written)
```
./rc_diag <input_file>
```

## `.rc` format

```
[4 bytes]   signature: 'r','c', flags, rle_sym
            flags bit 0: is_rle
[8 bytes]   uint64_t original_len (LE)
--- RLE mode (is_rle=1) ---
            (nothing else — rle_sym is in the signature)
--- RC mode (is_rle=0) ---
[512 bytes] cum[1..256] — uint16_t LE (cumulative frequencies)
            cum[0]=0 is not stored (always a constant)
[4*N bytes] uint32_t words LE — encoder stream
```

Header: 4 + 8 + 512 = 524 bytes, 32-bit aligned.
RLE: the whole file is 12 bytes.

## Model

Static order-0, total = 2^14 = 16384.
Frequencies are built once over the whole stream.
Scaling: scaled = raw * 16384 / total_raw.
If a frequency is > 0 but scaled = 0, it is raised to 1.
The sum is corrected back to 16384 via the maximum frequency.
RLE is used when only one symbol is active.

## Results — enwik9 (1 GB), all engines, O3, clang, x86_64

Order-0 static models; roundtrip verified (`cmp`); in-memory core timing
(I/O and model/table construction excluded); **median of 3–5 runs** (the min/max
of past runs was noisy — first run is cold-cache/page-fault heavy on 1 GB, and
the `rdtsc` frequency calibration jitters ±0.5% per run; the median is stable).

| Engine | Model | Compressed | bpb | Encode | Decode | enc+dec |
|---|---|---|---|---|---|---|
| 64-bit RC (Schindler) | 14-bit | 644,975,728 (64.50%) | 5.160 | 250 MB/s (9 t/s) | 56 MB/s (42 t/s) | 21.9 s |
| 32-bit RC | 12-bit | 648,568,754 (64.86%) | 5.189 | 204 MB/s (11 t/s) | 68 MB/s (35 t/s) | 19.6 s |
| **RC24 single (LUT24)** | **12-bit** | **648,987,394 (64.90%)** | **5.192** | **~136 MB/s (17 t/s)** | **~62 MB/s (38 t/s)** | **~13.7 s** |
| **RC24S N=4 shared (LUT24)** | **12-bit** | **648,987,394 (64.90%)** | **5.192** | **~104 MB/s (22 t/s)** | **~59 MB/s (40 t/s)** | **~14.1 s** |
| rANS (4-way interleave) | 14-bit | 645,067,056 (64.51%) | 5.161 | 200 MB/s (11 t/s) | **416 MB/s (5 t/s)** | 7.4 s |
| FSE tANS (reference) | 14-bit | 644,780,828 (64.48%) | 5.158 | 372 MB/s (6 t/s) | 349 MB/s (6 t/s) | 5.6 s |
| HUF / Huff0 (reference) | per-block | 646,646,919 (64.66%) | 5.173 | **569 MB/s (4 t/s)** | **651 MB/s (3 t/s)** | **3.3 s** |

**FSE** = `Cyan4973/FiniteStateEntropy` (the tANS used by zstd), vendored under
`lib/` and built as the `fse_encode`/`fse_decode` tools, which mirror the rc64
rANS scheme **exactly**: 256 KB blocks, one global order-0 model built over the
whole file (untimed), and a `zpl_rdtsc()` timed per-block loop for both encode
and decode — same block size, same model granularity, same measurement. tableLog
raised from the default cap of 12 to **14** to match the rc64 14-bit model.
FSE's decode runs **2-way interleaved** (`state1`/`state2`), so this is FSE's
normal single-threaded path; multithreading is out of scope, interleave is kept
(the X4/X6 paths belong to HUF/Huffman, not FSE).

**HUF** (Huff0) is the Huffman coder from the same `lib/`, built as
`huf_encode`/`huf_decode`. It uses the **high-level** `HUF_compress()` /
`HUF_decompress()` API per 128 KB block (`HUF_BLOCKSIZE_MAX` — the largest block
HUF accepts). Each block is self-contained: HUF builds its **own per-block**
Huffman table inside the one timed `HUF_compress()` call (table construction is
part of the measurement, by design — no tree extraction). Incompressible blocks
(`HUF_compress` returns 0) are stored raw. zpl_rdtsc() timing per block, same as
the others. The "4X" in Huff0 is **4 interleaved bitstreams within one thread**
(ILP/SIMD), not OS threads — neither HUF nor FSE spawn threads.

- **RC24 note**: both single-stream (`rc24_decode`) and N=4 shared-buffer
  (`rc24s_decode`) use the 12-bit direct LUT (`rc_fast_div24`) by default.
  We also benchmarked two alternate division backends on this Haswell
  i7-4870HQ:
  - Plain integer division (`-DUSE_INT_DIV24`): ~49 MB/s single-stream,
    ~59 MB/s in RC24S.  Slower than LUT on single-stream, ties on N=4.
  - SSE `_mm_rcp_ss` Newton-Raphson (`-DUSE_SSE_RCP24`): ~42 MB/s
    single-stream, ~54 MB/s in RC24S.  Slower than LUT on both.
  The LUT remains the default; the alternate paths are kept as compile-time
  options for other CPUs.

- **Measurement fairness**: the FSE and rANS numbers are now measured with the
  *identical* scheme — 256 KB blocks, global model built once (untimed), in-core
  per-block `zpl_rdtsc()` timing, I/O excluded. Same input, same 14-bit
  precision. So this is a clean codec-vs-codec comparison (the earlier
  block-vs-single-stream caveat no longer applies).
- **Compression**: Huffman's integer code lengths cost ~0.015 bpb vs ANS — HUF
  trails (5.173); FSE is best (5.158); rANS and 64-bit RC tie (~5.160); the
  32-bit RC is worst (5.189, 12-bit model). Each codec pays a tiny per-block
  framing cost.
- **Encode**: HUF is fastest (table-driven, no division, 4 t/s), then FSE (6
  t/s). The rANS encoder is the slowest of the ANS codecs — N=4 pays the
  rotation overhead and the reciprocal `mul` (11 t/s; its encode peak is N=2 ≈
  256 MB/s, but N is shared with the decoder).
- **Decode**: **HUF is fastest (651 MB/s, 3 t/s)** — its table lookup is a pure
  load, no arithmetic. **rANS N=4 is next (416 MB/s, 5 t/s)**, beating FSE's
  2-way decode (349 MB/s, 6 t/s), and ~7× the RC decoders (56–68 MB/s).
- **Net (enc+dec)**: HUF leads on raw speed but pays the Huffman ratio tax; FSE
  is the best ANS speed/ratio balance; **rANS has a faster decoder than FSE at
  equal 14-bit precision** and is ~2.9× faster end-to-end than either RC engine.

## Block formats compared (rANS / FSE / HUF)

All three ANS-family engines split the input into **fixed-size INPUT blocks**
(256 KB for rANS/FSE, 128 KB for HUF — its `HUF_BLOCKSIZE_MAX`), so each codec
knows the original block size (`min(BLOCK_BYTES, remaining)`) and **never stores
it**. What differs is how the decoder locates the *compressed* boundary of each
block and where the model lives.

| | input block | model (frequency/table) | per-block compressed framing | how the decoder finds the next block |
|---|---|---|---|---|
| **rANS** (N=4) | 256 KB | **global** — `cum[1..256]` once in the 524-byte header | `4 flush pairs = 8 uint32` (lo,hi)×4 states, then `K` renorm words | **implicit, no length stored** — renorm words are interleaved with coding and self-delimiting via state; after `block_syms` symbols `words_p` lands on the next flush-pair group by rANS symmetry (emit K ⟺ consume K). The flush pairs are both the boundary marker and the final state for Init. |
| **FSE** | 256 KB | **global** — `tableLog + maxSymbolValue + norm[256]` once in the header | `4-byte csize` + `csize`-byte bare FSE bitstream (no tree) | **explicit `csize`** — the block is one contiguous packed bitstream; the decoder needs its length to seek to the next block. |
| **HUF (Huff0)** | 128 KB | **per-block** — HUF builds+embeds the Huffman table inside each block's compressed blob (no model in the header) | `4-byte cs` + `cs`-byte blob; `cs == 0xFFFFFFFF` ⇒ `bs` raw bytes (incompressible) | **explicit `cs`** — same contiguous-blob reason as FSE; plus a raw sentinel for blocks `HUF_compress` refused (returned 0). |

Key consequences:

- **rANS pays no per-block length field** — its renorm is state-driven, so the
  boundary falls out of the finite-state machine itself. It only stores the 4
  flush pairs (32 B) per block, which double as boundary + state.
- **FSE/HUF store a 4-byte length per block** because they pack the whole block
  into one contiguous blob; you can't find its end without an explicit size.
- **FSE keeps a global model** (one tree in the header, reused by every block);
  **HUF keeps a per-block model** (the tree is rebuilt and stored in each
  block). This is a size/precision trade-off, not a correctness one: HUF's
  per-block tree adapts to local statistics but costs ~tree-size per block; the
  global-model codecs adapt only file-wide but pay the model once.
- **RLE / incompressible**: none of the three needs a special RLE path — a
  single-symbol block compresses to ~1 byte (degenerate tree / single state).
  HUF additionally falls back to raw storage (`0xFFFFFFFF`) when a block yields
  no gain; rANS/FSE simply emit a near-incompressible stream.


### Decoder profile (assembly, 103 cycles/symbol)

| Component | Cycles | % |
|---|---|---|
| `divq` (code/t in get_cum) | 20-30 | 20-29% |
| binary search (8 steps, cmpl+setae+cmovbq) | ~40 | ~39% |
| `imulq` ×2 (step + range) | ~10 | ~10% |
| renorm (shrq+branch+shlq) | ~10 | ~10% |
| memory loads (cum, words[], out[]) | ~10 | ~10% |
| loop overhead | ~3 | ~3% |

Two bottlenecks:
1. `divq` — 64-bit division code/t, t is variable, irreducible.
2. binary search — 8 dependent memory loads from cum[257] (514 bytes = 8 cache lines).

### Optimizations applied

- `range / TARGET_TOTAL` → the compiler lowers it to `shrq $14` (compile-time constant).
- Binary search in model_find (8 steps instead of a linear 256).
- Streaming 16 KB output with the timer paused during fwrite.
- int64 timer (timer_ticks/timer_freq) instead of clock().
- cum[1..256] is stored directly (no prefix-sum step in the decoder).

## Experiments (subbotin_magic branch)

The subbotin_magic branch — Subbotin aligned trim instead of carry propagation:
- 64-bit, 16-bit shift, `range = (-low) & 0xFFFF`.
- Overhead vs carry: +0.00% (5232 bytes over 645 MB).
- Decoder ~16% faster (no carry patch).

## Files

| File | Purpose |
|---|---|
| `rc24_codec.h` | RC24 24-bit carryless Subbotin codec (encoder + decoder). Default decoder division: 12-bit direct LUT (`rc_fast_div24`). Optional SSE reciprocal via `-DUSE_SSE_RCP24`. |
| `rc24_encode.c` / `rc24_decode.c` | Single-stream RC24 tools (`.rc24`). |
| `rc24s_encode.c` / `rc24s_decode.c` | RC24 N=4 shared-buffer interleaved tools (`.rc24s`). |
| `rc32sub_codec.h` | RC32SUB 32-bit carryless Subbotin codec. |
| `rc32sub_encode.c` / `rc32sub_decode.c` | Single-stream RC32SUB tools (`.rc32sub`). |
| `roundtrip24.sh` | Roundtrip tests for single-stream RC24. |
| `roundtrip24s.sh` | Roundtrip tests for RC24S shared-buffer N=4. |
| `roundtrip32sub.sh` | Roundtrip tests for RC32SUB. |
| `test_rc24.c` | RC24 synthetic regression tests. |
| `future_work.md` | Open research problems, including dynamic ring-based reservation for RC24 interleave. |
| `rc_codec.h` | Inplace codec (encoder + decoder, always_inline). 64-bit Schindler. |
| `model.h` | Static order-0 model (14 bit, total=16384). For the 64-bit engines (RC and rANS). |
| `rc_encode.c` | Encoder (64-bit range coder). |
| `rc_decode.c` | Decoder (64-bit range coder, streaming output, pure timed region). |
| `rc_codec_32.h` | 32-bit in-place carry range coder (16-bit words, RC_TOTAL_BITS=12). |
| `model_12.h` | Static order-0 model (12 bit, total=4096). For the 32-bit engine. |
| `rc_encode32.c` | Encoder of the 32-bit engine. |
| `rc_decode32.c` | Decoder of the 32-bit engine. |
| `rans_codec.h` | rANS codec (64-bit state, 32-bit renorm). Encoder + decoder, always_inline. |
| `rans_encode.cpp` | rANS encoder (64-bit state, block scheme, all reversing on the encoder). |
| `rans_decode.cpp` | rANS decoder (block scheme, forward renorm consumption). |
| `timer.h` | Cross-platform int64 timer. |
| `test_data.h` | Datasets: LOREM, CCODE, ENGLISH, RUSSIAN, REPEAT, RANDOM. |
| `gen_data.c` | Test-data generator. |
| `rc_diag.c` | Encoder branch diagnostics. |
| `roundtrip.sh` | Roundtrip tests for the 64-bit range coder. |
| `roundtrip32.sh` | Roundtrip tests for the 32-bit engine. |
| `roundtrip32_lut.sh` | Roundtrip tests for the 32-bit RC + LUT Variant D decoder. |
| `roundtrip_rans.sh` | Roundtrip tests for the rANS engine. |
| `bench_enc.cpp` | rANS N-way interleave ENCODE benchmark (compile-time `-DNINTER=n`). |
| `bench_dec.cpp` | rANS N-way interleave DECODE benchmark (`-DNINTER=n`, verifies output). |
| `bench_rans.sh` | Runs the enc+dec benchmark for N=1..5 and prints a throughput table. |
| `lib/` | Vendored FSE (tANS) library from `Cyan4973/FiniteStateEntropy` (used by fse_encode/fse_decode). |
| `fse_encode.cpp` | FSE (tANS) order-0 BLOCK encoder — mirrors the rANS block scheme + `zpl_rdtsc()` per-block timing. |
| `fse_decode.cpp` | FSE order-0 BLOCK decoder (mirrors rans_decode). |
| `roundtrip_fse.sh` | Roundtrip tests for the FSE engine. |
| `huf_encode.cpp` | HUF (Huff0) order-0 encoder — high-level `HUF_compress()` per 128 KB block (per-block table), `zpl_rdtsc()` timed. |
| `huf_decode.cpp` | HUF order-0 decoder — high-level `HUF_decompress()` per block. |
| `roundtrip_huf.sh` | Roundtrip tests for the HUF engine. |

## 32-bit engine (rc_codec_32.h)

An alternative range coder that does not depend on 64-bit arithmetic. Validated
on 13+ datasets (uniform/skewed binary, 16/256-symbol uniform, Zipf, AR(1),
Laplace, real text, /dev/urandom, periodic, edge cases).

Principle:
- `r = range >> 12`, `low += r * cum_freq`, `range = r * freq`.
- Carry ripple: on 32-bit `low` overflow, increment already-written 16-bit
  words backward until the carry extinguishes.
- Renorm: while `range < 2^16`, emit the high word of `low` to the buffer.

The range headroom is `RC_TOP_BITS - RC_TOTAL_BITS = 4 bits`. This gives a lower
bound on overhead of ~0.1–0.2% on suitable data. The 64-bit engine is more
precise (14-bit model + 50-bit range), but the 32-bit engine ports to platforms
without 64-bit multiplies (Xtensa, MIPS32, Cortex-M0).

The `.rc32` format is identical in structure to `.rc` (524-byte header + stream),
except:
- signature `'r','3'` instead of `'r','c'`;
- the stream consists of `uint16_t` words (instead of `uint32_t`);
- the model is 12-bit (`TARGET_TOTAL_12 = 4096`) instead of 14-bit.

### Decoder division backend (selectable)

`rc32_dec_get_cum` computes `v = code / r` (the range-coder quotient). Three
selectable backends, chosen at compile time:

| Backend | Macro | How | Notes |
|---|---|---|---|
| integer `div` | *(default)* | `divl` | Simplest; best on CPUs with fast 32-bit divide. |
| double | `-DUSE_FLOAT_DIV` | `divsd` + imul/cmp/sbb correction | FP rounds-to-nearest, off-by-one-high fixed branchlessly. |
| **LUT Variant D** | `-DUSE_LUT_DIV` | 8-bit LUT (2 KB) + Newton-Raphson + 1 branchless correction | Pure integer, no div/FP; `rc_div_lut_init()` once at start. Exact for `v_true < 16384` (RC needs `< 4096`). |

Variant D is from `Cyan4973/FiniteStateEntropy`-style reciprocal work (8-bit LUT
+ Newton; see `model_12.h`). The LUT underestimates `1/r` (floor, not ceil), so
the remainder stays ≥ 0 (no unsigned wraparound) and a single upward correction
makes the result exact.

CMake targets: `rc_decode32` (default), `rc_decode32_fp` (`-DUSE_FLOAT_DIV`),
`rc_decode32_lutdiv` (`-DUSE_LUT_DIV`); `make roundtrip32_lut` verifies the LUT
decoder on all datasets.

**Measured on this machine** (Intel i7-4870HQ, Haswell/Crystalwell, enwik9,
median of 3, decode only — encode is division-free and common):

| Backend | decode ticks/sym | decode MB/s |
|---|---|---|
| **integer `div` (default)** | **36** | **65** |
| double (`USE_FLOAT_DIV`) | 43 | 55 |
| LUT Variant D (`USE_LUT_DIV`) | 42 | 56 |

On Haswell the integer `divl` **wins** here (the LUT's long critical path —
`lzcnt`+`shl`+`shr`+LUT load+3×`imul`+correction — doesn't overlap on this core,
and Haswell lacks the BMI2 `shlx`/`shrx` that speed the shifts). Variant D is
designed for Skylake+ where `div` latency is higher (~24c) and BMI2 is available.
**Always measure on the target CPU** — as the LUT research notes, on CPUs with a
fast `div` (Sapphire Rapids, Apple silicon, recent Neoverse) integer division
stays faster; the LUT pays off on Skylake-class parts.

### Why the LUT exists: N-way interleave throughput

The point of replacing `div` with the imul-based LUT is **not** latency (the
LUT's critical path is longer) but **throughput under interleaving**. A single
`div` unit serializes N independent streams; an imul pipeline (port 1, 1c
throughput) lets N streams overlap. Measured here (Haswell i7-4870HQ, enwik9,
decode, in-memory round-robin of N independent RC32 streams):

| N | integer `div` (t/sym · MB/s) | LUT Variant D (t/sym · MB/s) |
|---|---|---|
| 1 | 41 · 57 | 52 · 45 |
| 2 | **26 · 88** | 35 · 68 |
| 4 | 29 · 80 | 38 · 63 |
| 8 | 33 · 72 | 39 · 60 |

On **this** Haswell core `div` wins at every N — its `divl` is fast enough and
the LUT's 4-`imul` chain is longer, plus Haswell lacks BMI2 `shlx/shrx` (shifts
go through `CL`, slow), so the LUT's critical path doesn't collapse under
interleave. Interleave itself helps (N=2 ≈ **+50–60%**) but tops out at N=2
(div-unit contention) / N=2–3 (cache+register pressure on the N states).

**Where the LUT *would* win:** Skylake+ (`div` latency ~24c vs Haswell ~22c, and
BMI2 `shlx/shrx` collapse the shift chain) — there N≈4–6 should let the imul
throughput dominate and beat `div`. The numbers above are Haswell-specific; the
same probe on Skylake is expected to flip the LUT row ahead of `div` at N≥3.
The practical takeaway for a Haswell target: **RC32 + N=2 interleave + integer
`div`** is the fastest decoder here (≈88 MB/s, +54% over single-stream). On
Skylake-class targets, swap in `USE_LUT_DIV` and push N to ~4.

## rANS engine (rans_codec.h, rans_encode.cpp, rans_decode.cpp)

A 64-bit rANS (Asymmetric Numeral System) codec: 64-bit state, 32-bit renorm,
14-bit static model (scale_bits = 14, TARGET_TOTAL = 16384). The model is shared
with the 64-bit range coder (`model.h`); the alphabet, header, and RLE handling
match the `.rc` family (524-byte header, signature `'r','n'`).

### Block scheme — lightening the decoder

The design goal is to push all reversing onto the encoder so the decoder does a
single forward pass. The input is split into fixed-size independent blocks
(BLOCK_BYTES = 256 KB). Each block is an **independent** 4-way interleaved rANS:
four fresh `state = RANS64_L` at the start and its own 4 flush pairs at the end
— states are not carried across blocks.

- The encoder walks blocks FORWARD and, within each block, walks the input
  BACKWARD (rANS is LIFO), round-robining four encoder states by position
  (state = k & 3).
- The block's renorm words are written into a static buffer FROM THE END
  (`buf[--buf_head]`), so they land already in decoder-consumption order — no
  separate buffer flip step.
- The block is written as `[flush_lo][flush_hi][renorm...]`. The number of
  renorm words (K) **is not stored**: the decoder consumes renorm inline as it
  goes, and after decoding `block_syms` symbols the pointer lands on the next
  block's flush pair by itself (rANS symmetry — encoder emits K ⟺ decoder
  consumes K). The block boundary is determined by the fixed input block size,
  so no per-block header is needed.

### `.rans` format

```
[4 bytes]   signature: 'r','n', flags, rle_sym
            flags bit 0: is_rle
[8 bytes]   uint64_t original_len (LE)
--- RLE mode (is_rle=1) ---
            (nothing else — rle_sym is in the signature)
--- rANS mode (is_rle=0) ---
[512 bytes] cum[1..256] — uint16_t LE (cumulative frequencies, 14-bit)
            cum[0]=0 is not stored (always a constant)
then consecutive blocks (fixed INPUT size BLOCK_BYTES = 256 KB), 4-WAY
INTERLEAVED (four independent rANS states per block):
  [4*8 bytes] 8 uint32 flush words = 4 flush pairs (one per state s0..s3),
              forward order [s0.lo,s0.hi, s1.lo,s1.hi, s2.lo,s2.hi, s3.lo,s3.hi] (LE)
  [4*K]      uint32_t renorm words of the block, in decoder-consumption order
             (read FORWARD)
  (K is NOT stored — the decoder consumes renorm inline and, after
   block_syms = min(BLOCK_BYTES, orig_len − already_decoded) symbols,
   arrives at the next flush-pair group by rANS symmetry.)
```

Header: 4 + 8 + 512 = 524 bytes (same as `.rc` / `.rc32`).
RLE: the whole file is 12 bytes.

### Renorm buffer sizing

The renorm-word count per block is bounded by the **cross-entropy**, not the
model's own entropy:

```
renorm_count = (31 + n · H_cross − log2(state_final)) / 32,
H_cross = Σ p_i · (14 − log2 freq_i)  ≤ 14 bpb  (max code length at freq=1),
state_final ≥ RANS64_L = 2^31  (invariant after every Rans64EncPut).
⇒ renorm_count ≤ BLOCK_BYTES · 14 / 32 = 114688.
```

With +1024 slack, rounded up to a 256-byte boundary, the static BSS buffer is
`RENORM_BUF_WORDS = 115712` words (~452 KB) — fits in a 512 KB L2 slice. The
encoder keeps this single fixed buffer and reuses it per block (no malloc/free).
Note the cross-entropy bound is loose: a full 256 KB block cannot actually reach
H_cross = 14 (it would require every symbol to have model freq = 1, impossible
with a 256-symbol alphabet under a file-wide static model), so in practice a
full block stays around 8 bpb (rc ≈ 65536).

### Decoder

The decoder loads the whole renorm stream into memory once, then walks blocks
FORWARD with a `words_p` pointer. For each block it reads the 4 flush pairs
(`Init({lo, hi})` reconstructs each of the four interleaved states), then
decodes `block_syms` symbols round-robining the four states, advancing
`words_p` inline via `words_p += dec.Rans64Dec(cum_lo, freq, scale_bits,
*words_p)` — the decoder advances exactly when a renorm word is consumed, so it
lands on the next flush-pair group with no stored count.

### N-way interleave (state rotation)

Each symbol's step depends on the previous one through `state`, forming a long
dependency chain (the reciprocal `mul` on encode; `freq*(state>>scale)` plus the
serial renorm shift-in `state=(state<<32)|next` on decode). Interleaving N
independent states and round-robining them by position (`state = k & (N−1)`)
runs N chains in parallel, hiding that latency.

Two implementation notes:

- **Rotation, not indexing.** The states are held in a register-resident array
  and *rotated* each step (`e[N-1]=e[N-2]; …; e[0]=active`) rather than indexed
  as `e[k & (N-1)]`. A runtime-indexed access forces the array onto the stack
  (a load+store per state reference), whereas the rotation compiles to
  zero-latency register-to-register `mov`s — empirically, `e[k&1]` spills to L1
  while the rotation keeps both (and up to ~5) states in GPRs.
- **Register ceiling.** x86-64 has 16 GPRs; the encode/decode loops consume
  ~10 for their other live values, leaving ~5 for states. So N≤5 stays in
  registers; N=6 spills. In the production (loop inlined into `main`) the real
  ceiling is ~4.

Encoder and decoder peak at *different* N (measured, min of 5 runs, 32 MB
"english", `-O3 clang`, `bench_rans.sh`):

| N | enc t/sym | dec t/sym | enc+dec |
|---|---|---|---|
| 1 | 12.75 | 17.77 | 30.52 |
| 2 | **9.96** |  9.90 | 19.86 |
| 3 | 10.50 |  9.78 | 20.28 |
| 4 | 11.01 | **6.93** | **17.94** |
| 5 | 13.02 |  8.32 | 21.34 |

- The **encoder** is throughput-bound: 2-way ILP already hides the reciprocal
  `mul`, so it peaks at N=2 and degrades after (N−1 rotation moves/symbol +
  `buf_head` contention).
- The **decoder** is latency-bound, so it keeps improving to N=4 (−30% vs N=2);
  N=5 spills and regresses.

N is shared by the stream format (N flush pairs per block), so it is a
compromise. **N=4 is hardcoded** in the production codec: best combined
throughput (decoder's −3 t/sym outweighs the encoder's +1) and the decoder's
peak. Switch to N=2 for encode-heavy workloads (the `bench_enc`/`bench_dec`
tools are parameterized via `-DNINTER=n` to re-measure any N).
