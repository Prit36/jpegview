// Parallel baseline JPEG decoder using libjpeg-turbo internals.
// Splits MCU rows into independent bands decoded concurrently on worker threads.
// Uses a speculative parallel entropy walk to determine start bit states without serial prescan.

#include "stdafx.h"
#define JPEG_INTERNALS
#include "ParallelJPEG.h"
#include "jconfig.h"
#include "jpeglib.h"
#include "jdhuff.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <setjmp.h>
#include <thread>
#include <vector>

namespace ParallelJPEG {

// Error handling via longjmp to safely handle corrupt stream data
struct ErrMgr {
	jpeg_error_mgr pub;
	jmp_buf jmp;
};

static void error_exit(j_common_ptr cinfo) {
	ErrMgr* e = reinterpret_cast<ErrMgr*>(cinfo->err);
	longjmp(e->jmp, 1);
}

static void output_message(j_common_ptr, int) {
	// Suppress stderr noise for warnings
}

// Replicated private decoder layout matching jdhuff.c
struct savable_state_ {
	int last_dc_val[MAX_COMPS_IN_SCAN];
};

struct huff_entropy_decoder_ {
	struct jpeg_entropy_decoder pub;
	bitread_perm_state bitstate;
	savable_state_ saved;
	unsigned int restarts_to_go;
	d_derived_tbl* dc_derived_tbls[NUM_HUFF_TBLS];
	d_derived_tbl* ac_derived_tbls[NUM_HUFF_TBLS];
	d_derived_tbl* dc_cur_tbls[D_MAX_BLOCKS_IN_MCU];
	d_derived_tbl* ac_cur_tbls[D_MAX_BLOCKS_IN_MCU];
	boolean dc_needed[D_MAX_BLOCKS_IN_MCU];
	boolean ac_needed[D_MAX_BLOCKS_IN_MCU];
};

struct RowState {
	const JOCTET* next_input_byte = nullptr;
	bit_buf_type get_buffer = 0;
	int bits_left = 0;
	int last_dc[MAX_COMPS_IN_SCAN] = { 0 };
	int unread_marker = 0;
};

struct ScanInfo {
	int width = 0, height = 0, comps = 0;
	int mcuWidth = 0, mcuHeight = 0;
	int mcusPerRow = 0, mcuRows = 0;
	long sosEnd = 0;         // offset after SOS header (start of entropy data)
	long eoiPos = 0;         // offset of EOI marker
	const unsigned char* fileBase = nullptr;
	int subsampling = -1;    // matches TJSAMP enum (0=444, 1=422, 2=420, 4=440, 5=411)
	std::vector<RowState> rowStates; // [r] = state before MCU row r
};

static inline long parseSegLen(const unsigned char* p) {
	return (static_cast<long>(p[0]) << 8) | p[1];
}

// Walk JPEG markers to find SOS end, EOI offset, and frame geometry.
static bool findSegments(const unsigned char* buf, long sz, long* sosEnd, long* eoiPos,
	int* width, int* height, int* comps, int* mcuW, int* mcuH, int* precision, bool* progressive, long* restartInterval) {
	long i = 2;
	bool inScan = false;
	int maxH = 1, maxV = 1;
	*width = *height = *comps = 0;
	*mcuW = *mcuH = 0;
	*precision = 0;
	*progressive = false;
	*restartInterval = 0;

	while (i < sz - 1) {
		if (inScan) {
			const unsigned char* p = static_cast<const unsigned char*>(memchr(buf + i, 0xFF, static_cast<size_t>(sz - i)));
			if (p == nullptr) return false;
			i = static_cast<long>(p - buf);
			if (i >= sz - 1) return false;
			int m = buf[i + 1];
			if (m == 0xD9) { *eoiPos = i; return true; }
			if (m >= 0xD0 && m <= 0xD7) { i += 2; continue; }
			if (m == 0x00) { i += 2; continue; }
			if (m == 0xFF) { i++; continue; }
			if (m == 0xDA) return false; // Multi-scan unsupported
			i += 2;
			continue;
		}

		if (buf[i] != 0xFF) { i++; continue; }
		int m = buf[i + 1];
		// Skip extra 0xFF padding bytes between markers
		while (m == 0xFF) {
			i++;
			if (i >= sz - 1) return false;
			m = buf[i + 1];
		}

		// Standalone markers without payload
		if (m == 0xD8 || m == 0xD9 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
			i += 2;
			continue;
		}

		if (i + 3 >= sz) return false;
		int len = static_cast<int>(parseSegLen(buf + i + 2));
		if (len < 2 || i + 2 + len > sz) return false;

		if (m == 0xDA) { // SOS
			*sosEnd = i + 2 + len;
			inScan = true;
			i = *sosEnd;
			continue;
		}

		if (m == 0xDD) { // DRI
			if (len >= 4) {
				*restartInterval = (static_cast<long>(buf[i + 4]) << 8) | buf[i + 5];
			}
			i += 2 + len;
			continue;
		}

		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { // SOF
			*progressive = (m == 0xC2);
			if (m != 0xC0) return false; // Only baseline SOF0 supported
			if (len < 8) return false;
			*precision = buf[i + 4];
			*height = (static_cast<int>(buf[i + 5]) << 8) | buf[i + 6];
			*width = (static_cast<int>(buf[i + 7]) << 8) | buf[i + 8];
			*comps = buf[i + 9];
			if (len < 8 + *comps * 3) return false;
			for (int c = 0; c < *comps; c++) {
				int h = buf[i + 11 + c * 3] >> 4;
				int v = buf[i + 11 + c * 3] & 0x0F;
				if (h > maxH) maxH = h;
				if (v > maxV) maxV = v;
			}
			*mcuW = 8 * maxH;
			*mcuH = 8 * maxV;
			i += 2 + len;
			continue;
		}

		i += 2 + len;
	}
	return false;
}

// Setup libjpeg decompression context and validate baseline header parameters
struct PrescanCtx {
	jpeg_decompress_struct cinfo;
	ErrMgr err;
	bool initialized = false;
};

static bool prescanSetup(const unsigned char* buf, long sz, ScanInfo& si, PrescanCtx& ctx) {
	ctx.initialized = false;
	long sosEnd = 0, eoiPos = 0;
	bool progressive = false;
	long restartInt = 0;
	int precision = 0, w = 0, h = 0, comps = 0, mcuW = 0, mcuH = 0;

	if (!findSegments(buf, sz, &sosEnd, &eoiPos, &w, &h, &comps, &mcuW, &mcuH, &precision, &progressive, &restartInt)) {
		return false;
	}
	if (progressive || precision != 8 || restartInt != 0 || comps != 3) {
		return false;
	}

	ctx.cinfo.err = jpeg_std_error(&ctx.err.pub);
	ctx.err.pub.error_exit = error_exit;
	ctx.err.pub.emit_message = output_message;
	if (setjmp(ctx.err.jmp)) {
		jpeg_destroy_decompress(&ctx.cinfo);
		return false;
	}

	jpeg_create_decompress(&ctx.cinfo);
	ctx.initialized = true;
	jpeg_mem_src(&ctx.cinfo, buf, sz);
	if (jpeg_read_header(&ctx.cinfo, TRUE) != JPEG_HEADER_OK) {
		jpeg_destroy_decompress(&ctx.cinfo);
		ctx.initialized = false;
		return false;
	}
	if (ctx.cinfo.progressive_mode || ctx.cinfo.data_precision != 8 || ctx.cinfo.arith_code || ctx.cinfo.restart_interval != 0) {
		jpeg_destroy_decompress(&ctx.cinfo);
		ctx.initialized = false;
		return false;
	}
	jpeg_start_decompress(&ctx.cinfo);

	si.width = w;
	si.height = h;
	si.comps = comps;
	si.mcuWidth = mcuW;
	si.mcuHeight = mcuH;
	si.mcusPerRow = ctx.cinfo.MCUs_per_row;
	si.mcuRows = ctx.cinfo.MCU_rows_in_scan;
	si.sosEnd = sosEnd;
	si.eoiPos = eoiPos;
	si.fileBase = buf;
	if (si.mcuRows < 2 || si.mcusPerRow < 1) {
		jpeg_destroy_decompress(&ctx.cinfo);
		ctx.initialized = false;
		return false;
	}

	int hmax = 0, vmax = 0;
	for (int c = 0; c < ctx.cinfo.num_components; c++) {
		if (ctx.cinfo.comp_info[c].h_samp_factor > hmax) hmax = ctx.cinfo.comp_info[c].h_samp_factor;
		if (ctx.cinfo.comp_info[c].v_samp_factor > vmax) vmax = ctx.cinfo.comp_info[c].v_samp_factor;
	}
	int h1 = ctx.cinfo.comp_info[1].h_samp_factor;
	int v1 = ctx.cinfo.comp_info[1].v_samp_factor;
	si.subsampling = -1;
	if (h1 == hmax && v1 == vmax) si.subsampling = 0;             // 444
	else if (h1 == hmax && v1 * 2 == vmax) si.subsampling = 4;    // 440
	else if (h1 * 2 == hmax && v1 == vmax) si.subsampling = 1;    // 422
	else if (h1 * 2 == hmax && v1 * 2 == vmax) si.subsampling = 2; // 420
	else if (h1 * 4 == hmax && v1 == vmax) si.subsampling = 5;    // 411

	si.rowStates.resize(si.mcuRows + 1);
	huff_entropy_decoder_* hd = reinterpret_cast<huff_entropy_decoder_*>(ctx.cinfo.entropy);
	RowState rs;
	rs.next_input_byte = ctx.cinfo.src->next_input_byte;
	rs.get_buffer = hd->bitstate.get_buffer;
	rs.bits_left = hd->bitstate.bits_left;
	memcpy(rs.last_dc, hd->saved.last_dc_val, sizeof(rs.last_dc));
	rs.unread_marker = ctx.cinfo.unread_marker;
	si.rowStates[0] = rs;
	return true;
}

// Fallback sequential prescan
static bool prescanRun(PrescanCtx& ctx, ScanInfo& si) {
	if (setjmp(ctx.err.jmp)) {
		jpeg_destroy_decompress(&ctx.cinfo);
		return false;
	}
	huff_entropy_decoder_* hd = reinterpret_cast<huff_entropy_decoder_*>(ctx.cinfo.entropy);
	bool ok = true;
	for (int r = 0; r < si.mcuRows && ok; r++) {
		for (int m = 0; m < si.mcusPerRow; m++) {
			if (!ctx.cinfo.entropy->decode_mcu(&ctx.cinfo, nullptr)) {
				ok = false;
				break;
			}
		}
		RowState rs2;
		rs2.next_input_byte = ctx.cinfo.src->next_input_byte;
		rs2.get_buffer = hd->bitstate.get_buffer;
		rs2.bits_left = hd->bitstate.bits_left;
		memcpy(rs2.last_dc, hd->saved.last_dc_val, sizeof(rs2.last_dc));
		rs2.unread_marker = ctx.cinfo.unread_marker;
		si.rowStates[r + 1] = rs2;
	}
	jpeg_destroy_decompress(&ctx.cinfo);
	return ok;
}

// Build synthetic JPEG slice for row range [rowA, rowB)
static std::vector<unsigned char> buildBandJpeg(const unsigned char* buf, long sz, const ScanInfo& si, int rowA, int rowB) {
	std::vector<unsigned char> out;
	out.insert(out.end(), buf, buf + si.sosEnd);

	long bandTop = static_cast<long>(rowA) * si.mcuHeight;
	long bandBot = si.height - bandTop;
	if (bandBot > static_cast<long>(rowB - rowA) * si.mcuHeight) {
		bandBot = static_cast<long>(rowB - rowA) * si.mcuHeight;
	}

	for (size_t i = 2; i < out.size() - 1; ) {
		if (out[i] != 0xFF) { i++; continue; }
		int m = out[i + 1];
		while (m == 0xFF) {
			i++;
			if (i >= out.size() - 1) break;
			m = out[i + 1];
		}
		if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
		if (m == 0xD9 || m == 0xDA) break;
		if (i + 3 >= out.size()) break;
		size_t len = (static_cast<size_t>(out[i + 2]) << 8) | out[i + 3];
		if (len < 2 || i + 2 + len > out.size()) break;
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
			if (len >= 8) {
				out[i + 5] = static_cast<unsigned char>(bandBot >> 8);
				out[i + 6] = static_cast<unsigned char>(bandBot & 0xFF);
			}
			break;
		}
		i += 2 + len;
	}

