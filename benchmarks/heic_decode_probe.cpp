// Links the production Release objects; no decoder or color-transform stubs.
// The files contain little-endian width/height followed by full BGRA pixels.
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include "HEIFWrapper.h"
#include "GpuHeifDecoder.h"

static bool Decode(const std::vector<char>& data, const std::filesystem::path& output) {
    int width = 0, height = 0, channels = 0, frames = 0;
    void* exif = nullptr;
    bool oom = false, alpha = false;
    auto pixels = std::unique_ptr<unsigned char[]>(static_cast<unsigned char*>(
        HeifReader::ReadImage(width, height, channels, frames, exif, oom, alpha,
                              0, data.data(), static_cast<int>(data.size()))));
    free(exif);
    if (!pixels || channels != 4 || width <= 0 || height <= 0) return false;
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<const char*>(&width), sizeof(width));
    file.write(reinterpret_cast<const char*>(&height), sizeof(height));
    file.write(reinterpret_cast<const char*>(pixels.get()),
               static_cast<std::streamsize>(width) * height * channels);
    std::printf("%dx%d frames=%d channels=%d alpha=%d hardware_ready=%d\n",
                width, height, frames, channels, alpha, GpuHeifDecoder::IsHardwareReady());
    return file.good();
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 3;
    std::ifstream input(std::filesystem::path(argv[1]), std::ios::binary | std::ios::ate);
    if (!input) return 4;
    std::vector<char> data(static_cast<size_t>(input.tellg()));
    input.seekg(0);
    input.read(data.data(), static_cast<std::streamsize>(data.size()));
    std::filesystem::path prefix(argv[2]);
    bool cpu = Decode(data, prefix.wstring() + L".cpu.bgra");
    GpuHeifDecoder::WaitForHardwareInitialization();
    bool warm = Decode(data, prefix.wstring() + L".warm.bgra");
    CoUninitialize();
    return cpu && warm ? 0 : 5;
}
