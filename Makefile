# The codec comes from the LiftImage repository cloned inside this one
CORE   ?= LiftImage
CC     ?= cc
ARCH   ?= -march=x86-64-v3
CFLAGS ?= -std=c11 -O3 -g $(ARCH) -Wall -Wextra -Wno-unused-function
CPPFLAGS += -I$(CORE)/src -I$(CORE)/third_party -Ithird_party

CODEC = $(CORE)/src/ent_enc.c $(CORE)/src/ent_dec.c $(CORE)/src/lfi_enc.c $(CORE)/src/lfi_dec.c
OBJ   = build/ent_enc.o build/ent_dec.o build/lfi_enc.o build/lfi_dec.o

# The benchmark also compares against libjpeg-turbo when pkg-config finds it
TURBOJPEG := $(shell pkg-config --exists libturbojpeg 2>/dev/null && echo yes)
ifeq ($(TURBOJPEG),yes)
BENCH_DEFS = -DBENCH_TURBOJPEG $(shell pkg-config --cflags libturbojpeg)
BENCH_LIBS = $(shell pkg-config --libs libturbojpeg)
endif

all: build/bench lfi

$(CORE)/src/lfi.h:
	@echo "Clone the codec first: git clone https://github.com/maluoi/LiftImage.git $(CORE)"; exit 1

build/bench: $(OBJ) build/bench.o build/stb_impl.o
	$(CC) $(CFLAGS) -o $@ $^ $(BENCH_LIBS) -lm

build/bench.o: bench/bench.c $(CORE)/src/lfi.h $(CORE)/src/*.h | build
	$(CC) $(CPPFLAGS) $(BENCH_DEFS) $(CFLAGS) -c -o $@ $<

build/%.o: $(CORE)/src/%.c $(CORE)/src/lfi.h $(CORE)/src/*.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/stb_impl.o: bench/stb_impl.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -c -o $@ $<

# The command line tool, built by the codec's own Makefile with this machine's flags
lfi: $(CORE)/src/lfi.h
	$(MAKE) -C $(CORE) ARCH="$(ARCH)" build/lfi

# Lift against stb and libjpeg-turbo for tools/compare.py, and mozjpeg too with MOZJPEG=<mozjpeg install prefix>.
# mozjpeg's libjpeg headers get a directory of their own, since its turbojpeg.h is older than libjpeg-turbo's.
COMPARE_DEFS = $(BENCH_DEFS)
COMPARE_LIBS = $(BENCH_LIBS)
ifdef MOZJPEG
COMPARE_DEFS += -DCOMPARE_MOZJPEG -Ibuild/mozjpeg_include
COMPARE_LIBS := $(firstword $(wildcard $(MOZJPEG)/lib*/libjpeg.a)) $(COMPARE_LIBS)
endif

compare: build/jpeg_compare

build/jpeg_compare: bench/jpeg_compare.c $(CODEC) $(CORE)/src/*.h build/stb_impl.o | build
	@[ "$(TURBOJPEG)" = yes ] || { echo "make compare needs libjpeg-turbo, found by pkg-config"; exit 1; }
	$(if $(MOZJPEG),mkdir -p build/mozjpeg_include && cp $(addprefix $(MOZJPEG)/include/,jpeglib.h jmorecfg.h jerror.h jconfig.h) build/mozjpeg_include/)
	$(CC) $(CPPFLAGS) $(COMPARE_DEFS) $(CFLAGS) -o $@ bench/jpeg_compare.c $(CODEC) build/stb_impl.o $(COMPARE_LIBS) -lm

# Sanitized prefix, progressive and layer tests, the codec's vector test, and encodes byte-identical across builds
test: lfi
	$(MAKE) -C $(CORE) test
	sh tests/safety.sh

repro:
	sh tests/repro.sh

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all lfi compare test repro clean
