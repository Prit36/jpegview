#!/usr/bin/env python3
"""
JPEGView Multi-Format Downloads & Folder Benchmark Suite.
Scans Downloads (or any arbitrary folder) across multiple image formats (JPG, PNG, WEBP, HEIC, JXL, AVIF, BMP, DNG, RAW, etc.),
runs multi-iteration benchmarks per image, generates format-by-format breakdowns, and ranks the worst performers.
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

# Supported image extension mapping to format families
SUPPORTED_EXTENSIONS: dict[str, str] = {
    ".jpg": "JPEG",
    ".jpeg": "JPEG",
    ".jpe": "JPEG",
    ".jfif": "JPEG",
    ".png": "PNG",
    ".webp": "WEBP",
    ".jxl": "JXL",
    ".avif": "AVIF",
    ".heic": "HEIF",
    ".heif": "HEIF",
    ".hif": "HEIF",
    ".avci": "HEIF",
    ".bmp": "BMP",
    ".dib": "BMP",
    ".tga": "TGA",
    ".psd": "PSD",
    ".qoi": "QOI",
    ".gif": "GIF",
    ".tiff": "TIFF",
    ".tif": "TIFF",
    ".dng": "CameraRAW",
    ".cr2": "CameraRAW",
    ".cr3": "CameraRAW",
    ".nef": "CameraRAW",
    ".nrw": "CameraRAW",
    ".arw": "CameraRAW",
    ".srf": "CameraRAW",
    ".sr2": "CameraRAW",
    ".orf": "CameraRAW",
    ".pef": "CameraRAW",
    ".rw2": "CameraRAW",
    ".raf": "CameraRAW",
    ".raw": "CameraRAW",
    ".rwl": "CameraRAW",
    ".ico": "WIC",
    ".jxr": "WIC",
    ".wdp": "WIC",
    ".hdp": "WIC"
}


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


def find_folder_images(
    folder: Path,
    recursive: bool = False,
    allowed_exts: set[str] | None = None,
    allowed_formats: set[str] | None = None,
    min_size_kb: float = 0.0,
    max_size_mb: float = 0.0
) -> list[tuple[Path, str]]:
    """Discovers all supported image files in folder, returning (path, format_family)."""
    matched: list[tuple[Path, str]] = []
    seen: set[Path] = set()

    iterator = folder.rglob("*") if recursive else folder.glob("*")

    for p in iterator:
        if not p.is_file():
            continue
        ext = p.suffix.lower()
        if ext not in SUPPORTED_EXTENSIONS:
            continue
        fmt = SUPPORTED_EXTENSIONS[ext]

        if allowed_exts and ext not in allowed_exts:
            continue
        if allowed_formats and fmt.lower() not in allowed_formats and ext.lower() not in allowed_formats:
            continue

        size_kb = p.stat().st_size / 1024.0
        size_mb = size_kb / 1024.0

        if min_size_kb > 0 and size_kb < min_size_kb:
            continue
        if max_size_mb > 0 and size_mb > max_size_mb:
            continue

        resolved = p.resolve()
        if resolved not in seen:
            seen.add(resolved)
            matched.append((resolved, fmt))

    return sorted(matched, key=lambda x: (x[1], x[0].name.lower()))


def main() -> None:
    default_downloads = Path.home() / "Downloads"

    parser = argparse.ArgumentParser(
        description="Scan and benchmark all images in Downloads (or custom folder) across multiple formats.",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )

    parser.add_argument(
        "--folder", "-d",
        type=str,
        default=str(default_downloads),
        help=f"Path to folder to scan (default: {default_downloads})"
    )
    parser.add_argument(
        "--recursive", "-r",
        action="store_true",
        help="Scan directory recursively including subdirectories"
    )
    parser.add_argument(
        "--formats",
        type=str,
        default="",
        help="Comma-separated format families to filter (e.g. 'JPEG,PNG,HEIF,WEBP,CameraRAW', default: all)"
    )
    parser.add_argument(
        "--ext",
        type=str,
        default="",
        help="Comma-separated file extensions to filter (e.g. '.png,.heic,.dng', default: all supported)"
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
        help="Baseline target name when comparing multiple targets (default: original-fork)"
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
        help="Limit total number of images to benchmark (0 for all, default: 0)"
    )
    parser.add_argument(
        "--min-size-kb",
        type=float,
        default=5.0,
        help="Ignore files smaller than this size in KB (default: 5.0 KB to filter tiny icons)"
    )
    parser.add_argument(
        "--max-size-mb",
        type=float,
        default=0.0,
        help="Ignore files larger than this size in MB (0 for unlimited, default: 0)"
    )
    parser.add_argument(
        "--top-n",
        type=int,
        default=25,
        help="Number of worst-performing images to display in summary table (default: 25, 0 for all)"
    )
    parser.add_argument(
        "--sort-by",
        type=str,
        choices=["ttfp_desc", "ttfp_asc", "decode_desc", "resample_desc", "size_desc", "format", "name"],
        default="ttfp_desc",
        help="Sort order for results table (default: ttfp_desc - worst performers first)"
    )
    parser.add_argument(
        "--threshold-ms",
        type=float,
        default=150.0,
        help="TTFP threshold in ms to flag an image as slow (default: 150.0)"
    )
    parser.add_argument(
        "--force-rebuild",
        action="store_true",
        help="Force clean recompilation of target binaries"
    )
    parser.add_argument(
        "--save-json",
        type=str,
        default=str(BENCHMARKS_DIR / "results" / "downloads_benchmark_results.json"),
        help="Output path for JSON results"
    )
    parser.add_argument(
        "--save-csv",
        type=str,
        default=str(BENCHMARKS_DIR / "results" / "downloads_benchmark_results.csv"),
        help="Output path for CSV results"
    )
    parser.add_argument(
        "--save-md",
        type=str,
        default=str(BENCHMARKS_DIR / "results" / "downloads_benchmark_report.md"),
        help="Output path for Markdown report"
    )

    args = parser.parse_args()

    folder_path = Path(args.folder).resolve()
    if not folder_path.exists() or not folder_path.is_dir():
        print(f"{RED}[-] Error: Specified folder not found: {folder_path}{RESET}")
        sys.exit(1)

    allowed_exts: set[str] | None = None
    if args.ext:
        allowed_exts = {e.strip().lower() if e.strip().startswith(".") else f".{e.strip().lower()}" for e in args.ext.split(",") if e.strip()}

    allowed_formats: set[str] | None = None
    if args.formats:
        allowed_formats = {f.strip().lower() for f in args.formats.split(",") if f.strip()}

    discovered = find_folder_images(
        folder_path,
        recursive=args.recursive,
        allowed_exts=allowed_exts,
        allowed_formats=allowed_formats,
        min_size_kb=args.min_size_kb,
        max_size_mb=args.max_size_mb
    )

    if not discovered:
        print(f"{YELLOW}[!] No matching image files found in: {folder_path}{RESET}")
        print(f"    Supported formats: {', '.join(sorted(set(SUPPORTED_EXTENSIONS.values()))) }")
        sys.exit(0)

    if args.limit > 0:
        discovered = discovered[:args.limit]

    total_images = len(discovered)
    total_bytes = sum(p.stat().st_size for p, _ in discovered)
    total_size_mb = total_bytes / (1024 * 1024)

    # Format distribution count
    format_counts: dict[str, int] = {}
    format_sizes: dict[str, float] = {}
    for p, fmt in discovered:
        format_counts[fmt] = format_counts.get(fmt, 0) + 1
        format_sizes[fmt] = format_sizes.get(fmt, 0.0) + (p.stat().st_size / (1024 * 1024))

    fmt_dist_str = ", ".join([f"{fmt}: {cnt} ({format_sizes[fmt]:.1f}MB)" for fmt, cnt in sorted(format_counts.items(), key=lambda x: -x[1])])

    print("=" * 105)
    print(f"  {BOLD}{CYAN}JPEGView Downloads & Mixed-Format Folder Benchmark{RESET}")
    print(f"  Target Folder: {BOLD}{folder_path}{RESET} ({'Recursive' if args.recursive else 'Top-level'})")
    print(f"  Total Images:  {BOLD}{total_images}{RESET} files ({total_size_mb:.2f} MB)")
    print(f"  Format Breakdown: {fmt_dist_str}")
    print(f"  Runs / Image:  {BOLD}{args.iterations}{RESET} iterations ({args.warmups} warmups)")
    print(f"  SLA Threshold: {BOLD}{args.threshold_ms:.1f} ms{RESET}")
    print("=" * 105)

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

        print(f"\n[*] Benchmarking {total_images} images on '{t_name}'...")
        print(f"{'#':<5} | {'Format':<10} | {'Image':<28} | {'Size':<9} | {'TTFP Mean':<11} | {'Decode':<10} | {'Resample':<10} | {'RAM':<9} | {'Status':<10}")
        print("-" * 115)

        image_results: list[dict[str, Any]] = []

        try:
            for idx, (img_path, img_fmt) in enumerate(discovered, 1):
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

                # Status analysis
                is_fallback = (img_fmt == "JPEG" and resample_stats["mean"] > 5.0)
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

                decode_sec = (decode_stats["mean"] / 1000.0) if decode_stats["mean"] > 0 else 0.001
                throughput_mb_s = round(file_size_mb / decode_sec, 1)

                display_name = img_path.name if len(img_path.name) <= 28 else (img_path.name[:25] + "...")

                img_rec: dict[str, Any] = {
                    "index": idx,
                    "filename": img_path.name,
                    "path": str(img_path),
                    "extension": img_path.suffix.lower(),
                    "format": img_fmt,
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

                print(f"[{idx:>3}/{total_images}] | {img_fmt:<10} | {display_name:<28} | {file_size_mb:>6.2f} MB | {ttfp_stats['mean']:>7.2f} ms | {decode_stats['mean']:>6.2f} ms | {resample_stats['mean']:>6.2f} ms | {ram_stats['mean']:>6.1f} MB | {status_str:<10}")

        except KeyboardInterrupt:
            print(f"\n{YELLOW}[!] Benchmark interrupted by user. Saving partial results ({len(image_results)} images)...{RESET}")

        # Compute per-format statistics
        by_format: dict[str, dict[str, Any]] = {}
        for fmt in sorted(set(r["format"] for r in image_results)):
            fmt_records = [r for r in image_results if r["format"] == fmt]
            fmt_ttfsp = [r["ttfp_ms"]["mean"] for r in fmt_records]
            fmt_decodes = [r["decode_ms"]["mean"] for r in fmt_records]
            fmt_resamples = [r["resample_ms"]["mean"] for r in fmt_records]
            fmt_rams = [r["ram_mb"]["mean"] for r in fmt_records]
            fmt_tps = [r["throughput_mb_s"] for r in fmt_records if r["throughput_mb_s"] > 0]

            by_format[fmt] = {
                "count": len(fmt_records),
                "total_size_mb": round(sum(r["size_mb"] for r in fmt_records), 2),
                "ttfp_ms": calc_stats(fmt_ttfsp),
                "decode_ms": calc_stats(fmt_decodes),
                "resample_ms": calc_stats(fmt_resamples),
                "ram_mb": calc_stats(fmt_rams),
                "throughput_mb_s": round(statistics.mean(fmt_tps), 1) if fmt_tps else 0.0,
                "slow_count": sum(1 for r in fmt_records if r["is_slow"]),
                "fallback_count": sum(1 for r in fmt_records if r["is_fallback"]),
                "error_count": sum(1 for r in fmt_records if r["status"] == "ERROR")
            }

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
            "folder": str(folder_path),
            "images_tested": len(image_results),
            "total_size_mb": round(sum(r["size_mb"] for r in image_results), 2),
            "overall_ttfp_ms": calc_stats(all_ttfp_means),
            "overall_decode_ms": calc_stats(all_decode_means),
            "overall_resample_ms": calc_stats(all_resample_means),
            "overall_ram_mb": calc_stats(all_ram_means),
            "by_format": by_format,
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

    primary_target = "current" if "current" in all_target_results else target_names[0]
    primary_res = all_target_results[primary_target]
    img_list = list(primary_res["images"])

    # 1. Format Breakdown Table
    print("\n" + "=" * 108)
    print(f"  {BOLD}{CYAN}IMAGE FORMAT FAMILY BREAKDOWN SUMMARY ({primary_target}){RESET}")
    print("=" * 108)
    print(f"{'Format':<12} | {'Count':<6} | {'Total Size':<11} | {'TTFP Mean':<11} | {'TTFP p50':<10} | {'TTFP p95':<10} | {'Decode':<10} | {'Throughput':<11} | {'Status':<8}")
    print("-" * 108)

    for fmt_name, f_data in sorted(primary_res["by_format"].items(), key=lambda x: x[1]["ttfp_ms"]["mean"]):
        f_cnt = f_data["count"]
        f_sz = f"{f_data['total_size_mb']:.1f} MB"
        f_ttfp = f"{f_data['ttfp_ms']['mean']:.2f} ms"
        f_p50 = f"{f_data['ttfp_ms']['median']:.2f} ms"
        f_p95 = f"{f_data['ttfp_ms']['p95']:.2f} ms"
        f_dec = f"{f_data['decode_ms']['mean']:.2f} ms"
        f_tp = f"{f_data['throughput_mb_s']:.1f} MB/s"

        st = "OK"
        if f_data["error_count"] > 0:
            st_col = f"{RED}ERR ({f_data['error_count']}){RESET}"
        elif f_data["fallback_count"] > 0:
            st_col = f"{YELLOW}FALLBACK{RESET}"
        else:
            st_col = f"{GREEN}OK{RESET}"

        print(f"{fmt_name:<12} | {f_cnt:>6} | {f_sz:>11} | {f_ttfp:>11} | {f_p50:>10} | {f_p95:>10} | {f_dec:>10} | {f_tp:>11} | {st_col:<8}")

    print("=" * 108)

    # 2. Sorting options for image list
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
    elif args.sort_by == "format":
        img_list.sort(key=lambda x: (x["format"], -x["ttfp_ms"]["mean"]))
    elif args.sort_by == "name":
        img_list.sort(key=lambda x: x["filename"].lower())

    top_display = img_list if args.top_n <= 0 else img_list[:args.top_n]

    # 3. Print Top Worst Performers
    print("\n" + "=" * 115)
    print(f"  {BOLD}{CYAN}RANKED WORST-PERFORMING IMAGES (Sorted by {args.sort_by.upper()}){RESET}")
    print(f"  Showing top {len(top_display)} of {len(img_list)} evaluated images")
    print("=" * 115)
    print(f"{'Rank':<5} | {'Format':<10} | {'Filename':<28} | {'Size':<9} | {'TTFP Mean':<11} | {'TTFP Min':<10} | {'Decode':<10} | {'Throughput':<11} | {'Status':<10}")
    print("-" * 115)

    for rank, item in enumerate(top_display, 1):
        fmt = item["format"]
        fn = item["filename"] if len(item["filename"]) <= 28 else (item["filename"][:25] + "...")
        sz = f"{item['size_mb']:.2f} MB"
        ttfp_m = f"{item['ttfp_ms']['mean']:.2f} ms"
        ttfp_min = f"{item['ttfp_ms']['min']:.2f} ms"
        dec = f"{item['decode_ms']['mean']:.2f} ms"
        tp = f"{item['throughput_mb_s']:.1f} MB/s"

        st = item["status"]
        if st == "FALLBACK" or st == "ERROR":
            st_colored = f"{RED}{st}{RESET}"
        elif st == "SLOW":
            st_colored = f"{YELLOW}{st}{RESET}"
        else:
            st_colored = f"{GREEN}{st}{RESET}"

        print(f"{rank:<5} | {fmt:<10} | {fn:<28} | {sz:>9} | {ttfp_m:>11} | {ttfp_min:>10} | {dec:>10} | {tp:>11} | {st_colored:<10}")

    print("=" * 115)

    # 4. Watchlist / Fallbacks
    slow_or_fallback = [x for x in img_list if x["is_fallback"] or x["is_slow"] or x["status"] == "ERROR"]
    if slow_or_fallback:
        print(f"\n{BOLD}{YELLOW}[!] Attention: {len(slow_or_fallback)} Image(s) Exceeded {args.threshold_ms:.0f}ms SLA or Triggered Fallbacks:{RESET}")
        for item in slow_or_fallback[:15]:
            print(f"    - [{item['format']}] {item['filename']} ({item['size_mb']:.2f} MB): TTFP={item['ttfp_ms']['mean']:.2f}ms (Decode={item['decode_ms']['mean']:.2f}ms, Status={item['status']})")
        if len(slow_or_fallback) > 15:
            print(f"      ... and {len(slow_or_fallback) - 15} more (see reports for full list)")

    # 5. Overall Summary
    ov = primary_res["overall_ttfp_ms"]
    cnts = primary_res["counts"]

    print("\n" + "=" * 80)
    print(f"  {BOLD}OVERALL SUMMARY ({folder_path.name}){RESET}")
    print("=" * 80)
    print(f"  Total Images Evaluated : {len(img_list)} files ({primary_res['total_size_mb']:.2f} MB)")
    print(f"  Total Benchmark Time   : {total_suite_elapsed:.2f} seconds")
    print(f"  TTFP Mean              : {BOLD}{ov['mean']:.2f} ms{RESET}")
    print(f"  TTFP Median (p50)      : {ov['median']:.2f} ms")
    print(f"  TTFP 95th Percentile   : {ov['p95']:.2f} ms")
    print(f"  TTFP 99th Percentile   : {ov['p99']:.2f} ms")
    print(f"  Fastest Load           : {ov['min']:.2f} ms")
    print(f"  Slowest Load           : {ov['max']:.2f} ms")
    print("-" * 80)
    print(f"  Sub-100ms Pass Rate    : {BOLD}{cnts['sub100ms_pct']:.1f}%{RESET} ({cnts['sub100ms']}/{len(img_list)} images)")
    print(f"  Slow (> {args.threshold_ms:.0f}ms) Rate     : {cnts['slow_pct']:.1f}% ({cnts['slow_over_threshold']}/{len(img_list)} images)")
    print("=" * 80)

    # Save JSON results
    out_json = Path(args.save_json)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    with open(out_json, "w", encoding="utf-8") as f:
        json.dump(all_target_results, f, indent=2)
    print(f"\n[+] Saved JSON results to: {out_json}")

    # Save CSV results
    out_csv = Path(args.save_csv)
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    with open(out_csv, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([
            "target", "index", "format", "extension", "filename", "size_mb", "size_bytes",
            "ttfp_mean_ms", "ttfp_median_ms", "ttfp_min_ms", "ttfp_max_ms", "ttfp_stddev_ms",
            "decode_mean_ms", "resample_mean_ms", "ram_peak_mb", "throughput_mb_s",
            "status", "is_fallback", "is_slow", "path"
        ])
        for t_name, t_data in all_target_results.items():
            for r in t_data.get("images", []):
                writer.writerow([
                    t_name, r["index"], r["format"], r["extension"], r["filename"], r["size_mb"], r["size_bytes"],
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
    by_fmt = primary.get("by_format", {})

    md = f"""# JPEGView Downloads & Mixed-Format Folder Benchmark Report

