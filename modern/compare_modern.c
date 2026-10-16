// Lift Image against JPEG, WebP, JPEG XL, AVIF and jpegli over a quality sweep, for compare_modern.py. Prints one CSV
// line per image, setting and quality: setting,param,image,pixels,bytes,psnr,encode_seconds,decode_seconds. JPEGs decode
// with libjpeg-turbo, except stb's, which decode with stb_image. Everything decodes to RGBA8 on one thread. Duplicate
// images are skipped. With -d, each decoded image is also written as <dir>/<setting>_<param>_<image>.png before its
// line, for perceptual metrics. -l runs the lossless settings instead, -a the JPEG XL ablation. -p i/n keeps every n-th unique image from the i-th.
// usage: compare_modern [-l | -a] [-p i/n] [-d dir] <images...>

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
#include <webp/decode.h>
#include <webp/encode.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <avif/avif.h>

uint8_t* jpegli_encode_rgba(const uint8_t* rgba, int32_t w, int32_t h, int32_t quality, bool sub444, size_t* out_size);

typedef enum { codec_lift, codec_stb, codec_turbo, codec_png, codec_webp, codec_jxl, codec_avif, codec_jpegli } codec_e;

typedef struct {
	const char*  name;
	codec_e      codec;
	bool         sub444;      // 4:4:4 chroma, else 4:2:0
	bool         progressive;
	int32_t      method;      // WebP method, 0 for its default of 4
	bool         lossless;
	bool         original_profile; // JPEG XL lossy without XYB
	int32_t      jxl_option_count;
	int32_t      jxl_options[3][2]; // JPEG XL frame settings applied after effort 7
	const float* params;
	int32_t      param_count;
} setting_t;

static const float lift_steps[]      = { 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32 };
static const float jpeg_qualities[]  = { 50, 55, 60, 65, 70, 75, 80, 85, 90, 95 };
static const float webp_qualities[]  = { 10, 20, 30, 40, 50, 60, 70, 75, 80, 85, 90, 95, 100 };
static const float jxl_distances[]   = { 0.5f, 0.7f, 1, 1.4f, 2, 2.8f, 4, 5.6f, 8, 11 };
static const float jxl_distances_wide[] = { 0.5f, 0.7f, 1, 1.4f, 2, 2.8f, 4, 5.6f, 8, 11, 16, 25 }; // resampling held at 1 past d 20
static const float avif_qualities[]  = { 20, 30, 40, 50, 60, 65, 70, 75, 80, 85, 90, 95 };
static const float jpegli_qualities[] = { 10, 20, 30, 40, 50, 60, 70, 75, 80, 85, 90, 95 };
static const float lossless[]        = { 0 };

#define PARAMS(a) a, (int32_t)(sizeof(a) / sizeof(a[0]))

static const setting_t lossy_settings[] = {
	{ .name = "lift",                  .codec = codec_lift,   .params = PARAMS(lift_steps) },
	{ .name = "stb",                   .codec = codec_stb,    .params = PARAMS(jpeg_qualities) },
	{ .name = "turbo",                 .codec = codec_turbo,  .params = PARAMS(jpeg_qualities) },
	{ .name = "turbo_progressive_420", .codec = codec_turbo,  .progressive = true, .params = PARAMS(jpeg_qualities) },
	{ .name = "turbo_progressive_444", .codec = codec_turbo,  .progressive = true, .sub444 = true, .params = PARAMS(jpeg_qualities) },
	{ .name = "jpegli_420",            .codec = codec_jpegli, .params = PARAMS(jpegli_qualities) },
	{ .name = "jpegli_444",            .codec = codec_jpegli, .sub444 = true, .params = PARAMS(jpegli_qualities) },
	{ .name = "webp",                  .codec = codec_webp,   .params = PARAMS(webp_qualities) },
	{ .name = "webp_m6",               .codec = codec_webp,   .method = 6, .params = PARAMS(webp_qualities) },
	{ .name = "jxl",                   .codec = codec_jxl,    .params = PARAMS(jxl_distances) },
	{ .name = "avif_420",              .codec = codec_avif,   .params = PARAMS(avif_qualities) },
	{ .name = "avif_444",              .codec = codec_avif,   .sub444 = true, .params = PARAMS(avif_qualities) },
};

