#include "stdafx.h"

#include "HEIFWrapper.h"
#include "GpuHeifDecoder.h"
#include "MaxImageDef.h"
#include "ICCProfileTransform.h"

#include <thread>
#include <immintrin.h>
#include <algorithm>

// YCbCr (BT.x) -> RGB conversion coefficients, mirroring libheif's nclx.cc
// (get_Kr_Kb / get_YCbCr_to_RGB_coefficients) so results stay compatible
// with the library's own color conversion path.
struct YCbCrCoeffs {
	float r_cr, g_cb, g_cr, b_cb;
};

static YCbCrCoeffs GetYCbCrToRGBCoefficients(unsigned int matrix_coefficients)
{
	// Kr/Kb per matrix_coefficients, same table as libheif get_Kr_Kb()
	float Kr = 0.0f, Kb = 0.0f;
	switch (matrix_coefficients) {
		case 1:	Kr = 0.2126f; Kb = 0.0722f; break;
		case 4:	Kr = 0.30f;   Kb = 0.11f;   break;
		case 5:
		case 6:	Kr = 0.299f;  Kb = 0.114f;  break;
		case 7:	Kr = 0.212f;  Kb = 0.087f;  break;
		case 9:
		case 10: // BT.2020 (treated as NCL like libheif does)
			Kr = 0.2627f; Kb = 0.0593f; break;
		default:
			break;
	}
	if (Kr == 0.0f && Kb == 0.0f) {
		// default coefficients (Rec 601 values), same as libheif fallback
		YCbCrCoeffs c = { 1.402f, -0.344136f, -0.714136f, 1.772f };
		return c;
	}
	YCbCrCoeffs c;
	c.r_cr = 2 * (-Kr + 1);
	c.g_cb = 2 * Kb * (-Kb + 1) / (Kb + Kr - 1);
	c.g_cr = 2 * Kr * (-Kr + 1) / (Kb + Kr - 1);
	c.b_cb = 2 * (-Kb + 1);
	return c;
}

static inline unsigned char ClipU8Float(float fx)
{
	int x = (int)(fx + 0.5f);
	if (x < 0) return 0;
	if (x > 255) return 255;
	return (unsigned char)x;
}

// Bilinearly upsample one chroma row pair (2 output rows from one chroma row
// and its neighbor), matching libheif's Op_YCbCr420_bilinear_to_YCbCr444:
// chroma samples sit at the center of 2x2 luma blocks, weights are 3:1.
// out even-x uses 3:1 left:right; odd-x uses 1:3. +2 / +8 rounding as in libheif.
static void UpsampleChromaRowPairBilinear(const uint8_t* cbTop, const uint8_t* cbBot,
	const uint8_t* crTop, const uint8_t* crBot,
	int cw, int* cbOut0, int* cbOut1, int* crOut0, int* crOut1)
{
	// borders (first/last column replicate nearest sample with vertical blend)
	cbOut0[0] = (3 * cbTop[0] + cbBot[0] + 2) >> 2;
	cbOut1[0] = (cbTop[0] + 3 * cbBot[0] + 2) >> 2;
	crOut0[0] = (3 * crTop[0] + crBot[0] + 2) >> 2;
	crOut1[0] = (crTop[0] + 3 * crBot[0] + 2) >> 2;
	for (int cx = 0; cx < cw - 1; cx++) {
		int o = 2 * cx + 1;
		cbOut0[o] = (3 * cbTop[cx] + cbTop[cx + 1] + 2) >> 2;
		cbOut0[o + 1] = (cbTop[cx] + 3 * cbTop[cx + 1] + 2) >> 2;
		cbOut1[o] = (3 * cbBot[cx] + cbBot[cx + 1] + 2) >> 2;
		cbOut1[o + 1] = (cbBot[cx] + 3 * cbBot[cx + 1] + 2) >> 2;
		crOut0[o] = (3 * crTop[cx] + crTop[cx + 1] + 2) >> 2;
		crOut0[o + 1] = (crTop[cx] + 3 * crTop[cx + 1] + 2) >> 2;
		crOut1[o] = (3 * crBot[cx] + crBot[cx + 1] + 2) >> 2;
		crOut1[o + 1] = (crBot[cx] + 3 * crBot[cx + 1] + 2) >> 2;
	}
	if (cw >= 1) {
		int o = 2 * (cw - 1);
		cbOut0[o] = cbTop[cw - 1];
		cbOut1[o] = cbBot[cw - 1];
		crOut0[o] = crTop[cw - 1];
		crOut1[o] = crBot[cw - 1];
		if (o + 1 < 2 * cw) {
			cbOut0[o + 1] = cbTop[cw - 1];
			cbOut1[o + 1] = cbBot[cw - 1];
			crOut0[o + 1] = crTop[cw - 1];
			crOut1[o + 1] = crBot[cw - 1];
		}
	}
}

