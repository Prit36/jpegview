// FastPng.cpp - accelerated PNG decode path (libdeflate inflate + SIMD unfilter)
//
// Why this is much faster than the libpng path for large photos:
//  1. libdeflate inflates ~3x faster than vanilla zlib (measured: 62 ms vs
//     ~155-185 ms for a 15.6 MB / 18.7 MP PNG on an i5-12400F).
//  2. The per-pixel filter reconstruction runs with SSE/AVX2 intrinsics using
//     the proven register-carry technique, instead of libpng's byte-at-a-time
//     scalar loops.
//  3. Decoding writes straight into the single final BGRA buffer - the old path
//     allocated p_image + p_frame + p_temp + pixels (~300 MB of transient
//     buffers and two extra full-image memcpys for a 5760x3240 image).
//
// Only the common "photo" subset of PNG is handled here; anything else returns
// -1 and the caller falls back to libpng:
//   - 8-bit depth, non-interlaced, color types 0/2/4/6
//   - no tRNS (transparency key), no acTL (animation), no interlace
#include "FastPng.h"

#include <libdeflate.h>
#include <intrin.h>
#include <immintrin.h>
#include <cstring>
#include <vector>

namespace {

inline int PaethScalar(int a, int b, int c) {
	// Branchless: filtered data is effectively random, so the naive 2-level
	// branch here mispredicted ~20 cycles/byte (measured). Arithmetic select
	// lets the compiler emit conditional moves instead.
	int p = a + b - c;
	int da = p - a; int pa = da < 0 ? -da : da;
	int db = p - b; int pb = db < 0 ? -db : db;
	int dc = p - c; int pc = dc < 0 ? -dc : dc;
	int useA = (pa <= pb) & (pa <= pc);
	int useB = (1 - useA) & (pb <= pc);
	return useA * a + useB * b + (1 - useA - useB) * c;
}

// Paeth predictor computed in 16-bit lanes.
// IMPORTANT: the PNG spec evaluates p = a+b-c and the |p-x| distances with
// plain signed integer arithmetic (p may be negative, values are NOT reduced
// mod 256). Byte-lane math wraps mod 256 and silently selects the wrong
// predictor whenever the true |p-x| exceeds 127 - verified against libpng -
// hence the 16-bit expansion here.
inline __m128i PaethPixel16(__m128i a, __m128i b, __m128i c) {
	const __m128i zero = _mm_setzero_si128();
	const __m128i ones = _mm_set1_epi16(-1);
	__m128i aw = _mm_unpacklo_epi8(a, zero);
	__m128i bw = _mm_unpacklo_epi8(b, zero);
	__m128i cw = _mm_unpacklo_epi8(c, zero);
	__m128i p = _mm_sub_epi16(_mm_add_epi16(aw, bw), cw);
	__m128i pa = _mm_abs_epi16(_mm_sub_epi16(p, aw));
	__m128i pb = _mm_abs_epi16(_mm_sub_epi16(p, bw));
	__m128i pc = _mm_abs_epi16(_mm_sub_epi16(p, cw));
	__m128i sel_a = _mm_xor_si128(_mm_or_si128(_mm_cmpgt_epi16(pa, pb), _mm_cmpgt_epi16(pa, pc)), ones);
	__m128i sel_b = _mm_andnot_si128(sel_a, _mm_xor_si128(_mm_cmpgt_epi16(pb, pc), ones));
	__m128i resw = _mm_blendv_epi8(cw, bw, sel_b);
	resw = _mm_blendv_epi8(resw, aw, sel_a);
	return _mm_packus_epi16(resw, zero);
}

inline __m128i Load4(const void* p) { int t; memcpy(&t, p, 4); return _mm_cvtsi32_si128(t); }
inline void Store4(void* p, __m128i v) { int t = _mm_cvtsi128_si32(v); memcpy(p, &t, 4); }
inline __m128i Load3(const void* p) { int t = 0; memcpy(&t, p, 3); return _mm_cvtsi32_si128(t); }
inline void Store3(void* p, __m128i v) { int t = _mm_cvtsi128_si32(v); memcpy(p, &t, 3); }

// Unfilters one scanline in place. 'prev' is the already-reconstructed row
// above (nullptr on the first row). For Sub/Paeth the left neighbour is the
// *reconstructed* previous pixel, so it is carried register-to-register
// (one pixel per SIMD op, bpp bytes wide).
void UnfilterRow(unsigned char* row, const unsigned char* prev, size_t stride, int bpp, unsigned char ft,
                 unsigned char* rout = nullptr, int xmode = 0) {
	// xmode: 0 = no colour transform emitted, 6 = RGBA->BGRA (swap R/B),
	//        2 = RGB->BGRA (+ opaque alpha). When rout != nullptr the
	//        transformed pixels are emitted here while unfiltering, which
	// avoids a second pass over the image.
	const __m128i zero = _mm_setzero_si128();
	const __m128i swapRB = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
	const __m128i rgbToBgraMask = _mm_setr_epi8(2, 1, 0, (char)-1, 6, 5, 4, (char)-1, 10, 9, 8, (char)-1, 14, 13, 12, (char)-1);
	const __m128i alphaFF = _mm_setr_epi8(0, 0, 0, (char)-1, 0, 0, 0, (char)-1, 0, 0, 0, (char)-1, 0, 0, 0, (char)-1);

	if (bpp == 4) {
		// SIMD path: one pixel per op with the reconstructed left neighbour
		// carried register-to-register. 4 bytes/pixel amortizes the Paeth
		// pipeline well; measured much faster than scalar.
		size_t i = 0;
		size_t ro = 0;
		__m128i carry = zero;
		while (i + 4 <= stride) {
			__m128i d = Load4(row + i);
			__m128i a = carry;
			__m128i b = prev ? Load4(prev + i) : zero;
			__m128i c = (prev && i >= 4) ? Load4(prev + i - 4) : zero;
			__m128i r;
			if (ft == 0) r = d;
			else if (ft == 1) r = _mm_add_epi8(d, a);
			else if (ft == 2) r = _mm_add_epi8(d, b);
			else if (ft == 3) {
				__m128i avg = _mm_avg_epu8(a, b);
				__m128i odd = _mm_and_si128(_mm_xor_si128(a, b), _mm_set1_epi8(1));
				r = _mm_add_epi8(d, _mm_sub_epi8(avg, odd)); // exact floor((a+b)/2)
			}
			else r = _mm_add_epi8(d, PaethPixel16(a, b, c));
			Store4(row + i, r);
			if (rout) {
				if (xmode == 6) Store4(rout + ro, _mm_shuffle_epi8(r, swapRB));
				else           Store4(rout + ro, _mm_add_epi8(_mm_shuffle_epi8(r, rgbToBgraMask), alphaFF));
			}
			carry = r;
			i += 4;
			ro += 4;
		}
		for (; i < stride; i++) {
			int a = (i >= bpp) ? row[i - bpp] : 0;
			int b = prev ? prev[i] : 0;
			int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
			if (ft == 1) row[i] = (unsigned char)(row[i] + a);
			else if (ft == 2) row[i] = (unsigned char)(row[i] + b);
			else if (ft == 3) row[i] = (unsigned char)(row[i] + (unsigned char)((a + b) / 2));
			else row[i] = (unsigned char)(row[i] + PaethScalar(a, b, c));
		}
		return;
	}

	if (bpp == 3 && rout != nullptr) {
		// Scalar byte loop for RGB with fused BGRA emit. The 16-bit Paeth
		// SIMD pipeline costs more than it saves at only 3 bytes/pixel
		// (measured ~2.5x slower than this plain loop), so keep it simple -
		// the win for RGB comes from libdeflate inflate + the fused emit.
		size_t ro = 0;
		for (size_t i = 0; i + 3 <= stride; i += 3, ro += 4) {
			for (int k = 0; k < 3; k++) {
				int a = (i + k >= 3) ? row[i + k - 3] : 0;
				int b = prev ? prev[i + k] : 0;
				int c = (prev && i + k >= 3) ? prev[i + k - 3] : 0;
				int p;
				if (ft == 1) p = a;
				else if (ft == 2) p = b;
				else if (ft == 3) p = (a + b) / 2;
				else p = PaethScalar(a, b, c);
				row[i + k] = (unsigned char)(row[i + k] + p);
			}
			rout[ro]     = row[i + 2]; // B
			rout[ro + 1] = row[i + 1]; // G
			rout[ro + 2] = row[i];     // R
			rout[ro + 3] = 255;
		}
		return;
	}

	if (bpp == 3) {
		// no emit requested: plain scalar loop
		for (size_t i = 0; i + 3 <= stride; i += 3) {
			for (int k = 0; k < 3; k++) {
				int a = (i + k >= 3) ? row[i + k - 3] : 0;
				int b = prev ? prev[i + k] : 0;
				int c = (prev && i + k >= 3) ? prev[i + k - 3] : 0;
				int p;
				if (ft == 1) p = a;
				else if (ft == 2) p = b;
				else if (ft == 3) p = (a + b) / 2;
				else p = PaethScalar(a, b, c);
				row[i + k] = (unsigned char)(row[i + k] + p);
			}
		}
		return;
	}

	// scalar fallback (bpp 1/2: grayscale variants, rare for photos)
	if (ft == 1) { for (size_t i = bpp; i < stride; i++) row[i] = (unsigned char)(row[i] + row[i - bpp]); }
	else if (ft == 2) { for (size_t i = 0; i < stride; i++) row[i] = (unsigned char)(row[i] + (prev ? prev[i] : 0)); }
	else if (ft == 3) { for (size_t i = 0; i < stride; i++) { int a = prev ? prev[i] : 0; int b = (i >= bpp) ? row[i - bpp] : 0; row[i] = (unsigned char)(row[i] + (unsigned char)((a + b) / 2)); } }
	else if (ft == 4) { for (size_t i = 0; i < stride; i++) { int a = (i >= bpp) ? row[i - bpp] : 0; int b = prev ? prev[i] : 0; int c = (prev && i >= bpp) ? prev[i - bpp] : 0; row[i] = (unsigned char)(row[i] + PaethScalar(a, b, c)); } }
}

} // namespace

