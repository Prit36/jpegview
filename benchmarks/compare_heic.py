"""Paired real-app HEIC benchmark, including every supplied file and every run.

Fresh process per sample; normal /benchmark telemetry; unchanged full decode,
processing, painting and read-ahead. Warm OS file cache for both executables.
Randomized image order, alternating baseline/candidate order, no outlier removal.
"""
import argparse
import hashlib
import json
import random
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(exe, image):
    with tempfile.TemporaryDirectory(prefix="heic_pair_") as tmp:
        telemetry = Path(tmp) / "result.json"
        start = time.perf_counter()
        proc = subprocess.run([str(exe), str(image), f"/benchmark:{telemetry}",
                               "/benchmark_exit", "/autoexit"], cwd=exe.parent,
                              capture_output=True, timeout=30)
        wall = (time.perf_counter() - start) * 1000
        if proc.returncode != 0:
            raise RuntimeError(f"{exe} {image.name}: exit {proc.returncode}")
        data = json.loads(telemetry.read_text())
        if data["first_image_load_ms"] <= 0 or data["process_start_to_first_paint_ms"] <= 0:
            raise RuntimeError(f"Invalid sample: {image.name}: {data}")
        data["process_lifetime_wall_ms"] = wall
    time.sleep(0.5)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--iterations", type=int, default=12)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    exes = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    data_dir = Path(__file__).resolve().parent / "heic_test_data"
    images = sorted(p for p in data_dir.iterdir() if p.suffix.lower() in {".heic", ".heif", ".hif"})
    if not images:
        raise RuntimeError("Empty corpus")
    config = ["JPEGView.ini", "KeyMap.txt.default", "symbols.km"]
    runtimes = sorted(p.name for p in exes["baseline"].parent.glob("*.dll"))
    for name in config + runtimes:
        if sha(exes["baseline"].parent / name) != sha(exes["candidate"].parent / name):
            raise RuntimeError(f"Configuration/runtime mismatch: {name}")
    result = {"iterations_per_file_per_build": args.iterations, "seed": 4202,
              "cache": "warm OS file cache; fresh decoder/process per sample",
              "executables": {k: {"path": str(v), "sha256": sha(v)} for k, v in exes.items()},
              "images": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size} for p in images},
              "samples": [], "summary": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for image in images:
        for exe in exes.values():
            run(exe, image)
    rng = random.Random(result["seed"])
    for iteration in range(args.iterations):
        order = images.copy()
        rng.shuffle(order)
        for index, image in enumerate(order):
            builds = ["baseline", "candidate"] if (iteration + index) % 2 == 0 else ["candidate", "baseline"]
            for build in builds:
                sample = run(exes[build], image)
                result["samples"].append({"iteration": iteration, "image": image.name, "build": build, **sample})
        args.output.write_text(json.dumps(result, indent=2))
        print(f"Completed paired round {iteration + 1}/{args.iterations}", flush=True)
    for image in images:
        row = {"image": image.name}
        for metric in ["first_image_load_ms", "process_start_to_first_paint_ms", "process_lifetime_wall_ms", "peak_working_set_mb"]:
            paired = {}
            for build in exes:
                values = [s[metric] for s in result["samples"] if s["image"] == image.name and s["build"] == build]
                paired[build] = {"mean": statistics.mean(values), "median": statistics.median(values),
                                 "min": min(values), "max": max(values), "stdev": statistics.stdev(values)}
            paired["mean_reduction_percent"] = 100 * (1 - paired["candidate"]["mean"] / paired["baseline"]["mean"])
            row[metric] = paired
        result["summary"].append(row)
        load = row["first_image_load_ms"]
        paint = row["process_start_to_first_paint_ms"]
        print(f"{image.name}: load {load['baseline']['mean']:.1f} -> {load['candidate']['mean']:.1f} ms "
              f"({load['mean_reduction_percent']:.1f}%); paint "
              f"{paint['baseline']['mean']:.1f} -> {paint['candidate']['mean']:.1f} ms", flush=True)
    args.output.write_text(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
