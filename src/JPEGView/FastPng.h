// FastPng.h - accelerated PNG decode for the common 8-bit, non-interlaced,
// non-animated case (color types 0/2/4/6), used as a fast path ahead of the
// libpng decoder in PNGWrapper.cpp. Output contract matches PngReader:
// 4 channels (BGRA), top-row-first, row-major, malloc'd buffer.
#pragma once

#include <cstddef>

struct FastPngImage {
	int width = 0;
	int height = 0;
	unsigned char* pixels = nullptr;   // BGRA, malloc'd, caller frees
	void* exif_payload = nullptr;      // raw eXIf chunk payload, malloc'd, caller frees
	size_t exif_size = 0;
};

// Returns 0 on success (out filled). Returns -1 when the image is not eligible
// for the fast path - the caller must fall back to the libpng decoder.
// Never throws; on any internal failure it returns -1 and frees its own memory.
int FastPngDecode(const unsigned char* file, size_t size, FastPngImage& out);
