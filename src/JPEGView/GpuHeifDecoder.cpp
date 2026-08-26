#include "stdafx.h"
#include "GpuHeifDecoder.h"
#include "MaxImageDef.h"
#include "ICCProfileTransform.h"
#include "Helpers.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <codecapi.h>

#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <immintrin.h>
#include <omp.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Ole32.lib")

namespace {

// YCbCr -> RGB coefficients
struct YCbCrCoefficients {
	float r_cr;
	float g_cb;
	float g_cr;
	float b_cb;
};

static YCbCrCoefficients GetNclxCoefficients(uint16_t matrix_coefficients) {
	float Kr = 0.0f, Kb = 0.0f;
	switch (matrix_coefficients) {
		case 1:  Kr = 0.2126f; Kb = 0.0722f; break; // BT.709
		case 4:  Kr = 0.30f;   Kb = 0.11f;   break; // FCC
		case 5:
		case 6:  Kr = 0.299f;  Kb = 0.114f;  break; // BT.601
		case 7:  Kr = 0.212f;  Kb = 0.087f;  break; // SMPTE 240M
		case 9:
		case 10: Kr = 0.2627f; Kb = 0.0593f; break; // BT.2020
		default: break;
	}
	if (Kr == 0.0f && Kb == 0.0f) {
		// Default BT.601
		return { 1.402f, -0.344136f, -0.714136f, 1.772f };
	}
	YCbCrCoefficients c;
	c.r_cr = 2.0f * (1.0f - Kr);
	c.g_cb = 2.0f * Kb * (1.0f - Kb) / (Kb + Kr - 1.0f);
	c.g_cr = 2.0f * Kr * (1.0f - Kr) / (Kb + Kr - 1.0f);
	c.b_cb = 2.0f * (1.0f - Kb);
	return c;
}

// Ultra-fast 256-bit AVX2 FMA NV12 -> BGRA conversion kernel (Unrotated, 100% pixel perfect)
static void ConvertNV12ToBGRA_AVX2(
	const uint8_t* pNV12, uint32_t width, uint32_t height, uint32_t stride,
	uint8_t* pDstBGRA, float r_cr, float g_cb, float g_cr, float b_cb, bool fullRange)
{
	const uint8_t* pY = pNV12;
	const uint8_t* pUV = pNV12 + (size_t)stride * height;

	float uv_scale = fullRange ? 1.0f : (255.0f / 224.0f);
	float y_scale = fullRange ? 1.0f : (255.0f / 219.0f);
	float y_bias = fullRange ? 0.0f : (16.0f * 255.0f / 219.0f);

#if defined(__AVX2__)
	const __m256 vR_CR = _mm256_set1_ps(r_cr * uv_scale);
	const __m256 vG_CB = _mm256_set1_ps(g_cb * uv_scale);
	const __m256 vG_CR = _mm256_set1_ps(g_cr * uv_scale);
	const __m256 vB_CB = _mm256_set1_ps(b_cb * uv_scale);
	const __m256 vY_scale = _mm256_set1_ps(y_scale);
	const __m256 vY_bias = _mm256_set1_ps(y_bias);
	const __m256 v128 = _mm256_set1_ps(128.0f);

	const __m128i maskU_lo = _mm_setr_epi8(0, 0, 2, 2, 4, 4, 6, 6, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m128i maskV_lo = _mm_setr_epi8(1, 1, 3, 3, 5, 5, 7, 7, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m128i maskU_hi = _mm_setr_epi8(8, 8, 10, 10, 12, 12, 14, 14, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m128i maskV_hi = _mm_setr_epi8(9, 9, 11, 11, 13, 13, 15, 15, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m128i a_const = _mm_set1_epi8((char)0xFF);
#endif

	#pragma omp parallel for schedule(static)
	for (int y = 0; y < (int)height; y += 2) {
		for (int dy = 0; dy < 2 && (y + dy) < (int)height; dy++) {
			int curY = y + dy;
			const uint8_t* lineY = pY + (size_t)curY * stride;
			const uint8_t* lineUV = pUV + (size_t)(y / 2) * stride;
			uint8_t* out = pDstBGRA + (size_t)curY * (width * 4);

			int x = 0;

#if defined(__AVX2__)
			for (; x + 15 < (int)width; x += 16) {
				__m128i y_raw = _mm_loadu_si128((const __m128i*)(lineY + x));
				__m128i uv_raw = _mm_loadu_si128((const __m128i*)(lineUV + x));

				// Low 8 pixels (pixels 0..7)
				__m256i y_lo_i32 = _mm256_cvtepu8_epi32(y_raw);
				__m256 y_lo_f = _mm256_fmsub_ps(_mm256_cvtepi32_ps(y_lo_i32), vY_scale, vY_bias);

				__m256i u_lo_i32 = _mm256_cvtepu8_epi32(_mm_shuffle_epi8(uv_raw, maskU_lo));
				__m256i v_lo_i32 = _mm256_cvtepu8_epi32(_mm_shuffle_epi8(uv_raw, maskV_lo));
				__m256 u_lo_f = _mm256_sub_ps(_mm256_cvtepi32_ps(u_lo_i32), v128);
				__m256 v_lo_f = _mm256_sub_ps(_mm256_cvtepi32_ps(v_lo_i32), v128);

				__m256 r_lo_f = _mm256_fmadd_ps(v_lo_f, vR_CR, y_lo_f);
				__m256 g_lo_f = _mm256_fmadd_ps(u_lo_f, vG_CB, _mm256_fmadd_ps(v_lo_f, vG_CR, y_lo_f));
				__m256 b_lo_f = _mm256_fmadd_ps(u_lo_f, vB_CB, y_lo_f);

				__m256i r_lo_i = _mm256_cvtps_epi32(r_lo_f);
				__m256i g_lo_i = _mm256_cvtps_epi32(g_lo_f);
				__m256i b_lo_i = _mm256_cvtps_epi32(b_lo_f);

				// High 8 pixels (pixels 8..15)
				__m256i y_hi_i32 = _mm256_cvtepu8_epi32(_mm_srli_si128(y_raw, 8));
				__m256 y_hi_f = _mm256_fmsub_ps(_mm256_cvtepi32_ps(y_hi_i32), vY_scale, vY_bias);

				__m256i u_hi_i32 = _mm256_cvtepu8_epi32(_mm_shuffle_epi8(uv_raw, maskU_hi));
				__m256i v_hi_i32 = _mm256_cvtepu8_epi32(_mm_shuffle_epi8(uv_raw, maskV_hi));
				__m256 u_hi_f = _mm256_sub_ps(_mm256_cvtepi32_ps(u_hi_i32), v128);
				__m256 v_hi_f = _mm256_sub_ps(_mm256_cvtepi32_ps(v_hi_i32), v128);

				__m256 r_hi_f = _mm256_fmadd_ps(v_hi_f, vR_CR, y_hi_f);
				__m256 g_hi_f = _mm256_fmadd_ps(u_hi_f, vG_CB, _mm256_fmadd_ps(v_hi_f, vG_CR, y_hi_f));
				__m256 b_hi_f = _mm256_fmadd_ps(u_hi_f, vB_CB, y_hi_f);

				__m256i r_hi_i = _mm256_cvtps_epi32(r_hi_f);
				__m256i g_hi_i = _mm256_cvtps_epi32(g_hi_f);
				__m256i b_hi_i = _mm256_cvtps_epi32(b_hi_f);

				// Pack 8x 32-bit integers into 16-bit integers
				__m128i r_lo_16 = _mm_packus_epi32(_mm256_castsi256_si128(r_lo_i), _mm256_extracti128_si256(r_lo_i, 1));
				__m128i r_hi_16 = _mm_packus_epi32(_mm256_castsi256_si128(r_hi_i), _mm256_extracti128_si256(r_hi_i, 1));
				__m128i r8 = _mm_packus_epi16(r_lo_16, r_hi_16);

				__m128i g_lo_16 = _mm_packus_epi32(_mm256_castsi256_si128(g_lo_i), _mm256_extracti128_si256(g_lo_i, 1));
				__m128i g_hi_16 = _mm_packus_epi32(_mm256_castsi256_si128(g_hi_i), _mm256_extracti128_si256(g_hi_i, 1));
				__m128i g8 = _mm_packus_epi16(g_lo_16, g_hi_16);

				__m128i b_lo_16 = _mm_packus_epi32(_mm256_castsi256_si128(b_lo_i), _mm256_extracti128_si256(b_lo_i, 1));
				__m128i b_hi_16 = _mm_packus_epi32(_mm256_castsi256_si128(b_hi_i), _mm256_extracti128_si256(b_hi_i, 1));
				__m128i b8 = _mm_packus_epi16(b_lo_16, b_hi_16);

				// Interleave B, G, R, A
				__m128i bg_lo = _mm_unpacklo_epi8(b8, g8);
				__m128i ra_lo = _mm_unpacklo_epi8(r8, a_const);
				__m128i bgra0 = _mm_unpacklo_epi16(bg_lo, ra_lo);
				__m128i bgra1 = _mm_unpackhi_epi16(bg_lo, ra_lo);

				__m128i bg_hi = _mm_unpackhi_epi8(b8, g8);
				__m128i ra_hi = _mm_unpackhi_epi8(r8, a_const);
				__m128i bgra2 = _mm_unpacklo_epi16(bg_hi, ra_hi);
				__m128i bgra3 = _mm_unpackhi_epi16(bg_hi, ra_hi);

				_mm_storeu_si128((__m128i*)(out + (x + 0) * 4), bgra0);
				_mm_storeu_si128((__m128i*)(out + (x + 4) * 4), bgra1);
				_mm_storeu_si128((__m128i*)(out + (x + 8) * 4), bgra2);
				_mm_storeu_si128((__m128i*)(out + (x + 12) * 4), bgra3);
			}
#endif

			for (; x < (int)width; x += 2) {
				float cb = (float)lineUV[x] - 128.0f;
				float cr = (float)lineUV[x + 1] - 128.0f;
				if (!fullRange) {
					cb *= (255.0f / 224.0f);
					cr *= (255.0f / 224.0f);
				}

				float r_off = r_cr * cr;
				float g_off = g_cb * cb + g_cr * cr;
				float b_off = b_cb * cb;

				for (int dx = 0; dx < 2 && (x + dx) < (int)width; dx++) {
					float yval = (float)lineY[x + dx];
					if (!fullRange) yval = (yval - 16.0f) * y_scale;

					int b = (int)(yval + b_off + 0.5f);
					int g = (int)(yval + g_off + 0.5f);
					int r = (int)(yval + r_off + 0.5f);
					out[(x + dx) * 4 + 0] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
					out[(x + dx) * 4 + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
					out[(x + dx) * 4 + 2] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
					out[(x + dx) * 4 + 3] = 0xFF;
				}
			}
		}
	}
}

// Fused NV12 -> Rotated BGRA single-pass conversion
static void ConvertNV12ToBGRA_FusedRot(
	const uint8_t* pNV12, uint32_t width, uint32_t height, uint32_t stride,
	uint32_t* pDstBGRA, float r_cr, float g_cb, float g_cr, float b_cb, bool fullRange,
	int angle_ccw, int mirror_mode)
{
	const uint8_t* pY = pNV12;
	const uint8_t* pUV = pNV12 + (size_t)stride * height;

	float uv_scale = fullRange ? 1.0f : (255.0f / 224.0f);
	float y_scale = fullRange ? 1.0f : (255.0f / 219.0f);

	float R_CR = r_cr * uv_scale;
	float G_CB = g_cb * uv_scale;
	float G_CR = g_cr * uv_scale;
	float B_CB = b_cb * uv_scale;

	const int outW = (angle_ccw == 1 || angle_ccw == 3) ? (int)height : (int)width;

	const int TILE_Y = 32;
	const int TILE_X = 64;

	#pragma omp parallel for schedule(dynamic)
	for (int ty = 0; ty < (int)height; ty += TILE_Y) {
		int maxTy = min(ty + TILE_Y, (int)height);
		for (int tx = 0; tx < (int)width; tx += TILE_X) {
			int maxTx = min(tx + TILE_X, (int)width);

			for (int y = ty; y < maxTy; y += 2) {
				for (int dy = 0; dy < 2 && (y + dy) < maxTy; dy++) {
					int curY = y + dy;
					const uint8_t* lineY = pY + (size_t)curY * stride;
					const uint8_t* lineUV = pUV + (size_t)(y / 2) * stride;

					for (int x = tx; x < maxTx; x += 2) {
						float cb = (float)lineUV[x] - 128.0f;
						float cr = (float)lineUV[x + 1] - 128.0f;

						float r_off = R_CR * cr;
						float g_off = G_CB * cb + G_CR * cr;
						float b_off = B_CB * cb;

						for (int dx = 0; dx < 2 && (x + dx) < maxTx; dx++) {
							int curX = x + dx;
							float yval = (float)lineY[curX];
							if (!fullRange) yval = (yval - 16.0f) * y_scale;

							int b = (int)(yval + b_off + 0.5f);
							int g = (int)(yval + g_off + 0.5f);
							int r = (int)(yval + r_off + 0.5f);

							uint8_t u_b = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
							uint8_t u_g = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
							uint8_t u_r = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
							uint32_t bgra = (uint32_t)u_b | ((uint32_t)u_g << 8) | ((uint32_t)u_r << 16) | 0xFF000000;

							int srcX = (mirror_mode == 1) ? ((int)width - 1 - curX) : curX;
							int srcY = (mirror_mode == 0) ? ((int)height - 1 - curY) : curY;
							int dstX = 0, dstY = 0;

							if (angle_ccw == 1) { // 90 CCW
								dstX = srcY;
								dstY = (int)width - 1 - srcX;
							} else if (angle_ccw == 2) { // 180
								dstX = (int)width - 1 - srcX;
								dstY = (int)height - 1 - srcY;
							} else if (angle_ccw == 3) { // 270 CCW = 90 CW
								dstX = (int)height - 1 - srcY;
								dstY = srcX;
							} else {
								dstX = srcX;
								dstY = srcY;
							}
							pDstBGRA[dstY * outW + dstX] = bgra;
						}
					}
				}
			}
		}
	}
}

// Ultra-fast 256-bit AVX2 FMA Y210 (10-bit 4:2:2) -> BGRA conversion kernel
static void ConvertY210ToBGRA_AVX2(
	const uint8_t* pSrc, uint32_t width, uint32_t height, uint32_t stride,
	uint8_t* pDstBGRA, float r_cr, float g_cb, float g_cr, float b_cb, bool fullRange)
{
	float norm = 255.0f / 65472.0f; // 10-bit in MSB: 1023 << 6 = 65472
	float y_scale = fullRange ? norm : (norm * 255.0f / 219.0f);
	float y_bias = fullRange ? 0.0f : (16.0f * 255.0f / 219.0f);
	float c_scale = fullRange ? norm : (norm * 255.0f / 224.0f);
	float c_center = 32768.0f * c_scale; // scaled center

#if defined(__AVX2__)
	const __m256 vY_scale = _mm256_set1_ps(y_scale);
	const __m256 vY_bias  = _mm256_set1_ps(y_bias);
	const __m256 vC_scale = _mm256_set1_ps(c_scale);
	const __m256 vC_center = _mm256_set1_ps(c_center);
	const __m256 vR_CR    = _mm256_set1_ps(r_cr);
	const __m256 vG_CB    = _mm256_set1_ps(g_cb);
	const __m256 vG_CR    = _mm256_set1_ps(g_cr);
	const __m256 vB_CB    = _mm256_set1_ps(b_cb);
	const __m128i a_const = _mm_set1_epi8((char)0xFF);

	const __m128i maskY = _mm_setr_epi8(0, 1, 4, 5, 8, 9, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m128i maskU = _mm_setr_epi8(2, 3, 2, 3, 10, 11, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m128i maskV = _mm_setr_epi8(6, 7, 6, 7, 14, 15, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1);
#endif

	#pragma omp parallel for schedule(static)
	for (int y = 0; y < (int)height; y++) {
		const uint16_t* line = (const uint16_t*)(pSrc + (size_t)y * stride);
		uint8_t* out = pDstBGRA + (size_t)y * (width * 4);
		int x = 0;

#if defined(__AVX2__)
		for (; x + 7 < (int)width; x += 8) {
			__m128i raw_lo = _mm_loadu_si128((const __m128i*)(line + x * 2 + 0));
			__m128i raw_hi = _mm_loadu_si128((const __m128i*)(line + x * 2 + 8));

			__m128i y_lo_16 = _mm_shuffle_epi8(raw_lo, maskY);
			__m128i u_lo_16 = _mm_shuffle_epi8(raw_lo, maskU);
			__m128i v_lo_16 = _mm_shuffle_epi8(raw_lo, maskV);

			__m128i y_hi_16 = _mm_shuffle_epi8(raw_hi, maskY);
			__m128i u_hi_16 = _mm_shuffle_epi8(raw_hi, maskU);
			__m128i v_hi_16 = _mm_shuffle_epi8(raw_hi, maskV);

			__m256 y_f = _mm256_cvtepi32_ps(_mm256_set_m128i(_mm_cvtepu16_epi32(y_hi_16), _mm_cvtepu16_epi32(y_lo_16)));
			__m256 u_f = _mm256_cvtepi32_ps(_mm256_set_m128i(_mm_cvtepu16_epi32(u_hi_16), _mm_cvtepu16_epi32(u_lo_16)));
			__m256 v_f = _mm256_cvtepi32_ps(_mm256_set_m128i(_mm_cvtepu16_epi32(v_hi_16), _mm_cvtepu16_epi32(v_lo_16)));

			__m256 y_val = _mm256_fmsub_ps(y_f, vY_scale, vY_bias);
			__m256 cb    = _mm256_fmsub_ps(u_f, vC_scale, vC_center);
			__m256 cr    = _mm256_fmsub_ps(v_f, vC_scale, vC_center);

			__m256 r_f = _mm256_fmadd_ps(cr, vR_CR, y_val);
			__m256 g_f = _mm256_fmadd_ps(cb, vG_CB, _mm256_fmadd_ps(cr, vG_CR, y_val));
			__m256 b_f = _mm256_fmadd_ps(cb, vB_CB, y_val);

			__m256i r_i = _mm256_cvtps_epi32(r_f);
			__m256i g_i = _mm256_cvtps_epi32(g_f);
			__m256i b_i = _mm256_cvtps_epi32(b_f);

			__m128i r_lo = _mm256_castsi256_si128(r_i);
			__m128i r_hi = _mm256_extracti128_si256(r_i, 1);
			__m128i r16 = _mm_packus_epi32(r_lo, r_hi);
			__m128i r8  = _mm_packus_epi16(r16, r16);

			__m128i g_lo = _mm256_castsi256_si128(g_i);
			__m128i g_hi = _mm256_extracti128_si256(g_i, 1);
			__m128i g16 = _mm_packus_epi32(g_lo, g_hi);
			__m128i g8  = _mm_packus_epi16(g16, g16);

			__m128i b_lo = _mm256_castsi256_si128(b_i);
			__m128i b_hi = _mm256_extracti128_si256(b_i, 1);
			__m128i b16 = _mm_packus_epi32(b_lo, b_hi);
			__m128i b8  = _mm_packus_epi16(b16, b16);

			__m128i bg_lo = _mm_unpacklo_epi8(b8, g8);
			__m128i ra_lo = _mm_unpacklo_epi8(r8, a_const);
			__m128i bgra0 = _mm_unpacklo_epi16(bg_lo, ra_lo);
			__m128i bgra1 = _mm_unpackhi_epi16(bg_lo, ra_lo);

			_mm_storeu_si128((__m128i*)(out + (x + 0) * 4), bgra0);
			_mm_storeu_si128((__m128i*)(out + (x + 4) * 4), bgra1);
		}
#endif

		for (; x < (int)width; x += 2) {
			float y0_raw = (float)line[x * 2 + 0];
			float u_raw  = (float)line[x * 2 + 1];
			float y1_raw = (float)line[x * 2 + 2];
			float v_raw  = (float)line[x * 2 + 3];

			float cb = u_raw * c_scale - c_center;
			float cr = v_raw * c_scale - c_center;

			float r_off = r_cr * cr;
			float g_off = g_cb * cb + g_cr * cr;
			float b_off = b_cb * cb;

			float y0 = y0_raw * y_scale - y_bias;
			int b0 = (int)(y0 + b_off + 0.5f);
			int g0 = (int)(y0 + g_off + 0.5f);
			int r0 = (int)(y0 + r_off + 0.5f);
			out[(x + 0) * 4 + 0] = (uint8_t)(b0 < 0 ? 0 : (b0 > 255 ? 255 : b0));
			out[(x + 0) * 4 + 1] = (uint8_t)(g0 < 0 ? 0 : (g0 > 255 ? 255 : g0));
			out[(x + 0) * 4 + 2] = (uint8_t)(r0 < 0 ? 0 : (r0 > 255 ? 255 : r0));
			out[(x + 0) * 4 + 3] = 0xFF;

			if (x + 1 < (int)width) {
				float y1 = y1_raw * y_scale - y_bias;
				int b1 = (int)(y1 + b_off + 0.5f);
				int g1 = (int)(y1 + g_off + 0.5f);
				int r1 = (int)(y1 + r_off + 0.5f);
				out[(x + 1) * 4 + 0] = (uint8_t)(b1 < 0 ? 0 : (b1 > 255 ? 255 : b1));
				out[(x + 1) * 4 + 1] = (uint8_t)(g1 < 0 ? 0 : (g1 > 255 ? 255 : g1));
				out[(x + 1) * 4 + 2] = (uint8_t)(r1 < 0 ? 0 : (r1 > 255 ? 255 : r1));
				out[(x + 1) * 4 + 3] = 0xFF;
			}
		}
	}
}

// Fused Y210 -> Rotated BGRA single-pass conversion
static void ConvertY210ToBGRA_FusedRot(
	const uint8_t* pSrc, uint32_t width, uint32_t height, uint32_t stride,
	uint32_t* pDstBGRA, float r_cr, float g_cb, float g_cr, float b_cb, bool fullRange,
	int angle_ccw, int mirror_mode)
{
	float norm = 255.0f / 65472.0f;
	float y_scale = fullRange ? norm : (norm * 255.0f / 219.0f);
	float y_bias = fullRange ? 0.0f : (16.0f * 255.0f / 219.0f);
	float c_scale = fullRange ? norm : (norm * 255.0f / 224.0f);
	float c_center = 32768.0f * c_scale;

	const int outW = (angle_ccw == 1 || angle_ccw == 3) ? (int)height : (int)width;
	const int TILE_Y = 32;
	const int TILE_X = 64;

	#pragma omp parallel for schedule(dynamic)
	for (int ty = 0; ty < (int)height; ty += TILE_Y) {
		int maxTy = min(ty + TILE_Y, (int)height);
		for (int tx = 0; tx < (int)width; tx += TILE_X) {
			int maxTx = min(tx + TILE_X, (int)width);

			for (int y = ty; y < maxTy; y++) {
				const uint16_t* line = (const uint16_t*)(pSrc + (size_t)y * stride);

				for (int x = tx; x < maxTx; x += 2) {
					float y0_raw = (float)line[x * 2 + 0];
					float u_raw  = (float)line[x * 2 + 1];
					float y1_raw = (float)line[x * 2 + 2];
					float v_raw  = (float)line[x * 2 + 3];

					float cb = u_raw * c_scale - c_center;
					float cr = v_raw * c_scale - c_center;

					float r_off = r_cr * cr;
					float g_off = g_cb * cb + g_cr * cr;
					float b_off = b_cb * cb;

					for (int dx = 0; dx < 2 && (x + dx) < maxTx; dx++) {
						int curX = x + dx;
						float yraw = (dx == 0) ? y0_raw : y1_raw;
						float yval = yraw * y_scale - y_bias;

						int b = (int)(yval + b_off + 0.5f);
						int g = (int)(yval + g_off + 0.5f);
						int r = (int)(yval + r_off + 0.5f);

						uint8_t u_b = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
						uint8_t u_g = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
						uint8_t u_r = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
						uint32_t bgra = (uint32_t)u_b | ((uint32_t)u_g << 8) | ((uint32_t)u_r << 16) | 0xFF000000;

						int srcX = (mirror_mode == 1) ? ((int)width - 1 - curX) : curX;
						int srcY = (mirror_mode == 0) ? ((int)height - 1 - y) : y;
						int dstX = 0, dstY = 0;

						if (angle_ccw == 1) { // 90 CCW
							dstX = srcY;
							dstY = (int)width - 1 - srcX;
						} else if (angle_ccw == 2) { // 180
							dstX = (int)width - 1 - srcX;
							dstY = (int)height - 1 - srcY;
						} else if (angle_ccw == 3) { // 270 CCW = 90 CW
							dstX = (int)height - 1 - srcY;
							dstY = srcX;
						} else {
							dstX = srcX;
							dstY = srcY;
						}
						pDstBGRA[dstY * outW + dstX] = bgra;
					}
				}
			}
		}
	}
}

// Ultra-fast 256-bit AVX2 FMA P010 (10-bit 4:2:0) -> BGRA conversion kernel
static void ConvertP010ToBGRA_AVX2(
	const uint8_t* pP010, uint32_t width, uint32_t height, uint32_t stride,
	uint8_t* pDstBGRA, float r_cr, float g_cb, float g_cr, float b_cb, bool fullRange)
{
	const uint8_t* pY = pP010;
	const uint8_t* pUV = pP010 + (size_t)stride * height;

	float norm = 255.0f / 65472.0f;
	float y_scale = fullRange ? norm : (norm * 255.0f / 219.0f);
	float y_bias = fullRange ? 0.0f : (16.0f * 255.0f / 219.0f);
	float c_scale = fullRange ? norm : (norm * 255.0f / 224.0f);
	float c_center = 32768.0f * c_scale;

#if defined(__AVX2__)
	const __m256 vY_scale = _mm256_set1_ps(y_scale);
	const __m256 vY_bias  = _mm256_set1_ps(y_bias);
	const __m256 vC_scale = _mm256_set1_ps(c_scale);
	const __m256 vC_center = _mm256_set1_ps(c_center);
	const __m256 vR_CR    = _mm256_set1_ps(r_cr);
	const __m256 vG_CB    = _mm256_set1_ps(g_cb);
	const __m256 vG_CR    = _mm256_set1_ps(g_cr);
	const __m256 vB_CB    = _mm256_set1_ps(b_cb);
	const __m128i a_const = _mm_set1_epi8((char)0xFF);

	const __m128i maskU = _mm_setr_epi8(0, 1, 0, 1, 4, 5, 4, 5, 8, 9, 8, 9, 12, 13, 12, 13);
	const __m128i maskV = _mm_setr_epi8(2, 3, 2, 3, 6, 7, 6, 7, 10, 11, 10, 11, 14, 15, 14, 15);
#endif

	#pragma omp parallel for schedule(static)
	for (int y = 0; y < (int)height; y += 2) {
		for (int dy = 0; dy < 2 && (y + dy) < (int)height; dy++) {
			int curY = y + dy;
			const uint16_t* lineY = (const uint16_t*)(pY + (size_t)curY * stride);
			const uint16_t* lineUV = (const uint16_t*)(pUV + (size_t)(y / 2) * stride);
			uint8_t* out = pDstBGRA + (size_t)curY * (width * 4);

			int x = 0;

#if defined(__AVX2__)
			for (; x + 7 < (int)width; x += 8) {
				__m128i y_raw = _mm_loadu_si128((const __m128i*)(lineY + x));
				__m128i uv_raw = _mm_loadu_si128((const __m128i*)(lineUV + x));

				__m128i u_16 = _mm_shuffle_epi8(uv_raw, maskU);
				__m128i v_16 = _mm_shuffle_epi8(uv_raw, maskV);

				__m256 y_f = _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(y_raw));
				__m256 u_f = _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(u_16));
				__m256 v_f = _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(v_16));

				__m256 y_val = _mm256_fmsub_ps(y_f, vY_scale, vY_bias);
				__m256 cb    = _mm256_fmsub_ps(u_f, vC_scale, vC_center);
				__m256 cr    = _mm256_fmsub_ps(v_f, vC_scale, vC_center);

				__m256 r_f = _mm256_fmadd_ps(cr, vR_CR, y_val);
				__m256 g_f = _mm256_fmadd_ps(cb, vG_CB, _mm256_fmadd_ps(cr, vG_CR, y_val));
				__m256 b_f = _mm256_fmadd_ps(cb, vB_CB, y_val);

				__m256i r_i = _mm256_cvtps_epi32(r_f);
				__m256i g_i = _mm256_cvtps_epi32(g_f);
				__m256i b_i = _mm256_cvtps_epi32(b_f);

				__m128i r_lo = _mm256_castsi256_si128(r_i);
				__m128i r_hi = _mm256_extracti128_si256(r_i, 1);
				__m128i r16 = _mm_packus_epi32(r_lo, r_hi);
				__m128i r8  = _mm_packus_epi16(r16, r16);

				__m128i g_lo = _mm256_castsi256_si128(g_i);
				__m128i g_hi = _mm256_extracti128_si256(g_i, 1);
				__m128i g16 = _mm_packus_epi32(g_lo, g_hi);
				__m128i g8  = _mm_packus_epi16(g16, g16);

				__m128i b_lo = _mm256_castsi256_si128(b_i);
				__m128i b_hi = _mm256_extracti128_si256(b_i, 1);
				__m128i b16 = _mm_packus_epi32(b_lo, b_hi);
				__m128i b8  = _mm_packus_epi16(b16, b16);

				__m128i bg_lo = _mm_unpacklo_epi8(b8, g8);
				__m128i ra_lo = _mm_unpacklo_epi8(r8, a_const);
				__m128i bgra0 = _mm_unpacklo_epi16(bg_lo, ra_lo);
				__m128i bgra1 = _mm_unpackhi_epi16(bg_lo, ra_lo);

				_mm_storeu_si128((__m128i*)(out + (x + 0) * 4), bgra0);
				_mm_storeu_si128((__m128i*)(out + (x + 4) * 4), bgra1);
			}
#endif

			for (; x < (int)width; x += 2) {
				float cb = (float)lineUV[x] * c_scale - c_center;
				float cr = (float)lineUV[x + 1] * c_scale - c_center;

				float r_off = r_cr * cr;
				float g_off = g_cb * cb + g_cr * cr;
				float b_off = b_cb * cb;

				for (int dx = 0; dx < 2 && (x + dx) < (int)width; dx++) {
					float yval = (float)lineY[x + dx] * y_scale - y_bias;

					int b = (int)(yval + b_off + 0.5f);
					int g = (int)(yval + g_off + 0.5f);
					int r = (int)(yval + r_off + 0.5f);
					out[(x + dx) * 4 + 0] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
					out[(x + dx) * 4 + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
					out[(x + dx) * 4 + 2] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
					out[(x + dx) * 4 + 3] = 0xFF;
				}
			}
		}
	}
}

static void ConvertP010ToBGRA_FusedRot(
	const uint8_t* pP010, uint32_t width, uint32_t height, uint32_t stride,
	uint32_t* pDstBGRA, float r_cr, float g_cb, float g_cr, float b_cb, bool fullRange,
	int angle_ccw, int mirror_mode)
{
	const uint8_t* pY = pP010;
	const uint8_t* pUV = pP010 + (size_t)stride * height;

	float norm = 255.0f / 65472.0f;
	float y_scale = fullRange ? norm : (norm * 255.0f / 219.0f);
	float y_bias = fullRange ? 0.0f : (16.0f * 255.0f / 219.0f);
	float c_scale = fullRange ? norm : (norm * 255.0f / 224.0f);
	float c_center = 32768.0f * c_scale;

	const int outW = (angle_ccw == 1 || angle_ccw == 3) ? (int)height : (int)width;
	const int TILE_Y = 32;
	const int TILE_X = 64;

	#pragma omp parallel for schedule(dynamic)
	for (int ty = 0; ty < (int)height; ty += TILE_Y) {
		int maxTy = min(ty + TILE_Y, (int)height);
		for (int tx = 0; tx < (int)width; tx += TILE_X) {
			int maxTx = min(tx + TILE_X, (int)width);

			for (int y = ty; y < maxTy; y += 2) {
				for (int dy = 0; dy < 2 && (y + dy) < maxTy; dy++) {
					int curY = y + dy;
					const uint16_t* lineY = (const uint16_t*)(pY + (size_t)curY * stride);
					const uint16_t* lineUV = (const uint16_t*)(pUV + (size_t)(y / 2) * stride);

					for (int x = tx; x < maxTx; x += 2) {
						float cb = (float)lineUV[x] * c_scale - c_center;
						float cr = (float)lineUV[x + 1] * c_scale - c_center;

						float r_off = r_cr * cr;
						float g_off = g_cb * cb + g_cr * cr;
						float b_off = b_cb * cb;

						for (int dx = 0; dx < 2 && (x + dx) < maxTx; dx++) {
							int curX = x + dx;
							float yval = (float)lineY[curX] * y_scale - y_bias;

							int b = (int)(yval + b_off + 0.5f);
							int g = (int)(yval + g_off + 0.5f);
							int r = (int)(yval + r_off + 0.5f);

							uint8_t u_b = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
							uint8_t u_g = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
							uint8_t u_r = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
							uint32_t bgra = (uint32_t)u_b | ((uint32_t)u_g << 8) | ((uint32_t)u_r << 16) | 0xFF000000;

							int srcX = (mirror_mode == 1) ? ((int)width - 1 - curX) : curX;
							int srcY = (mirror_mode == 0) ? ((int)height - 1 - curY) : curY;
							int dstX = 0, dstY = 0;

							if (angle_ccw == 1) { // 90 CCW
								dstX = srcY;
								dstY = (int)width - 1 - srcX;
							} else if (angle_ccw == 2) { // 180
								dstX = (int)width - 1 - srcX;
								dstY = (int)height - 1 - srcY;
							} else if (angle_ccw == 3) { // 270 CCW = 90 CW
								dstX = (int)height - 1 - srcY;
								dstY = srcX;
							} else {
								dstX = srcX;
								dstY = srcY;
							}
							pDstBGRA[dstY * outW + dstX] = bgra;
						}
					}
				}
			}
		}
	}
}

// Convert arbitrary decoded hardware texture data to BGRA
static void ConvertDecodedTextureToBGRA(
	DXGI_FORMAT format,
	const uint8_t* pMappedData,
	uint32_t width,
	uint32_t height,
	uint32_t rowPitch,
	uint8_t* pDstBGRA,
	const YCbCrCoefficients& coeffs,
	bool fullRange,
	int rotation,
	int mirror)
{
	bool needsRot = (rotation != 0 || mirror >= 0);
	if (format == (DXGI_FORMAT)108 /* DXGI_FORMAT_Y210 */ || format == (DXGI_FORMAT)109 /* DXGI_FORMAT_Y216 */) {
		if (needsRot) {
			ConvertY210ToBGRA_FusedRot(pMappedData, width, height, rowPitch, (uint32_t*)pDstBGRA, coeffs.r_cr, coeffs.g_cb, coeffs.g_cr, coeffs.b_cb, fullRange, rotation, mirror);
		} else {
			ConvertY210ToBGRA_AVX2(pMappedData, width, height, rowPitch, pDstBGRA, coeffs.r_cr, coeffs.g_cb, coeffs.g_cr, coeffs.b_cb, fullRange);
		}
	} else if (format == (DXGI_FORMAT)104 /* DXGI_FORMAT_P010 */ || format == (DXGI_FORMAT)105 /* DXGI_FORMAT_P016 */) {
		if (needsRot) {
			ConvertP010ToBGRA_FusedRot(pMappedData, width, height, rowPitch, (uint32_t*)pDstBGRA, coeffs.r_cr, coeffs.g_cb, coeffs.g_cr, coeffs.b_cb, fullRange, rotation, mirror);
		} else {
			ConvertP010ToBGRA_AVX2(pMappedData, width, height, rowPitch, pDstBGRA, coeffs.r_cr, coeffs.g_cb, coeffs.g_cr, coeffs.b_cb, fullRange);
		}
	} else {
		// Default NV12
		if (needsRot) {
			ConvertNV12ToBGRA_FusedRot(pMappedData, width, height, rowPitch, (uint32_t*)pDstBGRA, coeffs.r_cr, coeffs.g_cb, coeffs.g_cr, coeffs.b_cb, fullRange, rotation, mirror);
		} else {
			ConvertNV12ToBGRA_AVX2(pMappedData, width, height, rowPitch, pDstBGRA, coeffs.r_cr, coeffs.g_cb, coeffs.g_cr, coeffs.b_cb, fullRange);
		}
	}
}

// Media Foundation High-Performance GPU Decoder Manager
class MfGpuContext {
public:
	static const int MAX_DECODERS = 1;
	ID3D11Device* m_pDevice = nullptr;
	ID3D11DeviceContext* m_pContext = nullptr;
	IMFDXGIDeviceManager* m_pDXGIManager = nullptr;
	IMFActivate* m_pDecoderActivate = nullptr;
	IMFTransform* m_pDecoders[MAX_DECODERS] = { nullptr };
	uint32_t m_curW[MAX_DECODERS] = { 0 };
	uint32_t m_curH[MAX_DECODERS] = { 0 };
	IMFSample* m_pInSample[MAX_DECODERS] = { nullptr };
	IMFMediaBuffer* m_pInBuf[MAX_DECODERS] = { nullptr };
	DWORD m_inBufCapacity[MAX_DECODERS] = { 0 };
	int m_numDecoders = 0;

	UINT m_resetToken = 0;
	bool m_initialized = false;
	bool m_supportChecked = false;
	bool m_hasHwDecoder = false;

	// Reusable textures
	ID3D11Texture2D* m_pStagingTex = nullptr;
	uint32_t m_stagingW = 0, m_stagingH = 0;
	DXGI_FORMAT m_stagingFormat = DXGI_FORMAT_UNKNOWN;

	ID3D11Texture2D* m_pCanvasTex = nullptr;
	uint32_t m_canvasW = 0, m_canvasH = 0;
	DXGI_FORMAT m_canvasFormat = DXGI_FORMAT_UNKNOWN;

	CRITICAL_SECTION m_cs;

	static MfGpuContext& Instance() {
		static MfGpuContext ctx;
		return ctx;
	}

	bool EnsureInit() {
		if (m_initialized) return true;
		if (m_supportChecked && !m_hasHwDecoder) return false;

		EnterCriticalSection(&m_cs);
		if (m_initialized) {
			LeaveCriticalSection(&m_cs);
			return true;
		}

		MFStartup(MF_VERSION);

		D3D_FEATURE_LEVEL fl;
		D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
		UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
		HRESULT hr = D3D11CreateDevice(
			nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
			createFlags,
			levels, 1, D3D11_SDK_VERSION,
			&m_pDevice, &fl, &m_pContext
		);
		if (FAILED(hr) || !m_pDevice) {
			m_supportChecked = true;
			m_hasHwDecoder = false;
			LeaveCriticalSection(&m_cs);
			return false;
		}

		ID3D11Multithread* pMultithread = nullptr;
		if (SUCCEEDED(m_pDevice->QueryInterface(__uuidof(ID3D11Multithread), (void**)&pMultithread))) {
			pMultithread->SetMultithreadProtected(TRUE);
			pMultithread->Release();
		}

		hr = MFCreateDXGIDeviceManager(&m_resetToken, &m_pDXGIManager);
		if (FAILED(hr) || !m_pDXGIManager) {
			m_supportChecked = true;
			m_hasHwDecoder = false;
			LeaveCriticalSection(&m_cs);
			return false;
		}

		hr = m_pDXGIManager->ResetDevice(m_pDevice, m_resetToken);
		if (FAILED(hr)) {
			m_supportChecked = true;
			m_hasHwDecoder = false;
			LeaveCriticalSection(&m_cs);
			return false;
		}

		MFT_REGISTER_TYPE_INFO inputType = { MFMediaType_Video, MFVideoFormat_HEVC };
		IMFActivate** ppActivate = NULL;
		UINT32 count = 0;
		hr = MFTEnumEx(
			MFT_CATEGORY_VIDEO_DECODER,
			MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
			&inputType, NULL, &ppActivate, &count
		);

		if (count == 0 || FAILED(hr)) {
			m_supportChecked = true;
			m_hasHwDecoder = false;
			LeaveCriticalSection(&m_cs);
			return false;
		}

		m_pDecoderActivate = ppActivate[0];
		m_pDecoderActivate->AddRef();
		for (UINT32 i = 0; i < count; i++) ppActivate[i]->Release();
		CoTaskMemFree(ppActivate);

		m_numDecoders = 0;
		for (int i = 0; i < MAX_DECODERS; i++) {
			IMFTransform* pDec = nullptr;
			hr = m_pDecoderActivate->ActivateObject(IID_IMFTransform, (void**)&pDec);
			if (SUCCEEDED(hr) && pDec) {
				pDec->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)m_pDXGIManager);
				IMFAttributes* pAttrs = nullptr;
				if (SUCCEEDED(pDec->GetAttributes(&pAttrs)) && pAttrs) {
					pAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);
					pAttrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
					pAttrs->SetUINT32(CODECAPI_AVDecVideoThumbnailGenerationMode, TRUE);
					pAttrs->Release();
				}
				m_pDecoders[m_numDecoders++] = pDec;
			}
		}

		if (m_numDecoders == 0) {
			m_supportChecked = true;
			m_hasHwDecoder = false;
			LeaveCriticalSection(&m_cs);
			return false;
		}

		m_hasHwDecoder = true;
		m_supportChecked = true;
		m_initialized = true;
		LeaveCriticalSection(&m_cs);
		return true;
	}

	ID3D11Texture2D* GetStagingTex(uint32_t w, uint32_t h, DXGI_FORMAT format = DXGI_FORMAT_NV12) {
		if (m_pStagingTex && m_stagingW == w && m_stagingH == h && m_stagingFormat == format) return m_pStagingTex;
		if (m_pStagingTex) m_pStagingTex->Release();
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = w;
		desc.Height = h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		m_pDevice->CreateTexture2D(&desc, nullptr, &m_pStagingTex);
		m_stagingW = w;
		m_stagingH = h;
		m_stagingFormat = format;
		return m_pStagingTex;
	}

	ID3D11Texture2D* GetCanvasTex(uint32_t w, uint32_t h, DXGI_FORMAT format = DXGI_FORMAT_NV12) {
		if (m_pCanvasTex && m_canvasW == w && m_canvasH == h && m_canvasFormat == format) return m_pCanvasTex;
		if (m_pCanvasTex) m_pCanvasTex->Release();
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = w;
		desc.Height = h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		m_pDevice->CreateTexture2D(&desc, nullptr, &m_pCanvasTex);
		m_canvasW = w;
		m_canvasH = h;
		m_canvasFormat = format;
		return m_pCanvasTex;
	}

	void EnsureInputBuffer(int decIdx, DWORD requiredSize) {
		if (m_pInBuf[decIdx] && m_inBufCapacity[decIdx] >= requiredSize) return;
		if (m_pInSample[decIdx]) { m_pInSample[decIdx]->Release(); m_pInSample[decIdx] = nullptr; }
		if (m_pInBuf[decIdx]) { m_pInBuf[decIdx]->Release(); m_pInBuf[decIdx] = nullptr; }

		DWORD allocSize = max(requiredSize, (DWORD)(32 * 1024 * 1024));
		MFCreateMemoryBuffer(allocSize, &m_pInBuf[decIdx]);
		MFCreateSample(&m_pInSample[decIdx]);
		m_pInSample[decIdx]->AddBuffer(m_pInBuf[decIdx]);
		m_inBufCapacity[decIdx] = allocSize;
	}

	bool DecodeTileToGPU(
		int decIdx,
		const std::vector<uint8_t>& hvcC, const uint8_t* sliceData, size_t sliceSize,
		uint32_t width, uint32_t height, ID3D11Texture2D*& outTex, UINT& outSubresource)
	{
		outTex = nullptr;
		outSubresource = 0;

		if (!EnsureInit()) return false;
		if (decIdx < 0 || decIdx >= m_numDecoders) decIdx = 0;
		IMFTransform* pDecoder = m_pDecoders[decIdx];

		if (m_curW[decIdx] != width || m_curH[decIdx] != height) {
			IMFMediaType* pInType = nullptr;
			MFCreateMediaType(&pInType);
			pInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
			pInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
			MFSetAttributeSize(pInType, MF_MT_FRAME_SIZE, width, height);
			pDecoder->SetInputType(0, pInType, 0);
			pInType->Release();

			IMFMediaType* pOutType = nullptr;
			MFCreateMediaType(&pOutType);
			pOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
			pOutType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
			MFSetAttributeSize(pOutType, MF_MT_FRAME_SIZE, width, height);
			pDecoder->SetOutputType(0, pOutType, 0);
			pOutType->Release();

			pDecoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
			pDecoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
			m_curW[decIdx] = width;
			m_curH[decIdx] = height;
		}

		if (hvcC.size() < 23) return false;
		size_t annexBSize = hvcC.size() + sliceSize + 64;
		EnsureInputBuffer(decIdx, (DWORD)annexBSize);

		BYTE* pDst = nullptr;
		m_pInBuf[decIdx]->Lock(&pDst, NULL, NULL);

		// Single-pass direct Annex-B stream writing into media buffer
		size_t outPos = 0;
		const uint8_t startCode[4] = { 0, 0, 0, 1 };
		size_t p = 8 + 22;
		uint8_t numOfArrays = hvcC[p++];

		for (uint8_t a = 0; a < numOfArrays && p + 3 <= hvcC.size(); a++) {
			p++;
			uint16_t numNalus = (hvcC[p] << 8) | hvcC[p + 1];
			p += 2;
			for (uint16_t n = 0; n < numNalus && p + 2 <= hvcC.size(); n++) {
				uint16_t nalLen = (hvcC[p] << 8) | hvcC[p + 1];
				p += 2;
				if (p + nalLen > hvcC.size() || outPos + 4 + nalLen > m_inBufCapacity[decIdx]) {
					m_pInBuf[decIdx]->Unlock();
					return false;
				}
				memcpy(pDst + outPos, startCode, 4);
				outPos += 4;
				memcpy(pDst + outPos, &hvcC[p], nalLen);
				outPos += nalLen;
				p += nalLen;
			}
		}

		size_t sliceP = 0;
		while (sliceP + 4 <= sliceSize) {
			uint32_t nalLen = (sliceData[sliceP] << 24) | (sliceData[sliceP + 1] << 16) | (sliceData[sliceP + 2] << 8) | sliceData[sliceP + 3];
			sliceP += 4;
			if (sliceP + nalLen > sliceSize || outPos + 4 + nalLen > m_inBufCapacity[decIdx]) break;
			memcpy(pDst + outPos, startCode, 4);
			outPos += 4;
			memcpy(pDst + outPos, &sliceData[sliceP], nalLen);
			outPos += nalLen;
			sliceP += nalLen;
		}

		m_pInBuf[decIdx]->Unlock();
		m_pInBuf[decIdx]->SetCurrentLength((DWORD)outPos);
		m_pInSample[decIdx]->SetSampleTime(0);
		m_pInSample[decIdx]->SetSampleDuration(1);

		HRESULT hr = pDecoder->ProcessInput(0, m_pInSample[decIdx], 0);
		if (FAILED(hr)) return false;

		pDecoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);

		MFT_OUTPUT_STREAM_INFO osi = { 0 };
		pDecoder->GetOutputStreamInfo(0, &osi);
		MFT_OUTPUT_DATA_BUFFER outputBuffer = { 0 };
		DWORD status = 0;

		for (int iter = 0; iter < 10; iter++) {
			outputBuffer.pSample = nullptr;
			if (!(osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
				IMFSample* pOutSample = nullptr;
				MFCreateSample(&pOutSample);
				IMFMediaBuffer* pOutBuf = nullptr;
				MFCreateMemoryBuffer(osi.cbSize, &pOutBuf);
				pOutSample->AddBuffer(pOutBuf);
				pOutBuf->Release();
				outputBuffer.pSample = pOutSample;
			}

			hr = pDecoder->ProcessOutput(0, 1, &outputBuffer, &status);
			if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
				IMFMediaType* pAvailableType = nullptr;
				hr = pDecoder->GetOutputAvailableType(0, 0, &pAvailableType);
				if (SUCCEEDED(hr)) {
					pDecoder->SetOutputType(0, pAvailableType, 0);
					pAvailableType->Release();
				}
				pDecoder->GetOutputStreamInfo(0, &osi);
				continue;
			}

			if (SUCCEEDED(hr) && outputBuffer.pSample) {
				IMFMediaBuffer* pBuf = nullptr;
				outputBuffer.pSample->GetBufferByIndex(0, &pBuf);
				if (pBuf) {
					IMFDXGIBuffer* pDxgiBuf = nullptr;
					if (SUCCEEDED(pBuf->QueryInterface(__uuidof(IMFDXGIBuffer), (void**)&pDxgiBuf))) {
						pDxgiBuf->GetResource(__uuidof(ID3D11Texture2D), (void**)&outTex);
						pDxgiBuf->GetSubresourceIndex(&outSubresource);
						pDxgiBuf->Release();
					}
					pBuf->Release();
				}
				outputBuffer.pSample->Release();
				break;
			}
			if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
		}

		pDecoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
		return (outTex != nullptr);
	}

private:
	MfGpuContext() {
		InitializeCriticalSection(&m_cs);
	}
	~MfGpuContext() {
		for (int i = 0; i < MAX_DECODERS; i++) {
			if (m_pInSample[i]) m_pInSample[i]->Release();
			if (m_pInBuf[i]) m_pInBuf[i]->Release();
			if (m_pDecoders[i]) m_pDecoders[i]->Release();
		}
		if (m_pStagingTex) m_pStagingTex->Release();
		if (m_pCanvasTex) m_pCanvasTex->Release();
		if (m_pDecoderActivate) m_pDecoderActivate->Release();
		if (m_pDXGIManager) m_pDXGIManager->Release();
		if (m_pContext) m_pContext->Release();
		if (m_pDevice) m_pDevice->Release();
		if (m_initialized) MFShutdown();
		DeleteCriticalSection(&m_cs);
	}
};

// ISOBMFF Structures
struct IsoItem {
	uint32_t id = 0;
	std::string type;
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t offset = 0;
	uint64_t length = 0;
	std::vector<uint8_t> hvcC_data;
	std::vector<uint8_t> icc_profile;
	uint16_t matrix_coefficients = 1; // BT.709 default
	bool full_range = true;
	bool has_nclx = false;
	uint8_t rotation = 0; // 0 = 0 deg, 1 = 90 CCW, 2 = 180, 3 = 270 CCW
	int8_t mirror = -1;  // -1 = none, 0 = vertical, 1 = horizontal
	uint8_t bit_depth = 8; // from pixi; 8 = 8-bit, 10 = 10-bit Main10 etc.
};

struct IsoGrid {
	uint8_t rows = 0;
	uint8_t columns = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	std::vector<uint32_t> tileItemIds;
};

class IsoHeifDemuxer {
public:
	const uint8_t* m_data = nullptr;
	size_t m_size = 0;
	size_t m_idatOffset = 0;
	size_t m_idatLength = 0;
	std::map<uint32_t, IsoItem> m_items;
	std::map<uint32_t, IsoGrid> m_grids;
	std::vector<uint32_t> m_topLevelItemIds;
	uint32_t m_primaryItemId = 0;
	uint32_t m_exifItemId = 0;

	bool Parse(const uint8_t* data, size_t size) {
		m_data = data;
		m_size = size;
		m_idatOffset = 0;
		m_idatLength = 0;
		m_items.clear();
		m_grids.clear();
		m_topLevelItemIds.clear();
		m_primaryItemId = 0;
		m_exifItemId = 0;

		size_t offset = 0;
		while (offset + 8 <= size) {
			uint64_t boxSize = ReadU32(offset);
			std::string boxType((const char*)&m_data[offset + 4], 4);
			size_t headerSize = 8;
			if (boxSize == 1) {
				if (offset + 16 > size) break;
				boxSize = ReadU64(offset + 8);
				headerSize = 16;
			} else if (boxSize == 0) {
				boxSize = size - offset;
			}

			if (boxType == "meta") {
				ParseMeta(offset + headerSize + 4, boxSize - headerSize - 4);
			}
			offset += boxSize;
		}

		if (m_primaryItemId != 0) {
			m_topLevelItemIds.push_back(m_primaryItemId);
		}
		for (const auto& kv : m_items) {
			if (kv.second.type == "hvc1" || kv.second.type == "grid") {
				if (std::find(m_topLevelItemIds.begin(), m_topLevelItemIds.end(), kv.first) == m_topLevelItemIds.end()) {
					bool isTile = false;
					for (const auto& g : m_grids) {
						if (std::find(g.second.tileItemIds.begin(), g.second.tileItemIds.end(), kv.first) != g.second.tileItemIds.end()) {
							isTile = true;
							break;
						}
					}
					if (!isTile) {
						m_topLevelItemIds.push_back(kv.first);
					}
				}
			}
		}

		return !m_items.empty();
	}

private:
	uint32_t ReadU32(size_t off) const {
		if (off + 4 > m_size) return 0;
		return (m_data[off] << 24) | (m_data[off + 1] << 16) | (m_data[off + 2] << 8) | m_data[off + 3];
	}
	uint64_t ReadU64(size_t off) const {
		if (off + 8 > m_size) return 0;
		uint64_t v = 0;
		for (int i = 0; i < 8; i++) v = (v << 8) | m_data[off + i];
		return v;
	}
	uint16_t ReadU16(size_t off) const {
		if (off + 2 > m_size) return 0;
		return (m_data[off] << 8) | m_data[off + 1];
	}

	void ParseMeta(size_t start, size_t length) {
		size_t end = min(start + length, m_size);
		size_t off = start;

		while (off + 8 <= end) {
			uint64_t boxSize = ReadU32(off);
			std::string boxType((const char*)&m_data[off + 4], 4);
			size_t headerSize = 8;
			if (boxSize == 1) {
				boxSize = ReadU64(off + 8);
				headerSize = 16;
			} else if (boxSize == 0) {
				boxSize = end - off;
			}
			if (boxSize < headerSize || off + boxSize > end) break;

			if (boxType == "idat") {
				m_idatOffset = off + headerSize;
				m_idatLength = boxSize - headerSize;
			}
			off += boxSize;
		}

		off = start;
		while (off + 8 <= end) {
			uint64_t boxSize = ReadU32(off);
			std::string boxType((const char*)&m_data[off + 4], 4);
			size_t headerSize = 8;
			if (boxSize == 1) {
				boxSize = ReadU64(off + 8);
				headerSize = 16;
			} else if (boxSize == 0) {
				boxSize = end - off;
			}
			if (boxSize < headerSize || off + boxSize > end) break;

			if (boxType == "pitm") {
				uint8_t ver = m_data[off + 8];
				m_primaryItemId = (ver == 0) ? ReadU16(off + 12) : ReadU32(off + 12);
			} else if (boxType == "iinf") {
				ParseIinf(off + headerSize, boxSize - headerSize);
			} else if (boxType == "iloc") {
				ParseIloc(off + headerSize, boxSize - headerSize);
			} else if (boxType == "iprp") {
				ParseIprp(off + headerSize, boxSize - headerSize);
			} else if (boxType == "iref") {
				ParseIref(off + headerSize, boxSize - headerSize);
			}

			off += boxSize;
		}
	}

	void ParseIinf(size_t start, size_t length) {
		if (start + 4 > m_size) return;
		uint8_t ver = m_data[start];
		size_t p = start + 4;
		uint32_t count = (ver == 0) ? ReadU16(p) : ReadU32(p);
		p += (ver == 0) ? 2 : 4;

		for (uint32_t i = 0; i < count && p + 8 <= start + length && p + 8 <= m_size; i++) {
			uint32_t infeSize = ReadU32(p);
			if (infeSize < 8 || p + infeSize > m_size) break;
			std::string infeType((const char*)&m_data[p + 4], 4);
			if (infeType == "infe") {
				uint8_t iVer = m_data[p + 8];
				uint32_t itemId = 0;
				std::string itemType;
				if (iVer >= 2) {
					itemId = (iVer == 2) ? ReadU16(p + 12) : ReadU32(p + 12);
					size_t typeOff = (iVer == 2) ? (p + 16) : (p + 18);
					if (typeOff + 4 <= m_size) {
						itemType = std::string((const char*)&m_data[typeOff], 4);
						m_items[itemId].id = itemId;
						m_items[itemId].type = itemType;
						if (itemType == "Exif") {
							m_exifItemId = itemId;
						}
					}
				}
			}
			p += infeSize;
		}
	}

	void ParseIloc(size_t start, size_t length) {
		if (start + 6 > m_size) return;
		uint8_t ver = m_data[start];
		uint8_t offsetSize = (m_data[start + 4] >> 4) & 0x0F;
		uint8_t lengthSize = m_data[start + 4] & 0x0F;
		uint8_t baseOffsetSize = (m_data[start + 5] >> 4) & 0x0F;
		uint8_t indexSize = (ver >= 1) ? (m_data[start + 5] & 0x0F) : 0;

		size_t p = start + 6;
		uint32_t count = (ver < 2) ? ReadU16(p) : ReadU32(p);
		p += (ver < 2) ? 2 : 4;

		for (uint32_t i = 0; i < count && p < start + length && p < m_size; i++) {
			uint32_t itemId = (ver < 2) ? ReadU16(p) : ReadU32(p);
			p += (ver < 2) ? 2 : 4;
			uint16_t constructionMethod = 0;
			if (ver >= 1) {
				constructionMethod = ReadU16(p);
				p += 2;
			}
			p += 2; // data_reference_index
			uint64_t baseOffset = ReadVarInt(p, baseOffsetSize);
			p += baseOffsetSize;
			uint16_t extentCount = ReadU16(p);
			p += 2;
			for (uint16_t e = 0; e < extentCount; e++) {
				if (ver >= 1 && indexSize > 0) p += indexSize;
				uint64_t extentOffset = ReadVarInt(p, offsetSize);
				p += offsetSize;
				uint64_t extentLength = ReadVarInt(p, lengthSize);
				p += lengthSize;

				uint64_t finalOffset = baseOffset + extentOffset;
				if (constructionMethod == 1) {
					finalOffset += m_idatOffset;
				}
				m_items[itemId].offset = finalOffset;
				m_items[itemId].length = extentLength;
			}
		}
	}

	void ParseIprp(size_t start, size_t length) {
		size_t end = min(start + length, m_size);
		size_t off = start;
		std::vector<std::pair<std::string, std::vector<uint8_t>>> properties;
		properties.push_back({ "", {} });

		while (off + 8 <= end) {
			uint64_t boxSize = ReadU32(off);
			if (boxSize < 8 || off + boxSize > end) break;
			std::string boxType((const char*)&m_data[off + 4], 4);
			if (boxType == "ipco") {
				size_t ipcoOff = off + 8;
				while (ipcoOff + 8 <= off + boxSize) {
					uint32_t propSize = ReadU32(ipcoOff);
					if (propSize < 8 || ipcoOff + propSize > off + boxSize) break;
					std::string propType((const char*)&m_data[ipcoOff + 4], 4);
					std::vector<uint8_t> propData(&m_data[ipcoOff], &m_data[ipcoOff + propSize]);
					properties.push_back({ propType, propData });
					ipcoOff += propSize;
				}
			} else if (boxType == "ipma") {
				uint8_t ver = m_data[off + 8];
				size_t p = off + 12;
				uint32_t entryCount = ReadU32(p);
				p += 4;
				for (uint32_t e = 0; e < entryCount && p < off + boxSize; e++) {
					uint32_t itemId = (ver < 1) ? ReadU16(p) : ReadU32(p);
					p += (ver < 1) ? 2 : 4;
					uint8_t assocCount = m_data[p++];
					for (uint8_t a = 0; a < assocCount; a++) {
						uint16_t propIndex = (m_data[off + 9] & 0x01) ? ((m_data[p] & 0x7F) << 8 | m_data[p + 1]) : (m_data[p] & 0x7F);
						p += (m_data[off + 9] & 0x01) ? 2 : 1;
						if (propIndex > 0 && propIndex < properties.size()) {
							const auto& prop = properties[propIndex];
							if (prop.first == "ispe" && prop.second.size() >= 20) {
								m_items[itemId].width = (prop.second[12] << 24) | (prop.second[13] << 16) | (prop.second[14] << 8) | prop.second[15];
								m_items[itemId].height = (prop.second[16] << 24) | (prop.second[17] << 16) | (prop.second[18] << 8) | prop.second[19];
							} else if (prop.first == "hvcC") {
								m_items[itemId].hvcC_data = prop.second;
								// Infer 10-bit from hvcC general_profile (Main10 = 0x04) when pixi is absent or not yet parsed
								if (prop.second.size() > 9 && m_items[itemId].bit_depth == 8) {
									uint8_t general_profile = prop.second[9];
									if (general_profile == 0x04) {
										m_items[itemId].bit_depth = 10;
									}
								}
							} else if (prop.first == "colr" && prop.second.size() >= 12) {
								std::string colrType((const char*)&prop.second[8], 4);
								if (colrType == "nclx" && prop.second.size() >= 19) {
									m_items[itemId].has_nclx = true;
									m_items[itemId].matrix_coefficients = (prop.second[16] << 8) | prop.second[17];
									m_items[itemId].full_range = (prop.second[18] & 0x80) != 0;
								} else if ((colrType == "prof" || colrType == "rICC") && prop.second.size() > 12) {
									m_items[itemId].icc_profile.assign(prop.second.begin() + 12, prop.second.end());
								}
							} else if (prop.first == "irot" && prop.second.size() >= 9) {
								m_items[itemId].rotation = prop.second[8] & 0x03;
							} else if (prop.first == "imir" && prop.second.size() >= 9) {
								m_items[itemId].mirror = prop.second[8] & 0x01;
							} else if (prop.first == "pixi" && prop.second.size() >= 13) {
								uint8_t numChannels = prop.second[12];
								uint8_t maxDepth = 0;
								for (uint8_t c = 0; c < numChannels && (size_t)(13 + c) < prop.second.size(); c++) {
									uint8_t d = prop.second[13 + c];
									if (d > maxDepth) maxDepth = d;
								}
								if (maxDepth != 0) {
									m_items[itemId].bit_depth = maxDepth;
								}
							}
						}
					}
				}
			}
			off += boxSize;
		}
	}

	void ParseIref(size_t start, size_t length) {
		if (start + 4 > m_size) return;
		uint8_t ver = m_data[start];
		size_t off = start + 4;
		size_t end = min(start + length, m_size);
		while (off + 8 <= end) {
			uint32_t boxSize = ReadU32(off);
			if (boxSize < 8 || off + boxSize > end) break;
			std::string refType((const char*)&m_data[off + 4], 4);
			if (refType == "dimg") {
				size_t p = off + 8;
				uint32_t fromId = (ver == 0) ? ReadU16(p) : ReadU32(p);
				p += (ver == 0) ? 2 : 4;
				uint16_t refCount = ReadU16(p);
				p += 2;
				for (uint16_t r = 0; r < refCount && p < off + boxSize; r++) {
					uint32_t toId = (ver == 0) ? ReadU16(p) : ReadU32(p);
					p += (ver == 0) ? 2 : 4;
					m_grids[fromId].tileItemIds.push_back(toId);
				}
			}
			off += boxSize;
		}
	}

	uint64_t ReadVarInt(size_t off, uint8_t size) const {
		if (size == 0 || off + size > m_size) return 0;
		if (size == 1) return m_data[off];
		if (size == 2) return ReadU16(off);
		if (size == 4) return ReadU32(off);
		if (size == 8) return ReadU64(off);
		return 0;
	}
};

} // anonymous namespace

bool GpuHeifDecoder::IsHardwareSupported()
{
	return MfGpuContext::Instance().EnsureInit();
}

bool GpuHeifDecoder::DecodeHeif(
	const void* buffer,
	size_t sizeBytes,
	int frameIndex,
	int& width,
	int& height,
	int& bpp,
	int& frameCount,
	void*& pPixelData,
	void*& exif_chunk,
	bool& hasAlpha,
	bool& outOfMemory)
{
	outOfMemory = false;
	hasAlpha = false;
	pPixelData = nullptr;
	exif_chunk = nullptr;
	width = height = 0;
	bpp = 4;
	frameCount = 1;

	if (!buffer || sizeBytes < 32) return false;
	const uint8_t* data = (const uint8_t*)buffer;
	IsoHeifDemuxer demuxer;
	if (!demuxer.Parse(data, sizeBytes)) {
		return false;
	}

	frameCount = (int)demuxer.m_topLevelItemIds.size();
	if (frameCount <= 0) return false;
	if (frameIndex < 0 || frameIndex >= frameCount) {
		frameIndex = 0;
	}

	uint32_t targetItemId = demuxer.m_topLevelItemIds[frameIndex];
	const auto& targetItem = demuxer.m_items[targetItemId];

	uint32_t finalW = targetItem.width;
	uint32_t finalH = targetItem.height;

	double t0 = Helpers::GetExactTickCount();
	MfGpuContext& gpuCtx = MfGpuContext::Instance();
	if (!gpuCtx.EnsureInit()) return false;
	double t_init = Helpers::GetExactTickCount() - t0;

	// Check if this is a grid derived item
	bool isGrid = (demuxer.m_grids.count(targetItemId) != 0);
	if (isGrid) {
		const auto& grid = demuxer.m_grids[targetItemId];
		if (grid.tileItemIds.empty()) {
			return false;
		}

		if (targetItem.offset + 8 > sizeBytes) {
			return false;
		}
		uint8_t flags = data[targetItem.offset + 1];
		uint8_t rows = data[targetItem.offset + 2] + 1;
		uint8_t cols = data[targetItem.offset + 3] + 1;

		if (flags & 1) { // 32-bit width/height
			if (targetItem.offset + 12 > sizeBytes) {
				return false;
			}
			finalW = (data[targetItem.offset + 4] << 24) | (data[targetItem.offset + 5] << 16) | (data[targetItem.offset + 6] << 8) | data[targetItem.offset + 7];
			finalH = (data[targetItem.offset + 8] << 24) | (data[targetItem.offset + 9] << 16) | (data[targetItem.offset + 10] << 8) | data[targetItem.offset + 11];
		} else { // 16-bit width/height
			finalW = (data[targetItem.offset + 4] << 8) | data[targetItem.offset + 5];
			finalH = (data[targetItem.offset + 6] << 8) | data[targetItem.offset + 7];
		}

		if (finalW > MAX_IMAGE_DIMENSION || finalH > MAX_IMAGE_DIMENSION || (double)finalW * finalH > MAX_IMAGE_PIXELS || finalW < 1 || finalH < 1) {
			outOfMemory = true;
			return false;
		}

		// Decode each tile directly on GPU and blit into full GPU canvas
		bool decodeOk = true;
		DXGI_FORMAT decodedFormat = DXGI_FORMAT_NV12;
		ID3D11Texture2D* pCanvas = nullptr;

		for (size_t i = 0; i < grid.tileItemIds.size(); i++) {
			uint32_t tileId = grid.tileItemIds[i];
			const auto& tileItem = demuxer.m_items[tileId];
			if (tileItem.offset + tileItem.length > sizeBytes) {
				decodeOk = false;
				break;
			}

			ID3D11Texture2D* pTileTex = nullptr;
			UINT subIndex = 0;
			if (gpuCtx.DecodeTileToGPU(0, tileItem.hvcC_data, &data[tileItem.offset], (size_t)tileItem.length, tileItem.width, tileItem.height, pTileTex, subIndex)) {
				if (!pCanvas) {
					D3D11_TEXTURE2D_DESC td;
					pTileTex->GetDesc(&td);
					decodedFormat = td.Format;
					pCanvas = gpuCtx.GetCanvasTex(finalW, finalH, decodedFormat);
					if (!pCanvas) {
						pTileTex->Release();
						decodeOk = false;
						break;
					}
				}

				uint32_t tileX = (uint32_t)(i % cols) * tileItem.width;
				uint32_t tileY = (uint32_t)(i / cols) * tileItem.height;

				D3D11_BOX srcBox = { 0, 0, 0, min(tileItem.width, finalW - tileX), min(tileItem.height, finalH - tileY), 1 };
				gpuCtx.m_pContext->CopySubresourceRegion(pCanvas, 0, tileX, tileY, 0, pTileTex, subIndex, &srcBox);
				pTileTex->Release();
			} else {
				decodeOk = false;
				break;
			}
		}

		if (!decodeOk || !pCanvas) {
			return false;
		}

		// Single GPU-to-CPU staging copy for the complete assembled canvas
		ID3D11Texture2D* pStaging = gpuCtx.GetStagingTex(finalW, finalH, decodedFormat);
		if (!pStaging) {
			return false;
		}

		gpuCtx.m_pContext->CopyResource(pStaging, pCanvas);
		D3D11_MAPPED_SUBRESOURCE mapRes;
		HRESULT hrMap = gpuCtx.m_pContext->Map(pStaging, 0, D3D11_MAP_READ, 0, &mapRes);
		if (FAILED(hrMap) || !mapRes.pData) {
			return false;
		}

		uint32_t outW = (targetItem.rotation == 1 || targetItem.rotation == 3) ? finalH : finalW;
		uint32_t outH = (targetItem.rotation == 1 || targetItem.rotation == 3) ? finalW : finalH;

		uint8_t* pDstPixels = new(std::nothrow) uint8_t[(size_t)outW * outH * 4];
		if (!pDstPixels) {
			gpuCtx.m_pContext->Unmap(pStaging, 0);
			outOfMemory = true;
			return false;
		}

		YCbCrCoefficients coeffs = GetNclxCoefficients(targetItem.matrix_coefficients);
		ConvertDecodedTextureToBGRA(decodedFormat, (const uint8_t*)mapRes.pData, finalW, finalH, mapRes.RowPitch, pDstPixels, coeffs, targetItem.full_range, targetItem.rotation, targetItem.mirror);
		gpuCtx.m_pContext->Unmap(pStaging, 0);

		finalW = outW;
		finalH = outH;

		// Apply ICC profile if attached
		if (!targetItem.icc_profile.empty()) {
			void* transform = ICCProfileTransform::CreateTransform(targetItem.icc_profile.data(), (unsigned int)targetItem.icc_profile.size(), ICCProfileTransform::FORMAT_BGRA);
			if (transform) {
				ICCProfileTransform::DoTransform(transform, pDstPixels, pDstPixels, finalW, finalH);
				ICCProfileTransform::DeleteTransform(transform);
			}
		}

		width = (int)finalW;
		height = (int)finalH;
		pPixelData = pDstPixels;
	}
	else {
		// Single HEVC item
		if (targetItem.type != "hvc1" && targetItem.type != "hev1") {
			return false;
		}

		if (finalW > MAX_IMAGE_DIMENSION || finalH > MAX_IMAGE_DIMENSION || (double)finalW * finalH > MAX_IMAGE_PIXELS || finalW < 1 || finalH < 1) {
			outOfMemory = true;
			return false;
		}

		if (targetItem.offset + targetItem.length > sizeBytes) {
			return false;
		}

		ID3D11Texture2D* pTex = nullptr;
		UINT subIndex = 0;
		if (!gpuCtx.DecodeTileToGPU(0, targetItem.hvcC_data, &data[targetItem.offset], (size_t)targetItem.length, finalW, finalH, pTex, subIndex)) {
			return false;
		}

		D3D11_TEXTURE2D_DESC td;
		pTex->GetDesc(&td);
		DXGI_FORMAT decodedFormat = td.Format;

		uint32_t outW = (targetItem.rotation == 1 || targetItem.rotation == 3) ? finalH : finalW;
		uint32_t outH = (targetItem.rotation == 1 || targetItem.rotation == 3) ? finalW : finalH;

		uint8_t* pDstPixels = new(std::nothrow) uint8_t[(size_t)outW * outH * 4];
		if (!pDstPixels) {
			pTex->Release();
			outOfMemory = true;
			return false;
		}

		ID3D11Texture2D* pStaging = gpuCtx.GetStagingTex(finalW, finalH, decodedFormat);
		if (!pStaging) {
			delete[] pDstPixels;
			pTex->Release();
			return false;
		}

		gpuCtx.m_pContext->CopySubresourceRegion(pStaging, 0, 0, 0, 0, pTex, subIndex, nullptr);
		pTex->Release();

		D3D11_MAPPED_SUBRESOURCE mapRes;
		HRESULT hrMap = gpuCtx.m_pContext->Map(pStaging, 0, D3D11_MAP_READ, 0, &mapRes);
		if (FAILED(hrMap) || !mapRes.pData) {
			delete[] pDstPixels;
			return false;
		}

		YCbCrCoefficients coeffs = GetNclxCoefficients(targetItem.matrix_coefficients);
		ConvertDecodedTextureToBGRA(decodedFormat, (const uint8_t*)mapRes.pData, finalW, finalH, mapRes.RowPitch, pDstPixels, coeffs, targetItem.full_range, targetItem.rotation, targetItem.mirror);
		gpuCtx.m_pContext->Unmap(pStaging, 0);

		finalW = outW;
		finalH = outH;

		// Apply ICC profile if attached
		if (!targetItem.icc_profile.empty()) {
			void* transform = ICCProfileTransform::CreateTransform(targetItem.icc_profile.data(), (unsigned int)targetItem.icc_profile.size(), ICCProfileTransform::FORMAT_BGRA);
			if (transform) {
				ICCProfileTransform::DoTransform(transform, pDstPixels, pDstPixels, finalW, finalH);
				ICCProfileTransform::DeleteTransform(transform);
			}
		}

		width = (int)finalW;
		height = (int)finalH;
		pPixelData = pDstPixels;
	}

	// Extract Exif metadata if present
	if (demuxer.m_exifItemId != 0) {
		const auto& exifItem = demuxer.m_items[demuxer.m_exifItemId];
		if (exifItem.offset + exifItem.length <= sizeBytes && exifItem.length > 4) {
			// In ISOBMFF, Exif item starts with a 4-byte offset indicating start of TIFF header
			uint32_t exifOffset = (data[exifItem.offset] << 24) | (data[exifItem.offset + 1] << 16) | (data[exifItem.offset + 2] << 8) | data[exifItem.offset + 3];
			size_t actualExifStart = exifItem.offset + 4 + exifOffset;
			if (actualExifStart < exifItem.offset + exifItem.length) {
				size_t exifPayloadSize = (exifItem.offset + exifItem.length) - actualExifStart;
				if (exifPayloadSize > 4 && exifPayloadSize < 65530) {
					// Format as JPEG Exif chunk: FF E1 [2-byte size] [payload]
					size_t chunkSize = exifPayloadSize + 4;
					exif_chunk = malloc(chunkSize);
					if (exif_chunk) {
						uint8_t* pExif = (uint8_t*)exif_chunk;
						pExif[0] = 0xFF;
						pExif[1] = 0xE1;
						pExif[2] = (uint8_t)((chunkSize - 2) >> 8);
						pExif[3] = (uint8_t)((chunkSize - 2) & 0xFF);
						memcpy(pExif + 4, &data[actualExifStart], exifPayloadSize);
					}
				}
			}
		}
	}

	return true;
}
