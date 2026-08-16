"""
Test Asset Generator for JPEGView Benchmarks.
Generates large 100MB-200MB test images and high-capacity directory datasets.
"""

import os
import sys
import json
import time
import hashlib
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    from .image_patterns import create_bmp_bytes, create_tga_bytes, create_qoi_bytes
except ImportError:
    from image_patterns import create_bmp_bytes, create_tga_bytes, create_qoi_bytes


class AssetGenerator:
    """Manages generation, validation, and caching of synthetic benchmark assets."""

    def __init__(self, base_dir: Optional[Path] = None):
        if base_dir is None:
            self.base_dir = Path(__file__).resolve().parent.parent / "assets"
        else:
            self.base_dir = Path(base_dir)
        self.base_dir.mkdir(parents=True, exist_ok=True)
        self.manifest_path = self.base_dir / "manifest.json"
        self.manifest = self._load_manifest()

    def _load_manifest(self) -> Dict:
        if self.manifest_path.exists():
            try:
                with open(self.manifest_path, "r", encoding="utf-8") as f:
                    return json.load(f)
            except Exception:
                return {}
        return {}

    def _save_manifest(self):
        with open(self.manifest_path, "w", encoding="utf-8") as f:
            json.dump(self.manifest, f, indent=2)

    def generate_large_image(
        self,
        dimension: int = 16384,
        target_mb: int = 150,
        pattern: str = "gradient",
        format_ext: str = "bmp",
        force: bool = False
    ) -> Path:
        """
        Generates or retrieves a large test image (e.g. 100MB-200MB / 16K-24K resolution).
        """
        filename = f"large_{dimension}x{dimension}_{pattern}.{format_ext}"
        out_path = self.base_dir / filename

        if out_path.exists() and not force:
            size_mb = out_path.stat().st_size / (1024 * 1024)
            if size_mb >= 5: # Valid file
                return out_path

        print(f"[*] Generating large test image: {filename} (~{target_mb}MB, {dimension}x{dimension})...")
        start_t = time.perf_counter()

        if format_ext == "bmp":
            # For exact target size, compute dimensions or use dimension
            data = create_bmp_bytes(dimension, dimension, pattern_type=pattern, bpp=24)
        elif format_ext == "qoi":
            data = create_qoi_bytes(min(dimension, 4096), min(dimension, 4096), pattern_type=pattern)
        elif format_ext == "tga":
            data = create_tga_bytes(dimension, dimension, pattern_type=pattern)
        else:
            data = create_bmp_bytes(dimension, dimension, pattern_type=pattern, bpp=24)

        with open(out_path, "wb") as f:
            f.write(data)

        elapsed = time.perf_counter() - start_t
        actual_mb = out_path.stat().st_size / (1024 * 1024)
        print(f"[+] Generated {filename} ({actual_mb:.2f} MB) in {elapsed:.2f}s")

        self.manifest[filename] = {
            "path": str(out_path),
            "size_mb": actual_mb,
            "dimension": dimension,
            "pattern": pattern,
            "format": format_ext
        }
        self._save_manifest()
        return out_path

    def generate_test_folder(
        self,
        folder_name: str = "dataset_standard",
        file_count: int = 100,
        dim_range: Tuple[int, int] = (800, 3840),
        force: bool = False
    ) -> Path:
        """
        Generates a test dataset folder with diverse image files for folder navigation benchmarks.
        """
        folder_path = self.base_dir / folder_name
        folder_path.mkdir(parents=True, exist_ok=True)

        existing_files = list(folder_path.glob("*.*"))
        if len(existing_files) >= file_count and not force:
            return folder_path

        print(f"[*] Generating test dataset '{folder_name}' with {file_count} images...")
        start_t = time.perf_counter()

        patterns = ["gradient", "checkerboard", "high_frequency", "solid"]
        formats = ["bmp", "tga", "qoi"]

        for i in range(file_count):
            pat = patterns[i % len(patterns)]
            fmt = formats[i % len(formats)]
            # Distribute dimensions: some small, some 1080p, some 4K UHD
            if i % 5 == 0:
                w, h = 3840, 2160 # 4K
            elif i % 5 == 1:
                w, h = 1920, 1080 # 1080p
            elif i % 5 == 2:
                w, h = 2560, 1440 # 1440p
            elif i % 5 == 3:
                w, h = 1200, 1600 # Portrait
            else:
                w, h = 800, 600   # Thumbnail/small

            img_name = f"img_{i:04d}_{w}x{h}_{pat}.{fmt}"
            img_path = folder_path / img_name

            if not img_path.exists() or force:
                if fmt == "bmp":
                    data = create_bmp_bytes(w, h, pattern_type=pat, bpp=24)
                elif fmt == "tga":
                    data = create_tga_bytes(w, h, pattern_type=pat)
                elif fmt == "qoi":
                    data = create_qoi_bytes(w, h, pattern_type=pat)
                else:
                    data = create_bmp_bytes(w, h, pattern_type=pat, bpp=24)

                with open(img_path, "wb") as f:
                    f.write(data)

        elapsed = time.perf_counter() - start_t
        print(f"[+] Dataset '{folder_name}' ready with {file_count} files in {elapsed:.2f}s")
        return folder_path

    def ensure_profile_assets(self, profile_config: Dict) -> Dict[str, Path]:
        """Ensures all assets required for a profile exist."""
        large_dim = profile_config.get("large_image_dim", 16384)
        large_mb = profile_config.get("large_image_mb", 150)
        file_count = profile_config.get("folder_file_count", 100)

        large_img = self.generate_large_image(dimension=large_dim, target_mb=large_mb)
        folder = self.generate_test_folder(folder_name=f"dataset_{file_count}_files", file_count=file_count)

        return {
            "large_image": large_img,
            "folder": folder
        }


if __name__ == "__main__":
    gen = AssetGenerator()
    gen.generate_large_image(dimension=8192, target_mb=50)
    gen.generate_test_folder(folder_name="dataset_quick", file_count=20)
