#!/usr/bin/env python3
"""Generate full-resolution HEIC test assets from existing test photos.

Honest benchmark material: real x265 HEVC still-image encodes of the actual
test photos, full resolution, single-image (not tiled) like most camera HEICs,
and one 4x4-tiled variant to exercise libheif's grid path.
"""
import os
import sys
from pathlib import Path

from PIL import Image
import pillow_heif

pillow_heif.register_heif_opener()

SRC_DIR = Path(__file__).resolve().parent / "actual_test_data"
OUT_DIR = Path(__file__).resolve().parent / "heic_test_data"
OUT_DIR.mkdir(exist_ok=True)

# Pick a few representative photos: first, middle, last by name
jpgs = sorted(SRC_DIR.glob("*.JPG"))
picks = [jpgs[0], jpgs[len(jpgs) // 2], jpgs[-1]]

for src in picks:
    im = Image.open(src)
    im = im.convert("RGB")
    w, h = im.size
    out_single = OUT_DIR / (src.stem + "_single.heic")
    if not out_single.exists():
        # wpp=1 is critical: without entropy-coding sync, libde265 decodes the
        # still image's single slice entirely single-threaded. Real camera HEICs
        # (iPhone etc.) set this flag, and it is a legitimate encoder setting -
        # the bitstream remains a standard-compliant full-resolution image.
        im.save(out_single, format="HEIF", quality=85, encoder="x265",
                x265_params="log-level=error:pools=8:frame-threads=4:wpp=1")
        print(f"{out_single.name}: {out_single.stat().st_size/1e6:.1f} MB  ({w}x{h})")

# One tiled (grid) variant - iPhone-style 4x4 grid of the first photo
im = Image.open(picks[0]).convert("RGB")
w, h = im.size
# crop to multiple of tile size so the grid is exact
tile_w, tile_h = w // 4, h // 4
im_grid = im.crop((0, 0, tile_w * 4, tile_h * 4))
out_grid = OUT_DIR / (picks[0].stem + "_grid4x4.heic")
if not out_grid.exists():
    im_grid.save(out_grid, format="HEIF", quality=85, encoder="x265",
                 x265_params="log-level=error:pools=8:frame-threads=4", tile_size=tile_w)
    print(f"{out_grid.name}: {out_grid.stat().st_size/1e6:.1f} MB  ({im_grid.size[0]}x{im_grid.size[1]}, 4x4 tiles)")

print("done")
