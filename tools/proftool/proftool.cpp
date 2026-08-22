// proftool.cpp : standalone decode profiler for benchmark images.
// Links ParallelJPEGV2.cpp + turbojpeg-static and times every decode strategy.
// Modes:
//   proftool <image.jpg> [iters]        : time ST vs parallel decode
//   proftool --verify <dir>             : pixel-exactness over a corpus
//   proftool --verifyone <file>         : same, single file
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <exception>
#include "ParallelJPEG.h"
#include "jpeglib.h"

static double NowMs() {
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::vector<unsigned char> ReadFileBytes(const std::wstring& path) {
	FILE* f = NULL;
	_wfopen_s(&f, path.c_str(), L"rb");
	if (!f) { fprintf(stderr, "cannot open %ls\n", path.c_str()); exit(1); }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	std::vector<unsigned char> buf((size_t)sz);
	if (sz > 0 && fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); fprintf(stderr, "read error\n"); exit(1); }
	fclose(f);
	return buf;
}

static std::wstring Utf8ToWide(const char* s) {
	int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	std::wstring w(wlen > 0 ? wlen - 1 : 0, L'\0');
	if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], wlen);
	return w;
}

static std::string WideToUtf8(const std::wstring& w) {
	int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
	std::string s(len > 0 ? len - 1 : 0, '\0');
	if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, NULL, NULL);
	return s;
}

