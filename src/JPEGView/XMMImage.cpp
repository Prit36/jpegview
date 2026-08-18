#include "StdAfx.h"
#include "XMMImage.h"
#include "Helpers.h"
#include <emmintrin.h>
#include <tmmintrin.h>

CXMMImage::CXMMImage(int nWidth, int nHeight, int padding) {
	Init(nWidth, nHeight, false, padding);
}

CXMMImage::CXMMImage(int nWidth, int nHeight, bool bPadHeight, int padding) {
	Init(nWidth, nHeight, bPadHeight, padding);
}

#include <immintrin.h>

CXMMImage::CXMMImage(int nWidth, int nHeight, int nFirstX, int nLastX, int nFirstY, int nLastY, 
	const void* pDIB, int nChannels, int padding) {
	int nSectionWidth = nLastX - nFirstX + 1;
	int nSectionHeight = nLastY - nFirstY + 1;
	Init(nSectionWidth, nSectionHeight, false, padding);

	if (m_pMemory != NULL) {
		int nSrcLineWidthPadded = Helpers::DoPadding(nWidth * nChannels, 4);

		if (nChannels == 4) {
			const __m256i mask_b = _mm256_setr_epi8(0, -1, 4, -1, 8, -1, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1,
			                                         0, -1, 4, -1, 8, -1, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1);
			const __m256i mask_g = _mm256_setr_epi8(1, -1, 5, -1, 9, -1, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1,
			                                         1, -1, 5, -1, 9, -1, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1);
			const __m256i mask_r = _mm256_setr_epi8(2, -1, 6, -1, 10, -1, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1,
			                                         2, -1, 6, -1, 10, -1, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1);

			for (int j = 0; j < nSectionHeight; j++) {
				const uint8* pSrc = (const uint8*)pDIB + ((size_t)nFirstY + j) * (size_t)nSrcLineWidthPadded + (size_t)nFirstX * 4;
				uint16* pDst = (unsigned short*)m_pMemory + (size_t)j * 3 * (size_t)m_nPaddedWidth;
				const uint32* pSrc32 = (const uint32*)pSrc;
				uint16* pDstB = pDst;
				uint16* pDstG = pDst + m_nPaddedWidth;
				uint16* pDstR = pDst + 2 * m_nPaddedWidth;

				int i = 0;
				for (; i + 16 <= nSectionWidth; i += 16) {
					__m256i px0 = _mm256_loadu_si256((const __m256i*)(pSrc32 + i));
					__m256i px1 = _mm256_loadu_si256((const __m256i*)(pSrc32 + i + 8));

					__m256i sb0 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px0, mask_b), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i sb1 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px1, mask_b), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i b16 = _mm256_slli_epi16(_mm256_permute2x128_si256(sb0, sb1, 0x20), 6);
					_mm256_storeu_si256((__m256i*)(pDstB + i), b16);

					__m256i sg0 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px0, mask_g), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i sg1 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px1, mask_g), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i g16 = _mm256_slli_epi16(_mm256_permute2x128_si256(sg0, sg1, 0x20), 6);
					_mm256_storeu_si256((__m256i*)(pDstG + i), g16);

					__m256i sr0 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px0, mask_r), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i sr1 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px1, mask_r), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i r16 = _mm256_slli_epi16(_mm256_permute2x128_si256(sr0, sr1, 0x20), 6);
					_mm256_storeu_si256((__m256i*)(pDstR + i), r16);
				}
				for (; i < nSectionWidth; i++) {
					uint32 sourcePixel = pSrc32[i];
					pDstB[i] = (uint16)((sourcePixel & 0xFF) << 6);
					pDstG[i] = (uint16)(((sourcePixel >> 8) & 0xFF) << 6);
					pDstR[i] = (uint16)(((sourcePixel >> 16) & 0xFF) << 6);
				}
			}
		} else {
			for (int j = 0; j < nSectionHeight; j++) {
				const uint8* pSrc = (const uint8*)pDIB + ((size_t)nFirstY + j) * (size_t)nSrcLineWidthPadded + (size_t)nFirstX * nChannels;
				uint16* pDst = (unsigned short*)m_pMemory + (size_t)j * 3 * (size_t)m_nPaddedWidth;
				uint16* pDstB = pDst;
				uint16* pDstG = pDst + m_nPaddedWidth;
				uint16* pDstR = pDst + 2 * m_nPaddedWidth;

				int i = 0;
				for (; i + 4 <= nSectionWidth; i += 4) {
					int s0 = i * 3;
					pDstB[i] = ((uint16)pSrc[s0]) << 6;
					pDstG[i] = ((uint16)pSrc[s0 + 1]) << 6;
					pDstR[i] = ((uint16)pSrc[s0 + 2]) << 6;
					pDstB[i + 1] = ((uint16)pSrc[s0 + 3]) << 6;
					pDstG[i + 1] = ((uint16)pSrc[s0 + 4]) << 6;
					pDstR[i + 1] = ((uint16)pSrc[s0 + 5]) << 6;
					pDstB[i + 2] = ((uint16)pSrc[s0 + 6]) << 6;
					pDstG[i + 2] = ((uint16)pSrc[s0 + 7]) << 6;
					pDstR[i + 2] = ((uint16)pSrc[s0 + 8]) << 6;
					pDstB[i + 3] = ((uint16)pSrc[s0 + 9]) << 6;
					pDstG[i + 3] = ((uint16)pSrc[s0 + 10]) << 6;
					pDstR[i + 3] = ((uint16)pSrc[s0 + 11]) << 6;
				}
				for (; i < nSectionWidth; i++) {
					int s = i * 3;
					pDstB[i] = ((uint16)pSrc[s]) << 6;
					pDstG[i] = ((uint16)pSrc[s + 1]) << 6;
					pDstR[i] = ((uint16)pSrc[s + 2]) << 6;
				}
			}
		}
	}
}

