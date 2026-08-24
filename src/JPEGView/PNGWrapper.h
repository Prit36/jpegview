#pragma once

class PngReader
{
public:
	// Returns data in 4 byte BGRA. Stateful for animated PNGs: the first call
	// must pass the file bytes; subsequent calls for the same animated file pass
	// buffer = NULL / sizebytes = 0 to receive the next frame (wraps around).
	static void* ReadImage(int& width,
		int& height,
		int& bpp,                 // BYTES (not bits) PER PIXEL - always 4 (BGRA)
		bool& has_animation,
		int& frame_count,
		int& frame_time,          // frame duration in milliseconds
		void*& exif_chunk,        // Pointer to Exif data (must be freed by caller)
		bool& outOfMemory,        // set to true when no memory to read image
		void* buffer,             // memory address containing png compressed data
		size_t sizebytes);

	static void DeleteCache();

	// True when this PNG must be handled by our internal decoder rather than
	// GDI+ (huge images GDI+ cannot process, and APNGs GDI+ does not support).
	static bool MustUseInternalDecoder(const void* buffer, size_t sizebytes);

	// Get EXIF Block
	static void* GetEXIFBlock(void* buffer, size_t sizebytes);
};
