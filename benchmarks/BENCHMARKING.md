# JPEGView Benchmarking & Performance Notes

Hard-won lessons from the TTFP optimization work (RAW15538.JPG, 17.13 MB,
8192x5464 baseline JPEG). Read this BEFORE trusting or debugging any
benchmark number in this repository.

## 1. THE PAGE-HEAP TRAP (cost us hours - check this FIRST)

**Symptom:** every measurement of `JPEGView.exe` is 1.5-2.5x slower than
expected, *proportionally across all phases* (startup, walk, render,
allocations), while any other executable (e.g. `tools/proftool.exe`) runs
at full speed. Survives reboot. Variance is LOW - it is a stable
regression, not noise.

**Cause:** `gflags /p /enable <exe> /full` writes a PERSISTENT registry
entry under

```
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\<exe name>
    GlobalFlag    REG_SZ    0x02000000   (FLG_HEAP_PAGE_ALLOCS)
    PageHeapFlags REG_SZ    0x3
```

Full Page Heap gives every heap allocation its own committed pages with
guard pages and no reuse. The decoder performs hundreds of thousands of
small allocations per image (walk snapshots, band JPEG vectors, ...), so
the tax applies everywhere at once. The gflags command may REPORT an
elevation error while still having written the key - do not trust the
error message, check the registry.

**Fix (elevated shell):**

```powershell
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\JPEGView.exe" /f
```

**Detection shortcut:** run the identical decode under a different
executable name (copy the exe). If the copy is fast and the original is
slow, suspect IFEO/PageHeap immediately.
`benchmarks/harness/process_runner.py` prints a loud warning when it
detects an active page-heap entry for the target executable.

## 2. MEASUREMENT OBSERVER EFFECTS

- **Never poll `GetProcessMemoryInfo` while the child runs.** It takes
  the target process's address-space lock and directly stalls its decode
  threads (+15-25 ms TTFP was measured). PeakWorkingSetSize is a
  high-water mark: query ONCE after process exit for an exact value with
  zero in-flight overhead.
- **No window enumeration during startup** (`EnumWindows` over all top-
  level windows every few ms steals CPU from the child). Modern binaries
  self-report via `/benchmark:<file>` telemetry and exit themselves via
  `/benchmark_exit`.
- Background agent/tooling processes burning even ~70% of ONE core
  inflate parallel-decode wall time substantially because the decode
  pipeline already saturates all logical CPUs. Close them or lower their
  priority before benchmarking.

## 3. DECODE PIPELINE FACTS (i5-12400F, 6C/12T)

Measured on RAW15538.JPG with tools/proftool:

| Phase                              | Wall    |
|------------------------------------|---------|
| Speculative walk (12 slices)       | ~20 ms  |
| Band render (11 bands, libjpeg)    | ~21 ms  |
| One-shot display resample (pool)   | ~18-25 ms |
| Single-threaded reference decode   | ~170 ms |

- The pipeline is CPU-saturated: overlapping two CPU-bound phases
  (e.g. pipelined resampling chunks against band rendering) does NOT
  reduce wall time on this core count - measured +65 ms worse. The
  one-shot late-start resample inside Decode's progress callback wins.
- Fewer walker/band threads (6) is WORSE than oversubscribing (12/11).
- `RotateToDIB`/`RotateBlockToDIB` write destination rows rounded UP to
  32-row blocks; buffers handed to SampleDown_HQ_SIMD must be padded
  (the function allocates DoPadding(cy,16) rows itself).

## 4. VERIFICATION DISCIPLINE

Every decode-path change must pass:

```
build\bin\Release\proftool.exe --verify benchmarks\actual_test_data
```

which compares parallel-decode output byte-for-byte against the single-
threaded libjpeg reference for all corpus photos (521 baseline files;
16 progressive/unsupported ones are skipped by design). A green verify
has caught multiple real corruption bugs during development.

## 5. USEFUL COMMANDS

```powershell
# Official dedicated benchmark (rebuilds current tree):
python benchmarks\benchmark_raw15538.py --targets current --iterations 10 --warmups 3

# Quick TTFP against an existing exe:
python benchmarks\quick_ttfp.py "src\JPEGView\bin\x64\Release\JPEGView.exe" 10 3

# Corpus pixel-exactness:
build\bin\Release\proftool.exe --verify benchmarks\actual_test_data

# Decode phase breakdown (stderr):
$env:JPEGVIEW_PJ_PROF="1"; build\bin\Release\proftool.exe <image> 5
```
