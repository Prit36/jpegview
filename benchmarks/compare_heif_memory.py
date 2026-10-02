"""Capture OS lifetime peak working set after exit, without polling the app."""
import ctypes
from ctypes import wintypes
import sys
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_heic

class Counters(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in
        ("peak", "working", "qpp", "qp", "qnpp", "qnp", "page", "peakpage", "private")]

get_memory = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
get_memory.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
get_memory.restype = wintypes.BOOL
original_run = compare_heic.run
latest = {}

def subprocess_run(args, *, cwd, capture_output, timeout):
    with subprocess.Popen(args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as proc:
        stdout, stderr = proc.communicate(timeout=timeout)
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        if not get_memory(int(proc._handle), ctypes.byref(counters), counters.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        latest["lifetime_peak_working_set_mb"] = counters.peak / (1024 * 1024)
        latest["lifetime_peak_pagefile_mb"] = counters.peakpage / (1024 * 1024)
        return subprocess.CompletedProcess(args, proc.returncode, stdout, stderr)

def run(exe, image):
    result = original_run(exe, image)
    result.update(latest)
    return result

compare_heic.subprocess.run = subprocess_run
compare_heic.run = run
compare_heic.main()
