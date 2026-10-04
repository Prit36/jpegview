"""Compile real before/after OCR implementations together and measure paired runs.

Usage: python benchmarks/run_ocr_benchmark.py [--baseline-ref HEAD] [--micro-only]
       python benchmarks/run_ocr_benchmark.py --data-dir benchmarks/random_data --runs 2
Requires MSVC and the Windows SDK; recognition requires an installed OCR language.
Generated artifacts stay in benchmarks/.cache/ocr. No network or image downloads.
"""
import argparse
import hashlib
import json
import os
import platform
import re
from pathlib import Path
import shutil
import subprocess

repo = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument("--baseline-ref", default="HEAD")
parser.add_argument("--micro-only", action="store_true")
parser.add_argument("--data-dir", type=Path, help="Measure every file using the app's actual loader")
parser.add_argument("--app-build", type=Path, default=repo / "build_x64", help="Existing x64 Release app build")
parser.add_argument("--runs", type=int, default=1, help="Independent full measurement runs")
parser.add_argument("--validate-only", action="store_true", help="Quick per-file correctness/shutdown diagnostic")
args = parser.parse_args()
if args.runs < 1:
    parser.error("--runs must be positive")
if args.data_dir and args.micro_only:
    parser.error("--micro-only and --data-dir cannot be combined")
if args.validate_only and not args.data_dir:
    parser.error("--validate-only requires --data-dir")
build = repo / ("benchmarks/.cache/ocr-files" if args.data_dir else "benchmarks/.cache/ocr")
build.mkdir(parents=True, exist_ok=True)
metadata = {"baseline_ref": args.baseline_ref,
            "baseline_commit": subprocess.check_output(["git", "rev-parse", args.baseline_ref], cwd=repo, text=True).strip(),
            "sha256": {}}
metadata["os"] = platform.platform()
metadata["cpu"] = platform.processor()
for name in ("OcrEngine.h", "OcrEngine.cpp"):
    old = subprocess.check_output(["git", "show", f"{args.baseline_ref}:src/JPEGView/{name}"], cwd=repo)
    new = (repo / "src/JPEGView" / name).read_bytes()
    metadata["sha256"][name] = {"before": hashlib.sha256(old).hexdigest(),
                                "after": hashlib.sha256(new).hexdigest()}
    baseline_name = "OcrBaseline.h" if name.endswith(".h") else name
    (build / baseline_name).write_text(old.decode("utf-8").replace("namespace Ocr {", "namespace OcrBaseline {")
                                     .replace('#include "OcrEngine.h"', '#include "OcrBaseline.h"'), encoding="utf-8")
vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                              "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
env_text = subprocess.check_output(f'cmd /d /s /c ""{vcvars}" >nul && set"', text=True)
env = {k.upper(): v for k, v in os.environ.items()}
env.update((k.upper(), v) for k, v in (line.split("=", 1) for line in env_text.splitlines()
                                     if "=" in line and not line.startswith("=")))
metadata["msvc_tools_version"] = env.get("VCTOOLSVERSION")
metadata["windows_sdk_version"] = env.get("WINDOWSSDKVERSION")
# Match the application's MSVC Release optimization and floating-point settings.
flags = ["/nologo", "/c", "/Ox", "/Oi", "/Ot", "/Ob3", "/fp:fast", "/GA", "/GT",
         "/GS-", "/Gy", "/Gw", "/GL", "/Oy", "/GF", "/Qpar", "/MD", "/EHsc",
         "/std:c++latest", "/utf-8", "/W4", "/arch:AVX2", "/openmp"]
objects = []
includes = [build]
if args.data_dir:
    includes += [repo / "src/JPEGView", repo / "deps/atlmfc/atlmfc/include", repo / "deps/WTL-sf/Include"]
harness = "ocr_file_benchmark.cpp" if args.data_dir else "ocr_benchmark.cpp"
for source, name in [(build / "OcrEngine.cpp", "before"),
                     (repo / "src/JPEGView/OcrEngine.cpp", "after"),
                     (repo / "benchmarks" / harness, "harness")]:
    obj = build / f"{name}.obj"
    subprocess.run([shutil.which("cl", path=env["PATH"]), *flags, "/Zi", "/DUNICODE", "/D_UNICODE",
                    *[f"/I{path}" for path in includes], f"/Fo{obj}", str(source)],
                   cwd=build, env=env, check=True)
    objects.append(str(obj))
exe = build / "ocr_benchmark.exe"
link_cwd = build
link_args = ["/nologo", "/LTCG", "/OPT:REF", "/OPT:ICF", "windowsapp.lib", "gdi32.lib", "user32.lib", "ole32.lib"]
if args.data_dir:
    app_build = args.app_build.resolve()
    link_cwd = app_build / "src/JPEGView"
    command_log = link_cwd / "JPEGView.dir/Release/JPEGView.tlog/link.command.1.tlog"
    command = " ".join(command_log.read_text(encoding="utf-16").splitlines()[1:])
    link_args = [a.replace('"', '') for a in re.findall(r'(?:[^\s"]|"[^"]*")+', command)]
    link_args = [a for a in link_args if not a.upper().startswith(("/OUT:", "/PDB:", "/IMPLIB:",
                                                                 "/LTCGOUT:", "/SUBSYSTEM:"))
                 and not a.upper().endswith("\\OCRENGINE.OBJ")]
    # Copy runtime dependencies; neither app binaries nor input images are modified.
    for dll in (app_build / "bin/Release").glob("*.dll"):
        shutil.copy2(dll, build / dll.name)
    (build / "ocr_benchmark.ini").write_text("[JPEGView]\nStoreToEXEPath=true\nAutoRotateEXIF=true\n", encoding="utf-8")
    metadata["data_dir"] = str(args.data_dir.resolve())
    metadata["corpus"] = [{"file": str(p.relative_to(args.data_dir.resolve())),
                           "bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                          for p in sorted(args.data_dir.resolve().rglob("*")) if p.is_file()]
subprocess.run([shutil.which("link", path=env["PATH"]), *link_args, *objects,
                f"/OUT:{exe}", "/DEBUG", f"/PDB:{build / 'ocr_benchmark.pdb'}", "/SUBSYSTEM:CONSOLE"], cwd=link_cwd, env=env, check=True)
metadata["compiler_flags"] = [*flags, "/Zi", "/DUNICODE", "/D_UNICODE"]
(build / "metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
command = [str(exe), *([str(args.data_dir.resolve()), "/ini", "ocr_benchmark.ini"] if args.data_dir else
                      ["--micro-only"] if args.micro_only else []), *(["--validate-only"] if args.validate_only else [])]
for run in range(1, args.runs + 1):
    print(f"Measurement run {run}/{args.runs}", flush=True)
    # Stream progress to the user and preserve it; summaries/raw samples remain CSV.
    with (build / "results.csv").open("w", encoding="utf-8") as output:
        process = subprocess.Popen(command, cwd=build, stdout=output, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8", errors="replace")
        messages = []
        for line in process.stderr:
            messages.append(line)
            print(line, end="", flush=True)
        exit_code = process.wait()
    summary = (build / "results.csv").read_text(encoding="utf-8")
    print(summary, end="")
    (build / "validation.txt").write_text("".join(messages), encoding="utf-8")
    metadata["run"] = run
    metadata["run_exit_code"] = exit_code
    metadata["command"] = command
    (build / "metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    for artifact in ("results.csv", "samples.csv", "metadata.json", "validation.txt"):
        shutil.copy2(build / artifact, build / f"run{run}-{artifact}")
    if exit_code:
        raise subprocess.CalledProcessError(exit_code, command)
