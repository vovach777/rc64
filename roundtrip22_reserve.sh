#!/usr/bin/env bash
# Correctness suite for the ADDITIVE rc22_reserve variant.
# Existing roundtrip*.sh scripts are left untouched.
BIN="${1:-.}"
cd "$BIN" || exit 1
set -e
# gen_data takes a NUMERIC dataset id (0-5), not a name.
NAMES=(lorem ccode english russian repeat random)

echo "== static datasets, 200000 bytes, N=1,2,4,8 =="
for N in 1 2 4 8; do
    for id in 0 1 2 3 4 5; do
        name=${NAMES[$id]}
        ./gen_data "$id" 200000 data.tmp
        ./rc22_reserve_encode data.tmp out.rc22rsv "$N" no_progress > enc.log
        ./rc22_reserve_decode out.rc22rsv data.dec no_progress > dec.log
        if cmp -s data.tmp data.dec; then
            sz=$(stat -c%s out.rc22rsv)
            printf "  N=%s %-8s OK  %s bytes (%.3f bpb)\n" \
                "$N" "$name" "$sz" "$(echo "scale=3; $sz * 8 / 200000" | bc)"
        else
            echo "  N=$N $name ROUNDTRIP FAIL"; exit 1
        fi
    done
done

echo "== size sweep, dataset 2 (english), N=1,2,4,8 =="
for SZ in 0 1 2 3 7 31 255 4096 65537; do
    ./gen_data 2 "$SZ" data.tmp
    for N in 1 2 4 8; do
        ./rc22_reserve_encode data.tmp out.rc22rsv "$N" no_progress > enc.log
        ./rc22_reserve_decode out.rc22rsv data.dec no_progress > dec.log
        cmp -s data.tmp data.dec || { echo "  size=$SZ N=$N ROUNDTRIP FAIL"; exit 1; }
    done
    printf "  size=%-6s OK (N=1,2,4,8)\n" "$SZ"
done

echo "== degenerate models (RLE / two-symbol / all bytes) =="
printf "" > data.tmp                                  # empty
./rc22_reserve_encode data.tmp out.rc22rsv 4 no_progress > enc.log
./rc22_reserve_decode out.rc22rsv data.dec no_progress > dec.log
cmp -s data.tmp data.dec && echo "  empty         OK" || { echo "  empty FAIL"; exit 1; }

head -c 100000 /dev/zero > data.tmp                  # one symbol -> RLE
./rc22_reserve_encode data.tmp out.rc22rsv 4 no_progress > enc.log
./rc22_reserve_decode out.rc22rsv data.dec no_progress > dec.log
cmp -s data.tmp data.dec && echo "  all-zero RLE  OK" || { echo "  RLE FAIL"; exit 1; }

python3 -c "import sys; sys.stdout.buffer.write(bytes([0,1])*50000)" > data.tmp
./rc22_reserve_encode data.tmp out.rc22rsv 4 no_progress > enc.log
./rc22_reserve_decode out.rc22rsv data.dec no_progress > dec.log
cmp -s data.tmp data.dec && echo "  two-symbol    OK" || { echo "  two-symbol FAIL"; exit 1; }

head -c 100000 /dev/urandom > data.tmp               # full 256-symbol alphabet
./rc22_reserve_encode data.tmp out.rc22rsv 8 no_progress > enc.log
./rc22_reserve_decode out.rc22rsv data.dec no_progress > dec.log
cmp -s data.tmp data.dec && echo "  full alphabet OK" || { echo "  alphabet FAIL"; exit 1; }

rm -f data.tmp out.rc22rsv data.dec enc.log dec.log
echo "=== ALL RC22 RESERVE-AHEAD ROUNDTRIPS PASSED (N=1,2,4,8) ==="
