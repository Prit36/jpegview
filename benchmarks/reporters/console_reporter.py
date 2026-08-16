"""
Console Reporter with ANSI-Colored Comparison Tables.
"""

from typing import Dict, Any, List


class ConsoleReporter:
    """Renders benchmark comparisons and results directly to terminal."""

    # ANSI Colors
    RESET = "\033[0m"
    BOLD = "\033[1m"
    GREEN = "\033[32m"
    RED = "\033[31m"
    YELLOW = "\033[33m"
    CYAN = "\033[36m"
    MAGENTA = "\033[35m"

    def print_comparison_table(self, comparison: Dict[str, Any], all_results: Dict[str, Any]):
        """Prints formatted comparison table across all targets."""
        baseline = comparison.get("baseline_target", "original-fork")
        targets = comparison.get("targets", [])

        print("\n" + "=" * 90)
        print(f"{self.BOLD}{self.CYAN} JPEGView Benchmark Systematic Target Comparison{self.RESET}")
        print(f" Baseline: {self.BOLD}{baseline}{self.RESET} | Targets: {', '.join(targets)}")
        print("=" * 90)

        # 1. Large Image Loading Table
        print(f"\n{self.BOLD}[1] Large Image Stress Test (Time to First Paint & Memory){self.RESET}")
        header = f"{'Target':<24} | {'TTFP Mean':<12} | {'TTFP Min':<10} | {'Delta vs Base':<16} | {'Peak RAM':<10}"
        print("-" * len(header))
        print(header)
        print("-" * len(header))

        base_ttfp = all_results.get(baseline, {}).get("e2e", {}).get("large_image_load", {}).get("ttfp_ms", {}).get("mean", 0.0)

        for target in targets:
            data = all_results.get(target, {}).get("e2e", {}).get("large_image_load", {})
            ttfp_mean = data.get("ttfp_ms", {}).get("mean", 0.0)
            ttfp_min = data.get("ttfp_ms", {}).get("min", 0.0)
            peak_ram = data.get("peak_working_set_mb", {}).get("mean", 0.0)

            if target == baseline:
                delta_str = f"{self.BOLD}(Baseline){self.RESET}"
            elif base_ttfp > 0 and ttfp_mean > 0:
                delta_pct = ((ttfp_mean - base_ttfp) / base_ttfp) * 100.0
                if delta_pct < -3.0:
                    delta_str = f"{self.GREEN}{delta_pct:+.1f}% (FASTER){self.RESET}"
                elif delta_pct > 3.0:
                    delta_str = f"{self.RED}{delta_pct:+.1f}% (SLOWER){self.RESET}"
                else:
                    delta_str = f"{self.YELLOW}{delta_pct:+.1f}% (PARITY){self.RESET}"
            else:
                delta_str = "N/A"

            print(f"{target:<24} | {ttfp_mean:>8.2f} ms | {ttfp_min:>7.2f} ms | {delta_str:<25} | {peak_ram:>6.1f} MB")

        # 2. Folder Navigation Throughput Table
        print(f"\n{self.BOLD}[2] High-Speed Folder Navigation & Frame Pacing{self.RESET}")
        header2 = f"{'Target':<24} | {'Throughput':<12} | {'Avg Frame':<11} | {'P99 Latency':<12} | {'Delta FPS':<16}"
        print("-" * len(header2))
        print(header2)
        print("-" * len(header2))

        base_fps = all_results.get(baseline, {}).get("e2e", {}).get("folder_navigation", {}).get("fps", {}).get("mean", 0.0)

        for target in targets:
            data = all_results.get(target, {}).get("e2e", {}).get("folder_navigation", {})
            fps_mean = data.get("fps", {}).get("mean", 0.0)
            avg_frame = data.get("avg_frame_ms", {}).get("mean", 0.0)
            p99_frame = data.get("p99_frame_ms", {}).get("mean", 0.0)

            if target == baseline:
                delta_str = f"{self.BOLD}(Baseline){self.RESET}"
            elif base_fps > 0 and fps_mean > 0:
                delta_pct = ((fps_mean - base_fps) / base_fps) * 100.0
                if delta_pct > 3.0:
                    delta_str = f"{self.GREEN}{delta_pct:+.1f}% (FASTER){self.RESET}"
                elif delta_pct < -3.0:
                    delta_str = f"{self.RED}{delta_pct:+.1f}% (SLOWER){self.RESET}"
                else:
                    delta_str = f"{self.YELLOW}{delta_pct:+.1f}% (PARITY){self.RESET}"
            else:
                delta_str = "N/A"

            print(f"{target:<24} | {fps_mean:>7.1f} FPS | {avg_frame:>7.2f} ms | {p99_frame:>8.2f} ms | {delta_str:<25}")

        # 3. Regression Alerts & Highlights
        regressions = comparison.get("regression_alerts", [])
        speedups = comparison.get("speedup_highlights", [])

        if regressions:
            print(f"\n{self.BOLD}{self.RED}[!] Performance Regressions Detected:{self.RESET}")
            for reg in regressions:
                print(f"    - [{reg.get('target')}] {reg.get('metric')}: {reg.get('delta_pct'):+.1f}% {reg.get('status')}")
        else:
            print(f"\n{self.GREEN}[+] Zero performance regressions detected! All metrics within parity/faster.{self.RESET}")

        if speedups:
            print(f"\n{self.BOLD}{self.GREEN}[*] Speedup Highlights:{self.RESET}")
            for sp in speedups:
                print(f"    - [{sp.get('target')}] {sp.get('metric')}: {sp.get('delta_pct'):+.1f}% (Speedup: {sp.get('speedup_ratio')}x)")

        print("\n" + "=" * 90 + "\n")