	const JOCTET* pA = si.rowStates[rowA].next_input_byte;
	long offA = static_cast<long>(pA - reinterpret_cast<const JOCTET*>(buf));
	long offB = (rowB >= si.mcuRows) ? si.eoiPos : static_cast<long>(si.rowStates[rowB].next_input_byte - reinterpret_cast<const JOCTET*>(buf));

	if (offA < si.sosEnd) offA = si.sosEnd;
	if (offB > si.eoiPos) offB = si.eoiPos;
	if (offB + 8 < sz) offB += 8; else offB = sz;

	out.insert(out.end(), buf + offA, buf + offB);
	out.push_back(0xFF);
	out.push_back(0xD9);
	return out;
}

// Decode single synthetic band directly into destination image buffer
static bool decodeBand(const unsigned char* bandBuf, long bandSz, const ScanInfo& si, int rowA, long offA,
	unsigned char* target, int pitch, int bandStartRowPx) {
	jpeg_decompress_struct cinfo;
	ErrMgr err;
	cinfo.err = jpeg_std_error(&err.pub);
	err.pub.error_exit = error_exit;
	err.pub.emit_message = output_message;
	if (setjmp(err.jmp)) {
		jpeg_destroy_decompress(&cinfo);
		return false;
	}

	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, bandBuf, bandSz);
	if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
		jpeg_destroy_decompress(&cinfo);
		return false;
	}
	cinfo.out_color_space = JCS_EXT_BGR;
	cinfo.dct_method = JDCT_IFAST;
	cinfo.do_fancy_upsampling = FALSE;
	cinfo.dither_mode = JDITHER_NONE;
	jpeg_start_decompress(&cinfo);

	const RowState& rs = si.rowStates[rowA];
	huff_entropy_decoder_* hd = reinterpret_cast<huff_entropy_decoder_*>(cinfo.entropy);
	hd->bitstate.get_buffer = rs.get_buffer;
	hd->bitstate.bits_left = rs.bits_left;
	memcpy(hd->saved.last_dc_val, rs.last_dc, sizeof(rs.last_dc));
	cinfo.entropy->insufficient_data = FALSE;

	long sosEnd = si.sosEnd;
	long rel = static_cast<long>(rs.next_input_byte - si.fileBase) - offA;
	if (rel < 0) rel = 0;
	long avail = static_cast<long>(bandSz - sosEnd);
	if (rel > avail) rel = avail;
	cinfo.src->next_input_byte = reinterpret_cast<const JOCTET*>(bandBuf + sosEnd) + rel;
	cinfo.src->bytes_in_buffer = static_cast<size_t>(avail - rel);
	cinfo.unread_marker = 0;

	JSAMPROW rows[16];
	while (cinfo.output_scanline < cinfo.output_height) {
		int start = static_cast<int>(cinfo.output_scanline);
		int n = min(16, static_cast<int>(cinfo.output_height - cinfo.output_scanline));
		for (int r = 0; r < n; r++) {
			rows[r] = target + static_cast<size_t>(bandStartRowPx + start + r) * pitch;
		}
		JDIMENSION ret = jpeg_read_scanlines(&cinfo, rows, n);
		if (ret == 0) {
			jpeg_destroy_decompress(&cinfo);
			return false;
		}
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	return true;
}

