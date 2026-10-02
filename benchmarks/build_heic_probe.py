"""Build the pixel probe using the exact objects and libraries of a Release app.

Usage: python benchmarks/build_heic_probe.py benchmarks/.cache/build/current
The console probe is placed beside JPEGView.exe so it uses the same DLLs/config.
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
build = Path(sys.argv[1]).resolve()
vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*",
                              "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                              "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
env_text = subprocess.check_output(f'cmd /d /s /c ""{vcvars}" >nul && set"', text=True)
env = {k.upper(): v for k, v in os.environ.items()}
env.update((k.upper(), v) for k, v in (line.split("=", 1) for line in env_text.splitlines()
                                     if "=" in line and not line.startswith("=")))
binary = build / "bin/Release"
obj = binary / "heic_decode_probe.obj"
exe = binary / "heic_decode_probe.exe"
includes = [repo / "src/JPEGView", repo / "src/JPEGView/libheif/include"]
subprocess.run([shutil.which("cl", path=env["PATH"]), "/nologo", "/c", "/O2", "/MD", "/EHsc", "/std:c++latest",
                *[f"/I{p}" for p in includes], f"/Fo{obj}",
                str(repo / "benchmarks/heic_decode_probe.cpp")], env=env, check=True)
project = build / "src/JPEGView"
log = project / "JPEGView.dir/Release/JPEGView.tlog/link.command.1.tlog"
command = log.read_text(encoding="utf-16").splitlines()[1:]
args = re.findall(r'(?:[^\s"]|"[^"]*")+', " ".join(command))
args = [a.replace('"', '') for a in args]
args = [a for a in args if not a.upper().startswith(("/OUT:", "/PDB:", "/IMPLIB:",
                                                    "/LTCGOUT:", "/SUBSYSTEM:"))]
if len(sys.argv) > 2:
    # Optional isolated software snapshot for pixel equivalence, never for timing.
    snapshot = Path(sys.argv[2]).resolve()
    snapshot_obj = binary / "HEIFWrapper_legacy_software.obj"
    exe = binary / "heic_decode_probe_legacy_software.exe"
    include_dirs = includes + [repo / "deps/atlmfc/atlmfc/include", repo / "deps/WTL-sf/Include"]
    subprocess.run([shutil.which("cl", path=env["PATH"]), "/nologo", "/c", "/O2", "/MD",
                    "/EHsc", "/std:c++latest", "/openmp", "/arch:AVX2", "/fp:fast", "/GL",
                    "/DUNICODE", "/D_UNICODE", *[f"/I{p}" for p in include_dirs],
                    f"/Fo{snapshot_obj}", str(snapshot)], env=env, check=True)
    args = [str(snapshot_obj) if a.upper().endswith("\\HEIFWRAPPER.OBJ") else a for a in args]
subprocess.run([shutil.which("link", path=env["PATH"]), *args, str(obj), f"/OUT:{exe}",
                "/SUBSYSTEM:CONSOLE"], cwd=project, env=env, check=True)
print(exe)
