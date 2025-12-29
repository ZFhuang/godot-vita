/**************************************************************************/
/*  png_indexed.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "png_indexed.h"

#include "core/os/file_access.h"

#include <png.h>
#include <cstring>

// Simple median-cut color quantization for 8-bit indexed PNG
// This is a simplified implementation that reduces image colors to 256 max

struct ColorBox {
	uint8_t r_min, r_max;
	uint8_t g_min, g_max;
	uint8_t b_min, b_max;
	Vector<uint32_t> colors;

	int get_longest_axis() const {
		int r_range = r_max - r_min;
		int g_range = g_max - g_min;
		int b_range = b_max - b_min;
		if (r_range >= g_range && r_range >= b_range) {
			return 0;
		} else if (g_range >= b_range) {
			return 1;
		}
		return 2;
	}

	void compute_bounds() {
		r_min = g_min = b_min = 255;
		r_max = g_max = b_max = 0;
		for (int i = 0; i < colors.size(); i++) {
			uint32_t c = colors[i];
			uint8_t r = (c >> 16) & 0xFF;
			uint8_t g = (c >> 8) & 0xFF;
			uint8_t b = c & 0xFF;
			r_min = MIN(r_min, r);
			r_max = MAX(r_max, r);
			g_min = MIN(g_min, g);
			g_max = MAX(g_max, g);
			b_min = MIN(b_min, b);
			b_max = MAX(b_max, b);
		}
	}

	uint32_t get_average_color() const {
		if (colors.size() == 0) {
			return 0;
		}
		uint64_t r_sum = 0, g_sum = 0, b_sum = 0;
		for (int i = 0; i < colors.size(); i++) {
			uint32_t c = colors[i];
			r_sum += (c >> 16) & 0xFF;
			g_sum += (c >> 8) & 0xFF;
			b_sum += c & 0xFF;
		}
		int count = colors.size();
		return ((r_sum / count) << 16) | ((g_sum / count) << 8) | (b_sum / count);
	}
};

static int _compare_colors_r(const void *a, const void *b) {
	uint32_t ca = *(const uint32_t *)a;
	uint32_t cb = *(const uint32_t *)b;
	return ((ca >> 16) & 0xFF) - ((cb >> 16) & 0xFF);
}

static int _compare_colors_g(const void *a, const void *b) {
	uint32_t ca = *(const uint32_t *)a;
	uint32_t cb = *(const uint32_t *)b;
	return ((ca >> 8) & 0xFF) - ((cb >> 8) & 0xFF);
}

static int _compare_colors_b(const void *a, const void *b) {
	uint32_t ca = *(const uint32_t *)a;
	uint32_t cb = *(const uint32_t *)b;
	return (ca & 0xFF) - (cb & 0xFF);
}

static void _split_box(ColorBox &box, ColorBox &new_box) {
	int axis = box.get_longest_axis();

	// Sort colors by the longest axis
	uint32_t *data = box.colors.ptrw();
	int count = box.colors.size();

	switch (axis) {
		case 0:
			qsort(data, count, sizeof(uint32_t), _compare_colors_r);
			break;
		case 1:
			qsort(data, count, sizeof(uint32_t), _compare_colors_g);
			break;
		case 2:
			qsort(data, count, sizeof(uint32_t), _compare_colors_b);
			break;
	}

	// Split at median
	int mid = count / 2;
	new_box.colors.resize(count - mid);
	for (int i = mid; i < count; i++) {
		new_box.colors.write[i - mid] = box.colors[i];
	}
	box.colors.resize(mid);

	box.compute_bounds();
	new_box.compute_bounds();
}

static int _find_nearest_palette_index(uint32_t color, const uint32_t *palette, int palette_size) {
	uint8_t r = (color >> 16) & 0xFF;
	uint8_t g = (color >> 8) & 0xFF;
	uint8_t b = color & 0xFF;

	int best_idx = 0;
	int best_dist = INT_MAX;

	for (int i = 0; i < palette_size; i++) {
		uint8_t pr = (palette[i] >> 16) & 0xFF;
		uint8_t pg = (palette[i] >> 8) & 0xFF;
		uint8_t pb = palette[i] & 0xFF;

		int dr = r - pr;
		int dg = g - pg;
		int db = b - pb;
		int dist = dr * dr + dg * dg + db * db;

		if (dist < best_dist) {
			best_dist = dist;
			best_idx = i;
		}
	}

	return best_idx;
}

Error save_indexed_png_for_vita(const Ref<Image> &p_image, const String &p_path, int p_target_width, int p_target_height) {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->empty(), ERR_INVALID_PARAMETER);

	Ref<Image> img = p_image->duplicate();

	// Resize if needed
	if (img->get_width() != p_target_width || img->get_height() != p_target_height) {
		img->resize(p_target_width, p_target_height, Image::INTERPOLATE_LANCZOS);
	}

	// Convert to RGB8 for processing
	if (img->get_format() != Image::FORMAT_RGB8) {
		img->convert(Image::FORMAT_RGB8);
	}

	int width = img->get_width();
	int height = img->get_height();

	// Collect all unique colors
	img->lock();
	Vector<uint32_t> unique_colors;
	HashMap<uint32_t, bool> color_set;

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			Color c = img->get_pixel(x, y);
			uint32_t rgb = ((uint32_t)(c.r * 255) << 16) | ((uint32_t)(c.g * 255) << 8) | (uint32_t)(c.b * 255);
			if (!color_set.has(rgb)) {
				color_set[rgb] = true;
				unique_colors.push_back(rgb);
			}
		}
	}

	// Quantize to 256 colors using median-cut
	Vector<uint32_t> palette;

	if (unique_colors.size() <= 256) {
		// Already 256 colors or less, use directly
		palette = unique_colors;
	} else {
		// Median-cut quantization
		Vector<ColorBox> boxes;
		boxes.resize(1);
		boxes.write[0].colors = unique_colors;
		boxes.write[0].compute_bounds();

		while (boxes.size() < 256) {
			// Find box with most colors
			int max_idx = 0;
			int max_colors = boxes[0].colors.size();
			for (int i = 1; i < boxes.size(); i++) {
				if (boxes[i].colors.size() > max_colors) {
					max_colors = boxes[i].colors.size();
					max_idx = i;
				}
			}

			if (max_colors <= 1) {
				break;
			}

			ColorBox new_box;
			_split_box(boxes.write[max_idx], new_box);
			boxes.push_back(new_box);
		}

		// Generate palette from box averages
		for (int i = 0; i < boxes.size(); i++) {
			palette.push_back(boxes[i].get_average_color());
		}
	}

	// Ensure we have at least one color
	if (palette.size() == 0) {
		palette.push_back(0);
	}

	// Create indexed image data
	Vector<uint8_t> indexed_data;
	indexed_data.resize(width * height);

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			Color c = img->get_pixel(x, y);
			uint32_t rgb = ((uint32_t)(c.r * 255) << 16) | ((uint32_t)(c.g * 255) << 8) | (uint32_t)(c.b * 255);
			int idx = _find_nearest_palette_index(rgb, palette.ptr(), palette.size());
			indexed_data.write[y * width + x] = idx;
		}
	}

	img->unlock();

	// Write indexed PNG using libpng directly
	FILE *fp = fopen(p_path.utf8().get_data(), "wb");
	ERR_FAIL_COND_V_MSG(!fp, ERR_FILE_CANT_WRITE, "Cannot open file for writing: " + p_path);

	png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png_ptr) {
		fclose(fp);
		return ERR_CANT_CREATE;
	}

	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) {
		png_destroy_write_struct(&png_ptr, nullptr);
		fclose(fp);
		return ERR_CANT_CREATE;
	}

	if (setjmp(png_jmpbuf(png_ptr))) {
		png_destroy_write_struct(&png_ptr, &info_ptr);
		fclose(fp);
		return ERR_FILE_CORRUPT;
	}

	png_init_io(png_ptr, fp);

	// Set header for 8-bit indexed PNG
	png_set_IHDR(png_ptr, info_ptr, width, height, 8,
			PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE,
			PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

	// Set palette
	png_color png_palette[256];
	memset(png_palette, 0, sizeof(png_palette));
	for (int i = 0; i < palette.size() && i < 256; i++) {
		png_palette[i].red = (palette[i] >> 16) & 0xFF;
		png_palette[i].green = (palette[i] >> 8) & 0xFF;
		png_palette[i].blue = palette[i] & 0xFF;
	}
	png_set_PLTE(png_ptr, info_ptr, png_palette, palette.size());

	png_write_info(png_ptr, info_ptr);

	// Write image data row by row
	for (int y = 0; y < height; y++) {
		png_write_row(png_ptr, &indexed_data.ptr()[y * width]);
	}

	png_write_end(png_ptr, nullptr);
	png_destroy_write_struct(&png_ptr, &info_ptr);
	fclose(fp);

	return OK;
}
