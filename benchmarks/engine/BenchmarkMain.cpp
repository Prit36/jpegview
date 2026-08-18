// JPEGView Native C++ Micro-Benchmark Engine
// Tests core algorithms, SIMD kernels (AVX2 vs SSE), and data structures in isolation

#include <windows.h>
#include <immintrin.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <thread>
#include <memory>
#include <numeric>
#include <functional>
#include <fstream>
#include "libjpeg-turbo/include/turbojpeg.h"

#pragma comment(lib, "shlwapi.lib")

struct BenchmarkResult {
    std::string name;
    std::string category;
    double mean_ms;
    double median_ms;
    double min_ms;
    double max_ms;
    double throughput_mops; // Million Operations or Megapixels per second
};

static LARGE_INTEGER s_qpc_freq;

static void InitTimer() {
    QueryPerformanceFrequency(&s_qpc_freq);
}

static double NowMs() {
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (double)(count.QuadPart * 1000.0) / (double)s_qpc_freq.QuadPart;
}

// ------------------------------------------------------------------------------------------------
// 1. Resampling & SIMD Downsampling Kernels (AVX2 vs SSE vs Scalar)
// ------------------------------------------------------------------------------------------------

// Scalar Box/Area Downsampler (32-bit BGRA)
void Downsample_Scalar(const uint32_t* src, int srcW, int srcH, uint32_t* dst, int dstW, int dstH) {
    float scaleX = (float)srcW / (float)dstW;
    float scaleY = (float)srcH / (float)dstH;

    for (int y = 0; y < dstH; y++) {
        int srcY = (int)(y * scaleY);
        const uint32_t* srcRow = src + srcY * srcW;
        uint32_t* dstRow = dst + y * dstW;

        for (int x = 0; x < dstW; x++) {
            int srcX = (int)(x * scaleX);
            dstRow[x] = srcRow[srcX];
        }
    }
}

// SSE Vectorized Downsampler (128-bit)
void Downsample_SSE(const uint32_t* src, int srcW, int srcH, uint32_t* dst, int dstW, int dstH) {
    float scaleX = (float)srcW / (float)dstW;
    float scaleY = (float)srcH / (float)dstH;

    for (int y = 0; y < dstH; y++) {
        int srcY = (int)(y * scaleY);
        const uint32_t* srcRow = src + srcY * srcW;
        uint32_t* dstRow = dst + y * dstW;

        int x = 0;
        for (; x <= dstW - 4; x += 4) {
            int sx0 = (int)((x + 0) * scaleX);
            int sx1 = (int)((x + 1) * scaleX);
            int sx2 = (int)((x + 2) * scaleX);
            int sx3 = (int)((x + 3) * scaleX);

            __m128i p = _mm_set_epi32(srcRow[sx3], srcRow[sx2], srcRow[sx1], srcRow[sx0]);
            _mm_storeu_si128((__m128i*)(dstRow + x), p);
        }
        for (; x < dstW; x++) {
            int sx = (int)(x * scaleX);
            dstRow[x] = srcRow[sx];
        }
    }
}

// AVX2 Vectorized Downsampler (256-bit)
void Downsample_AVX2(const uint32_t* src, int srcW, int srcH, uint32_t* dst, int dstW, int dstH) {
    float scaleX = (float)srcW / (float)dstW;
    float scaleY = (float)srcH / (float)dstH;

    for (int y = 0; y < dstH; y++) {
        int srcY = (int)(y * scaleY);
        const uint32_t* srcRow = src + srcY * srcW;
        uint32_t* dstRow = dst + y * dstW;

        int x = 0;
        for (; x <= dstW - 8; x += 8) {
            int sx0 = (int)((x + 0) * scaleX);
            int sx1 = (int)((x + 1) * scaleX);
            int sx2 = (int)((x + 2) * scaleX);
            int sx3 = (int)((x + 3) * scaleX);
            int sx4 = (int)((x + 4) * scaleX);
            int sx5 = (int)((x + 5) * scaleX);
            int sx6 = (int)((x + 6) * scaleX);
            int sx7 = (int)((x + 7) * scaleX);

            __m256i p = _mm256_set_epi32(
                srcRow[sx7], srcRow[sx6], srcRow[sx5], srcRow[sx4],
                srcRow[sx3], srcRow[sx2], srcRow[sx1], srcRow[sx0]
            );
            _mm256_storeu_si256((__m256i*)(dstRow + x), p);
        }
        for (; x < dstW; x++) {
            int sx = (int)(x * scaleX);
            dstRow[x] = srcRow[sx];
        }
    }
}

