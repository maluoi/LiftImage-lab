// Lift Image against JPEG encoders over a quality sweep, for tools/compare.py. Prints one CSV line per image, setting
// and quality: setting,param,image,pixels,bytes,psnr,encode_seconds,decode_seconds. JPEGs decode with libjpeg-turbo,
// except stb's, which decode with stb_image. Duplicate images are skipped. With -d, each decoded image is also written
// as <dir>/<setting>_<param>_<image>.png before its line, for perceptual metrics.
// usage: jpeg_compare [-d dir] <images...>
// Build with make compare, or make compare MOZJPEG=<mozjpeg install prefix> to add mozjpeg.

#define _POSIX_C_SOURCE 199309L
#include "lfi.h"
#include "stb_image.h"
#include "stb_image_write.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <turbojpeg.h>
#ifdef COMPARE_MOZJPEG
#include <jpeglib.h>
#endif

typedef enum { codec_lift, codec_stb, codec_turbo, codec_mozjpeg } codec_e;

typedef struct {
	const char* name;
	codec_e     codec;
	bool        sub444;      // 4:4:4 chroma, else 4:2:0
	bool        progressive;
	bool        tune_psnr;   // mozjpeg's flat quantization tables and trellis tuned for PSNR, as cjpeg -tune-psnr
} setting_t;

static const setting_t settings[] = {
	{ .name = "lift",                  .codec = codec_lift },
	{ .name = "stb",                   .codec = codec_stb },
	{ .name = "turbo",                 .codec = codec_turbo },
	{ .name = "turbo_progressive_420", .codec = codec_turbo,   .progressive = true },
	{ .name = "turbo_progressive_444", .codec = codec_turbo,   .progressive = true, .sub444 = true },
#ifdef COMPARE_MOZJPEG
	{ .name = "mozjpeg_420",           .codec = codec_mozjpeg },
	{ .name = "mozjpeg_444",           .codec = codec_mozjpeg, .sub444 = true },
	{ .name = "mozjpeg_psnr_420",      .codec = codec_mozjpeg, .tune_psnr = true },
	{ .name = "mozjpeg_psnr_444",      .codec = codec_mozjpeg, .tune_psnr = true, .sub444 = true },
#endif
};

static const float lift_steps[]    = { 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32 };
static const float jpeg_qualities[] = { 50, 55, 60, 65, 70, 75, 80, 85, 90, 95 };

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

typedef struct {
	uint8_t* data;
	size_t   size, capacity;
} sink_t;

static void sink(void* context, void* data, int size) {
	sink_t* s = context;
	if (s->size + (size_t)size > s->capacity) {
		s->capacity = (s->size + (size_t)size) * 2;
		s->data     = realloc(s->data, s->capacity);
	}
	memcpy(s->data + s->size, data, (size_t)size);
	s->size += (size_t)size;
}

static uint8_t* turbo_encode(const uint8_t* rgba, int32_t w, int32_t h, int32_t quality, setting_t s, size_t* out_size) {
	static tjhandle handle;
	if (!handle) handle = tj3Init(TJINIT_COMPRESS);
	tj3Set(handle, TJPARAM_QUALITY, quality);
	tj3Set(handle, TJPARAM_SUBSAMP, s.sub444 ? TJSAMP_444 : TJSAMP_420);
	tj3Set(handle, TJPARAM_PROGRESSIVE, s.progressive);
	unsigned char* jpeg = NULL;
	if (tj3Compress8(handle, rgba, w, 0, h, TJPF_RGBA, &jpeg, out_size)) { fprintf(stderr, "libjpeg-turbo: %s\n", tj3GetErrorStr(handle)); exit(1); }
	uint8_t* out = malloc(*out_size);
	memcpy(out, jpeg, *out_size);
	tj3Free(jpeg);
	return out;
}

#ifdef COMPARE_MOZJPEG
// mozjpeg's default profile is progressive, with trellis quantization and optimized scans
static uint8_t* mozjpeg_encode(const uint8_t* rgba, int32_t w, int32_t h, int32_t quality, setting_t s, size_t* out_size) {
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr       error;
	cinfo.err = jpeg_std_error(&error);
	jpeg_create_compress(&cinfo);
	unsigned char* mem      = NULL;
	unsigned long  mem_size = 0;
	jpeg_mem_dest(&cinfo, &mem, &mem_size);
	cinfo.image_width      = (JDIMENSION)w;
	cinfo.image_height     = (JDIMENSION)h;
	cinfo.input_components = 3;
	cinfo.in_color_space   = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	if (s.tune_psnr) {
		jpeg_c_set_int_param(&cinfo, JINT_BASE_QUANT_TBL_IDX, 1);
		jpeg_c_set_float_param(&cinfo, JFLOAT_LAMBDA_LOG_SCALE1, 9.0f);
		jpeg_c_set_float_param(&cinfo, JFLOAT_LAMBDA_LOG_SCALE2, 0.0f);
		jpeg_c_set_bool_param(&cinfo, JBOOLEAN_USE_LAMBDA_WEIGHT_TBL, FALSE);
	}
	jpeg_set_quality(&cinfo, quality, TRUE);
	cinfo.comp_info[0].h_samp_factor = cinfo.comp_info[0].v_samp_factor = s.sub444 ? 1 : 2;
	for (int32_t c = 1; c < 3; c++) cinfo.comp_info[c].h_samp_factor = cinfo.comp_info[c].v_samp_factor = 1;
	jpeg_start_compress(&cinfo, TRUE);
	uint8_t* row = malloc((size_t)w * 3);
	while (cinfo.next_scanline < cinfo.image_height) {
		const uint8_t* source = rgba + (size_t)cinfo.next_scanline * w * 4;
		for (int32_t x = 0; x < w; x++) memcpy(row + x * 3, source + x * 4, 3);
		JSAMPROW rows[1] = { row };
		jpeg_write_scanlines(&cinfo, rows, 1);
	}
	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	free(row);
	*out_size = mem_size;
	uint8_t* out = malloc(mem_size);
	memcpy(out, mem, mem_size);
	free(mem);
	return out;
}
#endif

