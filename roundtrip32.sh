#!/bin/sh
# Roundtrip test for the 32-bit engine.
# encode32 + decode32 + verify (cmp) on all 6 datasets.
# Usage: roundtrip32.sh <build_dir> [enc_bin] [dec_bin]
#   enc_bin default: rc_encode32
#   dec_bin default: rc_decode32
set -e
BIN="${1:-.}"
ENC="${2:-rc_encode32}"
DEC="${3:-rc_decode32}"

mkdir -p "$BIN/test32"

for ds in lorem ccode english russian repeat random; do
    echo "===== $ds (32-bit engine, 12-bit total) ====="
    case $ds in
        lorem)   i=0;;
        ccode)   i=1;;
        english) i=2;;
        russian) i=3;;
        repeat)  i=4;;
        random)  i=5;;
    esac
    "$BIN/gen_data" $i 200000 "$BIN/test32/$ds.orig"
    "$BIN/$ENC" "$BIN/test32/$ds.orig" "$BIN/test32/$ds.rc32" no_progress >/dev/null 2>&1
    "$BIN/$DEC" "$BIN/test32/$ds.rc32" "$BIN/test32/$ds.dec" no_progress >/dev/null 2>&1
    if cmp -s "$BIN/test32/$ds.orig" "$BIN/test32/$ds.dec"; then
        IN=$(wc -c < "$BIN/test32/$ds.orig")
        OUT=$(wc -c < "$BIN/test32/$ds.rc32")
        BPB=$(awk "BEGIN{printf \"%.3f\", $OUT*8/$IN}")
        echo "  ROUNDTRIP OK: $IN bytes -> $OUT bytes ($BPB bits/byte)"
    else
        echo "  ROUNDTRIP FAIL"
        exit 1
    fi
    echo
done

echo "=== ALL 32-BIT ROUNDTRIPS PASSED ==="