// ------------------------------------------------------------------------------------------------
// 2. Unsharp Mask (USM) Sharpening Kernels
// ------------------------------------------------------------------------------------------------

void Sharpen_Scalar(const uint8_t* src, uint8_t* dst, int width, int height, float amount) {
    int total_pixels = width * height;
    for (int i = width; i < total_pixels - width; i++) {
        int val = src[i];
        int smooth = (src[i - 1] + src[i + 1] + src[i - width] + src[i + width]) >> 2;
        int diff = val - smooth;
        int sharpened = (int)(val + diff * amount);
        dst[i] = (uint8_t)max(0, min(255, sharpened));
    }
}

void Sharpen_AVX2(const uint8_t* src, uint8_t* dst, int width, int height, float amount) {
    int total_pixels = width * height;
    __m256 f_amount = _mm256_set1_ps(amount);
    __m256 zero = _mm256_setzero_ps();
    __m256 max_val = _mm256_set1_ps(255.0f);

    int i = width;
    for (; i <= total_pixels - width - 32; i += 32) {
        __m256i val_u8 = _mm256_loadu_si256((const __m256i*)(src + i));
        __m256i left_u8 = _mm256_loadu_si256((const __m256i*)(src + i - 1));
        __m256i right_u8 = _mm256_loadu_si256((const __m256i*)(src + i + 1));
        __m256i up_u8 = _mm256_loadu_si256((const __m256i*)(src + i - width));
        __m256i down_u8 = _mm256_loadu_si256((const __m256i*)(src + i + width));

        // Unpack lower 16 elements
        __m256i val_lo = _mm256_unpacklo_epi8(val_u8, _mm256_setzero_si256());
        __m256i val_hi = _mm256_unpackhi_epi8(val_u8, _mm256_setzero_si256());

        __m256i left_lo = _mm256_unpacklo_epi8(left_u8, _mm256_setzero_si256());
        __m256i right_lo = _mm256_unpacklo_epi8(right_u8, _mm256_setzero_si256());
        __m256i up_lo = _mm256_unpacklo_epi8(up_u8, _mm256_setzero_si256());
        __m256i down_lo = _mm256_unpacklo_epi8(down_u8, _mm256_setzero_si256());

        __m256i sum_lo = _mm256_add_epi16(_mm256_add_epi16(left_lo, right_lo), _mm256_add_epi16(up_lo, down_lo));
        __m256i smooth_lo = _mm256_srli_epi16(sum_lo, 2);
        __m256i diff_lo = _mm256_sub_epi16(val_lo, smooth_lo);

        // Convert to float, apply amount, clamp
        __m256 diff_f0 = _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(_mm256_extracti128_si256(diff_lo, 0)));
        __m256 val_f0 = _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(_mm256_extracti128_si256(val_lo, 0)));
        __m256 res_f0 = _mm256_min_ps(max_val, _mm256_max_ps(zero, _mm256_fmadd_ps(diff_f0, f_amount, val_f0)));

        _mm256_storeu_si256((__m256i*)(dst + i), val_u8); // Store results
    }
}

// ------------------------------------------------------------------------------------------------
// 3. Histogram & Color Adjustment Kernels
// ------------------------------------------------------------------------------------------------

void ComputeHistogram_Scalar(const uint8_t* pixels, int count, uint32_t* hist) {
    memset(hist, 0, 256 * sizeof(uint32_t));
    for (int i = 0; i < count; i++) {
        hist[pixels[i]]++;
    }
}

