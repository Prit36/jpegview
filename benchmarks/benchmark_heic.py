#!/usr/bin/env python3
"""Honest HEIC decode benchmark: drives the real JPEGView.exe with its
/benchmark telemetry flag (same as benchmarks/harness) over full-resolution
HEIC files, N iterations each, and reports mean/min decode times.

No downsampling, no synthetic shortcuts - the app decodes the complete image.
"""
from __future__ import annotations
import json
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def run_once(exe: Path, image: Path, retries: int = 3) -> dict:
    tmp = Path(tempfile.gettempdir()) / f"heicbench_{time.time_ns()}.json"
    cmd = [str(exe), str(image), f"/benchmark:{tmp}", "/benchmark_exit", "/autoexit"]
    t0 = time.perf_counter()
    subprocess.run(cmd, cwd=str(exe.parent), stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=120)
    wall = (time.perf_counter() - t0) * 1000.0
    # let the previous instance fully exit so single-instance forwarding
    # doesn't swallow the next launch (that writes telemetry with no image)
    time.sleep(0.5)
    for _ in range(retries):
        if tmp.exists():
            try:
                data = json.loads(tmp.read_text(encoding="utf-8"))
                tmp.unlink()
                r = {
                    "ttfp": data.get("process_start_to_first_paint_ms", 0.0),
                    "load": data.get("first_image_load_ms", 0.0),
                    "wall": wall,
                }
                if r["load"] > 0.0 and r["ttfp"] > 0.0:
                    return r
                # zero metrics = forwarded-to-other-instance run; redo
            except Exception:
                pass
        time.sleep(0.05)
    raise RuntimeError(f"no valid telemetry for {image.name}")


def main() -> None:
    exe = Path(sys.argv[1]).resolve()
    iters = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    data_dir = REPO / "benchmarks" / "heic_test_data"
    images = sorted(data_dir.glob("*.heic"))
    if not images:
        print("no heic files found")
        sys.exit(1)

    print(f"EXE: {exe}")
    print(f"{'Image':<28} {'load ms (mean)':>15} {'min':>9} {'max':>9}   {'TTFP mean':>10}  MB/s")
    overall = []
    for img in images:
        # one untimed warmup (OS file cache, DLL warm)
        run_once(exe, img)
        samples, ttfps = [], []
        for _ in range(iters):
            r = run_once(exe, img)
            samples.append(r["load"])
            ttfps.append(r["ttfp"])
        mb = img.stat().st_size / 1e6
        m = statistics.mean(samples)
        overall.append(m)
        print(f"{img.name:<28} {m:>15.1f} {min(samples):>9.1f} {max(samples):>9.1f}   "
              f"{statistics.mean(ttfps):>10.1f}  {mb / (m / 1000.0):.1f}")
    print(f"\nOverall mean load time: {statistics.mean(overall):.1f} ms")


if __name__ == "__main__":
    main()
