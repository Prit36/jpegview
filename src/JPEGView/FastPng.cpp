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
// -1 and the caller falls back to GDI+:
//   - 8-bit depth, non-interlaced, color types 0/2/4/6 and 3 (palette 8-bit)
//   - no acTL (animation), no tRNS for non-palette (transparency key)
//   - palette tRNS (per-entry alpha) is handled
#include "FastPng.h"
#include "MaxImageDef.h"

#include <libdeflate.h>
#include <intrin.h>
#include <immintrin.h>
#include <cstring>
#include <memory>

namespace {

inline int PaethScalar(int a, int b, int c) {
	int p = a + b - c;
	int da = p - a; int pa = da < 0 ? -da : da;
	int db = p - b; int pb = db < 0 ? -db : db;
	int dc = p - c; int pc = dc < 0 ? -dc : dc;
	int useA = (pa <= pb) & (pa <= pc);
	int useB = (1 - useA) & (pb <= pc);
	return useA * a + useB * b + (1 - useA - useB) * c;
}

inline __m128i PaethPixel16(__m128i a, __m128i b, __m128i c) {
	const __m128i zero = _mm_setzero_si128();
	__m128i aw = _mm_unpacklo_epi8(a, zero);
	__m128i bw = _mm_unpacklo_epi8(b, zero);
	__m128i cw = _mm_unpacklo_epi8(c, zero);
	// |p-a|=|b-c| and |p-b|=|a-c|. This shortens the serial
	// left-pixel dependency chain; all arithmetic remains signed 16-bit.
	__m128i ac = _mm_sub_epi16(aw, cw);
	__m128i bc = _mm_sub_epi16(bw, cw);
	__m128i pa = _mm_abs_epi16(bc);
	__m128i pb = _mm_abs_epi16(ac);
	__m128i pc = _mm_abs_epi16(_mm_add_epi16(ac, bc));
	__m128i min_bc = _mm_min_epi16(pb, pc);
	__m128i sel_a = _mm_cmpeq_epi16(_mm_min_epi16(pa, min_bc), pa);
	__m128i sel_b = _mm_cmpeq_epi16(min_bc, pb);
	// Apply A last so ties still prefer A, then B, as required by PNG.
	__m128i resw = _mm_blendv_epi8(cw, bw, sel_b);
	resw = _mm_blendv_epi8(resw, aw, sel_a);
	return _mm_packus_epi16(resw, zero);
}

inline __m128i Load4(const void* p) { int t; memcpy(&t, p, 4); return _mm_cvtsi32_si128(t); }
inline void Store4(void* p, __m128i v) { int t = _mm_cvtsi128_si32(v); memcpy(p, &t, 4); }

inline void UnfilterRowNone4_AVX2(unsigned char* row, size_t stride, unsigned char* rout) {
	if (rout) {
		const __m256i shuf = _mm256_setr_epi8(
			2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15,
			2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15);
		size_t i = 0;
		for (; i + 32 <= stride; i += 32) {
			__m256i r = _mm256_loadu_si256((__m256i*)(row + i));
			__m256i out = _mm256_shuffle_epi8(r, shuf);
			_mm256_storeu_si256((__m256i*)(rout + i), out);
		}
		for (; i + 4 <= stride; i += 4) {
			__m128i r = Load4(row + i);
			const __m128i swapRB = _mm_setr_epi8(2,1,0,3,6,5,4,7,10,9,8,11,14,13,12,15);
			Store4(rout + i, _mm_shuffle_epi8(r, swapRB));
		}
	}
}

inline void UnfilterRowUp4_AVX2(unsigned char* row, const unsigned char* prev, size_t stride, unsigned char* rout) {
	const __m256i shuf = _mm256_setr_epi8(
		2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15,
		2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15);
	size_t i = 0;
	if (rout) {
		for (; i + 32 <= stride; i += 32) {
			__m256i d = _mm256_loadu_si256((__m256i*)(row + i));
			__m256i b = _mm256_loadu_si256((__m256i*)(prev + i));
			__m256i r = _mm256_add_epi8(d, b);
			_mm256_storeu_si256((__m256i*)(row + i), r);
			__m256i out = _mm256_shuffle_epi8(r, shuf);
			_mm256_storeu_si256((__m256i*)(rout + i), out);
		}
	} else {
		for (; i + 32 <= stride; i += 32) {
			__m256i d = _mm256_loadu_si256((__m256i*)(row + i));
			__m256i b = _mm256_loadu_si256((__m256i*)(prev + i));
			__m256i r = _mm256_add_epi8(d, b);
			_mm256_storeu_si256((__m256i*)(row + i), r);
		}
	}
	for (; i < stride; i++) {
		int b = prev ? prev[i] : 0;
		row[i] = (unsigned char)(row[i] + b);
		if (rout && i+4 <= stride) {
			// emit remaining pixels scalar after recon
			size_t remaining = stride - i;
			size_t pix = remaining / 4;
			for (size_t p=0;p<pix;p++) {
				size_t off = i + p*4;
				unsigned char r0=row[off], g0=row[off+1], b0=row[off+2], a0=row[off+3];
				rout[off]=b0; rout[off+1]=g0; rout[off+2]=r0; rout[off+3]=a0;
			}
			break;
		}
	}
}

inline void UnfilterRowGeneric4(unsigned char* row, const unsigned char* prev, size_t stride, unsigned char ft, unsigned char* rout, int xmode) {
	const __m128i zero = _mm_setzero_si128();
	const __m128i swapRB = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
	const __m128i rgbToBgraMask = _mm_setr_epi8(2, 1, 0, (char)-1, 6, 5, 4, (char)-1, 10, 9, 8, (char)-1, 14, 13, 12, (char)-1);
	const __m128i alphaFF = _mm_setr_epi8(0, 0, 0, (char)-1, 0, 0, 0, (char)-1, 0, 0, 0, (char)-1, 0, 0, 0, (char)-1);
	size_t i = 0;
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
			r = _mm_add_epi8(d, _mm_sub_epi8(avg, odd));
		} else r = _mm_add_epi8(d, PaethPixel16(a, b, c));
		Store4(row + i, r);
		if (rout) {
			if (xmode == 6) Store4(rout + i, _mm_shuffle_epi8(r, swapRB));
			else Store4(rout + i, _mm_add_epi8(_mm_shuffle_epi8(r, rgbToBgraMask), alphaFF));
		}
		carry = r;
		i += 4;
	}
	for (; i < stride; i++) {
		int a = (i >= 4) ? row[i - 4] : 0;
		int b = prev ? prev[i] : 0;
		int c = (prev && i >= 4) ? prev[i - 4] : 0;
		if (ft == 1) row[i] = (unsigned char)(row[i] + a);
		else if (ft == 2) row[i] = (unsigned char)(row[i] + b);
		else if (ft == 3) row[i] = (unsigned char)(row[i] + (unsigned char)((a + b) / 2));
		else if (ft == 4) row[i] = (unsigned char)(row[i] + PaethScalar(a, b, c));
	}
}

