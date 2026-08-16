"""
Target Compiler and Artifact Caching Orchestrator.
Auto-detects MSBuild / CMake and compiles isolated Release x64 binaries.
Modern Python 3.12+ implementation.
"""

from __future__ import annotations

import os
import shutil
import time
import subprocess
from pathlib import Path


class TargetBuilder:
    """Orchestrates compilation and caching of binaries for evaluated targets."""

    def __init__(self, repo_root: Path | str | None = None) -> None:
        if repo_root is None:
            self.repo_root = Path(__file__).resolve().parent.parent.parent
        else:
            self.repo_root = Path(repo_root)

        self.bin_cache_dir = self.repo_root / "benchmarks" / ".cache" / "bin"
        self.build_cache_dir = self.repo_root / "benchmarks" / ".cache" / "build"
        self.bin_cache_dir.mkdir(parents=True, exist_ok=True)
        self.build_cache_dir.mkdir(parents=True, exist_ok=True)

        self.cmake_path = self._find_cmake()
        self.msbuild_path = self._find_msbuild()

    def _find_cmake(self) -> str | None:
        paths = [
            r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        ]
        for p in paths:
            if os.path.isfile(p):
                return p
        return shutil.which("cmake")

    def _find_msbuild(self) -> str | None:
        paths = [
            r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
            r"C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        ]
        for p in paths:
            if os.path.isfile(p):
                return p
        return shutil.which("MSBuild")

    def build_target(
        self,
        target_name: str,
        source_dir: Path,
        commit_hash: str,
        force_rebuild: bool = False
    ) -> Path:
        """
        Builds the target in Release x64 mode and caches the resulting binary.
        """
        out_target_dir = self.bin_cache_dir / target_name
        out_target_dir.mkdir(parents=True, exist_ok=True)
        exe_path = out_target_dir / "JPEGView.exe"
        hash_file = out_target_dir / ".commit_hash"

        # Check cached binary
        if not force_rebuild and exe_path.exists() and hash_file.exists():
            try:
                cached_hash = hash_file.read_text(encoding="utf-8").strip()
                if cached_hash == commit_hash and target_name != "current":
                    print(f"[+] Reusing cached binary for '{target_name}' ({commit_hash[:8]})")
                    return exe_path
            except Exception:
                pass



        print(f"[*] Building Release x64 binary for '{target_name}'...")
        start_t = time.perf_counter()

        target_build_dir = self.build_cache_dir / target_name
        target_build_dir.mkdir(parents=True, exist_ok=True)

        has_cmake = (source_dir / "CMakeLists.txt").exists()

        if has_cmake and self.cmake_path:
            cmake_cmd = [
                self.cmake_path,
                "-B", str(target_build_dir),
                "-S", str(source_dir),
                "-A", "x64",
                "-DCMAKE_BUILD_TYPE=Release"
            ]
            print(f"    Running CMake configure on {source_dir}...")
            cfg_res = subprocess.run(cmake_cmd, capture_output=True, text=True)
            if cfg_res.returncode != 0:
                print(f"[-] CMake configure error:\n{cfg_res.stderr}\n{cfg_res.stdout}")
                raise RuntimeError(f"CMake configure failed for {target_name}: {cfg_res.stderr}")

            build_cmd = [
                self.cmake_path,
                "--build", str(target_build_dir),
                "--config", "Release",
                "--parallel"
            ]
            print(f"    Running CMake build (Release x64)...")
            res = subprocess.run(build_cmd, capture_output=True, text=True)
            if res.returncode != 0:
                print(f"[-] Build error:\n{res.stderr}\n{res.stdout}")
                raise RuntimeError(f"Failed to build target {target_name}: {res.stderr}")

            candidates = list(target_build_dir.glob("**/JPEGView.exe"))
            if not candidates:
                raise FileNotFoundError(f"Could not locate JPEGView.exe after building in {target_build_dir}")

            built_exe = candidates[0]
            shutil.copy2(built_exe, exe_path)
            for dll in built_exe.parent.glob("*.dll"):
                shutil.copy2(dll, out_target_dir / dll.name)

        elif self.msbuild_path:
            wic_proj = source_dir / "src" / "WICLoader" / "WICLoader.vcxproj"
            if not wic_proj.exists():
                wic_proj = source_dir / "WICLoader" / "WICLoader.vcxproj"

            jpeg_proj = source_dir / "src" / "JPEGView" / "JPEGView.vcxproj"
            if not jpeg_proj.exists():
                jpeg_proj = source_dir / "JPEGView" / "JPEGView.vcxproj"

            if not jpeg_proj.exists():
                raise FileNotFoundError(f"No CMakeLists.txt or JPEGView.vcxproj found in {source_dir}")

            common_msbuild_flags = [
                "-p:Configuration=Release",
                "-p:Platform=x64",
                "-p:PlatformToolset=v143",
                "-p:PostBuildEventUseInBuild=false",
                "-m"
            ]

            if wic_proj.exists():
                print(f"    Building WICLoader.vcxproj...")
                res_wic = subprocess.run([self.msbuild_path, str(wic_proj)] + common_msbuild_flags, capture_output=True, text=True)
                if res_wic.returncode != 0:
                    print(f"[-] WICLoader MSBuild error:\n{res_wic.stdout}\n{res_wic.stderr}")

            print(f"    Building JPEGView.vcxproj...")
            res = subprocess.run([self.msbuild_path, str(jpeg_proj)] + common_msbuild_flags, capture_output=True, text=True)
            if res.returncode != 0:
                print(f"[-] MSBuild error:\n{res.stdout}\n{res.stderr}")
                raise RuntimeError(f"MSBuild failed for {target_name}: {res.stderr}")

            candidates = list(source_dir.glob("**/JPEGView.exe"))
            if not candidates:
                raise FileNotFoundError(f"Could not find built JPEGView.exe in {source_dir}")

            built_exe = candidates[0]
            shutil.copy2(built_exe, exe_path)
            for dll in source_dir.glob("**/*.dll"):
                shutil.copy2(dll, out_target_dir / dll.name)

        else:
            raise RuntimeError("Neither CMake nor MSBuild was found on the system.")

        hash_file.write_text(commit_hash, encoding="utf-8")

        elapsed = time.perf_counter() - start_t
        print(f"[+] Build successful for '{target_name}' in {elapsed:.2f}s -> {exe_path}")
        return exe_path
