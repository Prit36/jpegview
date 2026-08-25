# PNG Decode Optimization Plan — “much muuch faster” without cheating

No downsampling, no quality loss, no misleading tricks. Every optimization preserves byte-identical BGRA output (verified via `pngbench --dump` vs Pillow).

## 1. Baseline (i5-12400F, BlackMarble 5760×3240 RGBA 15.6 MB)

| Phase | old libpng+zlib | current FastPng (libdeflate+SSE) |
|-------|----------------|---------------------------------|
| zlib inflate | ~155–185 ms | ~62 ms (libdeflate) |
| unfilter + RGBA→BGRA | ~120 ms scalar | ~55 ms SSE (1 px / op) |
| transient copies (3×75 MB) | ~42 ms | 0 |
| **ReadImage** | **~320 ms** | **~117 ms** |
| End-to-end first paint | 331 ms | 173 ms |

Inflate is now ~50% of wall time, so further wins must come from unfilter, I/O, and the post-blend pass.

## 2. Bottlenecks found in current FastPng

1. **Per-pixel SSE loop even for `Up`/`None`** – `bpp==4` does `Load4/Add4/Store4` 5 760× per row (18.6 M iterations). `Up` and `None` have no left dependency and can do 32 B/iteration (8× fewer ops).
2. **Branch per-pixel on `ft`** – `if (ft==0)…else if…` inside the hottest loop prevents inlining and causes branch mispredicts on mixed filter streams.
3. **RGB (`bpp==3`) fused scalar loop** – reported as “~2.5× slower than scalar for SIMD” but still processes 18.6 M bytes byte-by-byte; `Up`/`None` rows there are also vectorizable.
4. **`libdeflate_alloc/free` per image** – 10–15 µs + heap churn; hurts small PNGs (Downloads folder median ~1–4 ms).
5. **`vector::insert` per IDAT chunk** – per-chunk capacity checks and `memmove`; avoidable with single `resize + memcpy`.
6. **File I/O via `CreateFile + GlobalAlloc + ReadFile`** – extra kernel copy and `GlobalLock` overhead vs zero-copy `CMemoryMappedFile` already used for JPEG.
7. **Post-decode alpha blend loop** (`ImageLoadThread`) iterates 18.6 M pixels scalar even when 99% are opaque – ~10–15 ms on large image.
8. **No palette/tRNS fast path** – icons/small palette PNGs fall back to GDI+ (measured 2–3× slower than FastPng would be).

## 3. Optimizations (ordered by ROI)

### A. Wide AVX2 kernels for `None` and `Up` (bpp==4) — ~18–22 ms saved on large RBGA
* Dispatch per row (`switch(ft)`) to avoid per-pixel branches.
* `None`: 32 B load → shuffle (`_mm256_shuffle_epi8`) → store BGRA, plus store row.
* `Up`: 32 B load filtered + 32 B load prev → `_mm256_add_epi8` → store row + shuffle emit.
* Tail scalar (<32 B). Uses `_mm256_loadu/storeu` so no alignment requirement.
* Expected: 720 ops/row vs 5760 ops/row → ~8× fewer instructions for those rows. If 40% of rows are Up/None → ~15–20 ms saving.

### B. Split RGB `Up`/`None` into vector add + scalar emit — ~8–12 ms on RGB photos
* First pass: 32 B vector add in-place (same as above, but stride = w*3, rout stride = w*4 separate).
* Second pass: trivial `B= row[i+2], G=row[i+1], R=row[i], A=255` scalar emit (no filter math). Keeps correctness but removes `PaethScalar` from hot path for those rows.
* Keeps scalar fused path for `Sub/Avg/Paeth` where left dependency prevents simple widening.

### C. Dispatch per row, remove per-pixel `ft` branches — ~3–5 ms
* `UnfilterRow` becomes `switch(ft)` at row entry calling specialized `UnfilterRow*` helpers; inner loops are straight-line.
* Allows compiler to unroll and keep `PaethPixel16` out of `Up`/`None` paths.

### D. Thread-local `libdeflate_decompressor` reuse — ~0.1 ms large, ~0.3–0.6 ms small
* `thread_local libdeflate_decompressor* t_dec` lazy-initialized, never freed per image.
* Saves allocator churn; small PNGs (70 KB) decode in 0.5 ms where 0.2 ms was alloc/free.

### E. Single `resize + memcpy` IDAT gather — ~0.5 ms large
* First scan computes `totalIdat`, `idat.resize(totalIdat)`, second scan `memcpy(dst, ...)`. No `vector::insert` overhead.