// JPEG XL with one tool at a time turned off, for -a
static const setting_t ablation_settings[] = {
	{ .name = "lift",       .codec = codec_lift, .params = PARAMS(lift_steps) },
	{ .name = "jxl",        .codec = codec_jxl,  .params = PARAMS(jxl_distances) },
	{ .name = "jxl_noepf",  .codec = codec_jxl,  .jxl_option_count = 1, .jxl_options = { { JXL_ENC_FRAME_SETTING_EPF, 0 } }, .params = PARAMS(jxl_distances) },
	{ .name = "jxl_nogab",  .codec = codec_jxl,  .jxl_option_count = 1, .jxl_options = { { JXL_ENC_FRAME_SETTING_GABORISH, 0 } }, .params = PARAMS(jxl_distances) },
	{ .name = "jxl_nofilt", .codec = codec_jxl,  .jxl_option_count = 2, .jxl_options = { { JXL_ENC_FRAME_SETTING_EPF, 0 }, { JXL_ENC_FRAME_SETTING_GABORISH, 0 } }, .params = PARAMS(jxl_distances) },
	{ .name = "jxl_e3",     .codec = codec_jxl,  .jxl_option_count = 1, .jxl_options = { { JXL_ENC_FRAME_SETTING_EFFORT, 3 } }, .params = PARAMS(jxl_distances) },
	{ .name = "jxl_e5",     .codec = codec_jxl,  .jxl_option_count = 1, .jxl_options = { { JXL_ENC_FRAME_SETTING_EFFORT, 5 } }, .params = PARAMS(jxl_distances) },
	{ .name = "jxl_noxyb",  .codec = codec_jxl,  .original_profile = true, .jxl_option_count = 1, .jxl_options = { { JXL_ENC_FRAME_SETTING_RESAMPLING, 1 } }, .params = PARAMS(jxl_distances_wide) },
	{ .name = "jxl_noperc", .codec = codec_jxl,  .original_profile = true, .jxl_option_count = 2, .jxl_options = { { JXL_ENC_FRAME_SETTING_DISABLE_PERCEPTUAL_HEURISTICS, 1 }, { JXL_ENC_FRAME_SETTING_RESAMPLING, 1 } }, .params = PARAMS(jxl_distances_wide) },
	{ .name = "jxl_nopatch", .codec = codec_jxl, .jxl_option_count = 2, .jxl_options = { { JXL_ENC_FRAME_SETTING_PATCHES, 0 }, { JXL_ENC_FRAME_SETTING_DOTS, 0 } }, .params = PARAMS(jxl_distances) },
	{ .name = "jxl_dec4",   .codec = codec_jxl,  .jxl_option_count = 1, .jxl_options = { { JXL_ENC_FRAME_SETTING_DECODING_SPEED, 4 } }, .params = PARAMS(jxl_distances) },
};

// png is the source file itself, as build/bench measures it
static const setting_t lossless_settings[] = {
	{ .name = "lift",          .codec = codec_lift, .lossless = true, .params = PARAMS(lossless) },
	{ .name = "png",           .codec = codec_png,  .lossless = true, .params = PARAMS(lossless) },
	{ .name = "webp_lossless", .codec = codec_webp, .lossless = true, .params = PARAMS(lossless) },
	{ .name = "jxl_lossless",  .codec = codec_jxl,  .lossless = true, .params = PARAMS(lossless) },
};

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

// Lossy at the given method, or lossless at cwebp's default -z 6. RGBX ignores the source's alpha, as the other codecs do
static uint8_t* webp_encode(const uint8_t* rgba, int32_t w, int32_t h, float quality, setting_t s, size_t* out_size) {
	WebPConfig config;
	if (!WebPConfigInit(&config)) exit(1);
	if (s.lossless) {
		if (!WebPConfigLosslessPreset(&config, 6)) exit(1);
	} else {
		config.quality = quality;
		if (s.method) config.method = s.method;
	}
	WebPPicture picture;
	if (!WebPPictureInit(&picture)) exit(1);
	picture.width    = w;
	picture.height   = h;
	picture.use_argb = s.lossless;
	if (!WebPPictureImportRGBX(&picture, rgba, w * 4)) exit(1);
	WebPMemoryWriter writer;
	WebPMemoryWriterInit(&writer);
	picture.writer     = WebPMemoryWrite;
	picture.custom_ptr = &writer;
	if (!WebPEncode(&config, &picture)) { fprintf(stderr, "libwebp: error %d\n", picture.error_code); exit(1); }
	WebPPictureFree(&picture);
	*out_size = writer.size;
	return writer.mem;
}

