#!/usr/bin/env bash
BIN="${1:-.}"
cd "$BIN" || exit 1
set -e

for name in lorem ccode english russian repeat random; do
    echo "===== $name (RC24 shared-buffer 2-way) ====="
    ./gen_data $name 200000 data.tmp
    ./rc24_2way_encode data.tmp out.rc24s2w no_progress
    ./rc24_2way_decode out.rc24s2w data.dec no_progress
    if cmp -s data.tmp data.dec; then
        sz=$(stat -f%z out.rc24s2w 2>/dev/null || stat -c%s out.rc24s2w 2>/dev/null)
        printf "  ROUNDTRIP OK: 200000 bytes -> %s bytes (%.3f bits/byte)\n" "$sz" "$(echo "scale=3; $sz * 8 / 200000" | bc)"
    else
        echo "  ROUNDTRIP FAIL"
        exit 1
    fi
done
rm -f data.tmp out.rc24s2w data.dec

echo "=== ALL RC24 SHARED-BUFFER 2-WAY ROUNDTRIPS PASSED ==="
