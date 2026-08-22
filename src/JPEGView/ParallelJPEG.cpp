// Parallel baseline JPEG decoder using libjpeg-turbo internals.
//
// Strategy:
//  1. Scan segments; reject progressive/arithmetic/restart/non-8-bit/multi-scan.
//  2. Prescan: drive the entropy decoder (decode_mcu with NULL output = no
//     coefficient writes) over every MCU row, recording at each row boundary
//     the bit-reader state and per-component DC values.
//  3. Split the MCU rows into N bands. Each band is decoded from a synthetic
//     JPEG (verbatim header with the SOF height patched to the band height,
//     plus the band's entropy slice) with the prescan state injected at the
//     band start, so bands are independent and can decode on N threads.
//  4. Pipelined schedule: the prescan runs on its own thread and bands start
//     as soon as the state for their start row is available (band 0 needs no
//     prescan state). Spare cores decode bands while the prescan proceeds.
//
// The replicated huff_entropy_decoder_ layout must match jdhuff.c; this is
// validated by the pixel-exact prototype (mismatch = 0 over the test corpus).

#include "stdafx.h"
#define JPEG_INTERNALS
#include "ParallelJPEG.h"
#include "jconfig.h"
#include "jpeglib.h"
#include "jdhuff.h"
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <setjmp.h>
#include <chrono>
#include <cstdio>
#include <algorithm>

