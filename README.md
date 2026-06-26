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

## Results (enwik9, 1 GB, O3, clang x86_64)

```
input:      1,000,000,000 bytes
output:       644,975,732 bytes (64.50%, 5.160 bpb)
encode:         3.7 sec (258.7 MB/s)
decode:        32.2 sec  (29.6 MB/s)  — pure timed region, I/O excluded
roundtrip:   OK
```

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
| `roundtrip_rans.sh` | Roundtrip tests for the rANS engine. |

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

## rANS engine (rans_codec.h, rans_encode.cpp, rans_decode.cpp)

A 64-bit rANS (Asymmetric Numeral System) codec: 64-bit state, 32-bit renorm,
14-bit static model (scale_bits = 14, TARGET_TOTAL = 16384). The model is shared
with the 64-bit range coder (`model.h`); the alphabet, header, and RLE handling
match the `.rc` family (524-byte header, signature `'r','n'`).

### Block scheme — lightening the decoder

The design goal is to push all reversing onto the encoder so the decoder does a
single forward pass. The input is split into fixed-size independent blocks
(BLOCK_BYTES = 256 KB). Each block is an **independent** rANS: a fresh
`state = RANS64_L` at the start and its own flush pair at the end — state is not
carried across blocks.

- The encoder walks blocks FORWARD and, within each block, walks the input
  BACKWARD (rANS is LIFO).
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
then consecutive blocks (fixed INPUT size BLOCK_BYTES = 256 KB):
  [4 bytes]  uint32_t flush_lo = LOW32  of the block's final state (LE)
  [4 bytes]  uint32_t flush_hi = HIGH32 of the block's final state (LE)
  [4*K]      uint32_t renorm words of the block, in decoder-consumption order
             (read FORWARD)
  (K is NOT stored — the decoder consumes renorm inline and, after
   block_syms = min(BLOCK_BYTES, orig_len − already_decoded) symbols,
   arrives at the next flush pair by rANS symmetry.)
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
FORWARD with a `words_p` pointer. For each block it reads the flush pair
(`Init({flush_lo, flush_hi})` reconstructs the block's final state), then
decodes `block_syms` symbols, advancing `words_p` inline via
`words_p += dec.Rans64Dec(cum_lo, freq, scale_bits, *words_p)` — the decoder
advances exactly when a renorm word is consumed, so it lands on the next flush
pair with no stored count.