# EMBERCAST — native + wasm32 (no libc)
CC      ?= gcc
WASI    ?= $(WASI_SDK)
WASM_CC ?= $(WASI)/bin/clang
CFLAGS  ?= -O3 -ffast-math -Wall -Wextra -std=c11
NATIVE  ?= embercast

WASM_FLAGS = --target=wasm32 -nostdlib -O3 -ffast-math -flto \
	-Wl,--no-entry \
	-Wl,--export-memory \
	-Wl,--initial-memory=16777216 \
	-Wl,--max-memory=16777216 \
	-Wl,-z,stack-size=1048576 \
	-Wl,--lto-O3

.PHONY: all native wasm test clean

all: native

native: $(NATIVE)

$(NATIVE): embercast.c native_main.c embercast.h
	$(CC) $(CFLAGS) -o $@ embercast.c native_main.c -lm

wasm: embercast.wasm

embercast.wasm: embercast.c embercast.h
	@if [ -z "$(WASI)" ] || [ ! -x "$(WASM_CC)" ]; then \
		echo "Set WASI_SDK to a wasi-sdk root (needs bin/clang)"; exit 1; \
	fi
	$(WASM_CC) $(WASM_FLAGS) -o $@ embercast.c
	@if [ -d ../public ]; then cp $@ ../public/engine.wasm; fi

test: $(NATIVE)
	./$(NATIVE) --selftest

clean:
	rm -f $(NATIVE) embercast.wasm
