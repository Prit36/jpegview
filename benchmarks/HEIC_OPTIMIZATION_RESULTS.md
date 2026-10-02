# HEIC opening performance — 2026-10-02

The previous alpha-blending change has been reverted. This change targets HEIC
opening using all eight files currently present in `benchmarks/heic_test_data`.
The user's replacement corpus and other workspace changes were preserved.

## Result

On this machine (Intel Core i5-12400F, 6 cores / 12 logical processors), mean
full-resolution load time decreased from **212.7 to 129.3 ms (39.2%)**. Mean
application-start-to-first-paint decreased from **238.3 to 167.1 ms (29.9%)**.
Every image improved, and all **96/96 paired samples** improved both metrics.

| Image | Baseline load ms | New load ms | Load reduction | Baseline first paint ms | New first paint ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| chef-with-trumpet.heic | 207.6 | 76.1 | 63.3% | 235.6 | 111.5 |
| childrens-show-theater.heic | 206.2 | 69.7 | 66.2% | 232.5 | 113.4 |
| classic-car.heic | 220.6 | 130.9 | 40.7% | 244.4 | 163.7 |
| greyhounds-looking-for-a-table.heic | 223.4 | 112.3 | 49.7% | 247.6 | 148.0 |
| old-safe-wall.heic | 222.1 | 135.0 | 39.2% | 246.4 | 182.3 |
| sewing-threads.heic | 203.8 | 167.0 | 18.0% | 230.1 | 205.8 |
| shelf-christmas-decoration.heic | 204.9 | 155.4 | 24.1% | 231.6 | 196.7 |
| soundboard.heic | 213.5 | 188.2 | 11.8% | 238.1 | 215.7 |

For load time, even the slowest candidate sample was faster than the fastest
baseline sample for each image. First-paint distributions overlap slightly for
one file (shelf-christmas-decoration), although all its paired runs improved.

These results establish consistent improvement on the supplied corpus and this
machine. They do not establish universal improvement for every HEIC layout,
CPU, GPU, or storage-cache state.

## Changes

Profiling found that creating the D3D11 device and discovering its hardware MFT
dominated the first HEIC load: approximately 159 ms for the device and 22 ms for
decoder enumeration in a representative run. Software decoding starts immediately
while the hardware decoder is cold. Hardware initialization starts after a
successful software decode; subsequent images use hardware once it is ready.

For grids with small tiles (at most 1024 × 1024 pixels per tile), libheif now
decodes tiles concurrently with up to 16 workers, bounded by logical CPU count,
and one codec worker per tile. Previously, four concurrent tiles each used up to
16 codec workers, repeatedly creating workers and oversubscribing the CPU.
Single-image and large-tile software decoding retain the existing codec-worker
policy. The choice uses image structure and CPU count, never filenames.

The background task publishes readiness atomically and is joined before process
teardown. Hardware demuxing now excludes hidden, thumbnail, and auxiliary items
from navigable frames. This keeps frame counts consistent with libheif when
switching decoder paths.

## Tradeoff measured, not hidden

The optimization reduces the time until the image is displayed; it does not
eliminate GPU startup. Immediate close waits for the background task. In this
launch-and-exit test, mean total process lifetime increased from **306.3 to
427.4 ms (+39.5%)**. Mean peak working set recorded by the existing first-paint
telemetry decreased from **173.3 to 104.8 MB**; this is not a measurement of the
peak after all background GPU work completes.

The existing CPU and GPU color-conversion paths produce different pixels.
The threading change preserves the software fallback's pixels exactly, but the
first image now uses those pixels rather than the former hardware result.
Against pillow-heif, software mean absolute RGB error was 0.10–0.50 on the
0–255 scale, compared with 0.39–2.64 for hardware. Neither existing conversion
path is identical to that independent reference; the conversion kernels were
not changed as part of this optimization.

## Verification and benchmark method

- Release x64 build succeeded. Existing compiler/linker warnings remain.
- Real JPEGView app, unchanged `/benchmark`, processing, painting and read-ahead.
- Eight supplied files, one warm-up per file/build, then 12 measured launches per
  file/build: 192 measured launches total. Fresh process and decoder each time;
  warmed OS file cache for both builds. Cold disk-cache opening was not tested.
- Randomized image order with a fixed seed; alternating baseline/candidate order;
  identical runtime DLL and configuration hashes; no outlier removal or retries.
- All measured processes exited successfully and supplied valid telemetry.
- The pixel probe links the actual Release app objects and libraries, without
  decoder or color-transform stubs. It verifies 4032×3024, 3024×4032,
  4000×3000 and 3000×4000 outputs, orientation, full buffer size and opaque alpha.
- Software and warmed hardware paths each report one frame on all eight files.
- All eight tuned software pixel buffers match the original software fallback
  byte for byte, verified by SHA-256. The validation-only original-source snapshot
  retains its decoder/threading/conversion code, with hardware startup gated to
  isolate the software comparison. It was never used for the performance results.
- Profiling and experimental environment-variable overrides were removed from
  production code before the measured build.

Executable SHA-256:

```text
baseline  553c8f3fb606ecbb5fedd62b6152d1d92dcdb5d576bdd2232d9d22ec54013681
candidate 0a0af5d77f90de2f001a1e818f11c1906d73ea714fad4094221e640163b90b41
```

Raw measurements, file hashes and distributions:
[heic_comparison_2026-10-02.json](heic_comparison_2026-10-02.json).
Pixel checks and hashes:
[heic_pixels_2026-10-02.json](heic_pixels_2026-10-02.json).

Reproduce the paired benchmark from the repository root:

```powershell
python benchmarks/compare_heic.py benchmarks/.cache/bin/baseline_heic_current_corpus/JPEGView.exe benchmarks/.cache/build/current/bin/Release/JPEGView.exe --iterations 12 --output benchmarks/.cache/heic_comparison.json
```

Build and run the production-object pixel probe:

```powershell
python benchmarks/build_heic_probe.py benchmarks/.cache/build/current
python benchmarks/check_heic_pixels.py benchmarks/.cache/build/current/bin/Release/heic_decode_probe.exe
```

To verify original software equivalence, pass the preserved validation snapshot
`benchmarks/.cache/HEIFWrapper_legacy_software.cpp` as the second build argument,
then pass `heic_decode_probe_legacy_software.exe` as the second pixel-check argument.