namespace ParallelJPEG {

// Env-gated phase instrumentation (JPEGVIEW_PJ_PROF=1): prints prescan/band/join
// wall times to stderr so the decode pipeline can be profiled standalone.
static bool ProfEnabled() {
	static int v = -1;
	if (v < 0) {
		char buf[8]; DWORD sz = sizeof(buf);
		v = (::GetEnvironmentVariableA("JPEGVIEW_PJ_PROF", buf, sz) > 0) ? 1 : 0;
	}
	return v != 0;
}
static bool DbgEnabled() {
	static int v = -1;
	if (v < 0) {
		char buf[8]; DWORD sz = sizeof(buf);
		v = (::GetEnvironmentVariableA("JPEGVIEW_PJ_DBG", buf, sz) > 0) ? 1 : 0;
	}
	return v != 0;
}
struct ProfClock {
	double t0;
	ProfClock() : t0(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count()) {}
	double Now() const { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
};



// ---- error handling: longjmp instead of exit() on malformed data ----
struct ErrMgr {
	jpeg_error_mgr pub;
	jmp_buf jmp;
};

static void error_exit(j_common_ptr cinfo) {
	ErrMgr* e = (ErrMgr*)cinfo->err;
	longjmp(e->jmp, 1);
}

static void output_message(j_common_ptr cinfo, int msg_level) {
	(void)cinfo; (void)msg_level; // suppress stderr noise for warnings
}

// ---- replicated private decoder layout (must match jdhuff.c) ----
struct savable_state_ { int last_dc_val[MAX_COMPS_IN_SCAN]; };
struct huff_entropy_decoder_ {
	struct jpeg_entropy_decoder pub;
	bitread_perm_state bitstate;
	savable_state_ saved;
	unsigned int restarts_to_go;
	d_derived_tbl *dc_derived_tbls[NUM_HUFF_TBLS];
	d_derived_tbl *ac_derived_tbls[NUM_HUFF_TBLS];
	d_derived_tbl *dc_cur_tbls[D_MAX_BLOCKS_IN_MCU];
	d_derived_tbl *ac_cur_tbls[D_MAX_BLOCKS_IN_MCU];
	boolean dc_needed[D_MAX_BLOCKS_IN_MCU];
	boolean ac_needed[D_MAX_BLOCKS_IN_MCU];
};

struct RowState {
	const JOCTET* next_input_byte;
	bit_buf_type get_buffer;
	int bits_left;
	int last_dc[MAX_COMPS_IN_SCAN];
	int unread_marker;
};

struct ScanInfo {
	int width, height, comps;
	int mcuWidth, mcuHeight;
	int mcusPerRow, mcuRows;
	long sosEnd;         // offset after SOS segment (start of entropy data)
	long eoiPos;         // offset of EOI marker
	const unsigned char* fileBase;
	int subsampling;     // matches TJSAMP enum (0/1/2/4/5), -1 = unknown
	std::vector<RowState> rowStates; // [r] = state AFTER MCU row r-1; [0] = start of row 0
};

static long parseSegLen(const unsigned char* p) { return ((long)p[0] << 8) | p[1]; }

// Walk markers to find SOS end / EOI and SOF geometry. Returns false for
// progressive/multi-scan/non-baseline forms.
static bool findSegments(const unsigned char* buf, long sz, long* sosEnd, long* eoiPos,
	int* width, int* height, int* comps, int* mcuW, int* mcuH, int* precision, bool* progressive, long* restartInterval) {
	long i = 2;
	bool inScan = false;
	int maxH = 1, maxV = 1;
	*width = *height = *comps = 0; *mcuW = *mcuH = 0; *precision = 0; *progressive = false; *restartInterval = 0;
	while (i < sz - 1) {
		if (inScan) {
			// Entropy region: jump between 0xFF bytes with memchr instead of
			// scanning byte-by-byte (the entropy is megabytes of arbitrary data).
			const unsigned char* p = (const unsigned char*)memchr(buf + i, 0xFF, (size_t)(sz - i));
			if (p == NULL) return false;
			i = (long)(p - buf);
			if (i >= sz - 1) return false;
			int m = buf[i + 1];
			if (m == 0xD9) { *eoiPos = i; return true; }
			if (m >= 0xD0 && m <= 0xD7) { i += 2; continue; }
			if (m == 0x00) { i += 2; continue; }
			if (m == 0xFF) { i++; continue; }
			if (m == 0xDA) return false; // second scan -> multi-scan; bail
			i += 2; continue;
		}
		if (buf[i] != 0xFF) { i++; continue; }
		int m = buf[i + 1];
		// Some encoders pad segment boundaries with extra 0xFF fill bytes
		// (e.g. RAW15571 has FF FF FF E1 between APP segments). Skip them,
		// otherwise they are parsed as a segment with a garbage length and the
		// walk jumps into the middle of the entropy data.
		while (m == 0xFF) {
			i++;
			if (i >= sz - 1) return false;
			m = buf[i + 1];
		}
		if (m == 0xDA) {
			int len = (int)parseSegLen(buf + i + 2);
			*sosEnd = i + 2 + len;
			inScan = true;
			i = *sosEnd;
			continue;
		}
		if (m == 0xDD) { *restartInterval = ((long)buf[i + 2] << 8) | buf[i + 3]; int len = (int)parseSegLen(buf + i + 2); i += len; continue; }
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
			*progressive = (m == 0xC2);
			if (m != 0xC0) return false; // only baseline SOF0 supported
			*precision = buf[i + 4];
			*height = ((int)buf[i + 5] << 8) | buf[i + 6];
			*width = ((int)buf[i + 7] << 8) | buf[i + 8];
			*comps = buf[i + 9];
			for (int c = 0; c < *comps; c++) {
				int h = buf[i + 11 + c * 3] >> 4;
				int v = buf[i + 11 + c * 3] & 0x0F;
				if (h > maxH) maxH = h;
				if (v > maxV) maxV = v;
			}
			*mcuW = 8 * maxH; *mcuH = 8 * maxV;
			int len = (int)parseSegLen(buf + i + 2);
			i += len;
			continue;
		}
		int len = (int)parseSegLen(buf + i + 2);
		if (len > 0) i += len; else i += 2;
	}
	return false;
}

// ---- prescan ----
struct PrescanCtx {
	jpeg_decompress_struct cinfo;
	ErrMgr err;
	bool initialized;
};

static bool prescanSetup(const unsigned char* buf, long sz, ScanInfo& si, PrescanCtx& ctx) {
	ctx.initialized = false;
	long sosEnd, eoiPos;
	bool progressive; long restartInt;
	int precision, w, h, comps, mcuW, mcuH;
	if (!findSegments(buf, sz, &sosEnd, &eoiPos, &w, &h, &comps, &mcuW, &mcuH, &precision, &progressive, &restartInt)) return false;
	if (progressive || precision != 8 || restartInt != 0 || comps != 3) return false;

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
	if (jpeg_read_header(&ctx.cinfo, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&ctx.cinfo); ctx.initialized = false; return false; }
	if (ctx.cinfo.progressive_mode || ctx.cinfo.data_precision != 8 || ctx.cinfo.arith_code || ctx.cinfo.restart_interval != 0) {
		jpeg_destroy_decompress(&ctx.cinfo); ctx.initialized = false; return false;
	}
	jpeg_start_decompress(&ctx.cinfo);

	si.width = w; si.height = h; si.comps = comps;
	si.mcuWidth = mcuW; si.mcuHeight = mcuH;
	si.mcusPerRow = ctx.cinfo.MCUs_per_row;
	si.mcuRows = ctx.cinfo.MCU_rows_in_scan;
	si.sosEnd = sosEnd;
	si.eoiPos = eoiPos;
	si.fileBase = buf;
	if (si.mcuRows < 2 || si.mcusPerRow < 1) { jpeg_destroy_decompress(&ctx.cinfo); ctx.initialized = false; return false; }

	int hmax = 0, vmax = 0;
	for (int c = 0; c < ctx.cinfo.num_components; c++) {
		if (ctx.cinfo.comp_info[c].h_samp_factor > hmax) hmax = ctx.cinfo.comp_info[c].h_samp_factor;
		if (ctx.cinfo.comp_info[c].v_samp_factor > vmax) vmax = ctx.cinfo.comp_info[c].v_samp_factor;
	}
	int h1 = ctx.cinfo.comp_info[1].h_samp_factor, v1 = ctx.cinfo.comp_info[1].v_samp_factor;
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

// Entropy pass; calls onRow(r+1) after each MCU row r completes.
static bool prescanRun(PrescanCtx& ctx, ScanInfo& si, std::function<void(int)> onRow) {
	if (setjmp(ctx.err.jmp)) {
		jpeg_destroy_decompress(&ctx.cinfo);
		return false;
	}
	huff_entropy_decoder_* hd = reinterpret_cast<huff_entropy_decoder_*>(ctx.cinfo.entropy);
	bool ok = true;
	for (int r = 0; r < si.mcuRows && ok; r++) {
		for (int m = 0; m < si.mcusPerRow; m++) {
			if (!ctx.cinfo.entropy->decode_mcu(&ctx.cinfo, NULL)) { ok = false; break; }
		}
		RowState rs2;
		rs2.next_input_byte = ctx.cinfo.src->next_input_byte;
		rs2.get_buffer = hd->bitstate.get_buffer;
		rs2.bits_left = hd->bitstate.bits_left;
		memcpy(rs2.last_dc, hd->saved.last_dc_val, sizeof(rs2.last_dc));
		rs2.unread_marker = ctx.cinfo.unread_marker;
		si.rowStates[r + 1] = rs2;
		if (onRow) onRow(r + 1);
	}
	jpeg_destroy_decompress(&ctx.cinfo);
	return ok;
}

// ---- build synthetic band JPEG ----
static std::vector<unsigned char> buildBandJpeg(const unsigned char* buf, long sz, const ScanInfo& si, int rowA, int rowB) {
	std::vector<unsigned char> out;
	const unsigned char* b = buf;
	out.insert(out.end(), b, b + si.sosEnd);

	long bandTop = (long)rowA * si.mcuHeight;
	long bandBot = si.height - bandTop;
	if (bandBot > (long)(rowB - rowA) * si.mcuHeight) bandBot = (long)(rowB - rowA) * si.mcuHeight;
	for (size_t i = 2; i < out.size() - 1; ) {
		if (out[i] != 0xFF) { i++; continue; }
		int m = out[i + 1];
		if (m == 0xD8 || m == 0xFF) { i += 2; continue; }
		if ((m >= 0xD0 && m <= 0xD7) || m == 0x00) { i += 2; continue; }
		if (m == 0xD9 || m == 0xDA) break;
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
			out[i + 5] = (unsigned char)(bandBot >> 8);
			out[i + 6] = (unsigned char)(bandBot & 0xFF);
			break;
		}
		size_t len = ((size_t)out[i + 2] << 8) | out[i + 3];
		i += len ? len : 2;
	}

	const JOCTET* pA = si.rowStates[rowA].next_input_byte;
	long offA = (long)(pA - (const JOCTET*)buf);
	long offB;
	if (rowB >= si.mcuRows) offB = si.eoiPos; // last band: slice ends at EOI
	else offB = (long)(si.rowStates[rowB].next_input_byte - (const JOCTET*)buf);
	if (offA < si.sosEnd) offA = si.sosEnd;
	if (offB > si.eoiPos) offB = si.eoiPos;
	if (offB + 8 < sz) offB += 8; else offB = sz;
	out.insert(out.end(), b + offA, b + offB);
	out.push_back(0xFF); out.push_back(0xD9);
	return out;
}

// ---- band decode ----
static bool decodeBand(const unsigned char* bandBuf, long bandSz, const ScanInfo& si, int rowA, int rowB, long offA,
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
	if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&cinfo); return false; }
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

	long sosEnd = si.sosEnd; // band header is a verbatim copy of [0, sosEnd)
	long rel = (long)(rs.next_input_byte - si.fileBase) - offA;
	if (rel < 0) rel = 0;
	long avail = (long)(bandSz - sosEnd);
	if (rel > avail) rel = avail;
	cinfo.src->next_input_byte = (const JOCTET*)(bandBuf + sosEnd) + rel;
	cinfo.src->bytes_in_buffer = (size_t)(avail - rel);
	cinfo.unread_marker = 0;

	JSAMPROW rows[16];
	while (cinfo.output_scanline < cinfo.output_height) {
		int start = (int)cinfo.output_scanline;
		int n = min(16, (int)(cinfo.output_height - cinfo.output_scanline));
		for (int r = 0; r < n; r++) {
			rows[r] = target + (size_t)(bandStartRowPx + start + r) * pitch;
		}
		JDIMENSION ret = jpeg_read_scanlines(&cinfo, rows, n);
		if (ret == 0) { jpeg_destroy_decompress(&cinfo); return false; }
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	return true;
}

