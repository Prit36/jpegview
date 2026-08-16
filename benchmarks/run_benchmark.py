#!/usr/bin/env python3
"""
JPEGView Systematic Performance Benchmark Suite
Unified CLI entry point for running, comparing, and profiling JPEGView across git targets.
Modern Python 3.12+ implementation.
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

from generators.asset_generator import AssetGenerator
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
        description="JPEGView Systematic Performance Benchmark Suite",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )

    subparsers = parser.add_subparsers(dest="command", help="Benchmark action to execute")

    # Command: compare
    p_compare = subparsers.add_parser("compare", help="Compare performance across multiple targets (Original Fork vs Last Commit vs Current)")
    p_compare.add_argument("--targets", type=str, default="original-fork,last-commit,current", help="Comma-separated list of targets")
    p_compare.add_argument("--baseline", type=str, default="original-fork", help="Target name to use as comparison baseline")
    p_compare.add_argument("--profile", type=str, default="standard", choices=["quick", "standard", "stress", "ci"], help="Benchmark intensity profile")
    p_compare.add_argument("--force-rebuild", action="store_true", help="Force recompilation of target binaries")

    # Command: run
    p_run = subparsers.add_parser("run", help="Run benchmark suite on a single target (default: current)")
    p_run.add_argument("--target", type=str, default="current", help="Target name to evaluate")
    p_run.add_argument("--profile", type=str, default="standard", choices=["quick", "standard", "stress", "ci"], help="Benchmark intensity profile")

    # Command: micro
    p_micro = subparsers.add_parser("micro", help="Run native C++ SIMD algorithm micro-benchmarks")
    p_micro.add_argument("--iterations", type=int, default=10, help="Number of measurement iterations per kernel")

    # Command: generate-assets
    p_assets = subparsers.add_parser("generate-assets", help="Generate synthetic large images and test folders")
    p_assets.add_argument("--profile", type=str, default="standard", choices=["quick", "standard", "stress", "ci"], help="Profile assets to generate")
    p_assets.add_argument("--force", action="store_true", help="Overwrite existing assets")

    args = parser.parse_args()

    # Default to compare if no command provided
    if not args.command:
        args.command = "compare"
        args.targets = "original-fork,last-commit,current"
        args.baseline = "original-fork"
        args.profile = "standard"
        args.force_rebuild = False

    # Load configuration
    config = load_json(BENCHMARKS_DIR / "config" / "benchmark_config.json")
    profiles = load_json(BENCHMARKS_DIR / "config" / "test_profiles.json")
    profile_name = getattr(args, "profile", "standard")
    profile_cfg = profiles.get(profile_name, profiles.get("standard", {}))

    # Initialize components
    asset_gen = AssetGenerator(BENCHMARKS_DIR / "assets")
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

    print("\n" + "=" * 80)
    print("  JPEGView Systematic Performance Benchmark Suite")
    print("=" * 80)
    print(f"[*] Profile: {profile_name.upper()} ({profile_cfg.get('description', '')})")

    # Telemetry
    sys_info = get_system_telemetry()

    match args.command:
        case "generate-assets":
            asset_gen.ensure_profile_assets(profile_cfg)
            print("[+] Test assets generated successfully in benchmarks/assets/")
            return

        case "micro":
            micro_runner.run_micro_suite(iterations=args.iterations)
            return

        case "compare" | "run" | _:
            pass

    # Ensure test assets
    assets = asset_gen.ensure_profile_assets(profile_cfg)

    # Benchmark Execution (compare or run)
    target_names: list[str] = [t.strip() for t in (args.targets.split(",") if hasattr(args, "targets") else [args.target]) if t.strip()]
    all_target_results: dict[str, dict[str, Any]] = {}

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
            force_rebuild=getattr(args, "force_rebuild", False)
        )

        # 3. Run E2E benchmarks
        e2e_res = e2e_suite.run_suite(
            exe_path,
            assets,
            warmup_iterations=profile_cfg.get("warmup_iterations", 2),
            measure_iterations=profile_cfg.get("measure_iterations", 5)
        )

        all_target_results[t_name] = {
            "name": t_name,
            "git_ref": git_ref,
            "commit_hash": commit_hash,
            "description": desc,
            "e2e": e2e_res
        }

    # Run Micro-Benchmarks on current engine
    print("\n[*] Executing native SIMD algorithm micro-benchmarks on active engine...")
    try:
        micro_res = micro_runner.run_micro_suite(iterations=profile_cfg.get("micro_iterations", 5))
        if "current" in all_target_results:
            all_target_results["current"]["micro"] = micro_res
    except Exception as e:
        print(f"[-] Micro-benchmark warning: {e}")

    # Comparative analysis
    baseline_name = getattr(args, "baseline", target_names[0])
    comparison = comparator.compare_targets(all_target_results, baseline_target=baseline_name)

    # Generate reports
    console_rep.print_comparison_table(comparison, all_target_results)
    
    md_path = results_dir / "benchmark_report.md"
    json_path = results_dir / "benchmark_results.json"
    html_path = results_dir / "benchmark_report.html"

    md_rep.generate_report(comparison, all_target_results, sys_info, md_path)
    json_rep.generate_report(comparison, all_target_results, sys_info, json_path)
    html_rep.generate_report(comparison, all_target_results, sys_info, html_path)

    print("\n[+] Benchmark complete! Reports generated:")
    print(f"    - Markdown: {md_path}")
    print(f"    - HTML:     {html_path}")
    print(f"    - JSON:     {json_path}\n")


if __name__ == "__main__":
    main()
