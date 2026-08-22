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

	(void)nSourceHeight;
	int nStartXAligned = nStartX & ~15;
	int nEndXAligned = (nStartX + nWidth + 15) & ~15;
	CXMMImage* tempImage = new CXMMImage(nEndXAligned - nStartXAligned, nTargetHeight, 16);
	if (tempImage->AlignedPtr() == nullptr) {
		delete tempImage;
		return nullptr;
	}

	int nChannelLenBytes = pSourceImg->GetPaddedWidth() * sizeof(short);
	int nRowLenBytes = nChannelLenBytes * 3;
	int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 4;
	const uint8* pSourceStart = static_cast<const uint8*>(pSourceImg->AlignedPtr()) + nStartXAligned * sizeof(short);
	AVXFilterKernel** pKernelIndexStart = filter.Indices;

	const int nRowLenDestBytes = nNumberOfBlocksX * 3 * static_cast<int>(sizeof(__m256i));
	const __m256i ymm0 = _mm256_set1_epi16(16383 - 42); // 1.0 in fixed point notation minus rounding correction
	const __m256i ymmZero = _mm256_setzero_si256();

	for (int y = 0; y < nTargetHeight; y++) {
		int nCurY = nStartY_FP + y * nIncrementY_FP;
		uint32 nCurYInt = static_cast<uint32>(nCurY) >> 16;
		int filterIndex = y + nFilterOffset;
		AVXFilterKernel* pKernel = pKernelIndexStart[filterIndex];
		int filterLen = pKernel->FilterLen;
		int filterOffset = pKernel->FilterOffset;
		const __m256i* pFilterStart = reinterpret_cast<const __m256i*>(&(pKernel->Kernel));
		const uint8* pSourceRowBase = pSourceStart + (static_cast<int>(nCurYInt) - filterOffset) * nRowLenBytes;
		__m256i* pDestination = reinterpret_cast<__m256i*>(static_cast<uint8*>(tempImage->AlignedPtr()) + static_cast<size_t>(y) * nRowLenDestBytes);

		for (int blk = 0; blk < nNumberOfBlocksX; blk++) {
			const __m256i* pFilter = pFilterStart;
			const uint8* pSrcB = pSourceRowBase + blk * sizeof(__m256i);
			const uint8* pSrcG = pSrcB + nChannelLenBytes;
			const uint8* pSrcR = pSrcG + nChannelLenBytes;

			__m256i b = _mm256_setzero_si256();
			__m256i g = _mm256_setzero_si256();
			__m256i r = _mm256_setzero_si256();

			for (int i = 0; i < filterLen; i++) {
				__m256i k = *pFilter++;

				__m256i pxB = *reinterpret_cast<const __m256i*>(pSrcB);
				pxB = _mm256_add_epi16(pxB, pxB);
				pxB = _mm256_mulhi_epi16(pxB, k);
				pxB = _mm256_add_epi16(pxB, pxB);
				b = _mm256_adds_epi16(b, pxB);
				pSrcB += nRowLenBytes;

				__m256i pxG = *reinterpret_cast<const __m256i*>(pSrcG);
				pxG = _mm256_add_epi16(pxG, pxG);
				pxG = _mm256_mulhi_epi16(pxG, k);
				pxG = _mm256_add_epi16(pxG, pxG);
				g = _mm256_adds_epi16(g, pxG);
				pSrcG += nRowLenBytes;

				__m256i pxR = *reinterpret_cast<const __m256i*>(pSrcR);
				pxR = _mm256_add_epi16(pxR, pxR);
				pxR = _mm256_mulhi_epi16(pxR, k);
				pxR = _mm256_add_epi16(pxR, pxR);
				r = _mm256_adds_epi16(r, pxR);
				pSrcR += nRowLenBytes;
			}

			b = _mm256_max_epi16(_mm256_min_epi16(b, ymm0), ymmZero);
			g = _mm256_max_epi16(_mm256_min_epi16(g, ymm0), ymmZero);
			r = _mm256_max_epi16(_mm256_min_epi16(r, ymm0), ymmZero);

			*pDestination++ = b;
			*pDestination++ = g;
			*pDestination++ = r;
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
	if (tempImage->AlignedPtr() == nullptr) {
		delete tempImage;
		return nullptr;
	}

	int nSrcLineWidthPadded = Helpers::DoPadding(nDIBWidth * nChannels, 4);
	int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 4;
	AVXFilterKernel** pKernelIndexStart = filter.Indices;

	const int nRowLenDestBytes = nNumberOfBlocksX * 3 * static_cast<int>(sizeof(__m256i));
	const __m256i ymm0 = _mm256_set1_epi16(16383 - 42);
	const __m256i ymmZero = _mm256_setzero_si256();

	const __m256i mask_b = _mm256_setr_epi8(0, -1, 4, -1, 8, -1, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	                                         0, -1, 4, -1, 8, -1, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m256i mask_g = _mm256_setr_epi8(1, -1, 5, -1, 9, -1, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	                                         1, -1, 5, -1, 9, -1, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m256i mask_r = _mm256_setr_epi8(2, -1, 6, -1, 10, -1, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	                                         2, -1, 6, -1, 10, -1, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1);

	// 3-channel 48-byte shuffle masks
	const __m128i mb0 = _mm_setr_epi8(0, -1, 3, -1, 6, -1, 9, -1, 12, -1, 15, -1, -1, -1, -1, -1);
	const __m128i mb1 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 2, -1, 5, -1);
	const __m128i mb2 = _mm_setr_epi8(8, -1, 11, -1, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m128i mb3 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, 1, -1, 4, -1, 7, -1, 10, -1, 13, -1);

	const __m128i mg0 = _mm_setr_epi8(1, -1, 4, -1, 7, -1, 10, -1, 13, -1, -1, -1, -1, -1, -1, -1);
	const __m128i mg1 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, -1, 3, -1, 6, -1);
	const __m128i mg2 = _mm_setr_epi8(9, -1, 12, -1, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m128i mg3 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, 2, -1, 5, -1, 8, -1, 11, -1, 14, -1);

	const __m128i mr0 = _mm_setr_epi8(2, -1, 5, -1, 8, -1, 11, -1, 14, -1, -1, -1, -1, -1, -1, -1);
	const __m128i mr1 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1, -1, 4, -1, 7, -1);
	const __m128i mr2 = _mm_setr_epi8(10, -1, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
	const __m128i mr3 = _mm_setr_epi8(-1, -1, -1, -1, 0, -1, 3, -1, 6, -1, 9, -1, 12, -1, 15, -1);

	for (int y = 0; y < nTargetHeight; y++) {
		int nCurY = nStartY_FP + y * nIncrementY_FP;
		uint32 nCurYInt = static_cast<uint32>(nCurY) >> 16;
		int filterIndex = y + nFilterOffset;
		AVXFilterKernel* pKernel = pKernelIndexStart[filterIndex];
		int filterLen = pKernel->FilterLen;
		int filterOffset = pKernel->FilterOffset;
		const __m256i* pFilterStart = reinterpret_cast<const __m256i*>(&(pKernel->Kernel));
		__m256i* pDestination = reinterpret_cast<__m256i*>(static_cast<uint8*>(tempImage->AlignedPtr()) + static_cast<size_t>(y) * nRowLenDestBytes);

		for (int blk = 0; blk < nNumberOfBlocksX; blk++) {
			int pixelX = nStartXAligned + blk * 16;
			__m256i b_acc = _mm256_setzero_si256();
			__m256i g_acc = _mm256_setzero_si256();
			__m256i r_acc = _mm256_setzero_si256();

			const __m256i* pFilter = pFilterStart;

			for (int i = 0; i < filterLen; i++) {
				int srcRowY = static_cast<int>(nCurYInt) - filterOffset + i;
				if (srcRowY < 0) srcRowY = 0;
				else if (srcRowY >= nSourceHeight) srcRowY = nSourceHeight - 1;

				const uint8* pSrcRow = static_cast<const uint8*>(pDIB) + static_cast<size_t>(srcRowY) * static_cast<size_t>(nSrcLineWidthPadded) + static_cast<size_t>(pixelX) * nChannels;
				__m256i k = *pFilter++;

				if (nChannels == 4) {
					const uint32* pSrc32 = reinterpret_cast<const uint32*>(pSrcRow);
					__m256i px0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pSrc32));
					__m256i px1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pSrc32 + 8));

					__m256i sb0 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px0, mask_b), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i sb1 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px1, mask_b), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i b16 = _mm256_slli_epi16(_mm256_permute2x128_si256(sb0, sb1, 0x20), 7);
					b16 = _mm256_mulhi_epi16(b16, k);
					b16 = _mm256_add_epi16(b16, b16);
					b_acc = _mm256_adds_epi16(b_acc, b16);

					__m256i sg0 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px0, mask_g), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i sg1 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px1, mask_g), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i g16 = _mm256_slli_epi16(_mm256_permute2x128_si256(sg0, sg1, 0x20), 7);
					g16 = _mm256_mulhi_epi16(g16, k);
					g16 = _mm256_add_epi16(g16, g16);
					g_acc = _mm256_adds_epi16(g_acc, g16);

					__m256i sr0 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px0, mask_r), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i sr1 = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(px1, mask_r), _MM_SHUFFLE(3, 1, 2, 0));
					__m256i r16 = _mm256_slli_epi16(_mm256_permute2x128_si256(sr0, sr1, 0x20), 7);
					r16 = _mm256_mulhi_epi16(r16, k);
					r16 = _mm256_add_epi16(r16, r16);
					r_acc = _mm256_adds_epi16(r_acc, r16);
				} else {
					const __m128i* pSrc128 = reinterpret_cast<const __m128i*>(pSrcRow);
					__m128i v0 = _mm_loadu_si128(pSrc128);
					__m128i v1 = _mm_loadu_si128(pSrc128 + 1);
					__m128i v2 = _mm_loadu_si128(pSrc128 + 2);

					__m128i b_lo = _mm_or_si128(_mm_shuffle_epi8(v0, mb0), _mm_shuffle_epi8(v1, mb1));
					__m128i b_hi = _mm_or_si128(_mm_shuffle_epi8(v1, mb2), _mm_shuffle_epi8(v2, mb3));
					__m256i b16 = _mm256_slli_epi16(_mm256_set_m128i(b_hi, b_lo), 7);
					b16 = _mm256_mulhi_epi16(b16, k);
					b16 = _mm256_add_epi16(b16, b16);
					b_acc = _mm256_adds_epi16(b_acc, b16);

					__m128i g_lo = _mm_or_si128(_mm_shuffle_epi8(v0, mg0), _mm_shuffle_epi8(v1, mg1));
					__m128i g_hi = _mm_or_si128(_mm_shuffle_epi8(v1, mg2), _mm_shuffle_epi8(v2, mg3));
					__m256i g16 = _mm256_slli_epi16(_mm256_set_m128i(g_hi, g_lo), 7);
					g16 = _mm256_mulhi_epi16(g16, k);
					g16 = _mm256_add_epi16(g16, g16);
					g_acc = _mm256_adds_epi16(g_acc, g16);

					__m128i r_lo = _mm_or_si128(_mm_shuffle_epi8(v0, mr0), _mm_shuffle_epi8(v1, mr1));
					__m128i r_hi = _mm_or_si128(_mm_shuffle_epi8(v1, mr2), _mm_shuffle_epi8(v2, mr3));
					__m256i r16 = _mm256_slli_epi16(_mm256_set_m128i(r_hi, r_lo), 7);
					r16 = _mm256_mulhi_epi16(r16, k);
					r16 = _mm256_add_epi16(r16, r16);
					r_acc = _mm256_adds_epi16(r_acc, r16);
				}
			}

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

#endif