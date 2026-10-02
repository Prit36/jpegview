#pragma once

#include <cstdint>
#include <cstddef>

class GpuHeifDecoder
{
public:
	// Attempts to decode a HEIF/HEIC image using GPU hardware acceleration.
	// Returns true on success, filling width, height, bpp, pPixelData (4-byte BGRA),
	// exif_chunk (must be freed with free() by caller if non-null), and has_alpha.
	// Returns false if GPU decoding is not available or if the format is unsupported,
	// allowing fallback to CPU software decoding.
	static bool DecodeHeif(
		const void* buffer,
		size_t sizeBytes,
		int frameIndex,
		int& width,
		int& height,
		int& bpp,
		int& frameCount,
		void*& pPixelData,
		void*& exif_chunk,
		bool& hasAlpha,
		bool& outOfMemory
	);

	// Checks if hardware HEVC decoding is supported on this system
	static bool IsHardwareSupported();

	// The first HEIF image is decoded in software while the GPU decoder is cold.
	// Initialize it in the background afterwards so later images can use it.
	static void StartHardwareInitialization();
	static void WaitForHardwareInitialization();
	static bool IsHardwareReady();
};
