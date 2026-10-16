#!/bin/sh
# Builds the prefix, progressive and layer tests with ASan and UBSan against the codec's sources, and runs them over
# modes and steps. Prints one line per run, a FAIL line for any failure or sanitizer report, and exits 1 on failure.
cd "$(dirname "$0")/.." || exit 1
C=${CORE:-LiftImage}
B=build/tests
mkdir -p $B
CC="gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I$C/src -I$C/third_party"
SRC="$C/src/lfi_enc.c $C/src/lfi_dec.c $C/src/ent_enc.c $C/src/ent_dec.c build/stb_impl.o"
for t in prefix prog layers; do $CC tests/$t.c $SRC -lm -o $B/$t || exit 1; done
$CC -DLFI_ENCODER_QUALITY=0 tests/hdrlayers.c $SRC -lm -o $B/hdrlayers || exit 1
export ASAN_OPTIONS=detect_leaks=0
failed=0
fail() { echo "FAIL $1"; echo "$2" | tail -5; failed=1; }

for args in "corpus/modes/metal_plate_arm_data.png 16 3 data" "corpus/modes/metal_plate_arm_data.png 0 4 data" "corpus/modes/metal_plate_arm_data.png 8 4 data" \
            "corpus/examples/parrot.png 16 3" "corpus/examples/parrot.png 0 3" "corpus/examples/texture_wood_floor_deck.png 32 3" "corpus/modes/rock_rough_gray.png 16 1" \
            "corpus/modes/texture_normal_nrm.png 8 2" "corpus/modes/fern_rgba.png 16 4" "corpus/modes/fern_premul_rgba.png 16 4" \
            "corpus/modes/fern_cutout_rgba.png 4 4" "corpus/examples/texture_brick_wall_001.png 64 3"; do
	out=$($B/prefix $args 2>&1); rc=$?
	echo "$out" | grep -q "verify ok" && [ $rc = 0 ] && ! echo "$out" | grep -q "ERROR\|runtime error" && echo "prefix ok  $args" || fail "prefix $args" "$out"
	out=$($B/prog $args 2>&1)
	echo "$out" | grep -q "identical: yes" && ! echo "$out" | grep -q "ERROR\|runtime error" && echo "prog ok    $args" || fail "prog $args" "$out"
done
for step in 0 8 16 32; do
	for set in "1 tests/layers/f*.png" "0 tests/layers/a*.png"; do
		out=$($B/layers $step $set 2>&1); echo "$out" | tail -1
		echo "$out" | grep -q "ERROR\|runtime error\|FAIL" && fail "layers $step $set" "$out"
	done
done
out=$($B/hdrlayers corpus/hdr/kloofendal_48d_partly_cloudy_puresky.hdr 2>&1); echo "$out" | tail -1
echo "$out" | grep -q "ERROR\|runtime error\|FAIL" && fail "hdrlayers" "$out"
exit $failed
