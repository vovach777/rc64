# Makefile for rc64 (fallback для окружений без cmake)
# Сборка: make
# Тесты:  make roundtrip   make roundtrip32

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
LDFLAGS ?=
LDLIBS  ?= -lm

BIN_64 = rc_encode rc_decode gen_data rc_diag
BIN_32 = rc_encode32 rc_decode32

all: $(BIN_64) $(BIN_32)

# 64-битный движок
rc_encode: rc_encode.c rc_codec.h model.h timer.h zpl.h test_data.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

rc_decode: rc_decode.c rc_codec.h model.h timer.h zpl.h test_data.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

rc_diag: rc_diag.c rc_codec.h model.h timer.h zpl.h test_data.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

gen_data: gen_data.c test_data.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

# 32-битный движок
rc_encode32: rc_encode32.c rc_codec_32.h model_12.h zpl.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

rc_decode32: rc_decode32.c rc_codec_32.h model_12.h zpl.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

# Тесты
roundtrip: $(BIN_64)
	sh roundtrip.sh .

roundtrip32: $(BIN_32) gen_data
	sh roundtrip32.sh .

test: roundtrip roundtrip32

clean:
	rm -f $(BIN_64) $(BIN_32)
	rm -rf test test32

.PHONY: all roundtrip roundtrip32 test clean
