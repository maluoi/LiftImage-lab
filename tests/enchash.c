// usage: enchash images... : encodes each image at several steps and options, printing a hash of every file, so
// builds from different compilers and CPU levels can be compared. .hdr files go through the HDR encoder.
#include "lfi.h"
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned fnv(const unsigned char* p, size_t n) { unsigned h = 2166136261u; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }
int main(int argc, char** argv) {
	float steps[] = { 0, 8, 32 };
	for (int f = 1; f < argc; f++) {
		int w, h, c, hdr = strstr(argv[f], ".hdr") != NULL;
		void* px = hdr ? (void*)stbi_loadf(argv[f], &w, &h, &c, 4) : (void*)stbi_load(argv[f], &w, &h, &c, 4);
		if (!px) { printf("%s unreadable\n", argv[f]); continue; }
		for (int s = 0; s < 3; s++)
			for (int v = 0; v < (hdr ? 2 : 4); v++) {
				lfi_opt_t opt;
				memset(&opt, 0, sizeof(opt));
				opt.step = steps[s];
				if (v == 1) { if (hdr) opt.channels = 1; else opt.dark = 4; }
				if (v == 2) { opt.data = 1; opt.channels = 4; }
				if (v == 3) { opt.channels = 4; opt.premultiplied = 1; }
				size_t size;
				unsigned char* file = hdr ? lfi_encode_hdr((float*)px, w, h, &opt, &size) : lfi_encode((unsigned char*)px, w, h, &opt, &size);
				const char* name = strrchr(argv[f], '/') ? strrchr(argv[f], '/') + 1 : argv[f];
				printf("%s %g %d %zu %08x\n", name, steps[s], v, size, fnv(file, size));
				free(file);
			}
		free(px);
	}
	return 0;
}
