#pragma once

class CXMMImage;
struct AVXFilterKernelBlock;

// Used by BasicProcessing.cpp: Applies a filter using AVX. Own compilation unit to be able to compile this with AVX compiler flag.
CXMMImage* ApplyFilter_AVX(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const AVXFilterKernelBlock& filter,
	int nFilterOffset, const CXMMImage* pSourceImg);

CXMMImage* ApplyFilter_DirectFromDIB_AVX(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const AVXFilterKernelBlock& filter,
	int nFilterOffset, const void* pDIB, int nChannels, int nDIBWidth);

CXMMImage* ApplyFilter_DirectFrom1Channel_AVX(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const AVXFilterKernelBlock& filter,
	int nFilterOffset, const unsigned char* pPlane, int nPlaneStride);

CXMMImage* ApplyFilter_1Channel_AVX(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const AVXFilterKernelBlock& filter,
	int nFilterOffset, const CXMMImage* pSourceImg);

void RotateAndConvertYUVToDIB_AVX(const CXMMImage* pY, const CXMMImage* pU, const CXMMImage* pV,
	unsigned char* pTargetDIB, int targetWidth, int targetHeight);