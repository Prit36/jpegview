#include "StdAfx.h"
#include "XMMImage.h"
#include "ResizeFilter.h"
#include "ApplyFilterAVX.h"
#include "Helpers.h"

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

CXMMImage* ApplyFilter_DirectFromDIB_AVX(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const AVXFilterKernelBlock& filter,
	int nFilterOffset, const void* pDIB, int nChannels, int nDIBWidth) {

	int nStartXAligned = nStartX & ~15;
	int nEndXAligned = (nStartX + nWidth + 15) & ~15;
	CXMMImage* tempImage = new CXMMImage(nEndXAligned - nStartXAligned, nTargetHeight, 16);
	if (tempImage->AlignedPtr() == NULL) {
		delete tempImage;
		return NULL;
	}

	int nSrcLineWidthPadded = Helpers::DoPadding(nDIBWidth * nChannels, 4);
	int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 4;
	AVXFilterKernel** pKernelIndexStart = filter.Indices;

	const int nRowLenDestBytes = nNumberOfBlocksX * 3 * (int)sizeof(__m256i);
	const __m256i ymm0 = _mm256_set1_epi16(16383 - 42); // 1.0 in fixed point notation, minus rounding correction
	const __m256i ymmZero = _mm256_setzero_si256();

	const __m256i mask_b = _mm256_setr_epi8(0, -1, 4, -1, 8, -1, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	                                         0, -1, 4, -1, 8, -1, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m256i mask_g = _mm256_setr_epi8(1, -1, 5, -1, 9, -1, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	                                         1, -1, 5, -1, 9, -1, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m256i mask_r = _mm256_setr_epi8(2, -1, 6, -1, 10, -1, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	                                         2, -1, 6, -1, 10, -1, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1);

	for (int y = 0; y < nTargetHeight; y++) {
		int nCurY = nStartY_FP + y * nIncrementY_FP;
		uint32 nCurYInt = (uint32)nCurY >> 16; // integer part of Y
		int filterIndex = y + nFilterOffset;
		AVXFilterKernel* pKernel = pKernelIndexStart[filterIndex];
		int filterLen = pKernel->FilterLen;
		int filterOffset = pKernel->FilterOffset;
		const __m256i* pFilterStart = (__m256i*)&(pKernel->Kernel);
		__m256i* pDestination = (__m256i*)((uint8*)tempImage->AlignedPtr() + (size_t)y * nRowLenDestBytes);

		for (int blk = 0; blk < nNumberOfBlocksX; blk++) {
			int pixelX = nStartXAligned + blk * 16;
			__m256i b_acc = _mm256_setzero_si256();
			__m256i g_acc = _mm256_setzero_si256();
			__m256i r_acc = _mm256_setzero_si256();

			const __m256i* pFilter = pFilterStart;

			for (int i = 0; i < filterLen; i++) {
				int srcRowY = (int)nCurYInt - filterOffset + i;
				if (srcRowY < 0) srcRowY = 0;
				else if (srcRowY >= nSourceHeight) srcRowY = nSourceHeight - 1;

				const uint8* pSrcRow = (const uint8*)pDIB + (size_t)srcRowY * (size_t)nSrcLineWidthPadded + (size_t)pixelX * nChannels;
				__m256i k = *pFilter++;

				if (nChannels == 4) {
					const uint32* pSrc32 = (const uint32*)pSrcRow;
					__m256i px0 = _mm256_loadu_si256((const __m256i*)pSrc32);
					__m256i px1 = _mm256_loadu_si256((const __m256i*)(pSrc32 + 8));

					// Blue
					__m256i sb0 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px0, mask_b), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i sb1 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px1, mask_b), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i b16 = _mm256_slli_epi16(_mm256_permute2x128_si256(sb0, sb1, 0x20), 7);
					b16 = _mm256_mulhi_epi16(b16, k);
					b16 = _mm256_add_epi16(b16, b16);
					b_acc = _mm256_adds_epi16(b_acc, b16);

					// Green
					__m256i sg0 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px0, mask_g), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i sg1 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px1, mask_g), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i g16 = _mm256_slli_epi16(_mm256_permute2x128_si256(sg0, sg1, 0x20), 7);
					g16 = _mm256_mulhi_epi16(g16, k);
					g16 = _mm256_add_epi16(g16, g16);
					g_acc = _mm256_adds_epi16(g_acc, g16);

					// Red
					__m256i sr0 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px0, mask_r), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i sr1 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px1, mask_r), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i r16 = _mm256_slli_epi16(_mm256_permute2x128_si256(sr0, sr1, 0x20), 7);
					r16 = _mm256_mulhi_epi16(r16, k);
					r16 = _mm256_add_epi16(r16, r16);
					r_acc = _mm256_adds_epi16(r_acc, r16);
				} else {
					// 3 channels: load 48 bytes (16 BGR pixels) using 3 128-bit loads
					const __m128i* pSrc128 = (const __m128i*)pSrcRow;
					__m128i v0 = _mm_loadu_si128(pSrc128);
					__m128i v1 = _mm_loadu_si128(pSrc128 + 1);
					__m128i v2 = _mm_loadu_si128(pSrc128 + 2);

					// Masks for B
					const __m128i mb0 = _mm_setr_epi8(0, -1, 3, -1, 6, -1, 9, -1, 12, -1, 15, -1, -1, -1, -1, -1);
					const __m128i mb1 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 2, -1, 5, -1);
					const __m128i mb2 = _mm_setr_epi8(8, -1, 11, -1, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
					const __m128i mb3 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, 1, -1, 4, -1, 7, -1, 10, -1, 13, -1);

					__m128i b_lo = _mm_or_si128(_mm_shuffle_epi8(v0, mb0), _mm_shuffle_epi8(v1, mb1));
					__m128i b_hi = _mm_or_si128(_mm_shuffle_epi8(v1, mb2), _mm_shuffle_epi8(v2, mb3));
					__m256i b16 = _mm256_slli_epi16(_mm256_set_m128i(b_hi, b_lo), 7);
					b16 = _mm256_mulhi_epi16(b16, k);
					b16 = _mm256_add_epi16(b16, b16);
					b_acc = _mm256_adds_epi16(b_acc, b16);

					// Masks for G
					const __m128i mg0 = _mm_setr_epi8(1, -1, 4, -1, 7, -1, 10, -1, 13, -1, -1, -1, -1, -1, -1, -1);
					const __m128i mg1 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, -1, 3, -1, 6, -1);
					const __m128i mg2 = _mm_setr_epi8(9, -1, 12, -1, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
					const __m128i mg3 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, 2, -1, 5, -1, 8, -1, 11, -1, 14, -1);

					__m128i g_lo = _mm_or_si128(_mm_shuffle_epi8(v0, mg0), _mm_shuffle_epi8(v1, mg1));
					__m128i g_hi = _mm_or_si128(_mm_shuffle_epi8(v1, mg2), _mm_shuffle_epi8(v2, mg3));
					__m256i g16 = _mm256_slli_epi16(_mm256_set_m128i(g_hi, g_lo), 7);
					g16 = _mm256_mulhi_epi16(g16, k);
					g16 = _mm256_add_epi16(g16, g16);
					g_acc = _mm256_adds_epi16(g_acc, g16);

					// Masks for R
					const __m128i mr0 = _mm_setr_epi8(2, -1, 5, -1, 8, -1, 11, -1, 14, -1, -1, -1, -1, -1, -1, -1);
					const __m128i mr1 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1, -1, 4, -1, 7, -1);
					const __m128i mr2 = _mm_setr_epi8(10, -1, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
					const __m128i mr3 = _mm_setr_epi8(-1, -1, -1, -1, 0, -1, 3, -1, 6, -1, 9, -1, 12, -1, 15, -1);

					__m128i r_lo = _mm_or_si128(_mm_shuffle_epi8(v0, mr0), _mm_shuffle_epi8(v1, mr1));
					__m128i r_hi = _mm_or_si128(_mm_shuffle_epi8(v1, mr2), _mm_shuffle_epi8(v2, mr3));
					__m256i r16 = _mm256_slli_epi16(_mm256_set_m128i(r_hi, r_lo), 7);
					r16 = _mm256_mulhi_epi16(r16, k);
					r16 = _mm256_add_epi16(r16, r16);
					r_acc = _mm256_adds_epi16(r_acc, r16);
				}
			}

			// Clamp and store
			b_acc = _mm256_max_epi16(_mm256_min_epi16(b_acc, ymm0), ymmZero);
			g_acc = _mm256_max_epi16(_mm256_min_epi16(g_acc, ymm0), ymmZero);
			r_acc = _mm256_max_epi16(_mm256_min_epi16(r_acc, ymm0), ymmZero);

			*pDestination++ = b_acc;
			*pDestination++ = g_acc;
			*pDestination++ = r_acc;
		}
	}

	return tempImage;
}

