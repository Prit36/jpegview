"""
Native Micro-Benchmark Subsystem Runner.
Compiles and invokes the C++ benchmark executable, collecting metrics across test categories.
"""

import os
import sys
import json
import time
import tempfile
import subprocess
from pathlib import Path
from typing import Dict, Any, List, Optional


class MicroBenchmarkRunner:
    """Manages building and execution of native C++ micro-benchmarks."""

    def __init__(self, repo_root: Optional[Path] = None):
        if repo_root is None:
            self.repo_root = Path(__file__).resolve().parent.parent.parent
        else:
            self.repo_root = Path(repo_root)

        self.engine_src_dir = self.repo_root / "benchmarks" / "engine"
        self.engine_bin_dir = self.repo_root / "benchmarks" / ".cache" / "bin" / "Release"
        self.engine_exe = self.engine_bin_dir / "JPEGViewBenchmark.exe"
        self.cmake_path = self._find_cmake()

    def _find_cmake(self) -> Optional[str]:
        paths = [
            r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
            r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        ]
        for p in paths:
            if os.path.isfile(p):
                return p
        import shutil
        return shutil.which("cmake")

    def build_engine(self, force: bool = False) -> Path:
        """Builds JPEGViewBenchmark.exe if missing or forced."""
        if self.engine_exe.exists() and not force:
            return self.engine_exe

        if not self.cmake_path:
            raise RuntimeError("CMake is required to build the micro-benchmark engine.")

        build_dir = self.repo_root / "benchmarks" / ".cache" / "build" / "engine"
        build_dir.mkdir(parents=True, exist_ok=True)

        print("[*] Compiling JPEGView Native Micro-Benchmark Engine...")
        subprocess.run(
            [self.cmake_path, "-B", str(build_dir), "-S", str(self.engine_src_dir), "-A", "x64"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        subprocess.run(
            [self.cmake_path, "--build", str(build_dir), "--config", "Release"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )

        if not self.engine_exe.exists():
            # Search if built in different location
            candidates = list(build_dir.glob("**/JPEGViewBenchmark.exe")) + list(self.repo_root.glob("**/JPEGViewBenchmark.exe"))
            if candidates:
                self.engine_exe = candidates[0]

        return self.engine_exe

    def run_micro_suite(self, iterations: int = 10) -> Dict[str, Any]:
        """Runs the micro-benchmark suite and returns structured metrics."""
        self.build_engine()

        temp_json = Path(tempfile.gettempdir()) / f"micro_bench_{os.getpid()}_{time.time_ns()}.json"

        cmd = [
            str(self.engine_exe),
            "--iterations", str(iterations),
            "--json", str(temp_json)
        ]

        print(f"[*] Running Native Micro-Benchmarks ({iterations} iterations per kernel)...")
        proc = subprocess.run(cmd, capture_output=True, text=True)

        results = {}
        if temp_json.exists():
            try:
                with open(temp_json, "r", encoding="utf-8") as f:
                    results = json.load(f)
                temp_json.unlink(missing_ok=True)
            except Exception:
                pass

        if not results:
            print(f"[-] Output parsing fallback: {proc.stdout}")

        return results


if __name__ == "__main__":
    runner = MicroBenchmarkRunner()
    res = runner.run_micro_suite(iterations=3)
    print("Parsed Micro Results:", len(res.get("micro_benchmarks", [])))