// Effort 7, no parallel runner, so one thread
static uint8_t* jxl_encode(const uint8_t* rgba, int32_t w, int32_t h, float distance, setting_t s, size_t* out_size) {
	size_t   pixels = (size_t)w * h;
	uint8_t* rgb    = malloc(pixels * 3);
	for (size_t i = 0; i < pixels; i++) memcpy(rgb + i * 3, rgba + i * 4, 3);
	JxlEncoder*  encoder = JxlEncoderCreate(NULL);
	JxlBasicInfo info;
	JxlEncoderInitBasicInfo(&info);
	info.xsize                 = (uint32_t)w;
	info.ysize                 = (uint32_t)h;
	info.bits_per_sample       = 8;
	info.num_color_channels    = 3;
	info.uses_original_profile = s.lossless || s.original_profile;
	if (JxlEncoderSetBasicInfo(encoder, &info) != JXL_ENC_SUCCESS) { fprintf(stderr, "libjxl: basic info\n"); exit(1); }
	JxlColorEncoding color;
	JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
	JxlEncoderSetColorEncoding(encoder, &color);
	JxlEncoderFrameSettings* frame = JxlEncoderFrameSettingsCreate(encoder, NULL);
	JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_EFFORT, 7);
	for (int32_t i = 0; i < s.jxl_option_count; i++)
		if (JxlEncoderFrameSettingsSetOption(frame, (JxlEncoderFrameSettingId)s.jxl_options[i][0], s.jxl_options[i][1]) != JXL_ENC_SUCCESS) { fprintf(stderr, "libjxl: option %d rejected\n", s.jxl_options[i][0]); exit(1); }
	if (s.lossless) JxlEncoderSetFrameLossless(frame, JXL_TRUE);
	JxlEncoderSetFrameDistance(frame, s.lossless ? 0 : distance);
	JxlPixelFormat format = { 3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
	if (JxlEncoderAddImageFrame(frame, &format, rgb, pixels * 3) != JXL_ENC_SUCCESS) { fprintf(stderr, "libjxl: add frame\n"); exit(1); }
	JxlEncoderCloseInput(encoder);
	size_t   capacity = pixels + 4096, used = 0;
	uint8_t* out      = malloc(capacity);
	for (;;) {
		uint8_t*         next      = out + used;
		size_t           available = capacity - used;
		JxlEncoderStatus status    = JxlEncoderProcessOutput(encoder, &next, &available);
		used = (size_t)(next - out);
		if (status == JXL_ENC_SUCCESS) break;
		if (status != JXL_ENC_NEED_MORE_OUTPUT) { fprintf(stderr, "libjxl: encode failed\n"); exit(1); }
		capacity *= 2;
		out = realloc(out, capacity);
	}
	JxlEncoderDestroy(encoder);
	free(rgb);
	*out_size = used;
	return out;
}

// aom at speed 6, avifenc's default, on one thread, BT.601 full range as avifenc writes
static uint8_t* avif_encode(const uint8_t* rgba, int32_t w, int32_t h, float quality, setting_t s, size_t* out_size) {
	avifImage*   image = avifImageCreate((uint32_t)w, (uint32_t)h, 8, s.sub444 ? AVIF_PIXEL_FORMAT_YUV444 : AVIF_PIXEL_FORMAT_YUV420);
	avifRGBImage rgb;
	avifRGBImageSetDefaults(&rgb, image);
	rgb.format      = AVIF_RGB_FORMAT_RGBA;
	rgb.depth       = 8;
	rgb.ignoreAlpha = AVIF_TRUE;
	rgb.pixels      = (uint8_t*)rgba;
	rgb.rowBytes    = (uint32_t)w * 4;
	if (avifImageRGBToYUV(image, &rgb) != AVIF_RESULT_OK) { fprintf(stderr, "libavif: RGB to YUV\n"); exit(1); }
	avifEncoder* encoder = avifEncoderCreate();
	encoder->codecChoice = AVIF_CODEC_CHOICE_AOM;
	encoder->maxThreads  = 1;
	encoder->speed       = 6;
	encoder->quality     = (int)quality;
	avifRWData output    = AVIF_DATA_EMPTY;
	avifResult result    = avifEncoderWrite(encoder, image, &output);
	if (result != AVIF_RESULT_OK) { fprintf(stderr, "libavif: %s\n", avifResultToString(result)); exit(1); }
	avifEncoderDestroy(encoder);
	avifImageDestroy(image);
	uint8_t* out = malloc(output.size);
	memcpy(out, output.data, output.size);
	*out_size = output.size;
	avifRWDataFree(&output);
	return out;
}

