#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Ocr {
// A bounded snapshot owns its pixels. Workers never access the viewer's images.
constexpr int SnapshotLimit = 2048;
struct Snapshot {
    int width = 0, height = 0;
    int imageWidth = 0, imageHeight = 0;
    std::vector<uint8_t> gray;
};
struct Point { float x, y; };
struct Word {
    std::wstring text;
    std::array<Point, 4> corners;
    size_t line = 0;
};
struct Result {
    std::vector<Word> words;
    std::wstring language;
    std::wstring error;
};

// BGR/BGRA, top-down, DWORD-aligned rows. Bilinear sampling preserves small text
// better than nearest-neighbor while bounding memory independently of image size.
Snapshot MakeSnapshot(const void* pixels, int width, int height, int channels);
// Initializes WinRT on the calling worker, never on the UI thread.
Result Recognize(const Snapshot& image, const std::atomic<bool>& canceled);
bool Contains(const Word& word, Point point);
std::wstring SelectedText(const std::vector<Word>& words, int first, int last);
}
