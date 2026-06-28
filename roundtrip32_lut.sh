#!/bin/sh
# Roundtrip test for the 32-bit RC engine with the LUT (Variant D) decoder.
# rc_encode32 (unchanged, no division) + rc_decode32_lutdiv + verify (cmp).
# Usage: roundtrip32_lut.sh <build_dir> [enc_bin] [dec_bin]
set -e
BIN="${1:-.}"
ENC="${2:-rc_encode32}"
DEC="${3:-rc_decode32_lutdiv}"

mkdir -p "$BIN/test32lut"

for ds in lorem ccode english russian repeat random; do
    echo "===== $ds (RC32 + LUT decoder, Variant D) ====="
    case $ds in
        lorem)   i=0;;
        ccode)   i=1;;
        english) i=2;;
        russian) i=3;;
        repeat)  i=4;;
        random)  i=5;;
    esac
    "$BIN/gen_data" $i 200000 "$BIN/test32lut/$ds.orig"
    "$BIN/$ENC" "$BIN/test32lut/$ds.orig" "$BIN/test32lut/$ds.rc32" no_progress >/dev/null 2>&1
    "$BIN/$DEC" "$BIN/test32lut/$ds.rc32" "$BIN/test32lut/$ds.dec" no_progress >/dev/null 2>&1
    if cmp -s "$BIN/test32lut/$ds.orig" "$BIN/test32lut/$ds.dec"; then
        IN=$(wc -c < "$BIN/test32lut/$ds.orig")
        OUT=$(wc -c < "$BIN/test32lut/$ds.rc32")
        BPB=$(awk "BEGIN{printf \"%.3f\", $OUT*8/$IN}")
        echo "  ROUNDTRIP OK: $IN bytes -> $OUT bytes ($BPB bits/byte)"
    else
        echo "  ROUNDTRIP FAIL"
        exit 1
    fi
    echo
done

echo "=== ALL RC32 LUT ROUNDTRIPS PASSED ==="