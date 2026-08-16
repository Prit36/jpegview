#include "StdAfx.h"
#include "XMMImage.h"
#include "ResizeFilter.h"
#include "ApplyFilterAVX.h"

#ifdef _WIN64

CXMMImage* ApplyFilter_AVX(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const AVXFilterKernelBlock& filter,
	int nFilterOffset, const CXMMImage* pSourceImg) {

	int nStartXAligned = nStartX & ~15;
	int nEndXAligned = (nStartX + nWidth + 15) & ~15;
	CXMMImage* tempImage = new CXMMImage(nEndXAligned - nStartXAligned, nTargetHeight, 16);
	if (tempImage->AlignedPtr() == NULL) {
		delete tempImage;
		return NULL;
	}

	int nCurY = nStartY_FP;
	int nChannelLenBytes = pSourceImg->GetPaddedWidth() * sizeof(short);
	int nRowLenBytes = nChannelLenBytes * 3;
	int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 4;
	const uint8* pSourceStart = (const uint8*)pSourceImg->AlignedPtr() + nStartXAligned * sizeof(short);
	AVXFilterKernel** pKernelIndexStart = filter.Indices;

	const int nRowLenDestBytes = nNumberOfBlocksX * 3 * (int)sizeof(__m256i);
	const __m256i ymm0 = _mm256_set1_epi16(16383 - 42); // 1.0 in fixed point notation, minus rounding correction

	#pragma omp parallel for schedule(static)
	for (int y = 0; y < nTargetHeight; y++) {
		int nCurY = nStartY_FP + y * nIncrementY_FP;
		uint32 nCurYInt = (uint32)nCurY >> 16; // integer part of Y
		int filterIndex = y + nFilterOffset;
		AVXFilterKernel* pKernel = pKernelIndexStart[filterIndex];
		int filterLen = pKernel->FilterLen;
		int filterOffset = pKernel->FilterOffset;
		const __m256i* pFilterStart = (__m256i*)&(pKernel->Kernel);
		const __m256i* pSourceRow = (const __m256i*)(pSourceStart + ((int)nCurYInt - filterOffset) * nRowLenBytes);
		__m256i* pDestination = (__m256i*)((uint8*)tempImage->AlignedPtr() + (size_t)y * nRowLenDestBytes);

		int x = 0;
		for (; x + 4 <= nNumberOfBlocksX; x += 4) {
			const __m256i* pFilter = pFilterStart;
			const uint8* pSrc0 = (const uint8*)pSourceRow;
			const uint8* pSrc1 = pSrc0 + sizeof(__m256i);
			const uint8* pSrc2 = pSrc0 + 2 * sizeof(__m256i);
			const uint8* pSrc3 = pSrc0 + 3 * sizeof(__m256i);

			__m256i r0 = _mm256_setzero_si256(), r1 = _mm256_setzero_si256(), r2 = _mm256_setzero_si256(), r3 = _mm256_setzero_si256();
			__m256i g0 = _mm256_setzero_si256(), g1 = _mm256_setzero_si256(), g2 = _mm256_setzero_si256(), g3 = _mm256_setzero_si256();
			__m256i b0 = _mm256_setzero_si256(), b1 = _mm256_setzero_si256(), b2 = _mm256_setzero_si256(), b3 = _mm256_setzero_si256();

			for (int i = 0; i < filterLen; i++) {
				__m256i k = *pFilter++;

				// Red channel
				__m256i px0 = *(__m256i*)pSrc0;
				__m256i px1 = *(__m256i*)pSrc1;
				__m256i px2 = *(__m256i*)pSrc2;
				__m256i px3 = *(__m256i*)pSrc3;
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px2 = _mm256_add_epi16(px2, px2);
				px3 = _mm256_add_epi16(px3, px3);
				px0 = _mm256_mulhi_epi16(px0, k);
				px1 = _mm256_mulhi_epi16(px1, k);
				px2 = _mm256_mulhi_epi16(px2, k);
				px3 = _mm256_mulhi_epi16(px3, k);
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px2 = _mm256_add_epi16(px2, px2);
				px3 = _mm256_add_epi16(px3, px3);
				r0 = _mm256_adds_epi16(r0, px0);
				r1 = _mm256_adds_epi16(r1, px1);
				r2 = _mm256_adds_epi16(r2, px2);
				r3 = _mm256_adds_epi16(r3, px3);
				pSrc0 += nChannelLenBytes;
				pSrc1 += nChannelLenBytes;
				pSrc2 += nChannelLenBytes;
				pSrc3 += nChannelLenBytes;

				// Green channel
				px0 = *(__m256i*)pSrc0;
				px1 = *(__m256i*)pSrc1;
				px2 = *(__m256i*)pSrc2;
				px3 = *(__m256i*)pSrc3;
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px2 = _mm256_add_epi16(px2, px2);
				px3 = _mm256_add_epi16(px3, px3);
				px0 = _mm256_mulhi_epi16(px0, k);
				px1 = _mm256_mulhi_epi16(px1, k);
				px2 = _mm256_mulhi_epi16(px2, k);
				px3 = _mm256_mulhi_epi16(px3, k);
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px2 = _mm256_add_epi16(px2, px2);
				px3 = _mm256_add_epi16(px3, px3);
				g0 = _mm256_adds_epi16(g0, px0);
				g1 = _mm256_adds_epi16(g1, px1);
				g2 = _mm256_adds_epi16(g2, px2);
				g3 = _mm256_adds_epi16(g3, px3);
				pSrc0 += nChannelLenBytes;
				pSrc1 += nChannelLenBytes;
				pSrc2 += nChannelLenBytes;
				pSrc3 += nChannelLenBytes;

				// Blue channel
				px0 = *(__m256i*)pSrc0;
				px1 = *(__m256i*)pSrc1;
				px2 = *(__m256i*)pSrc2;
				px3 = *(__m256i*)pSrc3;
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px2 = _mm256_add_epi16(px2, px2);
				px3 = _mm256_add_epi16(px3, px3);
				px0 = _mm256_mulhi_epi16(px0, k);
				px1 = _mm256_mulhi_epi16(px1, k);
				px2 = _mm256_mulhi_epi16(px2, k);
				px3 = _mm256_mulhi_epi16(px3, k);
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px2 = _mm256_add_epi16(px2, px2);
				px3 = _mm256_add_epi16(px3, px3);
				b0 = _mm256_adds_epi16(b0, px0);
				b1 = _mm256_adds_epi16(b1, px1);
				b2 = _mm256_adds_epi16(b2, px2);
				b3 = _mm256_adds_epi16(b3, px3);
				pSrc0 += nChannelLenBytes;
				pSrc1 += nChannelLenBytes;
				pSrc2 += nChannelLenBytes;
				pSrc3 += nChannelLenBytes;
			}

			const __m256i ymmZero = _mm256_setzero_si256();
			r0 = _mm256_max_epi16(_mm256_min_epi16(r0, ymm0), ymmZero);
			r1 = _mm256_max_epi16(_mm256_min_epi16(r1, ymm0), ymmZero);
			r2 = _mm256_max_epi16(_mm256_min_epi16(r2, ymm0), ymmZero);
			r3 = _mm256_max_epi16(_mm256_min_epi16(r3, ymm0), ymmZero);
			g0 = _mm256_max_epi16(_mm256_min_epi16(g0, ymm0), ymmZero);
			g1 = _mm256_max_epi16(_mm256_min_epi16(g1, ymm0), ymmZero);
			g2 = _mm256_max_epi16(_mm256_min_epi16(g2, ymm0), ymmZero);
			g3 = _mm256_max_epi16(_mm256_min_epi16(g3, ymm0), ymmZero);
			b0 = _mm256_max_epi16(_mm256_min_epi16(b0, ymm0), ymmZero);
			b1 = _mm256_max_epi16(_mm256_min_epi16(b1, ymm0), ymmZero);
			b2 = _mm256_max_epi16(_mm256_min_epi16(b2, ymm0), ymmZero);
			b3 = _mm256_max_epi16(_mm256_min_epi16(b3, ymm0), ymmZero);

			pDestination[0] = r0;
			pDestination[1] = g0;
			pDestination[2] = b0;
			pDestination[3] = r1;
			pDestination[4] = g1;
			pDestination[5] = b1;
			pDestination[6] = r2;
			pDestination[7] = g2;
			pDestination[8] = b2;
			pDestination[9] = r3;
			pDestination[10] = g3;
			pDestination[11] = b3;
			pDestination += 12;
			pSourceRow += 4;
		}

		for (; x + 2 <= nNumberOfBlocksX; x += 2) {
			const __m256i* pFilter = pFilterStart;
			const uint8* pSrc0 = (const uint8*)pSourceRow;
			const uint8* pSrc1 = pSrc0 + sizeof(__m256i);

			__m256i r0 = _mm256_setzero_si256(), r1 = _mm256_setzero_si256();
			__m256i g0 = _mm256_setzero_si256(), g1 = _mm256_setzero_si256();
			__m256i b0 = _mm256_setzero_si256(), b1 = _mm256_setzero_si256();

			for (int i = 0; i < filterLen; i++) {
				__m256i k = *pFilter++;

				// Red channel
				__m256i px0 = *(__m256i*)pSrc0;
				__m256i px1 = *(__m256i*)pSrc1;
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px0 = _mm256_mulhi_epi16(px0, k);
				px1 = _mm256_mulhi_epi16(px1, k);
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				r0 = _mm256_adds_epi16(r0, px0);
				r1 = _mm256_adds_epi16(r1, px1);
				pSrc0 += nChannelLenBytes;
				pSrc1 += nChannelLenBytes;

				// Green channel
				px0 = *(__m256i*)pSrc0;
				px1 = *(__m256i*)pSrc1;
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px0 = _mm256_mulhi_epi16(px0, k);
				px1 = _mm256_mulhi_epi16(px1, k);
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				g0 = _mm256_adds_epi16(g0, px0);
				g1 = _mm256_adds_epi16(g1, px1);
				pSrc0 += nChannelLenBytes;
				pSrc1 += nChannelLenBytes;

				// Blue channel
				px0 = *(__m256i*)pSrc0;
				px1 = *(__m256i*)pSrc1;
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px0 = _mm256_mulhi_epi16(px0, k);
				px1 = _mm256_mulhi_epi16(px1, k);
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				b0 = _mm256_adds_epi16(b0, px0);
				b1 = _mm256_adds_epi16(b1, px1);
				pSrc0 += nChannelLenBytes;
				pSrc1 += nChannelLenBytes;
			}

			const __m256i ymmZero = _mm256_setzero_si256();
			r0 = _mm256_max_epi16(_mm256_min_epi16(r0, ymm0), ymmZero);
			r1 = _mm256_max_epi16(_mm256_min_epi16(r1, ymm0), ymmZero);
			g0 = _mm256_max_epi16(_mm256_min_epi16(g0, ymm0), ymmZero);
			g1 = _mm256_max_epi16(_mm256_min_epi16(g1, ymm0), ymmZero);
			b0 = _mm256_max_epi16(_mm256_min_epi16(b0, ymm0), ymmZero);
			b1 = _mm256_max_epi16(_mm256_min_epi16(b1, ymm0), ymmZero);

			pDestination[0] = r0;
			pDestination[1] = g0;
			pDestination[2] = b0;
			pDestination[3] = r1;
			pDestination[4] = g1;
			pDestination[5] = b1;
			pDestination += 6;
			pSourceRow += 2;
		}

		for (; x < nNumberOfBlocksX; x++) {
			const __m256i* pSource = pSourceRow;
			const __m256i* pFilter = pFilterStart;
			__m256i ymm4 = _mm256_setzero_si256();
			__m256i ymm5 = _mm256_setzero_si256();
			__m256i ymm6 = _mm256_setzero_si256();
			for (int i = 0; i < filterLen; i++) {
				__m256i ymm7 = *pFilter;

				// the pixel data RED channel
				__m256i ymm2 = *pSource;
				ymm2 = _mm256_add_epi16(ymm2, ymm2);
				ymm2 = _mm256_mulhi_epi16(ymm2, ymm7);
				ymm2 = _mm256_add_epi16(ymm2, ymm2);
				ymm4 = _mm256_adds_epi16(ymm4, ymm2);
				pSource = (__m256i*)((uint8*)pSource + nChannelLenBytes);

				// the pixel data GREEN channel
				__m256i ymm3 = *pSource;
				ymm3 = _mm256_add_epi16(ymm3, ymm3);
				ymm3 = _mm256_mulhi_epi16(ymm3, ymm7);
				ymm3 = _mm256_add_epi16(ymm3, ymm3);
				ymm5 = _mm256_adds_epi16(ymm5, ymm3);
				pSource = (__m256i*)((uint8*)pSource + nChannelLenBytes);

				// the pixel data BLUE channel
				ymm2 = *pSource;
				ymm2 = _mm256_add_epi16(ymm2, ymm2);
				ymm2 = _mm256_mulhi_epi16(ymm2, ymm7);
				ymm2 = _mm256_add_epi16(ymm2, ymm2);
				ymm6 = _mm256_adds_epi16(ymm6, ymm2);
				pSource = (__m256i*)((uint8*)pSource + nChannelLenBytes);

				pFilter++;
			}

			// limit to range 0 (in ymm1), 16383-42 (in ymm0)
			ymm4 = _mm256_min_epi16(ymm4, ymm0);
			ymm5 = _mm256_min_epi16(ymm5, ymm0);
			ymm6 = _mm256_min_epi16(ymm6, ymm0);

			__m256i ymm1 = _mm256_setzero_si256();

			ymm4 = _mm256_max_epi16(ymm4, ymm1);
			ymm5 = _mm256_max_epi16(ymm5, ymm1);
			ymm6 = _mm256_max_epi16(ymm6, ymm1);

			// store result in blocks
			*pDestination++ = ymm4;
			*pDestination++ = ymm5;
			*pDestination++ = ymm6;

			pSourceRow++;
		}
	}

	return tempImage;
}

#endif