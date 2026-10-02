# Expanded HEIC before/after results — 2026-10-02

Measured on the same Intel Core i5-12400F machine. Before is the preserved
pre-optimization Release x64 app; after is the preserved first HEIC optimization
approved and committed as `4a59d6212f2b336f3f9a3c830dc0f2f1ddf2ef8d`.
No later conversion optimization is included. No app code changed for this test.

## Aggregate measured means

| Metric | Before | After | Change |
| --- | ---: | ---: | ---: |
| Image load | 212.8 ms | 117.5 ms | 44.8% lower |
| Process start to first paint | 237.1 ms | 152.7 ms | 35.6% lower |
| Peak working set through first-paint telemetry | 148.2 MiB | 87.4 MiB | 41.0% lower |
| Full-process peak working set through exit | 151.4 MiB | 142.7 MiB | 5.8% lower |
| Immediate-exit process lifetime | 301.9 ms | 400.4 ms | 32.6% longer |

All 21 files included. Four sample1 files are byte-identical: 18 unique images.
Giving each unique content hash equal weight (pooling its duplicate samples),
load was 217.0 → 123.7 ms (43.0% lower), first paint 242.0 → 161.5 ms
(33.3% lower), first-paint peak 160.3 → 95.6 MiB, and full-process peak
163.9 → 156.5 MiB (4.5% lower).

All files improved in mean load and mean first-paint time. Load improved in
252/252 paired timing comparisons. First paint improved in 251/252: sewing-threads
in zero-based round 10 took 223.791 ms before and 230.940 ms after. No sample was
removed. These measurements do not establish universal improvement.

## Every file: before → after

Times and first-paint peak are means of 12 runs per build per file. Full-process
peak RAM is the mean of 5 additional runs per build per file. Memory units are
MiB (1,048,576 bytes), not GPU VRAM or total system memory.

| File | Load ms | First paint ms | Peak RAM through first paint MiB | Full-process peak RAM MiB |
| --- | ---: | ---: | ---: | ---: |
| 20220831_001704_66A1ECB0.heic | 222.9 → 120.5 | 248.0 → 157.4 | 166.4 → 100.5 | 168.7 → 159.8 |
| 20220916_045305_FEA16BAD.heic | 223.1 → 113.2 | 248.0 → 155.2 | 163.5 → 100.3 | 164.3 → 164.5 |
| 20230224_142047_BE5A46AD.heic | 230.3 → 143.0 | 255.3 → 186.6 | 172.3 → 100.0 | 175.6 → 167.2 |
| 20230224_142445_EE655C90.heic | 226.3 → 141.7 | 251.4 → 180.8 | 170.6 → 99.5 | 173.3 → 166.2 |
| 20230224_142448_B0A19107.heic | 225.4 → 134.5 | 250.3 → 172.7 | 168.8 → 99.0 | 170.0 → 161.4 |
| 20230224_142452_E4051711.heic | 225.9 → 132.8 | 250.5 → 172.1 | 170.4 → 101.0 | 170.8 → 166.4 |
| chef-with-trumpet.heic | 210.8 → 77.7 | 239.5 → 116.0 | 175.2 → 108.9 | 232.4 → 174.5 |
| childrens-show-theater.heic | 207.4 → 68.9 | 234.3 → 116.4 | 175.6 → 110.0 | 175.1 → 163.7 |
| classic-car.heic | 222.1 → 131.5 | 247.1 → 171.0 | 170.8 → 100.9 | 168.5 → 164.5 |
| greyhounds-looking-for-a-table.heic | 224.3 → 115.6 | 249.2 → 159.8 | 162.1 → 100.4 | 160.7 → 172.8 |
| HeicWithKeywords.heic | 242.4 → 190.9 | 267.5 → 222.4 | 186.3 → 99.2 | 188.3 → 159.8 |
| iphone_15_pro.heic | 221.2 → 94.5 | 245.7 → 141.2 | 161.9 → 100.9 | 161.4 → 167.9 |
| old-safe-wall.heic | 225.6 → 135.7 | 250.6 → 161.1 | 170.9 → 99.0 | 170.3 → 100.0 |
| sample1 (1).heic | 187.9 → 81.3 | 207.5 → 100.5 | 75.8 → 39.3 | 75.9 → 60.8 |
| sample1 (2).heic | 185.6 → 79.9 | 205.0 → 99.2 | 75.7 → 39.3 | 76.0 → 60.7 |
| sample1 (3).heic | 187.6 → 80.5 | 207.4 → 99.8 | 75.8 → 39.3 | 75.9 → 60.8 |
| sample1.heic | 190.0 → 80.4 | 209.9 → 99.7 | 75.8 → 36.9 | 75.9 → 55.5 |
| Screenshot 2024-10-31 at 08.19.26 Alpha.heic | 188.8 → 31.5 | 210.2 → 66.1 | 66.4 → 46.8 | 66.5 → 140.9 |
| sewing-threads.heic | 200.9 → 166.1 | 228.5 → 209.0 | 180.4 → 109.2 | 180.8 → 181.3 |
| shelf-christmas-decoration.heic | 201.9 → 156.6 | 229.4 → 199.6 | 181.4 → 107.7 | 182.1 → 182.2 |
| soundboard.heic | 218.3 → 191.4 | 243.3 → 220.4 | 166.5 → 98.2 | 166.0 → 165.0 |