// Convert planar YCbCr 4:2:0 8-bit to packed BGRA in one fused pass, matching
// libheif's pipeline result: bilinear chroma upsampling to 4:4:4 (weights 3:1,
// chroma centered on the 2x2 luma block) followed by Op_YCbCr_to_RGB with the
// nclx-derived coefficients and range handling.
// Parallelized with OpenMP over row pairs.
static void ConvertYCbCr420ToBGRA(const uint8_t* pY, int yStride,
	const uint8_t* pCb, int cbStride,
	const uint8_t* pCr, int crStride,
	int chromaRows,
	uint8_t* pDst,
	int width, int height,
	bool hasAlpha, const uint8_t* pAlphaPlane, int alphaStride,
	const YCbCrCoeffs& coeffs, bool fullRange)
{
	const float r_cr = coeffs.r_cr, g_cb = coeffs.g_cb, g_cr = coeffs.g_cr, b_cb = coeffs.b_cb;
	const int cw = (width + 1) / 2;

	#pragma omp parallel for schedule(static)
	for (int row2 = 0; row2 < height; row2 += 2) {
		// per-thread scratch for the two bilinearly upsampled chroma rows
		std::vector<int> cbUp0(2 * cw), cbUp1(2 * cw), crUp0(2 * cw), crUp1(2 * cw);

		for (int dy = 0; dy < 2 && (row2 + dy) < height; dy++) {
			int row = row2 + dy;
			const uint8_t* lineY = pY + (size_t)row * yStride;
			const uint8_t* lineA = hasAlpha ? (pAlphaPlane + (size_t)row * alphaStride) : NULL;
			uint8_t* out = pDst + (size_t)row * (width * 4);

			const int* cbUp = dy ? (int*)cbUp1.data() : (int*)cbUp0.data();
			const int* crUp = dy ? (int*)crUp1.data() : (int*)crUp0.data();
			if (dy == 0 || (row2 + 1) < height) {
				// compute both rows' chroma once per pair (dy==0 computes into Up0+Up1)
				if (dy == 0) {
					int cy = row2 >> 1;
					const uint8_t* cbTop = pCb + (size_t)cy * cbStride;
					const uint8_t* crTop = pCr + (size_t)cy * crStride;
					int cyNext = (cy + 1 < chromaRows) ? (cy + 1) : cy;
					const uint8_t* cbBot = pCb + (size_t)cyNext * cbStride;
					const uint8_t* crBot = pCr + (size_t)cyNext * crStride;
					UpsampleChromaRowPairBilinear(cbTop, cbBot, crTop, crBot, cw,
						cbUp0.data(), cbUp1.data(), crUp0.data(), crUp1.data());
				}
			}

			for (int col = 0; col < width; col++) {
				float yv = (float)lineY[col];
				float cb = (float)cbUp[col] - 128.0f;
				float cr = (float)crUp[col] - 128.0f;
				if (!fullRange) {
					yv = (yv - 16.0f) * 1.1689f;
					cb = cb * 1.1429f;
					cr = cr * 1.1429f;
				}
				out[col * 4 + 0] = ClipU8Float(yv + b_cb * cb);
				out[col * 4 + 1] = ClipU8Float(yv + g_cb * cb + g_cr * cr);
				out[col * 4 + 2] = ClipU8Float(yv + r_cr * cr);
				out[col * 4 + 3] = hasAlpha ? lineA[col] : 0xFF;
			}
		}
	}
}

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

	// Primary accelerated path: GPU Hardware HEVC Decoding (Direct3D 11 + Media Foundation MFT)
	if (GpuHeifDecoder::DecodeHeif(buffer, sizebytes, frame_index, width, height, nchannels, frame_count, (void*&)pPixelData, exif_chunk, has_alpha, outOfMemory)) {
		if (pPixelData != NULL) {
			return (void*)pPixelData;
		}
	}

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

	// Fast path: request planar YCbCr output so libheif's scalar, single-threaded
	// color conversion pipeline is bypassed entirely. The decoder's native planes
	// are converted to BGRA ourselves in one fused parallel pass below.
	heif_image* raw_image = NULL;
	heif_error decode_err = heif_decode_image(
		handle.get_raw_image_handle(),
		&raw_image,
		heif_colorspace_YCbCr,
		heif_chroma_420, // force planar 4:2:0 output
		decode_options
	);

	if (decode_err.code != heif_error_Ok || raw_image == NULL) {
		if (decode_options != NULL) {
			heif_decoding_options_free(decode_options);
		}
		return NULL;
	}

	heif::Image image(raw_image);
	bool fastPath = false;
	uint8_t* planeY = NULL; int strideY = 0;
	uint8_t* planeCb = NULL; int strideCb = 0;
	uint8_t* planeCr = NULL; int strideCr = 0;

	if (image.get_chroma_format() == heif_chroma_420 &&
		image.get_bits_per_pixel(heif_channel_Y) == 8 &&
		image.get_bits_per_pixel(heif_channel_Cb) == 8 &&
		image.get_bits_per_pixel(heif_channel_Cr) == 8) {
		planeY = image.get_plane(heif_channel_Y, &strideY);
		planeCb = image.get_plane(heif_channel_Cb, &strideCb);
		planeCr = image.get_plane(heif_channel_Cr, &strideCr);
		fastPath = (planeY != NULL && planeCb != NULL && planeCr != NULL);
	}

	if (fastPath && !has_alpha) {
		width = image.get_width(heif_channel_Y);
		height = image.get_height(heif_channel_Y);

		if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION ||
			abs((double)width * height) > MAX_IMAGE_PIXELS || width < 1 || height < 1) {
			if ((double)width * height > MAX_IMAGE_PIXELS) {
				outOfMemory = true;
			}
			heif_decoding_options_free(decode_options);
			return NULL;
		}

		pPixelData = new(std::nothrow) unsigned char[width * 4 * height];
		if (pPixelData == NULL) {
			outOfMemory = true;
			heif_decoding_options_free(decode_options);
			return NULL;
		}

		// Use the NCLX profile attached to the decoded image - this is exactly
		// what libheif's conversion pipeline would consult (including profiles
		// recovered from the bitstream).
		YCbCrCoeffs coeffs = { 1.402f, -0.344136f, -0.714136f, 1.772f };
		bool fullRange = true;
		heif_color_profile_nclx* nclx = NULL;
		heif_error nclx_err = heif_image_get_nclx_color_profile(raw_image, &nclx);
		if (nclx_err.code == heif_error_Ok && nclx != NULL) {
			coeffs = GetYCbCrToRGBCoefficients(nclx->matrix_coefficients);
			fullRange = nclx->full_range_flag != 0;
			heif_nclx_color_profile_free(nclx);
		}

		int chromaRows = image.get_height(heif_channel_Cb);
		ConvertYCbCr420ToBGRA(planeY, strideY, planeCb, strideCb, planeCr, strideCr, chromaRows, pPixelData, width, height, has_alpha, NULL, 0, coeffs, fullRange);

		// An embedded ICC profile overrides the NCLX-derived color interpretation.
		std::vector<uint8_t> iccp = image.get_raw_color_profile();
		void* transform = ICCProfileTransform::CreateTransform(iccp.data(), (unsigned int)iccp.size(), ICCProfileTransform::FORMAT_BGRA);
		if (transform != NULL) {
			ICCProfileTransform::DoTransform(transform, pPixelData, pPixelData, width, height);
			ICCProfileTransform::DeleteTransform(transform);
		}
	}
	else {
		// Fallback: original interleaved RGBA path (alpha images, HDR->8bit,
		// monochrome, 444, unusual subsamplings etc.)
		heif_image* rgba_image = NULL;
		heif_error rgba_err = heif_decode_image(
			handle.get_raw_image_handle(),
			&rgba_image,
			heif_colorspace_RGB,
			heif_chroma_interleaved_RGBA,
			decode_options
		);

		image = heif::Image(NULL); // release the YCbCr attempt early

		if (rgba_err.code != heif_error_Ok || rgba_image == NULL) {
			heif_decoding_options_free(decode_options);
			return NULL;
		}
		image = heif::Image(rgba_image);
		int stride;
		uint8_t* data = image.get_plane(heif_channel_interleaved, &stride);
		width = image.get_width(heif_channel_interleaved);
		height = image.get_height(heif_channel_interleaved);

		if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
			heif_decoding_options_free(decode_options);
			return NULL;
		}
		if (abs((double)width * height) > MAX_IMAGE_PIXELS) {
			outOfMemory = true;
			heif_decoding_options_free(decode_options);
			return NULL;
		}
		if (width < 1 || height < 1 || width * nchannels > stride) {
			heif_decoding_options_free(decode_options);
			return NULL;
		}

		int size = width * nchannels * height;
		pPixelData = new(std::nothrow) unsigned char[size];
		if (pPixelData == NULL) {
			outOfMemory = true;
			heif_decoding_options_free(decode_options);
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
	}

	heif_decoding_options_free(decode_options);

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
