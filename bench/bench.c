#define _DEFAULT_SOURCE
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_dxt.h"

#include "ent.h"
#include "lfi.h"

#ifdef BENCH_TURBOJPEG
#include <turbojpeg.h>
#endif

typedef enum {
	codec_src,
	codec_jpg,   // stb_image_write and stb_image
	codec_turbo, // libjpeg-turbo, when the build finds it: baseline 4:2:0 as stb codes it
	codec_turbo_progressive, // its strongest widely decodable setting
	codec_turbo_arithmetic,  // its smallest files: progressive with arithmetic coding, which few decoders read
	codec_lfi,
} codec_e;

typedef struct {
	codec_e codec;
	float   param;
} config_t;

static const config_t configs[] = {
	{ codec_src, 0  },
	{ codec_jpg, 75 }, { codec_jpg, 90 },
#ifdef BENCH_TURBOJPEG
	{ codec_turbo, 75 }, { codec_turbo, 90 },
	{ codec_turbo_progressive, 75 }, { codec_turbo_progressive, 90 },
	{ codec_turbo_arithmetic, 75 }, { codec_turbo_arithmetic, 90 },
#endif
	{ codec_lfi, 0  }, { codec_lfi, 4  }, { codec_lfi, 8  }, { codec_lfi, 16 }, { codec_lfi, 32 },
};
#define CONFIG_COUNT (int32_t)(sizeof(configs) / sizeof(configs[0]))

static bool is_turbo(codec_e codec) { return codec == codec_turbo || codec == codec_turbo_progressive || codec == codec_turbo_arithmetic; }

static const char* codec_tags[] = { [codec_src] = "src", [codec_jpg] = "jpg", [codec_turbo] = "turbo", [codec_turbo_progressive] = "turbo_prog", [codec_turbo_arithmetic] = "turbo_arith", [codec_lfi] = "lfi" };

typedef struct {
	double psnr;   // RGB
	double psnr_y; // BT.601 luma
	double psnr_c; // BT.601 chroma
	double dark;   // RGB error on pixels with source luma < 64, relative to that range. NAN if too few
	double dark_c; // chroma error on those same pixels, where hue shifts show once lit
	double ssim;   // luma, 8x8 box windows
	double gpu;    // coded channels after a GPU block format round trip: BC1 color, BC4 gray, BC5 normals
	double alpha;  // alpha channel, NAN without alpha
	double angle;  // mean normal error in degrees, with Z rebuilt from XY. NAN unless the image is a normal map
} metrics_t;

typedef struct {
	double  bits;
	double  pixels;
	double  dec_seconds;
	double  enc_seconds;
	double  psnr, psnr_y, psnr_c, ssim, gpu;
	double  dark, dark_c;
	int32_t dark_images;
	int32_t images;
} stats_t;

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void write_cb(void* ctx, void* data, int size) { buffer_push(ctx, data, (size_t)size); }

#ifdef BENCH_TURBOJPEG
// Handles live for the whole run, as an app would keep them. Baseline takes 4:2:0 like stb. The stronger settings
// take 4:4:4 from q85, since on the corpus it gives smaller files than 4:2:0 above about 37 dB.
static uint8_t* turbo_encode(codec_e codec, const uint8_t* rgba, int32_t w, int32_t h, int32_t quality, size_t* out_size) {
	static tjhandle handle;
	if (!handle) handle = tj3Init(TJINIT_COMPRESS);
	tj3Set(handle, TJPARAM_QUALITY, quality);
	tj3Set(handle, TJPARAM_SUBSAMP, codec != codec_turbo && quality >= 85 ? TJSAMP_444 : TJSAMP_420);
	tj3Set(handle, TJPARAM_PROGRESSIVE, codec != codec_turbo);
	tj3Set(handle, TJPARAM_ARITHMETIC, codec == codec_turbo_arithmetic);
	unsigned char* jpeg = NULL;
	if (tj3Compress8(handle, rgba, w, 0, h, TJPF_RGBA, &jpeg, out_size)) { fprintf(stderr, "turbo: %s\n", tj3GetErrorStr(handle)); exit(1); }
	uint8_t* out = malloc(*out_size);
	memcpy(out, jpeg, *out_size);
	tj3Free(jpeg);
	return out;
}

static void turbo_decode(const uint8_t* data, size_t size, uint8_t* out_rgba) {
	static tjhandle handle;
	if (!handle) handle = tj3Init(TJINIT_DECOMPRESS);
	if (tj3DecompressHeader(handle, data, size) || tj3Decompress8(handle, data, size, out_rgba, 0, TJPF_RGBA)) { fprintf(stderr, "turbo: %s\n", tj3GetErrorStr(handle)); exit(1); }
}
#endif

static uint8_t* encode(config_t c, const uint8_t* rgba, int32_t w, int32_t h, lfi_opt_t opt, size_t* out_size) {
	opt.step = c.param;
	if (c.codec == codec_lfi) return lfi_encode(rgba, w, h, &opt, out_size);
#ifdef BENCH_TURBOJPEG
	if (is_turbo(c.codec)) return turbo_encode(c.codec, rgba, w, h, (int32_t)c.param, out_size);
#endif

	buffer_t out = {0};
	stbi_write_jpg_to_func(write_cb, &out, w, h, 4, rgba, (int)c.param);
	*out_size = out.size;
	return out.data;
}

