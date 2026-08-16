"""
Markdown Report Generator for JPEGView Benchmarks.
Generates GitHub Flavored Markdown reports with summary tables, delta percentages, and alerts.
"""

from pathlib import Path
from typing import Dict, Any, List


class MarkdownReporter:
    """Generates Markdown benchmark summary documents."""

    def generate_report(
        self,
        comparison: Dict[str, Any],
        all_results: Dict[str, Any],
        system_info: Dict[str, Any],
        out_path: Path
    ) -> Path:
        baseline = comparison.get("baseline_target", "original-fork")
        targets = comparison.get("targets", [])

        lines = [
            "# JPEGView Systematic Performance Benchmark Report",
            "",
            "> Automated benchmark comparison across git targets to prevent performance regressions.",
            "",
            "## Environment & Hardware Telemetry",
            "",
            f"- **CPU**: `{system_info.get('cpu_model', 'Unknown')}` ({system_info.get('cpu_cores_logical', 0)} logical cores)",
            f"- **RAM**: `{system_info.get('ram_total_gb', 0)} GB Total` (`{system_info.get('ram_available_gb', 0)} GB Available`)",
            f"- **OS**: `{system_info.get('os_name', '')} {system_info.get('os_release', '')} (Build {system_info.get('os_version', '')})`",
            f"- **SIMD Support**: `AVX2: {'Yes' if system_info.get('avx2_supported') else 'No'}` | `AVX-512: {'Yes' if system_info.get('avx512_supported') else 'No'}`",
            "",
            "## Target Comparison Matrix",
            "",
            f"**Baseline**: `{baseline}` | **Comparison Targets**: `{', '.join(targets)}`",
            "",
            "### 1. Large Image Loading (Time to First Paint & Memory)",
            "",
            "| Target | TTFP (Mean) | TTFP (Min) | Load Time | Delta vs Base | Peak RAM |",
            "| :--- | :--- | :--- | :--- | :--- | :--- |"
        ]

        base_ttfp = all_results.get(baseline, {}).get("e2e", {}).get("large_image_load", {}).get("ttfp_ms", {}).get("mean", 0.0)

        for target in targets:
            data = all_results.get(target, {}).get("e2e", {}).get("large_image_load", {})
            ttfp_mean = data.get("ttfp_ms", {}).get("mean", 0.0)
            ttfp_min = data.get("ttfp_ms", {}).get("min", 0.0)
            load_mean = data.get("load_ms", {}).get("mean", 0.0)
            peak_ram = data.get("peak_working_set_mb", {}).get("mean", 0.0)

            if target == baseline:
                delta_str = "**Baseline**"
            elif base_ttfp > 0 and ttfp_mean > 0:
                delta_pct = ((ttfp_mean - base_ttfp) / base_ttfp) * 100.0
                if delta_pct < -3.0:
                    delta_str = f"🟢 **{delta_pct:+.1f}%** *(Faster)*"
                elif delta_pct > 3.0:
                    delta_str = f"🔴 **{delta_pct:+.1f}%** *(Slower)*"
                else:
                    delta_str = f"⚪ **{delta_pct:+.1f}%** *(Parity)*"
            else:
                delta_str = "N/A"

            lines.append(f"| **`{target}`** | {ttfp_mean:.2f} ms | {ttfp_min:.2f} ms | {load_mean:.2f} ms | {delta_str} | {peak_ram:.1f} MB |")

        lines.extend([
            "",
            "### 2. High-Speed Folder Navigation & Frame Pacing",
            "",
            "| Target | Throughput (FPS) | Avg Switch Latency | P99 Latency | Frame Jitter | Delta FPS |",
            "| :--- | :--- | :--- | :--- | :--- | :--- |"
        ])

        base_fps = all_results.get(baseline, {}).get("e2e", {}).get("folder_navigation", {}).get("fps", {}).get("mean", 0.0)

        for target in targets:
            data = all_results.get(target, {}).get("e2e", {}).get("folder_navigation", {})
            fps_mean = data.get("fps", {}).get("mean", 0.0)
            avg_frame = data.get("avg_frame_ms", {}).get("mean", 0.0)
            p99_frame = data.get("p99_frame_ms", {}).get("mean", 0.0)
            jitter = data.get("frame_jitter_ms", {}).get("mean", 0.0)

            if target == baseline:
                delta_str = "**Baseline**"
            elif base_fps > 0 and fps_mean > 0:
                delta_pct = ((fps_mean - base_fps) / base_fps) * 100.0
                if delta_pct > 3.0:
                    delta_str = f"🟢 **{delta_pct:+.1f}%** *(Faster)*"
                elif delta_pct < -3.0:
                    delta_str = f"🔴 **{delta_pct:+.1f}%** *(Slower)*"
                else:
                    delta_str = f"⚪ **{delta_pct:+.1f}%** *(Parity)*"
            else:
                delta_str = "N/A"

            lines.append(f"| **`{target}`** | {fps_mean:.1f} FPS | {avg_frame:.2f} ms | {p99_frame:.2f} ms | {jitter:.2f} ms | {delta_str} |")

        # Regressions & Highlights
        regressions = comparison.get("regression_alerts", [])
        speedups = comparison.get("speedup_highlights", [])

        lines.extend(["", "## Regression & Performance Assessment", ""])
        if regressions:
            lines.append("> [!WARNING]")
            lines.append("> **Potential Performance Regressions Detected:**")
            for reg in regressions:
                lines.append(f"> - `[{reg.get('target')}]` **{reg.get('metric')}**: `{reg.get('delta_pct'):+.1f}%` ({reg.get('status')})")
        else:
            lines.append("> [!NOTE]")
            lines.append("> **No performance regressions detected!** All evaluated operations performed within parity or faster than baseline.")

        if speedups:
            lines.append("")
            lines.append("> [!TIP]")
            lines.append("> **Speedup Highlights:**")
            for sp in speedups:
                lines.append(f"> - `[{sp.get('target')}]` **{sp.get('metric')}**: `{sp.get('delta_pct'):+.1f}%` (Speedup: `{sp.get('speedup_ratio')}x`)")

        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

        print(f"[+] Exported Markdown report to {out_path}")
        return out_path
