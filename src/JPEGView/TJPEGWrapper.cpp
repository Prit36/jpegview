
#include "stdafx.h"
#include "TJPEGWrapper.h"
#include "libjpeg-turbo\include\turbojpeg.h"
#include "libjpeg-turbo\include\jpeglib.h"
#include "libjpeg-turbo\include\jerror.h"
#include "MaxImageDef.h"
#include "Helpers.h"
#include <vector>
#include <thread>

void * TurboJpeg::ReadImage(int &width,
					   int &height,
					   int &nchannels,
					   TJSAMP &chromoSubsampling,
					   bool &outOfMemory,
					   const void *buffer,
					   int sizebytes)
{
	outOfMemory = false;
	width = height = 0;
	nchannels = 4;
	chromoSubsampling = TJSAMP_420;

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
