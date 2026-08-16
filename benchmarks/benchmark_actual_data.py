#!/usr/bin/env python3
"""
JPEGView Real-World Photo Benchmark Suite.
Systematically benchmarks Original Fork vs Last Commit vs Current changes against actual camera photos in benchmarks/actual_test_data.
Modern Python 3.14+ implementation.
"""

from __future__ import annotations

import sys
import json
import argparse
from pathlib import Path
from typing import Any

# Add benchmarks directory to python path
BENCHMARKS_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCHMARKS_DIR.parent
sys.path.insert(0, str(BENCHMARKS_DIR))

from harness.system_info import get_system_telemetry
from harness.git_manager import GitManager
from harness.builder import TargetBuilder
from harness.e2e_benchmark import E2EBenchmarkSuite
from harness.micro_benchmark import MicroBenchmarkRunner
from harness.comparator import PerformanceComparator
from reporters.console_reporter import ConsoleReporter
from reporters.markdown_reporter import MarkdownReporter
from reporters.json_reporter import JSONReporter
from reporters.html_reporter import HTMLReporter


def load_json(path: Path) -> dict[str, Any]:
    if path.exists():
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            return {}
    return {}


def main() -> None:
    parser = argparse.ArgumentParser(
        description="JPEGView Real-World Photo Benchmark Suite (benchmarks/actual_test_data)",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )

    parser.add_argument(
        "--targets",
        type=str,
        default="original-fork,last-commit,current",
        help="Comma-separated list of targets (default: original-fork,last-commit,current)"
    )
    parser.add_argument(
        "--baseline",
        type=str,
        default="original-fork",
        help="Target name to use as comparison baseline (default: original-fork)"
    )
    parser.add_argument(
        "--data-dir",
        type=str,
        default=str(BENCHMARKS_DIR / "actual_test_data"),
        help="Path to actual photos folder (default: benchmarks/actual_test_data)"
    )
    parser.add_argument(
        "--nav-steps",
        type=int,
        default=50,
        help="Number of photos to navigate through in folder benchmark (default: 50, 0 for all photos). "
             "WARNING: large values multiplied by iterations and warmups can take hours."
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
        help="Force recompilation of target binaries"
    )

    args = parser.parse_args()

    data_path = Path(args.data_dir)
    if not data_path.exists() or not data_path.is_dir():
        print(f"[-] Error: Actual test data directory not found at: {data_path}")
        sys.exit(1)

    all_photos = sorted(list({p for p in data_path.iterdir() if p.suffix.lower() in [".jpg", ".jpeg"]}))
    if not all_photos:
        print(f"[-] Error: No JPEG photographs found in: {data_path}")
        sys.exit(1)

    # Pick representative photos
    # 1. Largest photo in dataset
    largest_photo = max(all_photos, key=lambda p: p.stat().st_size)
    # 2. First photo
    first_photo = all_photos[0]

    nav_count = len(all_photos) if args.nav_steps <= 0 else min(args.nav_steps, len(all_photos))

    print("\n" + "=" * 90)
    print("  JPEGView Real-World Photo Performance Benchmark")
    print(f"  Dataset: {data_path.name} ({len(all_photos)} actual camera photos)")
    print(f"  Representative Stress Image: {largest_photo.name} ({largest_photo.stat().st_size / (1024*1024):.2f} MB)")
    print(f"  Folder Navigation Scope: {nav_count} photos / {args.iterations} runs")
    print("=" * 90)


    # Load configuration
    config = load_json(BENCHMARKS_DIR / "config" / "benchmark_config.json")

    # Initialize components
    git_mgr = GitManager(REPO_ROOT)
    builder = TargetBuilder(REPO_ROOT)
    e2e_suite = E2EBenchmarkSuite()
    micro_runner = MicroBenchmarkRunner(REPO_ROOT)
    comparator = PerformanceComparator(config.get("regression_thresholds"))

    console_rep = ConsoleReporter()
    md_rep = MarkdownReporter()
    json_rep = JSONReporter()
    html_rep = HTMLReporter()

    results_dir = BENCHMARKS_DIR / "results"
    results_dir.mkdir(parents=True, exist_ok=True)

    sys_info = get_system_telemetry()

    target_names: list[str] = [t.strip() for t in args.targets.split(",") if t.strip()]
    all_target_results: dict[str, dict[str, Any]] = {}

    # Estimate total runtime and warn if it's going to be very long
    # (~200ms per nav frame, ~450ms per image load, accounting for warmups)
    total_nav_runs = (args.warmups + args.iterations) * len(target_names)
    total_load_runs = (args.warmups + args.iterations) * len(target_names)
    est_secs = (nav_count * 0.22 * total_nav_runs) + (0.5 * total_load_runs)
    if est_secs > 300:
        print(f"  ⚠  Estimated runtime: ~{est_secs/60:.0f} minutes  "
              f"({nav_count} steps × {total_nav_runs} nav runs + {total_load_runs} load runs)")
        print(f"     Tip: use --nav-steps 20 --iterations 3 for a quick ~{(20*0.22*3+0.5*3)/60:.0f}-min run.")
        print("=" * 90)

    for t_name in target_names:
        print(f"\n[{t_name.upper()}] Preparing and evaluating target '{t_name}'...")
        git_ref, desc = git_mgr.resolve_target_ref(t_name, config)
        commit_hash = git_mgr.get_commit_hash(git_ref if git_ref != "WORKING_TREE" else "HEAD")
        print(f"    Git Ref: {git_ref} ({commit_hash[:8]}) - {desc}")

        # 1. Source checkout
        source_dir = git_mgr.prepare_target_source(t_name, git_ref)

        # 2. Build target
        exe_path = builder.build_target(
            t_name,
            source_dir,
            commit_hash,
            force_rebuild=args.force_rebuild
        )

        # 3. Run E2E benchmarks on actual photo dataset
        assets = {
            "large_image": largest_photo,
            "folder_dataset": data_path
        }

        e2e_res = e2e_suite.run_suite(
            exe_path,
            assets,
            warmup_iterations=args.warmups,
            measure_iterations=args.iterations,
            nav_steps=nav_count
        )

        all_target_results[t_name] = {
            "name": t_name,
            "git_ref": git_ref,
            "commit_hash": commit_hash,
            "description": desc,
            "dataset": "actual_test_data",
            "e2e": e2e_res
        }

    # Run Micro-Benchmarks on current engine
    print("\n[*] Executing native SIMD algorithm micro-benchmarks on active engine...")
    try:
        micro_res = micro_runner.run_micro_suite(iterations=args.iterations)
        if "current" in all_target_results:
            all_target_results["current"]["micro"] = micro_res
    except Exception as e:
        print(f"[-] Micro-benchmark warning: {e}")

    # Comparative analysis
    baseline_name = args.baseline if args.baseline in all_target_results else target_names[0]
    comparison = comparator.compare_targets(all_target_results, baseline_target=baseline_name)

    # Print results
    console_rep.print_comparison_table(comparison, all_target_results)

    # Export reports
    md_path = results_dir / "actual_data_benchmark_report.md"
    json_path = results_dir / "actual_data_benchmark_results.json"
    html_path = results_dir / "actual_data_benchmark_report.html"

    md_rep.generate_report(comparison, all_target_results, sys_info, md_path)
    json_rep.generate_report(comparison, all_target_results, sys_info, json_path)
    html_rep.generate_report(comparison, all_target_results, sys_info, html_path)

    print("\n[+] Real-world photo benchmark complete! Reports generated:")
    print(f"    - Markdown: {md_path}")
    print(f"    - HTML:     {html_path}")
    print(f"    - JSON:     {json_path}\n")


if __name__ == "__main__":
    main()
