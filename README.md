# rc64 — Schindler 64-bit Range Coder

Статический order-0 range coder на базе алгоритма Михаила Шиндлера.
64-битный state, 32-битный shift, carry через uint64 overflow.
Inplace cache/FF — нет deferred FF, нет циклов записи на safe emit.

## Сборка

```
mkdir build && cd build
cmake ..
make
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
Если указан original_file — проверяет roundtrip побайтово.

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
[512 байт]  freq[256] — uint16_t LE (индивидуальные частоты)
[4*N байт]  uint32_t words LE — поток энкодера
```

Заголовок 32-битно выровнен: 4 + 8 = 12, затем 512 (кратно 4), затем поток.
RLE: весь файл = 12 байт.

## Модель

Статическая order-0, total = 2^14 = 16384.
Частоты строятся один раз по всему потоку.
Масштабирование: scaled = raw * 16384 / total_raw.
Если частота > 0 но scaled = 0 — поднимается до 1.
Сумма корректируется до 16384 через максимальную частоту.
RLE если только один активный символ.

## Результаты (enwik9, 1 ГБ, O3)

```
input:      1,000,000,000 bytes
output:       644,975,732 bytes (64.50%, 5.160 bpb)
encode:      13.9 sec (68.8 MB/s)
decode:      71.7 sec (13.3 MB/s)
roundtrip:   OK
```

## Файлы

| Файл | Назначение |
|---|---|
| `rc_codec.h` | Inplace кодек (энкодер + декодер, always_inline). |
| `model.h` | Статическая order-0 модель (14 бит, total=16384). |
| `test_data.h` | Наборы данных: LOREM, CCODE, ENGLISH, RUSSIAN, REPEAT, RANDOM. |
| `rc_encode.c` | Кодер. |
| `rc_decode.c` | Декодер + roundtrip verify. |
| `gen_data.c` | Генератор тестовых данных. |
| `rc_diag.c` | Диагностика веток кодера. |