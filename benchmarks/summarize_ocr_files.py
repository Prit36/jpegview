"""Validate and preserve complete per-file OCR benchmark runs, including raw samples."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import shutil
from collections import defaultdict

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parent / ".cache/ocr-files")
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--runs", type=int, default=2)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
samples = defaultdict(lambda: {"before": [], "after": []})
info = {}
expected = None
for run in range(1, args.runs + 1):
    meta = json.loads((args.source / f"run{run}-metadata.json").read_text(encoding="utf-8"))
    if meta.get("run_exit_code") != 0:
        raise RuntimeError("Measurement process did not exit successfully")
    corpus = {p["file"].replace("\\", "/") for p in meta["corpus"]}
    if expected is not None and corpus != expected:
        raise RuntimeError("Input corpus changed between runs")
    expected = corpus
    for entry in meta["corpus"]:
        path = Path(meta["data_dir"]) / entry["file"]
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise RuntimeError(f"Input changed since measurement: {path}")
    with (args.source / f"run{run}-results.csv").open(encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    keys = {(row["file"], row["stage"]) for row in rows}
    if keys != {(name, stage) for name in expected for stage in ("snapshot", "snapshot_plus_windows_ocr")}:
        raise RuntimeError("Missing file/stage or measurement failure")
    validation = (args.source / f"run{run}-validation.txt").read_text(encoding="utf-8")
    if "FAIL:" in validation or "CLEANUP: complete" not in validation:
        raise RuntimeError("Validation/cleanup failed")
    for row in rows:
        key = row["file"], row["stage"]
        value = tuple(row[field] for field in ("width", "height", "channels", "words"))
        if key in info and info[key] != value:
            raise RuntimeError(f"Image geometry or recognized word count changed between runs: {key}")
        info[key] = value
    with (args.source / f"run{run}-samples.csv").open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            key = row["file"], row["stage"]
            samples[key]["before"].append(float(row["before_ms"]))
            samples[key]["after"].append(float(row["after_ms"]))
    for artifact in ("results.csv", "samples.csv", "metadata.json", "validation.txt"):
        shutil.copy2(args.source / f"run{run}-{artifact}", args.output / f"run{run}-{artifact}")

def percentile(values, p):
    return sorted(values)[math.ceil(p * len(values)) - 1]

pooled = []
for name in sorted(expected, key=str.casefold):
    for stage in ("snapshot", "snapshot_plus_windows_ocr"):
        values = samples[name, stage]
        count = args.runs * (40 if stage == "snapshot" else 24)
        if len(values["before"]) != count or len(values["after"]) != count:
            raise RuntimeError(f"Incorrect sample count for {name}, {stage}")
        b, a = percentile(values["before"], .5), percentile(values["after"], .5)
        w, h, c, words = info[name, stage]
        pooled.append(dict(file=name, stage=stage, width=w, height=h, channels=c, samples=count,
                           before_median_ms=b, after_median_ms=a, reduction_percent=100 * (b - a) / b,
                           speedup=b / a, before_p95_ms=percentile(values["before"], .95),
                           after_p95_ms=percentile(values["after"], .95), words=words))
with (args.output / "pooled-results.csv").open("w", encoding="utf-8", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=list(pooled[0]))
    writer.writeheader()
    writer.writerows(pooled)

lines = ["# OCR before/after: all files in random_data", "",
         "Actual measurements on Intel Core i5-12400F / Windows 11, x64 MSVC Release. "
         f"All {len(expected)} files decoded successfully using JPEGView's real application loader. "
         "Baseline commit: `f3fc6d7b5882f7723e88e922b281e6098b696608`; after: optimized working tree.", "",
         f"Results pool {args.runs} complete, normally exited runs: {24 * args.runs} paired OCR calls per file, "
         f"and {40 * args.runs} paired snapshot samples per file (each averages three calls). "
         "Warm-up calls are excluded; before/after order alternates every pair. Medians and p95 use nearest rank.", "",
         "**Total** means snapshot preparation plus actual Windows recognition, including recognizer creation, "
         "bitmap upload, waiting, and result extraction. Each file is decoded once before timing; both "
         "implementations receive the same pixels. Decode time, pixel copying, deferred rotation, worker launch, "
         "UI message delivery and rendering are excluded. These are warmed OCR pipeline timings, not "
         "click-to-overlay or cold-start timings. Source dimensions below include EXIF rotation.", "",
         "An isolated INI enables EXIF autorotation and leaves transparency handling to the app's loader. "
         "HEIC decoding uses the app's existing adaptive software/hardware routing. Windows OCR language: "
         "English (United States). Zero words is a successful no-text result, not a decode failure.", "",
         "Every comparison passed: snapshot pixels are byte-identical; recognized language, words, order, "
         "line indices and all corner coordinates match before/after. Word counts also match between runs. "
         "No recognition-accuracy improvement is claimed.", "",
         "Initial discarded harness runs encountered a heap-corruption error at exit. Crash dumps "
         "located it in resize-filter cache teardown. The harness omitted the application's explicit "
         "cache initialization before starting worker threads; concurrent first use could register "
         "duplicate exit cleanup. The harness now mirrors that startup order and waits for HEIC "
         "initialization at shutdown. Only subsequently completed, successfully exited runs are included.", "",
         "| File | Dimensions | Words | Prep before ms | Prep after ms | Total before ms | Total after ms | Total reduction |",
         "|---|---|---:|---:|---:|---:|---:|---:|"]
for name in sorted(expected, key=str.casefold):
    prep = next(r for r in pooled if r["file"] == name and r["stage"] == "snapshot")
    total = next(r for r in pooled if r["file"] == name and r["stage"] == "snapshot_plus_windows_ocr")
    lines.append(f'| {name} | {total["width"]} × {total["height"]} | {total["words"]} | '
                 f'{prep["before_median_ms"]:.2f} | {prep["after_median_ms"]:.2f} | '
                 f'{total["before_median_ms"]:.2f} | {total["after_median_ms"]:.2f} | '
                 f'{total["reduction_percent"]:.1f}% |')
lines += ["", "Per-stage p95 and full precision are in [pooled-results.csv](pooled-results.csv). "
          "Both runs' summaries, raw paired samples, input hashes, compiler metadata and validation logs "
          "are preserved alongside this report.", "", "Reproduce from the repository root:", "", "```powershell",
          "python benchmarks/run_ocr_benchmark.py --data-dir benchmarks/random_data --baseline-ref "
          "f3fc6d7b5882f7723e88e922b281e6098b696608 --runs 2",
          f'python benchmarks/summarize_ocr_files.py --output "{args.output}" --runs {args.runs}', "```", ""]
(args.output / "README.md").write_text("\n".join(lines), encoding="utf-8")
print("\n".join(lines))
