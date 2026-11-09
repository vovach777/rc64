#!/usr/bin/env bash
# ADDITIVE benchmark: reserve-ahead (dynamic slots) vs the existing fixed
# shared[k*N+i] layout. Nothing is removed; existing targets still run.
BIN="${1:-.}"
cd "$BIN" || exit 1
set -e
# gen_data wants a numeric dataset id (0-5); default 2 = ENGLISH.
DATA="${2:-2}"
SIZE="${3:-2000000}"
RUNS=3

med() { sort -n | sed -n "$(( (RUNS + 1) / 2 ))p"; }
get() { grep -E "^  $1 *:" "$2" | head -1 | sed 's/.*: *//'; }

./gen_data "$DATA" "$SIZE" bench.tmp
echo "dataset=$DATA size=$SIZE runs=$RUNS"
echo
printf "%-6s %-26s %10s %8s %10s %10s %12s %10s\n" \
    "N" "variant" "bpb" "ratio%" "enc_MB/s" "dec_MB/s" "slots" "cuts/1M"

for N in 1 2 4 8; do
    em=""; dm=""; bpb=""; rat=""; sl=""; c1=""
    for r in $(seq 1 "$RUNS"); do
        ./rc24_reserve_encode bench.tmp bench.rc24rsv "$N" no_progress > e.log
        ./rc24_reserve_decode bench.rc24rsv bench.dec no_progress > d.log
        cmp -s bench.tmp bench.dec || { echo "FAIL N=$N"; exit 1; }
        em="$em $(get encode_mbs e.log)"
        dm="$dm $(get decode_mbs d.log)"
        bpb="$(get bpb e.log)"
        rat="$(get ratio_pct e.log)"
        sl="$(get slots e.log)"
        c1="$(get cuts_per_1M_renorms e.log)"
    done
    EM=$(echo "$em" | tr ' ' '\n' | grep -v '^$' | med)
    DM=$(echo "$dm" | tr ' ' '\n' | grep -v '^$' | med)
    printf "%-6s %-26s %10s %8s %10s %10s %12s %10s\n" \
        "$N" "reserve-ahead (dynamic)" "$bpb" "$rat" "$EM" "$DM" "$sl" "$c1"
done

echo
echo "-- reference: existing fixed shared[k*2+i] layout (unchanged) --"
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
printf "%-6s %-26s %10s %8s %10s %10s %12s %10s\n" \
    "2" "fixed shared[k*2+i]" "$bpb" "$rat" "$EM" "$DM" "max_len*2" "n/a"

rm -f bench.tmp bench.rc24rsv bench.rc24s2w bench.dec e.log d.log
echo
echo "=== bench_rc24_reserve done ==="