// Speculative parallel entropy walker structures
struct FastTbl {
	JLONG maxcode[18];
	JLONG valoffset[18];
	int lookup[1 << 8];
	unsigned char huffval[256];
};

static void CopyDerivedTbl(FastTbl& dst, const d_derived_tbl* src) {
	if (src == nullptr) {
		memset(&dst, 0, sizeof(dst));
		return;
	}
	memcpy(dst.maxcode, src->maxcode, sizeof(dst.maxcode));
	memcpy(dst.valoffset, src->valoffset, sizeof(dst.valoffset));
	memcpy(dst.lookup, src->lookup, sizeof(dst.lookup));
	if (src->pub != nullptr) {
		memcpy(dst.huffval, src->pub->huffval, sizeof(dst.huffval));
	} else {
		memset(dst.huffval, 0, sizeof(dst.huffval));
	}
}

struct WalkState {
	const JOCTET* ptr = nullptr;
	uint64_t get_buffer = 0;
	int bits_left = 0;
	int marker = 0;
};

struct WalkerConf {
	const JOCTET* base = nullptr;
	long sosEnd = 0, eoiPos = 0;
	int mcusPerRow = 0;
	int blocksInMCU = 0;
	unsigned char blockComp[D_MAX_BLOCKS_IN_MCU] = { 0 };
	FastTbl dcTbl[D_MAX_BLOCKS_IN_MCU];
	FastTbl acTbl[D_MAX_BLOCKS_IN_MCU];
	bool dcNeeded[D_MAX_BLOCKS_IN_MCU] = { false };
	bool acNeeded[D_MAX_BLOCKS_IN_MCU] = { false };
};

