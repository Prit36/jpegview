"""
End-to-End Benchmark Suite Orchestrator.
Runs multi-iteration measurements of startup time, large image loading, and folder browsing.
"""

import math
import statistics
from pathlib import Path
from typing import Dict, Any, List, Optional

try:
    from .process_runner import ProcessRunner
except ImportError:
    from process_runner import ProcessRunner


class E2EBenchmarkSuite:
    """Orchestrates End-to-End benchmark runs across test assets."""

    def __init__(self):
        self.runner = ProcessRunner()

    def _calc_stats(self, values: List[float]) -> Dict[str, float]:
        if not values:
            return {"mean": 0.0, "median": 0.0, "min": 0.0, "max": 0.0, "stddev": 0.0}
        mean_val = statistics.mean(values)
        median_val = statistics.median(values)
        min_val = min(values)
        max_val = max(values)
        stddev_val = statistics.stdev(values) if len(values) > 1 else 0.0
        return {
            "mean": round(mean_val, 3),
            "median": round(median_val, 3),
            "min": round(min_val, 3),
            "max": round(max_val, 3),
            "stddev": round(stddev_val, 3)
        }

    def run_suite(
        self,
        exe_path: Path,
        assets: Dict[str, Path],
        warmup_iterations: int = 2,
        measure_iterations: int = 5
    ) -> Dict[str, Any]:
        """
        Executes all configured E2E benchmark tests against the target executable.
        """
        results: Dict[str, Any] = {}

        large_img = assets.get("large_image")
        folder = assets.get("folder")

        # -------------------------------------------------------------
        # Test 1: Cold & Warm Startup to First Paint (TTFP) on Large Image
        # -------------------------------------------------------------
        if large_img and large_img.exists():
            print(f"[*] Running Large Image TTFP Benchmark on {large_img.name} ({warmup_iterations} warmups, {measure_iterations} runs)...")

            # Warmups
            for _ in range(warmup_iterations):
                self.runner.run_image_load_benchmark(exe_path, large_img)

            # Measurements
            ttfp_samples = []
            load_samples = []
            last_op_samples = []
            usm_samples = []
            peak_ram_samples = []

            for i in range(measure_iterations):
                sample = self.runner.run_image_load_benchmark(exe_path, large_img)
                ttfp_samples.append(sample["ttfp_ms"])
                load_samples.append(sample["load_ms"])
                last_op_samples.append(sample["last_op_ms"])
                usm_samples.append(sample["unsharp_mask_ms"])
                peak_ram_samples.append(sample["peak_working_set_mb"])

            results["large_image_load"] = {
                "asset": str(large_img.name),
                "asset_size_mb": round(large_img.stat().st_size / (1024 * 1024), 2),
                "iterations": measure_iterations,
                "ttfp_ms": self._calc_stats(ttfp_samples),
                "load_ms": self._calc_stats(load_samples),
                "last_op_ms": self._calc_stats(last_op_samples),
                "unsharp_mask_ms": self._calc_stats(usm_samples),
                "peak_working_set_mb": self._calc_stats(peak_ram_samples),
                "raw_samples": {
                    "ttfp_ms": ttfp_samples,
                    "load_ms": load_samples,
                    "peak_working_set_mb": peak_ram_samples
                }
            }

        # -------------------------------------------------------------
        # Test 2: Rapid Folder Navigation Throughput & Frame Pacing
        # -------------------------------------------------------------
        if folder and folder.exists():
            file_count = len(list(folder.glob("*.*")))
            nav_steps = min(50, file_count)
            print(f"[*] Running Folder Navigation Benchmark on '{folder.name}' ({nav_steps} steps, {measure_iterations} runs)...")

            # Warmups
            for _ in range(warmup_iterations):
                self.runner.run_folder_navigation_benchmark(exe_path, folder, nav_count=nav_steps)

            # Measurements
            fps_samples = []
            avg_frame_samples = []
            p95_samples = []
            p99_samples = []
            jitter_samples = []
            peak_ram_samples = []

            for i in range(measure_iterations):
                sample = self.runner.run_folder_navigation_benchmark(exe_path, folder, nav_count=nav_steps)
                fps_samples.append(sample["fps"])
                avg_frame_samples.append(sample["avg_frame_ms"])
                p95_samples.append(sample["p95_frame_ms"])
                p99_samples.append(sample["p99_frame_ms"])
                jitter_samples.append(sample["frame_jitter_ms"])
                peak_ram_samples.append(sample["peak_working_set_mb"])

            results["folder_navigation"] = {
                "folder": str(folder.name),
                "nav_steps": nav_steps,
                "iterations": measure_iterations,
                "fps": self._calc_stats(fps_samples),
                "avg_frame_ms": self._calc_stats(avg_frame_samples),
                "p95_frame_ms": self._calc_stats(p95_samples),
                "p99_frame_ms": self._calc_stats(p99_samples),
                "frame_jitter_ms": self._calc_stats(jitter_samples),
                "peak_working_set_mb": self._calc_stats(peak_ram_samples),
                "raw_samples": {
                    "fps": fps_samples,
                    "avg_frame_ms": avg_frame_samples
                }
            }

        return results