// Best of several runs
static double encode_timed(config_t c, const uint8_t* rgba, int32_t w, int32_t h, lfi_opt_t opt, uint8_t** out_data, size_t* out_size) {
	double best = 1e9, total = 0;
	*out_data = NULL;
	for (int32_t i = 0; i < 20 && (i < 2 || total < 0.25); i++) {
		free(*out_data);
		double t0 = now();
		*out_data = encode(c, rgba, w, h, opt, out_size);
		double t = now() - t0;
		best   = t < best ? t : best;
		total += t;
	}
	return best;
}

static lfi_info_t info_of(const uint8_t* data, size_t size) {
	lfi_info_t info = {0};
	lfi_info(data, size, &info);
	return info;
}

// Best of several runs of decoding mip 2, then 1, then 0, each step continuing from the last one's cache
static double progressive_timed(const uint8_t* data, size_t size, uint8_t* out_pixels) {
	void*    scratch = malloc(lfi_scratch_size(data, size));
	int16_t* cache2  = malloc(lfi_cache_size(data, size, 2));
	int16_t* cache1  = malloc(lfi_cache_size(data, size, 1));
	double   best    = 1e9, total = 0;
	for (int32_t i = 0; i < 50 && (i < 3 || total < 0.25); i++) {
		double t0 = now();
		lfi_decode_step(data, size, NULL,   0, 2, lfi_format_native, cache2, out_pixels, scratch);
		lfi_decode_step(data, size, cache2, 2, 1, lfi_format_native, cache1, out_pixels, scratch);
		lfi_decode_step(data, size, cache1, 1, 0, lfi_format_native, NULL,   out_pixels, scratch);
		double t = now() - t0;
		best   = t < best ? t : best;
		total += t;
	}
	free(scratch);
	free(cache2);
	free(cache1);
	return best;
}

// Best of several runs. Apps would keep decoder scratch per thread, so it is allocated outside the timing.
// out_pixels gets the decoder's packed channels, or RGBA from stb and libjpeg-turbo.
static double decode_timed(config_t c, const uint8_t* data, size_t size, int32_t mip, uint8_t* out_pixels) {
	void*  scratch = c.codec == codec_lfi ? malloc(lfi_scratch_size(data, size)) : NULL;
	if (c.codec == codec_lfi && lfi_decode(data, size, mip, lfi_format_native, out_pixels, scratch) != lfi_ok) { fprintf(stderr, "decode failed\n"); exit(1); }
	double best    = 1e9, total = 0;
	for (int32_t i = 0; i < 50 && (i < 3 || total < 0.25); i++) {
		double t0 = now();
		if (c.codec == codec_lfi) {
			lfi_decode(data, size, mip, lfi_format_native, out_pixels, scratch);
#ifdef BENCH_TURBOJPEG
		} else if (is_turbo(c.codec)) {
			turbo_decode(data, size, out_pixels);
#endif
		} else {
			int32_t w, h, comp;
			stbi_image_free(stbi_load_from_memory(data, (int)size, &w, &h, &comp, 4));
		}
		double t = now() - t0;
		best   = t < best ? t : best;
		total += t;
	}
	free(scratch);

	if (c.codec != codec_lfi && !is_turbo(c.codec)) {
		int32_t  w, h, comp;
		uint8_t* px = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 4);
		memcpy(out_pixels, px, (size_t)w * h * 4);
		stbi_image_free(px);
	}
	return best;
}

static double to_psnr(double mse, double peak) { return mse == 0 ? 99 : 10 * log10(peak * peak / mse); }

static void ycbcr(const uint8_t* px, double* out_y, double* out_cb, double* out_cr) {
	double r = px[0], g = px[1], b = px[2];
	*out_y  =  0.299    * r + 0.587    * g + 0.114    * b;
	*out_cb = -0.168736 * r - 0.331264 * g + 0.5      * b;
	*out_cr =  0.5      * r - 0.418688 * g - 0.081312 * b;
}

static double ssim_luma(const uint8_t* a, const uint8_t* b, int32_t w, int32_t h) {
	enum { win = 8 };
	if (w < win || h < win) return 1;
	// Summed-area tables of x, y, x^2, y^2, xy
	size_t  sw  = (size_t)w + 1;
	double* sat = calloc(sw * (h + 1) * 5, sizeof(double));
	for (int32_t y = 0; y < h; y++) {
		double row[5] = {0};
		for (int32_t x = 0; x < w; x++) {
			double ya, yb, cb, cr;
			ycbcr(a + ((size_t)y * w + x) * 4, &ya, &cb, &cr);
			ycbcr(b + ((size_t)y * w + x) * 4, &yb, &cb, &cr);
			double v[5] = { ya, yb, ya * ya, yb * yb, ya * yb };
			for (int32_t k = 0; k < 5; k++) {
				row[k] += v[k];
				sat[(((size_t)y + 1) * sw + x + 1) * 5 + k] = sat[((size_t)y * sw + x + 1) * 5 + k] + row[k];
			}
		}
	}
	const double c1 = 6.5025, c2 = 58.5225, n = win * win;
	double       sum = 0;
	for (int32_t y = 0; y + win <= h; y++) {
		for (int32_t x = 0; x + win <= w; x++) {
			double s[5];
			for (int32_t k = 0; k < 5; k++) {
				s[k] = sat[(((size_t)y + win) * sw + x + win) * 5 + k] - sat[((size_t)y * sw + x + win) * 5 + k]
				     - sat[(((size_t)y + win) * sw + x) * 5 + k]       + sat[((size_t)y * sw + x) * 5 + k];
			}
			double ma = s[0] / n, mb = s[1] / n;
			double va = s[2] / n - ma * ma, vb = s[3] / n - mb * mb, cov = s[4] / n - ma * mb;
			sum += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
		}
	}
	free(sat);
	return sum / ((double)(w - win + 1) * (h - win + 1));
}

