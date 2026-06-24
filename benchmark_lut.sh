#!/bin/bash
# Benchmark 4 configurations:
#   1. 64-bit engine + LUT      (rc_encode / rc_decode)
#   2. 64-bit engine + NoLUT    (rc_encode_nl / rc_decode_nl)
#   3. 32-bit engine + LUT      (rc_encode32 / rc_decode32)
#   4. 32-bit engine + NoLUT    (rc_encode32_nl / rc_decode32_nl)
#
# Decoder hot path:
#   - rc_dec_get_cum: same in all 4 (division)
#   - model_find:     LUT=1 load, NoLUT=8 dependent loads (binary search)
#   - rc_dec_step:    32-bit vs 64-bit arithmetic
#
# Usage: ./benchmark_lut.sh
set -e
cd "$(dirname "$0")"

SIZES=(5242880 26214400)   # 5 MB, 25 MB
DATASETS=(lorem ccode english russian repeat random)
REPEATS=3

mkdir -p bench

run_one() {
    local bin="$1" inp="$2" outp="$3"
    local t
    t=$("$bin" "$inp" "$outp" 2>&1 | tail -20 | grep -oE '[0-9]+ +ticks per symbol' | grep -oE '^[0-9]+') || { echo "FAIL"; return; }
    [ -z "$t" ] && { echo "FAIL"; return; }
    echo "$t"
}

run_engine() {
    # $1=enc_bin, $2=dec_bin, $3=src, $4=label_out_var_name
    local enc="$1" dec="$2" src="$3"
    local enc_sum=0 dec_sum=0 ok=1 t
    for r in $(seq 1 $REPEATS); do
        t=$(run_one "./$enc" "$src" "bench/tmp.out")
        [ "$t" = "FAIL" ] && { ok=0; break; }
        enc_sum=$((enc_sum + t))
        t=$(run_one "./$dec" "bench/tmp.out" "bench/tmp.dec")
        [ "$t" = "FAIL" ] && { ok=0; break; }
        dec_sum=$((dec_sum + t))
    done
    if [ $ok -eq 1 ]; then
        echo "$((enc_sum / REPEATS)) $((dec_sum / REPEATS))"
    else
        echo "FAIL FAIL"
    fi
}

echo "============================================================"
echo "BENCHMARK: 5 configs — 64/32-bit × LUT/NoLUT/FP (encoder + decoder)"
echo "  CPUs: rdtsc-based timing inside binaries"
echo "  Each cell: avg of $REPEATS runs (ticks/symbol)"
echo "============================================================"

for SIZE in "${SIZES[@]}"; do
    SIZE_MB=$((SIZE / 1048576))
    echo
    echo "===== File size: ${SIZE_MB} MB ====="
    printf "%-10s | %14s | %14s | %14s | %14s | %14s\n" \
        "dataset" "64-LUT enc/dec" "64-NoLUT enc/dec" "32-LUT enc/dec" "32-NoLUT enc/dec" "32-LUT-FP enc/dec"
    printf "%-10s-+----------------+----------------+----------------+----------------+----------------\n" \
        "----------"

    for ds in "${DATASETS[@]}"; do
        case $ds in
            lorem)   i=0;;
            ccode)   i=1;;
            english) i=2;;
            russian) i=3;;
            repeat)  i=4;;
            random)  i=5;;
        esac

        src="bench/${ds}_${SIZE}.orig"
        if [ ! -f "$src" ]; then
            ./gen_data $i $SIZE "$src"
        fi

        r1=$(run_engine rc_encode    rc_decode    "$src")
        r2=$(run_engine rc_encode_nl rc_decode_nl "$src")
        r3=$(run_engine rc_encode32  rc_decode32  "$src")
        r4=$(run_engine rc_encode32_nl rc_decode32_nl "$src")
        r5=$(run_engine rc_encode32_fp rc_decode32_fp "$src")

        e1=${r1% *}; d1=${r1#* }
        e2=${r2% *}; d2=${r2#* }
        e3=${r3% *}; d3=${r3#* }
        e4=${r4% *}; d4=${r4#* }
        e5=${r5% *}; d5=${r5#* }

        printf "%-10s | %7s / %-6s | %7s / %-6s | %7s / %-6s | %7s / %-6s | %7s / %-6s\n" \
            "$ds" "$e1" "$d1" "$e2" "$d2" "$e3" "$d3" "$e4" "$d4" "$e5" "$d5"
    done
done

echo
echo "============================================================"
echo "Summary: ticks/symbol — smaller = faster"
echo "  64-LUT     : Schindler 64-bit + LUT (14-bit model)"
echo "  64-NoLUT   : Schindler 64-bit + 8-step binary search"
echo "  32-LUT     : 32-bit carry RC + LUT (12-bit model)"
echo "  32-NoLUT   : 32-bit carry RC + binary search"
echo "  32-LUT-FP  : 32-bit carry RC + LUT + double-precision FP division"
echo "               (replaces divl ~23cyc with divsd ~13cyc + correction)"
echo "============================================================"
