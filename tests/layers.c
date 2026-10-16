#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stb_image.h"
#include "lfi.h"
// usage: layers step cubemap face_files... : encodes the faces as one layered file and checks each layer
// against encoding it alone, every mip prefix, and progressive steps
static double psnr(const uint8_t* a, const uint8_t* b, size_t n) {
	double e = 0;
	for (size_t i = 0; i < n; i++) if (i % 4 != 3) e += (double)(a[i] - b[i]) * (a[i] - b[i]);
	return e ? 10 * log10(255.0 * 255 * (n * 3 / 4) / e) : 99;
}
int main(int argc, char** argv) {
	float step = atof(argv[1]); int cube = atoi(argv[2]), layers = argc - 3, w, h, c, ok = 1;
	uint8_t* px = NULL;
	for (int i = 0; i < layers; i++) {
		uint8_t* p = stbi_load(argv[3 + i], &w, &h, &c, 4);
		px = realloc(px, (size_t)w * h * 4 * layers);
		memcpy(px + (size_t)i * w * h * 4, p, (size_t)w * h * 4);
	}
	size_t size; uint8_t* d = lfi_encode(px, w, h, &(lfi_opt_t){ .step = step, .layers = layers, .cubemap = cube }, &size);
	lfi_info_t info; ok &= d && lfi_info(d, size, &info) == lfi_ok && info.layers == layers && info.cubemap == cube && lfi_verify(d, size) == lfi_ok;
	size_t layer_bytes = (size_t)w * h * 4, alone_total = 0;
	uint8_t* out = malloc(layer_bytes * layers), * one = malloc(layer_bytes);
	void* scratch = malloc(lfi_scratch_size(d, size));
	ok &= lfi_decode(d, size, 0, 0, out, scratch) == lfi_ok;
	for (int i = 0; i < layers; i++) {
		size_t s1; uint8_t* d1 = lfi_encode(px + i * layer_bytes, w, h, &(lfi_opt_t){ .step = step }, &s1);
		void* sc1 = malloc(lfi_scratch_size(d1, s1));
		lfi_decode(d1, s1, 0, 0, one, sc1);
		alone_total += s1;
		printf("  layer %d: psnr %.2f in the array, %.2f alone\n", i, psnr(px + i * layer_bytes, out + i * layer_bytes, layer_bytes), psnr(px + i * layer_bytes, one, layer_bytes));
		free(d1); free(sc1);
	}
	// Prefixes and progressive steps match direct decodes at every mip
	int16_t* cache[8] = {0}; uint8_t* direct = malloc(layer_bytes * layers), * stepped = malloc(layer_bytes * layers);
	for (int m = info.mips - 1; m >= 0; m--) {
		size_t n = lfi_mip_bytes(d, size, m), bytes = (size_t)((w + (1 << m) - 1) >> m) * ((h + (1 << m) - 1) >> m) * 4 * layers;
		cache[m] = malloc(lfi_cache_size(d, size, m));
		ok &= lfi_decode(d, n, m, 0, direct, scratch) == lfi_ok && lfi_decode(d, n - 1, m, 0, direct, scratch) == lfi_error_truncated;
		ok &= lfi_decode(d, n, m, 0, direct, scratch) == lfi_ok;
		ok &= lfi_decode_step(d, n, m + 1 < info.mips ? cache[m + 1] : NULL, m + 1, m, 0, cache[m], stepped, scratch) == lfi_ok;
		ok &= !memcmp(direct, stepped, bytes);
	}
	printf("%d layers%s at step %g: %zu bytes, %zu alone, checks %s\n", layers, cube ? " (cubemap)" : "", step, size, alone_total, ok ? "ok" : "FAILED");
	return !ok;
}
