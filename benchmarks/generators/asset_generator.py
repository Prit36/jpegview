"""
Synthetic Massive Asset and Test Folder Generator for JPEGView Benchmarks.
Generates 100MB-200MB+ stress images (8K to 24K) and diverse folder navigation datasets.
Modern Python 3.12+ implementation.
"""

from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any
from .image_patterns import create_bmp_bytes, create_tga_bytes, create_qoi_bytes


class AssetGenerator:
    """Manages generation, caching, and lifecycle of synthetic benchmark test assets."""

    def __init__(self, base_dir: Path | str | None = None) -> None:
        if base_dir is None:
            self.base_dir = Path(__file__).resolve().parent.parent / "assets"
        else:
            self.base_dir = Path(base_dir)

        self.base_dir.mkdir(parents=True, exist_ok=True)
        self.manifest_file = self.base_dir / "assets_manifest.json"
        self.manifest: dict[str, Any] = self._load_manifest()

    def _load_manifest(self) -> dict[str, Any]:
        if self.manifest_file.exists():
            try:
                return json.loads(self.manifest_file.read_text(encoding="utf-8"))
            except Exception:
                return {}
        return {}

    def _save_manifest(self) -> None:
        self.manifest_file.write_text(json.dumps(self.manifest, indent=2), encoding="utf-8")

    def generate_large_image(
        self,
        target_size_mb: int = 120,
        dimension: tuple[int, int] = (16384, 16384),
        pattern: str = "gradient",
        format_ext: str = "bmp",
        force: bool = False
    ) -> Path:
        """
        Generates a massive high-resolution image file (up to 200MB+).
        """
        width, height = dimension
        filename = f"large_{width}x{height}_{pattern}.{format_ext}"
        out_path = self.base_dir / filename

        if out_path.exists() and not force:
            actual_mb = out_path.stat().st_size / (1024 * 1024)
            print(f"[+] Reusing cached large image: {filename} ({actual_mb:.2f} MB)")
            return out_path

        print(f"[*] Generating {width}x{height} {pattern} {format_ext.upper()} (~{target_size_mb} MB)...")
        start_t = time.perf_counter()

        match format_ext.lower():
            case "bmp":
                data = create_bmp_bytes(width, height, pattern_type=pattern, bpp=24)
            case "tga":
                data = create_tga_bytes(width, height, pattern_type=pattern)
            case "qoi":
                data = create_qoi_bytes(width, height, pattern_type=pattern)
            case _:
                data = create_bmp_bytes(width, height, pattern_type=pattern, bpp=24)

        out_path.write_bytes(data)
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
        force: bool = False
    ) -> Path:
        """
        Generates a test dataset folder with diverse image files for folder navigation benchmarks.
        """
        folder_path = self.base_dir / folder_name
        folder_path.mkdir(parents=True, exist_ok=True)

        existing_files = list(folder_path.glob("*.*"))
        if len(existing_files) >= file_count and not force:
            print(f"[+] Reusing cached dataset '{folder_name}' ({len(existing_files)} files)")
            return folder_path

        print(f"[*] Generating test dataset '{folder_name}' with {file_count} images...")
        start_t = time.perf_counter()

        patterns = ["gradient", "checkerboard", "high_frequency", "solid"]
        formats = ["bmp", "tga", "qoi"]

        for i in range(file_count):
            pat = patterns[i % len(patterns)]
            fmt = formats[i % len(formats)]
            match i % 5:
                case 0:
                    w, h = 3840, 2160  # 4K
                case 1:
                    w, h = 1920, 1080  # 1080p
                case 2:
                    w, h = 2560, 1440  # 1440p
                case 3:
                    w, h = 1200, 1600  # Portrait
                case _:
                    w, h = 800, 600    # Thumbnail

            img_name = f"img_{i:04d}_{w}x{h}_{pat}.{fmt}"
            img_path = folder_path / img_name

            if not img_path.exists() or force:
                match fmt:
                    case "bmp":
                        data = create_bmp_bytes(w, h, pattern_type=pat, bpp=24)
                    case "tga":
                        data = create_tga_bytes(w, h, pattern_type=pat)
                    case "qoi":
                        data = create_qoi_bytes(w, h, pattern_type=pat)
                    case _:
                        data = create_bmp_bytes(w, h, pattern_type=pat, bpp=24)
                img_path.write_bytes(data)

        elapsed = time.perf_counter() - start_t
        print(f"[+] Dataset '{folder_name}' ready with {file_count} files in {elapsed:.2f}s")
        return folder_path

    def ensure_profile_assets(self, profile_config: dict[str, Any]) -> dict[str, Path]:
        """Ensures all assets required for a profile exist on disk."""
        assets: dict[str, Path] = {}
        large_cfg = profile_config.get("large_image", {})
        if large_cfg:
            dim = tuple(large_cfg.get("dimension", [16384, 16384]))
            p = self.generate_large_image(
                target_size_mb=large_cfg.get("target_size_mb", 120),
                dimension=dim,
                pattern=large_cfg.get("pattern", "gradient"),
                format_ext=large_cfg.get("format", "bmp")
            )
            assets["large_image"] = p

        folder_cfg = profile_config.get("folder_dataset", {})
        if folder_cfg:
            p_folder = self.generate_test_folder(
                folder_name=folder_cfg.get("name", "dataset_standard"),
                file_count=folder_cfg.get("file_count", 100)
            )
            assets["folder_dataset"] = p_folder

        return assets