// ===================== Speculative parallel state walk =====================
// Replaces the serial prescan on the critical path. The entropy stream is
// split into N byte slices; N walker threads decode their slice concurrently
// (replicating jdhuff.c bit-consumption semantics exactly), each recording:
//  - the decoder state at every MCU-row boundary inside the slice (used by the
//    existing synthetic-band libjpeg render path), and
//  - a short overlap log of (bit position, DC deltas) extending into the next
//    slice, which PROVES the assumed start alignment correct and resolves the
//    absolute DC predictor chain. Any mismatch -> retry next-best alignment,
//    then fall back to the legacy serial prescan.
// Bit position invariant: pos = 8*(ptr - fileBase) - bits_left, identical for
// every walker because all of them derive from the same file base pointer.

struct FastTbl {
	JLONG maxcode[18];
	JLONG valoffset[18];
	int lookup[1 << 8];
	unsigned char huffval[256];
};

static void CopyDerivedTbl(FastTbl& dst, const d_derived_tbl* src) {
	if (src == NULL) { memset(&dst, 0, sizeof(dst)); return; }
	memcpy(dst.maxcode, src->maxcode, sizeof(dst.maxcode));
	memcpy(dst.valoffset, src->valoffset, sizeof(dst.valoffset));
	memcpy(dst.lookup, src->lookup, sizeof(dst.lookup));
	if (src->pub != NULL) memcpy(dst.huffval, src->pub->huffval, sizeof(dst.huffval));
	else memset(dst.huffval, 0, sizeof(dst.huffval));
}

struct WalkState {
	const JOCTET* ptr;
	unsigned __int64 get_buffer;
	int bits_left;
	int marker;
};

struct WalkerConf {
	const JOCTET* base;
	long sosEnd, eoiPos;
	int mcusPerRow;
	int blocksInMCU;
	unsigned char blockComp[D_MAX_BLOCKS_IN_MCU]; // comp-in-scan index per block
	FastTbl dcTbl[D_MAX_BLOCKS_IN_MCU];
	FastTbl acTbl[D_MAX_BLOCKS_IN_MCU];
	bool dcNeeded[D_MAX_BLOCKS_IN_MCU];
	bool acNeeded[D_MAX_BLOCKS_IN_MCU];
};

