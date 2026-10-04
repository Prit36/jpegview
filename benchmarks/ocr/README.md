# OCR performance measurements — 2026-10-04

The optimization reduces time spent preparing OCR pixels on the UI thread. Images
within the existing 2048-pixel limit now use a direct, integer grayscale conversion.
Larger images reuse horizontal bilinear sampling coordinates across rows. BGR and
BGRA paths specialize their pixel stride. The image limit, interpolation precision,
Windows recognizer, language selection and result processing are unchanged.

## Measured results

These are **actual wall-clock measurements**, not estimates. The table uses the
second full run's medians. All times are milliseconds; smaller is better.

| Stage | Source dimensions | Channels | Before | After | Speedup | Before p95 | After p95 |
|---|---|---|---:|---:|---:|---:|---:|
| Snapshot | 800 × 600 | BGR | 3.753 | 0.341 | 11.00× | 3.952 | 0.443 |
| Snapshot | 1920 × 1080 | BGR | 16.651 | 1.913 | 8.70× | 17.135 | 2.100 |
| Snapshot | 1920 × 1080 | BGRA | 16.967 | 1.897 | 8.94× | 17.397 | 2.124 |
| Snapshot | 2048 × 2048 | BGRA | 34.135 | 3.934 | 8.68× | 34.856 | 4.254 |
| Snapshot | 3840 × 2160 | BGR | 18.843 | 9.424 | 2.00× | 19.531 | 9.915 |
| Snapshot | 3840 × 2160 | BGRA | 19.374 | 9.488 | 2.04× | 20.090 | 9.988 |
| Snapshot | 6000 × 4000 | BGR | 22.562 | 11.341 | 1.99× | 23.278 | 11.914 |
| Snapshot | 6000 × 4000 | BGRA | 23.025 | 11.327 | 2.03× | 23.777 | 11.866 |
| Snapshot + Windows OCR | 800 × 600 | BGR | 20.475 | 16.806 | 1.22× | 22.197 | 18.140 |
| Snapshot + Windows OCR | 1920 × 1080 | BGRA | 49.210 | 34.442 | 1.43× | 50.531 | 36.201 |
| Snapshot + Windows OCR | 3840 × 2160 | BGRA | 54.952 | 45.390 | 1.21× | 56.962 | 48.029 |
| Snapshot + Windows OCR | 6000 × 4000 | BGR | 64.286 | 52.573 | 1.22× | 71.115 | 57.011 |

Total snapshot + recognition latency fell **17–30%** in this run. Preparation
speedup is larger because Windows OCR still accounts for most of the total time.
The first matching-flags run showed 17–31% total latency reduction, 8.66–10.95×
unscaled preparation speedup, and 1.99–2.03× downscaled preparation speedup.

## Method and limitations

- Intel Core i5-12400F, 6 cores / 12 logical processors; Windows 11 Pro 10.0.26200.
- MSVC x64 Release, Windows SDK 10.0.26100.0, including `/Ox`, `/fp:fast`,
  `/arch:AVX2`, `/Qpar`, `/GL` and `/LTCG`, matching the app's Release settings.
  Full flags, tool versions and source SHA-256 hashes are in the metadata files.
- Baseline is commit `943dafd486e64204b63673554743f332b5085111`. The harness compiles
  its real OCR implementation and the working-tree implementation into one
  executable, changing only the baseline namespace to avoid symbol collisions.
- Snapshot timings use deterministic random pixel inputs, include allocations,
  and contain 40 paired samples per case. Each sample averages three calls.
  Three untimed warm-up pairs precede each case.
- Recognition timings use deterministic GDI-rendered English document fixtures,
  contain 24 pairs per case, and include snapshot allocation/conversion, apartment
  initialization, recognizer creation, bitmap upload, actual `RecognizeAsync`,
  waiting and extraction of words/coordinates. Recognizers are created afresh on
  each call, as in the application. Installed language: English (United States).
- Before/after order alternates for every pair. The first call of each
  implementation for each recognition fixture is excluded to avoid attributing
  Windows cold initialization to either implementation. Builds were finished
  before the two reported measurement runs; neither reported run overlapped a build.
- This measures the OCR pipeline on already-decoded pixels. It excludes image
  decoding, deferred image rotation, worker thread launch, message delivery,
  overlay rendering and clipboard actions. It is **not** a click-to-overlay UI
  benchmark or a cold-start benchmark. Generated text is not representative of
  every photograph, font, language or page density; these numbers are specific
  to this machine and corpus.
- p95 uses nearest-rank percentiles. Snapshot p95 describes three-call sample
  averages; recognition p95 describes single calls. Do not interpret these
  small sample sets as universal tail-latency guarantees.
- The downscale coordinate table adds at most 48 KiB on x64. The existing bounded
  grayscale snapshot remains at most 4 MiB. No larger source copy is introduced.

## Correctness and build

The final harness checks byte-for-byte snapshot equality across 98 inputs:
22 boundary/stride/aspect-ratio cases, 64 seeded random cases, eight timed random
images and four text fixtures. Both BGR and BGRA are included, along with
one-pixel axes, padded rows and dimensions around the 2048-pixel limit.

Every timed recognition pair, and its initial warm-up pair, produced identical
language, word text, ordering, line indices and all four word corners. The four
fixtures returned 73, 85, 85 and 94 words. This verifies unchanged OCR inputs and
outputs on the test corpus; it does not claim a new recognition-accuracy score.

The full x64 Release application build passed and produced
`build_x64/bin/Release/JPEGView.exe`. The Python runner also passed bytecode
compilation, and the source diff passed whitespace checks.

## Reproduce

From the repository root, with MSVC Build Tools, the Windows SDK and an installed
Windows OCR language:

```powershell
python benchmarks/run_ocr_benchmark.py --baseline-ref 943dafd486e64204b63673554743f332b5085111
```

Use `--micro-only` to measure preparation without Windows recognition. Generated
executable, baseline source, metadata, summary CSV, per-pair CSV and validation
log go into `benchmarks/.cache/ocr/`. The runner fails if pixels or OCR outputs
differ, or if recognition fails/returns no words.

The checked-in `2026-10-04-run1-*` and `2026-10-04-run2-*` files preserve both
reported runs. `2026-10-04-validation.txt` preserves the second run's validation.
The second run adds the 64 seeded correctness cases before timing; its timed
random-image byte sequences therefore differ from the first run. Text fixtures
and all timing procedures are the same.
