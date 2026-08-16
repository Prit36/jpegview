"""
Process Execution, Windows HWND Automation, and High-Precision Telemetry Monitor.
Measures process lifecycle, Time-to-First-Paint (TTFP), peak working set RAM, and navigation pacing.
Compatible with modern JPEGView (/benchmark flag) and legacy binaries (via Win32 GUI automation).
"""

import os
import sys
import time
import json
import tempfile
import subprocess
from pathlib import Path
from typing import Dict, Any, List, Optional, Tuple

if sys.platform == "win32":
    import ctypes
    from ctypes import wintypes

    class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
            ("PrivateUsage", ctypes.c_size_t),
        ]


class ProcessRunner:
    """Manages spawning, precision profiling, and telemetry harvesting of JPEGView."""

    def __init__(self):
        self.is_win32 = (sys.platform == "win32")
        if self.is_win32:
            self.psapi = ctypes.WinDLL("psapi.dll")
            self.kernel32 = ctypes.WinDLL("kernel32.dll")
            self.user32 = ctypes.WinDLL("user32.dll")
            
            self._freq = ctypes.c_int64()
            self.kernel32.QueryPerformanceFrequency(ctypes.byref(self._freq))

    def _now_ms(self) -> float:
        if self.is_win32:
            t = ctypes.c_int64()
            self.kernel32.QueryPerformanceCounter(ctypes.byref(t))
            return (t.value * 1000.0) / self._freq.value
        return time.perf_counter() * 1000.0

    def _get_process_memory_mb(self, h_process) -> Tuple[float, float]:
        if not self.is_win32 or not h_process:
            return 0.0, 0.0
        counters = PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS_EX)
        if self.psapi.GetProcessMemoryInfo(
            h_process,
            ctypes.byref(counters),
            counters.cb
        ):
            working_set_mb = counters.WorkingSetSize / (1024.0 * 1024.0)
            peak_working_set_mb = counters.PeakWorkingSetSize / (1024.0 * 1024.0)
            return working_set_mb, peak_working_set_mb
        return 0.0, 0.0

    def _find_window_for_pid(self, pid: int) -> Optional[int]:
        if not self.is_win32:
            return None
        found_hwnds = []

        def enum_cb(hwnd, extra):
            if self.user32.IsWindowVisible(hwnd):
                lpdw_pid = wintypes.DWORD()
                self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(lpdw_pid))
                if lpdw_pid.value == pid:
                    found_hwnds.append(hwnd)
            return True

        WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
        self.user32.EnumWindows(WNDENUMPROC(enum_cb), 0)
        return found_hwnds[0] if found_hwnds else None

    def run_image_load_benchmark(
        self,
        exe_path: Path,
        image_path: Path,
        timeout_sec: float = 30.0
    ) -> Dict[str, Any]:
        """
        Runs Time to First Paint (TTFP) benchmark on an image file.
        Compatible with both modern (/benchmark) and legacy JPEGView binaries.
        """
        temp_json = Path(tempfile.gettempdir()) / f"telemetry_{os.getpid()}_{time.time_ns()}.json"

        cmd = [
            str(exe_path),
            f'"{str(image_path)}"',
            f'/benchmark:"{str(temp_json)}"',
            "/benchmark_exit",
            "/autoexit"
        ]

        cmd_str = " ".join(cmd)
        t_start = self._now_ms()

        proc = subprocess.Popen(
            cmd_str,
            cwd=str(exe_path.parent),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )

        peak_rss_mb = 0.0
        h_proc = None
        if self.is_win32:
            h_proc = self.kernel32.OpenProcess(0x0400 | 0x0010, False, proc.pid)

        t_first_paint = None
        window_handle = None

        try:
            # Wait for process initialization and first window / exit
            poll_interval = 0.002
            deadline = self._now_ms() + (timeout_sec * 1000.0)

            while self._now_ms() < deadline:
                if proc.poll() is not None:
                    # Process exited normally (e.g. /benchmark_exit)
                    if t_first_paint is None:
                        t_first_paint = self._now_ms()
                    break

                if h_proc:
                    _, cur_peak = self._get_process_memory_mb(h_proc)
                    if cur_peak > peak_rss_mb:
                        peak_rss_mb = cur_peak

                # Check if window is visible and responsive (for legacy binaries)
                if not window_handle:
                    window_handle = self._find_window_for_pid(proc.pid)
                    if window_handle:
                        t_first_paint = self._now_ms()
                        # Give legacy window a brief moment to finish paint, then send WM_CLOSE
                        time.sleep(0.05)
                        WM_CLOSE = 0x0010
                        self.user32.PostMessageW(window_handle, WM_CLOSE, 0, 0)

                time.sleep(poll_interval)

            if proc.poll() is None:
                # Force kill if still lingering
                proc.kill()
                proc.wait(timeout=2)

        finally:
            if h_proc:
                self.kernel32.CloseHandle(h_proc)

        t_end = self._now_ms()
        external_wall_time_ms = (t_first_paint or t_end) - t_start

        # Parse internal telemetry if available
        telemetry = {}
        if temp_json.exists():
            try:
                with open(temp_json, "r", encoding="utf-8") as f:
                    telemetry = json.load(f)
                temp_json.unlink(missing_ok=True)
            except Exception:
                pass

        ttfp_ms = telemetry.get("process_start_to_first_paint_ms", external_wall_time_ms)
        load_ms = telemetry.get("first_image_load_ms", 0.0)
        last_op_ms = telemetry.get("first_image_last_op_ms", 0.0)
        usm_ms = telemetry.get("first_image_unsharp_mask_ms", 0.0)
        reported_ram = telemetry.get("peak_working_set_mb", peak_rss_mb)

        return {
            "ttfp_ms": ttfp_ms,
            "load_ms": load_ms,
            "last_op_ms": last_op_ms,
            "unsharp_mask_ms": usm_ms,
            "peak_working_set_mb": max(reported_ram, peak_rss_mb),
            "exit_code": proc.returncode,
            "has_internal_telemetry": bool(telemetry)
        }

    def run_folder_navigation_benchmark(
        self,
        exe_path: Path,
        folder_or_image_path: Path,
        nav_count: int = 50,
        nav_steps: Optional[int] = None,
        timeout_sec: float = 30.0
    ) -> Dict[str, Any]:
        """
        Runs folder navigation benchmark traversing `nav_steps` files.
        Works via internal /benchmark_nav or simulated high-speed VK_RIGHT key messages.
        """
        steps = nav_steps if nav_steps is not None else nav_count
        first_image_path = folder_or_image_path
        if folder_or_image_path.is_dir():
            candidates = sorted(list(folder_or_image_path.glob("*.*")))
            if candidates:
                first_image_path = candidates[0]

        temp_json = Path(tempfile.gettempdir()) / f"telemetry_nav_{os.getpid()}_{time.time_ns()}.json"

        cmd = [
            str(exe_path),
            f'"{str(first_image_path)}"',
            f'/benchmark:"{str(temp_json)}"',
            f'/benchmark_nav:{steps}',
            "/benchmark_exit"
        ]

        cmd_str = " ".join(cmd)
        t_start = self._now_ms()

        proc = subprocess.Popen(
            cmd_str,
            cwd=str(exe_path.parent),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )

        h_proc = None
        if self.is_win32:
            h_proc = self.kernel32.OpenProcess(0x0400 | 0x0010, False, proc.pid)

        window_handle = None
        deadline = self._now_ms() + (timeout_sec * 1000.0)

        peak_rss_mb = 0.0
        try:
            while self._now_ms() < deadline:
                if proc.poll() is not None:
                    break

                if h_proc:
                    _, cur_peak = self._get_process_memory_mb(h_proc)
                    if cur_peak > peak_rss_mb:
                        peak_rss_mb = cur_peak

                if not window_handle and self.is_win32:
                    window_handle = self._find_window_for_pid(proc.pid)
                    if window_handle:
                        # Legacy fallback: send VK_RIGHT key events to navigate
                        WM_KEYDOWN = 0x0100
                        WM_KEYUP = 0x0101
                        VK_RIGHT = 0x27
                        WM_CLOSE = 0x0010

                        for _ in range(steps):
                            self.user32.PostMessageW(window_handle, WM_KEYDOWN, VK_RIGHT, 0)
                            self.user32.PostMessageW(window_handle, WM_KEYUP, VK_RIGHT, 0)
                            time.sleep(0.005)

                        time.sleep(0.1)
                        self.user32.PostMessageW(window_handle, WM_CLOSE, 0, 0)

                time.sleep(0.005)

            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=2)

        finally:
            if h_proc:
                self.kernel32.CloseHandle(h_proc)

        t_end = self._now_ms()
        total_duration_ms = t_end - t_start

        telemetry = {}
        if temp_json.exists():
            try:
                with open(temp_json, "r", encoding="utf-8") as f:
                    telemetry = json.load(f)
                temp_json.unlink(missing_ok=True)
            except Exception:
                pass

        if telemetry and "nav_render_times_ms" in telemetry:
            render_times = telemetry.get("nav_render_times_ms", [])
            avg_frame_ms = telemetry.get("avg_frame_ms", 1.0)
            fps = telemetry.get("fps", 0.0)
        else:
            steps_done = max(1, steps)
            avg_frame_ms = max(0.1, total_duration_ms / steps_done)
            fps = (1000.0 / avg_frame_ms) if avg_frame_ms > 0 else 0.0
            render_times = [avg_frame_ms] * steps_done

        # Compute jitter (standard deviation of frame intervals)
        if len(render_times) > 1:
            mean = sum(render_times) / len(render_times)
            variance = sum((x - mean) ** 2 for x in render_times) / (len(render_times) - 1)
            jitter_ms = variance ** 0.5
            sorted_times = sorted(render_times)
            p95_idx = min(len(sorted_times) - 1, int(0.95 * len(sorted_times)))
            p99_idx = min(len(sorted_times) - 1, int(0.99 * len(sorted_times)))
            p95_frame_ms = sorted_times[p95_idx]
            p99_frame_ms = sorted_times[p99_idx]
        else:
            jitter_ms = 0.0
            p95_frame_ms = avg_frame_ms
            p99_frame_ms = avg_frame_ms

        reported_ram = telemetry.get("peak_working_set_mb", peak_rss_mb)

        return {
            "fps": fps,
            "avg_frame_ms": avg_frame_ms,
            "p95_frame_ms": p95_frame_ms,
            "p99_frame_ms": p99_frame_ms,
            "frame_jitter_ms": jitter_ms,
            "peak_working_set_mb": max(reported_ram, peak_rss_mb),
            "frames_measured": len(render_times),
            "has_internal_telemetry": bool(telemetry)
        }


