# rc64 — Schindler 64-bit Range Coder

Статический order-0 range coder на базе алгоритма Михаила Шиндлера.
64-битный state, 32-битный shift, carry через uint64 overflow.
Inplace cache/FF — нет deferred FF, нет циклов записи на safe emit.

## Сборка

```
mkdir build && cd build
cmake ..
make
make roundtrip      # 64-bit движок (14-битная модель, uint32_t слова)
make roundtrip32    # 32-bit движок (12-битная модель, uint16_t слова)
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

### 32-битный движок (RC_TOTAL_BITS=12, 16-битные слова)
```
./rc_encode32 <input_file> <output_file.rc32>
./rc_decode32 <input_file.rc32> <output_file>
```

### Тест roundtrip (все 6 датасетов)
```
make roundtrip      # 64-битный движок
make roundtrip32    # 32-битный движок
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
| `rc_codec.h` | Inplace кодек (энкодер + декодер, always_inline). 64-битный Schindler. |
| `model.h` | Статическая order-0 модель (14 бит, total=16384). Для 64-битного движка. |
| `rc_encode.c` | Кодер (64-битный движок). |
| `rc_decode.c` | Декодер (64-битный движок, streaming output, чистый замер). |
| `rc_codec_32.h` | 32-битный in-place carry range coder (16-битные слова, RC_TOTAL_BITS=12). |
| `model_12.h` | Статическая order-0 модель (12 бит, total=4096). Для 32-битного движка. |
| `rc_encode32.c` | Кодер 32-битного движка. |
| `rc_decode32.c` | Декодер 32-битного движка. |
| `timer.h` | Кроссплатформенный int64 таймер. |
| `test_data.h` | Наборы данных: LOREM, CCODE, ENGLISH, RUSSIAN, REPEAT, RANDOM. |
| `gen_data.c` | Генератор тестовых данных. |
| `rc_diag.c` | Диагностика веток кодера. |
| `roundtrip.sh` | Roundtrip тесты для 64-битного движка. |
| `roundtrip32.sh` | Roundtrip тесты для 32-битного движка. |

## 32-битный движок (rc_codec_32.h)

Альтернативный range coder, не зависящий от 64-битной арифметики. Проверен
на 13+ датасетах (равномерные/скошенные бинарные, 16/256-символьные uniform,
Zipf, AR(1), Laplace, реальный текст, /dev/urandom, periodic, edge cases).

Принцип:
- `r = range >> 12`, `low += r * cum_freq`, `range = r * freq`
- Carry ripple: при переполнении 32-битного `low` инкрементируем уже
  записанные 16-битные слова назад, пока перенос не погаснет.
- Renorm: пока `range < 2^16` — выдвигаем старшее слово `low` в буфер.

Запас точности range = `RC_TOP_BITS - RC_TOTAL_BITS = 4 бита`. Это даёт
нижнюю границу overhead ~0.1–0.2% на подходящих данных. Точность 64-битного
движка выше (14-битная модель + 50 бит range), но 32-битный движок переносится
на платформы без 64-битных умножений (Xtensa, MIPS32, Cortex-M0).

Формат `.rc32` идентичен `.rc` по структуре (524 байта заголовка + поток),
но:
- сигнатура `'r','3'` вместо `'r','c'`
- поток состоит из `uint16_t` слов (а не `uint32_t`)
- модель 12-битная (`TARGET_TOTAL_12 = 4096`) вместо 14-битной