CXMMImage* ApplyFilter_DirectFrom1Channel_AVX(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const AVXFilterKernelBlock& filter,
	int nFilterOffset, const unsigned char* pPlane, int nPlaneStride) {

	int nStartXAligned = nStartX & ~15;
	int nEndXAligned = (nStartX + nWidth + 15) & ~15;
	CXMMImage* tempImage = new CXMMImage(nEndXAligned - nStartXAligned, nTargetHeight, 16);
	if (tempImage->AlignedPtr() == NULL) {
		delete tempImage;
		return NULL;
	}

	int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 4;
	AVXFilterKernel** pKernelIndexStart = filter.Indices;

	const int nRowLenDestBytes = nNumberOfBlocksX * 3 * (int)sizeof(__m256i);
	const __m256i ymm0 = _mm256_set1_epi16(16383 - 42);
	const __m256i ymmZero = _mm256_setzero_si256();

	for (int y = 0; y < nTargetHeight; y++) {
		int nCurY = nStartY_FP + y * nIncrementY_FP;
		uint32 nCurYInt = (uint32)nCurY >> 16;
		int filterIndex = y + nFilterOffset;
		AVXFilterKernel* pKernel = pKernelIndexStart[filterIndex];
		int filterLen = pKernel->FilterLen;
		int filterOffset = pKernel->FilterOffset;
		const __m256i* pFilterStart = (__m256i*)&(pKernel->Kernel);
		__m256i* pDestination = (__m256i*)((uint8*)tempImage->AlignedPtr() + (size_t)y * nRowLenDestBytes);

		for (int blk = 0; blk < nNumberOfBlocksX; blk++) {
			int pixelX = nStartXAligned + blk * 16;
			__m256i acc = _mm256_setzero_si256();
			const __m256i* pFilter = pFilterStart;

			for (int i = 0; i < filterLen; i++) {
				int srcRowY = (int)nCurYInt - filterOffset + i;
				if (srcRowY < 0) srcRowY = 0;
				else if (srcRowY >= nSourceHeight) srcRowY = nSourceHeight - 1;

				const uint8* pSrc = pPlane + (size_t)srcRowY * (size_t)nPlaneStride + pixelX;
				__m256i k = *pFilter++;

				__m128i bytes16 = _mm_loadu_si128((const __m128i*)pSrc);
				__m256i words16 = _mm256_slli_epi16(_mm256_cvtepu8_epi16(bytes16), 7);
				__m256i prod = _mm256_mulhi_epi16(words16, k);
				prod = _mm256_add_epi16(prod, prod);
				acc = _mm256_adds_epi16(acc, prod);
			}

			acc = _mm256_max_epi16(_mm256_min_epi16(acc, ymm0), ymmZero);
			*pDestination++ = acc;
		}
	}

	return tempImage;
}

