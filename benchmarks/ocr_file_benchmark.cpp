// Links the actual application Release objects to decode files exactly as the viewer does.
#include "stdafx.h"
#include "JPEGProvider.h"
#include "JPEGImage.h"
#include "ProcessingThreadPool.h"
#include "ResizeFilter.h"
#include "SettingsProvider.h"
#include "GpuHeifDecoder.h"
#include <gdiplus.h>
#undef min
#undef max
#include <filesystem>
#include <cstring>
#define OCR_FILE_BENCHMARK
#include "ocr_benchmark.cpp"

Image Load(const std::filesystem::path& path) {
    CJPEGProvider provider(nullptr, 1, 1);
    CImageProcessingParams imageParams(0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    CProcessParams params(1920, 1080, CSize(1920, 1080), CRotationParams(0), 0, -1,
                         Helpers::ZM_FitToScreenNoZoom, CPoint(0, 0), imageParams,
                         PFLAG_NoProcessingAfterLoad);
    bool oom = false, exception = false;
    auto image = provider.RequestImage(nullptr, CJPEGProvider::NONE, path.c_str(), 0,
                                      params, oom, exception);
    if (!image) throw std::runtime_error(oom ? "Decoder ran out of memory" :
                                        exception ? "Decoder exception" : "Decoder could not load image");
    // Matches COcrController::Toggle: apply deferred rotation before reading dimensions.
    const auto pixels = image->OriginalPixels();
    Image result(image->OrigWidth(), image->OrigHeight(), image->OriginalChannels());
    std::memcpy(result.pixels.data(), pixels, result.pixels.size());
    provider.NotifyNotUsed(image);
    return result;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    bool failed = false;
    bool validateOnly = false;
    for (int i = 2; i < argc; ++i) validateOnly |= std::wstring(argv[i]) == L"--validate-only";
    try {
        std::cout << std::fixed << std::setprecision(6);
        raw << std::fixed << std::setprecision(6);
        raw << "file,stage,width,height,channels,pair,before_ms,after_ms\n";
        std::cout << "file,stage,width,height,channels,pairs,before_median_ms,after_median_ms,speedup,before_p95_ms,after_p95_ms,words\n";
        _Module.Init(nullptr, GetModuleHandle(nullptr));
        CSettingsProvider::This();
        // CMainDlg initializes this singleton before decoder/processing threads
        // start. Otherwise simultaneous first use can register duplicate exit cleanup.
        CResizeFilterCache::This();
        CProcessingThreadPool::This().CreateThreadPoolThreads();
        ULONG_PTR gdiplusToken = 0;
        Gdiplus::GdiplusStartupInput gdiplusInput;
        if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok)
            throw std::runtime_error("GDI+ initialization failed");
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1]))
            if (entry.is_regular_file()) paths.push_back(entry.path());
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            const auto name = std::filesystem::relative(path, argv[1]).generic_u8string();
            currentFile.assign(reinterpret_cast<const char*>(name.data()), name.size());
            try {
                std::cerr << "MEASURING: " << currentFile << '\n';
                const auto image = Load(path);
                if (validateOnly) CheckSnapshot(image);
                else Micro(image);
                Recognition(image, true, validateOnly ? 1 : 24);
                std::cerr << "PASS: " << currentFile << ": identical snapshot and OCR results\n";
            } catch (const std::exception& error) {
                failed = true;
                std::cerr << "FAIL: " << currentFile << ": " << error.what() << '\n';
                std::cout << '"' << currentFile << "\",error,0,0,0,0,0,0,0,0,0,0\n";
            }
            std::cout.flush(); raw.flush(); std::cerr.flush();
        }
        std::cerr << "CLEANUP: waiting for HEIC initialization\n";
        GpuHeifDecoder::WaitForHardwareInitialization();
        std::cerr << "CLEANUP: stopping processing threads\n";
        CProcessingThreadPool::This().StopAllThreads();
        std::cerr << "CLEANUP: shutting down GDI+\n";
        Gdiplus::GdiplusShutdown(gdiplusToken);
        std::cerr << "CLEANUP: terminating ATL module\n";
        _Module.Term();
        std::cerr << "CLEANUP: complete\n";
        if (paths.empty()) throw std::runtime_error("No files found");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failed ? 1 : 0;
}
