[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue)](https://github.com/Prit36/jpegview/blob/master/LICENSE.txt)
[![OS Support](https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-blue)](#system-requirements)

# JPEGView - Ultra-Fast Image Viewer and Editor

A performance-focused, modernized fork of [sylikc/jpegview](https://github.com/sylikc/jpegview), which itself continues the legacy of the excellent [JPEGView by David Kleiner](https://sourceforge.net/projects/jpegview/).

JPEGView is a lean, fast and highly configurable image viewer/editor with a minimal GUI.

## The Journey - What This Fork Changed

This fork started from sylikc's `master` (post v1.3.46) and went through a full
modernization and ground-up performance overhaul (43 commits, August 2026).
Nothing was version-tracked along the way; **v2.1.0** is the first tagged milestone.
See [CHANGELOG.txt](CHANGELOG.txt) for the complete detailed history. Highlights:

### Modernization
* Visual Studio 2026 toolchain, C++23, CMake 4.x-compatible build (plus vcpkg manifest)
* Legacy support removed: 32-bit/x86 builds and Windows XP/Vista/7/8 code paths are gone
  - now targets **64-bit Windows 11 / 10 / Server 2025 only**
* Direct2D 1.3 rendering path with HDR support
* Modern file associations via the Windows 10/11 Default Apps experience
* All third-party codecs bumped to current releases (libjpeg-turbo 3.2.0, libwebp 1.6.0,
  LibRaw 0.22.2, libheif 1.23.1, libjxl 0.12.0, libavif 1.4.2, ...)

### Performance (the big one)
* **Parallel multi-threaded baseline JPEG decoding** - speculative entropy walk +
  per-band rendering on the proven libjpeg-turbo SIMD path, verified pixel-exact on a
  521-photo corpus
* **GPU hardware-accelerated HEIF/HEIC decoding** via Media Foundation + Direct3D 11,
  with a native zero-copy ISOBMFF parser (grid tiles, irot/imir transforms):
  >7x faster on 45MP HEICs (~2100ms -> ~296ms)
* **Time-to-first-paint on 45MP photos cut from ~171ms to ~80-90ms**: memory-mapped
  zero-copy JPEG input, delay-loaded DLLs, lazy directory scan, pipelined band decode
  with hidden resample, deferred EXIF rotation past first paint
* Parallel SIMD resampling across all cores (legacy 4-core cap removed), AVX2 kernels
  throughout, fused OpenMP color conversion for HEIF
* Whole-program optimization (/GL /LTCG /Ox /Ob3 /fp:fast /Qpar) with AVX2 codegen

### Bug fixes
* Shutdown race condition, GDI+ shutdown crash, PNG use-after-free
* JPEG marker-segment stride bug in the parallel walker (0xFF prefix handling),
  DRI restart-interval extraction
* Magic-byte-first image format detection (no more misdetection by extension)
* Startup window flash fixed
* HEIF: idat offset resolution, ImageGrid header parsing, out-of-bounds chroma clamp,
  missing CPU-fallback color conversion, irot/imir application

### Tooling
* New systematic benchmark suite in [`benchmarks/`](benchmarks/) with honest telemetry
  (via the app's own `/benchmark` self-exit mechanism), multi-run statistics, dataset
  download tooling, and an IFEO page-heap guard so measurements can't be silently skewed

## Formats Supported

JPEGView has built-in support the following formats:

* Popular: JPEG, GIF
* Lossless: BMP, PNG, TIFF, PSD
* Web: WEBP, JXL, HEIF/HEIC, AVIF
* Camera RAW formats:
  * Adobe (DNG), Canon (CRW, CR2, CR3), Nikon (NEF, NRW), Sony (ARW, SR2)
  * Olympus (ORF), Panasonic (RW2), Fujifilm (RAF)
  * Sigma (X3F), Pentax (PEF), Minolta (MRW), Kodak (KDC, DCR)
  * A full list is available here: [LibRaw supported cameras](https://www.libraw.org/supported-cameras)

Many additional formats are supported by Windows Imaging Component (WIC)

### Basic Image Editor

Basic on-the-fly image processing is provided - allowing adjusting typical parameters:

* sharpness
* color balance
* rotation
* perspective
* contrast
* local under-exposure/over-exposure

### Other Features

* Small and fast, uses AVX2/SSE2 and all available CPU cores
* High quality resampling filter, preserving sharpness of images
* Basic image processing tools can be applied realtime during viewing
* Movie/Slideshow mode - to play folder of JPEGs as movie

# Building

Build with Visual Studio 2026 (`src\JPEGView.sln`) or CMake:

```shell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Third-party dependencies live under `extras/third_party`; see `extras/scripts`.

# System Requirements

* 64-bit Windows 11, Windows 10, or Windows Server 2025 (x64 / AMD64)
* AVX2-capable CPU recommended
* Visual Studio 2026 / C++23 runtime

# What's New

* See [CHANGELOG.txt](CHANGELOG.txt) to review everything in detail, starting with the
  **[2.1.0]** fork release notes at the top.

# Localization

By default, the language is auto-detected to match your Windows Locale.  All the text in the menus and user interface should show in your language.  To override the auto-detection, manually set `Language` option in `JPEGView.ini`

JPEGView is currently translated/localized to 28 languages:

| INI Option | Language |
| ---------- | -------- |
| be | Belarusian |
| bg | Bulgarian |
| cs | Czech |
| de | German |
| el | Greek, Modern |
| es-ar | Spanish (Argentina) |
| es | Spanish |
| eu | Basque |
| fi | Finnish |
| fr | French (Français) |
| hu | Hungarian |
| it | Italian |
| ja | Japanese (日本語) |
| ko | Korean (한국어) |
| pl | Polish |
| pt-br | Portuguese (Brazilian) |
| pt | Portuguese |
| ro | Romanian |
| ru | Russian (Русский) |
| sk | Slovak |
| sl | Slovenian (Slovenščina) |
| sr | Serbian (српски) |
| sv | Swedish |
| ta | Tamil |
| tr | Turkish (Türkçe) |
| uk | Ukrainian (Українська) |
| zh-tw | Chinese, Traditional (繁體中文) |
| zh | Chinese, Simplified (简体中文) |

# Help / Documentation

The bundled documentation ([readme.html](src/JPEGView/Config/readme.html)) ships inside the package and covers usage, INI settings and keyboard shortcuts.

# Brief History

JPEGView was created by [David Kleiner](https://sourceforge.net/projects/jpegview/) (2006-2018).
[Kevin M (sylikc)](https://github.com/sylikc/jpegview) revived it on GitHub in 2020, adding new codecs and keeping it alive through v1.3.46 (2023).

Since August 2026 this repository is maintained, focusing on modernizing the toolchain, dropping legacy platforms, and squeezing every millisecond out of image loading and display.

## Special Thanks

* David Kleiner - for creating JPEGView
* Kevin M (sylikc) - for reviving and maintaining the upstream fork
* [qbnu](https://github.com/qbnu) - for adding additional codec support:
  * Animated WebP / Animated PNG
  * JPEG XL with animation support
  * HEIF/HEIC/AVIF support
  * QOI support
  * ICC Profile support for WebP, JPEG XL, HEIF/HEIC, AVIF
  * LibRaw support (all updated RAW formats, such as CR3)
  * Photoshop PSD support
* All the _translators_ which keep JPEGView strings up-to-date in different languages!
