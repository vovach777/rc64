#!/bin/sh
# Roundtrip test: encode + decode + verify on all 6 datasets
# Usage: roundtrip.sh <build_dir>
set -e
BIN="${1:-.}"

mkdir -p "$BIN/test"

for ds in lorem ccode english russian repeat random; do
    echo "===== $ds ====="
    case $ds in
        lorem)   i=0;;
        ccode)   i=1;;
        english) i=2;;
        russian) i=3;;
        repeat)  i=4;;
        random)  i=5;;
    esac
    "$BIN/gen_data" $i 200000 "$BIN/test/$ds.orig"
    "$BIN/rc_encode" "$BIN/test/$ds.orig" "$BIN/test/$ds.rc"
    "$BIN/rc_decode" "$BIN/test/$ds.rc" "$BIN/test/$ds.dec" "$BIN/test/$ds.orig"
    echo
done

echo "=== ALL ROUNDTRIPS PASSED ==="