The first-paint and full-process memory columns come from separate passes;
their means need not be monotonic across passes. For example, chef baseline
full-process peak ranged 227.9–238.6 MiB in the five-run memory pass, whereas
its earlier first-paint mean was 175.2 MiB. Do not subtract these columns to
estimate background allocation.

Full-process memory is not consistently lower. Greyhounds, iphone_15_pro and
the alpha screenshot show increases; tiny changes on other files should not
be interpreted as meaningful. The alpha screenshot's before range was
66.5–66.6 MiB and after range 135.5–148.2 MiB, so its increase is substantial.
Background GPU initialization occurs after initial software decode and is joined
on exit, explaining why first-paint telemetry alone is not a lifetime measurement.

## Method and evidence

- 504 measured timing launches plus 210 measured full-process memory launches.
  One warmup per file per build in each pass: 84 additional launches.
- Fresh process each launch; warm OS file cache. Randomized file order, alternating
  baseline/candidate order, seed 4202. Same configuration and runtime DLL hashes.
- Real app `/benchmark`, `/benchmark_exit`, `/autoexit`; normal decoding,
  processing and painting. No downsampling or production-code benchmark changes.
- All launches completed successfully with positive load and first-paint telemetry.
  No outlier exclusions or retries. Performance tests are not exhaustive pixel,
  alpha or color-correctness validation of the newly added images.
- First-paint RAM is the app's existing Windows PeakWorkingSetSize telemetry.
  Full-process RAM uses GetProcessMemoryInfo on the retained process handle after
  successful exit, reading the OS lifetime PeakWorkingSetSize; no periodic polling.
  Peak working set is resident process memory; private commit, shared allocations
  and GPU VRAM are different metrics. Tests do not measure cold disk-cache loading,
  warmed navigation, or memory while browsing multiple images.

Executable SHA-256:

```text
before 553c8f3fb606ecbb5fedd62b6152d1d92dcdb5d576bdd2232d9d22ec54013681
after  0a0af5d77f90de2f001a1e818f11c1906d73ea714fad4094221e640163b90b41
```

Raw per-run timing, hashes, means, medians, ranges and standard deviations:
[timing JSON](heif_results_2026-10-02/heic_expanded_comparison_2026-10-02.json).
Raw per-run full-process memory and timing:
[memory JSON](heif_results_2026-10-02/heic_expanded_memory_2026-10-02.json).
Additional measurement wrapper:
[compare_heif_memory.py](compare_heif_memory.py).

```powershell
python benchmarks/compare_heic.py benchmarks/.cache/bin/baseline_heic_current_corpus/JPEGView.exe benchmarks/.cache/bin/baseline_heic_round2/JPEGView.exe --iterations 12 --output benchmarks/.cache/heic_expanded_comparison_2026-10-02.json
python benchmarks/compare_heif_memory.py benchmarks/.cache/bin/baseline_heic_current_corpus/JPEGView.exe benchmarks/.cache/bin/baseline_heic_round2/JPEGView.exe --iterations 5 --output benchmarks/.cache/heic_expanded_memory_2026-10-02.json
```
