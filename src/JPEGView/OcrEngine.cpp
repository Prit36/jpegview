#define NOMINMAX
#include "OcrEngine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <MemoryBuffer.h>

namespace Ocr {
Snapshot MakeSnapshot(const void* pixels, int width, int height, int channels) {
    if (!pixels || width <= 0 || height <= 0 || (channels != 3 && channels != 4))
        throw std::invalid_argument("Invalid OCR image");
    const double scale = std::min(1.0, double(SnapshotLimit) / std::max(width, height));
    Snapshot result;
    result.imageWidth = width;
    result.imageHeight = height;
    result.width = std::max(1, int(width * scale));
    result.height = std::max(1, int(height * scale));
    result.gray.resize(size_t(result.width) * result.height);
    const size_t stride = (size_t(width) * channels + 3) & ~size_t(3);
    auto source = static_cast<const uint8_t*>(pixels);
    auto luminance = [](const uint8_t* p) { return (29 * p[0] + 150 * p[1] + 77 * p[2]) / 256.0; };
    for (int y = 0; y < result.height; ++y) {
        const double sy = std::max(0.0, (y + 0.5) * height / result.height - 0.5);
        const int y0 = int(sy), y1 = std::min(y0 + 1, height - 1);
        const double fy = sy - y0;
        for (int x = 0; x < result.width; ++x) {
            const double sx = std::max(0.0, (x + 0.5) * width / result.width - 0.5);
            const int x0 = int(sx), x1 = std::min(x0 + 1, width - 1);
            const double fx = sx - x0;
            const auto a = source + size_t(y0) * stride;
            const auto b = source + size_t(y1) * stride;
            const double top = luminance(a + size_t(x0) * channels) * (1 - fx) + luminance(a + size_t(x1) * channels) * fx;
            const double bottom = luminance(b + size_t(x0) * channels) * (1 - fx) + luminance(b + size_t(x1) * channels) * fx;
            result.gray[size_t(y) * result.width + x] = uint8_t(top * (1 - fy) + bottom * fy + 0.5);
        }
    }
    return result;
}

Result Recognize(const Snapshot& image, const std::atomic<bool>& canceled) {
    Result result;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
        using namespace winrt::Windows::Graphics::Imaging;
        using winrt::Windows::Media::Ocr::OcrEngine;
        if (canceled.load()) return result;
        auto engine = OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) {
            const auto languages = OcrEngine::AvailableRecognizerLanguages();
            if (languages.Size()) engine = OcrEngine::TryCreateFromLanguage(languages.GetAt(0));
        }
        if (!engine) {
            result.error = L"No Windows OCR language is installed. Add a supported language in Windows Settings > Time & language > Language & region.";
            return result;
        }
        if (image.width <= 0 || image.height <= 0 || image.gray.size() != size_t(image.width) * image.height)
            throw std::invalid_argument("Invalid OCR snapshot");
        const int limit = int(OcrEngine::MaxImageDimension());
        const double scale = std::min(1.0, double(limit) / std::max(image.width, image.height));
        const int width = std::max(1, int(image.width * scale));
        const int height = std::max(1, int(image.height * scale));
        SoftwareBitmap bitmap(BitmapPixelFormat::Gray8, width, height, BitmapAlphaMode::Ignore);
        {
            auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
            auto reference = buffer.CreateReference();
            auto access = reference.as<::Windows::Foundation::IMemoryBufferByteAccess>();
            uint8_t* bytes = nullptr;
            uint32_t capacity = 0;
            winrt::check_hresult(access->GetBuffer(&bytes, &capacity));
            const auto plane = buffer.GetPlaneDescription(0);
            for (int y = 0; y < height; ++y) {
                auto row = bytes + plane.StartIndex + size_t(y) * plane.Stride;
                const auto source = image.gray.data() + size_t(y * image.height / height) * image.width;
                if (width == image.width) std::memcpy(row, source, width);
                else for (int x = 0; x < width; ++x) row[x] = source[x * image.width / width];
            }
        }
        if (canceled.load()) return result;
        auto operation = engine.RecognizeAsync(bitmap);
        // WinRT permits only one completion handler. Keep the event alive if
        // cancellation returns before the operation finishes on another thread.
        auto completed = std::make_shared<winrt::handle>(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!*completed) winrt::throw_last_error();
        operation.Completed([completed](const auto&, auto) { ::SetEvent(completed->get()); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            if (canceled.load() || std::chrono::steady_clock::now() >= deadline) {
                operation.Cancel();
                if (!canceled.load()) result.error = L"Text recognition timed out. Try a smaller image.";
                return result;
            }
            const DWORD status = ::WaitForSingleObject(completed->get(), 50);
            if (status == WAIT_OBJECT_0) break;
            if (status == WAIT_FAILED) winrt::throw_last_error();
        }
        auto recognized = operation.GetResults();
        if (canceled.load()) return result;
        result.language = engine.RecognizerLanguage().DisplayName().c_str();
        const auto textAngle = recognized.TextAngle();
        const double angle = textAngle ? textAngle.Value() * 3.14159265358979323846 / 180.0 : 0.0;
        const double cosine = std::cos(angle), sine = std::sin(angle);
        size_t lineIndex = 0;
        for (const auto& line : recognized.Lines()) {
            for (const auto& word : line.Words()) {
                const auto bounds = word.BoundingRect();
                Word value{std::wstring(word.Text()), {{{bounds.X, bounds.Y}, {bounds.X + bounds.Width, bounds.Y},
                    {bounds.X + bounds.Width, bounds.Y + bounds.Height}, {bounds.X, bounds.Y + bounds.Height}}}, lineIndex};
                // Windows reports bounds in its deskewed image; restore the image angle.
                for (auto& p : value.corners) {
                    const double x = p.x - width * 0.5, y = p.y - height * 0.5;
                    p.x = float((x * cosine - y * sine + width * 0.5) * image.imageWidth / width);
                    p.y = float((x * sine + y * cosine + height * 0.5) * image.imageHeight / height);
                }
                result.words.push_back(std::move(value));
            }
            ++lineIndex;
        }
    } catch (const winrt::hresult_error& error) {
        if (!canceled.load()) result.error = L"Windows text recognition failed: " + std::wstring(error.message());
    } catch (const std::exception&) {
        if (!canceled.load()) result.error = L"Could not prepare text recognition. The image may be too large or memory may be low.";
    }
    return result;
}

bool Contains(const Word& word, Point point) {
    bool positive = false, negative = false;
    for (size_t i = 0; i < 4; ++i) {
        const auto a = word.corners[i], b = word.corners[(i + 1) % 4];
        const float cross = (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x);
        positive |= cross > 0;
        negative |= cross < 0;
    }
    return !(positive && negative);
}

std::wstring SelectedText(const std::vector<Word>& words, int first, int last) {
    if (first < 0 || last < 0 || size_t(first) >= words.size() || size_t(last) >= words.size()) return {};
    if (first > last) std::swap(first, last);
    std::wstring text;
    for (int i = first; i <= last; ++i) {
        if (i != first) text += words[i].line == words[i - 1].line ? L" " : L"\r\n";
        text += words[i].text;
    }
    return text;
}
}