- **Date:** {time.strftime('%Y-%m-%d %H:%M:%S')}
- **Folder:** `{primary['folder']}`
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
| **Slow (> {args.threshold_ms:.0f}ms) Rate** | **{cnts['slow_pct']:.1f}%** ({cnts['slow_over_threshold']}/{primary['images_tested']}) |

---

## Format Family Breakdown

| Format | Count | Total Size | TTFP Mean | TTFP Median | TTFP p95 | Decode Mean | Throughput | Errors |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
"""
    for fmt_name, f_data in sorted(by_fmt.items(), key=lambda x: x[1]["ttfp_ms"]["mean"]):
        md += f"| **{fmt_name}** | {f_data['count']} | {f_data['total_size_mb']:.1f} MB | **{f_data['ttfp_ms']['mean']:.2f} ms** | {f_data['ttfp_ms']['median']:.2f} ms | {f_data['ttfp_ms']['p95']:.2f} ms | {f_data['decode_ms']['mean']:.2f} ms | {f_data['throughput_mb_s']:.1f} MB/s | {f_data['error_count']} |\n"

    md += f"""
---

## Worst-Performing Images (Sorted by {args.sort_by})

| Rank | Format | Filename | Size (MB) | TTFP Mean (ms) | TTFP Min (ms) | Decode (ms) | Throughput (MB/s) | Status |
| :---: | :---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
"""
    for rank, r in enumerate(top_display, 1):
        md += f"| {rank} | `{r['format']}` | `{r['filename']}` | {r['size_mb']:.2f} | **{r['ttfp_ms']['mean']:.2f}** | {r['ttfp_ms']['min']:.2f} | {r['decode_ms']['mean']:.2f} | {r['throughput_mb_s']:.1f} | `{r['status']}` |\n"

    return md


if __name__ == "__main__":
    main()
