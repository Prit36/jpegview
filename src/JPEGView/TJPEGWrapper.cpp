
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
};

static void LateResampleCallback(void* user, const unsigned char* src, int srcW, int srcH) {
	LateResampleCtx* s = (LateResampleCtx*)user;
	if (s->started) return;
	s->started = true;
	if (srcW <= 0 || srcH <= 0 || src == NULL) return;
	// Compute the fit size and allocate the display DIB.
	CSize t;
	if (s->zoom < 0.0) {
		t = Helpers::GetImageRect(srcW, srcH, s->clientWidth, s->clientHeight, s->zoomMode, s->zoom);
	} else {
		t = CSize((int)(srcW * s->zoom + 0.5), (int)(srcH * s->zoom + 0.5));
	}
	t.cx = max(1, min(65535, t.cx)); t.cy = max(1, min(65535, t.cy));
	size_t rowBytes = (size_t)t.cx * 4;
	unsigned char* d = new (std::nothrow) unsigned char[rowBytes * t.cy];
	if (d == NULL) return;
	s->targetW = t.cx; s->targetH = t.cy;
	s->pDIB = d;
	// Resample the whole image in one pool call (the decode threads are done,
	// so the pool has the machine to itself).
	unsigned char* pOut = (unsigned char*)CBasicProcessing::SampleDown_HQ_SIMD(
		CSize(t.cx, t.cy), CPoint(0, 0), CSize(t.cx, t.cy),
		CSize(srcW, srcH), src, 3, s->sharpen, s->filter, s->simd);
	if (pOut != NULL) {
		delete[] s->pDIB;
		s->pDIB = pOut;
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
		memset(&lr, 0, sizeof(lr));
		ParallelJPEG::ProgressFn progress = NULL;
		void* user = NULL;
		if (pResample != NULL && pResample->clientWidth > 0 && pResample->clientHeight > 0) {
			lr.clientWidth = pResample->clientWidth;
			lr.clientHeight = pResample->clientHeight;
			lr.zoomMode = pResample->zoomMode;
			lr.zoom = pResample->zoom;
			lr.sharpen = pResample->sharpen;
			lr.filter = pResample->filter;
			lr.simd = ToSIMDArch(CSettingsProvider::This().AlgorithmImplementation());
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
