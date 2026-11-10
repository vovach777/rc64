#!/usr/bin/env bash
# ADDITIVE benchmark: 22-bit reserve-ahead (1024-slot uint32 inverse LUT)
# vs 24-bit reserve-ahead (4096-slot uint64 + __uint128_t) vs the untouched
# fixed shared[k*2+i] layout. Nothing is removed; all existing targets run.
BIN="${1:-.}"
cd "$BIN" || exit 1
set -e
DATA="${2:-2}"          # numeric dataset id (gen_data), default 2 = english
SIZE="${3:-2000000}"
RUNS=3

med() { sort -n | sed -n "$(( (RUNS + 1) / 2 ))p"; }
get() { grep -E "^  $1 *:" "$2" | head -1 | sed 's/.*: *//'; }

./gen_data "$DATA" "$SIZE" bench.tmp
echo "dataset=$DATA size=$SIZE runs=$RUNS"
echo
printf "%-4s %-27s %8s %8s %10s %10s %12s %12s\n" \
    "N" "variant" "bpb" "ratio%" "enc_MB/s" "dec_MB/s" "slots" "extra"

run_row() {
    local N="$1" label="$2" enc="$3" dec="$4" ext="$5" field="$6"
    local em="" dm="" bpb="" rat="" sl="" by=""
    for r in $(seq 1 "$RUNS"); do
        ./$enc bench.tmp "bench.$ext" "$N" no_progress > e.log
        ./$dec "bench.$ext" bench.dec no_progress > d.log
        cmp -s bench.tmp bench.dec || { echo "FAIL $label N=$N"; exit 1; }
        em="$em $(get encode_mbs e.log)"
        dm="$dm $(get decode_mbs d.log)"
        bpb="$(get bpb e.log)"
        rat="$(get ratio_pct e.log)"
        sl="$(get slots e.log)"
        by="$(get "$field" e.log | awk '{print $1}')"
    done
    local EM DM
    EM=$(echo "$em" | tr ' ' '\n' | grep -v '^$' | med)
    DM=$(echo "$dm" | tr ' ' '\n' | grep -v '^$' | med)
    printf "%-4s %-27s %8s %8s %10s %10s %12s %12s\n" \
        "$N" "$label" "$bpb" "$rat" "$EM" "$DM" "$sl" "$by"
}

for N in 1 2 4 8; do
    run_row "$N" "rc22 reserve (2^22/1024)" rc22_reserve_encode rc22_reserve_decode rc22rsv bypass
done

echo
echo "-- reference rows (unchanged engines) --"
run_row 1 "rc24 reserve (2^15 CDF)" rc24_reserve_encode rc24_reserve_decode rc24rsv cuts_per_1M_renorms
run_row 2 "rc24 reserve (2^15 CDF)" rc24_reserve_encode rc24_reserve_decode rc24rsv cuts_per_1M_renorms
# fixed layout takes no N argument: run it directly for the historical row
em=""; dm=""; bpb=""; rat=""
for r in $(seq 1 "$RUNS"); do
    ./rc24_2way_encode bench.tmp bench.rc24s2w no_progress > e.log
    ./rc24_2way_decode bench.rc24s2w bench.dec no_progress > d.log
    cmp -s bench.tmp bench.dec || { echo "FAIL fixed"; exit 1; }
    em="$em $(get encode_mbs e.log)"
    dm="$dm $(get decode_mbs d.log)"
    bpb="$(get bpb e.log)"
    rat="$(get ratio_pct e.log)"
done
EM=$(echo "$em" | tr ' ' '\n' | grep -v '^$' | med)
DM=$(echo "$dm" | tr ' ' '\n' | grep -v '^$' | med)
printf "%-4s %-27s %8s %8s %10s %10s %12s %12s\n" \
    "2" "fixed shared[k*2+i]" "$bpb" "$rat" "$EM" "$DM" "max_len*2" "n/a"

echo
echo "  'extra' column is engine-specific (not a common unit):"
echo "    rc22 rows  = bypass triggers (total count for this dataset)"
echo "    rc24 rows  = carry cuts per 1M renorms"
echo "    fixed row  = n/a"
rm -f bench.tmp bench.rc22rsv bench.rc24rsv bench.rc24s2w bench.dec e.log d.log
echo
echo "=== bench_rc22_reserve done ==="
