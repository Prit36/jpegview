// pngbench.cpp - faithful standalone benchmark & correctness checker for
// the real JPEGView PNG decode path (PngReader::ReadImage in PNGWrapper.cpp).
//
// Build: see build_bench.bat
//
// Usage:
//   pngbench <pngfile> [iterations] [warmups] [--verify] [--blend]
//
//   --verify  : also decode with a reference libpng path and compare bytes
//   --blend   : additionally time the AlphaBlendBackground post-loop

#include <windows.h>
#include <png.h>
#include "PNGWrapper.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>

static double g_freq = 0;
static double now_sec() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / g_freq;
}

// ---- Replicates Helpers::AlphaBlendBackground (for timing the post-loop) ----
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

// ---- Reference decode using libpng directly (same transforms as PngReader) ----
static void* reference_decode(const void* buf, size_t size, int& w, int& h, int& ch) {
    png_structp pp = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop ip = png_create_info_struct(pp);
    if (!pp || !ip) { if (pp) png_destroy_read_struct(&pp, &ip, NULL); return NULL; }
    if (setjmp(png_jmpbuf(pp))) { png_destroy_read_struct(&pp, &ip, NULL); return NULL; }

    png_set_sig_bytes(pp, 8);
    struct Ctx { const unsigned char* p; size_t off; size_t size; };
    static Ctx ctx;
    ctx.p = (const unsigned char*)buf; ctx.off = 8; ctx.size = size;
    png_set_read_fn(pp, &ctx, [](png_structp png, png_bytep out, png_size_t sz) {
        Ctx* c = (Ctx*)png_get_io_ptr(png);
        if (c->off + sz > c->size) png_error(png, "oob");
        memcpy(out, c->p + c->off, sz);
        c->off += sz;
    });

    png_read_info(pp, ip);
    png_set_expand(pp);
    png_set_strip_16(pp);
    png_set_gray_to_rgb(pp);
    png_set_add_alpha(pp, 0xff, PNG_FILLER_AFTER);
    png_set_bgr(pp);
    (void)png_set_interlace_handling(pp);
    png_read_update_info(pp, ip);
    w = png_get_image_width(pp, ip);
    h = png_get_image_height(pp, ip);
    ch = png_get_channels(pp, ip);
    if (ch != 4) { png_destroy_read_struct(&pp, &ip, NULL); return NULL; }
    png_uint_32 rowbytes = png_get_rowbytes(pp, ip);
    unsigned char* out = (unsigned char*)malloc((size_t)h * rowbytes);
    if (!out) { png_destroy_read_struct(&pp, &ip, NULL); return NULL; }
    std::vector<png_bytep> rows(h);
    for (png_uint_32 j = 0; j < h; j++) rows[j] = out + j * rowbytes;
    png_read_image(pp, rows.data());
    png_read_end(pp, ip);
    png_destroy_read_struct(&pp, &ip, NULL);
    return out;
}

#include <zlib.h>
#include <libdeflate.h>
#include "fastpng.h"