struct LogEntry {
	int64_t pos = 0;                           // 8*(ptr-base) - bits_left
	int pred[MAX_COMPS_IN_SCAN] = { 0 };       // Running last_dc_val
};

#define WALK_GET_BYTE(st) { \
	int c0_ = *(st).ptr++; \
	int c1_ = *(st).ptr; \
	(st).get_buffer = ((st).get_buffer << 8) | c0_; \
	(st).bits_left += 8; \
	if (c0_ == 0xFF) { \
		(st).ptr++; \
		if (c1_ != 0) { \
			(st).marker = c1_; \
			(st).ptr -= 2; \
			(st).get_buffer &= ~static_cast<uint64_t>(0xFF); \
		} \
	} \
}

#define WALK_FILL(st) if ((st).bits_left <= 16) { \
	WALK_GET_BYTE((st)) WALK_GET_BYTE((st)) WALK_GET_BYTE((st)) \
	WALK_GET_BYTE((st)) WALK_GET_BYTE((st)) WALK_GET_BYTE((st)) }

static inline int WalkHuffSymbol(WalkState& st, const FastTbl& tbl) {
	WALK_FILL(st);
	int s = static_cast<int>((st.get_buffer >> (st.bits_left - 8)) & 0xFF);
	s = tbl.lookup[s];
	int nb = s >> 8;
	st.bits_left -= nb;
	s &= 0xFF;
	if (nb > 8) {
		s = static_cast<int>((st.get_buffer >> st.bits_left) & ((static_cast<uint64_t>(1) << nb) - 1));
		while (s > tbl.maxcode[nb]) {
			s <<= 1;
			st.bits_left--;
			s |= static_cast<int>((st.get_buffer >> st.bits_left) & 1);
			nb++;
		}
		s = (nb > 16) ? 0 : tbl.huffval[static_cast<int>(s + tbl.valoffset[nb]) & 0xFF];
	}
	return s;
}

