#!/bin/sh
# Roundtrip test for the FSE engine.
# fse_encode + fse_decode + verify (cmp) on all 6 datasets.
# Usage: roundtrip_fse.sh <build_dir> [enc_bin] [dec_bin]
set -e
BIN="${1:-.}"
ENC="${2:-fse_encode}"
DEC="${3:-fse_decode}"

mkdir -p "$BIN/testfse"

for ds in lorem ccode english russian repeat random; do
    echo "===== $ds (FSE engine, tableLog=14) ====="
    case $ds in
        lorem)   i=0;;
        ccode)   i=1;;
        english) i=2;;
        russian) i=3;;
        repeat)  i=4;;
        random)  i=5;;
    esac
    "$BIN/gen_data" $i 200000 "$BIN/testfse/$ds.orig"
    "$BIN/$ENC" "$BIN/testfse/$ds.orig" "$BIN/testfse/$ds.fse" no_progress >/dev/null 2>&1
    "$BIN/$DEC" "$BIN/testfse/$ds.fse" "$BIN/testfse/$ds.dec" no_progress >/dev/null 2>&1
    if cmp -s "$BIN/testfse/$ds.orig" "$BIN/testfse/$ds.dec"; then
        IN=$(wc -c < "$BIN/testfse/$ds.orig")
        OUT=$(wc -c < "$BIN/testfse/$ds.fse")
        BPB=$(awk "BEGIN{printf \"%.3f\", $OUT*8/$IN}")
        echo "  ROUNDTRIP OK: $IN bytes -> $OUT bytes ($BPB bits/byte)"
    else
        echo "  ROUNDTRIP FAIL"
        exit 1
    fi
    echo
done

echo "=== ALL FSE ROUNDTRIPS PASSED ==="
