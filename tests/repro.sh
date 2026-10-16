#!/bin/sh
# Encodes a spread of images with gcc and clang at several flag sets, and with MSVC at SSE2 and AVX2 under Wine when
# MSVC_ENV names its environment script, then checks every build wrote the same bytes. Exits 1 if any differ.
cd "$(dirname "$0")/.." || exit 1
C=${CORE:-LiftImage}
B=build/repro
mkdir -p $B && rm -f $B/*.txt
IMAGES="corpus/examples/parrot.png corpus/examples/texture_wood_floor_deck.png corpus/examples/texture_bark_willow.png corpus/kodak/kodim05.pngcorpus/modes/fern_rgba.png corpus/modes/fern_premul_rgba.png corpus/modes/fern_cutout_rgba.png corpus/modes/metal_plate_arm_data.png corpus/modes/rock_rough_gray.png corpus/hdr/cape_hill.hdr corpus/hdr/music_hall_01.hdr corpus/hdr/the_sky_is_on_fire.hdr"
SRC="$C/src/lfi_enc.c $C/src/lfi_dec.c $C/src/ent_enc.c $C/src/ent_dec.c"
build() { name=$1; shift; "$@" -w -I$C/src -I$C/third_party -Ithird_party -o $B/$name tests/enchash.c $SRC bench/stb_impl.c -lm && $B/$name $IMAGES > $B/$name.txt; }
for cc in gcc clang; do for arch in x86-64 x86-64-v3; do build ${cc}_$arch $cc -std=c11 -O3 -march=$arch; done; done
# Builds that contract multiply-adds or skip optimization, as users' default flags might
build gcc_gnu17      gcc   -std=gnu17 -O2 -march=x86-64-v3
build gcc_contract   gcc   -std=c11 -O3 -ffp-contract=fast -march=x86-64-v3
build clang_contract clang -std=c11 -O3 -ffp-contract=fast -march=x86-64-v3
build clang_O0       clang -std=c11 -O0

if [ -n "$MSVC_ENV" ]; then
	. "$MSVC_ENV" > /dev/null 2>&1
	mkdir -p $B/msvc && cp $C/src/*.c $C/src/*.h $C/third_party/*.h third_party/*.h bench/stb_impl.c tests/enchash.c $B/msvc/
	for arch in "" "/arch:AVX2"; do
		name=msvc$( [ -n "$arch" ] && echo _avx2)
		(cd $B/msvc && timeout 900 cl /nologo /std:c11 /O2 $arch /D_CRT_SECURE_NO_WARNINGS enchash.c lfi_enc.c lfi_dec.c ent_enc.c ent_dec.c stb_impl.c /Fe:$name.exe > /dev/null 2>&1) || { echo "$name build failed"; continue; }
		wine $B/msvc/$name.exe $IMAGES 2>/dev/null | tr -d '\r' > $B/$name.txt
	done
fi

differ=0
for f in $B/*.txt; do
	count=$(diff $B/gcc_x86-64.txt $f | grep -c '^>')
	echo "$(basename $f .txt): $(wc -l < $f) files, $count differ from gcc x86-64"
	[ $count = 0 ] || differ=1
done
exit $differ
