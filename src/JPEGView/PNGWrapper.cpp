#include "stdafx.h"

#include "PNGWrapper.h"

#include "FastPng.h"
#include "MaxImageDef.h"
#include <cstring>
#include <stdexcept>

#ifndef PNG_UINT_31_MAX
#define PNG_UINT_31_MAX 0x7FFFFFFF
#endif

void PngReader::DeleteCache() {
	// Simple fast path is stateless; nothing cached between still images.
	// Kept for API compatibility (ImageLoadThread calls it when switching files).
}

bool PngReader::MustUseInternalDecoder(const void* bufferIn, size_t sizebytes) {
	const char* buffer = (const char*)bufferIn;
	if (sizebytes < 24) return false;
	unsigned int width = _byteswap_ulong(*(unsigned int*)(buffer + 16));
	unsigned int height = _byteswap_ulong(*(unsigned int*)(buffer + 20));
	if (4.0 * width * height > INT_MAX) return true;
	size_t offset = 8;
	while (offset + 7 < sizebytes) {
		if (memcmp(buffer + offset + 4, "acTL", 4) == 0) return true;
		if (memcmp(buffer + offset + 4, "IDAT", 4) == 0) return false;
		unsigned int chunksize = _byteswap_ulong(*(unsigned int*)(buffer + offset));
		if (chunksize > PNG_UINT_31_MAX) return false;
		offset += chunksize + 12;
	}
	return false;
}

void* PngReader::GetEXIFBlock(void* buffer, size_t sizebytes) {
	size_t offset = 8;
	while (offset + 7 < sizebytes) {
		unsigned int chunksize = _byteswap_ulong(*(unsigned int*)((char*)buffer + offset));
		if (memcmp((char*)buffer + offset + 4, "eXIf", 4) == 0 && chunksize < 65528 && offset + chunksize + 12 <= sizebytes) {
			void* exif_chunk = malloc(chunksize + 10);
			if (exif_chunk != NULL) {
				memcpy(exif_chunk, "\xFF\xE1\0\0Exif\0\0", 10);
				*((unsigned short*)exif_chunk + 1) = _byteswap_ushort((unsigned short)(chunksize + 8));
				memcpy((char*)exif_chunk + 10, (char*)buffer + offset + 8, chunksize);
			}
			return exif_chunk;
		}
		if (chunksize > PNG_UINT_31_MAX) return NULL;
		offset += chunksize + 12;
	}
	return NULL;
}

void* PngReader::ReadImage(int& width,
	int& height,
	int& nchannels,
	bool& has_animation,
	int& frame_count,
	int& frame_time,
	void*& exif_chunk,
	bool& outOfMemory,
	void* buffer,
	size_t sizebytes)
{
	exif_chunk = NULL;
	if (buffer && sizebytes >= 33) {
		unsigned int fw = _byteswap_ulong(*(unsigned int*)((char*)buffer + 16));
		unsigned int fh = _byteswap_ulong(*(unsigned int*)((char*)buffer + 20));
		if (fw <= MAX_IMAGE_DIMENSION && fh <= MAX_IMAGE_DIMENSION && (double)fw * fh <= (double)MAX_IMAGE_PIXELS) {
			FastPngImage fp;
			if (FastPngDecode((const unsigned char*)buffer, sizebytes, fp) == 0) {
				width = fp.width;
				height = fp.height;
				nchannels = 4;
				has_animation = false;
				frame_count = 1;
				frame_time = 100;
				if (fp.exif_payload && fp.exif_size > 8 && fp.exif_size < 65528) {
					exif_chunk = malloc(fp.exif_size + 10);
					if (exif_chunk != NULL) {
						memcpy(exif_chunk, "\xFF\xE1\0\0Exif\0\0", 10);
						*((unsigned short*)exif_chunk + 1) = _byteswap_ushort((unsigned short)(fp.exif_size + 8));
						memcpy((char*)exif_chunk + 10, fp.exif_payload, fp.exif_size);
					}
				}
				if (fp.exif_payload) free(fp.exif_payload);
				return fp.pixels;
			}
		}
	}
	return NULL;
}