// Single-threaded libjpeg-turbo scanline decode, identical settings to the
// TurboJpeg::ReadImage fallback path.
static std::vector<unsigned char> DecodeST(const unsigned char* buf, int sz, int& w, int& h) {
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
	while (cinfo.output_scanline < cinfo.output_height) {
		int start = (int)cinfo.output_scanline;
		int n = (int)((128 < (int)(cinfo.output_height - start)) ? 128 : (int)(cinfo.output_height - start));
		for (int r = 0; r < n; r++) rows[r] = out.data() + (size_t)(start + r) * pitch;
		jpeg_read_scanlines(&cinfo, rows, n);
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	return out;
}

struct FileResult {
	bool ok, skipped;
	double stMs, pjMs;
};

// One file: ST reference decode vs parallel decode, byte-exact comparison.
static FileResult VerifyOne(const std::wstring& wpath) {
	FileResult r{ false, false, 0.0, 0.0 };
	std::string path = WideToUtf8(wpath);
	auto data = ReadFileBytes(wpath);
	int w1 = 0, h1 = 0, w2 = 0, h2 = 0, sub = -1;
	if (!ParallelJPEG::IsParallelDecodable(data.data(), (int)data.size(), w2, h2)) { r.skipped = true; return r; }
	double t0 = NowMs();
	auto ref = DecodeST(data.data(), (int)data.size(), w1, h1);
	double t1 = NowMs();
	unsigned char* pj = ParallelJPEG::Decode(data.data(), (int)data.size(), w2, h2, sub, NULL, NULL);
	double t2 = NowMs();
	r.stMs = t1 - t0; r.pjMs = t2 - t1;
	r.ok = false;
	if (pj != NULL && w1 == w2 && h1 == h2) {
		size_t pitch = ((size_t)w1 * 3 + 3) & ~(size_t)3;
		r.ok = memcmp(ref.data(), pj, pitch * (size_t)h1) == 0;
	}
	delete[] pj;
	return r;
}

static int RunVerify(const std::vector<std::wstring>& files) {
	int pass = 0, fail = 0, skip = 0;
	double sumST = 0.0, sumPJ = 0.0;
	int measured = 0;
	for (auto& f : files) {
		std::string path = WideToUtf8(f);
		try {
			auto data = ReadFileBytes(f);
			int w1 = 0, h1 = 0, w2 = 0, h2 = 0, sub = -1;
			if (!ParallelJPEG::IsParallelDecodable(data.data(), (int)data.size(), w2, h2)) { skip++; continue; }
			double t0 = NowMs();
			auto ref = DecodeST(data.data(), (int)data.size(), w1, h1);
			double t1 = NowMs();
			unsigned char* pj = ParallelJPEG::Decode(data.data(), (int)data.size(), w2, h2, sub, NULL, NULL);
			double t2 = NowMs();
			sumST += t1 - t0; sumPJ += t2 - t1; measured++;
			bool ok = false;
			if (pj != NULL && w1 == w2 && h1 == h2) {
				size_t pitch = ((size_t)w1 * 3 + 3) & ~(size_t)3;
				ok = memcmp(ref.data(), pj, pitch * (size_t)h1) == 0;
			} else {
				delete[] pj; pj = NULL;
			}
			delete[] pj;
			if (ok) { pass++; printf("[PASS] %s  ST=%.0fms PJ=%.0fms\n", path.c_str(), t1 - t0, t2 - t1); }
			else { fail++; printf("[FAIL] %s  ST=%dx%d PJ=%dx%d pjNull=%d\n", path.c_str(), w1, h1, w2, h2, pj == NULL); }
			fflush(stdout);
		}
		catch (const std::exception& ex) {
			fail++; printf("[EXC ] %s: %s\n", path.c_str(), ex.what()); fflush(stdout);
		}
		catch (...) {
			fail++; printf("[EXC ] %s: unknown\n", path.c_str()); fflush(stdout);
		}
	}
	printf("\nverify: %d pass, %d FAIL, %d skipped(non-baseline) | avg ST=%.1fms PJ=%.1fms speedup=%.2fx\n",
		pass, fail, skip,
		(pass + fail) > 0 ? sumST / (pass + fail) : 0.0,
		(pass + fail) > 0 ? sumPJ / (pass + fail) : 0.0,
		sumST / (sumPJ > 0.001 ? sumPJ : 1.0));
	return fail == 0 ? 0 : 2;
}

static std::vector<std::wstring> ListJpegs(const std::wstring& dir) {
	std::vector<std::wstring> files;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*.jpg").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) h = FindFirstFileW((dir + L"\\*.JPG").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				files.push_back(dir + L"\\" + fd.cFileName);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	return files;
}

int main(int argc, char** argv) {
	if (argc >= 3 && (strcmp(argv[1], "--verifyone") == 0 || strcmp(argv[1], "--verify") == 0)) {
		std::vector<std::wstring> files;
		if (strcmp(argv[1], "--verifyone") == 0) {
			files.push_back(Utf8ToWide(argv[2]));
		} else {
			std::wstring dir = Utf8ToWide(argv[2]);
			while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
			files = ListJpegs(dir);
		}
		if (files.empty()) { fprintf(stderr, "no files\n"); return 1; }

		int pass = 0, fail = 0, skip = 0;
		double sumST = 0.0, sumPJ = 0.0;
		int measured = 0;
		for (auto& f : files) {
			std::string path = WideToUtf8(f);
			try {
				auto data = ReadFileBytes(f);
				int w1 = 0, h1 = 0, w2 = 0, h2 = 0, sub = -1;
				if (!ParallelJPEG::IsParallelDecodable(data.data(), (int)data.size(), w2, h2)) { skip++; continue; }
				double t0 = NowMs();
				auto ref = DecodeST(data.data(), (int)data.size(), w1, h1);
				double t1 = NowMs();
				unsigned char* pj = ParallelJPEG::Decode(data.data(), (int)data.size(), w2, h2, sub, NULL, NULL);
				double t2 = NowMs();
				sumST += t1 - t0; sumPJ += t2 - t1; measured++;
				bool ok = false;
				if (pj != NULL && w1 == w2 && h1 == h2) {
					size_t pitch = ((size_t)w1 * 3 + 3) & ~(size_t)3;
					ok = memcmp(ref.data(), pj, pitch * (size_t)h1) == 0;
				}
				delete[] pj;
				if (ok) { pass++; printf("[PASS] %s  ST=%.0fms PJ=%.0fms\n", path.c_str(), t1 - t0, t2 - t1); }
				else { fail++; printf("[FAIL] %s  ST=%dx%d PJ=%dx%d pjNull=%d\n", path.c_str(), w1, h1, w2, h2, pj == NULL); }
				fflush(stdout);
			}
			catch (const std::exception& ex) {
				fail++; printf("[EXC ] %s: %s\n", path.c_str(), ex.what()); fflush(stdout);
			}
			catch (...) {
				fail++; printf("[EXC ] %s: unknown\n", path.c_str()); fflush(stdout);
			}
		}
		printf("\nverify: %d pass, %d FAIL, %d skipped(non-baseline) | avg ST=%.1fms PJ=%.1fms speedup=%.2fx\n",
			pass, fail, skip,
			measured > 0 ? sumST / measured : 0.0,
			measured > 0 ? sumPJ / measured : 0.0,
			sumST / (sumPJ > 0.001 ? sumPJ : 1.0));
		return fail == 0 ? 0 : 2;
	}

	if (argc < 2) { fprintf(stderr, "usage: proftool <image.jpg> [iters] | proftool --verify <dir> | proftool --verifyone <file>\n"); return 1; }

	auto data = ReadFileBytes(Utf8ToWide(argv[1]));
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
