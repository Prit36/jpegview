"""
Process Execution and High-Precision Telemetry Monitor.
Measures process lifecycle, Time-to-First-Paint (TTFP), peak working set RAM, and navigation pacing.
Modern binaries self-report via /benchmark telemetry. See benchmarks/BENCHMARKING.md.
"""

from __future__ import annotations

import os
import sys
import time
import json
import tempfile
import subprocess
from pathlib import Path
from dataclasses import dataclass, asdict
from typing import Any

if sys.platform == "win32":
    import ctypes
    import winreg
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


def check_ifeo_page_heap(image_name: str = "JPEGView.exe") -> None:
    """
    Abort if Full Page Heap is enabled for the measured executable via IFEO.

    gflags-style page heap makes every heap allocation of the target process
    brutally slow (1.5-2.5x total slowdown) and PERSISTS ACROSS REBOOTS in
    HKLM registry state. It once poisoned an entire optimization session
    because 'gflags /p /enable' reported an elevation error while still
    writing the key. See benchmarks/BENCHMARKING.md section 1.

    Set JPEGVIEW_ALLOW_PAGEHEAP=1 to bypass this guard deliberately.
    """
    if os.environ.get("JPEGVIEW_ALLOW_PAGEHEAP") == "1" or sys.platform != "win32":
        return
    try:
        key_path = (
            r"SOFTWARE\Microsoft\Windows NT\CurrentVersion"
            r"\Image File Execution Options\\" + image_name
        )
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_path) as key:
            global_flag = 0
            page_heap_flags = 0
            try:
                global_flag = int(winreg.QueryValueEx(key, "GlobalFlag")[0], 16)
            except OSError:
                pass
            try:
                page_heap_flags = int(winreg.QueryValueEx(key, "PageHeapFlags")[0])
            except OSError:
                pass
            page_heap_active = bool(global_flag & 0x02000000) or page_heap_flags != 0
            if page_heap_active:
                raise RuntimeError(
                    f"\n{'=' * 78}\n"
                    f"REFUSING TO BENCHMARK: Full Page Heap is ENABLED for "
                    f"'{image_name}' (IFEO GlobalFlag&0x02000000 / PageHeapFlags="
                    f"{page_heap_flags}).\n"
                    f"This slows every measurement of that executable by 1.5-2.5x\n"
                    f"and survives reboots. Fix in an ELEVATED shell:\n\n"
                    f'  reg delete "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion'
                    f'\\Image File Execution Options\\{image_name}" /f\n\n'
                    f"(deliberately override with JPEGVIEW_ALLOW_PAGEHEAP=1)\n"
                    f"{'=' * 78}\n"
                )
    except FileNotFoundError:
        pass  # no IFEO entry at all - the good case


