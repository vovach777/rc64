# rc64 — Schindler 64-bit Range Coder (v3: Subbotin magic)

Range coder на базе алгоритма Михаила Шиндлера, с Subbotin aligned trim
вместо carry propagation. 64-битный state, 16-битный сдвиг, trim к 2^16-блоку.

## Версии

- **v2 (main)**: inplace carry propagation (cache + deferred FF). 32-битный сдвиг.
- **v3 (subbotin_magic)**: Subbotin aligned trim. 16-битный сдвиг, uint16 I/O.

## Сборка

```
mkdir build && cd build
cmake ..
make
make roundtrip
```

## Использование

### Кодер
```
./rc_encode <input_file> <output_file.rc>
```

### Декодер
```
./rc_decode <input_file.rc> <output_file> [original_file]
```

## Формат .rc (v3)

```
[4 байта]   сигнатура: 'r','c', flags, rle_sym
[8 байт]    uint64_t original_len (LE)
--- RLE mode (is_rle=1) ---
            (больше ничего)
--- RC mode (is_rle=0) ---
[512 байт]  freq[256] — uint16_t LE (индивидуальные частоты)
[2*N байт]  uint16_t words BE — поток энкодера (big-endian)
```

## Модель

Статическая order-0, total = 2^14 = 16384. (см. v2 README)

## Subbotin aligned trim

Вместо carry propagation (cache + deferred FF) — обрезка интервала:
```
if (low + range < low)  // переполнение uint64
    range = (-low) & 0xFFFF;  // расстояние до границы 2^16-блока
```

Интервал [low, low+range] остаётся внутри одного 2^16-блока.
Никакого "плывания" — математически точная траектория.
Overhead vs carry propagation: 0.00% (неощутимо).

## Результаты (enwik9, 1 ГБ, O3)

```
input:      1,000,000,000 bytes
output:       644,980,956 bytes (64.50%, 5.1598 bpb)
encode:      14.8 sec (64.4 MB/s)
decode:      53.4 sec (17.9 MB/s)
roundtrip:   OK
```

Сравнение вариантов на enwik9:

| Вариант | overhead | enc | dec |
|---|---|---|---|
| v2 carry propagation (main) | — | 13.3s | 62.0s |
| 32-bit 8-bit IO | +4.91% | 12.7s | 50.3s |
| 32-bit 16-bit IO | +2.74% | 10.6s | 49.8s |
| 64-bit 32-bit naive | +1.21% | 13.8s | 51.3s |
| **v3 64-bit 16-bit Subbotin** | **+0.00%** | **14.8s** | **53.4s** |

## Файлы

| Файл | Назначение |
|---|---|
| `rc_codec.h` | Subbotin codec (энкодер + декодер, always_inline). |
| `model.h` | Статическая order-0 модель (14 бит, total=16384). |
| `test_data.h` | Наборы данных: LOREM, CCODE, ENGLISH, RUSSIAN, REPEAT, RANDOM. |
| `rc_encode.c` | Кодер (uint16 BE output). |
| `rc_decode.c` | Декодер + roundtrip verify. |
| `gen_data.c` | Генератор тестовых данных. |
| `rc_diag.c` | Диагностика веток кодера. |