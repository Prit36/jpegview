
#include "stdafx.h"
#include "TJPEGWrapper.h"
#include "ParallelJPEG.h"
#include "libjpeg-turbo\include\turbojpeg.h"
#include "libjpeg-turbo\include\jpeglib.h"
#include "libjpeg-turbo\include\jerror.h"
#include "MaxImageDef.h"
#include "Helpers.h"
#include "BasicProcessing.h"
#include "SettingsProvider.h"
#include <vector>
#include <thread>
#include <cstdarg>

// Guard the parallel decoder: on any structured exception, fall back to the
// single-threaded path below instead of crashing the app.
static int ParallelJPEGSEHFilter(EXCEPTION_POINTERS*) {
	return EXCEPTION_EXECUTE_HANDLER;
}

static unsigned char* SafeParallelDecode(const void* buffer, int sizebytes, int& width, int& height, int& nSub,
	const ParallelJPEG::ProgressFn& progress, void* user) {
	__try {
		return ParallelJPEG::Decode(buffer, sizebytes, width, height, nSub, progress, user);
	}
	__except (ParallelJPEGSEHFilter(GetExceptionInformation())) {
		return NULL;
	}
}

static CBasicProcessing::SIMDArchitecture ToSIMDArch(Helpers::CPUType cpuType) {
	switch (cpuType) {
	case Helpers::CPU_MMX: return CBasicProcessing::MMX;
	case Helpers::CPU_SSE: return CBasicProcessing::SSE;
	case Helpers::CPU_AVX2: return CBasicProcessing::AVX2;
	default: return CBasicProcessing::SSE;
	}
}

// Diagnostics for the pipelined resample (enabled with JPEGVIEW_RESAMPLE_LOG=1).
static void LogResampleDiag(const char* fmt, ...);

// Late-start resample state: the decode's progress callback fires when the last
// band completes (whole source decoded); at that point we run the full display
// resample on the load thread's pool, overlapping the decode's join/return
// overhead instead of running after ReadImage returns.
struct LateResampleCtx {
	int clientWidth, clientHeight;
	Helpers::EAutoZoomMode zoomMode;
	double zoom;
	double sharpen;
	EFilterType filter;
	CBasicProcessing::SIMDArchitecture simd;
	unsigned char* pDIB;
	int targetW, targetH;
	bool started;
	int rotation; // 0,90,180,270 - EXIF auto-rotate to handle

	LateResampleCtx() : clientWidth(0), clientHeight(0), zoomMode(Helpers::ZM_FitToScreenNoZoom),
		zoom(-1.0), sharpen(0.0), filter(Filter_Downsampling_Narrow), simd(CBasicProcessing::SSE),
		pDIB(NULL), targetW(0), targetH(0), started(false), rotation(0) {}
};

// Compute the final display size and the (possibly transposed) resample size.
static void ComputeResampleGeometry(LateResampleCtx* s, int srcW, int srcH, CSize& finalSize, CSize& resampleSize) {
	if (s->zoom < 0.0) {
		if ((s->rotation == 90 || s->rotation == 270) && srcW != srcH) {
			finalSize = Helpers::GetImageRect(srcH, srcW, s->clientWidth, s->clientHeight, s->zoomMode, s->zoom);
		} else {
			finalSize = Helpers::GetImageRect(srcW, srcH, s->clientWidth, s->clientHeight, s->zoomMode, s->zoom);
		}
	} else {
		if (s->rotation == 90 || s->rotation == 270) {
			finalSize = CSize((int)(srcH * s->zoom + 0.5), (int)(srcW * s->zoom + 0.5));
		} else {
			finalSize = CSize((int)(srcW * s->zoom + 0.5), (int)(srcH * s->zoom + 0.5));
		}
	}
	finalSize.cx = max(1, min(65535, finalSize.cx)); finalSize.cy = max(1, min(65535, finalSize.cy));
	resampleSize = finalSize;
	if (s->rotation == 90 || s->rotation == 270) {
		resampleSize = CSize(finalSize.cy, finalSize.cx);
	}
}

// Diagnostics for the late-start resample (enabled with JPEGVIEW_RESAMPLE_LOG=1).
static void LogResampleDiag(const char* fmt, ...) {
	static const bool sbEnabled = getenv("JPEGVIEW_RESAMPLE_LOG") != NULL;
	if (!sbEnabled) return;
	FILE* fL = NULL;
	char pLog[MAX_PATH];
	GetTempPathA(MAX_PATH, pLog);
	strcat_s(pLog, "jpgv_resample.log");
	if (fopen_s(&fL, pLog, "a") == 0 && fL != NULL) {
		va_list args;
		va_start(args, fmt);
		vfprintf(fL, fmt, args);
		va_end(args);
		fclose(fL);
	}
}