struct LogEntry {
	__int64 pos;                              // 8*(ptr-base) - bits_left
	int pred[MAX_COMPS_IN_SCAN];              // running last_dc_val (slice-relative)
};

// --- bit reader, mirrors jdhuff.c GET_BYTE / FILL_BIT_BUFFER_FAST ---

#define WALK_GET_BYTE(st) { \
	register int c0_, c1_; \
	c0_ = *(st).ptr++; \
	c1_ = *(st).ptr; \
	(st).get_buffer = ((st).get_buffer << 8) | c0_; \
	(st).bits_left += 8; \
	if (c0_ == 0xFF) { \
		(st).ptr++; \
		if (c1_ != 0) { \
			(st).marker = c1_; \
			(st).ptr -= 2; \
			(st).get_buffer &= ~(__int64)0xFF; \
		} \
	} \
}

#define WALK_FILL(st) if ((st).bits_left <= 16) { \
	WALK_GET_BYTE((st)) WALK_GET_BYTE((st)) WALK_GET_BYTE((st)) \
	WALK_GET_BYTE((st)) WALK_GET_BYTE((st)) WALK_GET_BYTE((st)) }

// Mirrors HUFF_DECODE_FAST (HUFF_LOOKAHEAD == 8).
static __forceinline int WalkHuffSymbol(WalkState& st, const FastTbl& tbl) {
	WALK_FILL(st);
	int s = (int)((st.get_buffer >> (st.bits_left - 8)) & 0xFF);
	s = tbl.lookup[s];
	int nb = s >> 8;
	st.bits_left -= nb;
	s &= 0xFF;
	if (nb > 8) {
		s = (int)((st.get_buffer >> st.bits_left) & (((unsigned __int64)1 << nb) - 1));
		while (s > tbl.maxcode[nb]) {
			s <<= 1;
			st.bits_left--;
			s |= (int)((st.get_buffer >> st.bits_left) & 1);
			nb++;
		}
		if (nb > 16) s = 0;
		else s = tbl.huffval[(int)(s + tbl.valoffset[nb]) & 0xFF];
	}
	return s;
}

static __forceinline int HuffExtend(int r, int s) {
	return (r < (1 << (s - 1))) ? (r + ((-1) << s) + 1) : r;
}

// Decode one MCU of entropy data (consumption only, no coefficient writes).
// Returns false if a marker was hit (mirrors decode_mcu_fast bailing out).
static bool WalkMCU(const WalkerConf& cfg, WalkState& st, int lastDc[MAX_COMPS_IN_SCAN]) {
	for (int blkn = 0; blkn < cfg.blocksInMCU; blkn++) {
		int s = WalkHuffSymbol(st, cfg.dcTbl[blkn]);
		if (st.marker) return false;
		if (s) {
			WALK_FILL(st);
			int r = (int)((st.get_buffer >> (st.bits_left -= s)) & (((unsigned __int64)1 << s) - 1));
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
				st.bits_left -= sa; // extension bits: consume only
			} else {
				if (r != 15) break;
				k += 15;
			}
		}
	}
	return true;
}

static __int64 WalkPos(const WalkerConf& cfg, const WalkState& st) {
	return (__int64)8 * (st.ptr - cfg.base) - st.bits_left;
}

// Safe tail margin: stop walking this close to EOI (fast reader has no bounds check)
static const long WALK_TAIL_MARGIN = 4096;

// Probe-decode up to nMCU MCUs from an arbitrary state; logs pos+preds.
// Used for alignment scoring and for state-equivalence verification.
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

// Heuristic plausibility score for a candidate start alignment (lower=better).
static __int64 WalkScoreAlignment(const WalkerConf& cfg, WalkState st) {
	int lastDc[MAX_COMPS_IN_SCAN] = { 0 };
	__int64 pen = 0;
	// One probe row is enough to separate a true alignment from garbage:
	// misaligned decodes produce exploding |DC| within a single MCU row.
	int n = cfg.mcusPerRow;
	LogEntry scratch[16];
	for (int m = 0; m < n; m += 16) {
		int chunk = min(16, n - m);
		int done = WalkProbe(cfg, st, lastDc, chunk, scratch, false);
		if (done < chunk) return _I64_MAX / 2; // marker hit / tail
		for (int c = 0; c < MAX_COMPS_IN_SCAN; c++) {
			int d = lastDc[c] < 0 ? -lastDc[c] : lastDc[c];
			if (d > 2047) pen += (d - 2047);
		}
	}
	return pen;
}

// Build a jdhuff-compatible start state at file byte B for bit offset o.
// Returns false if not enough unstuffed data is available locally.
static bool BuildCandidate(const WalkerConf& cfg, long B, int o, WalkState& out) {
	if (B > cfg.eoiPos - WALK_TAIL_MARGIN) return false;
	// Unstuff up to 20 bytes starting at B, tracking the file offset after each.
	unsigned char u[20];
	long offAfter[20];
	int nu = 0;
	long p = B;
	while (nu < 20 && p < cfg.eoiPos) {
		unsigned char c = cfg.base[p];
		if (c == 0xFF) {
			unsigned char c1 = cfg.base[p + 1];
			if (c1 == 0x00) { u[nu] = 0xFF; p += 2; offAfter[nu++] = p; continue; }
			break; // marker: stop window here
		}
		u[nu] = c; p++; offAfter[nu++] = p;
	}
	if (nu < 8) return false; // need >= 8 unstuffed bytes for a deep fill
	// Load m whole unstuffed bytes so that loaded bits - o >= 48.
	int m = 7;
	while (m < nu && 8 * m - o < 48) m++;
	// Fill get_buffer with bits [o, 8m): the first o bits are consumed.
	unsigned __int64 gb = 0;
	for (int i = 0; i < m; i++) gb = (gb << 8) | u[i];
	gb <<= o;
	out.ptr = cfg.base + offAfter[m - 1];
	out.get_buffer = gb;
	out.bits_left = 8 * m - o;
	out.marker = 0;
	return true;
}

