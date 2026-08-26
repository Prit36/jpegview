#!/usr/bin/env python3
import json
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

def run_once(exe: Path, image: Path, retries: int = 3) -> dict:
    tmp = Path(tempfile.gettempdir()) / f"hifbench_{time.time_ns()}.json"
    cmd = [str(exe), str(image), f"/benchmark:{tmp}", "/benchmark_exit", "/autoexit"]
    t0 = time.perf_counter()
    subprocess.run(cmd, cwd=str(exe.parent), stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=120)
    wall = (time.perf_counter() - t0) * 1000.0
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
            except Exception:
                pass
        time.sleep(0.05)
    raise RuntimeError(f"no valid telemetry for {image.name}")

def main():
    exe = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else (REPO / "build" / "bin" / "Release" / "JPEGView.exe").resolve()
    iters = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    data_dir = REPO / "benchmarks" / "heic_test_data"
    images = sorted(list(data_dir.glob("*.HIF")) + list(data_dir.glob("*.heic")))
    
    print(f"EXE: {exe}")
    print(f"{'Image':<28} {'load ms (mean)':>15} {'min':>9} {'max':>9}   {'TTFP mean':>10}  MB/s")
    overall = []
    for img in images:
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
