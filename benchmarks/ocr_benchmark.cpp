#define NOMINMAX
#include "../src/JPEGView/OcrEngine.h"
#include <OcrBaseline.h>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>

using Clock = std::chrono::steady_clock;
volatile uint64_t sink = 0;
std::ofstream raw("samples.csv");
std::string currentFile;
struct Image {
    int width, height, channels;
    size_t stride;
    std::vector<uint8_t> pixels;
    Image(int w, int h, int c) : width(w), height(h), channels(c),
        stride((size_t(w) * c + 3) & ~size_t(3)), pixels(stride * h) {}
};
double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
double Percentile(std::vector<double> samples, double p) {
    std::sort(samples.begin(), samples.end());
    return samples[size_t(std::ceil(p * samples.size())) - 1];
}
void Report(const char* stage, const Image& image, const std::vector<double>& before,
            const std::vector<double>& after, size_t count) {
    const double b = Percentile(before, .5), a = Percentile(after, .5);
    for (size_t i = 0; i < before.size(); ++i) {
        if (!currentFile.empty()) raw << '"' << currentFile << "\",";
        raw << stage << ',' << image.width << ',' << image.height << ',' << image.channels
            << ',' << i << ',' << before[i] << ',' << after[i] << '\n';
    }
    if (!currentFile.empty()) std::cout << '"' << currentFile << "\",";
    std::cout << stage << ',' << image.width << ',' << image.height << ',' << image.channels
        << ',' << before.size() << ',' << b << ',' << a << ',' << b / a << ','
        << Percentile(before, .95) << ',' << Percentile(after, .95) << ',' << count << '\n';
}
void CheckSnapshot(const Image& image) {
    auto b = OcrBaseline::MakeSnapshot(image.pixels.data(), image.width, image.height, image.channels);
    auto a = Ocr::MakeSnapshot(image.pixels.data(), image.width, image.height, image.channels);
    if (a.width != b.width || a.height != b.height || a.imageWidth != b.imageWidth ||
        a.imageHeight != b.imageHeight || a.gray != b.gray)
        throw std::runtime_error("Snapshot pixel equivalence failed: " + std::to_string(image.width) +
                                 "x" + std::to_string(image.height) + " channels=" + std::to_string(image.channels));
}
template<typename Make> double TimeSnapshots(const Image& image, Make make, int repeats) {
    const auto start = Clock::now();
    for (int i = 0; i < repeats; ++i) {
        auto snapshot = make(image.pixels.data(), image.width, image.height, image.channels);
        sink = sink + snapshot.gray[size_t(i) % snapshot.gray.size()];
    }
    return Milliseconds(start) / repeats;
}
void Micro(const Image& image) {
    CheckSnapshot(image);
    for (int i = 0; i < 3; ++i) {
        TimeSnapshots(image, OcrBaseline::MakeSnapshot, 1);
        TimeSnapshots(image, Ocr::MakeSnapshot, 1);
    }
    std::vector<double> before, after;
    for (int i = 0; i < 40; ++i) {
        double b, a;
        if (i % 2) {
            a = TimeSnapshots(image, Ocr::MakeSnapshot, 3);
            b = TimeSnapshots(image, OcrBaseline::MakeSnapshot, 3);
        } else {
            b = TimeSnapshots(image, OcrBaseline::MakeSnapshot, 3);
            a = TimeSnapshots(image, Ocr::MakeSnapshot, 3);
        }
        before.push_back(b); after.push_back(a);
    }
    Report("snapshot", image, before, after, 0);
}
Image TextImage(int width, int height, int channels) {
    Image image(width, height, channels);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!bitmap || !dc) throw std::runtime_error("Cannot create OCR fixture");
    HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    RECT bounds{0, 0, width, height};
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const int fontSize = std::max(24, width / 50);
    HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ previousFont = SelectObject(dc, font);
    SetTextColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    const wchar_t* lines[] = {L"JPEGView OCR performance benchmark", L"The quick brown fox jumps over the lazy dog.",
        L"Invoice 12345 Date 2026 October 04", L"Local text recognition with Windows OCR", L"Total amount 987.65 Quantity 42"};
    for (int y = fontSize; y < height - fontSize; y += fontSize * 2) {
        const auto text = lines[(y / (fontSize * 2)) % 5];
        TextOutW(dc, fontSize, y, text, int(wcslen(text)));
    }
    GdiFlush();
    const auto source = static_cast<const uint8_t*>(pixels);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < channels; ++c)
                image.pixels[size_t(y) * image.stride + size_t(x) * channels + c] =
                    source[(size_t(y) * width + x) * 4 + c];
    SelectObject(dc, previousFont); SelectObject(dc, previousBitmap);
    DeleteObject(font); DeleteObject(bitmap); DeleteDC(dc);
    return image;
}
template<typename Make, typename Recognize> auto TimeOcr(const Image& image, Make make, Recognize recognize) {
    const auto start = Clock::now();
    auto snapshot = make(image.pixels.data(), image.width, image.height, image.channels);
    std::atomic<bool> canceled{false};
    auto result = recognize(snapshot, canceled);
    const double elapsed = Milliseconds(start);
    if (!result.error.empty() || result.language.empty())
        throw std::runtime_error("Windows OCR failed; no recognition timing claim is possible");
    return std::make_pair(elapsed, std::move(result));
}
void Recognition(const Image& image, bool allowEmpty = false, int pairs = 24) {
    CheckSnapshot(image);
    auto firstB = TimeOcr(image, OcrBaseline::MakeSnapshot, OcrBaseline::Recognize);
    auto firstA = TimeOcr(image, Ocr::MakeSnapshot, Ocr::Recognize);
    if (!allowEmpty && firstA.second.words.empty())
        throw std::runtime_error("Synthetic text fixture returned no words");
    // Exclude first calls from timing: Windows OCR initialization has a cold-start cost.
    std::vector<double> before, after;
    auto check = [](const auto& b, const auto& a) {
        if (b.language != a.language || b.words.size() != a.words.size())
            throw std::runtime_error("OCR results differ");
        for (size_t i = 0; i < b.words.size(); ++i) {
            if (b.words[i].text != a.words[i].text || b.words[i].line != a.words[i].line)
                throw std::runtime_error("OCR text/order differs");
            for (size_t j = 0; j < 4; ++j)
                if (b.words[i].corners[j].x != a.words[i].corners[j].x ||
                    b.words[i].corners[j].y != a.words[i].corners[j].y)
                    throw std::runtime_error("OCR geometry differs");
        }
    };
    check(firstB.second, firstA.second);
    std::wcerr << L"OCR fixture " << image.width << L"x" << image.height << L": "
        << firstA.second.language << L", " << firstA.second.words.size() << L" words\n";
    for (int i = 0; i < pairs; ++i) {
        decltype(firstB) b; decltype(firstA) a;
        if (i % 2) {
            a = TimeOcr(image, Ocr::MakeSnapshot, Ocr::Recognize);
            b = TimeOcr(image, OcrBaseline::MakeSnapshot, OcrBaseline::Recognize);
        } else {
            b = TimeOcr(image, OcrBaseline::MakeSnapshot, OcrBaseline::Recognize);
            a = TimeOcr(image, Ocr::MakeSnapshot, Ocr::Recognize);
        }
        check(b.second, a.second);
        before.push_back(b.first); after.push_back(a.first);
    }
    Report("snapshot_plus_windows_ocr", image, before, after, firstA.second.words.size());
}
#ifndef OCR_FILE_BENCHMARK
int main(int argc, char**) {
    try {
        std::cout << std::fixed << std::setprecision(6);
        raw << std::fixed << std::setprecision(6);
        raw << "stage,width,height,channels,pair,before_ms,after_ms\n";
        std::mt19937 random(12345);
        // Include one-pixel axes, padded BGR rows, limit boundaries and awkward aspect ratios.
        const int sizes[][2] = {{1, 1}, {1, 2051}, {2051, 1}, {17, 31}, {2047, 19}, {2048, 23},
            {2049, 33}, {33, 2049}, {2051, 2063}, {4097, 2191}, {97, 3001}};
        for (auto& size : sizes) for (int channels : {3, 4}) {
            Image image(size[0], size[1], channels);
            for (auto& p : image.pixels) p = uint8_t(random());
            CheckSnapshot(image);
        }
        for (int i = 0; i < 64; ++i) {
            Image image(1 + int(random() % 5000), 1 + int(random() % 97), i % 2 ? 3 : 4);
            for (auto& p : image.pixels) p = uint8_t(random());
            CheckSnapshot(image);
        }
        std::cout << "stage,width,height,channels,pairs,before_median_ms,after_median_ms,speedup,before_p95_ms,after_p95_ms,words\n";
        const int cases[][3] = {{800, 600, 3}, {1920, 1080, 3}, {1920, 1080, 4},
            {2048, 2048, 4}, {3840, 2160, 3}, {3840, 2160, 4}, {6000, 4000, 3}, {6000, 4000, 4}};
        for (auto& size : cases) {
            Image image(size[0], size[1], size[2]);
            for (auto& p : image.pixels) p = uint8_t(random());
            Micro(image);
        }
        if (argc == 1)
            for (auto& size : std::vector<std::array<int, 3>>{{800, 600, 3}, {1920, 1080, 4},
                                                           {3840, 2160, 4}, {6000, 4000, 3}})
                Recognition(TextImage(size[0], size[1], size[2]));
        std::cerr << "PASS: snapshots byte-identical on all tested inputs.\n";
        if (argc == 1) std::cerr << "PASS: OCR language/text/order/geometry identical on all measured fixtures.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
#endif
