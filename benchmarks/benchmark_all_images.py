#!/usr/bin/env python3
"""
JPEGView Comprehensive All-Images Benchmark Suite.
Benchmarks every image in actual_test_data (or custom folder) with multiple runs per image.
Identifies worst-performing images, single-threaded fallbacks, resample bottlenecks, and throughput metrics.
Modern Python 3.12+ implementation.
"""

from __future__ import annotations

import sys
import os
import csv
import json
import time
import argparse
import statistics
from pathlib import Path
from typing import Any

# Path setup
BENCHMARKS_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCHMARKS_DIR.parent
sys.path.insert(0, str(BENCHMARKS_DIR))

from harness.system_info import get_system_telemetry
from harness.git_manager import GitManager
from harness.builder import TargetBuilder
from harness.process_runner import ProcessRunner


# Terminal ANSI Colors
RESET = "\033[0m"
BOLD = "\033[1m"
GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
MAGENTA = "\033[35m"
DIM = "\033[2m"


def load_json(path: Path) -> dict[str, Any]:
    if path.exists():
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            return {}
    return {}


def calc_stats(samples: list[float]) -> dict[str, float]:
    if not samples:
        return {"mean": 0.0, "median": 0.0, "min": 0.0, "max": 0.0, "stddev": 0.0, "p90": 0.0, "p95": 0.0, "p99": 0.0}

    sorted_s = sorted(samples)
    p90_idx = min(len(sorted_s) - 1, int(0.90 * len(sorted_s)))
    p95_idx = min(len(sorted_s) - 1, int(0.95 * len(sorted_s)))
    p99_idx = min(len(sorted_s) - 1, int(0.99 * len(sorted_s)))

    return {
        "mean": round(statistics.mean(samples), 2),
        "median": round(statistics.median(samples), 2),
        "min": round(min(samples), 2),
        "max": round(max(samples), 2),
        "stddev": round(statistics.stdev(samples), 2) if len(samples) > 1 else 0.0,
        "p90": round(sorted_s[p90_idx], 2),
        "p95": round(sorted_s[p95_idx], 2),
        "p99": round(sorted_s[p99_idx], 2),
    }


