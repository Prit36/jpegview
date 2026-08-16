"""
Target Performance Comparator and Regression Analyzer.
Computes multi-target metric deltas, percentage speedups/regressions, and regression alerts.
Modern Python 3.12+ implementation.
"""

from __future__ import annotations

from dataclasses import dataclass, field, asdict
from typing import Any


@dataclass(slots=True)
class TargetComparisonResult:
    baseline_target: str
    targets: list[str]
    comparisons: dict[str, Any] = field(default_factory=dict)
    regression_alerts: list[dict[str, Any]] = field(default_factory=list)
    speedup_highlights: list[dict[str, Any]] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class PerformanceComparator:
    """Compares benchmark metrics across multiple targets."""

    def __init__(self, thresholds: dict[str, float] | None = None) -> None:
        self.thresholds = thresholds or {
            "significant_regression_pct": 5.0,
            "warning_regression_pct": 2.5,
            "significant_speedup_pct": 5.0
        }

    def compare_targets(
        self,
        target_results: dict[str, dict[str, Any]],
        baseline_target: str = "original-fork"
    ) -> dict[str, Any]:
        """
        Performs systematic comparative analysis against the baseline target.
        """
        targets = list(target_results.keys())
        if baseline_target not in target_results and targets:
            baseline_target = targets[0]

        result = TargetComparisonResult(
            baseline_target=baseline_target,
            targets=targets
        )

        baseline_data = target_results.get(baseline_target, {})

        for target in targets:
            if target == baseline_target:
                continue

            current_data = target_results[target]
            target_cmp = self._compare_single_target(baseline_data, current_data, baseline_target, target)
            result.comparisons[target] = target_cmp

            for alert in target_cmp.get("regressions", []):
                result.regression_alerts.append({
                    "target": target,
                    **alert
                })
            for highlight in target_cmp.get("speedups", []):
                result.speedup_highlights.append({
                    "target": target,
                    **highlight
                })

        return result.to_dict()

    def _compare_single_target(
        self,
        base: dict[str, Any],
        curr: dict[str, Any],
        base_name: str,
        curr_name: str
    ) -> dict[str, Any]:
        """Compares a specific target against the baseline."""
        diffs: dict[str, Any] = {
            "metrics": {},
            "regressions": [],
            "speedups": []
        }

        base_e2e = base.get("e2e", {})
        curr_e2e = curr.get("e2e", {})

        # 1. Large image load TTFP
        base_ttfp = base_e2e.get("large_image_load", {}).get("ttfp_ms", {}).get("median", 0.0)
        curr_ttfp = curr_e2e.get("large_image_load", {}).get("ttfp_ms", {}).get("median", 0.0)
        if base_ttfp > 0 and curr_ttfp > 0:
            delta_pct = ((curr_ttfp - base_ttfp) / base_ttfp) * 100.0
            status = "FASTER" if delta_pct < -self.thresholds["warning_regression_pct"] else \
                     ("SLOWER" if delta_pct > self.thresholds["warning_regression_pct"] else "EQUAL")

            metric_entry = {
                "metric": "Large Image TTFP (Time to First Paint)",
                "unit": "ms",
                "baseline_val": base_ttfp,
                "current_val": curr_ttfp,
                "delta_pct": round(delta_pct, 2),
                "speedup_ratio": round(base_ttfp / curr_ttfp, 2) if curr_ttfp > 0 else 1.0,
                "status": status,
                "lower_is_better": True
            }
            diffs["metrics"]["large_image_ttfp"] = metric_entry
            if status == "SLOWER":
                diffs["regressions"].append(metric_entry)
            elif status == "FASTER":
                diffs["speedups"].append(metric_entry)

        # 2. Large image memory
        base_ram = base_e2e.get("large_image_load", {}).get("peak_working_set_mb", {}).get("mean", 0.0)
        curr_ram = curr_e2e.get("large_image_load", {}).get("peak_working_set_mb", {}).get("mean", 0.0)
        if base_ram > 0 and curr_ram > 0:
            delta_pct = ((curr_ram - base_ram) / base_ram) * 100.0
            diffs["metrics"]["peak_working_set_mb"] = {
                "metric": "Peak Working Set RAM",
                "unit": "MB",
                "baseline_val": base_ram,
                "current_val": curr_ram,
                "delta_pct": round(delta_pct, 2),
                "lower_is_better": True
            }

        # 3. Folder Navigation FPS
        base_fps = base_e2e.get("folder_navigation", {}).get("fps", {}).get("median", 0.0)
        curr_fps = curr_e2e.get("folder_navigation", {}).get("fps", {}).get("median", 0.0)
        if base_fps > 0 and curr_fps > 0:
            delta_pct = ((curr_fps - base_fps) / base_fps) * 100.0
            status = "FASTER" if delta_pct > self.thresholds["warning_regression_pct"] else \
                     ("SLOWER" if delta_pct < -self.thresholds["warning_regression_pct"] else "EQUAL")

            metric_entry = {
                "metric": "Folder Navigation Throughput",
                "unit": "FPS",
                "baseline_val": base_fps,
                "current_val": curr_fps,
                "delta_pct": round(delta_pct, 2),
                "speedup_ratio": round(curr_fps / base_fps, 2) if base_fps > 0 else 1.0,
                "status": status,
                "lower_is_better": False
            }
            diffs["metrics"]["folder_fps"] = metric_entry
            if status == "SLOWER":
                diffs["regressions"].append(metric_entry)
            elif status == "FASTER":
                diffs["speedups"].append(metric_entry)

        # 4. Folder Navigation Frame Latency
        base_frame = base_e2e.get("folder_navigation", {}).get("avg_frame_ms", {}).get("median", 0.0)
        curr_frame = curr_e2e.get("folder_navigation", {}).get("avg_frame_ms", {}).get("median", 0.0)
        if base_frame > 0 and curr_frame > 0:
            delta_pct = ((curr_frame - base_frame) / base_frame) * 100.0
            diffs["metrics"]["folder_avg_frame_ms"] = {
                "metric": "Avg Switch Latency Per Image",
                "unit": "ms",
                "baseline_val": base_frame,
                "current_val": curr_frame,
                "delta_pct": round(delta_pct, 2),
                "lower_is_better": True
            }

        return diffs