CXMMImage::~CXMMImage(void) {
	if (m_pMemory != NULL) {
		_aligned_free(m_pMemory);
		m_pMemory = NULL;
	}
}

void* CXMMImage::ConvertToDIBRGBA() const {
	if (m_pMemory == NULL) {
		return NULL;
	}
	uint8* pDIB = new uint8[m_nWidth * 4 * m_nHeight];
	
	const uint16* pSrc = (const uint16*)m_pMemory;
	uint8* pDst = pDIB;
	const __m128i a8 = _mm_set1_epi8(-1);

	for (int j = 0; j < m_nHeight; j++) {
		const uint16* pSrcB = pSrc;
		const uint16* pSrcG = pSrc + m_nPaddedWidth;
		const uint16* pSrcR = pSrc + 2 * m_nPaddedWidth;
		uint8* pDstRow = pDst;

		int i = 0;
		for (; i + 8 <= m_nWidth; i += 8) {
			__m128i b16 = _mm_srli_epi16(_mm_loadu_si128((const __m128i*)(pSrcB + i)), 6);
			__m128i g16 = _mm_srli_epi16(_mm_loadu_si128((const __m128i*)(pSrcG + i)), 6);
			__m128i r16 = _mm_srli_epi16(_mm_loadu_si128((const __m128i*)(pSrcR + i)), 6);

			__m128i b8 = _mm_packus_epi16(b16, _mm_setzero_si128());
			__m128i g8 = _mm_packus_epi16(g16, _mm_setzero_si128());
			__m128i r8 = _mm_packus_epi16(r16, _mm_setzero_si128());

			__m128i bg = _mm_unpacklo_epi8(b8, g8);
			__m128i ra = _mm_unpacklo_epi8(r8, a8);
			__m128i bgra_lo = _mm_unpacklo_epi16(bg, ra);
			__m128i bgra_hi = _mm_unpackhi_epi16(bg, ra);

			_mm_storeu_si128((__m128i*)(pDstRow + i * 4), bgra_lo);
			_mm_storeu_si128((__m128i*)(pDstRow + i * 4 + 16), bgra_hi);
		}

		for (; i < m_nWidth; i++) {
			pDstRow[i * 4 + 0] = (uint8)(pSrcB[i] >> 6);
			pDstRow[i * 4 + 1] = (uint8)(pSrcG[i] >> 6);
			pDstRow[i * 4 + 2] = (uint8)(pSrcR[i] >> 6);
			pDstRow[i * 4 + 3] = 0xFF;
		}

		pSrc += 3 * m_nPaddedWidth;
		pDst += m_nWidth * 4;
	}

	return pDIB;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////

void CXMMImage::Init(int nWidth, int nHeight, bool bPadHeight, int padding) {
	// pad scanlines
	m_nPaddedWidth = Helpers::DoPadding(nWidth, padding);
	if (bPadHeight) {
		m_nPaddedHeight = Helpers::DoPadding(nHeight, padding);
	} else {
		m_nPaddedHeight = nHeight;
	}
	m_nWidth = nWidth;
	m_nHeight = nHeight;
	int nMemSize = GetMemSize();
	// Allocate 64-byte aligned memory using high-performance CRT heap
	m_pMemory = _aligned_malloc(nMemSize, 64);
}