static void bc1_decode_block(const uint8_t* blk, uint8_t* out_rgba16) {
	uint32_t c[2] = { blk[0] | blk[1] << 8, blk[2] | blk[3] << 8 };
	int32_t  pal[4][3];
	for (int32_t i = 0; i < 2; i++) {
		uint32_t r = c[i] >> 11 & 31, g = c[i] >> 5 & 63, b = c[i] & 31;
		pal[i][0] = (int32_t)(r << 3 | r >> 2);
		pal[i][1] = (int32_t)(g << 2 | g >> 4);
		pal[i][2] = (int32_t)(b << 3 | b >> 2);
	}
	for (int32_t ch = 0; ch < 3; ch++) {
		if (c[0] > c[1]) {
			pal[2][ch] = (2*pal[0][ch] + pal[1][ch]) / 3;
			pal[3][ch] = (pal[0][ch] + 2*pal[1][ch]) / 3;
		} else {
			pal[2][ch] = (pal[0][ch] + pal[1][ch]) / 2;
			pal[3][ch] = 0;
		}
	}
	uint32_t idx = blk[4] | blk[5] << 8 | blk[6] << 16 | (uint32_t)blk[7] << 24;
	for (int32_t i = 0; i < 16; i++, idx >>= 2) {
		for (int32_t ch = 0; ch < 3; ch++) out_rgba16[i*4 + ch] = (uint8_t)pal[idx & 3][ch];
		out_rgba16[i*4 + 3] = 255;
	}
}

// Stand-in for a runtime GPU encode, to measure quality after it
static void bc1_roundtrip(const uint8_t* rgba, int32_t w, int32_t h, uint8_t* out_rgba) {
	for (int32_t by = 0; by < h; by += 4)
		for (int32_t bx = 0; bx < w; bx += 4) {
			uint8_t src[64], dst[64], blk[8];
			for (int32_t i = 0; i < 16; i++) {
				int32_t x = bx + (i & 3), y = by + (i >> 2);
				x = x < w ? x : w - 1;
				y = y < h ? y : h - 1;
				memcpy(src + i*4, rgba + ((size_t)y * w + x) * 4, 4);
			}
			stb_compress_dxt_block(blk, src, 0, STB_DXT_NORMAL);
			bc1_decode_block(blk, dst);
			for (int32_t i = 0; i < 16; i++) {
				int32_t x = bx + (i & 3), y = by + (i >> 2);
				if (x < w && y < h) memcpy(out_rgba + ((size_t)y * w + x) * 4, dst + i*4, 4);
			}
		}
}

// BC4 with min/max endpoints on one channel of RGBA, in place
static void bc4_roundtrip(uint8_t* ref_rgba, int32_t w, int32_t h, int32_t ch) {
	for (int32_t by = 0; by < h; by += 4)
		for (int32_t bx = 0; bx < w; bx += 4) {
			int32_t mn = 255, mx = 0;
			for (int32_t i = 0; i < 16; i++) {
				int32_t x = bx + (i & 3), y = by + (i >> 2);
				if (x >= w || y >= h) continue;
				int32_t v = ref_rgba[((size_t)y * w + x) * 4 + ch];
				mn = v < mn ? v : mn;
				mx = v > mx ? v : mx;
			}
			for (int32_t i = 0; i < 16; i++) {
				int32_t x = bx + (i & 3), y = by + (i >> 2);
				if (x >= w || y >= h) continue;
				uint8_t* v    = &ref_rgba[((size_t)y * w + x) * 4 + ch];
				int32_t  k    = mx > mn ? ((*v - mn) * 7 + (mx - mn) / 2) / (mx - mn) : 0;
				*v = (uint8_t)((mn * (7 - k) + mx * k + 3) / 7);
			}
		}
}

static double normal_angle(const uint8_t* a, const uint8_t* b) {
	double n[2][3];
	for (int32_t k = 0; k < 2; k++) {
		const uint8_t* p = k ? b : a;
		double x = p[0] / 127.5 - 1, y = p[1] / 127.5 - 1, zz = 1 - x * x - y * y;
		n[k][0] = x; n[k][1] = y; n[k][2] = zz > 0 ? sqrt(zz) : 0;
	}
	double dot = 0, la = 0, lb = 0;
	for (int32_t i = 0; i < 3; i++) { dot += n[0][i] * n[1][i]; la += n[0][i] * n[0][i]; lb += n[1][i] * n[1][i]; }
	double c = dot / sqrt(la * lb + 1e-12);
	return acos(c > 1 ? 1 : c < -1 ? -1 : c) * 57.29577951308232;
}

