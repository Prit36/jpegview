#!/usr/bin/env python3
"""
Dedicated standalone benchmark for opening RAW15538.JPG (17.13 MB camera photo) from actual_test_data.
Focuses strictly on measuring Time to First Paint (TTFP), decode time, resample time, and peak memory.
"""

from __future__ import annotations

import sys
import json
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


def load_json(path: Path) -> dict[str, Any]:
    if path.exists():
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            return {}
    return {}


def calc_stats(samples: list[float]) -> dict[str, float]:
    if not samples:
        return {"mean": 0.0, "median": 0.0, "min": 0.0, "max": 0.0, "stddev": 0.0, "p95": 0.0, "p99": 0.0}

    sorted_s = sorted(samples)
    p95_idx = min(len(sorted_s) - 1, int(0.95 * len(sorted_s)))
    p99_idx = min(len(sorted_s) - 1, int(0.99 * len(sorted_s)))

    return {
        "mean": round(statistics.mean(samples), 2),
        "median": round(statistics.median(samples), 2),
        "min": round(min(samples), 2),
        "max": round(max(samples), 2),
        "stddev": round(statistics.stdev(samples), 2) if len(samples) > 1 else 0.0,
        "p95": round(sorted_s[p95_idx], 2),
        "p99": round(sorted_s[p99_idx], 2)
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Dedicated Benchmark for RAW15538.JPG (17.13 MB) from actual_test_data",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )

    parser.add_argument(
        "--targets",
        type=str,
        default="original-fork,current",
        help="Comma-separated list of targets to benchmark (default: original-fork,current)"
    )
    parser.add_argument(
        "--baseline",
        type=str,
        default="original-fork",
        help="Target name to use as comparison baseline (default: original-fork)"
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=5,
        help="Number of measurement runs per target (default: 5)"
    )
    parser.add_argument(
        "--warmups",
        type=int,
        default=2,
        help="Number of warmup runs before recording metrics (default: 2)"
    )
    parser.add_argument(
        "--force-rebuild",
        action="store_true",
        help="Force clean recompilation of target binaries"
    )

    args = parser.parse_args()

    image_path = BENCHMARKS_DIR / "actual_test_data" / "RAW15538.JPG"
    if not image_path.exists():
        print(f"[-] Error: Image not found at {image_path}")
        sys.exit(1)

    file_size_mb = image_path.stat().st_size / (1024 * 1024)

    print("=" * 86)
    print("  JPEGView Dedicated Single-Photo Benchmark: RAW15538.JPG")
    print(f"  Photo: {image_path.name} ({file_size_mb:.2f} MB)")
    print(f"  Iterations: {args.iterations} runs ({args.warmups} warmups)")
    print("=" * 86)

    config = load_json(BENCHMARKS_DIR / "config" / "benchmark_config.json")
    git_mgr = GitManager(REPO_ROOT)
    builder = TargetBuilder(REPO_ROOT)
    runner = ProcessRunner()

    target_names = [t.strip() for t in args.targets.split(",") if t.strip()]
    target_results: dict[str, Any] = {}

    for name in target_names:
        print(f"\n[{name.upper()}] Preparing and evaluating target '{name}'...")

        git_ref, desc = git_mgr.resolve_target_ref(name, config)
        commit_hash = git_mgr.get_commit_hash(git_ref if git_ref != "WORKING_TREE" else "HEAD")
        print(f"    Git Ref: {git_ref} ({commit_hash[:8]}) - {desc}")

        source_dir = git_mgr.prepare_target_source(name, git_ref)
        exe_path = builder.build_target(name, source_dir, commit_hash, force_rebuild=args.force_rebuild)

        print(f"[*] Running TTFP measurements on {image_path.name}...")

        # Warmups
        for w in range(args.warmups):
            runner.run_image_load_benchmark(exe_path, image_path)

        ttfp_samples: list[float] = []
        load_samples: list[float] = []
        last_op_samples: list[float] = []
        peak_ram_samples: list[float] = []

        for i in range(args.iterations):
            sample = runner.run_image_load_benchmark(exe_path, image_path)
            ttfp_samples.append(sample["ttfp_ms"])
            load_samples.append(sample["load_ms"])
            last_op_samples.append(sample["last_op_ms"])
            peak_ram_samples.append(sample["peak_working_set_mb"])
            print(f"    Run {i+1}/{args.iterations}: TTFP = {sample['ttfp_ms']:.2f} ms (Decode = {sample['load_ms']:.2f} ms, Resample = {sample['last_op_ms']:.2f} ms, RAM = {sample['peak_working_set_mb']:.1f} MB)")

        target_results[name] = {
            "target": name,
            "git_ref": git_ref,
            "commit_hash": commit_hash,
            "exe_path": str(exe_path),
            "ttfp_ms": calc_stats(ttfp_samples),
            "load_ms": calc_stats(load_samples),
            "last_op_ms": calc_stats(last_op_samples),
            "peak_working_set_mb": calc_stats(peak_ram_samples),
            "raw_samples": {
                "ttfp_ms": ttfp_samples,
                "load_ms": load_samples,
                "last_op_ms": last_op_samples,
                "peak_working_set_mb": peak_ram_samples
            }
        }

    # Summary table
    print("\n" + "=" * 92)
    print("  RAW15538.JPG Benchmark Comparison Summary")
    print("=" * 92)
    print(f"{'Target':<18} | {'TTFP Mean':<11} | {'TTFP Min':<10} | {'Decode Mean':<12} | {'Resample Mean':<14} | {'Delta vs Base':<15}")
    print("-" * 92)

    base_ttfp = target_results[args.baseline]["ttfp_ms"]["mean"] if args.baseline in target_results else None

    for name, res in target_results.items():
        ttfp_mean = res["ttfp_ms"]["mean"]
        ttfp_min = res["ttfp_ms"]["min"]
        decode_mean = res["load_ms"]["mean"]
        resample_mean = res["last_op_ms"]["mean"]

        if name == args.baseline or base_ttfp is None or base_ttfp == 0:
            delta_str = "(Baseline)"
        else:
            diff_pct = ((ttfp_mean - base_ttfp) / base_ttfp) * 100.0
            if diff_pct < 0:
                delta_str = f"{diff_pct:+.1f}% (FASTER)"
            else:
                delta_str = f"{diff_pct:+.1f}% (SLOWER)"

        print(f"{name:<18} | {ttfp_mean:>8.2f} ms | {ttfp_min:>7.2f} ms | {decode_mean:>9.2f} ms | {resample_mean:>11.2f} ms | {delta_str:<15}")

    print("=" * 92)

    # Save results JSON
    results_dir = BENCHMARKS_DIR / "results"
    results_dir.mkdir(parents=True, exist_ok=True)
    out_json = results_dir / "raw15538_benchmark_results.json"
    with open(out_json, "w", encoding="utf-8") as f:
        json.dump(target_results, f, indent=2)
    print(f"\n[+] Saved results to: {out_json}")


if __name__ == "__main__":
    main()