struct SliceWork {
	// inputs
	long byteStart, byteEnd;
	// outputs
	std::vector<LogEntry> headLog;   // first HEAD_N MCUs of the slice
	std::vector<LogEntry> tailLog;   // EXT_N MCUs past byteEnd
	int headCount, tailCount;
	int endPred[MAX_COMPS_IN_SCAN];
	long mcusWalked;
	// row states recorded inside this slice: (globalRowTag, state, preds)
	struct RowSnap { int row; WalkState st; int pred[MAX_COMPS_IN_SCAN]; };
	std::vector<RowSnap> rows;
	bool confirmed;
	int triedAligns;
	SliceWork() : headCount(0), tailCount(0), mcusWalked(0), confirmed(false), triedAligns(0) {
		memset(endPred, 0, sizeof(endPred));
	}
};

static const int HEAD_N = 64;
static const int EXT_N = 64;

// Match my headLog against previous slice's tailLog. On success returns the
// index in prevTailLog where the streams coincide and verifies that all
// subsequent DC delta pairs agree (exact confirmation of my alignment).
static bool MatchOverlap(const std::vector<LogEntry>& prevTail, const std::vector<LogEntry>& myHead,
	int& matchPrevIdx, int& matchMyIdx) {
	matchPrevIdx = matchMyIdx = -1;
	if (prevTail.empty() || myHead.empty()) return false;
	for (size_t a = 0; a < myHead.size(); a++) {
		for (size_t b = 0; b < prevTail.size(); b++) {
			if (myHead[a].pos != prevTail[b].pos) continue;
			// verify delta agreement for the overlapping run
			size_t run = min(prevTail.size() - b, myHead.size() - a);
			bool ok = true;
			for (size_t k = 1; k < run && ok; k++) {
				for (int c = 0; c < MAX_COMPS_IN_SCAN && ok; c++) {
					if ((myHead[a + k].pred[c] - myHead[a].pred[c]) !=
						(prevTail[b + k].pred[c] - prevTail[b].pred[c])) ok = false;
				}
			}
			if (ok && run >= 8) {
				matchPrevIdx = (int)b; matchMyIdx = (int)a;
				return true;
			}
			// positions matching but deltas disagreeing is proof enough of a bad align
			if (!ok) return false;
		}
	}
	return false;
}

// ---- public API ----
bool IsParallelDecodable(const void* buffer, int sizebytes, int& width, int& height) {
	const unsigned char* buf = (const unsigned char*)buffer;
	if (sizebytes < 64 || buf == NULL) return false;
	if (buf[0] != 0xFF || buf[1] != 0xD8) return false;
	long sosEnd, eoiPos;
	bool progressive; long restartInt;
	int precision, w, h, comps, mcuW, mcuH;
	if (!findSegments(buf, sizebytes, &sosEnd, &eoiPos, &w, &h, &comps, &mcuW, &mcuH, &precision, &progressive, &restartInt)) return false;
	if (progressive || precision != 8 || restartInt != 0 || comps != 3) return false;
	width = w; height = h;
	return true;
}

// Copy the live libjpeg decoder configuration (derived Huffman tables, MCU
// geometry, per-block table assignment) into the plain walker config.
static void ExtractWalkerConf(PrescanCtx& ctx, const ScanInfo& si, WalkerConf& cfg) {
	cfg.base = si.fileBase;
	cfg.sosEnd = si.sosEnd;
	cfg.eoiPos = si.eoiPos;
	cfg.mcusPerRow = si.mcusPerRow;
	cfg.blocksInMCU = ctx.cinfo.blocks_in_MCU;
	huff_entropy_decoder_* hd = reinterpret_cast<huff_entropy_decoder_*>(ctx.cinfo.entropy);
	memset(cfg.blockComp, 0, sizeof(cfg.blockComp));
	for (int b = 0; b < D_MAX_BLOCKS_IN_MCU; b++) {
		cfg.dcNeeded[b] = false; cfg.acNeeded[b] = false;
		CopyDerivedTbl(cfg.dcTbl[b], NULL);
		CopyDerivedTbl(cfg.acTbl[b], NULL);
	}
	for (int b = 0; b < cfg.blocksInMCU; b++) {
		cfg.blockComp[b] = (unsigned char)ctx.cinfo.MCU_membership[b];
		cfg.dcNeeded[b] = hd->dc_needed[b] != FALSE;
		cfg.acNeeded[b] = hd->ac_needed[b] != FALSE;
		if (hd->dc_cur_tbls[b] != NULL) CopyDerivedTbl(cfg.dcTbl[b], hd->dc_cur_tbls[b]);
		if (hd->ac_cur_tbls[b] != NULL) CopyDerivedTbl(cfg.acTbl[b], hd->ac_cur_tbls[b]);
	}
}

struct Snap {
	__int64 pos;
	WalkState st;
	int pred[MAX_COMPS_IN_SCAN];
};

