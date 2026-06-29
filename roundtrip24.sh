#!/bin/sh
# Roundtrip test for the RC24 (Subbotin carryless) engine.
# Usage: roundtrip24.sh <build_dir> [enc_bin] [dec_bin]
set -e
BIN="${1:-.}"
ENC="${2:-rc24_encode}"
DEC="${3:-rc24_decode}"
mkdir -p "$BIN/test24"
for ds in lorem ccode english russian repeat random; do
    echo "===== $ds (RC24 engine) ====="
    case $ds in
        lorem)   i=0;; ccode)   i=1;; english) i=2;;
        russian) i=3;; repeat)  i=4;; random)  i=5;;
    esac
    "$BIN/gen_data" $i 200000 "$BIN/test24/$ds.orig"
    "$BIN/$ENC" "$BIN/test24/$ds.orig" "$BIN/test24/$ds.rc24" no_progress >/dev/null 2>&1
    "$BIN/$DEC" "$BIN/test24/$ds.rc24" "$BIN/test24/$ds.dec" no_progress >/dev/null 2>&1
    if cmp -s "$BIN/test24/$ds.orig" "$BIN/test24/$ds.dec"; then
        IN=$(wc -c < "$BIN/test24/$ds.orig")
        OUT=$(wc -c < "$BIN/test24/$ds.rc24")
        BPB=$(awk "BEGIN{printf \"%.3f\", $OUT*8/$IN}")
        echo "  ROUNDTRIP OK: $IN bytes -> $OUT bytes ($BPB bits/byte)"
    else
        echo "  ROUNDTRIP FAIL"; exit 1
    fi
    echo
done
echo "=== ALL RC24 ROUNDTRIPS PASSED ==="
