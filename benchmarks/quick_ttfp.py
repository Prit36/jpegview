#!/usr/bin/env python3
"""Quick TTFP benchmark on RAW15538.JPG against an arbitrary exe."""
from __future__ import annotations
import sys, statistics
from pathlib import Path

BENCHMARKS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(BENCHMARKS_DIR))

from harness.process_runner import ProcessRunner

def main():
    exe = Path(sys.argv[1]) if len(sys.argv) > 1 else BENCHMARKS_DIR.parent / "src" / "JPEGView" / "bin" / "x64" / "Release" / "JPEGView.exe"
    img = Path(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].isdigit() else BENCHMARKS_DIR / "actual_test_data" / "RAW15538.JPG"
    iters = int(sys.argv[3]) if len(sys.argv) > 3 else (int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 5)
    warmups = int(sys.argv[4]) if len(sys.argv) > 4 else 2
    exe = Path(exe)
    assert exe.exists(), f"exe not found: {exe}"

    runner = ProcessRunner()
    print(f"EXE: {exe}")
    print(f"IMG: {img} ({img.stat().st_size/1048576:.2f} MB)")

    for _ in range(warmups):
        runner.run_image_load_benchmark(exe, img)

    ttfp, load, lastop, usm, ram = [], [], [], [], []
    for i in range(iters):
        s = runner.run_image_load_benchmark(exe, img)
        ttfp.append(s["ttfp_ms"]); load.append(s["load_ms"])
        lastop.append(s["last_op_ms"]); usm.append(s["unsharp_mask_ms"])
        ram.append(s["peak_working_set_mb"])
        print(f"  run {i+1}: ttfp={s['ttfp_ms']:.1f} load={s['load_ms']:.1f} last_op={s['last_op_ms']:.1f} usm={s['unsharp_mask_ms']:.1f} ram={s['peak_working_set_mb']:.0f}MB telemetry={s['has_internal_telemetry']}")

    def st(v):
        return f"mean={statistics.mean(v):.1f} min={min(v):.1f} max={max(v):.1f}"
    print(f"\nTTFP ms : {st(ttfp)}")
    print(f"LOAD ms : {st(load)}")
    print(f"LASTOP ms: {st(lastop)}")
    print(f"USM ms  : {st(usm)}")
    print(f"RAM MB  : mean={statistics.mean(ram):.0f}")

if __name__ == "__main__":
    main()
