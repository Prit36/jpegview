# JPEGView Systematic Performance Benchmark Suite

A modular, high-precision, multi-tiered benchmark system designed to systematically evaluate performance, analyze algorithm breakdowns, and detect regressions across:
1. **Original Fork** (upstream fork baseline `upstream/master` / `efd55a1`)
2. **Last Git Commit** (`HEAD~1`)
3. **Current Working Copy** (uncommitted active changes)
4. *Custom Targets*: Any branch, commit hash, tag, or pre-built binary.

---

## Key Features

- **Automated Worktree Isolation**: Builds comparison targets in isolated cache directories without modifying active working copy files.
- **Time to First Paint (TTFP)**: Measures sub-millisecond cold and warm startup latency from process launch to full window image display.
- **Massive Image Stress Testing (100MB - 200MB)**: Evaluates memory consumption (Working Set / RSS), decoder execution, and downsampling throughput on extreme 16K-24K resolution assets.
- **High-Speed Directory Traversal**: Profiles folder browsing throughput (FPS), switch latency per frame, and UI frame pacing / jitter across 100 to 1,000 files.
- **Native C++ SIMD Micro-Benchmarks**: Tests AVX2 vs SSE vs Scalar resampling, Unsharp Mask sharpening, tone-mapping LUTs, and alphanumeric sorting in isolation.
- **Multi-Format Visual Reports**:
  - **Console**: ANSI-colored comparative tables with speedup/regression status.
  - **Markdown**: GitHub-ready tables and percentile distributions (`benchmarks/results/benchmark_report.md`).
  - **Interactive HTML**: Self-contained dark-mode dashboard with Chart.js charts and latency waterfalls (`benchmarks/results/benchmark_report.html`).
  - **JSON**: Machine-readable metrics for automated CI pipelines (`benchmarks/results/benchmark_results.json`).

---

## Quick Start

### 1. Run 3-Way Target Comparison
```powershell
# Fast smoke comparison (~30s)
python benchmarks/run_benchmark.py compare --profile quick

# Or using PowerShell launcher
.\benchmarks\run_benchmark.ps1 -Profile quick
```

### 2. Standard PR & Regression Benchmark (~1-2 min)
```powershell
python benchmarks/run_benchmark.py compare --profile standard
```

### 3. Deep Stress Benchmark (200MB Images & 1,000 files)
```powershell
python benchmarks/run_benchmark.py compare --profile stress
```

### 4. Run Native Algorithm Micro-Benchmarks
```powershell
python benchmarks/run_benchmark.py micro --iterations 10
```

### 5. Generate Test Assets Only
```powershell
python benchmarks/run_benchmark.py generate-assets --profile standard
```

---

## Test Profiles

| Profile | Target Large Image | Folder Files | Warmup Iterations | Measure Iterations | Typical Runtime |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`quick`** | 8K x 8K (~50MB) | 50 files | 1 | 3 | ~15-30s / target |
| **`standard`** | 16K x 16K (~120MB) | 200 files | 2 | 5 | ~1-2m / target |
| **`stress`** | 24K x 24K (~200MB) | 1,000 files | 3 | 10 | ~3-5m / target |
| **`ci`** | 12K x 12K (~80MB) | 100 files | 2 | 5 | ~1m / target |

---

## Directory Architecture

```
benchmarks/
├── README.md                           # Documentation and usage guide
├── run_benchmark.py                    # Unified Python CLI entry point
├── run_benchmark.ps1                   # Windows PowerShell launcher script
├── config/
│   ├── benchmark_config.json           # Global suite thresholds and target definitions
│   └── test_profiles.json              # Presets (quick, standard, stress, ci)
├── generators/
│   ├── asset_generator.py              # Generates synthetic large images and test folders
│   └── image_patterns.py               # Deterministic gradients, high-freq patterns, noise
├── harness/
│   ├── system_info.py                  # Hardware telemetry collector (CPU, AVX2, RAM, QPC)
│   ├── git_manager.py                  # Worktree isolation manager for git targets
│   ├── builder.py                      # MSBuild & CMake compilation orchestrator
│   ├── process_runner.py               # High-precision process telemetry monitor
│   ├── e2e_benchmark.py                # End-to-End latency and throughput test suites
│   ├── micro_benchmark.py              # Native C++ algorithm benchmark runner
│   └── comparator.py                   # Multi-target comparator and regression analyzer
├── engine/                             # Native C++ micro-benchmark engine
│   ├── CMakeLists.txt                  # Standalone CMake build script
│   └── BenchmarkMain.cpp               # C++ test harness for SIMD kernels
├── reporters/
│   ├── console_reporter.py             # ANSI-colored terminal output
│   ├── markdown_reporter.py            # GitHub-formatted Markdown report
│   ├── json_reporter.py                # Raw structured JSON export
│   └── html_reporter.py                # Interactive HTML dashboard with Chart.js
├── assets/                             # Synthetic test assets (gitignored)
└── results/                            # Benchmark reports and logs (gitignored)
```