CXMMImage* ApplyFilter_1Channel_AVX(int nSourceHeight, int nTargetHeight, int nWidth,
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

	int nRowLenBytes = pSourceImg->GetPaddedWidth() * sizeof(short) * 3;
	int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 4;
	const uint8* pSourceStart = (const uint8*)pSourceImg->AlignedPtr() + nStartXAligned * sizeof(short);
	AVXFilterKernel** pKernelIndexStart = filter.Indices;

	const int nRowLenDestBytes = nNumberOfBlocksX * 3 * (int)sizeof(__m256i);
	const __m256i ymm0 = _mm256_set1_epi16(16383 - 42);
	const __m256i ymmZero = _mm256_setzero_si256();

	for (int y = 0; y < nTargetHeight; y++) {
		int nCurY = nStartY_FP + y * nIncrementY_FP;
		uint32 nCurYInt = (uint32)nCurY >> 16;
		int filterIndex = y + nFilterOffset;
		AVXFilterKernel* pKernel = pKernelIndexStart[filterIndex];
		int filterLen = pKernel->FilterLen;
		int filterOffset = pKernel->FilterOffset;
		const __m256i* pFilterStart = (__m256i*)&(pKernel->Kernel);
		const __m256i* pSourceRow = (const __m256i*)(pSourceStart + ((int)nCurYInt - filterOffset) * nRowLenBytes);
		__m256i* pDestination = (__m256i*)((uint8*)tempImage->AlignedPtr() + (size_t)y * nRowLenDestBytes);

		for (int blk = 0; blk < nNumberOfBlocksX; blk++) {
			const __m256i* pFilter = pFilterStart;
			const uint8* pSrc = (const uint8*)pSourceRow;
			__m256i acc = _mm256_setzero_si256();

			for (int i = 0; i < filterLen; i++) {
				__m256i k = *pFilter++;
				__m256i px = *(__m256i*)pSrc;
				px = _mm256_add_epi16(px, px);
				px = _mm256_mulhi_epi16(px, k);
				px = _mm256_add_epi16(px, px);
				acc = _mm256_adds_epi16(acc, px);
				pSrc += nRowLenBytes;
			}

			acc = _mm256_max_epi16(_mm256_min_epi16(acc, ymm0), ymmZero);
			*pDestination++ = acc;
			pSourceRow++;
		}
	}

	return tempImage;
}