static inline int HuffExtend(int r, int s) {
	return (r < (1 << (s - 1))) ? (r + ((-1) << s) + 1) : r;
}

static bool WalkMCU(const WalkerConf& cfg, WalkState& st, int lastDc[MAX_COMPS_IN_SCAN]) {
	for (int blkn = 0; blkn < cfg.blocksInMCU; blkn++) {
		int s = WalkHuffSymbol(st, cfg.dcTbl[blkn]);
		if (st.marker) return false;
		if (s) {
			WALK_FILL(st);
			int r = static_cast<int>((st.get_buffer >> (st.bits_left -= s)) & ((static_cast<uint64_t>(1) << s) - 1));
			s = HuffExtend(r, s);
		}
		if (cfg.dcNeeded[blkn]) {
			int ci = cfg.blockComp[blkn];
			s += lastDc[ci];
			lastDc[ci] = s;
		}
		const FastTbl& actbl = cfg.acTbl[blkn];
		for (int k = 1; k < 64; k++) {
			int sa = WalkHuffSymbol(st, actbl);
			if (st.marker) return false;
			int r = sa >> 4;
			sa &= 15;
			if (sa) {
				k += r;
				WALK_FILL(st);
				st.bits_left -= sa;
			} else {
				if (r != 15) break;
				k += 15;
			}
		}
	}
	return true;
}

static inline int64_t WalkPos(const WalkerConf& cfg, const WalkState& st) {
	return static_cast<int64_t>(8) * (st.ptr - cfg.base) - st.bits_left;
}

static constexpr long WALK_TAIL_MARGIN = 4096;
static constexpr int HEAD_N = 64;
static constexpr int EXT_N = 64;
static constexpr int MAX_SLICES_CHAIN = 64;

static int WalkProbe(const WalkerConf& cfg, WalkState st, int lastDc[MAX_COMPS_IN_SCAN],
	int nMCU, LogEntry* log, bool logOn) {
	int done = 0;
	for (int m = 0; m < nMCU; m++) {
		if (!WalkMCU(cfg, st, lastDc)) break;
		if (st.ptr > cfg.base + cfg.eoiPos - WALK_TAIL_MARGIN) break;
		if (logOn) {
			log[done].pos = WalkPos(cfg, st);
			memcpy(log[done].pred, lastDc, sizeof(log[done].pred));
		}
		done++;
	}
	return done;
}

static int64_t WalkScoreAlignment(const WalkerConf& cfg, WalkState st) {
	int lastDc[MAX_COMPS_IN_SCAN] = { 0 };
	int64_t pen = 0;
	int probeRows = 2;
	int n = probeRows * cfg.mcusPerRow;
	LogEntry scratch[16];
	for (int m = 0; m < n; m += 16) {
		int chunk = min(16, n - m);
		int done = WalkProbe(cfg, st, lastDc, chunk, scratch, false);
		if (done < chunk) return _I64_MAX / 2;
		for (int c = 0; c < MAX_COMPS_IN_SCAN; c++) {
			int d = (lastDc[c] < 0) ? -lastDc[c] : lastDc[c];
			if (d > 2047) pen += (d - 2047);
		}
	}
	return pen;
}

static bool BuildCandidate(const WalkerConf& cfg, long B, int o, WalkState& out) {
	if (B > cfg.eoiPos - WALK_TAIL_MARGIN) return false;
	unsigned char u[20];
	long offAfter[20];
	int nu = 0;
	long p = B;
	while (nu < 20 && p < cfg.eoiPos) {
		unsigned char c = cfg.base[p];
		if (c == 0xFF) {
			unsigned char c1 = cfg.base[p + 1];
			if (c1 == 0x00) {
				u[nu] = 0xFF;
				p += 2;
				offAfter[nu++] = p;
				continue;
			}
			break;
		}
		u[nu] = c;
		p++;
		offAfter[nu++] = p;
	}
	if (nu < 8) return false;
	int m = 7;
	while (m < nu && 8 * m - o < 48) m++;

	uint64_t gb = 0;
	for (int i = 0; i < m; i++) gb = (gb << 8) | u[i];
	gb <<= o;
	out.ptr = cfg.base + offAfter[m - 1];
	out.get_buffer = gb;
	out.bits_left = 8 * m - o;
	out.marker = 0;
	return true;
}

struct Snap {
	int64_t pos = 0;
	WalkState st;
	int pred[MAX_COMPS_IN_SCAN] = { 0 };
};

struct SliceResult {
	bool walked = false;
	int alignUsed = -1;
	int rankAlign[3] = { -1, -1, -1 };
	std::vector<LogEntry> head, tail;
	std::vector<Snap> snaps;
	long mcus = 0;
	int predEnd[MAX_COMPS_IN_SCAN] = { 0 };
};

