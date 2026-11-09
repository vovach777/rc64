#!/usr/bin/env bash
# Correctness suite for the ADDITIVE reserve-ahead variant.
# Existing roundtrip24_2way.sh is left untouched.
BIN="${1:-.}"
cd "$BIN" || exit 1
set -e
# NOTE: gen_data takes a NUMERIC dataset id (0-5), not a name. The pre-existing
# roundtrip*.sh scripts pass names, so atoi() collapses them all to id 0.
# The additive suite below uses the real ids; existing scripts are untouched.
NAMES=(lorem ccode english russian repeat random)
for N in 1 2 4 8; do
    for id in 0 1 2 3 4 5; do
        name=${NAMES[$id]}
        ./gen_data "$id" 200000 data.tmp
        ./rc24_reserve_encode data.tmp out.rc24rsv "$N" no_progress > enc.log
        ./rc24_reserve_decode out.rc24rsv data.dec no_progress > dec.log
        if cmp -s data.tmp data.dec; then
            sz=$(stat -f%z out.rc24rsv 2>/dev/null || stat -c%s out.rc24rsv 2>/dev/null)
            printf "  N=%s %-8s OK  %s bytes (%.3f bpb)\n" \
                "$N" "$name" "$sz" "$(echo "scale=3; $sz * 8 / 200000" | bc)"
        else
            echo "  N=$N $name ROUNDTRIP FAIL"
            exit 1
        fi
    done
done
rm -f data.tmp out.rc24rsv data.dec enc.log dec.log
echo "=== ALL RC24 RESERVE-AHEAD ROUNDTRIPS PASSED (N=1,2,4,8) ==="