def find_images(data_dir: Path, patterns: list[str]) -> list[Path]:
    """Finds and deduplicates all matching images in directory."""
    matched: set[Path] = set()
    for pat in patterns:
        for p in data_dir.glob(pat.strip()):
            if p.is_file():
                matched.add(p.resolve())
    return sorted(list(matched), key=lambda p: p.name)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Benchmark all images in a folder with multiple runs per image to find worst performers and anomalies.",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )

    parser.add_argument(
        "--data-dir",
        type=str,
        default=str(BENCHMARKS_DIR / "actual_test_data"),
        help="Path to folder containing test images (default: benchmarks/actual_test_data)"
    )
    parser.add_argument(
        "--pattern",
        type=str,
        default="*.jpg,*.jpeg,*.JPG,*.JPEG",
        help="Comma-separated glob patterns for images (default: *.jpg,*.jpeg,*.JPG,*.JPEG)"
    )
    parser.add_argument(
        "--targets",
        type=str,
        default="current",
        help="Comma-separated list of targets to benchmark (default: current, e.g. 'original-fork,current')"
    )
    parser.add_argument(
        "--baseline",
        type=str,
        default="original-fork",
        help="Target to use as baseline when multiple targets are compared (default: original-fork)"
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=3,
        help="Number of measurement runs per image (default: 3)"
    )
    parser.add_argument(
        "--warmups",
        type=int,
        default=1,
        help="Number of warmup runs per image before recording (default: 1)"
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Limit number of images to benchmark (0 for all, default: 0)"
    )
    parser.add_argument(
        "--top-n",
        type=int,
        default=25,
        help="Number of worst-performing images to highlight in the ranking table (default: 25, 0 for all)"
    )
    parser.add_argument(
        "--sort-by",
        type=str,
        choices=["ttfp_desc", "ttfp_asc", "decode_desc", "resample_desc", "size_desc", "name"],
        default="ttfp_desc",
        help="Sort order for results table (default: ttfp_desc - worst performers first)"
    )
    parser.add_argument(
        "--threshold-ms",
        type=float,
        default=120.0,
        help="TTFP threshold in ms above which an image is flagged as slow (default: 120.0)"
    )
    parser.add_argument(
        "--force-rebuild",
        action="store_true",
        help="Force clean recompilation of target binaries"
    )
    parser.add_argument(
        "--save-json",
        type=str,
        default=str(BENCHMARKS_DIR / "results" / "all_images_benchmark_results.json"),
        help="Output path for JSON results"
    )
    parser.add_argument(
        "--save-csv",
        type=str,
        default=str(BENCHMARKS_DIR / "results" / "all_images_benchmark_results.csv"),
        help="Output path for CSV results"
    )
    parser.add_argument(
        "--save-md",
        type=str,
        default=str(BENCHMARKS_DIR / "results" / "all_images_benchmark_report.md"),
        help="Output path for Markdown report"
    )

    args = parser.parse_args()

    data_dir = Path(args.data_dir).resolve()
    if not data_dir.exists() or not data_dir.is_dir():
        print(f"{RED}[-] Error: Images directory not found: {data_dir}{RESET}")
        sys.exit(1)

    patterns = [p.strip() for p in args.pattern.split(",") if p.strip()]
    images = find_images(data_dir, patterns)

    if not images:
        print(f"{RED}[-] Error: No images matching {args.pattern} found in: {data_dir}{RESET}")
        sys.exit(1)

    if args.limit > 0:
        images = images[:args.limit]

    total_images = len(images)
    total_size_mb = sum(img.stat().st_size for img in images) / (1024 * 1024)

    print("=" * 100)
    print(f"  {BOLD}{CYAN}JPEGView Comprehensive All-Images Benchmark Suite{RESET}")
    print(f"  Directory:     {BOLD}{data_dir}{RESET}")
    print(f"  Total Images:  {BOLD}{total_images}{RESET} files ({total_size_mb:.2f} MB total)")
    print(f"  Runs / Image:  {BOLD}{args.iterations}{RESET} iterations ({args.warmups} warmups)")
    print(f"  Targets:       {BOLD}{args.targets}{RESET}")
    print(f"  SLA Threshold: {BOLD}{args.threshold_ms:.1f} ms{RESET}")
    print("=" * 100)

    config = load_json(BENCHMARKS_DIR / "config" / "benchmark_config.json")
    git_mgr = GitManager(REPO_ROOT)
    builder = TargetBuilder(REPO_ROOT)
    runner = ProcessRunner()

    target_names = [t.strip() for t in args.targets.split(",") if t.strip()]
    all_target_results: dict[str, dict[str, Any]] = {}

    start_suite_time = time.perf_counter()

    for t_name in target_names:
        print(f"\n[{t_name.upper()}] Preparing target '{t_name}'...")
        git_ref, desc = git_mgr.resolve_target_ref(t_name, config)
        commit_hash = git_mgr.get_commit_hash(git_ref if git_ref != "WORKING_TREE" else "HEAD")
        print(f"    Git Ref: {git_ref} ({commit_hash[:8]}) - {desc}")

        source_dir = git_mgr.prepare_target_source(t_name, git_ref)
        exe_path = builder.build_target(t_name, source_dir, commit_hash, force_rebuild=args.force_rebuild).resolve()

        print(f"\n[*] Benchmarking {total_images} images on target '{t_name}'...")
        print(f"{'#':<5} | {'Image':<20} | {'Size':<9} | {'TTFP Mean':<11} | {'Decode':<10} | {'Resample':<10} | {'RAM':<9} | {'Status':<10}")
        print("-" * 100)

        image_results: list[dict[str, Any]] = []

        try:
            for idx, img_path in enumerate(images, 1):
                file_size_mb = img_path.stat().st_size / (1024 * 1024)

                # Warmup runs
                for _ in range(args.warmups):
                    runner.run_image_load_benchmark(exe_path, img_path)

                # Measurement runs
                ttfp_samples: list[float] = []
                decode_samples: list[float] = []
                resample_samples: list[float] = []
                ram_samples: list[float] = []
                exit_codes: list[int | None] = []
                has_telemetries: list[bool] = []

                for _ in range(args.iterations):
                    sample = runner.run_image_load_benchmark(exe_path, img_path)
                    ttfp_samples.append(sample["ttfp_ms"])
                    decode_samples.append(sample["load_ms"])
                    resample_samples.append(sample["last_op_ms"])
                    ram_samples.append(sample["peak_working_set_mb"])
                    exit_codes.append(sample["exit_code"])
                    has_telemetries.append(sample["has_internal_telemetry"])

                ttfp_stats = calc_stats(ttfp_samples)
                decode_stats = calc_stats(decode_samples)
                resample_stats = calc_stats(resample_samples)
                ram_stats = calc_stats(ram_samples)

                # Fallback / status detection:
                # - Fallback if resample > 5.0ms (UI thread had to resample instead of parallel late-resample)
                # - Slow if TTFP > threshold
                # - Error if exit code non-zero
                is_fallback = (resample_stats["mean"] > 5.0)
                is_slow = (ttfp_stats["mean"] > args.threshold_ms)
                has_error = any(code != 0 and code is not None for code in exit_codes)

                if has_error:
                    status_str = f"{RED}ERROR{RESET}"
                    status_plain = "ERROR"
                elif is_fallback:
                    status_str = f"{RED}FALLBACK{RESET}"
                    status_plain = "FALLBACK"
                elif is_slow:
                    status_str = f"{YELLOW}SLOW{RESET}"
                    status_plain = "SLOW"
                else:
                    status_str = f"{GREEN}OK{RESET}"
                    status_plain = "OK"

                # Calculate decode throughput (MB/s)
                decode_sec = (decode_stats["mean"] / 1000.0) if decode_stats["mean"] > 0 else 0.001
                throughput_mb_s = round(file_size_mb / decode_sec, 1)

                img_rec: dict[str, Any] = {
                    "index": idx,
                    "filename": img_path.name,
                    "path": str(img_path),
                    "size_bytes": img_path.stat().st_size,
                    "size_mb": round(file_size_mb, 2),
                    "ttfp_ms": ttfp_stats,
                    "decode_ms": decode_stats,
                    "resample_ms": resample_stats,
                    "ram_mb": ram_stats,
                    "throughput_mb_s": throughput_mb_s,
                    "status": status_plain,
                    "is_fallback": is_fallback,
                    "is_slow": is_slow,
                    "exit_code": exit_codes[0] if exit_codes else None,
                    "has_telemetry": all(has_telemetries),
                    "raw_samples": {
                        "ttfp_ms": ttfp_samples,
                        "decode_ms": decode_samples,
                        "resample_ms": resample_samples,
                        "ram_mb": ram_samples
                    }
                }
                image_results.append(img_rec)

                # Live progress line
                print(f"[{idx:>3}/{total_images}] | {img_path.name:<20} | {file_size_mb:>6.2f} MB | {ttfp_stats['mean']:>7.2f} ms | {decode_stats['mean']:>6.2f} ms | {resample_stats['mean']:>6.2f} ms | {ram_stats['mean']:>6.1f} MB | {status_str:<10}")

        except KeyboardInterrupt:
            print(f"\n{YELLOW}[!] Benchmark interrupted by user. Saving partial results ({len(image_results)} images)...{RESET}")

        # Compute aggregate target statistics
        all_ttfp_means = [r["ttfp_ms"]["mean"] for r in image_results]
        all_decode_means = [r["decode_ms"]["mean"] for r in image_results]
        all_resample_means = [r["resample_ms"]["mean"] for r in image_results]
        all_ram_means = [r["ram_mb"]["mean"] for r in image_results]

        fallback_count = sum(1 for r in image_results if r["is_fallback"])
        slow_count = sum(1 for r in image_results if r["is_slow"])
        sub100_count = sum(1 for r in image_results if r["ttfp_ms"]["mean"] < 100.0)

        target_summary = {
            "target": t_name,
            "git_ref": git_ref,
            "commit_hash": commit_hash,
            "exe_path": str(exe_path),
            "images_tested": len(image_results),
            "total_size_mb": round(sum(r["size_mb"] for r in image_results), 2),
            "overall_ttfp_ms": calc_stats(all_ttfp_means),
            "overall_decode_ms": calc_stats(all_decode_means),
            "overall_resample_ms": calc_stats(all_resample_means),
            "overall_ram_mb": calc_stats(all_ram_means),
            "counts": {
                "sub100ms": sub100_count,
                "sub100ms_pct": round((sub100_count / len(image_results) * 100.0), 1) if image_results else 0.0,
                "slow_over_threshold": slow_count,
                "slow_pct": round((slow_count / len(image_results) * 100.0), 1) if image_results else 0.0,
                "fallbacks": fallback_count,
                "fallback_pct": round((fallback_count / len(image_results) * 100.0), 1) if image_results else 0.0
            },
            "images": image_results
        }
        all_target_results[t_name] = target_summary

    total_suite_elapsed = time.perf_counter() - start_suite_time

    # Sort and display worst-performing analysis for primary/current target
    primary_target = "current" if "current" in all_target_results else target_names[0]
    primary_res = all_target_results[primary_target]
    img_list = list(primary_res["images"])

    # Sorting options
    if args.sort_by == "ttfp_desc":
        img_list.sort(key=lambda x: x["ttfp_ms"]["mean"], reverse=True)
    elif args.sort_by == "ttfp_asc":
        img_list.sort(key=lambda x: x["ttfp_ms"]["mean"])
    elif args.sort_by == "decode_desc":
        img_list.sort(key=lambda x: x["decode_ms"]["mean"], reverse=True)
    elif args.sort_by == "resample_desc":
        img_list.sort(key=lambda x: x["resample_ms"]["mean"], reverse=True)
    elif args.sort_by == "size_desc":
        img_list.sort(key=lambda x: x["size_bytes"], reverse=True)
    elif args.sort_by == "name":
        img_list.sort(key=lambda x: x["filename"])

    top_display = img_list if args.top_n <= 0 else img_list[:args.top_n]

    # Print Summary & Worst Performers Table
    print("\n" + "=" * 108)
    print(f"  {BOLD}{CYAN}RANKED WORST-PERFORMING IMAGES (Sorted by {args.sort_by.upper()}){RESET}")
    print(f"  Showing top {len(top_display)} of {len(img_list)} evaluated images for target '{primary_target}'")
    print("=" * 108)
    print(f"{'Rank':<5} | {'Filename':<20} | {'Size':<9} | {'TTFP Mean':<11} | {'TTFP Min':<10} | {'Decode':<10} | {'Resample':<10} | {'Throughput':<11} | {'Status':<10}")
    print("-" * 108)

    for rank, item in enumerate(top_display, 1):
        fn = item["filename"]
        sz = f"{item['size_mb']:.2f} MB"
        ttfp_m = f"{item['ttfp_ms']['mean']:.2f} ms"
        ttfp_min = f"{item['ttfp_ms']['min']:.2f} ms"
        dec = f"{item['decode_ms']['mean']:.2f} ms"
        res = f"{item['resample_ms']['mean']:.2f} ms"
        tp = f"{item['throughput_mb_s']:.1f} MB/s"

        st = item["status"]
        if st == "FALLBACK" or st == "ERROR":
            st_colored = f"{RED}{st}{RESET}"
        elif st == "SLOW":
            st_colored = f"{YELLOW}{st}{RESET}"
        else:
            st_colored = f"{GREEN}{st}{RESET}"

        print(f"{rank:<5} | {fn:<20} | {sz:>9} | {ttfp_m:>11} | {ttfp_min:>10} | {dec:>10} | {res:>10} | {tp:>11} | {st_colored:<10}")

    print("=" * 108)

    # Print Fallbacks Watchlist if any exist
    fallbacks = [x for x in img_list if x["is_fallback"]]
    if fallbacks:
        print(f"\n{BOLD}{RED}[!] ATTENTION: {len(fallbacks)} Single-Threaded Fallback(s) / High-Resample Anomalies Detected:{RESET}")
        for f_item in fallbacks:
            print(f"    - {f_item['filename']} ({f_item['size_mb']:.2f} MB): Decode={f_item['decode_ms']['mean']:.2f}ms, Resample={f_item['resample_ms']['mean']:.2f}ms (TTFP={f_item['ttfp_ms']['mean']:.2f}ms)")
    else:
        print(f"\n{BOLD}{GREEN}[+] Clean Execution: 0 Fallbacks or Resample Bottlenecks Detected! (100% Parallel Coverage){RESET}")

    # Overall Summary Metrics Table
    ov = primary_res["overall_ttfp_ms"]
    cnts = primary_res["counts"]

    print("\n" + "=" * 80)
    print(f"  {BOLD}OVERALL SUITE PERFORMANCE SUMMARY ({primary_target}){RESET}")
    print("=" * 80)
    print(f"  Total Images Evaluated : {len(img_list)} files ({primary_res['total_size_mb']:.2f} MB)")
    print(f"  Total Benchmark Time   : {total_suite_elapsed:.2f} seconds")
    print(f"  TTFP Mean              : {BOLD}{ov['mean']:.2f} ms{RESET}")
    print(f"  TTFP Median (p50)      : {ov['median']:.2f} ms")
    print(f"  TTFP 90th Percentile   : {ov['p90']:.2f} ms")
    print(f"  TTFP 95th Percentile   : {ov['p95']:.2f} ms")
    print(f"  TTFP 99th Percentile   : {ov['p99']:.2f} ms")
    print(f"  Fastest Image Load     : {ov['min']:.2f} ms")
    print(f"  Slowest Image Load     : {ov['max']:.2f} ms")
    print("-" * 80)
    print(f"  Sub-100ms Pass Rate    : {BOLD}{cnts['sub100ms_pct']:.1f}%{RESET} ({cnts['sub100ms']}/{len(img_list)} images)")
    print(f"  Slow (> {args.threshold_ms:.0f}ms) Rate     : {cnts['slow_pct']:.1f}% ({cnts['slow_over_threshold']}/{len(img_list)} images)")
    print(f"  Fallback Rate          : {cnts['fallback_pct']:.1f}% ({cnts['fallbacks']}/{len(img_list)} images)")
    print("=" * 80)

    # Save JSON results
    out_json = Path(args.save_json)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    with open(out_json, "w", encoding="utf-8") as f:
        json.dump(all_target_results, f, indent=2)
    print(f"\n[+] Saved full JSON results to: {out_json}")

    # Save CSV results
    out_csv = Path(args.save_csv)
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    with open(out_csv, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([
            "target", "index", "filename", "size_mb", "size_bytes",
            "ttfp_mean_ms", "ttfp_median_ms", "ttfp_min_ms", "ttfp_max_ms", "ttfp_stddev_ms",
            "decode_mean_ms", "resample_mean_ms", "ram_peak_mb", "throughput_mb_s",
            "status", "is_fallback", "is_slow", "path"
        ])
        for t_name, t_data in all_target_results.items():
            for r in t_data.get("images", []):
                writer.writerow([
                    t_name, r["index"], r["filename"], r["size_mb"], r["size_bytes"],
                    r["ttfp_ms"]["mean"], r["ttfp_ms"]["median"], r["ttfp_ms"]["min"], r["ttfp_ms"]["max"], r["ttfp_ms"]["stddev"],
                    r["decode_ms"]["mean"], r["resample_ms"]["mean"], r["ram_mb"]["mean"], r["throughput_mb_s"],
                    r["status"], r["is_fallback"], r["is_slow"], r["path"]
                ])
    print(f"[+] Saved CSV spreadsheet to:   {out_csv}")

    # Save Markdown report
    out_md = Path(args.save_md)
    out_md.parent.mkdir(parents=True, exist_ok=True)
    md_content = generate_markdown_report(all_target_results, primary_target, top_display, args)
    out_md.write_text(md_content, encoding="utf-8")
    print(f"[+] Saved Markdown report to:    {out_md}")


def generate_markdown_report(all_results: dict[str, Any], primary_target: str, top_display: list[dict[str, Any]], args: Any) -> str:
    primary = all_results[primary_target]
    ov = primary["overall_ttfp_ms"]
    cnts = primary["counts"]

    md = f"""# JPEGView All-Images Benchmark Report

- **Date:** {time.strftime('%Y-%m-%d %H:%M:%S')}
- **Target:** `{primary_target}` ({primary.get('commit_hash', 'unknown')[:8]})
- **Images Evaluated:** {primary['images_tested']} files ({primary['total_size_mb']:.2f} MB)
- **Runs per Image:** {args.iterations} measurement runs ({args.warmups} warmups)
- **SLA Threshold:** {args.threshold_ms:.1f} ms

---

## Executive Summary

| Metric | Value |
| :--- | :---: |
| **TTFP Mean** | **{ov['mean']:.2f} ms** |
| **TTFP Median (p50)** | **{ov['median']:.2f} ms** |
| **TTFP 95th Percentile** | **{ov['p95']:.2f} ms** |
| **TTFP 99th Percentile** | **{ov['p99']:.2f} ms** |
| **Fastest Load** | **{ov['min']:.2f} ms** |
| **Slowest Load** | **{ov['max']:.2f} ms** |
| **Sub-100ms Pass Rate** | **{cnts['sub100ms_pct']:.1f}%** ({cnts['sub100ms']}/{primary['images_tested']}) |
| **Single-Threaded Fallbacks** | **{cnts['fallbacks']}** ({cnts['fallback_pct']:.1f}%) |

---

## Top Worst-Performing Images (Sorted by {args.sort_by})

| Rank | Filename | Size (MB) | TTFP Mean (ms) | TTFP Min (ms) | Decode (ms) | Resample (ms) | Throughput (MB/s) | Status |
| :---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
"""
    for rank, r in enumerate(top_display, 1):
        md += f"| {rank} | `{r['filename']}` | {r['size_mb']:.2f} | **{r['ttfp_ms']['mean']:.2f}** | {r['ttfp_ms']['min']:.2f} | {r['decode_ms']['mean']:.2f} | {r['resample_ms']['mean']:.2f} | {r['throughput_mb_s']:.1f} | `{r['status']}` |\n"

    fallbacks = [x for x in primary["images"] if x["is_fallback"]]
    if fallbacks:
        md += "\n---\n\n## ⚠️ Single-Threaded Fallback & Resample Anomalies\n\n"
        md += "| Filename | Size (MB) | Decode Time | Resample Overhead | TTFP |\n| :--- | :---: | :---: | :---: | :---: |\n"
        for f in fallbacks:
            md += f"| `{f['filename']}` | {f['size_mb']:.2f} | {f['decode_ms']['mean']:.2f} ms | {f['resample_ms']['mean']:.2f} ms | **{f['ttfp_ms']['mean']:.2f} ms** |\n"
    else:
        md += "\n---\n\n## ✅ Quality Gate: No Fallbacks Detected\n\n100% of tested images used the high-performance parallel decoding engine with zero UI-thread resampling stalls.\n"

    return md


if __name__ == "__main__":
    main()
