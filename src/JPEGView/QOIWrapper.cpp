#include "stdafx.h"

#include "QOIWrapper.h"
#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "qoi/qoi.h"
#include "MaxImageDef.h"
#include "Helpers.h"

#define LINEAR_TO_SRGB(rgb) ((rgb) > 0 ? ((rgb) < 255 ? (255.0 * (1.055 * pow((rgb)/255.0, 1.0/2.4) - 0.055)) : 255) : 0)

void* QoiReaderWriter::ReadImage(int& width,
	int& height,
	int& nchannels,
	bool& outOfMemory,
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;

	qoi_desc desc = { 0 };
	unsigned char* pDecodedPixels = (unsigned char*)qoi_decode(buffer, sizebytes, &desc, 0);
	if (pDecodedPixels == NULL)
		return NULL;
	width = desc.width;
	height = desc.height;
	nchannels = desc.channels;
	if (abs((double)width * height) > MAX_IMAGE_PIXELS)
		outOfMemory = true;
	if (outOfMemory || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
		QOI_FREE(pDecodedPixels);
		return NULL;
	}
	int decoded_stride = width * nchannels;
	int padded_stride = Helpers::DoPadding(decoded_stride, 4);
	unsigned char* pPixelData = new(std::nothrow) unsigned char[padded_stride * height];
	if (pPixelData != NULL) {
		// Copy from RGB(A) to BGR(A) using efficient scanline indexing
		for (int y = 0; y < height; ++y) {
			const unsigned char* pSrcRow = pDecodedPixels + y * decoded_stride;
			unsigned char* pDstRow = pPixelData + y * padded_stride;

			if (desc.colorspace == QOI_LINEAR) {
				for (int x = 0; x < width; ++x) {
					int srcIdx = x * nchannels;
					int dstIdx = x * nchannels;
					pDstRow[dstIdx    ] = (unsigned char)LINEAR_TO_SRGB(pSrcRow[srcIdx + 2]);
					pDstRow[dstIdx + 1] = (unsigned char)LINEAR_TO_SRGB(pSrcRow[srcIdx + 1]);
					pDstRow[dstIdx + 2] = (unsigned char)LINEAR_TO_SRGB(pSrcRow[srcIdx    ]);
					if (nchannels == 4)
						pDstRow[dstIdx + 3] = pSrcRow[srcIdx + 3];
				}
			} else {
				for (int x = 0; x < width; ++x) {
					int srcIdx = x * nchannels;
					int dstIdx = x * nchannels;
					pDstRow[dstIdx    ] = pSrcRow[srcIdx + 2];
					pDstRow[dstIdx + 1] = pSrcRow[srcIdx + 1];
					pDstRow[dstIdx + 2] = pSrcRow[srcIdx    ];
					if (nchannels == 4)
						pDstRow[dstIdx + 3] = pSrcRow[srcIdx + 3];
				}
			}
		}
	} else {
		outOfMemory = true;
	}
	QOI_FREE(pDecodedPixels);
	return (void*)pPixelData;
}

void* QoiReaderWriter::Compress(const void* source,
	int width,
	int height,
	int& len) {

	int nchannels = 3;
	void* pOutput = NULL;

	qoi_desc desc;
	desc.width = width;
	desc.height = height;
	desc.channels = nchannels;
	desc.colorspace = QOI_SRGB;
	int input_stride = width * nchannels;
	int padded_stride = Helpers::DoPadding(input_stride, 4);
	unsigned char* pPixelData = new(std::nothrow) unsigned char[input_stride * height];
	const unsigned char* pSourcePixels = (const unsigned char*)source;
	if (pPixelData != NULL) {
		// Copy from BGR to RGB using efficient scanline indexing
		for (int y = 0; y < height; ++y) {
			const unsigned char* pSrcRow = pSourcePixels + y * padded_stride;
			unsigned char* pDstRow = pPixelData + y * input_stride;
			for (int x = 0; x < width; ++x) {
				int idx = x * nchannels;
				pDstRow[idx    ] = pSrcRow[idx + 2];
				pDstRow[idx + 1] = pSrcRow[idx + 1];
				pDstRow[idx + 2] = pSrcRow[idx    ];
			}
		}
		pOutput = qoi_encode(pPixelData, &desc, &len);
		delete[] pPixelData;
	}
	return pOutput;
}

void QoiReaderWriter::FreeMemory(void* pointer) {
	QOI_FREE(pointer);
}
