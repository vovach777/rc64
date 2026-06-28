#!/bin/sh
# Roundtrip test for the HUF (Huff0) engine.
# huf_encode + huf_decode + verify (cmp) on all 6 datasets.
# Usage: roundtrip_huf.sh <build_dir> [enc_bin] [dec_bin]
set -e
BIN="${1:-.}"
ENC="${2:-huf_encode}"
DEC="${3:-huf_decode}"

mkdir -p "$BIN/testhuf"

for ds in lorem ccode english russian repeat random; do
    echo "===== $ds (HUF engine) ====="
    case $ds in
        lorem)   i=0;;
        ccode)   i=1;;
        english) i=2;;
        russian) i=3;;
        repeat)  i=4;;
        random)  i=5;;
    esac
    "$BIN/gen_data" $i 200000 "$BIN/testhuf/$ds.orig"
    "$BIN/$ENC" "$BIN/testhuf/$ds.orig" "$BIN/testhuf/$ds.huf" no_progress >/dev/null 2>&1
    "$BIN/$DEC" "$BIN/testhuf/$ds.huf" "$BIN/testhuf/$ds.dec" no_progress >/dev/null 2>&1
    if cmp -s "$BIN/testhuf/$ds.orig" "$BIN/testhuf/$ds.dec"; then
        IN=$(wc -c < "$BIN/testhuf/$ds.orig")
        OUT=$(wc -c < "$BIN/testhuf/$ds.huf")
        BPB=$(awk "BEGIN{printf \"%.3f\", $OUT*8/$IN}")
        echo "  ROUNDTRIP OK: $IN bytes -> $OUT bytes ($BPB bits/byte)"
    else
        echo "  ROUNDTRIP FAIL"
        exit 1
    fi
    echo
done

echo "=== ALL HUF ROUNDTRIPS PASSED ==="
