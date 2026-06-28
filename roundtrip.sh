#!/bin/sh
# Roundtrip test: encode + decode + verify (cmp) on all 6 datasets
# Usage: roundtrip.sh <build_dir> [enc_bin] [dec_bin]
#   enc_bin default: rc_encode
#   dec_bin default: rc_decode
set -e
BIN="${1:-.}"
ENC="${2:-rc_encode}"
DEC="${3:-rc_decode}"

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
    "$BIN/$ENC" "$BIN/test/$ds.orig" "$BIN/test/$ds.rc" no_progress >/dev/null 2>&1
    "$BIN/$DEC" "$BIN/test/$ds.rc" "$BIN/test/$ds.dec" no_progress >/dev/null 2>&1
    if cmp -s "$BIN/test/$ds.orig" "$BIN/test/$ds.dec"; then
        echo "  ROUNDTRIP OK: $(wc -c < "$BIN/test/$ds.orig") bytes"
    else
        echo "  ROUNDTRIP FAIL"
        exit 1
    fi
    echo
done

echo "=== ALL ROUNDTRIPS PASSED ==="
