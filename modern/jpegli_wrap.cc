// jpegli's encoder behind a C entry point, with cjpegli's defaults: adaptive quantization, progressive level 2,
// optimized Huffman codes, quality mapped to a Butteraugli distance.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/jpegli/encode.h"

extern "C" uint8_t* jpegli_encode_rgba(const uint8_t* rgba, int32_t w, int32_t h, int32_t quality, bool sub444, size_t* out_size) {
	jpeg_compress_struct cinfo;
	jpeg_error_mgr       error;
	memset(&cinfo, 0, sizeof(cinfo));
	cinfo.err = jpegli_std_error(&error);
	jpegli_create_compress(&cinfo);
	unsigned char* mem      = NULL;
	unsigned long  mem_size = 0;
	jpegli_mem_dest(&cinfo, &mem, &mem_size);
	cinfo.image_width      = (JDIMENSION)w;
	cinfo.image_height     = (JDIMENSION)h;
	cinfo.input_components = 3;
	cinfo.in_color_space   = JCS_RGB;
	jpegli_set_defaults(&cinfo);
	cinfo.comp_info[0].h_samp_factor = cinfo.comp_info[0].v_samp_factor = sub444 ? 1 : 2;
	for (int c = 1; c < cinfo.num_components; c++) cinfo.comp_info[c].h_samp_factor = cinfo.comp_info[c].v_samp_factor = 1;
	jpegli_enable_adaptive_quantization(&cinfo, TRUE);
	jpegli_set_distance(&cinfo, jpegli_quality_to_distance(quality), TRUE);
	jpegli_set_progressive_level(&cinfo, 2);
	cinfo.optimize_coding = TRUE;
	jpegli_start_compress(&cinfo, TRUE);
	uint8_t* row = (uint8_t*)malloc((size_t)w * 3);
	while (cinfo.next_scanline < cinfo.image_height) {
		const uint8_t* source = rgba + (size_t)cinfo.next_scanline * w * 4;
		for (int32_t x = 0; x < w; x++) memcpy(row + x * 3, source + x * 4, 3);
		JSAMPROW rows[1] = { row };
		jpegli_write_scanlines(&cinfo, rows, 1);
	}
	jpegli_finish_compress(&cinfo);
	jpegli_destroy_compress(&cinfo);
	free(row);
	*out_size = mem_size;
	uint8_t* out = (uint8_t*)malloc(mem_size);
	memcpy(out, mem, mem_size);
	free(mem);
	return out;
}