static void LateResampleCallback(void* user, const unsigned char* src, int srcW, int srcH) {
	LateResampleCtx* s = (LateResampleCtx*)user;
	if (s->pDIB != NULL) return; // already finalized
	double tCB0 = (getenv("JPEGVIEW_PJ_PROF") != NULL) ? Helpers::GetExactTickCount() : 0.0;
	if (srcW <= 0 || srcH <= 0 || src == NULL) {
		return;
	}

	CSize finalSize, resampleSize;
	ComputeResampleGeometry(s, srcW, srcH, finalSize, resampleSize);

	bool needSmallRotate = false;
	if (s->rotation == 90 || s->rotation == 270) {
		needSmallRotate = true;
	} else if (s->rotation == 180) {
		needSmallRotate = true;
	}

	// One-shot resample of the fully decoded source.
	unsigned char* pFinal = (unsigned char*)CBasicProcessing::SampleDown_HQ_SIMD(
		CSize(resampleSize.cx, resampleSize.cy), CPoint(0, 0), CSize(resampleSize.cx, resampleSize.cy),
		CSize(srcW, srcH), src, 3, s->sharpen, s->filter, s->simd);
	if (pFinal == NULL) {
		return;
	}

	if (needSmallRotate) {
		// Small rotate of the already downsampled image (0.78MP vs 44MP)
		void* pRotated = CBasicProcessing::Rotate32bpp(resampleSize.cx, resampleSize.cy, pFinal, s->rotation);
		if (pRotated != NULL) {
			delete[] pFinal;
			pFinal = (unsigned char*)pRotated;
		} else {
			// Fallback: keep resampled as is (will be slightly wrong orientation but better than failure)
			needSmallRotate = false;
		}
	}

	s->targetW = finalSize.cx;
	s->targetH = finalSize.cy;
	s->pDIB = pFinal;
	if (tCB0 > 0.0) {
		double tCB1 = Helpers::GetExactTickCount();
		LogResampleDiag("final %.2f ms (%dx%d -> %dx%d)\n", tCB1 - tCB0, srcW, srcH, finalSize.cx, finalSize.cy);
	}
}

