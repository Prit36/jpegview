"""Check production decoder pixel equivalence; never used for performance."""
import hashlib
import json
import struct
import subprocess
import tempfile
import sys
from pathlib import Path

repo = Path(__file__).resolve().parents[1]
binary = repo / "benchmarks/.cache/build/current/bin/Release"
new = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else binary / "heic_decode_probe.exe"
old = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else binary / "heic_decode_probe_legacy_software.exe"
rows = []
for image in sorted((repo / "benchmarks/heic_test_data").iterdir()):
    if image.suffix.lower() not in {".heic", ".heif", ".hif"}: continue
    with tempfile.TemporaryDirectory(prefix="heif_pixel_check_") as tmp:
        probes = {}
        buffers = {}
        for label, exe in [("old", old), ("new", new)]:
            prefix = Path(tmp) / label
            process = subprocess.run([str(exe), str(image), str(prefix)], cwd=exe.parent,
                                     capture_output=True, text=True, timeout=60, check=True)
            probes[label] = process.stdout.strip()
            for mode in ["cpu", "warm"]:
                raw = Path(f"{prefix}.{mode}.bgra").read_bytes()
                width, height = struct.unpack("<ii", raw[:8])
                assert len(raw) == 8 + width * height * 4
                alpha = raw[11::4]
                buffers[f"{label}_{mode}"] = {
                    "width": width, "height": height,
                    "sha256": hashlib.sha256(raw).hexdigest(),
                    "alpha_min": min(alpha), "alpha_max": max(alpha)}
        # The heavy 10-bit 4:2:2 files now use the existing hardware output
        # even when cold. Other non-alpha files retain their software-first path.
        old_mode = "warm" if "hardware_ready=1" in probes["new"].splitlines()[0] else "cpu"
        assert buffers["new_cpu"] == buffers[f"old_{old_mode}"], image.name
        has_alpha = "alpha=1" in probes["new"].splitlines()[0]
        expected_warm = "old_cpu" if has_alpha else "old_warm"
        assert buffers["new_warm"] == buffers[expected_warm], image.name
        row = {"image": image.name, "probe": probes, "buffers": buffers,
               "cold_matches_existing_path": old_mode, "warm_matches": expected_warm}
        print(json.dumps(row), flush=True)
        rows.append(row)
(repo / "benchmarks/.cache/heif_adaptive_pixels_2026-10-02.json").write_text(json.dumps(rows, indent=2))
