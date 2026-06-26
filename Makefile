# Makefile for rc64 (fallback для окружений без cmake)
# Сборка:
#   make            — 64+32 bit движки с LUT (default)
#   make nolut      — 64+32 bit движки БЕЗ LUT (DISABLE_LUT)
#   make fp         — 32 bit + LUT + FP-деление (USE_FLOAT_DIV)
#   make all5       — все 5 вариантов (для бенчмарка)
# Тесты:
#   make roundtrip   / make roundtrip32      (с LUT)
#   make roundtrip_nl / make roundtrip32_nl  (без LUT)
#   make roundtrip32_fp                      (с FP-делением)

CC      ?= gcc
CXX     ?= g++
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
LDFLAGS ?=
LDLIBS  ?= -lm

# Конфигурации по умолчанию (с LUT)
BIN_64 = rc_encode rc_decode gen_data rc_diag
BIN_32 = rc_encode32 rc_decode32
BIN_RANS = rans_encode rans_decode

# Варианты без LUT
BIN_64_NL = rc_encode_nl rc_decode_nl
BIN_32_NL = rc_encode32_nl rc_decode32_nl

# Варианты с FP-делением (32-bit + LUT + double division)
BIN_32_FP = rc_encode32_fp rc_decode32_fp

all: $(BIN_64) $(BIN_32) $(BIN_RANS)

nolut: $(BIN_64_NL) $(BIN_32_NL)

fp: $(BIN_32_FP)

all5: all nolut fp

# === 64-битный движок (с LUT по умолчанию) ===
rc_encode: rc_encode.c rc_codec.h model.h timer.h zpl.h test_data.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

rc_decode: rc_decode.c rc_codec.h model.h timer.h zpl.h test_data.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

rc_diag: rc_diag.c rc_codec.h model.h timer.h zpl.h test_data.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

gen_data: gen_data.c test_data.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

# === 32-битный движок (с LUT по умолчанию) ===
rc_encode32: rc_encode32.c rc_codec_32.h model_12.h zpl.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

rc_decode32: rc_decode32.c rc_codec_32.h model_12.h zpl.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

# === rANS движок (64-bit state, 32-bit renorm, C++) ===
rans_encode: rans_encode.cpp rans_codec.h model.h zpl.h
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

rans_decode: rans_decode.cpp rans_codec.h model.h zpl.h
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

# === 64-битный движок БЕЗ LUT (DISABLE_LUT) ===
rc_encode_nl: rc_encode.c rc_codec.h model.h timer.h zpl.h test_data.h
	$(CC) $(CFLAGS) -DDISABLE_LUT $< -o $@ $(LDFLAGS) $(LDLIBS)

rc_decode_nl: rc_decode.c rc_codec.h model.h timer.h zpl.h test_data.h
	$(CC) $(CFLAGS) -DDISABLE_LUT $< -o $@ $(LDFLAGS) $(LDLIBS)

# === 32-битный движок БЕЗ LUT (DISABLE_LUT) ===
rc_encode32_nl: rc_encode32.c rc_codec_32.h model_12.h zpl.h
	$(CC) $(CFLAGS) -DDISABLE_LUT $< -o $@ $(LDFLAGS) $(LDLIBS)

rc_decode32_nl: rc_decode32.c rc_codec_32.h model_12.h zpl.h
	$(CC) $(CFLAGS) -DDISABLE_LUT $< -o $@ $(LDFLAGS) $(LDLIBS)

# === 32-битный движок с FP-делением (USE_FLOAT_DIV) ===
rc_encode32_fp: rc_encode32.c rc_codec_32.h model_12.h zpl.h
	$(CC) $(CFLAGS) -DUSE_FLOAT_DIV $< -o $@ $(LDFLAGS) $(LDLIBS)

rc_decode32_fp: rc_decode32.c rc_codec_32.h model_12.h zpl.h
	$(CC) $(CFLAGS) -DUSE_FLOAT_DIV $< -o $@ $(LDFLAGS) $(LDLIBS)

# === Тесты ===
roundtrip: $(BIN_64)
	sh roundtrip.sh .

roundtrip32: $(BIN_32) gen_data
	sh roundtrip32.sh .

roundtrip_rans: $(BIN_RANS) gen_data
	sh roundtrip_rans.sh .

roundtrip_nl: $(BIN_64_NL) gen_data
	sh roundtrip.sh . rc_encode_nl rc_decode_nl

roundtrip32_nl: $(BIN_32_NL) gen_data
	sh roundtrip32.sh . rc_encode32_nl rc_decode32_nl

roundtrip32_fp: $(BIN_32_FP) gen_data
	sh roundtrip32.sh . rc_encode32_fp rc_decode32_fp

test: roundtrip roundtrip32 roundtrip_rans

clean:
	rm -f $(BIN_64) $(BIN_32) $(BIN_RANS) $(BIN_64_NL) $(BIN_32_NL) $(BIN_32_FP)
	rm -rf test test32 testrans bench

.PHONY: all nolut fp all5 roundtrip roundtrip32 roundtrip_rans roundtrip_nl roundtrip32_nl roundtrip32_fp test clean