// Reconstruct RGB/RGBA directly as BGRA in the inflate allocation. RGBA
// input starts at byte zero; RGB input is inflated at offset width*height,
// leaving room for the extra alpha bytes. In both cases every output store
// is behind unread input. Predictors are channel-independent, so a BGRA
// previous row and permuted residuals give exactly the original PNG pixels.
void UnfilterColorCompact(const unsigned char* row, const unsigned char* prev,
                          size_t stride, int bpp, unsigned char ft, unsigned char* dst) {
	const __m128i zero = _mm_setzero_si128();
	const __m128i swap = _mm_setr_epi8(2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15);
	size_t i = 0;
	if (bpp == 4 && (ft == 0 || ft == 2)) {
		const __m256i swap256 = _mm256_broadcastsi128_si256(swap);
		for (; i + 32 <= stride; i += 32) {
			__m256i d = _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(row+i)), swap256);
			if (ft == 2 && prev) d = _mm256_add_epi8(d, _mm256_loadu_si256((const __m256i*)(prev+i)));
			_mm256_storeu_si256((__m256i*)(dst+i), d);
		}
	}
	size_t o = i;
	if (bpp == 3 && (ft == 0 || ft == 2)) {
		const __m128i expand = _mm_setr_epi8(2,1,0,(char)-1, 5,4,3,(char)-1,
			8,7,6,(char)-1, 11,10,9,(char)-1);
		const __m128i alpha = _mm_set1_epi32((int)0xff000000);
		for (; i + 16 <= stride; i += 12, o += 16) {
			__m128i d = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(row+i)), expand);
			if (ft == 2 && prev) d = _mm_add_epi8(d, _mm_loadu_si128((const __m128i*)(prev+o)));
			_mm_storeu_si128((__m128i*)(dst+o), _mm_or_si128(d, alpha));
		}
	}
	__m128i a = zero;
	for (; i < stride; i += bpp, o += 4) {
		unsigned int dv;
		if (bpp == 4) memcpy(&dv, row+i, 4);
		else dv = row[i] | (row[i+1] << 8) | (row[i+2] << 16);
		__m128i d = _mm_shuffle_epi8(_mm_cvtsi32_si128((int)dv), swap);
		__m128i b = prev ? Load4(prev+o) : zero;
		__m128i c = (prev && o >= 4) ? Load4(prev+o-4) : zero;
		__m128i r = d;
		if (ft == 1) r = _mm_add_epi8(d, a);
		else if (ft == 2) r = _mm_add_epi8(d, b);
		else if (ft == 3) {
			__m128i avg = _mm_avg_epu8(a, b);
			__m128i odd = _mm_and_si128(_mm_xor_si128(a, b), _mm_set1_epi8(1));
			r = _mm_add_epi8(d, _mm_sub_epi8(avg, odd));
		} else if (ft == 4) r = _mm_add_epi8(d, PaethPixel16(a, b, c));
		if (bpp == 3) r = _mm_or_si128(r, _mm_set1_epi32((int)0xff000000));
		Store4(dst+o, r);
		a = r;
	}
}