static uint8_t* encode(setting_t s, const uint8_t* rgba, int32_t w, int32_t h, float param, const uint8_t* source, size_t source_size, size_t* out_size) {
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
	case codec_png: {
		uint8_t* out = malloc(source_size);
		memcpy(out, source, source_size);
		*out_size = source_size;
		return out;
	}
	case codec_turbo:  return turbo_encode(rgba, w, h, (int32_t)param, s, out_size);
	case codec_webp:   return webp_encode(rgba, w, h, param, s, out_size);
	case codec_jxl:    return jxl_encode(rgba, w, h, param, s, out_size);
	case codec_avif:   return avif_encode(rgba, w, h, param, s, out_size);
	case codec_jpegli: return jpegli_encode_rgba(rgba, w, h, (int32_t)param, s.sub444, out_size);
	}
	return NULL;
}

static void jxl_decode(const uint8_t* data, size_t size, size_t pixels, uint8_t* out_rgba) {
	static JxlDecoder* decoder;
	if (!decoder) decoder = JxlDecoderCreate(NULL);
	JxlDecoderReset(decoder);
	JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
	JxlDecoderSubscribeEvents(decoder, JXL_DEC_FULL_IMAGE);
	JxlDecoderSetInput(decoder, data, size);
	JxlDecoderCloseInput(decoder);
	for (;;) {
		JxlDecoderStatus status = JxlDecoderProcessInput(decoder);
		if (status == JXL_DEC_SUCCESS) break;
		if (status == JXL_DEC_FULL_IMAGE) continue;
		if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER && JxlDecoderSetImageOutBuffer(decoder, &format, out_rgba, pixels * 4) == JXL_DEC_SUCCESS) continue;
		fprintf(stderr, "libjxl: decode failed (%d)\n", (int)status);
		exit(1);
	}
	JxlDecoderReleaseInput(decoder);
}

// dav1d on one thread, then libavif's RGB conversion, which upsamples 4:2:0 bilinearly with libyuv
static void avif_decode(const uint8_t* data, size_t size, int32_t w, uint8_t* out_rgba) {
	avifDecoder* decoder = avifDecoderCreate();
	decoder->codecChoice = AVIF_CODEC_CHOICE_DAV1D;
	decoder->maxThreads  = 1;
	avifImage* image  = avifImageCreateEmpty();
	avifResult result = avifDecoderReadMemory(decoder, image, data, size);
	if (result != AVIF_RESULT_OK) { fprintf(stderr, "libavif: %s\n", avifResultToString(result)); exit(1); }
	avifRGBImage rgb;
	avifRGBImageSetDefaults(&rgb, image);
	rgb.format   = AVIF_RGB_FORMAT_RGBA;
	rgb.depth    = 8;
	rgb.pixels   = out_rgba;
	rgb.rowBytes = (uint32_t)w * 4;
	if (avifImageYUVToRGB(image, &rgb) != AVIF_RESULT_OK) { fprintf(stderr, "libavif: YUV to RGB\n"); exit(1); }
	avifImageDestroy(image);
	avifDecoderDestroy(decoder);
}

// Into out_rgba, w x h
static void decode(setting_t s, const uint8_t* data, size_t size, int32_t w, int32_t h, void* scratch, uint8_t* out_rgba) {
	static tjhandle handle;
	size_t          pixels = (size_t)w * h;
	switch (s.codec) {
	case codec_lift:
		if (lfi_decode(data, size, 0, lfi_format_native, out_rgba, scratch) != lfi_ok) { fprintf(stderr, "lfi_decode failed\n"); exit(1); }
		return;
	case codec_stb:
	case codec_png: {
		int32_t  pw, ph, c;
		uint8_t* p = stbi_load_from_memory(data, (int)size, &pw, &ph, &c, 4);
		memcpy(out_rgba, p, pixels * 4);
		stbi_image_free(p);
		return;
	}
	case codec_webp:
		if (!WebPDecodeRGBAInto(data, size, out_rgba, pixels * 4, w * 4)) { fprintf(stderr, "libwebp: decode failed\n"); exit(1); }
		return;
	case codec_jxl:  jxl_decode(data, size, pixels, out_rgba); return;
	case codec_avif: avif_decode(data, size, w, out_rgba); return;
	case codec_turbo:
	case codec_jpegli:
		if (!handle) handle = tj3Init(TJINIT_DECOMPRESS);
		if (tj3DecompressHeader(handle, data, size) || tj3Decompress8(handle, data, size, out_rgba, 0, TJPF_RGBA)) { fprintf(stderr, "libjpeg-turbo: %s\n", tj3GetErrorStr(handle)); exit(1); }
		return;
	}
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

static uint8_t* read_file(const char* path, size_t* out_size) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*out_size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* data = malloc(*out_size);
	*out_size     = fread(data, 1, *out_size, f);
	fclose(f);
	return data;
}

