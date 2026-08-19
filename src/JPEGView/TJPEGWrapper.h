
#pragma once

#include "libjpeg-turbo/include/turbojpeg.h"
#include "Helpers.h"

// Optional resample target for the parallel JPEG fast path. When set, ReadImage
// starts the display resample as soon as the decode's final band completes
// (overlapping the decode's join/return overhead) and returns the pre-resampled
// 32bpp top-down BGRX DIB so the caller's GetDIB becomes a cache hit.
struct TJResampleTarget {
	int clientWidth;    // target window / client size (width)
	int clientHeight;   // target window / client size (height)
	Helpers::EAutoZoomMode zoomMode;
	double zoom;        // zoom factor, < 0 = fit
	double sharpen;
	EFilterType filter;
	unsigned char* pDIB; // out (parallel path only): allocated BGRX top-down DIB, caller takes ownership (delete[])
	int targetWidth;    // out: computed fit size
	int targetHeight;   // out: computed fit size
};

class TurboJpeg
{
public:
	// Returns data in the form BGRBGR**********BGR000 where the zeros are padding to 4 byte boundary
	static void * ReadImage(int &width,   // width of the image loaded.
						 int &height,  // height of the image loaded.
						 int &bpp,     // BYTES (not bits) PER PIXEL.
						 TJSAMP &chromoSubsampling, // chromo subsampling of image
						 bool &outOfMemory, // set to true when no memory to read image
						 const void *buffer, // memory address containing jpeg compressed data.
						 int sizebytes, // size of jpeg compressed data.
						 TJResampleTarget* pResample = NULL); // optional late-start resample (parallel path only)

	// Compress image data into JPEG stream, returns compressed data.
	// The returned buffer must be freed with Free()!
	static void * Compress(const void *buffer, // address of image in memory, format must be 3 bytes per pixel BRGBGR with padding to 4 byte boundary
						 int width, // width of image in pixels
						 int height, // height of image in pixels.
						 int &len, // returns length of compressed data
						 bool &outOfMemory, // returns if out of memory
						 int quality=75); // image quality as a percentage

	// Free buffer allocated by Compress
	static void Free(void* buffer);
};