void RotateAndConvertYUVToDIB_AVX(const CXMMImage* pY, const CXMMImage* pU, const CXMMImage* pV,
	unsigned char* pTargetDIB, int targetWidth, int targetHeight) {

	const int16* pSrcY = (const int16*)pY->AlignedPtr();
	const int16* pSrcU = (const int16*)pU->AlignedPtr();
	const int16* pSrcV = (const int16*)pV->AlignedPtr();

	int nPaddedW = pY->GetPaddedWidth();
	int nTargetStride = targetWidth * 4;

	const __m256i c_359 = _mm256_set1_epi16(359);
	const __m256i c_88 = _mm256_set1_epi16(88);
	const __m256i c_183 = _mm256_set1_epi16(183);
	const __m256i c_454 = _mm256_set1_epi16(454);
	const __m256i c_128 = _mm256_set1_epi16(128);
	const __m256i c_255 = _mm256_set1_epi16(255);
	const __m256i ymmZero = _mm256_setzero_si256();

	for (int y = 0; y < targetHeight; y++) {
		uint8* pDstRow = pTargetDIB + (size_t)y * (size_t)nTargetStride;
		int x = 0;
		for (; x + 16 <= targetWidth; x += 16) {
			int16 yRaw[16], uRaw[16], vRaw[16];
			for (int k = 0; k < 16; k++) {
				yRaw[k] = pSrcY[(size_t)(x + k) * (size_t)nPaddedW * 3 + y];
				uRaw[k] = pSrcU[(size_t)(x + k) * (size_t)nPaddedW * 3 + y];
				vRaw[k] = pSrcV[(size_t)(x + k) * (size_t)nPaddedW * 3 + y];
			}
			__m256i vy = _mm256_srli_epi16(_mm256_loadu_si256((const __m256i*)yRaw), 6);
			__m256i vu = _mm256_sub_epi16(_mm256_srli_epi16(_mm256_loadu_si256((const __m256i*)uRaw), 6), c_128);
			__m256i vv = _mm256_sub_epi16(_mm256_srli_epi16(_mm256_loadu_si256((const __m256i*)vRaw), 6), c_128);

			__m256i vr = _mm256_add_epi16(vy, _mm256_srai_epi16(_mm256_mullo_epi16(vv, c_359), 8));
			__m256i vg = _mm256_sub_epi16(vy, _mm256_srai_epi16(_mm256_add_epi16(_mm256_mullo_epi16(vu, c_88), _mm256_mullo_epi16(vv, c_183)), 8));
			__m256i vb = _mm256_add_epi16(vy, _mm256_srai_epi16(_mm256_mullo_epi16(vu, c_454), 8));

			vr = _mm256_max_epi16(_mm256_min_epi16(vr, c_255), ymmZero);
			vg = _mm256_max_epi16(_mm256_min_epi16(vg, c_255), ymmZero);
			vb = _mm256_max_epi16(_mm256_min_epi16(vb, c_255), ymmZero);

			uint32* pDst32 = (uint32*)(pDstRow + x * 4);
			alignas(32) int16 bArr[16], gArr[16], rArr[16];
			_mm256_store_si256((__m256i*)bArr, vb);
			_mm256_store_si256((__m256i*)gArr, vg);
			_mm256_store_si256((__m256i*)rArr, vr);

			for (int k = 0; k < 16; k++) {
				pDst32[k] = 0xFF000000 | ((uint32)rArr[k] << 16) | ((uint32)gArr[k] << 8) | (uint32)bArr[k];
			}
		}
		for (; x < targetWidth; x++) {
			int16 yVal = pSrcY[(size_t)x * (size_t)nPaddedW * 3 + y] >> 6;
			int16 uVal = (pSrcU[(size_t)x * (size_t)nPaddedW * 3 + y] >> 6) - 128;
			int16 vVal = (pSrcV[(size_t)x * (size_t)nPaddedW * 3 + y] >> 6) - 128;

			int r = yVal + ((vVal * 359) >> 8);
			int g = yVal - ((uVal * 88 + vVal * 183) >> 8);
			int b = yVal + ((uVal * 454) >> 8);

			r = max(0, min(255, r));
			g = max(0, min(255, g));
			b = max(0, min(255, b));

			((uint32*)pDstRow)[x] = 0xFF000000 | ((uint32)r << 16) | ((uint32)g << 8) | (uint32)b;
		}
	}
}

#endif