void UnfilterRow(unsigned char* row, const unsigned char* prev, size_t stride, int bpp, unsigned char ft,
                 unsigned char* rout, int xmode) {
	if (bpp == 4) {
		if (ft == 0) { if (rout) UnfilterRowNone4_AVX2(row, stride, rout); return; }
		if (ft == 2 && prev) { UnfilterRowUp4_AVX2(row, prev, stride, rout); return; }
		if (ft == 2 && !prev) { if (rout) UnfilterRowNone4_AVX2(row, stride, rout); return; }
		UnfilterRowGeneric4(row, prev, stride, ft, rout, xmode);
		return;
	}
	if (bpp == 3 && rout != nullptr) {
		if (ft == 0) {
			size_t ro = 0;
			for (size_t i = 0; i + 3 <= stride; i += 3, ro += 4) {
				rout[ro]=row[i+2]; rout[ro+1]=row[i+1]; rout[ro+2]=row[i]; rout[ro+3]=255;
			}
			return;
		}
		if (ft == 2 && prev) {
			size_t i = 0;
			for (; i + 32 <= stride; i += 32) {
				__m256i d = _mm256_loadu_si256((__m256i*)(row + i));
				__m256i b = _mm256_loadu_si256((__m256i*)(prev + i));
				__m256i r = _mm256_add_epi8(d, b);
				_mm256_storeu_si256((__m256i*)(row + i), r);
			}
			for (; i < stride; i++) row[i] = (unsigned char)(row[i] + prev[i]);
			size_t ro = 0;
			for (size_t j = 0; j + 3 <= stride; j += 3, ro += 4) {
				rout[ro]=row[j+2]; rout[ro+1]=row[j+1]; rout[ro+2]=row[j]; rout[ro+3]=255;
			}
			return;
		}
		if (ft == 2 && !prev) {
			size_t ro = 0;
			for (size_t i = 0; i + 3 <= stride; i += 3, ro += 4) {
				rout[ro]=row[i+2]; rout[ro+1]=row[i+1]; rout[ro+2]=row[i]; rout[ro+3]=255;
			}
			return;
		}
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
			rout[ro]=row[i+2]; rout[ro+1]=row[i+1]; rout[ro+2]=row[i]; rout[ro+3]=255;
		}
		return;
	}
	if (bpp == 3) {
		if (ft == 0) return;
		if (ft == 2 && prev) {
			size_t i = 0;
			for (; i + 32 <= stride; i += 32) {
				__m256i d = _mm256_loadu_si256((__m256i*)(row + i));
				__m256i b = _mm256_loadu_si256((__m256i*)(prev + i));
				__m256i r = _mm256_add_epi8(d, b);
				_mm256_storeu_si256((__m256i*)(row + i), r);
			}
			for (; i < stride; i++) row[i] = (unsigned char)(row[i] + prev[i]);
			return;
		}
		if (ft == 2 && !prev) return;
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
	if (ft == 0) return;
	if (ft == 1) { for (size_t i = (size_t)bpp; i < stride; i++) row[i] = (unsigned char)(row[i] + row[i - bpp]); }
	else if (ft == 2) { for (size_t i = 0; i < stride; i++) row[i] = (unsigned char)(row[i] + (prev ? prev[i] : 0)); }
	else if (ft == 3) { for (size_t i = 0; i < stride; i++) { int a = prev ? prev[i] : 0; int b = (i >= (size_t)bpp) ? row[i - bpp] : 0; row[i] = (unsigned char)(row[i] + (unsigned char)((a + b) / 2)); } }
	else if (ft == 4) { for (size_t i = 0; i < stride; i++) { int a = (i >= (size_t)bpp) ? row[i - bpp] : 0; int b = prev ? prev[i] : 0; int c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0; row[i] = (unsigned char)(row[i] + PaethScalar(a, b, c)); } }
}

} // namespace

