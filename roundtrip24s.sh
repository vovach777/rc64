#!/bin/sh
# Roundtrip test for the RC24S (shared-buffer super-interleaved) engine.
set -e
BIN="${1:-.}"
ENC="${2:-rc24s_encode}"
DEC="${3:-rc24s_decode}"
mkdir -p "$BIN/test24s"
for ds in lorem ccode english russian repeat random; do
    echo "===== $ds (RC24S engine) ====="
    case $ds in
        lorem)   i=0;; ccode)   i=1;; english) i=2;;
        russian) i=3;; repeat)  i=4;; random)   i=5;;
    esac
    "$BIN/gen_data" $i 200000 "$BIN/test24s/$ds.orig"
    "$BIN/$ENC" "$BIN/test24s/$ds.orig" "$BIN/test24s/$ds.rc24s" no_progress >/dev/null 2>&1
    "$BIN/$DEC" "$BIN/test24s/$ds.rc24s" "$BIN/test24s/$ds.dec" no_progress >/dev/null 2>&1
    if cmp -s "$BIN/test24s/$ds.orig" "$BIN/test24s/$ds.dec"; then
        IN=$(wc -c < "$BIN/test24s/$ds.orig" | tr -d ' ')
        OUT=$(wc -c < "$BIN/test24s/$ds.rc24s" | tr -d ' ')
        BPB=$(awk "BEGIN{printf \"%.3f\", $OUT*8/$IN}")
        echo "  ROUNDTRIP OK: $IN bytes -> $OUT bytes ($BPB bits/byte)"
    else
        echo "  ROUNDTRIP FAIL"; exit 1
    fi
    echo
done
echo "=== ALL RC24S ROUNDTRIPS PASSED ==="
