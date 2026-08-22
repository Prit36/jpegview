#include "stdafx.h"
#include "TJPEGWrapper.h"
#include "ParallelJPEG.h"
#include "libjpeg-turbo/include/turbojpeg.h"
#include "libjpeg-turbo/include/jpeglib.h"
#include "libjpeg-turbo/include/jerror.h"
#include "MaxImageDef.h"
#include "Helpers.h"
#include "BasicProcessing.h"
#include "SettingsProvider.h"
#include <algorithm>
#include <climits>
#include <thread>
#include <vector>

static CBasicProcessing::SIMDArchitecture ToSIMDArch(Helpers::CPUType cpuType) {
	switch (cpuType) {
	case Helpers::CPU_MMX: return CBasicProcessing::MMX;
	case Helpers::CPU_SSE: return CBasicProcessing::SSE;
	case Helpers::CPU_AVX2: return CBasicProcessing::AVX2;
	default: return CBasicProcessing::SSE;
	}
}

// Late-start resample context: overlapping display resample with decode join
struct LateResampleCtx {
	int clientWidth = 0;
	int clientHeight = 0;
	Helpers::EAutoZoomMode zoomMode = Helpers::ZM_FitToScreenNoZoom;
	double zoom = -1.0;
	double sharpen = 0.0;
	EFilterType filter = Filter_Downsampling_Narrow;
	CBasicProcessing::SIMDArchitecture simd = CBasicProcessing::SSE;
	unsigned char* pDIB = nullptr;
	int targetW = 0;
	int targetH = 0;
	int rotation = 0; // 0, 90, 180, 270 - EXIF auto-rotate
};

static void ComputeResampleGeometry(LateResampleCtx* s, int srcW, int srcH, CSize& finalSize, CSize& resampleSize) {
	if (s->zoom < 0.0) {
		if ((s->rotation == 90 || s->rotation == 270) && srcW != srcH) {
			finalSize = Helpers::GetImageRect(srcH, srcW, s->clientWidth, s->clientHeight, s->zoomMode, s->zoom);
		} else {
			finalSize = Helpers::GetImageRect(srcW, srcH, s->clientWidth, s->clientHeight, s->zoomMode, s->zoom);
		}
	} else {
		if (s->rotation == 90 || s->rotation == 270) {
			finalSize = CSize(static_cast<int>(srcH * s->zoom + 0.5), static_cast<int>(srcW * s->zoom + 0.5));
		} else {
			finalSize = CSize(static_cast<int>(srcW * s->zoom + 0.5), static_cast<int>(srcH * s->zoom + 0.5));
		}
	}
	finalSize.cx = max(1, min(65535, finalSize.cx));
	finalSize.cy = max(1, min(65535, finalSize.cy));
	resampleSize = finalSize;
	if (s->rotation == 90 || s->rotation == 270) {
		resampleSize = CSize(finalSize.cy, finalSize.cx);
	}
}

static void LateResampleCallback(void* user, const unsigned char* src, int srcW, int srcH) {
	LateResampleCtx* s = static_cast<LateResampleCtx*>(user);
	if (s == nullptr || s->pDIB != nullptr || srcW <= 0 || srcH <= 0 || src == nullptr) {
		return;
	}

	CSize finalSize, resampleSize;
	ComputeResampleGeometry(s, srcW, srcH, finalSize, resampleSize);

	// Resample the fully decoded source directly
	unsigned char* pFinal = static_cast<unsigned char*>(CBasicProcessing::SampleDown_HQ_SIMD(
		CSize(resampleSize.cx, resampleSize.cy), CPoint(0, 0), CSize(resampleSize.cx, resampleSize.cy),
		CSize(srcW, srcH), src, 3, s->sharpen, s->filter, s->simd));
	if (pFinal == nullptr) {
		return;
	}

	if (s->rotation == 90 || s->rotation == 180 || s->rotation == 270) {
		void* pRotated = CBasicProcessing::Rotate32bpp(resampleSize.cx, resampleSize.cy, pFinal, s->rotation);
		if (pRotated != nullptr) {
			delete[] pFinal;
			pFinal = static_cast<unsigned char*>(pRotated);
		}
	}

	s->targetW = finalSize.cx;
	s->targetH = finalSize.cy;
	s->pDIB = pFinal;
}