// ref and dec are RGBA views of the coded channels. Color errors skip fully transparent pixels.
static metrics_t measure(const uint8_t* ref, const uint8_t* dec, int32_t w, int32_t h, int32_t mode, bool normal) {
	size_t  pixels = (size_t)w * h;
	int32_t chans  = mode < 3 ? mode : 3;
	double  err = 0, err_y = 0, err_c = 0, err_dark = 0, err_dark_c = 0, err_a = 0, angle = 0;
	size_t  dark = 0, visible = 0;
	for (size_t i = 0; i < pixels; i++) {
		double da = (double)ref[i*4 + 3] - dec[i*4 + 3];
		err_a += da * da;
		if (normal) angle += normal_angle(ref + i*4, dec + i*4);
		if (!ref[i*4 + 3]) continue;
		visible++;

		double ya, cba, cra, yb, cbb, crb, e = 0;
		for (int32_t ch = 0; ch < chans; ch++) {
			double d = (double)ref[i*4 + ch] - dec[i*4 + ch];
			e += d * d;
		}
		ycbcr(ref + i*4, &ya, &cba, &cra);
		ycbcr(dec + i*4, &yb, &cbb, &crb);
		double ec = (cba - cbb) * (cba - cbb) + (cra - crb) * (cra - crb);
		err   += e;
		err_y += (ya - yb) * (ya - yb);
		err_c += ec;
		if (ya < 64) { err_dark += e; err_dark_c += ec; dark++; }
	}

	uint8_t* gpu = malloc(pixels * 4);
	if (mode >= 3) bc1_roundtrip(dec, w, h, gpu);
	else {
		memcpy(gpu, dec, pixels * 4);
		for (int32_t ch = 0; ch < mode; ch++) bc4_roundtrip(gpu, w, h, ch);
	}
	double err_gpu = 0;
	for (size_t i = 0; i < pixels; i++) {
		if (!ref[i*4 + 3]) continue;
		for (int32_t ch = 0; ch < chans; ch++) {
			double d = (double)ref[i*4 + ch] - gpu[i*4 + ch];
			err_gpu += d * d;
		}
	}
	free(gpu);

	bool color = mode >= 3;
	return (metrics_t){
		.psnr   = to_psnr(err / (visible * chans), 255),
		.psnr_y = color ? to_psnr(err_y / visible, 255) : NAN,
		.psnr_c = color ? to_psnr(err_c / (visible * 2), 255) : NAN,
		.dark   = color && dark >= visible / 100 ? to_psnr(err_dark / (dark * 3), 64) : NAN,
		.dark_c = color && dark >= visible / 100 ? to_psnr(err_dark_c / (dark * 2), 64) : NAN,
		.ssim   = mode != 2 ? ssim_luma(ref, dec, w, h) : NAN,
		.gpu    = to_psnr(err_gpu / (visible * chans), 255),
		.alpha  = mode == 4 ? to_psnr(err_a / pixels, 255) : NAN,
		.angle  = normal ? angle / pixels : NAN,
	};
}

static uint8_t* read_file(const char* path, size_t* out_size) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*out_size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* data = malloc(*out_size);
	*out_size = fread(data, 1, *out_size, f);
	fclose(f);
	return data;
}

static bool is_image(const char* path) {
	const char* ext = strrchr(path, '.');
	return ext && (!strcasecmp(ext, ".png") || !strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".hdr"));
}

