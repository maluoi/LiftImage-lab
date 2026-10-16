#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "stb_image.h"
#include "lfi.h"
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc, char** argv) {
	int w, h, c; uint8_t* px = stbi_load(argv[1], &w, &h, &c, 4);
	size_t size; uint8_t* d = lfi_encode(px, w, h, &(lfi_opt_t){ .step = atof(argv[2]), .channels = atoi(argv[3]), .data = argc > 4 }, &size);
	lfi_info_t info; lfi_info(d, size, &info);
	void* scratch = malloc(lfi_scratch_size(d, size));
	int16_t* cache[8] = {0}; uint8_t* direct[8]; uint8_t* stepped[8];
	for (int m = 0; m < info.mips; m++) {
		size_t n = (size_t)((w + (1 << m) - 1) >> m) * ((h + (1 << m) - 1) >> m) * info.pixel_size;
		direct[m] = malloc(n); stepped[m] = malloc(n); cache[m] = malloc(lfi_cache_size(d, size, m));
		lfi_decode(d, size, m, 0, direct[m], scratch);
	}
	// Coarsest to full, one level at a time, each from the previous cache
	int top = info.mips - 1, ok = 1;
	lfi_decode_step(d, size, NULL, 0, top, 0, cache[top], stepped[top], scratch);
	for (int m = top - 1; m >= 0; m--) lfi_decode_step(d, size, cache[m + 1], m + 1, m, 0, cache[m], stepped[m], scratch);
	for (int m = 0; m < info.mips; m++) {
		size_t n = (size_t)((w + (1 << m) - 1) >> m) * ((h + (1 << m) - 1) >> m) * info.pixel_size;
		if (memcmp(direct[m], stepped[m], n)) { printf("mip %d differs\n", m); ok = 0; }
	}
	// Timing: full decode vs mip 2 -> 1 -> 0 with previews, best of 20
	double full = 1e9, prog = 1e9;
	for (int i = 0; i < 20; i++) {
		double t = now(); lfi_decode(d, size, 0, 0, direct[0], scratch); t = now() - t; if (t < full) full = t;
		t = now();
		lfi_decode_step(d, size, NULL, 0, 2, 0, cache[2], stepped[2], scratch);
		lfi_decode_step(d, size, cache[2], 2, 1, 0, cache[1], stepped[1], scratch);
		lfi_decode_step(d, size, cache[1], 1, 0, 0, NULL, stepped[0], scratch);
		t = now() - t; if (t < prog) prog = t;
	}
	printf("%s: all %d mips identical: %s; full %.2f ms, mip 2 -> 1 -> 0 with previews %.2f ms (%.2fx), cache at mip 1 %.1f KB\n",
		argv[1], info.mips, ok ? "yes" : "NO", full * 1e3, prog * 1e3, prog / full, lfi_cache_size(d, size, 1) / 1024.0);
}
