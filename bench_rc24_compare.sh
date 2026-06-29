#!/bin/sh
# Benchmark RC24 single vs RC24IL vs RC24S on enwik9.
set -e
BIN="${1:-build}"
SRC=$(cd "$(dirname "$0")" && pwd)
DATA="$SRC/data"; mkdir -p "$DATA"
ENWIK9="$DATA/enwik9"

if [ ! -f "$ENWIK9" ]; then
    if [ ! -f "$DATA/enwik9.zip" ]; then
        echo "Downloading enwik9.zip..."
        curl -L -o "$DATA/enwik9.zip" https://mattmahoney.net/dc/enwik9.zip
    fi
    echo "Extracting enwik9..."
    unzip -o "$DATA/enwik9.zip" -d "$DATA"
fi
SZ=$(wc -c < "$ENWIK9")
echo "enwik9: $SZ bytes"

run_one() {
    "$1" "$2" "$3" no_progress 2>&1 | grep -E 'ticks|bpb|mbs' | tail -5
}

median3() {
    vals=""
    for i in $(seq 1 3); do
        v=$("$@" no_progress 2>/dev/null | sed -n 's/.*ticks *: *\([0-9]*\).*/\1/p')
        [ -n "$v" ] || v=0
        vals="$vals $v"
    done
    echo "$vals" | tr ' ' '\n' | sort -n | tr '\n' ' ' | awk '{print $2}'
}

echo ""
echo "=== Encode ==="
echo "rc24 single:"
run_one "$BIN/rc24_encode" "$ENWIK9" "$DATA/enwik9.rc24"
echo "rc24il N=4:"
run_one "$BIN/rc24il_encode" "$ENWIK9" "$DATA/enwik9.rc24il"
echo "rc24s N=4 shared:"
run_one "$BIN/rc24s_encode" "$ENWIK9" "$DATA/enwik9.rc24s"

echo ""
echo "=== Decode (median ticks/sym, 5 runs) ==="
RC24SZ=$(wc -c < "$DATA/enwik9.rc24" | tr -d ' ')
ILSZ=$(wc -c < "$DATA/enwik9.rc24il" | tr -d ' ')
SSZ=$(wc -c < "$DATA/enwik9.rc24s" | tr -d ' ')
BPB24=$(awk "BEGIN{printf \"%.3f\", ($RC24SZ*8)/$SZ}")
BPBIL=$(awk "BEGIN{printf \"%.3f\", ($ILSZ*8)/$SZ}")
BPBS=$(awk "BEGIN{printf \"%.3f\", ($SSZ*8)/$SZ}")
echo "rc24 single   : bpb=$BPB24  dec_ticks=$(median3 "$BIN/rc24_decode" "$DATA/enwik9.rc24" /dev/null)"
echo "rc24il N=4    : bpb=$BPBIL  dec_ticks=$(median3 "$BIN/rc24il_decode" "$DATA/enwik9.rc24il" /dev/null)"
echo "rc24s N=4 sha : bpb=$BPBS  dec_ticks=$(median3 "$BIN/rc24s_decode" "$DATA/enwik9.rc24s" /dev/null)"

echo ""
echo "done"
