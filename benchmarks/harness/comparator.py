"""
Target Performance Comparator and Regression Analyzer.
Computes multi-target metric deltas, percentage speedups/regressions, and regression alerts.
"""

from typing import Dict, Any, List, Optional, Tuple


class PerformanceComparator:
    """Compares benchmark metrics across multiple targets."""

    def __init__(self, thresholds: Optional[Dict[str, float]] = None):
        self.thresholds = thresholds or {
            "significant_regression_pct": 5.0,
            "warning_regression_pct": 2.5,
            "significant_speedup_pct": 5.0
        }

    def compare_targets(
        self,
        target_results: Dict[str, Dict[str, Any]],
        baseline_target: str = "original-fork"
    ) -> Dict[str, Any]:
        """
        Performs systematic comparative analysis against the baseline target.
        """
        targets = list(target_results.keys())
        if baseline_target not in target_results and targets:
            baseline_target = targets[0]

        comparison: Dict[str, Any] = {
            "baseline_target": baseline_target,
            "targets": targets,
            "comparisons": {},
            "regression_alerts": [],
            "speedup_highlights": []
        }

        baseline_data = target_results.get(baseline_target, {})

        for target in targets:
            if target == baseline_target:
                continue

            current_data = target_results[target]
            target_cmp = self._compare_single_target(baseline_data, current_data, baseline_target, target)
            comparison["comparisons"][target] = target_cmp

            # Accumulate alerts
            for alert in target_cmp.get("regressions", []):
                comparison["regression_alerts"].append({
                    "target": target,
                    **alert
                })
            for highlight in target_cmp.get("speedups", []):
                comparison["speedup_highlights"].append({
                    "target": target,
                    **highlight
                })

        return comparison

    def _compare_single_target(
        self,
        base: Dict[str, Any],
        curr: Dict[str, Any],
        base_name: str,
        curr_name: str
    ) -> Dict[str, Any]:
        """Compares a specific target against the baseline."""
        diffs: Dict[str, Any] = {
            "metrics": {},
            "regressions": [],
            "speedups": []
        }

        # Compare E2E metrics
        base_e2e = base.get("e2e", {})
        curr_e2e = curr.get("e2e", {})

        # 1. Large image load TTFP
        base_ttfp = base_e2e.get("large_image_load", {}).get("ttfp_ms", {}).get("median", 0.0)
        curr_ttfp = curr_e2e.get("large_image_load", {}).get("ttfp_ms", {}).get("median", 0.0)
        if base_ttfp > 0 and curr_ttfp > 0:
            delta_pct = ((curr_ttfp - base_ttfp) / base_ttfp) * 100.0 # Negative is faster (good)
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
            delta_pct = ((curr_fps - base_fps) / base_fps) * 100.0 # Positive is faster (good)
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
