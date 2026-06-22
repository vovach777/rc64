# rc64 — Schindler 64-bit Range Coder

Статический order-0 range coder на базе алгоритма Михаила Шиндлера.
64-битный state, 32-битный shift, carry через uint64 overflow.
Inplace cache/FF — нет deferred FF, нет циклов записи на safe emit.

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
./rc_decode <input_file.rc> <output_file>
```

### Тест roundtrip (все 6 датасетов)
```
make roundtrip
```

### Диагностика (статистика по веткам кодера, без записи)
```
./rc_diag <input_file>
```

## Формат .rc

```
[4 байта]   сигнатура: 'r','c', flags, rle_sym
            flags bit 0: is_rle
[8 байт]    uint64_t original_len (LE)
--- RLE mode (is_rle=1) ---
            (больше ничего — rle_sym в сигнатуре)
--- RC mode (is_rle=0) ---
[512 байт]  cum[1..256] — uint16_t LE (кумулятивные частоты)
            cum[0]=0 не хранится (константа)
[4*N байт]  uint32_t words LE — поток энкодера
```

Заголовок: 4 + 8 + 512 = 524 байта, 32-битно выровнен.
RLE: весь файл = 12 байт.

## Модель

Статическая order-0, total = 2^14 = 16384.
Частоты строятся один раз по всему потоку.
Масштабирование: scaled = raw * 16384 / total_raw.
Если частота > 0 но scaled = 0 — поднимается до 1.
Сумма корректируется до 16384 через максимальную частоту.
RLE если только один активный символ.

## Результаты (enwik9, 1 ГБ, O3, clang x86_64)

```
input:      1,000,000,000 bytes
output:       644,975,732 bytes (64.50%, 5.160 bpb)
encode:         3.7 sec (258.7 MB/s)
decode:        32.2 sec  (29.6 MB/s)  — чистый замер без I/O
roundtrip:   OK
```

### Профиль декодера (ассемблер, 103 такта/символ)

| Компонент | Тактов | % |
|---|---|---|
| `divq` (code/t в get_cum) | 20-30 | 20-29% |
| binary search (8 шагов, cmpl+setae+cmovbq) | ~40 | ~39% |
| `imulq` ×2 (step + range) | ~10 | ~10% |
| renorm (shrq+branch+shlq) | ~10 | ~10% |
| memory loads (cum, words[], out[]) | ~10 | ~10% |
| loop overhead | ~3 | ~3% |

Два bottleneck:
1. `divq` — 64-битное деление code/t, t переменная, неустранимо
2. binary search — 8 dependent memory loads из cum[257] (514 байт = 8 cache lines)

### Оптимизации применённые

- `range / TARGET_TOTAL` → компилятор оптимизирует в `shrq $14` (compile-time constant)
- Binary search в model_find (8 шагов вместо линейного 256)
- Streaming output 16KB с паузой таймера во время fwrite
- int64 timer (timer_ticks/timer_freq) вместо clock()
- cum[1..256] хранится напрямую (без prefix sum в декодере)

## Эксперименты (ветка subbotin_magic)

Ветка subbotin_magic — Subbotin aligned trim вместо carry propagation:
- 64-bit, 16-bit shift, `range = (-low) & 0xFFFF`
- Overhead vs carry: +0.00% (5232 байт на 645 МБ)
- Декодер на 16% быстрее (нет carry patch)

## Файлы

| Файл | Назначение |
|---|---|
| `rc_codec.h` | Inplace кодек (энкодер + декодер, always_inline). |
| `model.h` | Статическая order-0 модель (14 бит, total=16384). |
| `timer.h` | Кроссплатформенный int64 таймер. |
| `test_data.h` | Наборы данных: LOREM, CCODE, ENGLISH, RUSSIAN, REPEAT, RANDOM. |
| `rc_encode.c` | Кодер. |
| `rc_decode.c` | Декодер (streaming output, чистый замер). |
| `gen_data.c` | Генератор тестовых данных. |
| `rc_diag.c` | Диагностика веток кодера. |