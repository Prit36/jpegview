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
			const uint8* pSrcB0 = (const uint8*)pSourceRow;
			const uint8* pSrcB1 = pSrcB0 + sizeof(__m256i);
			const uint8* pSrcB2 = pSrcB0 + 2 * sizeof(__m256i);
			const uint8* pSrcB3 = pSrcB0 + 3 * sizeof(__m256i);

			const uint8* pSrcG0 = pSrcB0 + nChannelLenBytes;
			const uint8* pSrcG1 = pSrcB1 + nChannelLenBytes;
			const uint8* pSrcG2 = pSrcB2 + nChannelLenBytes;
			const uint8* pSrcG3 = pSrcB3 + nChannelLenBytes;

			const uint8* pSrcR0 = pSrcG0 + nChannelLenBytes;
			const uint8* pSrcR1 = pSrcG1 + nChannelLenBytes;
			const uint8* pSrcR2 = pSrcG2 + nChannelLenBytes;
			const uint8* pSrcR3 = pSrcG3 + nChannelLenBytes;

			__m256i b0 = _mm256_setzero_si256(), b1 = _mm256_setzero_si256(), b2 = _mm256_setzero_si256(), b3 = _mm256_setzero_si256();
			__m256i g0 = _mm256_setzero_si256(), g1 = _mm256_setzero_si256(), g2 = _mm256_setzero_si256(), g3 = _mm256_setzero_si256();
			__m256i r0 = _mm256_setzero_si256(), r1 = _mm256_setzero_si256(), r2 = _mm256_setzero_si256(), r3 = _mm256_setzero_si256();

			for (int i = 0; i < filterLen; i++) {
				__m256i k = *pFilter++;

				// Red channel
				__m256i px0 = *(__m256i*)pSrcB0;
				__m256i px1 = *(__m256i*)pSrcB1;
				__m256i px2 = *(__m256i*)pSrcB2;
				__m256i px3 = *(__m256i*)pSrcB3;
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
				pSrcB0 += nRowLenBytes;
				pSrcB1 += nRowLenBytes;
				pSrcB2 += nRowLenBytes;
				pSrcB3 += nRowLenBytes;

				// Green channel
				px0 = *(__m256i*)pSrcG0;
				px1 = *(__m256i*)pSrcG1;
				px2 = *(__m256i*)pSrcG2;
				px3 = *(__m256i*)pSrcG3;
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
				pSrcG0 += nRowLenBytes;
				pSrcG1 += nRowLenBytes;
				pSrcG2 += nRowLenBytes;
				pSrcG3 += nRowLenBytes;

				// Blue channel
				px0 = *(__m256i*)pSrcR0;
				px1 = *(__m256i*)pSrcR1;
				px2 = *(__m256i*)pSrcR2;
				px3 = *(__m256i*)pSrcR3;
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
				pSrcR0 += nRowLenBytes;
				pSrcR1 += nRowLenBytes;
				pSrcR2 += nRowLenBytes;
				pSrcR3 += nRowLenBytes;
			}

			const __m256i ymmZero = _mm256_setzero_si256();
			b0 = _mm256_max_epi16(_mm256_min_epi16(b0, ymm0), ymmZero);
			b1 = _mm256_max_epi16(_mm256_min_epi16(b1, ymm0), ymmZero);
			b2 = _mm256_max_epi16(_mm256_min_epi16(b2, ymm0), ymmZero);
			b3 = _mm256_max_epi16(_mm256_min_epi16(b3, ymm0), ymmZero);
			g0 = _mm256_max_epi16(_mm256_min_epi16(g0, ymm0), ymmZero);
			g1 = _mm256_max_epi16(_mm256_min_epi16(g1, ymm0), ymmZero);
			g2 = _mm256_max_epi16(_mm256_min_epi16(g2, ymm0), ymmZero);
			g3 = _mm256_max_epi16(_mm256_min_epi16(g3, ymm0), ymmZero);
			r0 = _mm256_max_epi16(_mm256_min_epi16(r0, ymm0), ymmZero);
			r1 = _mm256_max_epi16(_mm256_min_epi16(r1, ymm0), ymmZero);
			r2 = _mm256_max_epi16(_mm256_min_epi16(r2, ymm0), ymmZero);
			r3 = _mm256_max_epi16(_mm256_min_epi16(r3, ymm0), ymmZero);

			pDestination[0] = b0;
			pDestination[1] = g0;
			pDestination[2] = r0;
			pDestination[3] = b1;
			pDestination[4] = g1;
			pDestination[5] = r1;
			pDestination[6] = b2;
			pDestination[7] = g2;
			pDestination[8] = r2;
			pDestination[9] = b3;
			pDestination[10] = g3;
			pDestination[11] = r3;
			pDestination += 12;
			pSourceRow += 4;
		}

		for (; x + 2 <= nNumberOfBlocksX; x += 2) {
			const __m256i* pFilter = pFilterStart;
			const uint8* pSrcB0 = (const uint8*)pSourceRow;
			const uint8* pSrcB1 = pSrcB0 + sizeof(__m256i);
			const uint8* pSrcG0 = pSrcB0 + nChannelLenBytes;
			const uint8* pSrcG1 = pSrcB1 + nChannelLenBytes;
			const uint8* pSrcR0 = pSrcG0 + nChannelLenBytes;
			const uint8* pSrcR1 = pSrcG1 + nChannelLenBytes;

			__m256i b0 = _mm256_setzero_si256(), b1 = _mm256_setzero_si256();
			__m256i g0 = _mm256_setzero_si256(), g1 = _mm256_setzero_si256();
			__m256i r0 = _mm256_setzero_si256(), r1 = _mm256_setzero_si256();

			for (int i = 0; i < filterLen; i++) {
				__m256i k = *pFilter++;

				// Red channel
				__m256i px0 = *(__m256i*)pSrcB0;
				__m256i px1 = *(__m256i*)pSrcB1;
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px0 = _mm256_mulhi_epi16(px0, k);
				px1 = _mm256_mulhi_epi16(px1, k);
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				b0 = _mm256_adds_epi16(b0, px0);
				b1 = _mm256_adds_epi16(b1, px1);
				pSrcB0 += nRowLenBytes;
				pSrcB1 += nRowLenBytes;

				// Green channel
				px0 = *(__m256i*)pSrcG0;
				px1 = *(__m256i*)pSrcG1;
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px0 = _mm256_mulhi_epi16(px0, k);
				px1 = _mm256_mulhi_epi16(px1, k);
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				g0 = _mm256_adds_epi16(g0, px0);
				g1 = _mm256_adds_epi16(g1, px1);
				pSrcG0 += nRowLenBytes;
				pSrcG1 += nRowLenBytes;

				// Blue channel
				px0 = *(__m256i*)pSrcR0;
				px1 = *(__m256i*)pSrcR1;
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				px0 = _mm256_mulhi_epi16(px0, k);
				px1 = _mm256_mulhi_epi16(px1, k);
				px0 = _mm256_add_epi16(px0, px0);
				px1 = _mm256_add_epi16(px1, px1);
				r0 = _mm256_adds_epi16(r0, px0);
				r1 = _mm256_adds_epi16(r1, px1);
				pSrcR0 += nRowLenBytes;
				pSrcR1 += nRowLenBytes;
			}

			const __m256i ymmZero = _mm256_setzero_si256();
			b0 = _mm256_max_epi16(_mm256_min_epi16(b0, ymm0), ymmZero);
			b1 = _mm256_max_epi16(_mm256_min_epi16(b1, ymm0), ymmZero);
			g0 = _mm256_max_epi16(_mm256_min_epi16(g0, ymm0), ymmZero);
			g1 = _mm256_max_epi16(_mm256_min_epi16(g1, ymm0), ymmZero);
			r0 = _mm256_max_epi16(_mm256_min_epi16(r0, ymm0), ymmZero);
			r1 = _mm256_max_epi16(_mm256_min_epi16(r1, ymm0), ymmZero);

			pDestination[0] = b0;
			pDestination[1] = g0;
			pDestination[2] = r0;
			pDestination[3] = b1;
			pDestination[4] = g1;
			pDestination[5] = r1;
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