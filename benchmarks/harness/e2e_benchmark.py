"""
End-to-End Latency, Throughput, and Folder Navigation Benchmark Suites.
Collects sample arrays, warmups, and calculates mean, median, min, max, stddev, and percentiles.
Modern Python 3.12+ implementation.
"""

from __future__ import annotations

import statistics
from pathlib import Path
from typing import Any
from .process_runner import ProcessRunner


class E2EBenchmarkSuite:
    """Runs complete end-to-end user scenarios and aggregates statistical distributions."""

    def __init__(self) -> None:
        self.runner = ProcessRunner()

    def _calc_stats(self, samples: list[float]) -> dict[str, float]:
        if not samples:
            return {"mean": 0.0, "median": 0.0, "min": 0.0, "max": 0.0, "stddev": 0.0, "p95": 0.0, "p99": 0.0}

        sorted_s = sorted(samples)
        p95_idx = min(len(sorted_s) - 1, int(0.95 * len(sorted_s)))
        p99_idx = min(len(sorted_s) - 1, int(0.99 * len(sorted_s)))

        return {
            "mean": round(statistics.mean(samples), 3),
            "median": round(statistics.median(samples), 3),
            "min": round(min(samples), 3),
            "max": round(max(samples), 3),
            "stddev": round(statistics.stdev(samples), 3) if len(samples) > 1 else 0.0,
            "p95": round(sorted_s[p95_idx], 3),
            "p99": round(sorted_s[p99_idx], 3)
        }

    def run_suite(
        self,
        exe_path: Path,
        assets: dict[str, Path],
        warmup_iterations: int = 2,
        measure_iterations: int = 5,
        nav_steps: int = 20
    ) -> dict[str, Any]:
        """
        Executes all E2E test suites on the given executable.
        nav_steps: how many folder images to navigate per run (passed from --nav-steps CLI arg).
        """
        results: dict[str, Any] = {}

        # -------------------------------------------------------------
        # Test 1: Massive Image Stress Load (Time to First Paint)
        # -------------------------------------------------------------
        large_img = assets.get("large_image")
        if large_img and large_img.exists():
            print(f"[*] Running Large Image TTFP Benchmark on {large_img.name} ({warmup_iterations} warmups, {measure_iterations} runs)...")

            for _ in range(warmup_iterations):
                self.runner.run_image_load_benchmark(exe_path, large_img)

            ttfp_samples: list[float] = []
            load_samples: list[float] = []
            last_op_samples: list[float] = []
            usm_samples: list[float] = []
            peak_ram_samples: list[float] = []

            for _ in range(measure_iterations):
                sample = self.runner.run_image_load_benchmark(exe_path, large_img)
                ttfp_samples.append(sample["ttfp_ms"])
                load_samples.append(sample["load_ms"])
                last_op_samples.append(sample["last_op_ms"])
                usm_samples.append(sample["unsharp_mask_ms"])
                peak_ram_samples.append(sample["peak_working_set_mb"])

            results["large_image_load"] = {
                "asset": str(large_img.name),
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
        folder = assets.get("folder_dataset")
        if folder and folder.exists():
            file_count = len(list(folder.glob("*.*")))
            actual_steps = min(nav_steps, file_count)

            # Timeout: allow ~0.5s per nav step + 20s startup overhead.
            # This kills legacy binaries (that don't understand /benchmark_nav
            # and just sit open forever) within a predictable window instead of
            # hanging the entire benchmark run.
            nav_timeout = max(20.0, actual_steps * 0.5 + 20.0)
            print(f"[*] Running Folder Navigation Benchmark on '{folder.name}' ({actual_steps} steps, {measure_iterations} runs)...")

            for _ in range(warmup_iterations):
                self.runner.run_folder_navigation_benchmark(
                    exe_path, folder, nav_count=actual_steps, timeout_sec=nav_timeout)

            fps_samples: list[float] = []
            avg_frame_samples: list[float] = []
            p95_samples: list[float] = []
            p99_samples: list[float] = []
            jitter_samples: list[float] = []
            peak_ram_samples: list[float] = []

            for _ in range(measure_iterations):
                sample = self.runner.run_folder_navigation_benchmark(
                    exe_path, folder, nav_count=actual_steps, timeout_sec=nav_timeout)
                fps_samples.append(sample["fps"])
                avg_frame_samples.append(sample["avg_frame_ms"])
                p95_samples.append(sample["p95_frame_ms"])
                p99_samples.append(sample["p99_frame_ms"])
                jitter_samples.append(sample["frame_jitter_ms"])
                peak_ram_samples.append(sample.get("peak_working_set_mb", 0.0))

            results["folder_navigation"] = {
                "folder": str(folder.name),
                "nav_steps": actual_steps,
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
