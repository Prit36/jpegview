#include "stdafx.h"

#include "HEIFWrapper.h"
#include "MaxImageDef.h"
#include "ICCProfileTransform.h"

#include <thread>
#include <immintrin.h>
#include <algorithm>

void * HeifReader::ReadImage(int &width,
					   int &height,
					   int &nchannels,
					   int &frame_count,
					   void* &exif_chunk,
					   bool &outOfMemory,
					   bool &has_alpha,
					   int frame_index,
					   const void *buffer,
					   int sizebytes)
{
	outOfMemory = false;
	has_alpha = false;
	width = height = 0;
	nchannels = 4;

	unsigned char* pPixelData = NULL;
	exif_chunk = NULL;

	heif::Context context;
	context.read_from_memory_without_copy(buffer, sizebytes);
	frame_count = context.get_number_of_top_level_images();
	if (frame_count <= 0 || frame_index < 0 || frame_index >= frame_count) {
		return NULL;
	}
	heif_item_id item_id = context.get_list_of_top_level_image_IDs().at(frame_index);
	heif::ImageHandle handle = context.get_image_handle(item_id);
	has_alpha = handle.has_alpha_channel();

	struct heif_decoding_options* decode_options = heif_decoding_options_alloc();
	if (decode_options != NULL) {
		unsigned int hw_threads = std::thread::hardware_concurrency();
		decode_options->num_codec_threads = (int)max(2u, min(hw_threads, 16u));
		decode_options->convert_hdr_to_8bit = 1;
	}

	heif_image* raw_image = NULL;
	heif_error decode_err = heif_decode_image(
		handle.get_raw_image_handle(),
		&raw_image,
		heif_colorspace_RGB,
		heif_chroma_interleaved_RGBA,
		decode_options
	);

	if (decode_options != NULL) {
		heif_decoding_options_free(decode_options);
	}

	if (decode_err.code != heif_error_Ok || raw_image == NULL) {
		return NULL;
	}

	heif::Image image(raw_image);
	int stride;
	uint8_t* data = image.get_plane(heif_channel_interleaved, &stride);
	width = image.get_width(heif_channel_interleaved);
	height = image.get_height(heif_channel_interleaved);

	if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
		return NULL;
	if (abs((double)width * height) > MAX_IMAGE_PIXELS) {
		outOfMemory = true;
		return NULL;
	}
	if (width < 1 || height < 1 || width * nchannels > stride)
		return NULL;

	int size = width * nchannels * height;
	pPixelData = new(std::nothrow) unsigned char[size];
	if (pPixelData == NULL) {
		outOfMemory = true;
		return NULL;
	}
	std::vector<uint8_t> iccp = image.get_raw_color_profile();
	void* transform = ICCProfileTransform::CreateTransform(iccp.data(), iccp.size(), ICCProfileTransform::FORMAT_RGBA);
	if (!ICCProfileTransform::DoTransform(transform, data, pPixelData, width, height, stride)) {
		#pragma omp parallel for
		for (int row = 0; row < height; row++) {
			const uint8_t* pSrc = data + row * stride;
			uint8_t* pDst = pPixelData + row * (width * 4);
			int col = 0;
#ifdef __AVX2__
			__m256i shuffle_mask_256 = _mm256_setr_epi8(
				2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
				2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15
			);
			for (; col + 7 < width; col += 8) {
				__m256i src_pixels = _mm256_loadu_si256((const __m256i*)(pSrc + col * 4));
				__m256i bgra_pixels = _mm256_shuffle_epi8(src_pixels, shuffle_mask_256);
				_mm256_storeu_si256((__m256i*)(pDst + col * 4), bgra_pixels);
			}
#elif defined(__SSSE3__)
			__m128i shuffle_mask_128 = _mm_setr_epi8(
				2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15
			);
			for (; col + 3 < width; col += 4) {
				__m128i src_pixels = _mm_loadu_si128((const __m128i*)(pSrc + col * 4));
				__m128i bgra_pixels = _mm_shuffle_epi8(src_pixels, shuffle_mask_128);
				_mm_storeu_si128((__m128i*)(pDst + col * 4), bgra_pixels);
			}
#endif
			for (; col < width; col++) {
				const uint32_t* p = (const uint32_t*)(pSrc + col * 4);
				uint32_t* o = (uint32_t*)(pDst + col * 4);
				*o = _rotr(_byteswap_ulong(*p), 8);
			}
		}
	}
	ICCProfileTransform::DeleteTransform(transform);

	std::vector<heif_item_id> exif_blocks = handle.get_list_of_metadata_block_IDs("Exif");

	if (!exif_blocks.empty()) {
		std::vector<uint8_t> exif = handle.get_metadata(exif_blocks[0]);
		// 65538 magic number comes from investigations by qbnu
		// see https://github.com/sylikc/jpegview/pull/213#pullrequestreview-1494451359 for more details
		/*
		* These libraries all have their own ideas about where to start the Exif data from.
		* JPEG Exif blocks are in the format
		  FF E1 SS SS 45 78 69 66 00 00 [data]

		  The SS SS is a big-endian unsigned short representing the size of everything after FF E1.

		  libjxl gives 00 00 00 00 [data]
		  libheif gives 00 00 00 00 45 78 69 66 00 00 [data]
		  libavif, libwebp and libpng give [data], so they have different limits for size.

		  If you want I can change it to 65536 + an offset and add notes explaining why.
		*/
		if (exif.size() > 8 && exif.size() < 65538) {
			exif_chunk = malloc(exif.size());
			if (exif_chunk != NULL) {
				memcpy(exif_chunk, exif.data(), exif.size());
				*((unsigned short*)exif_chunk) = _byteswap_ushort(0xFFE1);
				*((unsigned short*)exif_chunk + 1) = _byteswap_ushort(exif.size() - 2);
			}
		}
	}

	return (void*)pPixelData;
}
