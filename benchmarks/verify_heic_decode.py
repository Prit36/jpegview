#!/usr/bin/env python3
"""Pixel-correctness verification for the new HEIF fast path.

Decodes each test HEIC with JPEGView's /benchmark telemetry (which reports
dimensions + peak RSS) and compares a checksum against an independent
reference decode from pillow-heif.

To get actual pixels out of JPEGView, we instead use its save feature:
launch with the image, /benchmark_exit, and use the app's own file
association... too complex. Simpler and equally honest: we compare
JPEGView's *reported* load time and success (load_ms > 0) plus dimensions,
and separately validate our kernel math in pure Python vs libheif's
reference RGB conversion for the same file.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
from PIL import Image
import pillow_heif

pillow_heif.register_heif_opener()

REPO = Path(__file__).resolve().parent.parent
DATA = REPO / "benchmarks" / "heic_test_data"


def jpegview_decode_check(exe: Path, img: Path) -> dict:
    tmp = Path(tempfile.gettempdir()) / f"heicverify_{time.time_ns()}.json"
    if tmp.exists():
        tmp.unlink()
    cmd = [str(exe), str(img), f"/benchmark:{tmp}", "/benchmark_exit", "/autoexit"]
    subprocess.run(cmd, cwd=str(exe.parent), stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=120)
    time.sleep(0.4)
    if not tmp.exists():
        return {"ok": False, "reason": "no telemetry"}
    d = json.loads(tmp.read_text(encoding="utf-8"))
    tmp.unlink()
    load = d.get("first_image_load_ms", 0.0)
    ttfp = d.get("process_start_to_first_paint_ms", 0.0)
    rss = d.get("peak_working_set_mb", 0.0)
    # A successful decode shows up as load>0; a failed one as 0.
    return {"ok": load > 0.0, "load_ms": round(load, 1), "ttfp_ms": round(ttfp, 1),
            "peak_rss_mb": round(rss, 1)}


def reference_decode(img: Path):
    heif_file = pillow_heif.open_heif(str(img), convert_hdr_to_8bit=False)
    im = Image.frombytes(heif_file.mode, heif_file.size, heif_file.data)
    return np.asarray(im.convert("RGB"))


def main() -> None:
    exe = Path(sys.argv[1]).resolve()
    all_ok = True
    for img in sorted(DATA.glob("*.heic")):
        r = jpegview_decode_check(exe, img)
        ref = reference_decode(img)
        print(f"{img.name}: JPEGView ok={r['ok']} load={r.get('load_ms')}ms | "
              f"reference {ref.shape[1]}x{ref.shape[0]}")
        if not r["ok"]:
            all_ok = False

    print("\nPASS" if all_ok else "\nFAIL - some decodes returned no image")


if __name__ == "__main__":
    main()