static int32_t cmp_str(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

static void collect(const char* path, char*** ref_list, int32_t* ref_count) {
	struct stat st;
	if (stat(path, &st)) return;
	if (!S_ISDIR(st.st_mode)) {
		if (!is_image(path)) return;
		*ref_list = realloc(*ref_list, (size_t)(*ref_count + 1) * sizeof(char*));
		(*ref_list)[(*ref_count)++] = strdup(path);
		return;
	}
	DIR* dir = opendir(path);
	if (!dir) return;
	struct dirent* ent;
	while ((ent = readdir(dir))) {
		if (ent->d_name[0] == '.') continue;
		char child[4096];
		snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
		collect(child, ref_list, ref_count);
	}
	closedir(dir);
}

static void stem_of(const char* path, char* out_stem, size_t size) {
	const char* base = strrchr(path, '/');
	const char* stem = base ? base + 1 : path;
	const char* ext  = strrchr(stem, '.');
	snprintf(out_stem, size, "%.*s", (int)(ext ? ext - stem : (long)strlen(stem)), stem);
}

// 1 gray, 2 normal XY, 3 color, 4 color + alpha, from file name tags and the alpha channel
static int32_t coded_channels(const char* stem, const uint8_t* rgba, size_t pixels) {
	if (strstr(stem, "_gray")) return 1;
	if (strstr(stem, "_nrm"))  return 2;
	for (size_t i = 0; i < pixels; i++) if (rgba[i*4 + 3] != 255) return 4;
	return 3;
}

// Expands pixels with have channels, in place, to an RGBA view of the coded channels: gray as RGB, normals with B = 0
static void view(uint8_t* px, size_t pixels, int32_t mode, int32_t have) {
	for (size_t i = pixels; i-- > 0;) {
		uint8_t v[4];
		for (int32_t c = 0; c < 4; c++) v[c] = c < have ? px[i * have + c] : 255;
		uint8_t r = v[0], g = mode == 1 ? v[0] : v[1], b = mode == 1 ? v[0] : mode == 2 ? 0 : v[2];
		px[i*4 + 0] = r;
		px[i*4 + 1] = g;
		px[i*4 + 2] = b;
		px[i*4 + 3] = mode == 4 ? v[3] : 255;
	}
}

static float half_to_float(uint16_t h) {
	uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = h >> 10 & 0x1F, m = h & 0x3FF, bits;
	if (e == 0) {
		float v = (float)m * (1.0f / 16777216);
		return sign ? -v : v;
	}
	bits = sign | (e == 31 ? 0xFFu : e - 15 + 127) << 23 | m << 13;
	float f;
	memcpy(&f, &bits, 4);
	return f;
}

// Tone mapped PSNR over exposures 2 stops either side of the log average, and RMS error in stops
static void measure_hdr(const float* ref, const float* dec, size_t pixels, int32_t channels, double* out_tm_psnr, double* out_stops) {
	double log_sum = 0;
	for (size_t i = 0; i < pixels; i++) log_sum += log(1e-6 + (channels == 1 ? ref[i*4] : 0.2126 * ref[i*4] + 0.7152 * ref[i*4 + 1] + 0.0722 * ref[i*4 + 2]));
	double key = 0.18 / exp(log_sum / pixels), psnr = 0, stops = 0;
	size_t stop_count = 0;
	for (int32_t e = -2; e <= 2; e += 2) {
		double scale = key * pow(2, e), err = 0;
		for (size_t i = 0; i < pixels; i++)
			for (int32_t c = 0; c < channels; c++) {
				double a = ref[i*4 + c] * scale, b = dec[i*4 + c] * scale;
				a = a > 0 ? pow(a / (1 + a), 1 / 2.2) * 255 : 0;
				b = b > 0 ? pow(b / (1 + b), 1 / 2.2) * 255 : 0;
				err += (a - b) * (a - b);
			}
		psnr += to_psnr(err / (pixels * channels), 255) / 3;
	}
	for (size_t i = 0; i < pixels; i++)
		for (int32_t c = 0; c < channels; c++) {
			if (!(ref[i*4 + c] > 6.1e-5f)) continue;
			double d = log2((dec[i*4 + c] > 6e-8f ? dec[i*4 + c] : 6e-8f) / ref[i*4 + c]);
			stops += d * d;
			stop_count++;
		}
	*out_tm_psnr = psnr;
	*out_stops   = stop_count ? sqrt(stops / stop_count) : NAN;
}

static const float hdr_steps[] = { 0, 8, 16, 32, 64 };

// Writes <name>_ev<e>.png at exposures 2 stops apart around the source's log average, with the metric's curve
static void dump_hdr(const char* dir, const char* name, const float* px, const float* ref, size_t w, size_t h) {
	double log_sum = 0;
	for (size_t i = 0; i < w * h; i++) log_sum += log(1e-6 + 0.2126 * ref[i*4] + 0.7152 * ref[i*4 + 1] + 0.0722 * ref[i*4 + 2]);
	double   key = 0.18 / exp(log_sum / (w * h));
	uint8_t* out = malloc(w * h * 3);
	for (int32_t e = -2; e <= 2; e += 2) {
		double scale = key * pow(2, e);
		for (size_t i = 0; i < w * h; i++)
			for (int32_t c = 0; c < 3; c++) {
				double v = px[i*4 + c] * scale;
				out[i*3 + c] = (uint8_t)lround(v > 0 ? pow(v / (1 + v), 1 / 2.2) * 255 : 0);
			}
		char path[4096];
		snprintf(path, sizeof(path), "%s/%s_ev%+d.png", dir, name, e);
		stbi_write_png(path, (int)w, (int)h, 3, out, (int)w * 3);
	}
	free(out);
}

static double ufloat_to_double(uint32_t v, int32_t mantissa) {
	uint32_t e = v >> mantissa, m = v & ((1u << mantissa) - 1);
	return e ? ldexp(1.0 + (double)m / (1 << mantissa), (int)e - 15) : ldexp((double)m / (1 << mantissa), -14);
}

static double decode_hdr_timed(const uint8_t* data, size_t size, lfi_format_e format, uint8_t* out_pixels) {
	void*  scratch = malloc(lfi_scratch_size(data, size));
	if (lfi_decode(data, size, 0, format, out_pixels, scratch) != lfi_ok) { fprintf(stderr, "decode failed\n"); exit(1); }
	double best    = 1e9, total = 0;
	for (int32_t i = 0; i < 50 && (i < 3 || total < 0.25); i++) {
		double t0 = now();
		lfi_decode(data, size, 0, format, out_pixels, scratch);
		double t = now() - t0;
		best   = t < best ? t : best;
		total += t;
	}
	free(scratch);
	return best;
}

static void bench_hdr(const char* path, const char* opt_dump_dir, FILE* opt_csv, bool verbose) {
	int32_t w, h, comp;
	float*  ref = stbi_loadf(path, &w, &h, &comp, 4);
	if (!ref) { fprintf(stderr, "skip %s: %s\n", path, stbi_failure_reason()); return; }
	char stem[256];
	stem_of(path, stem, sizeof(stem));
	size_t   pixels   = (size_t)w * h;
	int32_t  channels = strstr(stem, "_gray") ? 1 : 3;
	uint8_t* out      = malloc(pixels * 8);
	float*   dec      = malloc(pixels * 4 * sizeof(float));
	if (verbose) printf("%s (%dx%d HDR, %d channels)\n", path, w, h, channels);

	size_t   src_size = 0;
	uint8_t* src      = read_file(path, &src_size);
	double   src_s    = 1e9, src_total = 0;
	for (int32_t i = 0; i < 20 && (i < 2 || src_total < 0.25); i++) {
		double t0 = now();
		stbi_image_free(stbi_loadf_from_memory(src, (int)src_size, &comp, &comp, &comp, 4));
		double t = now() - t0;
		src_s      = t < src_s ? t : src_s;
		src_total += t;
	}
	free(src);
	if (verbose) printf("  src      %7.3f bpp  dec %7.2f ms\n", src_size * 8.0 / pixels, src_s * 1e3);
	if (opt_dump_dir) {
		char name[512];
		snprintf(name, sizeof(name), "%s_src", stem);
		dump_hdr(opt_dump_dir, name, ref, ref, (size_t)w, (size_t)h);
	}
	if (opt_csv) fprintf(opt_csv, "%s,src,0,%d,%d,%zu,%.6f,0,99,nan,nan,nan,nan,nan,nan,nan,nan,%d,nan,nan,nan,nan,nan,0,1,nan\n", stem, w, h, src_size, src_s * 1e3, channels);

	for (int32_t si = 0; si < (int32_t)(sizeof(hdr_steps) / sizeof(hdr_steps[0])); si++) {
		lfi_opt_t opt  = { .step = hdr_steps[si], .channels = channels };
		size_t    size = 0;
		uint8_t*  data = NULL;
		double    enc  = 1e9, enc_total = 0;
		for (int32_t i = 0; i < 20 && (i < 2 || enc_total < 0.25); i++) {
			free(data);
			double t0 = now();
			data = lfi_encode_hdr(ref, w, h, &opt, &size);
			double t = now() - t0;
			enc        = t < enc ? t : enc;
			enc_total += t;
		}
		for (int32_t f = 0; f < 3; f++) {
			double    dec_s = decode_hdr_timed(data, size, (lfi_format_e)f, out);
			uint16_t* half  = (uint16_t*)out;
			uint32_t* px    = (uint32_t*)out;
			int32_t   psize = info_of(data, size).pixel_size / 2;
			for (size_t i = 0; i < pixels; i++) {
				for (int32_t k = 0; k < 4; k++) {
					float v = 0;
					if      (f == lfi_format_rg11b10f) v = k < 3 ? (float)ufloat_to_double(px[i] >> (11 * k) & (k < 2 ? 0x7FF : 0x3FF), k < 2 ? 6 : 5) : 0;
					else if (f == lfi_format_rgb9e5)   v = k < 3 ? (float)ldexp(px[i] >> (9 * k) & 0x1FF, (int)(px[i] >> 27) - 24) : 0;
					else if (k < psize)                v = half_to_float(half[i * psize + k]);
					dec[i*4 + k] = v;
				}
			}

			static const char* tags[] = { "lfi", "lfi11", "lfi9e5" };
			if (opt_dump_dir && f == lfi_format_native) {
				char name[512];
				snprintf(name, sizeof(name), "%s_%s%g", stem, tags[f], hdr_steps[si]);
				dump_hdr(opt_dump_dir, name, dec, ref, (size_t)w, (size_t)h);
			}
			double tm_psnr, stops;
			measure_hdr(ref, dec, pixels, channels, &tm_psnr, &stops);
			if (verbose) printf("  %-6s%-4g %7.3f bpp  dec %7.2f ms  enc %7.2f ms  tone mapped %6.2f dB  stops RMS %.4f\n", tags[f], hdr_steps[si], size * 8.0 / pixels, dec_s * 1e3, enc * 1e3, tm_psnr, stops);
			if (opt_csv) {
				fprintf(opt_csv, "%s,%s,%g,%d,%d,%zu,%.6f,%.6f,%.4f,nan,nan,nan,nan,nan,nan,nan,nan,%d,nan,nan,nan,nan,nan,%.6f,1,nan\n",
					stem, tags[f], hdr_steps[si], w, h, size, dec_s * 1e3, enc * 1e3, tm_psnr, channels, stops);
			}
		}
		free(data);
	}
	stbi_image_free(ref);
	free(out);
	free(dec);
}

// Premultiplied RGB PSNR of a decoded mip 2 against a premultiplied 4x4 box downsample of the source,
// which is where hidden colors under transparent pixels show up as fringes
static double mip2_psnr(const uint8_t* ref, int32_t w, int32_t h, const uint8_t* mip, bool premultiplied) {
	int32_t mw = (w + 3) >> 2, mh = (h + 3) >> 2;
	double  err = 0;
	for (int32_t y = 0; y < mh; y++)
		for (int32_t x = 0; x < mw; x++) {
			double sum[3] = {0};
			int32_t count = 0;
			for (int32_t sy = y*4; sy < y*4 + 4 && sy < h; sy++)
				for (int32_t sx = x*4; sx < x*4 + 4 && sx < w; sx++, count++) {
					const uint8_t* p = ref + ((size_t)sy * w + sx) * 4;
					for (int32_t c = 0; c < 3; c++) sum[c] += premultiplied ? p[c] : p[c] * p[3] / 255.0;
				}
			const uint8_t* d = mip + ((size_t)y * mw + x) * 4;
			for (int32_t c = 0; c < 3; c++) {
				double e = sum[c] / count - (premultiplied ? d[c] : d[c] * d[3] / 255.0);
				err += e * e;
			}
		}
	return to_psnr(err / ((double)mw * mh * 3), 255);
}

static void bench_image(const char* path, const char* opt_dump_dir, FILE* opt_csv, bool verbose, stats_t* ref_stats) {
	const char* ext = strrchr(path, '.');
	if (ext && !strcasecmp(ext, ".hdr")) { bench_hdr(path, opt_dump_dir, opt_csv, verbose); return; }

	size_t   src_size = 0;
	uint8_t* src      = read_file(path, &src_size);
	if (!src) { fprintf(stderr, "skip %s: unreadable\n", path); return; }
	int32_t  w, h, comp;
	uint8_t* ref = stbi_load_from_memory(src, (int)src_size, &w, &h, &comp, 4);
	if (!ref) { fprintf(stderr, "skip %s: %s\n", path, stbi_failure_reason()); free(src); return; }

	char stem[256];
	stem_of(path, stem, sizeof(stem));
	size_t   pixels = (size_t)w * h;
	uint8_t* dec    = malloc(pixels * 4);
	uint8_t* mip_buf = malloc(pixels * 4);
	int32_t  mode   = coded_channels(stem, ref, pixels);
	bool     normal = strstr(stem, "normal") != NULL;
	view(ref, pixels, mode, 4);
	if (verbose) printf("%s (%dx%d, %d channels)\n", path, w, h, mode);

	for (int32_t ci = 0; ci < CONFIG_COUNT; ci++) {
		config_t c    = configs[ci];
		size_t   size = src_size;
		uint8_t* data = src;
		double   enc  = 0;
		if ((c.codec == codec_jpg || is_turbo(c.codec)) && mode != 3) continue;
		lfi_opt_t opt = {
			.channels      = mode,
			.alpha         = strstr(stem, "_forcecut") ? lfi_alpha_cutout : lfi_alpha_auto,
			.premultiplied = strstr(stem, "_premul") != NULL,
			.data          = strstr(stem, "_data") != NULL,
		};
		if (c.codec != codec_src) enc = encode_timed(c, ref, w, h, opt, &data, &size);

		double  dec_s   = decode_timed(c, data, size, 0, dec);
		// Mips decode into a spare buffer, so dec still holds the full image
		double  mip_s[2] = { NAN, NAN }, mip_bytes[2] = { NAN, NAN };
		double  prog_s   = c.codec == codec_lfi && info_of(data, size).mips > 2 ? progressive_timed(data, size, mip_buf) : NAN;
		for (int32_t k = 0; c.codec == codec_lfi && k < 2 && k + 1 < info_of(data, size).mips; k++) {
			mip_s[k]     = decode_timed(c, data, size, k + 1, mip_buf);
			mip_bytes[k] = (double)lfi_mip_bytes(data, size, k + 1);
		}
		// mip_buf holds mip 2 from the last decode above
		double  mip2     = mode == 4 && !isnan(mip_s[1]) ? mip2_psnr(ref, w, h, mip_buf, opt.premultiplied) : NAN;
		int32_t dec_ch  = c.codec == codec_lfi ? info_of(data, size).pixel_size : 4;
		if (opt_dump_dir) {
			char out[4096];
			snprintf(out, sizeof(out), "%s/%s_%s%g.png", opt_dump_dir, stem, codec_tags[c.codec], c.param);
			if (dec_ch != 2) stbi_write_png(out, w, h, dec_ch, dec, w * dec_ch);
			else {
				// Normals as RGB with Z rebuilt, the way a shader would
				uint8_t* rgb = malloc(pixels * 3);
				for (size_t i = 0; i < pixels; i++) {
					double x = dec[i*2] / 127.5 - 1, y = dec[i*2 + 1] / 127.5 - 1, zz = 1 - x * x - y * y;
					rgb[i*3 + 0] = dec[i*2];
					rgb[i*3 + 1] = dec[i*2 + 1];
					rgb[i*3 + 2] = (uint8_t)lround(((zz > 0 ? sqrt(zz) : 0) + 1) * 127.5);
				}
				stbi_write_png(out, w, h, 3, rgb, w * 3);
				free(rgb);
			}
		}
		view(dec, pixels, mode, dec_ch);
		metrics_t m = measure(ref, dec, w, h, mode, normal);

		stats_t* s = &ref_stats[ci];
		s->bits        += (double)size * 8;
		s->pixels      += (double)pixels;
		s->dec_seconds += dec_s;
		s->enc_seconds += enc;
		s->psnr        += m.psnr;
		s->psnr_y      += m.psnr_y;
		s->psnr_c      += m.psnr_c;
		s->ssim        += m.ssim;
		s->gpu         += m.gpu;
		s->images      += 1;
		if (!isnan(m.dark)) { s->dark += m.dark; s->dark_c += m.dark_c; s->dark_images++; }

		if (verbose) {
			printf("  %s%-4g %7.3f bpp  dec %7.2f ms  enc %7.2f ms  %6.2f dB  Y %6.2f  C %6.2f  dark %6.2f  dark C %6.2f  ssim %.4f  gpu %6.2f  alpha %6.2f  angle %5.2f  mip2 %6.2f\n",
				codec_tags[c.codec], c.param, size * 8.0 / pixels, dec_s * 1e3, enc * 1e3, m.psnr, m.psnr_y, m.psnr_c, m.dark, m.dark_c, m.ssim, m.gpu, m.alpha, m.angle, mip2);
		}
		if (opt_csv) {
			fprintf(opt_csv, "%s,%s,%g,%d,%d,%zu,%.6f,%.6f,%.4f,%.4f,%.4f,%.4f,%.6f,%.4f,%.4f,%.4f,%.4f,%d,%.6f,%.6f,%.0f,%.0f,%.6f,nan,0,%.4f\n",
				stem, codec_tags[c.codec], c.param, w, h, size, dec_s * 1e3, enc * 1e3, m.psnr, m.psnr_y, m.psnr_c, m.dark, m.ssim, m.gpu, m.dark_c, m.alpha, m.angle, mode,
				mip_s[0] * 1e3, mip_s[1] * 1e3, mip_bytes[0], mip_bytes[1], prog_s * 1e3, mip2);
		}
		if (data != src) free(data);
	}
	stbi_image_free(ref);
	free(src);
	free(dec);
	free(mip_buf);
}

static void print_summary(const char* group, const stats_t* stats) {
	if (!stats[0].images) return;
	printf("\n== %s: %d images, %.1f MP ==\n", group, stats[0].images, stats[0].pixels * 1e-6);
	printf("%-8s %7s %9s %9s %7s %7s %7s %7s %7s %7s %7s\n", "config", "bpp", "dec MP/s", "enc MP/s", "PSNR", "Y", "C", "dark", "dark C", "SSIM", "GPU");
	for (int32_t ci = 0; ci < CONFIG_COUNT; ci++) {
		const stats_t* s = &stats[ci];
		char name[32];
		snprintf(name, sizeof(name), "%s%g", codec_tags[configs[ci].codec], configs[ci].param);
		printf("%-8s %7.3f %9.1f %9.1f %7.2f %7.2f %7.2f %7.2f %7.2f %7.4f %7.2f\n", name,
			s->bits / s->pixels,
			s->pixels / s->dec_seconds * 1e-6,
			s->enc_seconds > 0 ? s->pixels / s->enc_seconds * 1e-6 : 0,
			s->psnr / s->images, s->psnr_y / s->images, s->psnr_c / s->images,
			s->dark_images ? s->dark / s->dark_images : NAN,
			s->dark_images ? s->dark_c / s->dark_images : NAN,
			s->ssim / s->images, s->gpu / s->images);
	}
}

int main(int argc, char** argv) {
	const char* dump_dir = NULL;
	const char* csv_path = NULL;
	bool        verbose  = false;
	int32_t     first    = 1;
	for (; first < argc && argv[first][0] == '-'; first++) {
		if      (!strcmp(argv[first], "-v"))                    verbose  = true;
		else if (!strcmp(argv[first], "-d") && first + 1 < argc) dump_dir = argv[++first];
		else if (!strcmp(argv[first], "-c") && first + 1 < argc) csv_path = argv[++first];
	}
	if (first >= argc) {
		fprintf(stderr, "usage: bench [-v] [-d dump_dir] [-c out.csv] <image or dir>...\n");
		return 1;
	}

	FILE* csv = csv_path ? fopen(csv_path, "w") : NULL;
	if (csv) fprintf(csv, "image,codec,param,w,h,bytes,dec_ms,enc_ms,psnr,psnr_y,psnr_c,dark,ssim,gpu,dark_c,alpha,angle,mode,dec1_ms,dec2_ms,bytes1,bytes2,prog_ms,stops,hdr,mip2\n");

	for (int32_t a = first; a < argc; a++) {
		char**  files = NULL;
		int32_t count = 0;
		collect(argv[a], &files, &count);
		qsort(files, (size_t)count, sizeof(char*), cmp_str);

		stats_t stats[CONFIG_COUNT] = {0};
		for (int32_t i = 0; i < count; i++) {
			bench_image(files[i], dump_dir, csv, verbose, stats);
			free(files[i]);
		}
		free(files);
		print_summary(argv[a], stats);
		fflush(stdout);
	}
	if (csv) fclose(csv);
	return 0;
}
