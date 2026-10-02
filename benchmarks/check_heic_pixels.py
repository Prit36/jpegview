"""Compare full pixels from the production-object probe with pillow-heif.

Usage: python benchmarks/check_heic_pixels.py path/to/heic_decode_probe.exe
Writes raw production pixels and verification results to benchmarks/.cache.
"""
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path
import numpy as np
import pillow_heif

repo = Path(__file__).resolve().parent.parent
exe = Path(sys.argv[1]).resolve()
cache = repo / "benchmarks/.cache"
rows = []
for image in sorted((repo / "benchmarks/heic_test_data").glob("*.heic")):
    prefix = cache / f"verify_{image.stem}"
    process = subprocess.run([str(exe), str(image), str(prefix)], cwd=exe.parent,
                             capture_output=True, text=True, timeout=30, check=True)
    reference = pillow_heif.open_heif(image, convert_hdr_to_8bit=True)
    rgb = np.asarray(reference).astype(np.int16)
    row = {"image": image.name, "probe": process.stdout.strip(), "reference_size": reference.size}
    for mode in ["cpu", "warm"]:
        raw = Path(f"{prefix}.{mode}.bgra").read_bytes()
        width, height = struct.unpack("<ii", raw[:8])
        assert (width, height) == reference.size
        assert len(raw) == 8 + width * height * 4
        pixels = np.frombuffer(raw[8:], dtype=np.uint8).reshape(height, width, 4)
        assert np.all(pixels[:, :, 3] == 255)
        diff = np.abs(pixels[:, :, [2, 1, 0]].astype(np.int16) - rgb[:, :, :3])
        row[mode] = {"sha256": hashlib.sha256(raw).hexdigest(),
                     "mean_absolute_rgb_error": float(diff.mean()),
                     "p99_absolute_rgb_error": float(np.percentile(diff, 99)),
                     "maximum_absolute_rgb_error": int(diff.max())}
    if len(sys.argv) > 2:
        legacy = Path(sys.argv[2]).resolve()
        legacy_prefix = cache / f"legacy_{image.stem}"
        subprocess.run([str(legacy), str(image), str(legacy_prefix)], cwd=legacy.parent,
                       capture_output=True, timeout=30, check=True)
        legacy_raw = Path(f"{legacy_prefix}.cpu.bgra").read_bytes()
        row["legacy_software_sha256"] = hashlib.sha256(legacy_raw).hexdigest()
        row["unchanged_software_pixels"] = row["legacy_software_sha256"] == row["cpu"]["sha256"]
        assert row["unchanged_software_pixels"], f"Software pixels changed: {image.name}"
    print(json.dumps(row), flush=True)
    rows.append(row)
(cache / "heic_pixels.json").write_text(json.dumps(rows, indent=2))