void* TurboJpeg::ReadImage(int& width,
	int& height,
	int& nchannels,
	TJSAMP& chromoSubsampling,
	bool& outOfMemory,
	const void* buffer,
	int sizebytes,
	TJResampleTarget* pResample)
{
	outOfMemory = false;
	width = height = 0;
	nchannels = 4;
	chromoSubsampling = TJSAMP_420;

	// Fast path: multi-threaded band decode for baseline 8-bit JPEGs
	{
		int nSub = -1;
		LateResampleCtx lr;
		ParallelJPEG::ProgressFn progress = nullptr;
		void* user = nullptr;
		if (pResample != nullptr && pResample->clientWidth > 0 && pResample->clientHeight > 0) {
			lr.clientWidth = pResample->clientWidth;
			lr.clientHeight = pResample->clientHeight;
			lr.zoomMode = pResample->zoomMode;
			lr.zoom = pResample->zoom;
			lr.sharpen = pResample->sharpen;
			lr.filter = pResample->filter;
			lr.simd = ToSIMDArch(CSettingsProvider::This().AlgorithmImplementation());
			lr.rotation = pResample->rotation;
			progress = LateResampleCallback;
			user = &lr;
		}
		unsigned char* pParallel = ParallelJPEG::Decode(buffer, sizebytes, width, height, nSub, progress, user);
		if (pParallel != nullptr) {
			nchannels = 3;
			if (nSub >= 0) chromoSubsampling = static_cast<TJSAMP>(nSub);
			if (progress != nullptr && lr.pDIB != nullptr) {
				pResample->pDIB = lr.pDIB;
				pResample->targetWidth = lr.targetW;
				pResample->targetHeight = lr.targetH;
			}
			return pParallel;
		}
	}

	// Single-threaded fallback
	thread_local tjhandle hDecoder = nullptr;
	if (hDecoder == nullptr) {
		hDecoder = tj3Init(TJINIT_DECOMPRESS);
		if (hDecoder == nullptr) {
			return nullptr;
		}
	}

	unsigned char* pPixelData = nullptr;
	int nResult = tj3DecompressHeader(hDecoder, static_cast<const unsigned char*>(buffer), sizebytes);
	if (nResult == 0) {
		width = tj3Get(hDecoder, TJPARAM_JPEGWIDTH);
		height = tj3Get(hDecoder, TJPARAM_JPEGHEIGHT);
		chromoSubsampling = static_cast<TJSAMP>(tj3Get(hDecoder, TJPARAM_SUBSAMP));
		int colorspace = tj3Get(hDecoder, TJPARAM_COLORSPACE);

		if (abs(static_cast<double>(width) * height) > MAX_IMAGE_PIXELS) {
			outOfMemory = true;
		} else if (width <= MAX_IMAGE_DIMENSION && height <= MAX_IMAGE_DIMENSION && chromoSubsampling != TJSAMP_UNKNOWN) {
			tj3Set(hDecoder, TJPARAM_FASTUPSAMPLE, 1);
			tj3Set(hDecoder, TJPARAM_FASTDCT, 1);
			tj3SetScalingFactor(hDecoder, TJUNSCALED);

			if (colorspace == TJCS_GRAY || chromoSubsampling == TJSAMP_GRAY) {
				nchannels = 1;
				size_t pitch = static_cast<size_t>(TJPAD(width));
				pPixelData = new (std::nothrow) unsigned char[pitch * height];
				if (pPixelData != nullptr) {
					nResult = tj3Decompress8(hDecoder, static_cast<const unsigned char*>(buffer), sizebytes, pPixelData, static_cast<int>(pitch), TJPF_GRAY);
					if (nResult != 0) {
						delete[] pPixelData;
						pPixelData = nullptr;
					}
				} else {
					outOfMemory = true;
				}
			} else {
				nchannels = 3;
				size_t pitch = static_cast<size_t>(TJPAD(width * 3));
				pPixelData = new (std::nothrow) unsigned char[pitch * height];

				if (pPixelData != nullptr) {
					struct jpeg_decompress_struct cinfo;
					struct jpeg_error_mgr jerr;
					cinfo.err = jpeg_std_error(&jerr);
					jpeg_create_decompress(&cinfo);
					jpeg_mem_src(&cinfo, static_cast<const unsigned char*>(buffer), sizebytes);
					jpeg_read_header(&cinfo, TRUE);
					cinfo.out_color_space = JCS_EXT_BGR;
					cinfo.dct_method = JDCT_IFAST;
					cinfo.do_fancy_upsampling = FALSE;
					cinfo.dither_mode = JDITHER_NONE;
					jpeg_start_decompress(&cinfo);

					const int CHUNK_LINES = 128;
					JSAMPROW row_pointers[CHUNK_LINES];
					while (cinfo.output_scanline < cinfo.output_height) {
						int startScanline = cinfo.output_scanline;
						int linesToRead = min(CHUNK_LINES, static_cast<int>(cinfo.output_height - cinfo.output_scanline));
						for (int r = 0; r < linesToRead; r++) {
							row_pointers[r] = reinterpret_cast<JSAMPROW>(pPixelData + static_cast<size_t>(startScanline + r) * pitch);
						}
						jpeg_read_scanlines(&cinfo, row_pointers, linesToRead);
					}
					jpeg_finish_decompress(&cinfo);
					jpeg_destroy_decompress(&cinfo);
				} else {
					outOfMemory = true;
				}
			}
		}
	}

	return pPixelData;
}

void* TurboJpeg::Compress(const void* source,
	int width,
	int height,
	int& len,
	bool& outOfMemory,
	int quality)
{
	outOfMemory = false;
	len = 0;
	tjhandle hEncoder = tj3Init(TJINIT_COMPRESS);
	if (hEncoder == nullptr) {
		return nullptr;
	}

	unsigned char* pJPEGCompressed = nullptr;
	size_t nCompressedLen = 0;
	tj3Set(hEncoder, TJPARAM_SUBSAMP, TJSAMP_420);
	tj3Set(hEncoder, TJPARAM_QUALITY, quality);
	int nResult = tj3Compress8(hEncoder, static_cast<const unsigned char*>(source), width, TJPAD(width * 3), height, TJPF_BGR,
		&pJPEGCompressed, &nCompressedLen);
	if (nResult != 0 || nCompressedLen > INT_MAX) {
		if (pJPEGCompressed == nullptr) {
			outOfMemory = true;
		}
		Free(pJPEGCompressed);
		pJPEGCompressed = nullptr;
	}

	len = static_cast<int>(nCompressedLen);
	tj3Destroy(hEncoder);
	return pJPEGCompressed;
}

void TurboJpeg::Free(void* buffer) {
	tj3Free(buffer);
}