int main(int argc, char** argv) {
	const char* only = NULL; // -f: just this setting
	uint64_t    seen[256];
	int32_t     seen_count = 0, first = 1, part = 0, parts = 1;
	const char* opt_dump   = NULL;
	bool        lossless_mode = false, ablation_mode = false;
	for (; first < argc && argv[first][0] == '-'; first++) {
		if      (!strcmp(argv[first], "-l"))                      lossless_mode = true;
		else if (!strcmp(argv[first], "-a"))                      ablation_mode = true;
		else if (!strcmp(argv[first], "-f") && first + 1 < argc)  only = argv[++first];
		else if (!strcmp(argv[first], "-d") && first + 1 < argc)  opt_dump = argv[++first];
		else if (!strcmp(argv[first], "-p") && first + 1 < argc)  sscanf(argv[++first], "%d/%d", &part, &parts);
		else { fprintf(stderr, "usage: compare_modern [-l | -a] [-p i/n] [-d dir] <images...>\n"); return 1; }
	}
	const setting_t* settings      = lossless_mode ? lossless_settings : ablation_mode ? ablation_settings : lossy_settings;
	size_t           setting_count = lossless_mode ? sizeof(lossless_settings) / sizeof(lossless_settings[0])
	                               : ablation_mode ? sizeof(ablation_settings) / sizeof(ablation_settings[0]) : sizeof(lossy_settings) / sizeof(lossy_settings[0]);
	stbi_write_png_compression_level = 1;
	for (int32_t f = first; f < argc; f++) {
		size_t   source_size = 0;
		uint8_t* source      = read_file(argv[f], &source_size);
		int32_t  w, h, c;
		uint8_t* rgba = source ? stbi_load_from_memory(source, (int)source_size, &w, &h, &c, 4) : NULL;
		if (!rgba) { fprintf(stderr, "skip %s: unreadable\n", argv[f]); free(source); continue; }
		size_t   pixels = (size_t)w * h;
		uint64_t hash   = 0xcbf29ce484222325ull;
		for (size_t i = 0; i < pixels * 4; i++) hash = (hash ^ rgba[i]) * 0x100000001b3ull;
		bool duplicate = false;
		for (int32_t i = 0; i < seen_count; i++) duplicate |= seen[i] == hash;
		if (duplicate || seen_count == 256) { fprintf(stderr, "skip %s: duplicate\n", argv[f]); stbi_image_free(rgba); free(source); continue; }
		seen[seen_count++] = hash;
		if ((seen_count - 1) % parts != part) { stbi_image_free(rgba); free(source); continue; }

		const char* name = strrchr(argv[f], '/') ? strrchr(argv[f], '/') + 1 : argv[f];
		uint8_t*    out  = malloc(pixels * 4);
		for (size_t si = 0; si < setting_count; si++) {
			setting_t s = settings[si];
			if (only && strcmp(s.name, only)) continue;
			for (int32_t p = 0; p < s.param_count; p++) {
				size_t   size;
				double   t0       = now();
				uint8_t* data     = encode(s, rgba, w, h, s.params[p], source, source_size, &size);
				double   encode_s = now() - t0;
				void*    scratch  = s.codec == codec_lift ? malloc(lfi_scratch_size(data, size)) : NULL;
				double   decode_s = 1e9;
				for (int32_t r = 0; r < 3; r++) {
					t0 = now();
					decode(s, data, size, w, h, scratch, out);
					double t = now() - t0;
					decode_s = t < decode_s ? t : decode_s;
				}
				int32_t stem    = (int)(strrchr(name, '.') ? strrchr(name, '.') - name : (long)strlen(name));
				double  quality = psnr(rgba, out, pixels);
				if (s.lossless && quality != 99) { fprintf(stderr, "%s on %s is not lossless: %.2f dB\n", s.name, name, quality); exit(1); }
				if (opt_dump) {
					char path[4096];
					snprintf(path, sizeof(path), "%s/%s_%g_%.*s.png", opt_dump, s.name, s.params[p], stem, name);
					for (size_t i = 0; i < pixels; i++) memmove(out + i * 3, out + i * 4, 3);
					stbi_write_png(path, w, h, 3, out, w * 3);
				}
				printf("%s,%g,%.*s,%zu,%zu,%.4f,%.6f,%.6f\n", s.name, s.params[p], stem, name, pixels, size, quality, encode_s, decode_s);
				fflush(stdout);
				free(scratch);
				free(data);
			}
		}
		free(out);
		stbi_image_free(rgba);
		free(source);
	}
	return 0;
}
