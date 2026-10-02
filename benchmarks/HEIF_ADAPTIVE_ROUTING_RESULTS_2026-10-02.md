# HEIF content-based decoder routing — 2026-10-02

The unconditional software-first policy in commit 4a59d62 regressed on the
three supplied 60 MP, 10-bit, 4:2:2 HIF images. The working-tree correction
reads image metadata before choosing a decoder. No filenames/extensions, hashes,
benchmark flags, reduced resolution or conversion-kernel changes select the path.
This correction is included in release 2.8.0. The recorded performance binary
predates the version-resource bump; release-package checks are recorded separately.

## Routing policy

- Alpha: software, including when hardware is warm. The GPU decoder does not
  preserve auxiliary alpha. Do not initiate GPU warmup for that decode.
- Cold GPU and 8-bit YCbCr 4:2:0: software first for multi-tile grids with tile
  area <= 1024*1024, or images <= 4*1024*1024 pixels.
- Other non-alpha layouts: try hardware even when cold. These include the large
  10-bit 4:2:2 grids. Failed/unsupported hardware falls back to software.
- Warm GPU: try hardware for non-alpha images; software fallback remains.
- Existing small-tile software threading optimization is retained. Neither
  software nor hardware color-conversion code was changed.

This is a conservative metadata heuristic, not a calibrated predictor for every
CPU/GPU/image. The small-image bound is a policy limit, not a measured universal
break-even point. Existing hardware decode determines actual support; metadata
alone does not prove device codec support. Systems without suitable hardware
retain the existing slower software fallback. No automatic timing-based
learning or persistent per-image cache was added.

## Direct paired comparison against the regressed committed build

Five paired runs per image/build, alternating build order. All 15 paired load
and first-paint comparisons improved. Full-process peak RAM is measured through
exit, not just first paint.

| Image | Committed load ms | Corrected load ms | Reduction | Committed peak RAM MiB | Corrected peak RAM MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| DSC01858.HIF | 2171.6 | 293.5 | 86.5% | 797.6 | 669.5 |
| DSC01861.HIF | 3075.5 | 336.9 | 89.0% | 961.6 | 645.0 |
| DSC01869.HIF | 1981.1 | 282.7 | 85.7% | 788.1 | 669.8 |

First paint: 2213.4 → 334.9 ms, 3109.8 → 370.7 ms, and 2022.9 → 325.1 ms,
respectively. These are fresh paired measurements, not ratios assembled from
unpaired historical runs.

## Fresh paired comparison against the original pre-optimization build

All 24 current corpus files, five paired runs per file/build (240 measured
launches). Twenty-one HEIC files remain faster: mean load 214.08 → 118.99 ms
(44.4% lower), first paint 237.73 → 154.49 ms (35.0% lower). All 105 HEIC paired
loads and first paints improved. Four sample1 files are identical; these aggregate
means count all supplied files, while every individual file is shown below.

The HIF correction is still **4.6–6.5% slower in load time than the original
hardware-first build** in this test (13–20 ms). It removes the seconds-long
regression, but is not a new speed record for those images. The new metadata
selection and changed initialization scheduling have overhead; those overheads
were not separately profiled here.

All cells are measured means, before → corrected after:

