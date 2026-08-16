"""
Target Binary Compiler and Build Orchestrator.
Discovers MSBuild & CMake toolchains, builds targets in isolated cache directories,
and produces optimized Release x64 binaries.
"""

import os
import sys
import shutil
import subprocess
import time
from pathlib import Path
from typing import Optional, Dict, Tuple


class TargetBuilder:
    """Builds and caches Release x64 binaries for benchmark targets."""

    def __init__(self, repo_root: Optional[Path] = None):
        if repo_root is None:
            self.repo_root = Path(__file__).resolve().parent.parent.parent
        else:
            self.repo_root = Path(repo_root)

        self.cache_dir = self.repo_root / "benchmarks" / ".cache"
        self.bin_cache_dir = self.cache_dir / "bin"
        self.build_cache_dir = self.cache_dir / "build"
        self.bin_cache_dir.mkdir(parents=True, exist_ok=True)
        self.build_cache_dir.mkdir(parents=True, exist_ok=True)

        self.msbuild_path = self._find_msbuild()
        self.cmake_path = self._find_cmake()

    def _find_msbuild(self) -> Optional[str]:
        """Locates MSBuild executable on Windows."""
        standard_paths = [
            r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
            r"C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe",
        ]
        for p in standard_paths:
            if os.path.isfile(p):
                return p

        # Query vswhere
        vswhere = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
        if os.path.isfile(vswhere):
            try:
                out = subprocess.check_output(
                    [vswhere, "-latest", "-requires", "Microsoft.Component.MSBuild", "-find", r"MSBuild\**\Bin\amd64\MSBuild.exe"],
                    text=True
                ).strip()
                if out and os.path.isfile(out.splitlines()[0]):
                    return out.splitlines()[0]
            except Exception:
                pass

        return shutil.which("msbuild")

    def _find_cmake(self) -> Optional[str]:
        """Locates CMake executable."""
        standard_paths = [
            r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
            r"C:\Program Files\CMake\bin\cmake.exe",
        ]
        for p in standard_paths:
            if os.path.isfile(p):
                return p
        return shutil.which("cmake")

    def build_target(
        self,
        target_name: str,
        source_dir: Path,
        commit_hash: str,
        force_rebuild: bool = False
    ) -> Path:
        """
        Builds Release x64 binary for the specified target source.
        Returns path to the output executable.
        """
        out_target_dir = self.bin_cache_dir / target_name
        out_target_dir.mkdir(parents=True, exist_ok=True)
        exe_path = out_target_dir / "JPEGView.exe"

        # Check if already built for current commit
        hash_file = out_target_dir / ".commit_hash"
        if exe_path.exists() and not force_rebuild and target_name != "current":
            if hash_file.exists():
                with open(hash_file, "r") as f:
                    if f.read().strip() == commit_hash:
                        print(f"[+] Reusing cached binary for '{target_name}' ({commit_hash[:8]})")
                        return exe_path

        # If current working copy and build/bin/Release/JPEGView.exe exists and is fresh, copy directly
        if target_name == "current":
            root_release_exe = self.repo_root / "build" / "bin" / "Release" / "JPEGView.exe"
            if root_release_exe.exists() and not force_rebuild:
                shutil.copy2(root_release_exe, exe_path)
                # Also copy WICLoader.dll if present
                wic_dll = self.repo_root / "build" / "bin" / "Release" / "WICLoader.dll"
                if wic_dll.exists():
                    shutil.copy2(wic_dll, out_target_dir / "WICLoader.dll")
                print(f"[+] Copied current working tree binary from build/bin/Release/ to {exe_path}")
                return exe_path

        print(f"[*] Building Release x64 binary for '{target_name}'...")
        start_t = time.perf_counter()

        target_build_dir = self.build_cache_dir / target_name
        target_build_dir.mkdir(parents=True, exist_ok=True)

        has_cmake = (source_dir / "CMakeLists.txt").exists()
        has_sln = (source_dir / "src" / "JPEGView.sln").exists() or (source_dir / "JPEGView.sln").exists()

        if has_cmake and self.cmake_path:
            # 1. CMake Configure
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

            # 2. CMake Build
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

            # Locate built binary
            built_exe = target_build_dir / "bin" / "Release" / "JPEGView.exe"
            if not built_exe.exists():
                # Search recursively
                candidates = list(target_build_dir.glob("**/JPEGView.exe"))
                if candidates:
                    built_exe = candidates[0]

            if not built_exe.exists():
                raise FileNotFoundError(f"Could not locate JPEGView.exe after building in {target_build_dir}")

            shutil.copy2(built_exe, exe_path)
            # Copy dependent DLLs
            for dll in built_exe.parent.glob("*.dll"):
                shutil.copy2(dll, out_target_dir / dll.name)

        elif self.msbuild_path:
            # Check for specific vcxproj
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

            # 1. Build WICLoader if present
            if wic_proj.exists():
                print(f"    Building WICLoader.vcxproj...")
                res_wic = subprocess.run([self.msbuild_path, str(wic_proj)] + common_msbuild_flags, capture_output=True, text=True)
                if res_wic.returncode != 0:
                    print(f"[-] WICLoader MSBuild error:\n{res_wic.stdout}\n{res_wic.stderr}")

            # 2. Build JPEGView.vcxproj
            print(f"    Building JPEGView.vcxproj...")
            res = subprocess.run([self.msbuild_path, str(jpeg_proj)] + common_msbuild_flags, capture_output=True, text=True)
            if res.returncode != 0:
                print(f"[-] MSBuild error:\n{res.stdout}\n{res.stderr}")
                raise RuntimeError(f"MSBuild failed for {target_name}: {res.stderr}")

            # Look for output
            candidates = list(source_dir.glob("**/JPEGView.exe"))
            if not candidates:
                raise FileNotFoundError(f"Could not find built JPEGView.exe in {source_dir}")

            built_exe = candidates[0]
            shutil.copy2(built_exe, exe_path)
            for dll in source_dir.glob("**/*.dll"):
                shutil.copy2(dll, out_target_dir / dll.name)

        else:
            raise RuntimeError("Neither CMake nor MSBuild was found on the system.")

        with open(hash_file, "w") as f:
            f.write(commit_hash)

        elapsed = time.perf_counter() - start_t
        print(f"[+] Build successful for '{target_name}' in {elapsed:.2f}s -> {exe_path}")
        return exe_path


if __name__ == "__main__":
    b = TargetBuilder()
    print("MSBuild:", b.msbuild_path)
    print("CMake:", b.cmake_path)