int FastPngDecode(const unsigned char* file, size_t size, FastPngImage& out) {
	static const unsigned char kSig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	if (size < 33 || memcmp(file, kSig, 8) != 0) return -1;

	unsigned int w = 0, h = 0;
	int bitDepth = 0, colorType = 0, interlace = 0;
	bool haveIHDR = false, hasTRNS = false, hasACTL = false;
	std::vector<unsigned char> idat;
	const unsigned char* exif_data = nullptr;
	unsigned int exif_len = 0;

	size_t off = 8;
	// pre-count IDAT payload so the vector allocates exactly once
	for (size_t s = 8; s + 8 <= size; ) {
		unsigned int l = _byteswap_ulong(*(const unsigned int*)(file + s));
		if (memcmp(file + s + 4, "IDAT", 4) == 0) idat.reserve(idat.size() + l);
		if (memcmp(file + s + 4, "IEND", 4) == 0) break;
		s += 12 + (size_t)l;
	}
	while (off + 8 <= size) {
		unsigned int len = _byteswap_ulong(*(const unsigned int*)(file + off));
		const char* type = (const char*)(file + off + 4);
		if (memcmp(type, "IHDR", 4) == 0) {
			w = _byteswap_ulong(*(const unsigned int*)(file + off + 8));
			h = _byteswap_ulong(*(const unsigned int*)(file + off + 12));
			bitDepth = file[off + 16];
			colorType = file[off + 17];
			interlace = file[off + 19];
			haveIHDR = true;
		} else if (memcmp(type, "acTL", 4) == 0) { hasACTL = true; break; }
		else if (memcmp(type, "tRNS", 4) == 0) { hasTRNS = true; }
		else if (memcmp(type, "eXIf", 4) == 0) {
			if (off + 12 + (size_t)len <= size && len > 0 && len < 65528) { exif_data = file + off + 8; exif_len = len; }
		} else if (memcmp(type, "IDAT", 4) == 0) {
			if (off + 12 + (size_t)len > size) break;
			idat.insert(idat.end(), file + off + 8, file + off + 8 + len);
		} else if (memcmp(type, "IEND", 4) == 0) { break; }
		off += 12 + (size_t)len;
	}
	if (!haveIHDR || idat.empty()) return -1;
	if (bitDepth != 8 || interlace != 0) return -1;      // only 8-bit, no ADAM7
	if (hasACTL || hasTRNS) return -1;                   // animation / transparency key -> libpng
	int compIn = (colorType == 0) ? 1 : (colorType == 2) ? 3 : (colorType == 4) ? 2 : (colorType == 6) ? 4 : -1;
	if (compIn < 0) return -1;                           // palette & exotic -> libpng

	size_t stride = (size_t)w * compIn;
	size_t rawSize = (size_t)h * (1 + stride);
	unsigned char* raw = (unsigned char*)malloc(rawSize);
	if (!raw) return -1;

	struct libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
	size_t outN = 0;
	enum libdeflate_result r = libdeflate_zlib_decompress(dec, idat.data(), idat.size(), raw, rawSize, &outN);
	libdeflate_free_decompressor(dec);
	if (r != LIBDEFLATE_SUCCESS || outN != rawSize) { free(raw); return -1; }

	unsigned char* pixels = (unsigned char*)malloc((size_t)w * h * 4);
	if (!pixels) { free(raw); return -1; }

	const unsigned char* prev = nullptr;
	// ct6/ct2 emit BGRA directly from the unfilter loop (fused, no second pass)
	int xmode = (colorType == 6 || colorType == 2) ? colorType : 0;
	bool needPostTransform = (xmode == 0);
	for (unsigned int y = 0; y < h; y++) {
		unsigned char* rin = raw + y * (1 + stride) + 1;
		unsigned char* rout = pixels + (size_t)y * 4 * w;
		unsigned char ft = raw[y * (1 + stride)];
		UnfilterRow(rin, prev, stride, compIn, ft, needPostTransform ? nullptr : rout, xmode);
		if (needPostTransform) {
			// gray variants (rare): expand in place
			if (colorType == 0) {
				for (unsigned int x = 0; x < w; x++) { unsigned char g = rin[x]; rout[x*4]=g; rout[x*4+1]=g; rout[x*4+2]=g; rout[x*4+3]=255; }
			} else { // colorType == 4
				for (unsigned int x = 0; x < w; x++) { unsigned char g = rin[x*2], a = rin[x*2+1]; rout[x*4]=g; rout[x*4+1]=g; rout[x*4+2]=g; rout[x*4+3]=a; }
			}
		}
		prev = rin;
	}
	free(raw);

	out.width = (int)w;
	out.height = (int)h;
	out.pixels = pixels;
	out.exif_payload = nullptr;
	out.exif_size = 0;
	if (exif_data && exif_len > 8) {
		void* copy = malloc(exif_len);
		if (copy) { memcpy(copy, exif_data, exif_len); out.exif_payload = copy; out.exif_size = exif_len; }
	}
	return 0;
}