| Image | Load ms | First paint ms | Full-process peak RAM MiB |
| --- | ---: | ---: | ---: |
| 20220831_001704_66A1ECB0.heic | 226.0 → 122.7 | 250.0 → 163.5 | 167.3 → 162.8 |
| 20220916_045305_FEA16BAD.heic | 225.6 → 118.4 | 249.4 → 161.3 | 161.0 → 165.5 |
| 20230224_142047_BE5A46AD.heic | 227.6 → 142.9 | 252.1 → 184.3 | 174.9 → 164.7 |
| 20230224_142445_EE655C90.heic | 229.6 → 140.3 | 253.5 → 181.9 | 172.9 → 165.2 |
| 20230224_142448_B0A19107.heic | 224.2 → 133.2 | 248.9 → 173.4 | 171.1 → 165.6 |
| 20230224_142452_E4051711.heic | 224.7 → 133.1 | 248.2 → 178.3 | 169.0 → 167.9 |
| chef-with-trumpet.heic | 211.3 → 78.6 | 239.3 → 117.2 | 232.3 → 171.4 |
| childrens-show-theater.heic | 207.4 → 69.0 | 234.0 → 118.3 | 176.3 → 168.0 |
| classic-car.heic | 227.1 → 136.6 | 251.9 → 171.9 | 167.0 → 160.8 |
| DSC01858.HIF | 284.2 → 297.2 | 326.6 → 339.2 | 677.9 → 670.0 |
| DSC01861.HIF | 327.3 → 346.9 | 362.2 → 381.3 | 644.2 → 640.0 |
| DSC01869.HIF | 265.8 → 283.1 | 308.5 → 326.3 | 678.0 → 666.2 |
| greyhounds-looking-for-a-table.heic | 224.6 → 113.8 | 248.7 → 164.2 | 164.6 → 173.8 |
| HeicWithKeywords.heic | 236.3 → 187.2 | 260.2 → 223.7 | 187.0 → 163.5 |
| iphone_15_pro.heic | 220.7 → 100.9 | 246.5 → 140.7 | 162.9 → 166.3 |
| old-safe-wall.heic | 228.1 → 141.0 | 252.2 → 166.3 | 173.3 → 99.8 |
| sample1 (1).heic | 189.1 → 85.3 | 208.7 → 103.8 | 75.9 → 60.7 |
| sample1 (2).heic | 190.6 → 80.0 | 209.1 → 99.0 | 76.0 → 60.6 |
| sample1 (3).heic | 186.5 → 80.0 | 205.3 → 98.5 | 75.8 → 60.6 |
| sample1.heic | 187.1 → 82.0 | 205.7 → 100.9 | 75.9 → 55.6 |
| Screenshot 2024-10-31 at 08.19.26 Alpha.heic | 188.7 → 30.5 | 208.6 → 64.7 | 66.5 → 115.3 |
| sewing-threads.heic | 207.4 → 170.2 | 234.8 → 211.2 | 173.5 → 183.4 |
| shelf-christmas-decoration.heic | 209.4 → 158.7 | 237.3 → 198.5 | 179.1 → 183.3 |
| soundboard.heic | 223.6 → 194.3 | 247.8 → 222.7 | 166.8 → 164.3 |

Memory improvement remains image-dependent, not universal. Normal application
read-ahead is enabled: even an alpha image can have background non-alpha work.
Stopping GPU warmup for its own decode does not eliminate every GPU allocation
from the process. Peak resident RAM does not include GPU VRAM and is not the
same as private committed memory.

## Verification and measurement limits

- Release x64 build passed, with existing compiler/linker warnings.
- Production-object probes validated all 24 images, cold and warm: full dimensions,
  complete BGRA buffer size, and SHA-256 equality with the corresponding existing
  decoder path. HIF cold output matches existing warmed hardware output exactly;
  other cold output matches existing software output. Non-alpha warm output
  matches existing hardware; alpha warm output matches existing software.
- Alpha screenshot remains 900x705 with alpha spanning 0–255, including warm
  decode. The HIFs remain 9504x6336 or 6336x9504 (full 60 MP), not downsampled.
- Pixel checks prove preservation of existing path outputs, not absolute color
  correctness against an independent reference. Existing software/hardware
  outputs differ. No new chroma or color-conversion optimization was attempted.
- Two timing comparisons total 270 measured app launches plus 54 warmups.
  Fresh process, warm OS file cache, identical configuration/runtime DLL hashes,
  randomized image order and alternating build order, seed 4202. All launches
  completed successfully. No outlier removal or retries.
- Normal app decoding, painting and read-ahead; no benchmark-conditioned paths.
  GetProcessMemoryInfo reads Windows lifetime PeakWorkingSetSize after exit,
  using a retained process handle, without periodic polling.
- Unsupported-hardware fallback is retained in code but not independently
  exercised on a hardware-disabled machine. This is one machine/corpus, not
  a guarantee for arbitrary devices or images. Cold disk cache and warmed
  navigation performance are not established by these launch tests.

SHA-256:

```text
original  553c8f3fb606ecbb5fedd62b6152d1d92dcdb5d576bdd2232d9d22ec54013681
committed 0a0af5d77f90de2f001a1e818f11c1906d73ea714fad4094221e640163b90b41
corrected 8f5fba28037eff61a92c0f8fa64c56c05fc11adc66c7d81d9f084adccb3e4f56
```

Raw evidence with individual samples, image hashes and distributions:

- [24-file comparison against original](heif_results_2026-10-02/heif_adaptive_vs_original_2026-10-02.json)
- [HIF paired comparison against committed](heif_results_2026-10-02/heif_adaptive_vs_committed_hif_2026-10-02.json)
- [24-file pixel verification](heif_results_2026-10-02/heif_adaptive_pixels_2026-10-02.json)
- [Pixel verification script](validate_heif_routing.py)
