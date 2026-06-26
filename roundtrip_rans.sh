#!/bin/sh
# Roundtrip test for the rANS engine.
# rans_encode + rans_decode + verify (cmp) on all 6 datasets.
# Usage: roundtrip_rans.sh <build_dir> [enc_bin] [dec_bin]
#   enc_bin default: rans_encode
#   dec_bin default: rans_decode
set -e
BIN="${1:-.}"
ENC="${2:-rans_encode}"
DEC="${3:-rans_decode}"

mkdir -p "$BIN/testrans"

for ds in lorem ccode english russian repeat random; do
    echo "===== $ds (rANS engine, 14-bit total) ====="
    case $ds in
        lorem)   i=0;;
        ccode)   i=1;;
        english) i=2;;
        russian) i=3;;
        repeat)  i=4;;
        random)  i=5;;
    esac
    "$BIN/gen_data" $i 200000 "$BIN/testrans/$ds.orig"
    "$BIN/$ENC" "$BIN/testrans/$ds.orig" "$BIN/testrans/$ds.rans"
    "$BIN/$DEC" "$BIN/testrans/$ds.rans" "$BIN/testrans/$ds.dec"
    if cmp -s "$BIN/testrans/$ds.orig" "$BIN/testrans/$ds.dec"; then
        IN=$(wc -c < "$BIN/testrans/$ds.orig")
        OUT=$(wc -c < "$BIN/testrans/$ds.rans")
        BPB=$(awk "BEGIN{printf \"%.3f\", $OUT*8/$IN}")
        echo "  ROUNDTRIP OK: $IN bytes -> $OUT bytes ($BPB bits/byte)"
    else
        echo "  ROUNDTRIP FAIL"
        exit 1
    fi
    echo
done

echo "=== ALL rANS ROUNDTRIPS PASSED ==="