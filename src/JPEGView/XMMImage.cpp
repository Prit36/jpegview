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

CXMMImage::CXMMImage(int nWidth, int nHeight, int nFirstX, int nLastX, int nFirstY, int nLastY, 
	const void* pDIB, int nChannels, int padding) {
	int nSectionWidth = nLastX - nFirstX + 1;
	int nSectionHeight = nLastY - nFirstY + 1;
	Init(nSectionWidth, nSectionHeight, false, padding);

	if (m_pMemory != NULL) {
		int nSrcLineWidthPadded = Helpers::DoPadding(nWidth * nChannels, 4);
		const __m128i mask_b = _mm_setr_epi8(0, -1, 4, -1, 8, -1, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1);
		const __m128i mask_g = _mm_setr_epi8(1, -1, 5, -1, 9, -1, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1);
		const __m128i mask_r = _mm_setr_epi8(2, -1, 6, -1, 10, -1, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1);

		for (int j = 0; j < nSectionHeight; j++) {
			const uint8* pSrc = (const uint8*)pDIB + ((size_t)nFirstY + j) * (size_t)nSrcLineWidthPadded + (size_t)nFirstX * nChannels;
			uint16* pDst = (unsigned short*)m_pMemory + (size_t)j * 3 * (size_t)m_nPaddedWidth;

			if (nChannels == 4) {
				const uint32* pSrc32 = (const uint32*)pSrc;
				uint16* pDstB = pDst;
				uint16* pDstG = pDst + m_nPaddedWidth;
				uint16* pDstR = pDst + 2 * m_nPaddedWidth;

				int i = 0;
				for (; i + 8 <= nSectionWidth; i += 8) {
					__m128i px0 = _mm_loadu_si128((const __m128i*)(pSrc32 + i));
					__m128i px1 = _mm_loadu_si128((const __m128i*)(pSrc32 + i + 4));

					__m128i b0 = _mm_slli_epi16(_mm_shuffle_epi8(px0, mask_b), 6);
					__m128i b1 = _mm_slli_epi16(_mm_shuffle_epi8(px1, mask_b), 6);
					_mm_storeu_si128((__m128i*)(pDstB + i), _mm_unpacklo_epi64(b0, b1));

					__m128i g0 = _mm_slli_epi16(_mm_shuffle_epi8(px0, mask_g), 6);
					__m128i g1 = _mm_slli_epi16(_mm_shuffle_epi8(px1, mask_g), 6);
					_mm_storeu_si128((__m128i*)(pDstG + i), _mm_unpacklo_epi64(g0, g1));

					__m128i r0 = _mm_slli_epi16(_mm_shuffle_epi8(px0, mask_r), 6);
					__m128i r1 = _mm_slli_epi16(_mm_shuffle_epi8(px1, mask_r), 6);
					_mm_storeu_si128((__m128i*)(pDstR + i), _mm_unpacklo_epi64(r0, r1));
				}
				for (; i < nSectionWidth; i++) {
					uint32 sourcePixel = pSrc32[i];
					pDstB[i] = (uint16)((sourcePixel & 0xFF) << 6);
					pDstG[i] = (uint16)(((sourcePixel >> 8) & 0xFF) << 6);
					pDstR[i] = (uint16)(((sourcePixel >> 16) & 0xFF) << 6);
				}
			} else {
				uint16* pDstB = pDst;
				uint16* pDstG = pDst + m_nPaddedWidth;
				uint16* pDstR = pDst + 2 * m_nPaddedWidth;
				int i = 0;
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
