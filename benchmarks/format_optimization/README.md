# JPEGView 2.11.0: format-specific performance improvements

These measurements cover the application changes released in 2.11.0, measured
before the version-resource bump. Confidential inputs, image names, per-file
reports and raw samples remain local. Only aggregate results are published here.

## Scope and controls

- Original baseline: Release x64 at `a2d5900a355e97cc2d794077465d800b864a4d99` (2.10.1).
- Corpus: 19 images / 70.45 MiB: 6 PNG, 11 JPEG, 2 HEIC.
- Primary: 25 measured fresh-process launches per image per build, two warmups.
- Independent confirmation: 15 launches per image per build, two warmups.
- Alternate paired A/B order; warm filesystem cache; identical configuration,
  image quality and default read-ahead. This is not a cold-disk benchmark.
- Intel Core i5-12400F / Windows 11; each child starts suspended and receives
  affinity `0xf00` (logical CPUs 8–11), a 33.33% host CPU hard cap, a 3 GiB
  committed-memory cap and below-normal priority before execution.
- These are process **budgets, not exclusive CPU or physical RAM reservations**.
- Aggregate values are arithmetic means of the 19 per-image medians. First-paint
  includes startup, loading, processing and rendering; CPU/peak memory include
  the entire application process, including read-ahead and shutdown.

## Original baseline → optimized application

| Metric | Before | After | Reduction |
|---|---:|---:|---:|
| App entry → first paint | 104.80 ms | 90.81 ms | 13.35% |
| Image-load time | 78.57 ms | 65.13 ms | 17.11% |
| Whole-process CPU time | 538.65 ms | 462.99 ms | 14.05% |
| Peak working set | 144.75 MiB | 132.34 MiB | 8.58% |
| Peak committed memory | 133.79 MiB | 119.40 MiB | 10.76% |
| Resume → exit | 236.39 ms | 206.26 ms | 12.74% |

First-paint reduction's 95% paired bootstrap interval: **10.74–15.37%** (2,000
draws within each image, holding the corpus fixed). Independent confirmation:
**12.87%** lower first-paint time, **13.56%** lower CPU time, **8.48%** lower peak
working set and **10.49%** lower committed memory.

| Format | Files | Before first paint | After first paint | Reduction |
|---|---:|---:|---:|---:|
| PNG | 6 | 87.27 ms | 73.49 ms | 15.79% |
| JPEG | 11 | 84.35 ms | 78.38 ms | 7.07% |
| HEIC | 2 | 269.85 ms | 211.12 ms | 21.77% |

Gains are not uniform: larger PNG/baseline JPEG images benefit more than small
progressive JPEGs. Most HEIC improvement comes from one of the two images;
this is not evidence that every HEIC loads 22% faster.

Against the previous intermediate optimization, first-paint time improved a
further **10.28%**; working-set memory was effectively unchanged. A separate
15-run mixed-folder navigation test against that intermediate build improved
median average frame time from **97.090 to 86.492 ms** (10.92%). This navigation
comparison is **not** against the original baseline.

All three final measurement sessions completed without failed attempts. Earlier
original-baseline navigation attempts crashed/hung and were retained locally;
final original-baseline comparisons deliberately measure individual images only.
One earlier intermediate runtime had an unresolved access violation. Successful
final sessions do not guarantee universal stability. Host contention can still
influence timings; intervals do not establish performance on untested workloads.

## Relevant implementation changes

- PNG: compact in-place RGB/RGBA reconstruction, borrowed single-IDAT input,
  promptly released staging, a shorter SIMD Paeth dependency chain, and corrected
  IHDR/interlace/dimension checks with existing fallback behavior.
- JPEG: fused common DC/AC Huffman operations, shared lookup tables, bounded wide
  entropy loads, snapshots every 16 MCUs, direct output from borrowed compressed
  bands, correct alignment-probe advancement and completion notification. Large
  progressive YCbCr images share completed coefficients among output workers.
- HEIF: chroma scratch reused per OpenMP worker; AVX2/FMA conversion for opaque
  full-range 8-bit images, preserving scalar rounding. Alpha/limited-range/HDR
  paths remain intact. Single-thread grid-tile requests avoid redundant libde265
  worker startup. Both original DAV1D and libde265 decoders remain available.
- Processing: worker counts respect process affinity; smaller strips bound SIMD
  scratch, shared pool submissions are serialized, and shutdown frees its array.

The libheif submodule stays pinned to its existing public upstream commit.
`extras/scripts/patches/libheif-sync-single-thread.patch` records our change;
`extras/scripts/build-libheif_libavif.bat` applies it idempotently. The updated
tracked x64 `heif.dll` is required for the complete HEIC gains.

## Correctness and reproduction

Validation before the version bump:

- 3,009 PNG byte-exact comparison/rejection checks; two unsupported fixtures rejected.
- 537 reference JPEGs and all 11 requested JPEGs matched single-thread libjpeg.
- 26 HEIC/HIF cases (including alpha/HDR): complete software and hardware-warm
  outputs were byte-identical to the previous build.
- Release x64 builds, Python syntax checks and whitespace checks passed.

Tools require Windows; PNG tests additionally need Pillow. Supply your own
non-confidential images and keep the raw JSON outputs local:

```powershell
python benchmarks/benchmark_random_isolated.py --before C:\baseline\JPEGView.exe --after C:\optimized\JPEGView.exe --data C:\images --output scratch\comparison.json --iterations 25 --warmups 2 --no-navigation
cmd /c benchmarks\pngbench\build_fastpng_test.bat
python benchmarks/pngbench/verify_fast_png.py scratch/perf_random/fastpng_test.dll
python benchmarks/verify_heif_unchanged.py --before C:\baseline\heic_decode_probe.exe --after C:\optimized\heic_decode_probe.exe --data C:\heif-images --output scratch\heif-verification.json
```

Build the production-object HEIF probe using `benchmarks/build_heic_probe.py`.
Each executable must have its matching dependencies and identical INI settings
alongside it. The runner records EXE, configuration and adjacent DLL hashes to
catch stale-runtime comparisons. Do not substitute decoder-only microbenchmarks
for the GUI results above. OCR, printing, other formats and other datasets were
not part of this performance claim.
