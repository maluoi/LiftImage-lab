# Lift against WebP, JPEG XL, AVIF and jpegli, for the codec README's rows past JPEG. Needs the development packages
# of libjpeg-turbo, libwebp, libjxl, libavif and highway, plus jpegli built static from libjxl's source:
#   cmake -S $(JXLSRC) -B jxlbuild -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DJPEGXL_ENABLE_JPEGLI=ON
#   cmake --build jxlbuild --target jpegli-static
# then make JXLSRC=<libjxl source> JPEGLI=<the libjpegli-static.a it built>. INC and LIBDIR add search paths.
CORE   ?= ../LiftImage
CC     ?= cc
CFLAGS  = -std=c11 -O3 -g -march=x86-64-v3 -Wall -Wextra -Wno-unused-function
JXLSRC ?= jxlsrc/libjxl-0.11.2
JPEGLI ?= jxlbuild/lib/libjpegli-static.a
CODEC   = $(CORE)/src/ent_enc.c $(CORE)/src/ent_dec.c $(CORE)/src/lfi_enc.c $(CORE)/src/lfi_dec.c
LIBS    = $(LIBDIR) -lturbojpeg -lwebp -lsharpyuv -ljxl -lavif $(JPEGLI) -lhwy -lstdc++ -lm -lpthread

build/compare_modern: compare_modern.c $(CODEC) $(CORE)/src/*.h build/stb_impl.o build/jpegli_wrap.o | build
	$(CC) -I$(CORE)/src -I$(CORE)/third_party $(INC) $(CFLAGS) -o $@ compare_modern.c $(CODEC) build/stb_impl.o build/jpegli_wrap.o $(LIBS)

build/stb_impl.o: ../bench/stb_impl.c | build
	$(CC) -I$(CORE)/third_party -I../third_party $(CFLAGS) -w -c -o $@ $<

build/jpegli_wrap.o: jpegli_wrap.cc | build
	g++ -std=c++17 -O2 -I$(JXLSRC) $(INC) -c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: clean