static bool WalkSlice(const WalkerConf& cfg, long B, long Bend, int o, SliceResult& w) {
	WalkState st;
	if (B == cfg.sosEnd && o == 0) {
		st.ptr = cfg.base + cfg.sosEnd;
		st.get_buffer = 0;
		st.bits_left = 0;
		st.marker = 0;
	} else {
		if (!BuildCandidate(cfg, B, o, st)) return false;
	}

	int lastDc[MAX_COMPS_IN_SCAN] = { 0 };
	const long limit = 8 * Bend;
	w.snaps.clear();
	w.head.clear();
	w.tail.clear();
	w.mcus = 0;
	long est = (Bend - B) / 4 + EXT_N + 64;
	w.snaps.reserve(est);

	while (WalkPos(cfg, st) < limit) {
		if (st.marker) return false;
		if (st.ptr > cfg.base + cfg.eoiPos - WALK_TAIL_MARGIN) break;
		Snap s;
		s.pos = WalkPos(cfg, st);
		s.st = st;
		memcpy(s.pred, lastDc, sizeof(s.pred));
		w.snaps.push_back(s);
		if (static_cast<int>(w.head.size()) < HEAD_N) {
			LogEntry e;
			e.pos = s.pos;
			memcpy(e.pred, lastDc, sizeof(e.pred));
			w.head.push_back(e);
		}
		if (!WalkMCU(cfg, st, lastDc)) return false;
		w.mcus++;
	}

	for (int k = 0; k < EXT_N; k++) {
		if (st.marker) break;
		if (st.ptr > cfg.base + cfg.eoiPos - WALK_TAIL_MARGIN) break;
		LogEntry e;
		e.pos = WalkPos(cfg, st);
		memcpy(e.pred, lastDc, sizeof(e.pred));
		w.tail.push_back(e);
		if (!WalkMCU(cfg, st, lastDc)) break;
	}
	memcpy(w.predEnd, lastDc, sizeof(w.predEnd));
	w.walked = true;
	w.alignUsed = o;
	return true;
}

static bool MatchOverlap(const std::vector<LogEntry>& prevTail, const std::vector<LogEntry>& myHead,
	int& matchPrevIdx, int& matchMyIdx) {
	matchPrevIdx = matchMyIdx = -1;
	if (prevTail.empty() || myHead.empty()) return false;
	for (size_t a = 0; a < myHead.size(); a++) {
		for (size_t b = 0; b < prevTail.size(); b++) {
			if (myHead[a].pos != prevTail[b].pos) continue;
			size_t run = min(prevTail.size() - b, myHead.size() - a);
			bool ok = true;
			for (size_t k = 1; k < run && ok; k++) {
				for (int c = 0; c < MAX_COMPS_IN_SCAN && ok; c++) {
					if ((myHead[a + k].pred[c] - myHead[a].pred[c]) !=
						(prevTail[b + k].pred[c] - prevTail[b].pred[c])) {
						ok = false;
					}
				}
			}
			if (ok && run >= 8) {
				matchPrevIdx = static_cast<int>(b);
				matchMyIdx = static_cast<int>(a);
				return true;
			}
			if (!ok) return false;
		}
	}
	return false;
}

static void ExtractWalkerConf(PrescanCtx& ctx, const ScanInfo& si, WalkerConf& cfg) {
	cfg.base = si.fileBase;
	cfg.sosEnd = si.sosEnd;
	cfg.eoiPos = si.eoiPos;
	cfg.mcusPerRow = si.mcusPerRow;
	cfg.blocksInMCU = ctx.cinfo.blocks_in_MCU;
	huff_entropy_decoder_* hd = reinterpret_cast<huff_entropy_decoder_*>(ctx.cinfo.entropy);
	memset(cfg.blockComp, 0, sizeof(cfg.blockComp));
	for (int b = 0; b < D_MAX_BLOCKS_IN_MCU; b++) {
		cfg.dcNeeded[b] = false;
		cfg.acNeeded[b] = false;
		CopyDerivedTbl(cfg.dcTbl[b], nullptr);
		CopyDerivedTbl(cfg.acTbl[b], nullptr);
	}
	for (int b = 0; b < cfg.blocksInMCU; b++) {
		cfg.blockComp[b] = static_cast<unsigned char>(ctx.cinfo.MCU_membership[b]);
		cfg.dcNeeded[b] = (hd->dc_needed[b] != FALSE);
		cfg.acNeeded[b] = (hd->ac_needed[b] != FALSE);
		if (hd->dc_cur_tbls[b] != nullptr) CopyDerivedTbl(cfg.dcTbl[b], hd->dc_cur_tbls[b]);
		if (hd->ac_cur_tbls[b] != nullptr) CopyDerivedTbl(cfg.acTbl[b], hd->ac_cur_tbls[b]);
	}
}

