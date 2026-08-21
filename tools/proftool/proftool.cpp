// proftool.cpp : standalone decode profiler for benchmark images.
// Links ParallelJPEG.cpp + turbojpeg-static and times every decode strategy.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>
#include "ParallelJPEG.h"
#include "jpeglib.h"

static double NowMs() {
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::vector<unsigned char> ReadFile(const char* path) {
	FILE* f = NULL;
	fopen_s(&f, path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	std::vector<unsigned char> buf((size_t)sz);
	if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read error\n"); exit(1); }
	fclose(f);
	return buf;
}

// Single-threaded libjpeg-turbo scanline decode, identical settings to the
// TurboJpeg::ReadImage fallback path.
static std::vector<unsigned char> DecodeST(const unsigned char* buf, int sz, int& w, int& h, double* tEntropyOut = nullptr) {
	jpeg_decompress_struct cinfo; jpeg_error_mgr jerr;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, buf, sz);
	jpeg_read_header(&cinfo, TRUE);
	cinfo.out_color_space = JCS_EXT_BGR;
	cinfo.dct_method = JDCT_IFAST;
	cinfo.do_fancy_upsampling = FALSE;
	cinfo.dither_mode = JDITHER_NONE;
	jpeg_start_decompress(&cinfo);
	w = (int)cinfo.output_width; h = (int)cinfo.output_height;
	size_t pitch = ((size_t)w * 3 + 3) & ~(size_t)3;
	std::vector<unsigned char> out(pitch * (size_t)h);
	JSAMPROW rows[128];
	double tRender = 0.0;
	while (cinfo.output_scanline < cinfo.output_height) {
		int start = (int)cinfo.output_scanline;
		int n = (int)min(128, cinfo.output_height - cinfo.output_scanline);
		for (int r = 0; r < n; r++) rows[r] = out.data() + (size_t)(start + r) * pitch;
		double t0 = NowMs();
		jpeg_read_scanlines(&cinfo, rows, n);
		tRender += NowMs() - t0;
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	if (tEntropyOut) *tEntropyOut = tRender; // render-inclusive read_scanlines time
	return out;
}

int main(int argc, char** argv) {
	if (argc < 2) { fprintf(stderr, "usage: proftool <image.jpg> [iters] | proftool --verify <dir> | proftool --verifyone <file>\n"); return 1; }

	if (strcmp(argv[1], "--verifyone") == 0 || strcmp(argv[1], "--verify") == 0) {
		std::vector<std::wstring> files;
		if (strcmp(argv[1], "--verifyone") == 0) {
			int wlen = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, NULL, 0);
			std::wstring w(wlen - 1, 0); MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, &w[0], wlen);
			files.push_back(w);
		} else {
			int wlen = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, NULL, 0);
			std::wstring dir(wlen - 1, 0); MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, &dir[0], wlen);
			WIN32_FIND_DATAW fd;
			HANDLE h = FindFirstFileW((dir + L"\\*.jpg").c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE) h = FindFirstFileW((dir + L"\\*.JPG").c_str(), &fd);
			if (h != INVALID_HANDLE_VALUE) {
				do { files.push_back(dir + L"\\" + fd.cFileName); } while (FindNextFileW(h, &fd));
				FindClose(h);
			}
		}
		if (files.empty()) { fprintf(stderr, "no files\n"); return 1; }
		int pass = 0, fail = 0, skip = 0;
		double sumST = 0.0, sumPJ = 0.0;
		for (auto& f : files) {
			char path[MAX_PATH * 2];
			WideCharToMultiByte(CP_UTF8, 0, f.c_str(), -1, path, sizeof(path), NULL, NULL);
			auto data = ReadFile(path);
			int w1 = 0, h1 = 0, w2 = 0, h2 = 0, sub = -1;
			bool decodable = ParallelJPEG::IsParallelDecodable(data.data(), (int)data.size(), w2, h2);
			if (!decodable) { skip++; continue; }
			double t0 = NowMs();
			auto ref = DecodeST(data.data(), (int)data.size(), w1, h1);
			double t1 = NowMs();
			unsigned char* pj = ParallelJPEG::Decode(data.data(), (int)data.size(), w2, h2, sub, NULL, NULL);
			double t2 = NowMs();
			sumST += t1 - t0; sumPJ += t2 - t1;
			bool ok = false;
			if (pj != NULL && w1 == w2 && h1 == h2) {
				size_t pitch = ((size_t)w1 * 3 + 3) & ~(size_t)3;
				ok = memcmp(ref.data(), pj, pitch * (size_t)h1) == 0;
			}
			delete[] pj;
			if (ok) { pass++; printf("[PASS] %s  ST=%.0fms PJ=%.0fms\n", path, t1 - t0, t2 - t1); }
			else { fail++; printf("[FAIL] %s  ST=%dx%d PJ=%dx%d pjNull=%d\n", path, w1, h1, w2, h2, pj == NULL); }
			fflush(stdout);
		}
		printf("\nverify: %d pass, %d FAIL, %d skipped(non-baseline) | avg ST=%.1fms PJ=%.1fms speedup=%.2fx\n",
			pass, fail, skip, sumST / max(1, pass + fail), sumPJ / max(1, pass + fail), sumST / max(0.001, sumPJ));
		return fail == 0 ? 0 : 2;
	}

	auto data = ReadFile(argv[1]);
	int iters = (argc > 2) ? atoi(argv[2]) : 3;
	printf("file: %s (%.2f MB)\n", argv[1], data.size() / 1048576.0);

	// 1. single-threaded reference
	for (int i = 0; i < iters; i++) {
		int w = 0, h = 0;
		double t0 = NowMs();
		auto px = DecodeST(data.data(), (int)data.size(), w, h);
		double t1 = NowMs();
		printf("[ST ] %.1f ms  (%dx%d, %.1f MP/s)\n", t1 - t0, w, h, w * h / 1000.0 / (t1 - t0));
	}

	// 2. parallel band decoder (set JPEGVIEW_PJ_PROF=1 for phase detail)
	for (int i = 0; i < iters; i++) {
		int w = 0, h = 0, sub = -1;
		double t0 = NowMs();
		unsigned char* px = ParallelJPEG::Decode(data.data(), (int)data.size(), w, h, sub, NULL, NULL);
		double t1 = NowMs();
		printf("[PJ ] %.1f ms  ok=%d (%dx%d sub=%d)\n", t1 - t0, px != NULL, w, h, sub);
		delete[] px;
	}

	// 3. header parse only
	for (int i = 0; i < iters; i++) {
		int w = 0, h = 0;
		double t0 = NowMs();
		bool ok = ParallelJPEG::IsParallelDecodable(data.data(), (int)data.size(), w, h);
		double t1 = NowMs();
		printf("[HDR] %.2f ms  decodable=%d (%dx%d)\n", t1 - t0, ok, w, h);
	}
	return 0;
}
