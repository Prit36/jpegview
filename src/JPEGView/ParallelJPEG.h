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