### F. Zero-copy file mapping for PNG — ~5–8 ms large, ~0.2 ms small
* Replace `CreateFile+GlobalAlloc+ReadFile` with `CMemoryMappedFile` (already used for JPEG/HEIF). Eliminates extra copy and `HGLOBAL` lock.
* Requires changing `PngReader::ReadImage` to accept pointer+size from mmap (already does) – only `ImageLoadThread::ProcessReadPNGRequest` changes.

### G. Fused or AVX2-accelerated alpha blend — ~10 ms large when alpha present, ~4 ms when opaque (fast skip)
* Option G1 (chosen): Keep emit as shuffle-only; replace post-loop in `ImageLoadThread` with AVX2 fast-blend that:
  - Loads 8 pixels (32 B) → checks 8 alphas via `_mm256_movemask` / `_mm256_cmpeq_epi8`; if all `0xFF` skip.
  - Else does 16-bit ` (c*a + bg*(255-a)+127)/255` via `_mm256_mullo_epi16` etc. (2×8 pixels per 256-bit).
  - Result ~2–3 ms vs 10–15 ms scalar, and correctly skips opaque images in ~0.5 ms.
* Option G2 (future): Pass `bgColor` into `FastPngDecode` and blend during emit – would eliminate second pass entirely (0 ms). Implemented as optional overload with default so existing callers keep working; `ImageLoadThread` will pass `ColorTransparency()` and set a flag to skip post-blend when FastPng handled it.

### H. Palette & tRNS expansion LUT (future, small-file ROI)
* Parse `PLTE` + `tRNS` chunks, build 256-entry BGRA LUT, decode IDAT as indexed bytes (bit depths 1/2/4/8) and expand via LUT during emit. Moves ~30% of Downloads PNGs from GDI+ (~9 ms) to FastPng (~3 ms). Not required for large-photo win but listed for completeness. Not implemented in this pass to keep risk low; plan kept for next iteration.

## 4. Correctness invariants

* No downsampling, no color shift: output remains 8-bit BGRA, same as GDI+ fallback.
* Filter math stays integer-exact: `Up/Add` uses `_mm256_add_epi8` (mod 256, correct), `Average` corrects `_mm_avg_epu8` rounding (`avg = _mm_avg - ((a^b)&1)`), `Paeth` stays in 16-bit lanes (spec requires signed `p = a+b-c`).
* `pngbench --dump` vs Pillow byte-identical for corpus (palette/gray/16-bit/interlaced/tRNS fallbacks) and `BlackMarble`.
* `run_tests.py` must stay PASS; `verify_heic` unaffected.

## 5. Implementation order (this PR)

1. `src/JPEGView/FastPng.cpp` – add thread-local decompressor, single-resize IDAT, per-row dispatch, AVX2 `None`/`Up` for bpp==4, split vector `Up` for bpp==3, keep scalar Paeth/Avg fallback, add optional `bgColor` overload.
2. `src/JPEGView/FastPng.h` – expose new overload, keep backward compat.
3. `src/JPEGView/PNGWrapper.cpp` – forward `bgColor` when available.
4. `src/JPEGView/ImageLoadThread.cpp` – switch PNG to `CMemoryMappedFile`, use new FastPng overload with transparency color, replace scalar alpha blend loop with AVX2 fast path (with early-opaque skip).
5. `benchmarks/pngbench/pngbench.cpp` – optional: add `--fast` vs `--slow` timing to keep A/B.

## 6. Expected gains (conservative, i5-12400F)

* Large RGBA 18.7 MP: **117 ms → ~70–80 ms** (≈1.5× faster than current FastPng, ≈4× vs libpng). End-to-end first paint **173 ms → ~125 ms**.
* Large RGB 18 MP: **~165 ms → ~95–105 ms** (vector Up saves + RGB emit).
* Small PNGs (Downloads median): **0.6–4 ms → 0.3–2.5 ms** (mmap + decompressor reuse + palette next PR).
* Peak working set unchanged (~189 MB large), minus one `HGLOBAL` copy.

## 7. Verification

* `cmd /c benchmarks/pngbench/build_bench.bat` → `pngbench.exe <file> 10 3` for `large_png` and `corpus/*`.
* `python benchmarks/pngbench/run_tests.py` → 100% PASS.
* `python benchmarks/benchmark_downloads.py` (or `benchmark_all_images.py`) → PNG speedup column shows improvement without regressions.

## 8. Risks & mitigations

* AVX2 shuffle tables incorrect → corrupted BGRA (red/blue swap) → caught by Pillow diff at pixel 0.
* 32-B tail handling off-by-one → read past `raw` buffer → guard with `i+32<=stride` and scalar tail.
* `thread_local` decompressor leak on thread exit → OS frees at process exit; acceptable for viewer (few threads).
* PrefetchVirtualMemory on mmap may stall → keep but measure; fallback to plain `MapViewOfFile` already fast.
