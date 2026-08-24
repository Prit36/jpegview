// pngbench.cpp - PNG decode benchmark + pixel dumper for correctness testing.
//
// Usage:
//   pngbench <file> [iterations] [warmups] [--blend]
//   pngbench <file> --dump <outfile.bin>          : decode once, dump BGRA pixels
//   pngbench <file> --dumpframes <n> <prefix>     : cycle n animation frames -> prefix.<i>.bin
//
// Correctness testing is done against Pillow as an independent decoder:
// see run_tests.py (compares these dumps with PIL's own output).
#include <windows.h>
#include "PNGWrapper.h"
#include "FastPng.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

static double g_freq = 0;
static double now_sec() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / g_freq;
}

// Replicates Helpers::AlphaBlendBackground for timing the post-load loop.
static inline unsigned int alpha_blend(unsigned int pixel, unsigned int bg) {
    unsigned int alpha = pixel & 0xFF000000;
    if (alpha == 0xFF000000) return pixel;
    unsigned int bg_r = (bg >> 16) & 0xFF;
    unsigned int bg_g = (bg >> 8) & 0xFF;
    unsigned int bg_b = bg & 0xFF;
    if (alpha == 0) return 0xFF000000 | (bg_r << 16) | (bg_g << 8) | bg_b;
    unsigned int a = alpha >> 24;
    unsigned int oma = 255 - a;
    unsigned int r = (pixel >> 16) & 0xFF;
    unsigned int g = (pixel >> 8) & 0xFF;
    unsigned int b = pixel & 0xFF;
    unsigned int orr = (r * a + bg_r * oma + 127) / 255;
    unsigned int og = (g * a + bg_g * oma + 127) / 255;
    unsigned int ob = (b * a + bg_b * oma + 127) / 255;
    return 0xFF000000 | (orr << 16) | (og << 8) | ob;
}

int main(int argc, char** argv) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_freq = (double)f.QuadPart;

    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) { printf("usage: pngbench <file> [iters] [warmups] [--blend] | --dump <out> | --dumpframes <n> <prefix>\n"); return 1; }
    const char* path = args[0].c_str();
    int iterations = 10, warmups = 3;
    bool blend = false;

    // dump modes need the raw file buffer before anything else
    HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (fh == INVALID_HANDLE_VALUE) { printf("cannot open %s\n", path); return 1; }
    LARGE_INTEGER fs; GetFileSizeEx(fh, &fs);
    size_t fsize = (size_t)fs.QuadPart;
    void* fbuf = malloc(fsize);
    DWORD rd = 0;
    if (!ReadFile(fh, fbuf, (DWORD)fsize, &rd, NULL) || rd != fsize) { printf("read failed\n"); CloseHandle(fh); return 1; }
    CloseHandle(fh);

    // --dump <outfile>
    if (args.size() >= 3 && args[1] == "--dump") {
        int W, H, B, fc, ft; bool anim, oom; void* exif = NULL;
        void* px = PngReader::ReadImage(W, H, B, anim, fc, ft, exif, oom, fbuf, fsize);
        if (!px) { printf("decode failed\n"); free(fbuf); return 2; }
        FILE* fo = fopen(args[2].c_str(), "wb");
        fwrite(px, 1, (size_t)W * H * B, fo);
        fclose(fo);
        printf("dumped %dx%d ch=%d anim=%d\n", W, H, B, (int)anim);
        free(px); if (exif) free(exif); free(fbuf);
        return 0;
    }

    // --dumpframes <n> <prefix>
    if (args.size() >= 4 && args[1] == "--dumpframes") {
        int n = atoi(args[2].c_str());
        std::string prefix = args[3];
        int W = 0, H = 0, B = 0;
        for (int fi = 0; fi < n; fi++) {
            int fc, ft; bool anim, oom; void* exif = NULL;
            // mimic ImageLoadThread: first call passes bytes, later calls pass NULL/0
            void* buf = fi == 0 ? fbuf : NULL;
            size_t bs = fi == 0 ? fsize : 0;
            void* px = PngReader::ReadImage(W, H, B, anim, fc, ft, exif, oom, buf, bs);
            if (!px) { printf("decode failed at frame %d\n", fi); break; }
            char fpath[MAX_PATH];
            snprintf(fpath, MAX_PATH, "%s.%d.bin", prefix.c_str(), fi);
            FILE* fo = fopen(fpath, "wb");
            fwrite(px, 1, (size_t)W * H * B, fo);
            fclose(fo);
            free(px); if (exif) free(exif);
            if (!anim) { printf("static image (frame 0 dumped)\n"); break; }
        }
        printf("done (%dx%d ch=%d)\n", W, H, B);
        free(fbuf);
        return 0;
    }

    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "--blend") blend = true;
        else if (args[i][0] != '-') {
            if (iterations == 10 && warmups == 3) iterations = atoi(args[i].c_str());
            else warmups = atoi(args[i].c_str());
        }
    }

    // warmup passes
    for (int w = 0; w < warmups; w++) {
        int W, H, B, fc, ft; bool anim, oom; void* exif = NULL;
        void* px = PngReader::ReadImage(W, H, B, anim, fc, ft, exif, oom, fbuf, fsize);
        if (px) free(px); if (exif) free(exif);
    }

    std::vector<double> times;
    double blend_total = 0;
    int W = 0, H = 0;
    bool failed = false;
    for (int it = 0; it < iterations; it++) {
        int wW, wH, wB, wfc, wft; bool anim, oom; void* exif = NULL;
        double s = now_sec();
        void* px = PngReader::ReadImage(wW, wH, wB, anim, wfc, wft, exif, oom, fbuf, fsize);
        double e = now_sec();
        if (!px) { failed = true; break; }
        if (it == 0) { W = wW; H = wH; }
        if (blend) {
            const unsigned int bg = 0x00FFFFFF;
            double bs = now_sec();
            unsigned int* p = (unsigned int*)px;
            for (int i = 0; i < wW * wH; i++) p[i] = alpha_blend(p[i], bg);
            blend_total += now_sec() - bs;
        }
        times.push_back((e - s) * 1000.0);
        free(px); if (exif) free(exif);
    }

    if (failed || times.empty()) { printf("decode failed\n"); free(fbuf); return 2; }
    std::sort(times.begin(), times.end());
    double median = times[times.size() / 2];

    printf("==================================================================\n");
    printf("PNG decode benchmark : %s\n", path);
    printf("Dimensions           : %dx%d (%.2f MP)\n", W, H, W * (double)H / 1e6);
    printf("File size            : %.2f MB\n", fsize / (1024.0 * 1024.0));
    printf("------------------------------------------------------------------\n");
    printf("Decode  min   : %8.2f ms\n", times.front());
    printf("Decode  median: %8.2f ms\n", median);
    printf("Decode  max   : %8.2f ms\n", times.back());
    if (blend) printf("Blend   avg   : %8.2f ms\n", blend_total * 1000.0 / iterations);
    printf("==================================================================\n");

    free(fbuf);
    return 0;
}
