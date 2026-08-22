// Parallel baseline-JPEG decoder using libjpeg-turbo internals.
// Decodes entropy-coded MCU bands concurrently with a pipelined prescan that
// captures per-row decoder state (bit position, DC values). Falls back to the
// caller's single-threaded path on any non-baseline feature or failure.
#pragma once

namespace ParallelJPEG {

	// Optional callback invoked on the calling (load) thread once the whole
	// image is decoded (all bands done). Receives the decoded BGR-24 source and
	// its dimensions. Used to start the display resample at that point, so the
	// resample overlaps the decode's join/return overhead instead of running
	// after the whole decode. May be NULL.
	typedef void(*ProgressFn)(void* user, const unsigned char* src, int srcW, int srcH);

	// Per-component coefficient planes emitted by the speculative walk (one
	// 8x8 block of 16-bit coefficients per entry, natural raster order).
	// Bit-identical to libjpeg's jpeg_read_coefficients() output.
	// (short matches JCOEF; 4 = MAX_COMPS_IN_SCAN - kept libjpeg-free here.)
	struct CoeffPlanes {
		short* plane[4];
		int stride[4]; // blocks per row
		int rows[4];   // block rows
		CoeffPlanes() { for (int i = 0; i < 4; i++) { plane[i] = NULL; stride[i] = 0; rows[i] = 0; } }
	};

	// Decode a baseline, 8-bit, 3-component JPEG in parallel.
	// On success returns a new[] BGR-24 buffer with pitch = TJPAD(width*3);
	// width, height and subsampling are set (subsampling matches the TJSAMP
	// enum: 0=444, 1=422, 2=420, 4=440, 5=411). On any failure (non-baseline,
	// progressive, arithmetic, restart markers, malformed data, OOM) returns
	// NULL and the caller must fall back to single-threaded decoding.
	// progress/user: if non-NULL, progress(user, out, width, height) is called
	// once all bands are decoded; the callback runs on the calling thread and
	// must be reentrant (it may call thread-pool resample work).
	unsigned char* Decode(const void* buffer, int sizebytes, int& width, int& height, int& subsampling,
		ProgressFn progress = NULL, void* user = NULL);

	// Fast header-only check: true if this file is worth attempting Decode().
	bool IsParallelDecodable(const void* buffer, int sizebytes, int& width, int& height);
}