static uint8_t* encode(setting_t s, const uint8_t* rgba, int32_t w, int32_t h, float param, size_t* out_size) {
	switch (s.codec) {
	case codec_lift: {
		lfi_opt_t opt = { .step = param, .channels = 3 };
		return lfi_encode(rgba, w, h, &opt, out_size);
	}
	case codec_stb: {
		sink_t out = { 0 };
		stbi_write_jpg_to_func(sink, &out, w, h, 4, rgba, (int)param);
		*out_size = out.size;
		return out.data;
	}
	case codec_turbo: return turbo_encode(rgba, w, h, (int32_t)param, s, out_size);
#ifdef COMPARE_MOZJPEG
	case codec_mozjpeg: return mozjpeg_encode(rgba, w, h, (int32_t)param, s, out_size);
#else
	case codec_mozjpeg: break;
#endif
	}
	return NULL;
}

// Into out_rgba, w x h
static void decode(setting_t s, const uint8_t* data, size_t size, void* scratch, uint8_t* out_rgba) {
	static tjhandle handle;
	if (s.codec == codec_lift) {
		if (lfi_decode(data, size, 0, lfi_format_native, out_rgba, scratch) != lfi_ok) { fprintf(stderr, "lfi_decode failed\n"); exit(1); }
		return;
	}
	if (s.codec == codec_stb) {
		int32_t  w, h, c;
		uint8_t* pixels = stbi_load_from_memory(data, (int)size, &w, &h, &c, 4);
		memcpy(out_rgba, pixels, (size_t)w * h * 4);
		stbi_image_free(pixels);
		return;
	}
	if (!handle) handle = tj3Init(TJINIT_DECOMPRESS);
	if (tj3DecompressHeader(handle, data, size) || tj3Decompress8(handle, data, size, out_rgba, 0, TJPF_RGBA)) { fprintf(stderr, "libjpeg-turbo: %s\n", tj3GetErrorStr(handle)); exit(1); }
}

static double psnr(const uint8_t* a, const uint8_t* b, size_t pixels) {
	double error = 0;
	for (size_t i = 0; i < pixels; i++)
		for (int32_t c = 0; c < 3; c++) {
			double d = (double)a[i * 4 + c] - b[i * 4 + c];
			error += d * d;
		}
	error /= (double)pixels * 3;
	return error == 0 ? 99 : 10 * log10(255.0 * 255.0 / error);
}

int main(int argc, char** argv) {
	uint64_t    seen[256];
	int32_t     seen_count = 0, first = 1;
	const char* opt_dump   = NULL;
	if (argc > 2 && !strcmp(argv[1], "-d")) { opt_dump = argv[2]; first = 3; }
	stbi_write_png_compression_level = 1;
	for (int32_t f = first; f < argc; f++) {
		int32_t  w, h, c;
		uint8_t* rgba = stbi_load(argv[f], &w, &h, &c, 4);
		if (!rgba) { fprintf(stderr, "skip %s: unreadable\n", argv[f]); continue; }
		size_t   pixels = (size_t)w * h;
		uint64_t hash   = 0xcbf29ce484222325ull;
		for (size_t i = 0; i < pixels * 4; i++) hash = (hash ^ rgba[i]) * 0x100000001b3ull;
		bool duplicate = false;
		for (int32_t i = 0; i < seen_count; i++) duplicate |= seen[i] == hash;
		if (duplicate || seen_count == 256) { fprintf(stderr, "skip %s: duplicate\n", argv[f]); stbi_image_free(rgba); continue; }
		seen[seen_count++] = hash;

		const char* name = strrchr(argv[f], '/') ? strrchr(argv[f], '/') + 1 : argv[f];
		uint8_t*    out  = malloc(pixels * 4);
		for (size_t si = 0; si < sizeof(settings) / sizeof(settings[0]); si++) {
			setting_t    s      = settings[si];
			const float* params = s.codec == codec_lift ? lift_steps : jpeg_qualities;
			int32_t      count  = s.codec == codec_lift ? 11 : 10;
			for (int32_t p = 0; p < count; p++) {
				size_t   size;
				double   t0      = now();
				uint8_t* data    = encode(s, rgba, w, h, params[p], &size);
				double   encode_s = now() - t0;
				void*    scratch = s.codec == codec_lift ? malloc(lfi_scratch_size(data, size)) : NULL;
				double   decode_s = 1e9;
				for (int32_t r = 0; r < 3; r++) {
					t0 = now();
					decode(s, data, size, scratch, out);
					double t = now() - t0;
					decode_s = t < decode_s ? t : decode_s;
				}
				int32_t stem    = (int)(strrchr(name, '.') ? strrchr(name, '.') - name : (long)strlen(name));
				double  quality = psnr(rgba, out, pixels);
				if (opt_dump) {
					char path[4096];
					snprintf(path, sizeof(path), "%s/%s_%g_%.*s.png", opt_dump, s.name, params[p], stem, name);
					for (size_t i = 0; i < pixels; i++) memmove(out + i * 3, out + i * 4, 3);
					stbi_write_png(path, w, h, 3, out, w * 3);
				}
				printf("%s,%g,%.*s,%zu,%zu,%.4f,%.6f,%.6f\n", s.name, params[p], stem, name, pixels, size, quality, encode_s, decode_s);
				fflush(stdout);
				free(scratch);
				free(data);
			}
		}
		free(out);
		stbi_image_free(rgba);
	}
	return 0;
}
