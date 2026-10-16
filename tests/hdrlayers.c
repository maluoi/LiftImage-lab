// Two identical HDR layers must decode identically in every output format. Build with LFI_ENCODER_QUALITY=0, since
// rate-aware quantization learns from earlier tiles and so codes a repeated layer differently
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stb_image.h"
#include "lfi.h"
int main(int argc, char** argv) {
	int w, h, c; float* f = stbi_loadf(argv[1], &w, &h, &c, 4);
	float* two = malloc((size_t)w * h * 16 * 2); memcpy(two, f, (size_t)w * h * 16); memcpy(two + (size_t)w * h * 4, f, (size_t)w * h * 16);
	int ok = 1;
	for (int fmt = 0; fmt < 3; fmt++) {
		size_t size; uint8_t* d = lfi_encode_hdr(two, w, h, &(lfi_opt_t){ .step = 16, .layers = 2 }, &size);
		lfi_info_t info; lfi_info(d, size, &info);
		size_t ps = fmt ? 4 : info.pixel_size, lb = (size_t)w * h * ps;
		uint8_t* out = malloc(lb * 2); void* sc = malloc(lfi_scratch_size(d, size));
		ok &= lfi_decode(d, size, 0, fmt, out, sc) == lfi_ok && !memcmp(out, out + lb, lb);
		printf("format %d: %zu bytes, layers identical %s\n", fmt, size, ok ? "yes" : "NO");
	}
	return !ok;
}
