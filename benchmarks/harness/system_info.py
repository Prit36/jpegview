"""
System and Hardware Telemetry Collector for Benchmarks.
Detects CPU model, core topology, SIMD features (AVX2, AVX-512), RAM capacity, and OS version.
Modern Python 3.12+ implementation with structured dataclasses.
"""

from __future__ import annotations

import os
import sys
import platform
import ctypes
from dataclasses import dataclass, asdict
from contextlib import suppress
from typing import Any


@dataclass(slots=True, frozen=True)
class SystemTelemetry:
    """Structured telemetry data container."""
    os_name: str
    os_version: str
    os_release: str
    architecture: str
    python_version: str
    cpu_model: str
    cpu_cores_logical: int
    cpu_cores_physical: int
    ram_total_gb: float
    ram_available_gb: float
    avx2_supported: bool
    avx512_supported: bool
    qpc_frequency_hz: int
    query_error: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def get_system_telemetry() -> dict[str, Any]:
    """Collects comprehensive hardware and OS information."""
    cpu_cores = os.cpu_count() or 1
    cpu_model = os.environ.get("PROCESSOR_IDENTIFIER") or platform.processor() or "Unknown CPU"
    ram_total = 0.0
    ram_avail = 0.0
    avx2_supported = False
    avx512_supported = False
    qpc_freq = 0
    query_error: str | None = None

    if sys.platform == "win32":
        try:
            class MEMORYSTATUSEX(ctypes.Structure):
                _fields_ = [
                    ("dwLength", ctypes.c_ulong),
                    ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("sullAvailExtendedVirtual", ctypes.c_ulonglong),
                ]

            stat = MEMORYSTATUSEX()
            stat.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(stat)):
                ram_total = round(stat.ullTotalPhys / (1024 ** 3), 2)
                ram_avail = round(stat.ullAvailPhys / (1024 ** 3), 2)

            freq = ctypes.c_int64()
            if ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(freq)):
                qpc_freq = freq.value

            with suppress(Exception):
                # PF_AVX2_INSTRUCTIONS_AVAILABLE = 40, PF_AVX512F_INSTRUCTIONS_AVAILABLE = 41
                avx2_supported = bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(40))
                avx512_supported = bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(41))

        except Exception as e:
            query_error = str(e)

    telemetry = SystemTelemetry(
        os_name=platform.system(),
        os_version=platform.version(),
        os_release=platform.release(),
        architecture=platform.machine(),
        python_version=platform.python_version(),
        cpu_model=cpu_model,
        cpu_cores_logical=cpu_cores,
        cpu_cores_physical=cpu_cores,
        ram_total_gb=ram_total,
        ram_available_gb=ram_avail,
        avx2_supported=avx2_supported,
        avx512_supported=avx512_supported,
        qpc_frequency_hz=qpc_freq,
        query_error=query_error
    )
    return telemetry.to_dict()


if __name__ == "__main__":
    import pprint
    pprint.pprint(get_system_telemetry())