int FastPngDecode(const unsigned char* file, size_t size, FastPngImage& out) {
	static const unsigned char kSig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	if (size < 33 || memcmp(file, kSig, 8) != 0) return -1;

	unsigned int w = 0, h = 0;
	int bitDepth = 0, colorType = 0, interlace = 0;
	bool haveIHDR = false, hasACTL = false, hasTRNSNonPalette = false;
	const unsigned char* exif_data = nullptr;
	unsigned int exif_len = 0;
	unsigned char palBGRA[256*4];
	int palEntries = 0;
	bool hasPLTE = false;
	// Default palette: opaque black (will be overwritten)
	for (int i=0;i<256;i++){ palBGRA[i*4+0]=0; palBGRA[i*4+1]=0; palBGRA[i*4+2]=0; palBGRA[i*4+3]=255; }

	size_t totalIdat = 0;
	size_t idatCount = 0;
	const unsigned char* firstIdat = nullptr;
	size_t offScan = 8;
	while (offScan + 8 <= size) {
		unsigned int len = _byteswap_ulong(*(const unsigned int*)(file + offScan));
		if (offScan + 12 + (size_t)len > size) break;
		const char* type = (const char*)(file + offScan + 4);
		if (memcmp(type, "IHDR", 4) == 0) {
			if (len != 13 || file[offScan + 18] != 0 || file[offScan + 19] != 0) return -1;
			w = _byteswap_ulong(*(const unsigned int*)(file + offScan + 8));
			h = _byteswap_ulong(*(const unsigned int*)(file + offScan + 12));
			bitDepth = file[offScan + 16];
			colorType = file[offScan + 17];
			interlace = file[offScan + 20];
			haveIHDR = true;
		} else if (memcmp(type, "acTL", 4) == 0) { hasACTL = true; break; }
		else if (memcmp(type, "PLTE", 4) == 0) {
			if (len % 3 == 0 && len <= 768) {
				palEntries = (int)(len / 3);
				hasPLTE = true;
				for (int i=0;i<palEntries;i++){
					unsigned char r = file[offScan+8 + i*3 + 0];
					unsigned char g = file[offScan+8 + i*3 + 1];
					unsigned char b = file[offScan+8 + i*3 + 2];
					palBGRA[i*4+0]=b; palBGRA[i*4+1]=g; palBGRA[i*4+2]=r; palBGRA[i*4+3]=255;
				}
			}
		} else if (memcmp(type, "tRNS", 4) == 0) {
			// For palette, tRNS is per-entry alpha; for others it's transparency key (reject)
			// We need colorType to decide, but IHDR already parsed at this point for well-formed files (PLTE after IHDR)
			if (haveIHDR && colorType == 3) {
				int n = (int)len;
				if (n > palEntries) n = palEntries;
				for (int i=0;i<n;i++) palBGRA[i*4+3]= file[offScan+8 + i];
			} else {
				hasTRNSNonPalette = true;
			}
		} else if (memcmp(type, "IDAT", 4) == 0) {
			if (idatCount++ == 0) firstIdat = file + offScan + 8;
			totalIdat += len;
		}
		else if (memcmp(type, "eXIf", 4) == 0) {
			if (len > 0 && len < 65528) { exif_data = file + offScan + 8; exif_len = len; }
		} else if (memcmp(type, "IEND", 4) == 0) { break; }
		offScan += 12 + (size_t)len;
	}
	if (!haveIHDR || totalIdat == 0) return -1;
	// Bound all subsequent stride, inflate-offset and BGRA allocation math.
	if (!w || !h || w > MAX_IMAGE_DIMENSION || h > MAX_IMAGE_DIMENSION ||
		(uint64_t)w * h > MAX_IMAGE_PIXELS) return -1;
	if (interlace != 0) return -1;
	if (hasACTL) return -1;
	if (hasTRNSNonPalette) return -1;
	// Palette handling
	bool isPalette = (colorType == 3);
	if (isPalette) {
		if (!hasPLTE) return -1;
		if (bitDepth != 8 && bitDepth != 4 && bitDepth != 2 && bitDepth != 1) return -1;
		// For this fast path we only handle 8-bit palette (covers most icons/photos). Fallback for 1/2/4.
		if (bitDepth != 8) return -1;
	} else {
		if (bitDepth != 8) return -1;
	}
	int compIn;
	if (isPalette) compIn = 1;
	else compIn = (colorType == 0) ? 1 : (colorType == 2) ? 3 : (colorType == 4) ? 2 : (colorType == 6) ? 4 : -1;
	if (compIn < 0) return -1;

	// Borrow a single IDAT directly. For multiple chunks, join into an
	// uninitialized allocation (vector::resize used to zero every byte
	// immediately before memcpy overwrote it).
	std::unique_ptr<unsigned char, decltype(&free)> idat(nullptr, &free);
	const unsigned char* compressed = firstIdat;
	if (idatCount > 1) {
		idat.reset((unsigned char*)malloc(totalIdat));
		if (!idat) return -1;
		unsigned char* idatDst = idat.get();
		size_t off = 8;
		while (off + 8 <= size) {
			unsigned int len = _byteswap_ulong(*(const unsigned int*)(file + off));
			if (off + 12 + (size_t)len > size) break;
			const char* type = (const char*)(file + off + 4);
			if (memcmp(type, "IDAT", 4) == 0) {
				memcpy(idatDst, file + off + 8, len);
				idatDst += len;
			} else if (memcmp(type, "IEND", 4) == 0) { break; }
			off += 12 + (size_t)len;
		}
		compressed = idat.get();
	}

	size_t stride = isPalette ? (size_t)w : (size_t)w * compIn;
	size_t rawSize = (size_t)h * (1 + stride);
	const bool compactColor = (colorType == 2 || colorType == 6);
	const size_t inflateOffset = colorType == 2 ? (size_t)w * h : 0;
	unsigned char* allocation = (unsigned char*)malloc(rawSize + inflateOffset);
	if (!allocation) return -1;
	unsigned char* raw = allocation + inflateOffset;

	thread_local libdeflate_decompressor* t_dec = nullptr;
	if (!t_dec) t_dec = libdeflate_alloc_decompressor();
	if (!t_dec) { free(allocation); return -1; }
	size_t outN = 0;
	enum libdeflate_result r = libdeflate_zlib_decompress(t_dec, compressed, totalIdat, raw, rawSize, &outN);
	idat.reset(); // compressed staging is no longer needed during reconstruction
	if (r != LIBDEFLATE_SUCCESS || outN != rawSize) { free(allocation); return -1; }

	// Reuse the allocation for output; palette/gray formats keep their path.
	unsigned char* pixels = compactColor ? allocation : (unsigned char*)malloc((size_t)w * h * 4);
	if (!pixels) { free(allocation); return -1; }

	int hist[5]={0};
	const unsigned char* prev = nullptr;
	if (isPalette) {
		for (unsigned int y = 0; y < h; y++) {
			unsigned char* rin = raw + y * (1 + stride) + 1;
			unsigned char* rout = pixels + (size_t)y * 4 * w;
			unsigned char ft = raw[y * (1 + stride)];
			if (ft > 4) { free(allocation); free(pixels); return -1; }
			hist[ft]++;
			UnfilterRow(rin, prev, stride, 1, ft, nullptr, 0);
			// expand via palette LUT (scalar - palette images are typically small)
			for (unsigned int x = 0; x < w; x++) {
				unsigned char idx = rin[x];
				if ((int)idx >= palEntries) idx = 0;
				((uint32_t*)rout)[x] = ((uint32_t*)palBGRA)[idx];
			}
			prev = rin;
		}
	} else {
		int xmode = (colorType == 6 || colorType == 2) ? colorType : 0;
		bool needPostTransform = (xmode == 0);
		for (unsigned int y = 0; y < h; y++) {
			unsigned char* rin = raw + y * (1 + stride) + 1;
			unsigned char* rout = pixels + (size_t)y * 4 * w;
			unsigned char ft = raw[y * (1 + stride)];
			if (ft > 4) { if (!compactColor) free(pixels); free(allocation); return -1; }
			hist[ft]++;
			if (compactColor) {
				UnfilterColorCompact(rin, prev, stride, compIn, ft, rout);
				prev = rout;
				continue;
			}
			UnfilterRow(rin, prev, stride, compIn, ft, needPostTransform ? nullptr : rout, xmode);
			if (needPostTransform) {
				if (colorType == 0) {
					for (unsigned int x = 0; x < w; x++) { unsigned char g = rin[x]; rout[x*4]=g; rout[x*4+1]=g; rout[x*4+2]=g; rout[x*4+3]=255; }
				} else {
					for (unsigned int x = 0; x < w; x++) { unsigned char g = rin[x*2], a = rin[x*2+1]; rout[x*4]=g; rout[x*4+1]=g; rout[x*4+2]=g; rout[x*4+3]=a; }
				}
			}
			prev = rin;
		}
	}
	if (!compactColor) free(allocation);
	// print to stderr so pngbench captures

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