static bool SpeculativeWalk(const WalkerConf& cfg, ScanInfo& si, int nSlices) {
	const long rangeStart = cfg.sosEnd;
	const long rangeEnd = cfg.eoiPos - WALK_TAIL_MARGIN;
	const long range = rangeEnd - rangeStart;
	if (range < 4 * 1024 * 1024 || nSlices > MAX_SLICES_CHAIN) return false;
	while (nSlices > 2 && range / nSlices < 512 * 1024) nSlices--;

	std::vector<long> splits(nSlices + 1);
	for (int i = 0; i <= nSlices; i++) {
		splits[i] = rangeStart + static_cast<int64_t>(range) * i / nSlices;
	}
	splits[0] = rangeStart;
	splits[nSlices] = rangeEnd;

	std::vector<SliceResult> work(nSlices);

	// Phase A: Concurrent slice walks
	{
		std::vector<std::thread> ths;
		for (int i = 0; i < nSlices; i++) {
			ths.emplace_back([&, i]() {
				long B = (i == 0) ? cfg.sosEnd : splits[i];
				if (i == 0) {
					WalkSlice(cfg, B, splits[1], 0, work[0]);
					return;
				}
				std::pair<int64_t, int> rank[8];
				for (int o = 0; o < 8; o++) {
					WalkState cs;
					if (!BuildCandidate(cfg, B, o, cs)) {
						rank[o] = { _I64_MAX / 2, o };
						continue;
					}
					rank[o] = { WalkScoreAlignment(cfg, cs), o };
				}
				std::sort(rank, rank + 8);
				for (int attempt = 0; attempt < 3; attempt++) {
					if (rank[attempt].first >= _I64_MAX / 2) break;
					if (WalkSlice(cfg, B, splits[i + 1], rank[attempt].second, work[i])) {
						work[i].rankAlign[0] = rank[attempt].second;
						int n = 1;
						for (int r = attempt + 1; r < 8 && n < 3; r++) {
							if (rank[r].first < _I64_MAX / 2) {
								work[i].rankAlign[n++] = rank[r].second;
							}
						}
						return;
					}
				}
			});
		}
		for (auto& t : ths) t.join();
	}

	// Phase B: Chain verification and DC resolution
	for (int i = 0; i < nSlices; i++) {
		if (!work[i].walked) return false;
	}

	int64_t A[MAX_SLICES_CHAIN];
	int P[MAX_SLICES_CHAIN][MAX_COMPS_IN_SCAN];
	A[0] = 0;
	memset(P[0], 0, sizeof(P[0]));

	for (int i = 1; i < nSlices; i++) {
		int b = -1, a = -1;
		int retry = 0;
		while (!MatchOverlap(work[i - 1].tail, work[i].head, b, a)) {
			retry++;
			if (retry >= 3 || work[i].rankAlign[retry] < 0) return false;
			if (!WalkSlice(cfg, splits[i], splits[i + 1], work[i].rankAlign[retry], work[i])) {
				continue;
			}
		}
		A[i] = A[i - 1] + work[i - 1].mcus + (b - a);
		for (int c = 0; c < MAX_COMPS_IN_SCAN; c++) {
			P[i][c] = P[i - 1][c] + work[i - 1].tail[b].pred[c] - work[i].head[a].pred[c];
		}
	}

	const int p = cfg.mcusPerRow;
	int availCount = 0;
	for (int i = 0; i < nSlices; i++) {
		const auto& sn = work[i].snaps;
		for (long m = 0; m < work[i].mcus; m++) {
			int64_t g = A[i] + m;
			if (g % p != 0) continue;
			int r = static_cast<int>(g / p);
			if (r < 1 || r >= si.mcuRows) continue;
			RowState& rs = si.rowStates[r];
			rs.next_input_byte = sn[m].st.ptr;
			rs.get_buffer = sn[m].st.get_buffer;
			rs.bits_left = sn[m].st.bits_left;
			for (int c = 0; c < MAX_COMPS_IN_SCAN; c++) {
				rs.last_dc[c] = P[i][c] + sn[m].pred[c];
			}
			rs.unread_marker = 0;
			availCount++;
		}
	}
	return (availCount >= 2);
}

// Public API
bool IsParallelDecodable(const void* buffer, int sizebytes, int& width, int& height) {
	const unsigned char* buf = static_cast<const unsigned char*>(buffer);
	if (sizebytes < 64 || buf == nullptr) return false;
	if (buf[0] != 0xFF || buf[1] != 0xD8) return false;
	long sosEnd = 0, eoiPos = 0, restartInt = 0;
	bool progressive = false;
	int precision = 0, w = 0, h = 0, comps = 0, mcuW = 0, mcuH = 0;
	if (!findSegments(buf, sizebytes, &sosEnd, &eoiPos, &w, &h, &comps, &mcuW, &mcuH, &precision, &progressive, &restartInt)) {
		return false;
	}
	if (progressive || precision != 8 || restartInt != 0 || comps != 3) {
		return false;
	}
	width = w;
	height = h;
	return true;
}

