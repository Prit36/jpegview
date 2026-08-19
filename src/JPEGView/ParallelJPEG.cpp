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

namespace ParallelJPEG {


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

unsigned char* Decode(const void* buffer, int sizebytes, int& width, int& height, int& subsampling,
	ProgressFn progress, void* user) {
	width = height = 0;
	subsampling = -1;
	const unsigned char* buf = (const unsigned char*)buffer;
	long sz = sizebytes;
	if (buf == NULL || sz < 64 || buf[0] != 0xFF || buf[1] != 0xD8) return NULL;

	ScanInfo si;
	PrescanCtx ctx;
	if (!prescanSetup(buf, sz, si, ctx)) return NULL;
	subsampling = si.subsampling;

	// Cap bands by hardware threads (the load thread itself runs on one core).
	unsigned hw = std::thread::hardware_concurrency();
	int nBands = (int)((hw >= 4) ? min(8u, hw - 1) : 4);
	if (nBands < 2) nBands = 2;
	if (nBands > si.mcuRows) nBands = si.mcuRows;

	std::vector<std::pair<int, int>> bands;
	int rowsPerBand = si.mcuRows / nBands;
	int rem = si.mcuRows % nBands;
	int cur = 0;
	for (int i = 0; i < nBands; i++) {
		int cnt = rowsPerBand + (i < rem ? 1 : 0);
		if (cnt > 0) { bands.push_back({ cur, cur + cnt }); cur += cnt; }
	}
	if (bands.empty() || bands.back().second < si.mcuRows) bands.back().second = si.mcuRows;

	int pitch = (width = si.width) * 3;
	pitch = (pitch + 3) & ~3; // TJPAD
	height = si.height;
	unsigned char* out = new (std::nothrow) unsigned char[(size_t)pitch * si.height];
	if (out == NULL) { jpeg_destroy_decompress(&ctx.cinfo); return NULL; }

	std::atomic<bool> ok(true);
	std::atomic<int> ready(0);
	std::atomic<int> bandsDone(0);
	std::mutex mtx; std::condition_variable cv;
	auto waitReady = [&](int n) {
		std::unique_lock<std::mutex> lk(mtx);
		cv.wait(lk, [&]{ return !ok.load() || ready.load() >= n; });
	};

	std::thread prescanTh([&]() {
		bool okp = prescanRun(ctx, si, [&](int n) {
			{ std::lock_guard<std::mutex> lk(mtx); if (n > ready.load()) ready = n; }
			cv.notify_all();
		});
		if (!okp) { ok = false; cv.notify_all(); }
	});

	std::vector<std::thread> threads;
	for (size_t i = 0; i < bands.size(); i++) {
		int rowA = bands[i].first, rowB = bands[i].second;
		int need = (i + 1 == bands.size()) ? rowA : rowB;
		threads.emplace_back([&, i, rowA, rowB, need]() {
			waitReady(need);
			if (!ok.load()) return;
			std::vector<unsigned char> bandJpeg = buildBandJpeg(buf, sz, si, rowA, rowB);
			long bandTopPx = (long)rowA * si.mcuHeight;
			long offA = (long)(si.rowStates[rowA].next_input_byte - si.fileBase);
			if (!decodeBand(bandJpeg.data(), (long)bandJpeg.size(), si, rowA, rowB, offA, out, pitch, (int)bandTopPx)) {
				ok = false;
			} else {
				bandsDone.fetch_add(1);
			}
			cv.notify_all();
		});
	}

	try {
		// Wait for all bands; when the last band finishes, the whole source is
		// decoded, so notify the progress callback (which starts the resample).
		int notified = 0;
		{
			std::unique_lock<std::mutex> lk(mtx);
			while (ok.load() && bandsDone.load() < (int)bands.size()) {
				cv.wait(lk);
			}
		}
		if (ok.load() && bandsDone.load() >= (int)bands.size() && progress) {
			progress(user, out, width, height);
		}
		prescanTh.join();
		for (auto& th : threads) th.join();
	} catch (...) {
		ok = false;
	}

	if (!ok) {
		delete[] out;
		return NULL;
	}
	return out;
}

} // namespace ParallelJPEG
