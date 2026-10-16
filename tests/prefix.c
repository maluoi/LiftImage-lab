#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stb_image.h"
#include "lfi.h"
// usage: prefix image step channels : every mip decodes from exactly its prefix, one byte less is truncated,
// and lfi_verify catches a flipped byte
int main(int argc, char** argv) {
	int w, h, c; uint8_t* px = stbi_load(argv[1], &w, &h, &c, 4);
	size_t size; uint8_t* d = lfi_encode(px, w, h, &(lfi_opt_t){ .step = atof(argv[2]), .channels = atoi(argv[3]), .data = argc > 4 }, &size);
	lfi_info_t info; int ok = lfi_info(d, size, &info) == lfi_ok && info.file_size == size && lfi_verify(d, size) == lfi_ok;
	lfi_info_t head; ok &= lfi_info(d, size < LFI_HEADER_MAX ? size : LFI_HEADER_MAX, &head) == lfi_ok && head.file_size == size;
	printf("%s: %dx%d, %d mips, %zu bytes\n", argv[1], w, h, info.mips, size);
	void* scratch = malloc(lfi_scratch_size(d, size));
	for (int mip = info.mips - 1; mip >= 0; mip--) {
		size_t n = lfi_mip_bytes(d, size, mip), pixels = (size_t)((w + (1 << mip) - 1) >> mip) * ((h + (1 << mip) - 1) >> mip) * info.pixel_size;
		uint8_t* prefix = malloc(n); memcpy(prefix, d, n);
		uint8_t* a = malloc(pixels), * b = malloc(pixels);
		int ra = lfi_decode(d, size, mip, 0, a, scratch), rb = lfi_decode(prefix, n, mip, 0, b, scratch), rt = lfi_decode(prefix, n - 1, mip, 0, b, scratch);
		int same = ra == lfi_ok && rb == lfi_ok && rt == lfi_error_truncated && !memcmp(a, b, pixels);
		ok &= same;
		printf("  mip %d: prefix %7zu bytes (%5.1f%%), identical %s\n", mip, n, 100.0 * n / size, same ? "yes" : "NO");
		free(prefix); free(a); free(b);
	}
	d[size - 20] ^= 1;
	ok &= lfi_verify(d, size) == lfi_error_checksum;
	printf("  verify %s\n", ok ? "ok" : "FAILED");
	return !ok;
}