struct SliceResult {
	bool walked = false;
	int alignUsed = -1;
	int rankAlign[3]; // top-3 alignment candidates from the heuristic probe
	std::vector<LogEntry> head, tail;
	std::vector<Snap> snaps;
	long mcus = 0; // local MCUs decoded within the slice region (excl. extension)
	int predEnd[MAX_COMPS_IN_SCAN];
	SliceResult() : walked(false), alignUsed(-1) {
		rankAlign[0] = rankAlign[1] = rankAlign[2] = -1;
		memset(predEnd, 0, sizeof(predEnd));
	}
};

// Walk slice i (entropy bytes [B, Bend)) with start alignment o. Records a
// snapshot before every MCU, the first HEAD_N as head log, and EXT_N MCUs of
// extension past Bend as tail log. Returns false on marker/bounds failure.
static bool WalkSlice(const WalkerConf& cfg, long B, long Bend, int o, SliceResult& w) {
	WalkState st;
	if (!BuildCandidate(cfg, B, o, st)) return false;
	int lastDc[MAX_COMPS_IN_SCAN] = { 0 };
	const long limit = 8 * Bend;
	w.snaps.clear(); w.head.clear(); w.tail.clear();
	w.mcus = 0;
	long est = (Bend - B) / 4 + EXT_N + 64;
	w.snaps.reserve(est);
	while (WalkPos(cfg, st) < limit) {
		if (st.marker) return false;
		if (st.ptr > cfg.base + cfg.eoiPos - WALK_TAIL_MARGIN) break; // end-of-stream safety: accept partial coverage
		Snap s;
		s.pos = WalkPos(cfg, st);
		s.st = st;
		memcpy(s.pred, lastDc, sizeof(s.pred));
		w.snaps.push_back(s);
		if ((int)w.head.size() < HEAD_N) {
			LogEntry e; e.pos = s.pos; memcpy(e.pred, lastDc, sizeof(e.pred));
			w.head.push_back(e);
		}
		if (!WalkMCU(cfg, st, lastDc)) return false;
		w.mcus++;
	}
	for (int k = 0; k < EXT_N; k++) {
		if (st.marker) break;
		if (st.ptr > cfg.base + cfg.eoiPos - WALK_TAIL_MARGIN) break;
		LogEntry e; e.pos = WalkPos(cfg, st); memcpy(e.pred, lastDc, sizeof(e.pred));
		w.tail.push_back(e);
		if (!WalkMCU(cfg, st, lastDc)) break;
	}
	memcpy(w.predEnd, lastDc, sizeof(w.predEnd));
	w.walked = true;
	w.alignUsed = o;
	return true;
}

