#!/bin/sh
# rANS N-way interleave benchmark: encode + decode throughput vs N=1..5,
# plus rANS16x16 (16 / 32 / 64 word-rANS states in AVX2 vector pairs rotated
# round-robin; x16 is the fNBLI scheme).
#
# Builds bench_enc_<n>/bench_dec_<n> for each N, runs each min-of-5 (min damps
# background noise), prints a table. The per-N streams share the production
# .rans format for that N, so bench_dec_<n> verifies each encode.
#
# Usage: bench_rans.sh <build_dir> [dataset_index] [size]
#   dataset_index : see gen_data (default 2 = english)
#   size          : bytes (default 33554432 = 32 MB)
set -e
BIN="${1:-build}"
DS="${2:-2}"
SIZE="${3:-33554432}"
SRC=$(cd "$(dirname "$0")" && pwd)

ORIG="$BIN/bench_rans/big.orig"
mkdir -p "$BIN/bench_rans"
"$BIN/gen_data" "$DS" "$SIZE" "$ORIG"

CC="${CC:-clang++}"
CFLAGS="-O3 -std=c++17 -DNDEBUG -I$SRC"
for N in 1 2 3 4 5; do
    $CC $CFLAGS -DNINTER=$N "$SRC/bench_enc.cpp" -o "$BIN/bench_enc_$N"
    $CC $CFLAGS -DNINTER=$N "$SRC/bench_dec.cpp" -o "$BIN/bench_dec_$N"
done
# x16 / x32 need the target ISA (AVX2 for the vector decoder; without it a scalar loop decodes)
ARCH="${ARCH:--march=native}"
for L in 16 32 64; do
    $CC $CFLAGS $ARCH -DR16_LANES=$L "$SRC/bench_enc16.cpp" -o "$BIN/bench_enc16_x$L"
    $CC $CFLAGS $ARCH -DR16_LANES=$L "$SRC/bench_dec16.cpp" -o "$BIN/bench_dec16_x$L"
done

# print min ticks/sym over 5 runs of a binary
min5() {
    mn=999999
    for i in 1 2 3 4 5; do
        v=$("$@" 2>/dev/null | sed -n 's/.*ticks\/sym=\([0-9.]*\).*/\1/p')
        [ -n "$v" ] || continue
        if awk "BEGIN{exit !($v < $mn)}"; then mn=$v; fi
    done
    printf '%s' "$mn"
}

echo "dataset=$DS  size=$SIZE  (min of 5 runs)"
printf '%-4s %12s %12s %14s\n' "N" "enc t/sym" "dec t/sym" "enc+dec"
for N in 1 2 3 4 5; do
    "$BIN/bench_enc_$N" "$ORIG" "$BIN/bench_rans/o$N.rans" >/dev/null 2>&1
    e=$(min5 "$BIN/bench_enc_$N" "$ORIG" "$BIN/bench_rans/o$N.rans")
    d=$(min5 "$BIN/bench_dec_$N" "$BIN/bench_rans/o$N.rans" "$BIN/bench_rans/d$N.out" "$ORIG")
    tot=$(awk 'BEGIN{printf "%.2f", '"$e"' + '"$d"'}')
    printf '%-4s %12s %12s %14s\n' "$N" "$e" "$d" "$tot"
done
for L in 16 32 64; do
    "$BIN/bench_enc16_x$L" "$ORIG" "$BIN/bench_rans/x$L.r16" >/dev/null 2>&1
    e=$(min5 "$BIN/bench_enc16_x$L" "$ORIG" "$BIN/bench_rans/x$L.r16")
    d=$(min5 "$BIN/bench_dec16_x$L" "$BIN/bench_rans/x$L.r16" "$BIN/bench_rans/dx$L.out" "$ORIG")
    tot=$(awk 'BEGIN{printf "%.2f", '"$e"' + '"$d"'}')
    printf '%-4s %12s %12s %14s\n' "x$L" "$e" "$d" "$tot"
done
echo "done"
