#pragma once

class CXMMImage;
struct AVXFilterKernelBlock;

// Applied across image rows using AVX2 SIMD instructions.
CXMMImage* ApplyFilter_AVX(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const AVXFilterKernelBlock& filter,
	int nFilterOffset, const CXMMImage* pSourceImg);

// Direct single-pass filtering and format conversion from raw DIB pixel memory.
CXMMImage* ApplyFilter_DirectFromDIB_AVX(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const AVXFilterKernelBlock& filter,
	int nFilterOffset, const void* pDIB, int nChannels, int nDIBWidth);