// Try the speculative parallel state walk. On success fills si.rowStates for
// every row that starts a render band candidate and returns true.
static bool SpeculativeWalk(const WalkerConf& cfg, ScanInfo& si, int nSlices, double* profMs) {
	ProfClock clk;
	const long rangeStart = cfg.sosEnd;
	const long rangeEnd = cfg.eoiPos - WALK_TAIL_MARGIN;
	const long range = rangeEnd - rangeStart;
	if (range < 4 * 1024 * 1024) return false;
	while (nSlices > 2 && range / nSlices < 512 * 1024) nSlices--;

	std::vector<long> splits(nSlices + 1);
	for (int i = 0; i <= nSlices; i++)
		splits[i] = rangeStart + (__int64)range * i / nSlices;
	splits[0] = rangeStart;
	splits[nSlices] = rangeEnd;

	std::vector<SliceResult> work(nSlices);

	// Phase A: walk all slices concurrently with the best heuristic alignment.
	{
		std::vector<std::thread> ths;
		for (int i = 0; i < nSlices; i++) {
			ths.emplace_back([&, i]() {
				long B = (i == 0) ? cfg.sosEnd : splits[i];
				std::pair<__int64, int> rank[8];
				for (int o = 0; o < 8; o++) {
					WalkState cs;
					if (i == 0) { rank[o] = { 0, o }; continue; }
					if (!BuildCandidate(cfg, B, o, cs)) { rank[o] = { _I64_MAX / 2, o }; continue; }
					rank[o] = { WalkScoreAlignment(cfg, cs), o };
				}
				std::sort(rank, rank + 8);
				if (i == 0) {
					// True start: pristine state at sosEnd.
					SliceResult dummyAlign; dummyAlign.alignUsed = 0;
					WalkState st; st.ptr = cfg.base + cfg.sosEnd; st.get_buffer = 0; st.bits_left = 0; st.marker = 0;
					// inline walk identical to WalkSlice but with explicit start state
					int lastDc[MAX_COMPS_IN_SCAN] = { 0 };
					const long limit = 8 * splits[1];
					SliceResult& w = work[0];
					w.snaps.clear(); w.head.clear(); w.tail.clear(); w.mcus = 0;
					w.snaps.reserve((splits[1] - splits[0]) / 4 + EXT_N + 64);
					bool okw = true;
					while (WalkPos(cfg, st) < limit) {
						if (st.marker) { okw = false; break; }
						if (st.ptr > cfg.base + cfg.eoiPos - WALK_TAIL_MARGIN) break;
						Snap s; s.pos = WalkPos(cfg, st); s.st = st;
						memcpy(s.pred, lastDc, sizeof(s.pred));
						w.snaps.push_back(s);
						if ((int)w.head.size() < HEAD_N) { LogEntry e; e.pos = s.pos; memcpy(e.pred, lastDc, sizeof(e.pred)); w.head.push_back(e); }
						if (!WalkMCU(cfg, st, lastDc)) { okw = false; break; }
						w.mcus++;
					}
					if (okw) {
						for (int k = 0; k < EXT_N; k++) {
							if (st.marker || st.ptr > cfg.base + cfg.eoiPos - WALK_TAIL_MARGIN) break;
							LogEntry e; e.pos = WalkPos(cfg, st); memcpy(e.pred, lastDc, sizeof(e.pred));
							w.tail.push_back(e);
							if (!WalkMCU(cfg, st, lastDc)) break;
						}
						memcpy(w.predEnd, lastDc, sizeof(w.predEnd));
						w.walked = true; w.alignUsed = 0;
					}
					return;
				}
				for (int attempt = 0; attempt < 3; attempt++) {
					if (rank[attempt].first >= _I64_MAX / 2) break;
					if (WalkSlice(cfg, B, splits[i + 1], rank[attempt].second, work[i])) {
						work[i].rankAlign[0] = rank[attempt].second;
						// remember the remaining candidates for Phase-B retry
						int n = 1;
						for (int r = attempt + 1; r < 8 && n < 3; r++) {
							if (rank[r].first < _I64_MAX / 2) work[i].rankAlign[n++] = rank[r].second;
						}
						return;
					}
				}
			});
		}
		for (auto& t : ths) t.join();
	}

	// Phase B: sequential chain resolution (exact overlap confirmation).
#define MAX_SLICES_CHAIN 64
	if (nSlices > MAX_SLICES_CHAIN) return false;
	{
		int nw = 0;
		for (int i = 0; i < nSlices; i++) if (work[i].walked) nw++;
		if (DbgEnabled()) {
			fprintf(stderr, "[DBG] slices=%d walked=%d:", nSlices, nw);
			for (int i = 0; i < nSlices; i++)
				fprintf(stderr, " %d(a%d,m%ld,h%zu,t%zu)", work[i].walked ? 1 : 0, work[i].alignUsed, work[i].mcus, work[i].head.size(), work[i].tail.size());
			fprintf(stderr, "\n[DBG] cfg: blocks=%d mcusPerRow=%d sosEnd=%ld eoi=%ld\n",
				cfg.blocksInMCU, cfg.mcusPerRow, cfg.sosEnd, cfg.eoiPos);
		}
		if (nw < nSlices) return false;
	}
	__int64 A[MAX_SLICES_CHAIN];
	int P[MAX_SLICES_CHAIN][MAX_COMPS_IN_SCAN];
	A[0] = 0; memset(P[0], 0, sizeof(P[0]));
	for (int i = 1; i < nSlices; i++) {
		if (!work[i - 1].walked || !work[i].walked) return false;
		int b = -1, a = -1;
		int retry = 0;
		while (!MatchOverlap(work[i - 1].tail, work[i].head, b, a)) {
			if (DbgEnabled()) fprintf(stderr, "[DBG] match failed at slice %d (attempt %d, align %d)\n", i, retry, work[i].alignUsed);
			// The chain is rooted at slice 0 (true state), so any mismatch
			// localizes to slice i: re-walk it with the next candidate.
			retry++;
			if (retry >= 3 || work[i].rankAlign[retry] < 0) return false;
			if (!WalkSlice(cfg, splits[i], splits[i + 1], work[i].rankAlign[retry], work[i])) continue;
		}
		A[i] = A[i - 1] + work[i - 1].mcus + (b - a);
		for (int c = 0; c < MAX_COMPS_IN_SCAN; c++)
			P[i][c] = P[i - 1][c] + work[i - 1].tail[b].pred[c] - work[i].head[a].pred[c];
	}

	// Populate row states from the snapshots: the snapshot taken before the
	// first MCU of every global row becomes that row's start state.
	const int p = cfg.mcusPerRow;
	std::vector<int> availRows;
	for (int i = 0; i < nSlices; i++) {
		const std::vector<Snap>& sn = work[i].snaps;
		for (long m = 0; m < work[i].mcus; m++) {
			__int64 g = A[i] + m;
			if (g % p != 0) continue;
			int r = (int)(g / p);
			if (r < 1 || r >= si.mcuRows) continue;
			RowState& rs = si.rowStates[r];
			rs.next_input_byte = sn[m].st.ptr;
			rs.get_buffer = sn[m].st.get_buffer;
			rs.bits_left = sn[m].st.bits_left;
			for (int c = 0; c < MAX_COMPS_IN_SCAN; c++) rs.last_dc[c] = P[i][c] + sn[m].pred[c];
			rs.unread_marker = 0;
			availRows.push_back(r);
		}
	}
	if (availRows.size() < 2) return false;

	if (profMs != NULL) *profMs = clk.Now() - clk.t0;
	return true;
}


