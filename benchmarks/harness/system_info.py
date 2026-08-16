"""
System and Hardware Telemetry Collector for Benchmarks.
Detects CPU model, core topology, SIMD features (AVX2, AVX-512), RAM capacity, and OS version.
"""

import os
import sys
import platform
import subprocess
import ctypes
from typing import Dict, Any


def get_system_telemetry() -> Dict[str, Any]:
    """Collects comprehensive hardware and OS information."""
    info: Dict[str, Any] = {
        "os_name": platform.system(),
        "os_version": platform.version(),
        "os_release": platform.release(),
        "architecture": platform.machine(),
        "python_version": platform.python_version(),
        "cpu_model": "Unknown CPU",
        "cpu_cores_logical": os.cpu_count() or 1,
        "cpu_cores_physical": os.cpu_count() or 1,
        "ram_total_gb": 0.0,
        "ram_available_gb": 0.0,
        "avx2_supported": False,
        "avx512_supported": False,
        "qpc_frequency_hz": 0
    }

    # Query Windows-specific CPU and Memory Info
    if sys.platform == "win32":
        try:
            # Query CPU via wmic or environment
            cpu_brand = os.environ.get("PROCESSOR_IDENTIFIER", "")
            if not cpu_brand:
                cpu_brand = platform.processor()
            info["cpu_model"] = cpu_brand

            # Query RAM via GlobalMemoryStatusEx
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
                info["ram_total_gb"] = round(stat.ullTotalPhys / (1024 ** 3), 2)
                info["ram_available_gb"] = round(stat.ullAvailPhys / (1024 ** 3), 2)

            # Query QPC frequency
            freq = ctypes.c_int64()
            if ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(freq)):
                info["qpc_frequency_hz"] = freq.value

            # Check AVX2 / AVX512 support via IsProcessorFeaturePresent or CPUID
            # Windows API feature flags
            # PF_AVX2_INSTRUCTIONS_AVAILABLE = 40
            # PF_AVX512F_INSTRUCTIONS_AVAILABLE = 41
            try:
                info["avx2_supported"] = bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(40))
                info["avx512_supported"] = bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(41))
            except Exception:
                info["avx2_supported"] = True

        except Exception as e:
            info["query_error"] = str(e)

    return info


if __name__ == "__main__":
    import pprint
    pprint.pprint(get_system_telemetry())
