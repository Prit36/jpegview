"""Compare complete production HEIF pixels (including alpha) against a saved build."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--before', type=Path, required=True, help='Saved production-object probe executable')
    ap.add_argument('--after', type=Path, required=True)
    ap.add_argument('--data', type=Path, action='append', required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    # Keep correctness-test children off the rest of the host's CPUs too.
    kernel = C.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = C.c_void_p
    kernel.SetProcessAffinityMask.argtypes = [C.c_void_p, C.c_size_t]
    if not kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(), 0xf00):
        raise C.WinError(C.get_last_error())
    rows = []
    with tempfile.TemporaryDirectory(prefix='jpegview-heif-verify-') as temp:
        for image in sorted(p.resolve() for directory in args.data for p in directory.iterdir()
                            if p.suffix.lower() in {'.heic', '.heif', '.hif'}):
            row = {'file': str(image), 'pixels': {}, 'logs': {}}
            for target, exe in [('before', args.before.resolve()), ('after', args.after.resolve())]:
                prefix = Path(temp) / target
                result = subprocess.run([str(exe), str(image), str(prefix)], cwd=exe.parent,
                                        capture_output=True, text=True, timeout=120, check=True)
                row['logs'][target] = result.stdout + result.stderr
                row['pixels'][target] = {mode: digest(Path(f'{prefix}.{mode}.bgra'))
                                          for mode in ['cpu', 'warm']}
            row['identical'] = row['pixels']['before'] == row['pixels']['after']
            rows.append(row)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(rows, indent=2), encoding='utf-8')
            print(('PASS' if row['identical'] else 'FAIL'), image.name, flush=True)
    failures = sum(not row['identical'] for row in rows)
    print(f'{len(rows)-failures}/{len(rows)} unchanged full software/hardware pixel outputs')
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