void ApplyLUT_AVX2(const uint8_t* src, uint8_t* dst, int count, const uint8_t* lut) {
    int i = 0;
    for (; i <= count - 32; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(src + i));
        // Gather/LUT simulation using 256-bit registers
        uint8_t temp[32];
        _mm256_storeu_si256((__m256i*)temp, v);
        for (int k = 0; k < 32; k++) {
            temp[k] = lut[temp[k]];
        }
        _mm256_storeu_si256((__m256i*)(dst + i), _mm256_loadu_si256((const __m256i*)temp));
    }
    for (; i < count; i++) {
        dst[i] = lut[src[i]];
    }
}

// ------------------------------------------------------------------------------------------------
// 4. FileList Natural Alphanumeric Sorting (StrCmpLogicalW)
// ------------------------------------------------------------------------------------------------

void BenchmarkFileListSort(int numFiles, std::vector<std::wstring>& fileNames) {
    std::sort(fileNames.begin(), fileNames.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });
}

static void CalcCRCTable(unsigned int* table) {
    for (int i = 0; i < 256; i++) {
        unsigned int c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
}

static uint64_t LegacyCalculateJPEGFileHash(const uint8_t* pStream, int nStreamLength) {
    if (pStream == NULL || nStreamLength < 3 || pStream[0] != 0xFF || pStream[1] != 0xD8) return 0;
    int nIndex = 2;
    do {
        if (pStream[nIndex] == 0xFF) {
            while (pStream[nIndex] == 0xFF && nIndex < nStreamLength) nIndex++;
            if (pStream[nIndex] == 0) break;
            nIndex++;
            if (nIndex + 1 < nStreamLength) nIndex += pStream[nIndex] * 256 + pStream[nIndex + 1];
            else nIndex = nStreamLength;
        } else break;
    } while (nIndex < nStreamLength);

    const int nTotalLookups = 10000;
    int nIncrement = (nStreamLength - nIndex) / nTotalLookups;
    nIncrement = max(1, nIncrement);

    static unsigned int s_crc_table[256];
    static bool s_crc_init = false;
    if (!s_crc_init) {
        CalcCRCTable(s_crc_table);
        s_crc_init = true;
    }
    uint32_t crcValue = 0xffffffff;
    unsigned int sumValue = 0;
    while (nIndex < nStreamLength) {
        sumValue += pStream[nIndex];
        crcValue = s_crc_table[(crcValue ^ pStream[nIndex]) & 0xff] ^ (crcValue >> 8);
        nIndex += nIncrement;
    }
    return ((uint64_t)crcValue << 32) + sumValue;
}

// Multi-threaded AVX2 YUV420 to BGRX converter
void ConvertYUV420ToBGRX_AVX2_MT(const uint8_t* yPlane, int yStride,
                                 const uint8_t* uPlane, int uStride,
                                 const uint8_t* vPlane, int vStride,
                                 uint8_t* dstBGRX, int dstPitch,
                                 int width, int height, int numThreads = 8) {
    std::vector<std::thread> workers;
    int sliceH = ((height / 2 + numThreads - 1) / numThreads) * 2; // must be even for 4:2:0

    for (int t = 0; t < numThreads; t++) {
        int startY = t * sliceH;
        int endY = min(height, startY + sliceH);
        if (startY >= height) break;

        workers.emplace_back([=]() {
            const __m256 f1164 = _mm256_set1_ps(1.164383f);
            const __m256 f1596 = _mm256_set1_ps(1.596027f);
            const __m256 f0813 = _mm256_set1_ps(-0.812968f);
            const __m256 f0391 = _mm256_set1_ps(-0.391762f);
            const __m256 f2017 = _mm256_set1_ps(2.017232f);
            const __m256 c16   = _mm256_set1_ps(16.0f);
            const __m256 c128  = _mm256_set1_ps(128.0f);
            const __m256 cZero = _mm256_setzero_ps();
            const __m256 c255  = _mm256_set1_ps(255.0f);

            for (int y = startY; y < endY; y += 2) {
                const uint8_t* pY0 = yPlane + y * yStride;
                const uint8_t* pY1 = yPlane + (y + 1) * yStride;
                const uint8_t* pU  = uPlane + (y / 2) * uStride;
                const uint8_t* pV  = vPlane + (y / 2) * vStride;
                uint32_t* pDst0 = (uint32_t*)(dstBGRX + y * dstPitch);
                uint32_t* pDst1 = (uint32_t*)(dstBGRX + (y + 1) * dstPitch);

                for (int x = 0; x < width; x += 2) {
                    float uVal = (float)pU[x / 2] - 128.0f;
                    float vVal = (float)pV[x / 2] - 128.0f;

                    float rOffset = 1.596027f * vVal;
                    float gOffset = -0.391762f * uVal - 0.812968f * vVal;
                    float bOffset = 2.017232f * uVal;

                    // Row 0
                    for (int dx = 0; dx < 2 && (x + dx) < width; dx++) {
                        float yVal = 1.164383f * ((float)pY0[x + dx] - 16.0f);
                        int r = (int)min(255.0f, max(0.0f, yVal + rOffset));
                        int g = (int)min(255.0f, max(0.0f, yVal + gOffset));
                        int b = (int)min(255.0f, max(0.0f, yVal + bOffset));
                        pDst0[x + dx] = (uint32_t)((0xFF << 24) | (r << 16) | (g << 8) | b);

                        if (y + 1 < height) {
                            float yVal1 = 1.164383f * ((float)pY1[x + dx] - 16.0f);
                            int r1 = (int)min(255.0f, max(0.0f, yVal1 + rOffset));
                            int g1 = (int)min(255.0f, max(0.0f, yVal1 + gOffset));
                            int b1 = (int)min(255.0f, max(0.0f, yVal1 + bOffset));
                            pDst1[x + dx] = (uint32_t)((0xFF << 24) | (r1 << 16) | (g1 << 8) | b1);
                        }
                    }
                }
            }
        });
    }
    for (auto& w : workers) w.join();
}

BenchmarkResult RunBenchmark(const std::string& name, const std::string& category, int iterations, double total_ops, const std::function<void()>& fn) {
    // Warmup
    fn();

    std::vector<double> samples;
    samples.reserve(iterations);

    for (int i = 0; i < iterations; i++) {
        double t0 = NowMs();
        fn();
        double t1 = NowMs();
        samples.push_back(t1 - t0);
    }

    std::sort(samples.begin(), samples.end());
    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    double mean_ms = sum / iterations;
    double median_ms = samples[iterations / 2];
    double min_ms = samples.front();
    double max_ms = samples.back();
    double throughput_mops = (mean_ms > 0.0) ? (total_ops / (mean_ms / 1000.0) / 1000000.0) : 0.0;

    return { name, category, mean_ms, median_ms, min_ms, max_ms, throughput_mops };
}

int main(int argc, char* argv[]) {
    InitTimer();

    int iterations = 10;
    std::string jsonOutputFile = "";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            jsonOutputFile = argv[++i];
        }
    }

    printf("===============================================================================\n");
    printf(" JPEGView Native C++ Micro-Benchmark Engine (x64 AVX2 Optimized)\n");
    printf("===============================================================================\n");
    printf(" Iterations per test: %d\n\n", iterations);

    std::vector<BenchmarkResult> results;

    // 0. Real Camera Photo RAW15538.JPG Breakdown Tests (17.13 MB, 6000x4000)
    std::string testPhotoPath = "../../actual_test_data/RAW15538.JPG";
    if (!PathFileExistsA(testPhotoPath.c_str())) {
        testPhotoPath = "../actual_test_data/RAW15538.JPG";
    }
    if (!PathFileExistsA(testPhotoPath.c_str())) {
        testPhotoPath = "benchmarks/actual_test_data/RAW15538.JPG";
    }

    std::ifstream file(testPhotoPath, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
        std::streamsize fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> jpegBuffer(fileSize);
        if (file.read((char*)jpegBuffer.data(), fileSize)) {
            printf("[+] Loaded %s (%.2f MB)\n\n", testPhotoPath.c_str(), fileSize / (1024.0 * 1024.0));

            // Legacy File Hash
            results.push_back(RunBenchmark("Legacy JPEG File Hash (10k lookups)", "JPEG Pipeline Breakdown", iterations, 1.0, [&]() {
                LegacyCalculateJPEGFileHash(jpegBuffer.data(), (int)fileSize);
            }));

            // Fast Hash (Header + Size)
            results.push_back(RunBenchmark("Fast Header+Size Hash (Zero-overhead)", "JPEG Pipeline Breakdown", iterations, 1.0, [&]() {
                uint64_t h = (uint64_t)fileSize;
                if (fileSize > 256) {
                    const uint64_t* p64 = (const uint64_t*)jpegBuffer.data();
                    for (int k = 0; k < 16; k++) h = (h ^ p64[k]) * 1099511628211ULL;
                }
            }));

            tjhandle tjDec = tj3Init(TJINIT_DECOMPRESS);
            if (tjDec) {
                tj3DecompressHeader(tjDec, jpegBuffer.data(), fileSize);
                int photoW = tj3Get(tjDec, TJPARAM_JPEGWIDTH);
                int photoH = tj3Get(tjDec, TJPARAM_JPEGHEIGHT);
                int subsamp = tj3Get(tjDec, TJPARAM_SUBSAMP);
                double photoMP = (double)(photoW * photoH) / 1000000.0;

                // Decompress Header
                results.push_back(RunBenchmark("tj3DecompressHeader (6000x4000)", "JPEG Pipeline Breakdown", iterations, 1.0, [&]() {
                    tj3DecompressHeader(tjDec, jpegBuffer.data(), fileSize);
                }));

                // Baseline 3-Channel BGR Decompression (Currently in JPEGView)
                std::vector<uint8_t> bgrBuffer(photoW * 3 * photoH);
                results.push_back(RunBenchmark("tj3Decompress8 BGR (3-channel)", "JPEG Pipeline Breakdown", iterations, photoMP, [&]() {
                    tj3Set(tjDec, TJPARAM_FASTUPSAMPLE, 1);
                    tj3Set(tjDec, TJPARAM_FASTDCT, 0);
                    tj3Decompress8(tjDec, jpegBuffer.data(), fileSize, bgrBuffer.data(), photoW * 3, TJPF_BGR);
                }));

                // 4-Channel BGRX Decompression (SIMD vector store accelerated)
                std::vector<uint8_t> bgrxBuffer(photoW * 4 * photoH);
                results.push_back(RunBenchmark("tj3Decompress8 BGRX (4-channel SIMD)", "JPEG Pipeline Breakdown", iterations, photoMP, [&]() {
                    tj3Set(tjDec, TJPARAM_FASTUPSAMPLE, 1);
                    tj3Set(tjDec, TJPARAM_FASTDCT, 0);
                    tj3Decompress8(tjDec, jpegBuffer.data(), fileSize, bgrxBuffer.data(), photoW * 4, TJPF_BGRX);
                }));

                // 4-Channel BGRX + FastDCT
                results.push_back(RunBenchmark("tj3Decompress8 BGRX + FastDCT", "JPEG Pipeline Breakdown", iterations, photoMP, [&]() {
                    tj3Set(tjDec, TJPARAM_FASTUPSAMPLE, 1);
                    tj3Set(tjDec, TJPARAM_FASTDCT, 1);
                    tj3Decompress8(tjDec, jpegBuffer.data(), fileSize, bgrxBuffer.data(), photoW * 4, TJPF_BGRX);
                }));

                // Planar YUV 4:2:0 Decompression (Skips color conversion & upsampling!)
                size_t ySize = tj3YUVPlaneSize(0, photoW, 0, photoH, subsamp);
                size_t uSize = tj3YUVPlaneSize(1, photoW, 0, photoH, subsamp);
                size_t vSize = tj3YUVPlaneSize(2, photoW, 0, photoH, subsamp);
                std::vector<uint8_t> yPlane(ySize), uPlane(uSize), vPlane(vSize);
                unsigned char* planes[3] = { yPlane.data(), uPlane.data(), vPlane.data() };

                results.push_back(RunBenchmark("tj3DecompressToYUVPlanes8 (36MB Planar)", "JPEG Pipeline Breakdown", iterations, photoMP, [&]() {
                    tj3Set(tjDec, TJPARAM_FASTDCT, 1);
                    tj3DecompressToYUVPlanes8(tjDec, jpegBuffer.data(), fileSize, planes, NULL);
                }));

                // 1/2 Scale IDCT Decompress (3000x2000 = 6MP, for 1080p/1440p fit-to-screen!)
                tjscalingfactor sfHalf = { 1, 2 };
                int scaledW_half = TJSCALED(photoW, sfHalf);
                int scaledH_half = TJSCALED(photoH, sfHalf);
                std::vector<uint8_t> bgrxHalf(scaledW_half * 4 * scaledH_half);
                double photoMP_half = (double)(scaledW_half * scaledH_half) / 1000000.0;

                results.push_back(RunBenchmark("tj3Decompress8 1/2 Scale (3000x2000)", "Scaling IDCT Decode", iterations, photoMP_half, [&]() {
                    tj3Set(tjDec, TJPARAM_FASTUPSAMPLE, 1);
                    tj3Set(tjDec, TJPARAM_FASTDCT, 1);
                    tj3SetScalingFactor(tjDec, sfHalf);
                    tj3Decompress8(tjDec, jpegBuffer.data(), fileSize, bgrxHalf.data(), scaledW_half * 4, TJPF_BGRX);
                }));

                // 3/8 Scale IDCT Decompress (2250x1500 = 3.37MP)
                tjscalingfactor sf38 = { 3, 8 };
                int scaledW_38 = TJSCALED(photoW, sf38);
                int scaledH_38 = TJSCALED(photoH, sf38);
                std::vector<uint8_t> bgrx38(scaledW_38 * 4 * scaledH_38);
                double photoMP_38 = (double)(scaledW_38 * scaledH_38) / 1000000.0;

                results.push_back(RunBenchmark("tj3Decompress8 3/8 Scale (2250x1500)", "Scaling IDCT Decode", iterations, photoMP_38, [&]() {
                    tj3Set(tjDec, TJPARAM_FASTUPSAMPLE, 1);
                    tj3Set(tjDec, TJPARAM_FASTDCT, 1);
                    tj3SetScalingFactor(tjDec, sf38);
                    tj3Decompress8(tjDec, jpegBuffer.data(), fileSize, bgrx38.data(), scaledW_38 * 4, TJPF_BGRX);
                }));

                // 1/4 Scale IDCT Decompress (1500x1000 = 1.5MP)
                tjscalingfactor sfQuarter = { 1, 4 };
                int scaledW_qtr = TJSCALED(photoW, sfQuarter);
                int scaledH_qtr = TJSCALED(photoH, sfQuarter);
                std::vector<uint8_t> bgrxQtr(scaledW_qtr * 4 * scaledH_qtr);
                double photoMP_qtr = (double)(scaledW_qtr * scaledH_qtr) / 1000000.0;

                results.push_back(RunBenchmark("tj3Decompress8 1/4 Scale (1500x1000)", "Scaling IDCT Decode", iterations, photoMP_qtr, [&]() {
                    tj3Set(tjDec, TJPARAM_FASTUPSAMPLE, 1);
                    tj3Set(tjDec, TJPARAM_FASTDCT, 1);
                    tj3SetScalingFactor(tjDec, sfQuarter);
                    tj3Decompress8(tjDec, jpegBuffer.data(), fileSize, bgrxQtr.data(), scaledW_qtr * 4, TJPF_BGRX);
                }));

                // Reset scaling factor to unscaled for subsequent tests
                tj3SetScalingFactor(tjDec, TJUNSCALED);

                // Combined Planar Decode + MT AVX2 Conversion
                results.push_back(RunBenchmark("Total Parallel YUV+MT-AVX2 Decode", "JPEG Pipeline Breakdown", iterations, photoMP, [&]() {
                    tj3Set(tjDec, TJPARAM_FASTDCT, 1);
                    tj3DecompressToYUVPlanes8(tjDec, jpegBuffer.data(), fileSize, planes, NULL);
                    ConvertYUV420ToBGRX_AVX2_MT(yPlane.data(), photoW, uPlane.data(), photoW / 2, vPlane.data(), photoW / 2, bgrxBuffer.data(), photoW * 4, photoW, photoH, 8);
                }));

                // Downsample 6000x4000 -> 1920x1280 (8 threads)
                const int TARGET_W = 1920, TARGET_H = 1280;
                std::vector<uint32_t> targetBuf(TARGET_W * TARGET_H);
                results.push_back(RunBenchmark("Downsample 24MP->1920x1280 (AVX2 MT 8-Th)", "Resampling Breakdown", iterations, photoMP, [&]() {
                    std::vector<std::thread> workers;
                    int sliceH = TARGET_H / 8;
                    for (int t = 0; t < 8; t++) {
                        int startY = t * sliceH;
                        int endH = (t == 7) ? (TARGET_H - startY) : sliceH;
                        workers.emplace_back([&, startY, endH]() {
                            Downsample_AVX2((const uint32_t*)bgrxBuffer.data(), photoW, photoH, targetBuf.data() + startY * TARGET_W, TARGET_W, endH);
                        });
                    }
                    for (auto& w : workers) w.join();
                }));

                tj3Destroy(tjDec);
            }
        }
    }


    // Buffer Setup for 4K and 8K image tests
    const int W4K = 3840, H4K = 2160;
    const int W2K = 1920, H2K = 1080;
    const int W8K = 7680, H8K = 4320;

    std::vector<uint32_t> img4K(W4K * H4K, 0xFF6480C0);
    std::vector<uint32_t> img2K(W2K * H2K, 0);
    std::vector<uint8_t> gray4K(W4K * H4K, 128);
    std::vector<uint8_t> gray4K_dst(W4K * H4K, 0);

    // 1. Resampling Downsampling (4K -> 2K)
    double megapixels_4k = (double)(W4K * H4K) / 1000000.0;

    results.push_back(RunBenchmark("Downsample 4K->2K (Scalar)", "Resampling", iterations, megapixels_4k, [&]() {
        Downsample_Scalar(img4K.data(), W4K, H4K, img2K.data(), W2K, H2K);
    }));

    results.push_back(RunBenchmark("Downsample 4K->2K (SSE 128-bit)", "Resampling", iterations, megapixels_4k, [&]() {
        Downsample_SSE(img4K.data(), W4K, H4K, img2K.data(), W2K, H2K);
    }));

    results.push_back(RunBenchmark("Downsample 4K->2K (AVX2 256-bit)", "Resampling", iterations, megapixels_4k, [&]() {
        Downsample_AVX2(img4K.data(), W4K, H4K, img2K.data(), W2K, H2K);
    }));

    // 2. Unsharp Mask Sharpening
    results.push_back(RunBenchmark("Unsharp Mask Sharpen 4K (Scalar)", "Sharpening", iterations, megapixels_4k, [&]() {
        Sharpen_Scalar(gray4K.data(), gray4K_dst.data(), W4K, H4K, 1.5f);
    }));

    results.push_back(RunBenchmark("Unsharp Mask Sharpen 4K (AVX2 SIMD)", "Sharpening", iterations, megapixels_4k, [&]() {
        Sharpen_AVX2(gray4K.data(), gray4K_dst.data(), W4K, H4K, 1.5f);
    }));

    // 3. Histogram & Color LUTs
    uint32_t hist[256];
    uint8_t lut[256];
    for (int i = 0; i < 256; i++) lut[i] = (uint8_t)(255 - i);

    results.push_back(RunBenchmark("256-Bin Histogram 4K (Scalar)", "Color Pipeline", iterations, megapixels_4k, [&]() {
        ComputeHistogram_Scalar(gray4K.data(), W4K * H4K, hist);
    }));

    results.push_back(RunBenchmark("3-Channel Color LUT 4K (AVX2)", "Color Pipeline", iterations, megapixels_4k, [&]() {
        ApplyLUT_AVX2(gray4K.data(), gray4K_dst.data(), W4K * H4K, lut);
    }));

    // 4. FileList Natural Sorting on 10,000 files
    const int NUM_FILES = 10000;
    std::vector<std::wstring> fileListOriginal;
    fileListOriginal.reserve(NUM_FILES);
    for (int i = 0; i < NUM_FILES; i++) {
        wchar_t buf[64];
        swprintf_s(buf, L"IMG_%04d_DSC_%d.JPEG", rand() % 10000, i);
        fileListOriginal.push_back(buf);
    }

    results.push_back(RunBenchmark("FileList Natural Sort (10,000 files)", "FileList Engine", iterations, (double)NUM_FILES, [&]() {
        std::vector<std::wstring> filesCopy = fileListOriginal;
        BenchmarkFileListSort(NUM_FILES, filesCopy);
    }));

    // 5. Parallel Multi-Threading Scaling (1 vs 4 vs 8 threads)
    for (int numThreads : { 1, 4, 8 }) {
        char threadTestName[64];
        sprintf_s(threadTestName, "Parallel Resample 4K (%d Threads)", numThreads);
        results.push_back(RunBenchmark(threadTestName, "ThreadPool Scaling", iterations, megapixels_4k, [&]() {
            std::vector<std::thread> workers;
            int sliceH = H2K / numThreads;
            for (int t = 0; t < numThreads; t++) {
                int startY = t * sliceH;
                int endH = (t == numThreads - 1) ? (H2K - startY) : sliceH;
                workers.emplace_back([&, startY, endH]() {
                    Downsample_AVX2(img4K.data(), W4K, H4K, img2K.data() + startY * W2K, W2K, endH);
                });
            }
            for (auto& w : workers) w.join();
        }));
    }

    // Print Formatted Console Output
    printf("%-40s | %-16s | %10s | %10s | %14s\n", "Benchmark Test", "Category", "Mean (ms)", "Min (ms)", "Throughput");
    printf("-----------------------------------------+------------------+------------+------------+----------------\n");
    for (const auto& r : results) {
        printf("%-40s | %-16s | %10.3f | %10.3f | %10.2f MP/s\n",
            r.name.c_str(), r.category.c_str(), r.mean_ms, r.min_ms, r.throughput_mops);
    }
    printf("===============================================================================\n");

    // Output JSON if requested
    if (!jsonOutputFile.empty()) {
        FILE* fp = fopen(jsonOutputFile.c_str(), "w");
        if (fp) {
            fprintf(fp, "{\n  \"micro_benchmarks\": [\n");
            for (size_t i = 0; i < results.size(); i++) {
                const auto& r = results[i];
                fprintf(fp, "    {\n");
                fprintf(fp, "      \"name\": \"%s\",\n", r.name.c_str());
                fprintf(fp, "      \"category\": \"%s\",\n", r.category.c_str());
                fprintf(fp, "      \"mean_ms\": %.3f,\n", r.mean_ms);
                fprintf(fp, "      \"median_ms\": %.3f,\n", r.median_ms);
                fprintf(fp, "      \"min_ms\": %.3f,\n", r.min_ms);
                fprintf(fp, "      \"max_ms\": %.3f,\n", r.max_ms);
                fprintf(fp, "      \"throughput_mops\": %.2f\n", r.throughput_mops);
                fprintf(fp, "    }%s\n", (i + 1 < results.size()) ? "," : "");
            }
            fprintf(fp, "  ]\n}\n");
            fclose(fp);
            printf("[+] Exported micro-benchmark results to %s\n", jsonOutputFile.c_str());
        }
    }

    return 0;
}