unsigned char* Decode(const void* buffer, int sizebytes, int& width, int& height, int& subsampling,
	ProgressFn progress, void* user) {
	width = height = 0;
	subsampling = -1;
	const unsigned char* buf = (const unsigned char*)buffer;
	long sz = sizebytes;
	if (buf == NULL || sz < 64 || buf[0] != 0xFF || buf[1] != 0xD8) return NULL;

	const bool profTop = ProfEnabled();
	ProfClock clkTop;
	double tEntry = profTop ? clkTop.Now() : 0.0;

	ScanInfo si;
	PrescanCtx ctx;
	if (!prescanSetup(buf, sz, si, ctx)) return NULL;
	subsampling = si.subsampling;
	double tSetup = profTop ? clkTop.Now() : 0.0;

	unsigned hw = std::thread::hardware_concurrency();

	const bool prof = ProfEnabled();
	ProfClock clk;
	double tPrescanEnd = 0.0, tJoin = 0.0, tSpec = 0.0;
	double bandCopyMs = 0.0, bandDecodeMs = 0.0;
	std::atomic<int> bandCount(0);

	// ---- Fast path: speculative parallel state walk (no serial prescan) ----
	bool walked = false;
	if (sz >= 2 * 1024 * 1024 && hw >= 4 && si.mcuRows >= 8) {
		WalkerConf cfg;
		ExtractWalkerConf(ctx, si, cfg); // read-only extract; cinfo stays alive for fallback
		int nSlices = (int)min(12u, hw);
		walked = SpeculativeWalk(cfg, si, nSlices, &tSpec);
		if (walked) jpeg_destroy_decompress(&ctx.cinfo);
		else if (prof) fprintf(stderr, "[PJ] speculative walk failed -> legacy prescan\n");
	}
	if (!walked) {
		// ---- Legacy path: serial pipelined prescan ----
		prescanRun(ctx, si, NULL);
		if (prof) tPrescanEnd = clk.Now();
	}

	// Cap bands by hardware threads (the load thread itself runs on one core).
	int nBands = (int)((hw >= 4) ? min(12u, hw - 1) : 4);
	if (nBands < 2) nBands = 2;
	if (nBands > si.mcuRows) nBands = si.mcuRows;

	// Band partition: prefer row starts whose states exist (speculative path
	// records every row anyway; the legacy path records all rows too).
	std::vector<std::pair<int, int>> bands;
	{
		std::vector<int> availRows;
		for (int r = 1; r < si.mcuRows; r++) {
			if (si.rowStates[r].next_input_byte != NULL) availRows.push_back(r);
		}
		if ((int)availRows.size() >= nBands - 1) {
			int need = nBands - 1;
			int prev = 0;
			for (int j = 1; j <= need; j++) {
				int ideal = (int)((__int64)si.mcuRows * j / nBands);
				// nearest available row > prev
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
			if (bands.empty() || bands.back().second < si.mcuRows) bands.back().second = si.mcuRows;
		}
	}

	int pitch = (width = si.width) * 3;
	pitch = (pitch + 3) & ~3; // TJPAD
	height = si.height;
	unsigned char* out = new (std::nothrow) unsigned char[(size_t)pitch * si.height];
	if (out == NULL) return NULL;

	std::atomic<bool> ok(true);
	std::atomic<int> bandsDone(0);
	std::mutex mtx; std::condition_variable cv;

	std::vector<std::thread> threads;
	for (size_t i = 0; i < bands.size(); i++) {
		int rowA = bands[i].first, rowB = bands[i].second;
		threads.emplace_back([&, i, rowA, rowB]() {
			if (!ok.load()) return;
			double tb0 = prof ? clk.Now() : 0.0;
			std::vector<unsigned char> bandJpeg = buildBandJpeg(buf, sz, si, rowA, rowB);
			double tb1 = prof ? clk.Now() : 0.0;
			long bandTopPx = (long)rowA * si.mcuHeight;
			long offA = (long)(si.rowStates[rowA].next_input_byte - si.fileBase);
			bool bandOk = decodeBand(bandJpeg.data(), (long)bandJpeg.size(), si, rowA, rowB, offA, out, pitch, (int)bandTopPx);
			double tb2 = prof ? clk.Now() : 0.0;
			if (prof) {
				bandCopyMs += tb1 - tb0; bandDecodeMs += tb2 - tb1; bandCount.fetch_add(1);
			}
			if (!bandOk) {
				ok = false;
			} else {
				bandsDone.fetch_add(1);
			}
			cv.notify_all();
		});
	}
	double tLaunched = profTop ? clkTop.Now() : 0.0;

	try {
		// Wait for all bands; when the last band finishes, the whole source is
		// decoded, so notify the progress callback (which starts the resample).
		{
			std::unique_lock<std::mutex> lk(mtx);
			while (ok.load() && bandsDone.load() < (int)bands.size()) {
				cv.wait(lk);
			}
		}
		double tRendered = profTop ? clkTop.Now() : 0.0;
		if (prof) tJoin = clk.Now();
		if (ok.load() && bandsDone.load() >= (int)bands.size() && progress) {
			progress(user, out, width, height);
		}
		double tProgress = profTop ? clkTop.Now() : 0.0;
		for (auto& th : threads) th.join();
		double tJoined = profTop ? clkTop.Now() : 0.0;
		if (profTop && tEntry >= 0.0) {
			fprintf(stderr, "[PJ2] setup=%.1f walk=%.1f partAlloc=%.1f launch=%.1f render=%.1f progress=%.1f joins=%.1f\n",
				tSetup - tEntry,
				(tSpec > 0.0) ? tSpec : ((tPrescanEnd > clk.t0 + 900.0) ? (tPrescanEnd - clk.t0) : 0.0),
				(tSpec > 0.0 ? (tJoin - tSpec) : 0.0) ,
				tLaunched - tSetup,
				tRendered - tLaunched - (tSpec > 0.0 ? 0.0 : 0.0),
				tProgress - tRendered,
				tJoined - tProgress);
		}
	} catch (...) {
		ok = false;
	}

	if (prof) {
		double tEnd = clk.Now();
		fprintf(stderr, "[PJ] total=%.1f spec=%.1f join=%.1f bands=%d copySum=%.1f decodeSum=%.1f size=%dx%d nBands=%d\n",
			tEnd - clk.t0, tSpec, tJoin - tSpec, bandCount.load(),
			bandCopyMs, bandDecodeMs, width, height, (int)bands.size());
	}

	if (!ok) {
		delete[] out;
		return NULL;
	}
	return out;
}

} // namespace ParallelJPEG