// Collect all IDAT chunk data and inflate it once (no unfilter) to isolate
// the zlib inflate cost from the unfilter cost.
static double bench_inflate_only(const void* buf, size_t size, size_t out_cap, unsigned char* out) {
    const unsigned char* p = (const unsigned char*)buf;
    size_t off = 8;
    std::vector<unsigned char> idat;
    while (off + 8 <= size) {
        unsigned int len = _byteswap_ulong(*(const unsigned int*)(p + off));
        const char* type = (const char*)(p + off + 4);
        if (memcmp(type, "IDAT", 4) == 0) {
            if (off + 12 + (size_t)len > size) break;
            idat.insert(idat.end(), p + off + 8, p + off + 8 + len);
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        off += 12 + (size_t)len;
    }
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int irc = inflateInit(&strm);
    if (irc != Z_OK) { fprintf(stderr, "inflateInit rc=%d (%s) zlib=%s\n", irc, zError(irc), ZLIB_VERSION); return -1; }
    strm.next_in = idat.data();
    strm.avail_in = (uInt)idat.size();
    strm.next_out = out;
    strm.avail_out = (uInt)out_cap;
    int r = inflate(&strm, Z_FINISH);
    size_t produced = out_cap - strm.avail_out;
    inflateEnd(&strm);
    (void)r;
    return (double)produced;
}

static void print_usage(const char* name) {
    printf("Usage: %s <pngfile> [iterations] [warmups] [--verify] [--blend]\n", name);
}

int main(int argc, char** argv) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_freq = (double)f.QuadPart;

    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) { print_usage(argv[0]); return 1; }
    const char* path = args[0].c_str();
    int iterations = 10, warmups = 3;
    bool verify = false, blend = false, breakdown = false, inflate_mode = false;
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "--verify") verify = true;
        else if (args[i] == "--blend") blend = true;
        else if (args[i] == "--breakdown") breakdown = true;
        else if (args[i] == "--inflate") inflate_mode = true;
        else if (args[i][0] != '-') {
            if (iterations == 10 && warmups == 3) iterations = atoi(args[i].c_str());
            else warmups = atoi(args[i].c_str());
        }
    }

    // Read whole file into memory (like ImageLoadThread ReadFile into pBuffer).
    HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (fh == INVALID_HANDLE_VALUE) { printf("cannot open %s\n", path); return 1; }
    LARGE_INTEGER fs; GetFileSizeEx(fh, &fs);
    size_t fsize = (size_t)fs.QuadPart;
    void* fbuf = malloc(fsize);
    DWORD rd = 0;
    if (!ReadFile(fh, fbuf, (DWORD)fsize, &rd, NULL) || rd != fsize) {
        printf("read failed\n"); CloseHandle(fh); free(fbuf); return 1;
    }
    CloseHandle(fh);

    // Warm the file cache: read once.
    double t0 = now_sec();
    volatile unsigned char sink = 0;
    for (size_t i = 0; i < fsize; i += 4096) sink ^= ((unsigned char*)fbuf)[i];
    (void)sink;

    // Fast-path benchmark - production FastPng.cpp (libdeflate + SIMD unfilter).
    if (std::find(args.begin(), args.end(), std::string("--fast")) != args.end()) {
        FastPngImage fp;
        double s = now_sec();
        int rc = FastPngDecode((const unsigned char*)fbuf, fsize, fp);
        double e = now_sec();
        if (rc != 0) { printf("fast path returned %d (unsupported -> would fall back)\n", rc); free(fbuf); return 0; }
        printf("fast path decode (1x): %8.2f ms\n", (e - s) * 1000.0);
        int rw, rh, rch;
        void* ref = reference_decode(fbuf, fsize, rw, rh, rch);
        if (ref && rw == fp.width && rh == fp.height) {
            size_t n = (size_t)fp.width * fp.height * 4;
            if (memcmp(fp.pixels, ref, n) == 0) printf("[verify] PASS: fast path byte-identical to reference libpng\n");
            else {
                size_t d2 = 0, ndiff = 0, last = 0;
                for (size_t i = 0; i < n; i++) if (fp.pixels[i] != ((unsigned char*)ref)[i]) { if (!d2) d2 = i; ndiff++; last = i; }
                printf("[verify] FAIL: %zu/%zu differ; first@%zu (row %d px %d ch %d), last@%zu (row %d)\n",
                       ndiff, n, d2, (int)(d2/(fp.width*4)), (int)((d2%(fp.width*4))/4), (int)(d2%4), last, (int)(last/(fp.width*4)));
            }
        } else printf("[verify] dims mismatch ref=%dx%d got=%dx%d\n", rw, rh, fp.width, fp.height);
        if (ref) free(ref);
        if (fp.pixels) free(fp.pixels);
        if (fp.exif_payload) free(fp.exif_payload);
        free(fbuf);
        return 0;
    }

    // Dump decoded pixels for cross-build comparison: --dump <outfile>
    if (args.size() >= 3 && args[1] == "--dump") {
        int W, H, B, fc, ft; bool anim, oom; void* exif = NULL;
        void* px = PngReader::ReadImage(W, H, B, anim, fc, ft, exif, oom, fbuf, fsize);
        if (!px) { printf("decode failed\n"); free(fbuf); return 1; }
        FILE* fo = fopen(args[2].c_str(), "wb");
        fwrite(px, 1, (size_t)W * H * B, fo);
        fclose(fo);
        printf("dumped %dx%d ch=%d anim=%d frame=%d\n", W, H, B, (int)anim, 0);
        free(px); if (exif) free(exif); free(fbuf);
        return 0;
    }

    // Warmup decode passes (discard results).
    for (int w = 0; w < warmups; w++) {
        int W, H, B, fc, ft; bool anim, oom; void* exif = NULL;
        void* px = PngReader::ReadImage(W, H, B, anim, fc, ft, exif, oom, fbuf, fsize);
        if (px) free(px); if (exif) free(exif);
    }

    std::vector<double> decode_times;
    std::vector<double> ref_times;
    double blend_total = 0;
    int W = 0, H = 0;
    for (int it = 0; it < iterations; it++) {
        int wW, wH, wB, wfc, wft; bool anim, oom; void* exif = NULL;
        double s = now_sec();
        void* px = PngReader::ReadImage(wW, wH, wB, anim, wfc, wft, exif, oom, fbuf, fsize);
        double e = now_sec();
        if (!px) { printf("decode failed (oom=%d)\n", (int)oom); if (exif) free(exif); return 1; }
        if (it == 0) { W = wW; H = wH; }
        if (blend) {
            unsigned int bg = 0x00FFFFFF; // light gray-ish default
            double bs = now_sec();
            unsigned int* p = (unsigned int*)px;
            for (int i = 0; i < wW * wH; i++) p[i] = alpha_blend(p[i], bg);
            blend_total += now_sec() - bs;
        }
        decode_times.push_back((e - s) * 1000.0);
        free(px); if (exif) free(exif);

        if (breakdown) {
            int rw, rh, rch;
            double rs = now_sec();
            void* rp = reference_decode(fbuf, fsize, rw, rh, rch);
            double re = now_sec();
            if (rp) { ref_times.push_back((re - rs) * 1000.0); free(rp); }
        }
    }

    std::sort(decode_times.begin(), decode_times.end());
    double sum = 0; for (double v : decode_times) sum += v;
    double min = decode_times.front(), max = decode_times.back();
    double median = decode_times[decode_times.size() / 2];
    double mean = sum / decode_times.size();

    printf("==================================================================\n");
    printf("PNG decode benchmark : %s\n", path);
    printf("Dimensions           : %dx%d  (%.2f MP)  channels=4 (BGRA)\n", W, H, W * (double)H / 1e6);
    printf("File size            : %.2f MB\n", fsize / (1024.0 * 1024.0));
    printf("Iterations/Warmups   : %d / %d\n", iterations, warmups);
    printf("------------------------------------------------------------------\n");
    printf("Decode  min   : %8.2f ms\n", min);
    printf("Decode  median: %8.2f ms\n", median);
    printf("Decode  mean  : %8.2f ms\n", mean);
    printf("Decode  max   : %8.2f ms\n", max);
    if (blend) printf("Blend   total : %8.2f ms  (avg %.2f ms)\n", blend_total * 1000.0, blend_total * 1000.0 / iterations);
    if (breakdown) {
        if (!ref_times.empty()) {
            std::sort(ref_times.begin(), ref_times.end());
            printf("RefDec  median: %8.2f ms  (libpng inflate+unfilter, no extra copies)\n", ref_times[ref_times.size()/2]);
            printf("Copy    est   : %8.2f ms  (extra memcpys/mallocs in current PngReader)\n", median - ref_times[ref_times.size()/2]);
        }
    }
    printf("==================================================================\n");

    // libdeflate inflate-only benchmark (separates inflate cost; also proves libdeflate works here)
    if (std::find(args.begin(), args.end(), std::string("--ldinflate")) != args.end() || inflate_mode) {
        // collect IDAT
        const unsigned char* p = (const unsigned char*)fbuf;
        size_t off = 8;
        std::vector<unsigned char> idat;
        int w = 0, h = 0, ct = 0, bd = 0;
        while (off + 8 <= fsize) {
            unsigned int len = _byteswap_ulong(*(const unsigned int*)(p + off));
            const char* type = (const char*)(p + off + 4);
            if (memcmp(type, "IHDR", 4) == 0) {
                w = (int)_byteswap_ulong(*(const unsigned int*)(p + off + 8));
                h = (int)_byteswap_ulong(*(const unsigned int*)(p + off + 12));
                bd = p[off + 16]; ct = p[off + 17];
            }
            if (memcmp(type, "IDAT", 4) == 0) { if (off + 12 + (size_t)len <= fsize) idat.insert(idat.end(), p + off + 8, p + off + 8 + len); }
            else if (memcmp(type, "IEND", 4) == 0) break;
            off += 12 + (size_t)len;
        }
        int compin = (ct==0)?1:(ct==2)?3:(ct==4)?2:(ct==6)?4:4;
        size_t rawcap = (size_t)h * (1 + (size_t)w * compin);
        unsigned char* raw = (unsigned char*)malloc(rawcap);
        struct libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
        std::vector<double> itimes;
        for (int it = 0; it < iterations; it++) {
            size_t out_n = 0;
            double s = now_sec();
            enum libdeflate_result r = libdeflate_zlib_decompress(dec, idat.data(), idat.size(), raw, rawcap, &out_n);
            double e = now_sec();
            if (r != LIBDEFLATE_SUCCESS) { printf("libdeflate rc=%d out=%zu cap=%zu\n", r, out_n, rawcap); break; }
            itimes.push_back((e - s) * 1000.0);
        }
        libdeflate_free_decompressor(dec);
        if (!itimes.empty()) {
            std::sort(itimes.begin(), itimes.end());
            printf("libdeflate inflate median: %8.2f ms  (IDAT %zu bytes -> %zu bytes raw, %dx%d ct%d bd%d)\n",
                   itimes[itimes.size()/2], idat.size(), rawcap, w, h, ct, bd);
        }
        free(raw); free(fbuf);
        return 0;
    }

    // Inflate-only benchmark (separates zlib inflate cost from unfilter).
    if (inflate_mode) {
        int w, h, ch;
        // need dimensions to size output buffer
        png_structp pp = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        png_infop ip = png_create_info_struct(pp);
        struct Ctx2 { const unsigned char* p; size_t off; size_t size; };
        static Ctx2 c2; c2.p = (const unsigned char*)fbuf; c2.off = 8; c2.size = fsize;
        png_set_sig_bytes(pp, 8);
        png_set_read_fn(pp, &c2, [](png_structp png, png_bytep o, png_size_t sz) {
            Ctx2* c = (Ctx2*)png_get_io_ptr(png);
            if (c->off + sz > c->size) png_error(png, "oob");
            memcpy(o, c->p + c->off, sz); c->off += sz;
        });
        png_read_info(pp, ip);
        w = png_get_image_width(pp, ip); h = png_get_image_height(pp, ip);
        png_uint_32 rb = png_get_rowbytes(pp, ip);
        png_destroy_read_struct(&pp, &ip, NULL);
        size_t rawcap = (size_t)h * (1 + rb);
        unsigned char* raw = (unsigned char*)malloc(rawcap);
        std::vector<double> itimes;
        for (int it = 0; it < iterations; it++) {
            double s = now_sec();
            double prod = bench_inflate_only(fbuf, fsize, rawcap, raw);
            double e = now_sec();
            if (prod < 0) { printf("inflate init failed\n"); free(raw); free(fbuf); return 1; }
            itimes.push_back((e - s) * 1000.0);
        }
        std::sort(itimes.begin(), itimes.end());
        printf("Inflate-only median: %8.2f ms  (zlib, %zu bytes -> %zu bytes raw)\n", itimes[itimes.size()/2], (size_t)fsize, rawcap);
        free(raw); free(fbuf);
        return 0;
    }

    if (verify) {
        int rw, rh, rch;
        void* ref = reference_decode(fbuf, fsize, rw, rh, rch);
        if (!ref) { printf("[verify] reference decode failed\n"); free(fbuf); return 1; }
        // decode once more for comparison
        int cW, cH, cB, cfc, cft; bool anim, oom; void* exif = NULL;
        void* px = PngReader::ReadImage(cW, cH, cB, anim, cfc, cft, exif, oom, fbuf, fsize);
        if (px && cW == rw && cH == rh && cB == rch) {
            size_t n = (size_t)cW * cH * cB;
            if (memcmp(px, ref, n) == 0) printf("[verify] PASS: optimized output byte-identical to reference libpng\n");
            else {
                // find first diff
                size_t diff = 0; for (size_t i = 0; i < n; i++) if (((unsigned char*)px)[i] != ((unsigned char*)ref)[i]) { diff = i; break; }
                printf("[verify] FAIL: first diff at byte %zu (of %zu)\n", diff, n);
            }
        } else {
            printf("[verify] dims mismatch ref=%dx%d ch=%d got=%dx%d ch=%d\n", rw, rh, rch, cW, cH, cB);
        }
        if (px) free(px); if (exif) free(exif);
        free(ref);
    }

    free(fbuf);
    return 0;
}
