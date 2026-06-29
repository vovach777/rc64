#!/bin/sh
# Benchmark RC24 decoder (default = rc_fast_div24) on enwik9.
set -e
BIN="${1:-build}"
SRC=$(cd "$(dirname "$0")" && pwd)
DATA="$SRC/data"; mkdir -p "$DATA"
ENWIK9="$DATA/enwik9"; RC24="$DATA/enwik9.rc24"

if [ ! -f "$ENWIK9" ]; then
    if [ ! -f "$DATA/enwik9.zip" ]; then
        echo "Downloading enwik9.zip..."
        curl -L -o "$DATA/enwik9.zip" https://mattmahoney.net/dc/enwik9.zip
    fi
    echo "Extracting enwik9..."
    unzip -o "$DATA/enwik9.zip" -d "$DATA"
fi
echo "enwik9: $(wc -c < "$ENWIK9") bytes"

if [ ! -f "$RC24" ]; then
    echo "Encoding enwik9 -> enwik9.rc24 ..."
    "$BIN/rc24_encode" "$ENWIK9" "$RC24" no_progress
fi
SZ=$(wc -c < "$RC24")
BPB=$(awk "BEGIN{printf \"%.3f\", $SZ*8/1000000000}")
echo "rc24: $SZ bytes ($BPB bpb)"
echo ""

median5() {
    vals=""
    for i in $(seq 1 5); do
        v=$("$@" 2>/dev/null | sed -n 's/.*dec_ticks *: *\([0-9]*\).*/\1/p')
        [ -n "$v" ] || v=0
        vals="$vals $v"
    done
    echo "$vals" | tr ' ' '\n' | sort -n | tr '\n' ' ' | awk '{print $3}'
}

echo "Benchmark: median dec_ticks (5 runs)"
echo "=========================================="
T=$(median5 "$BIN/rc24_decode" "$RC24" /dev/null no_progress)
echo "dec_ticks_rc24_lut24 : $T"
echo "done"