void * TurboJpeg::ReadImage(int &width,
					   int &height,
					   int &nchannels,
					   TJSAMP &chromoSubsampling,
					   bool &outOfMemory,
					   const void *buffer,
					   int sizebytes,
					   TJResampleTarget* pResample)
{
	outOfMemory = false;
	width = height = 0;
	nchannels = 4;
	chromoSubsampling = TJSAMP_420;

	// Fast path: parallel band decode for baseline 8-bit JPEGs. Falls back to
	// the single-threaded decode below for anything else.
	{
		int nSub = -1;
		LateResampleCtx lr;
		ParallelJPEG::ProgressFn progress = NULL;
		void* user = NULL;
		if (pResample != NULL && pResample->clientWidth > 0 && pResample->clientHeight > 0
			&& getenv("JPEGVIEW_NO_LATE_RESAMPLE") == NULL) {
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
		unsigned char* pParallel = SafeParallelDecode(buffer, sizebytes, width, height, nSub, progress, user);
		if (pParallel != NULL) {
			nchannels = 3;
			if (nSub >= 0) chromoSubsampling = (TJSAMP)nSub;
			if (progress != NULL && lr.pDIB != NULL) {
				pResample->pDIB = lr.pDIB;
				pResample->targetWidth = lr.targetW;
				pResample->targetHeight = lr.targetH;
			}
			return pParallel;
		}
		if (getenv("JPEGVIEW_PJ_PROF") != NULL) {
			fprintf(stderr, "[TJ] parallel decode failed/unsupported -> single-thread fallback\n");
		}
	}

	thread_local tjhandle hDecoder = NULL;
	if (hDecoder == NULL) {
		hDecoder = tj3Init(TJINIT_DECOMPRESS);
		if (hDecoder == NULL) {
			return NULL;
		}
	}

	double t0 = Helpers::GetExactTickCount();
	unsigned char* pPixelData = NULL;
	int nResult = tj3DecompressHeader(hDecoder, (unsigned char*)buffer, sizebytes);
	double t_hdr = Helpers::GetExactTickCount();
	if (nResult == 0) {
		width = tj3Get(hDecoder, TJPARAM_JPEGWIDTH);
		height = tj3Get(hDecoder, TJPARAM_JPEGHEIGHT);
		chromoSubsampling = (TJSAMP)tj3Get(hDecoder, TJPARAM_SUBSAMP);
		int colorspace = tj3Get(hDecoder, TJPARAM_COLORSPACE);

		if (abs((double)width * height) > MAX_IMAGE_PIXELS) {
			outOfMemory = true;
		} else if (width <= MAX_IMAGE_DIMENSION && height <= MAX_IMAGE_DIMENSION && chromoSubsampling != TJSAMP_UNKNOWN) {
			// Enable fast chrominance upsampling for maximum decode throughput
			tj3Set(hDecoder, TJPARAM_FASTUPSAMPLE, 1);
			tj3Set(hDecoder, TJPARAM_FASTDCT, 1);
			tj3SetScalingFactor(hDecoder, TJUNSCALED);

			// For grayscale JPEGs, decode to 1 channel
			if (colorspace == TJCS_GRAY || chromoSubsampling == TJSAMP_GRAY) {
				nchannels = 1;
				size_t pitch = (size_t)TJPAD(width);
				pPixelData = new(std::nothrow) unsigned char[pitch * height];
				if (pPixelData != NULL) {
					nResult = tj3Decompress8(hDecoder, (unsigned char*)buffer, sizebytes, pPixelData, (int)pitch, TJPF_GRAY);
					if (nResult != 0) {
						delete[] pPixelData;
						pPixelData = NULL;
					}
				} else {
					outOfMemory = true;
				}
			} else {
				// Direct high-throughput libjpeg-turbo scanline decode (BGR 24-bit)
				nchannels = 3;
				size_t pitch = (size_t)TJPAD(width * 3);
				pPixelData = new(std::nothrow) unsigned char[pitch * height];

				if (pPixelData != NULL) {
					struct jpeg_decompress_struct cinfo;
					struct jpeg_error_mgr jerr;
					cinfo.err = jpeg_std_error(&jerr);
					jpeg_create_decompress(&cinfo);
					jpeg_mem_src(&cinfo, (const unsigned char*)buffer, sizebytes);
					jpeg_read_header(&cinfo, TRUE);
					cinfo.out_color_space = JCS_EXT_BGR;
					cinfo.dct_method = JDCT_IFAST;
					cinfo.do_fancy_upsampling = FALSE;
					cinfo.dither_mode = JDITHER_NONE;
					jpeg_start_decompress(&cinfo);

					const int CHUNK_LINES = 128;
					JSAMPROW row_pointers[CHUNK_LINES];
					double t_scan_start = Helpers::GetExactTickCount();
					while (cinfo.output_scanline < cinfo.output_height) {
						int startScanline = cinfo.output_scanline;
						int linesToRead = min(CHUNK_LINES, (int)(cinfo.output_height - cinfo.output_scanline));
						for (int r = 0; r < linesToRead; r++) {
							row_pointers[r] = (JSAMPROW)(pPixelData + (size_t)(startScanline + r) * pitch);
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

void * TurboJpeg::Compress(const void *source,
					  int width,
					  int height,
					  int &len,
					  bool &outOfMemory,
					  int quality)
{
	outOfMemory = false;
	len = 0;
	tjhandle hEncoder = tj3Init(TJINIT_COMPRESS);
	if (hEncoder == NULL) {
		return NULL;
	}

	unsigned char* pJPEGCompressed = NULL;
	size_t nCompressedLen = 0;
	tj3Set(hEncoder, TJPARAM_SUBSAMP, TJSAMP_420);
	tj3Set(hEncoder, TJPARAM_QUALITY, quality);
	int nResult = tj3Compress8(hEncoder, (unsigned char*)source, width, TJPAD(width * 3), height, TJPF_BGR,
		&pJPEGCompressed, &nCompressedLen);
	if (nResult != 0 || nCompressedLen > INT_MAX) {
		if (pJPEGCompressed == NULL) {
			outOfMemory = true;
		}
		Free(pJPEGCompressed);
		pJPEGCompressed = NULL;
	}

	len = nCompressedLen;

	tj3Destroy(hEncoder);

	return pJPEGCompressed;
}

void TurboJpeg::Free(void* buffer) {
	tj3Free(buffer);
}
