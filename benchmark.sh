#!/bin/bash
# Benchmark: 64-bit (Schindler, 14-bit total) vs 32-bit (carry, 12-bit total)
# engine: encoder + decoder.
#
# Прогоняет каждый движок 3 раза на каждом датасете, усредняет ticks/symbol.
# Использует встроенный в binaries rdtsc-замер (не зависит от системной нагрузки).
#
# Запуск: ./benchmark.sh
set -e
cd "$(dirname "$0")"

# 6 датасетов × 2 размера
SIZES=(5242880 26214400)   # 5 MB, 25 MB
DATASETS=(lorem ccode english russian repeat random)
REPEATS=3

mkdir -p bench

run_one() {
    # $1 = bin, $2 = input, $3 = output
    # парсит: "ticks per symbol"
    # Возвращает "FAIL" если бинарник упал
    local out
    out=$("$1" "$2" "$3" 2>&1 | tail -20) || { echo "FAIL"; return; }
    local tps=$(echo "$out" | grep -oE '[0-9]+ +ticks per symbol' | grep -oE '^[0-9]+' || echo 0)
    echo "$tps"
}

echo "============================================================"
echo "BENCHMARK: 64-bit vs 32-bit range coder (encoder + decoder)"
echo "  CPUs: rdtsc-based timing inside binaries"
echo "  Each cell: avg of $REPEATS runs (ticks/symbol)"
echo "============================================================"
echo

for SIZE in "${SIZES[@]}"; do
    SIZE_MB=$((SIZE / 1048576))
    echo "===== File size: ${SIZE_MB} MB ====="
    printf "%-10s %6s | %16s | %16s | %12s\n" \
        "dataset" "size" "64-bit enc/dec" "32-bit enc/dec" "enc% / dec%"
    printf "%-10s %6s | %16s | %16s | %12s\n" \
        "----------" "------" "----------------" "----------------" "------------"

    for ds in "${DATASETS[@]}"; do
        case $ds in
            lorem)   i=0;;
            ccode)   i=1;;
            english) i=2;;
            russian) i=3;;
            repeat)  i=4;;
            random)  i=5;;
        esac

        # Генерируем датасет один раз на размер
        src="bench/${ds}_${SIZE}.orig"
        if [ ! -f "$src" ]; then
            ./gen_data $i $SIZE "$src"
        fi

        # 64-bit движок
        enc64_avg="FAIL"; dec64_avg="FAIL"
        if [ "$ds" != "random" ] || [ "$SIZE" -le 5242880 ]; then
            enc64_tps_sum=0; dec64_tps_sum=0; ok=1
            for r in $(seq 1 $REPEATS); do
                t=$(run_one ./rc_encode "$src" "bench/tmp.rc")
                [ "$t" = "FAIL" ] && { ok=0; break; }
                enc64_tps_sum=$((enc64_tps_sum + t))
                t=$(run_one ./rc_decode "bench/tmp.rc" "bench/tmp.dec")
                [ "$t" = "FAIL" ] && { ok=0; break; }
                dec64_tps_sum=$((dec64_tps_sum + t))
            done
            if [ $ok -eq 1 ]; then
                enc64_avg=$((enc64_tps_sum / REPEATS))
                dec64_avg=$((dec64_tps_sum / REPEATS))
            fi
        fi

        # 32-bit движок
        enc32_avg="FAIL"; dec32_avg="FAIL"
        enc32_tps_sum=0; dec32_tps_sum=0; ok=1
        for r in $(seq 1 $REPEATS); do
            t=$(run_one ./rc_encode32 "$src" "bench/tmp.rc32")
            [ "$t" = "FAIL" ] && { ok=0; break; }
            enc32_tps_sum=$((enc32_tps_sum + t))
            t=$(run_one ./rc_decode32 "bench/tmp.rc32" "bench/tmp.dec")
            [ "$t" = "FAIL" ] && { ok=0; break; }
            dec32_tps_sum=$((dec32_tps_sum + t))
        done
        if [ $ok -eq 1 ]; then
            enc32_avg=$((enc32_tps_sum / REPEATS))
            dec32_avg=$((dec32_tps_sum / REPEATS))
        fi

        # Процент: насколько 32-битный быстрее/медленнее 64-битного
        # Положительный % = 32-bit быстрее
        if [ "$enc64_avg" != "FAIL" ] && [ "$enc32_avg" != "FAIL" ] && [ $enc64_avg -gt 0 ]; then
            enc_pct=$(awk "BEGIN{printf \"%+6.1f\", ($enc64_avg-$enc32_avg)/$enc64_avg*100}")
        else enc_pct="  n/a"; fi
        if [ "$dec64_avg" != "FAIL" ] && [ "$dec32_avg" != "FAIL" ] && [ $dec64_avg -gt 0 ]; then
            dec_pct=$(awk "BEGIN{printf \"%+6.1f\", ($dec64_avg-$dec32_avg)/$dec64_avg*100}")
        else dec_pct="  n/a"; fi

        enc64_show=${enc64_avg/FAIL/FAIL}
        dec64_show=${dec64_avg/FAIL/FAIL}
        enc32_show=${enc32_avg/FAIL/FAIL}
        dec32_show=${dec32_avg/FAIL/FAIL}

        printf "%-10s %4dMB | %8s / %-7s | %8s / %-7s | %6s%% %6s%%\n" \
            "$ds" "$SIZE_MB" \
            "$enc64_show" "$dec64_show" \
            "$enc32_show" "$dec32_show" \
            "$enc_pct" "$dec_pct"
    done
    echo
done

# Также выведем размер сжатия для справки
echo "===== Compression ratio (25 MB dataset) ====="
printf "%-10s %12s %12s %8s\n" "dataset" "64-bit out" "32-bit out" "32 vs 64"
printf "%-10s %12s %12s %8s\n" "----------" "----------" "----------" "-------"
SIZE=26214400
for ds in "${DATASETS[@]}"; do
    src="bench/${ds}_${SIZE}.orig"
    [ -f "$src" ] || continue
    # 64-bit движок падает на random 25MB из-за tight buffer allocation
    if [ "$ds" = "random" ]; then
        printf "%-10s %12s %12s %8s\n" "$ds" "n/a (buf)" "n/a" "n/a"
        continue
    fi
    ./rc_encode   "$src" "bench/tmp.rc"   > /dev/null 2>&1 || true
    ./rc_encode32 "$src" "bench/tmp.rc32" > /dev/null 2>&1
    if [ -f "bench/tmp.rc" ]; then
        s64=$(stat -c%s "bench/tmp.rc")
    else
        s64="FAIL"
    fi
    s32=$(stat -c%s "bench/tmp.rc32")
    if [ "$s64" = "FAIL" ]; then
        printf "%-10s %12s %12s %8s\n" "$ds" "$s64" "$s32" "n/a"
    else
        delta=$(awk "BEGIN{printf \"%+5.2f%%\", ($s32-$s64)/$s64*100}")
        printf "%-10s %9d B %9d B %8s\n" "$ds" "$s64" "$s32" "$delta"
    fi
    rm -f bench/tmp.rc bench/tmp.rc32
done

echo
echo "============================================================"
echo "Interpretation:"
echo "  • ticks/symbol < smaller = faster"
echo "  • enc% / dec% > 0 = 32-bit faster than 64-bit (positive direction)"
echo "  • 32-bit engine has no 64-bit division in decoder hot path,"
echo "    so decode should be significantly faster on the same hardware."
echo "============================================================"