@dataclass(slots=True, frozen=True)
class ImageLoadMetric:
    ttfp_ms: float
    load_ms: float
    last_op_ms: float
    unsharp_mask_ms: float
    peak_working_set_mb: float
    exit_code: int | None
    has_internal_telemetry: bool

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(slots=True, frozen=True)
class FolderNavMetric:
    fps: float
    avg_frame_ms: float
    p95_frame_ms: float
    p99_frame_ms: float
    frame_jitter_ms: float
    peak_working_set_mb: float
    frames_measured: int
    has_internal_telemetry: bool

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class ProcessRunner:
    """Manages spawning, precision profiling, and telemetry harvesting of JPEGView."""

    def __init__(self) -> None:
        self.is_win32 = (sys.platform == "win32")
        check_ifeo_page_heap("JPEGView.exe")
        if self.is_win32:
            self.psapi = ctypes.WinDLL("psapi.dll")
            self.kernel32 = ctypes.WinDLL("kernel32.dll")

            self._freq = ctypes.c_int64()
            self.kernel32.QueryPerformanceFrequency(ctypes.byref(self._freq))

    def _now_ms(self) -> float:
        if self.is_win32:
            t = ctypes.c_int64()
            self.kernel32.QueryPerformanceCounter(ctypes.byref(t))
            return (t.value * 1000.0) / self._freq.value
        return time.perf_counter() * 1000.0

    def _get_process_memory_mb(self, h_process: Any) -> tuple[float, float]:
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

    def run_image_load_benchmark(
        self,
        exe_path: Path,
        image_path: Path,
        timeout_sec: float = 30.0
    ) -> dict[str, Any]:
        """
        Runs Time to First Paint (TTFP) benchmark on an image file.
        Modern JPEGView binaries self-report precise internal telemetry via the
        /benchmark flag and exit themselves (/benchmark_exit), so the harness
        stays completely passive during the measured window: no window
        enumeration, no periodic memory queries (GetProcessMemoryInfo takes the
        child's address-space lock and measurably stalls its decode threads).
        Peak RSS is read once after exit (it is a cumulative high-water mark).
        """
        temp_json = Path(tempfile.gettempdir()) / f"telemetry_{os.getpid()}_{time.time_ns()}.json"

        # Use list form so Python handles quoting — avoids the double-quote bug
        # that caused /benchmark:"path" to be misread by JPEGView, silently
        # dropping the telemetry file and returning zeros for all load timings.
        cmd = [
            str(exe_path),
            str(image_path),
            f"/benchmark:{temp_json}",
            "/benchmark_exit",
            "/autoexit"
        ]

        t_start = self._now_ms()

        proc = subprocess.Popen(
            cmd,
            cwd=str(exe_path.parent),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )

        peak_rss_mb = 0.0
        h_proc = None
        if self.is_win32:
            h_proc = self.kernel32.OpenProcess(0x0400 | 0x0010, False, proc.pid)

        try:
            # Passive wait: the child exits by itself after writing telemetry.
            while proc.poll() is None:
                if self._now_ms() > t_start + timeout_sec * 1000.0:
                    break
                time.sleep(0.005)

            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=2)
        finally:
            # Single post-exit query: PeakWorkingSetSize is a high-water mark,
            # so this captures the true peak with zero in-flight overhead.
            if h_proc:
                _, peak_after = self._get_process_memory_mb(h_proc)
                if peak_after > peak_rss_mb:
                    peak_rss_mb = peak_after
                self.kernel32.CloseHandle(h_proc)

        t_end = self._now_ms()
        external_wall_time_ms = t_end - t_start

        telemetry: dict[str, Any] = {}
        if temp_json.exists():
            try:
                telemetry = json.loads(temp_json.read_text(encoding="utf-8"))
                temp_json.unlink(missing_ok=True)
            except Exception:
                pass

        ttfp_ms = telemetry.get("process_start_to_first_paint_ms", external_wall_time_ms)
        load_ms = telemetry.get("first_image_load_ms", 0.0)
        last_op_ms = telemetry.get("first_image_last_op_ms", 0.0)
        usm_ms = telemetry.get("first_image_unsharp_mask_ms", 0.0)
        reported_ram = telemetry.get("peak_working_set_mb", peak_rss_mb)

        metric = ImageLoadMetric(
            ttfp_ms=ttfp_ms,
            load_ms=load_ms,
            last_op_ms=last_op_ms,
            unsharp_mask_ms=usm_ms,
            peak_working_set_mb=max(reported_ram, peak_rss_mb),
            exit_code=proc.returncode,
            has_internal_telemetry=bool(telemetry)
        )
        return metric.to_dict()

    def run_folder_navigation_benchmark(
        self,
        exe_path: Path,
        folder_or_image_path: Path,
        nav_count: int = 50,
        nav_steps: int | None = None,
        timeout_sec: float = 60.0
    ) -> dict[str, Any]:
        """
        Runs folder navigation benchmark traversing `nav_steps` files.
        Works via internal /benchmark_nav or simulated paced VK_RIGHT key messages.
        """
        steps = nav_steps if nav_steps is not None else nav_count
        first_image_path = folder_or_image_path
        if folder_or_image_path.is_dir():
            candidates = sorted(list(folder_or_image_path.glob("*.*")))
            if candidates:
                first_image_path = candidates[0]

        temp_json = Path(tempfile.gettempdir()) / f"telemetry_nav_{os.getpid()}_{time.time_ns()}.json"

        # Use list form — same quoting fix as run_image_load_benchmark
        cmd = [
            str(exe_path),
            str(first_image_path),
            f"/benchmark:{temp_json}",
            f"/benchmark_nav:{steps}",
            "/benchmark_exit"
        ]

        t_start = self._now_ms()

        proc = subprocess.Popen(
            cmd,
            cwd=str(exe_path.parent),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )

        h_proc = None
        if self.is_win32:
            h_proc = self.kernel32.OpenProcess(0x0400 | 0x0010, False, proc.pid)

        window_handle: int | None = None
        deadline = self._now_ms() + (timeout_sec * 1000.0)
        peak_rss_mb = 0.0

        try:
            # /benchmark_nav is a modern-only feature — JPEGView will self-exit after
            # completing all nav frames and writing telemetry. We simply wait for it.
            # No window automation needed or safe here (it would race the internal loop).
            while self._now_ms() < deadline:
                if proc.poll() is not None:
                    break

                if h_proc:
                    _, cur_peak = self._get_process_memory_mb(h_proc)
                    if cur_peak > peak_rss_mb:
                        peak_rss_mb = cur_peak

                time.sleep(0.01)

            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=2)

        finally:
            if h_proc:
                self.kernel32.CloseHandle(h_proc)

        t_end = self._now_ms()
        total_duration_ms = t_end - t_start

        telemetry: dict[str, Any] = {}
        if temp_json.exists():
            try:
                telemetry = json.loads(temp_json.read_text(encoding="utf-8"))
                temp_json.unlink(missing_ok=True)
            except Exception:
                pass

        if telemetry and "frame_times_ms" in telemetry:
            render_times: list[float] = telemetry.get("frame_times_ms", [])
            avg_frame_ms = telemetry.get("avg_frame_time_ms", 1.0)
            fps = telemetry.get("fps", 0.0)
        elif telemetry and "avg_frame_time_ms" in telemetry:
            avg_frame_ms = telemetry.get("avg_frame_time_ms", 1.0)
            fps = telemetry.get("fps", (1000.0 / avg_frame_ms) if avg_frame_ms > 0 else 0.0)
            render_times = [avg_frame_ms] * max(1, steps)
        else:
            steps_done = max(1, steps)
            avg_frame_ms = max(0.1, total_duration_ms / steps_done)
            fps = (1000.0 / avg_frame_ms) if avg_frame_ms > 0 else 0.0
            render_times = [avg_frame_ms] * steps_done

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

        metric = FolderNavMetric(
            fps=fps,
            avg_frame_ms=avg_frame_ms,
            p95_frame_ms=p95_frame_ms,
            p99_frame_ms=p99_frame_ms,
            frame_jitter_ms=jitter_ms,
            peak_working_set_mb=max(reported_ram, peak_rss_mb),
            frames_measured=len(render_times),
            has_internal_telemetry=bool(telemetry)
        )
        return metric.to_dict()