unsigned char* Decode(const void* buffer, int sizebytes, int& width, int& height, int& subsampling,
	ProgressFn progress, void* user) {
	width = height = 0;
	subsampling = -1;
	const unsigned char* buf = static_cast<const unsigned char*>(buffer);
	long sz = sizebytes;
	if (buf == nullptr || sz < 64 || buf[0] != 0xFF || buf[1] != 0xD8) return nullptr;

	ScanInfo si;
	PrescanCtx ctx;
	if (!prescanSetup(buf, sz, si, ctx)) return nullptr;
	subsampling = si.subsampling;

	unsigned hw = std::thread::hardware_concurrency();

	bool walked = false;
	if (sz >= 2 * 1024 * 1024 && hw >= 4 && si.mcuRows >= 8) {
		WalkerConf cfg;
		ExtractWalkerConf(ctx, si, cfg);
		int nSlices = static_cast<int>(min(12u, hw));
		walked = SpeculativeWalk(cfg, si, nSlices);
		if (walked) jpeg_destroy_decompress(&ctx.cinfo);
	}
	if (!walked) {
		prescanRun(ctx, si);
	}

	int nBands = static_cast<int>((hw >= 4) ? min(12u, hw - 1) : 4);
	if (nBands < 2) nBands = 2;
	if (nBands > si.mcuRows) nBands = si.mcuRows;

	std::vector<std::pair<int, int>> bands;
	{
		std::vector<int> availRows;
		for (int r = 1; r < si.mcuRows; r++) {
			if (si.rowStates[r].next_input_byte != nullptr) availRows.push_back(r);
		}
		if (static_cast<int>(availRows.size()) >= nBands - 1) {
			int need = nBands - 1;
			int prev = 0;
			for (int j = 1; j <= need; j++) {
				int ideal = static_cast<int>(static_cast<int64_t>(si.mcuRows) * j / nBands);
				int best = -1;
				auto it = std::lower_bound(availRows.begin(), availRows.end(), ideal);
				if (it != availRows.end()) best = *it;
				if (best <= prev && it != availRows.begin()) { --it; best = *it; }
				if (best <= prev) break;
				bands.push_back({ prev, best });
				prev = best;
			}
			bands.push_back({ prev, si.mcuRows });
		} else {
			int rowsPerBand = si.mcuRows / nBands;
			int rem = si.mcuRows % nBands;
			int cur = 0;
			for (int i = 0; i < nBands; i++) {
				int cnt = rowsPerBand + (i < rem ? 1 : 0);
				if (cnt > 0) { bands.push_back({ cur, cur + cnt }); cur += cnt; }
			}
			if (bands.empty() || bands.back().second < si.mcuRows) {
				bands.back().second = si.mcuRows;
			}
		}
	}

	int pitch = (width = si.width) * 3;
	pitch = (pitch + 3) & ~3; // 4-byte DIB alignment
	height = si.height;
	unsigned char* out = new (std::nothrow) unsigned char[static_cast<size_t>(pitch) * si.height];
	if (out == nullptr) return nullptr;

	std::atomic<bool> ok(true);
	std::atomic<int> bandsDone(0);
	std::mutex mtx;
	std::condition_variable cv;

	std::vector<std::thread> threads;
	for (size_t i = 0; i < bands.size(); i++) {
		int rowA = bands[i].first, rowB = bands[i].second;
		threads.emplace_back([&, rowA, rowB]() {
			if (!ok.load()) return;
			std::vector<unsigned char> bandJpeg = buildBandJpeg(buf, sz, si, rowA, rowB);
			long bandTopPx = static_cast<long>(rowA) * si.mcuHeight;
			long offA = static_cast<long>(si.rowStates[rowA].next_input_byte - si.fileBase);
			bool bandOk = decodeBand(bandJpeg.data(), static_cast<long>(bandJpeg.size()), si, rowA, offA, out, pitch, static_cast<int>(bandTopPx));
			if (!bandOk) {
				ok = false;
			} else {
				bandsDone.fetch_add(1);
			}
			cv.notify_all();
		});
	}

	try {
		{
			std::unique_lock<std::mutex> lk(mtx);
			while (ok.load() && bandsDone.load() < static_cast<int>(bands.size())) {
				cv.wait(lk);
			}
		}
		if (ok.load() && bandsDone.load() >= static_cast<int>(bands.size()) && progress) {
			progress(user, out, width, height);
		}
		for (auto& th : threads) th.join();
	} catch (...) {
		ok = false;
	}

	if (!ok) {
		delete[] out;
		return nullptr;
	}
	return out;
}

} // namespace ParallelJPEG