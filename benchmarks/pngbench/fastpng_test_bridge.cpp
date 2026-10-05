// Tiny ctypes bridge: tests invoke the production decoder, not a reimplementation.
#include "FastPng.h"
#include <cstdlib>
extern "C" __declspec(dllexport) unsigned char* decode_png(
    const unsigned char* bytes, size_t size, int* width, int* height) {
    FastPngImage image;
    if (FastPngDecode(bytes, size, image) != 0) return nullptr;
    *width = image.width;
    *height = image.height;
    free(image.exif_payload);
    return image.pixels;
}
extern "C" __declspec(dllexport) void free_png(unsigned char* pixels) { free(pixels); }
