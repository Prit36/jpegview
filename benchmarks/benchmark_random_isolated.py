"""Paired, resource-bounded Windows whole-application benchmark.

Starts each child suspended, assigns a Job Object (CPU hard cap, affinity,
3 GiB committed-memory budget), then resumes. No polling of child memory.
These are budgets, NOT exclusive CPU/physical RAM reservations. Host load,
clock scaling and filesystem cache can still affect wall time. CPU time and
alternating A/B order help detect that noise. Requires Windows Python.
"""
from __future__ import annotations
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics as S
import subprocess
import time
import _winapi

class Basic(C.Structure):
    _fields_ = [('process_time', C.c_int64), ('job_time', C.c_int64),
                ('flags', W.DWORD), ('min_ws', C.c_size_t), ('max_ws', C.c_size_t),
                ('active', W.DWORD), ('affinity', C.c_size_t),
                ('priority', W.DWORD), ('scheduling', W.DWORD)]
class IO(C.Structure):
    _fields_ = [(n, C.c_uint64) for n in ['read_ops','write_ops','other_ops','read_bytes','write_bytes','other_bytes']]
class Extended(C.Structure):
    _fields_ = [('basic', Basic), ('io', IO), ('process_limit', C.c_size_t),
                ('job_limit', C.c_size_t), ('peak_process', C.c_size_t), ('peak_job', C.c_size_t)]
class CPU(C.Structure):
    _fields_ = [('flags', W.DWORD), ('rate', W.DWORD)]
class Memory(C.Structure):
    _fields_ = [('cb', W.DWORD), ('faults', W.DWORD)] + [(n, C.c_size_t) for n in
        ['peak_ws','ws','peak_paged','paged','peak_nonpaged','nonpaged','pagefile','peak_pagefile','private']]

K = C.WinDLL('kernel32', use_last_error=True)
P = C.WinDLL('psapi', use_last_error=True)
for name, args, result in [
    ('CreateJobObjectW', [C.c_void_p, W.LPCWSTR], W.HANDLE),
    ('SetInformationJobObject', [W.HANDLE, C.c_int, C.c_void_p, W.DWORD], W.BOOL),
    ('QueryInformationJobObject', [W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.c_void_p], W.BOOL),
    ('AssignProcessToJobObject', [W.HANDLE, W.HANDLE], W.BOOL),
    ('SetProcessAffinityMask', [W.HANDLE, C.c_size_t], W.BOOL),
    ('ResumeThread', [W.HANDLE], W.DWORD),
    ('GetProcessTimes', [W.HANDLE, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p], W.BOOL),
    ('CloseHandle', [W.HANDLE], W.BOOL),
]:
    f = getattr(K, name); f.argtypes = args; f.restype = result
P.GetProcessMemoryInfo.argtypes = [W.HANDLE, C.c_void_p, W.DWORD]
P.GetProcessMemoryInfo.restype = W.BOOL

def check(ok):
    if not ok: raise C.WinError(C.get_last_error())

def run(exe: Path, image: Path, output: Path, mask: int, rate: int, gib: float, nav=0):
    output.unlink(missing_ok=True)
    job = K.CreateJobObjectW(None, None)
    check(job)
    hp = ht = None
    try:
        limit = Extended()
        limit.basic.flags = 0x2000 | 0x200 | 0x10  # kill-on-close, job memory, affinity
        limit.basic.affinity = mask
        limit.job_limit = int(gib * 1024**3)
        check(K.SetInformationJobObject(job, 9, C.byref(limit), C.sizeof(limit)))
        cpu = CPU(1 | 4, rate)  # enable, hard cap in 1/100 percent of all host CPUs
        check(K.SetInformationJobObject(job, 15, C.byref(cpu), C.sizeof(cpu)))
        cmd = [str(exe), str(image), f'/benchmark:{output}', '/benchmark_exit']
        if nav: cmd.append(f'/benchmark_nav:{nav}')
        hp, ht, pid, tid = _winapi.CreateProcess(str(exe), subprocess.list2cmdline(cmd),
            None, None, False, 0x4 | 0x4000, None, str(exe.parent), subprocess.STARTUPINFO())
        check(K.AssignProcessToJobObject(job, hp))
        check(K.SetProcessAffinityMask(hp, mask))
        start = time.perf_counter()
        if K.ResumeThread(ht) == 0xffffffff: raise C.WinError(C.get_last_error())
        if _winapi.WaitForSingleObject(hp, 60000) != 0:
            raise TimeoutError(f'No successful app exit: {image}')
        wall = (time.perf_counter() - start) * 1000
        code = _winapi.GetExitCodeProcess(hp)
        if code != 0 or not output.exists():
            raise RuntimeError(f'{image.name}: exit={code}, telemetry={output.exists()}')
        telemetry = json.loads(output.read_text(encoding='utf-8'))
        if telemetry['process_start_to_first_paint_ms'] <= 0:
            raise RuntimeError('Missing first-paint measurement')
        created, exited, kernel, user = (C.c_uint64() for _ in range(4))
        check(K.GetProcessTimes(hp, C.byref(created), C.byref(exited), C.byref(kernel), C.byref(user)))
        memory = Memory(); memory.cb = C.sizeof(memory)
        check(P.GetProcessMemoryInfo(hp, C.byref(memory), memory.cb))
        check(K.QueryInformationJobObject(job, 9, C.byref(limit), C.sizeof(limit), None))
        telemetry.update(cpu_ms=(kernel.value + user.value)/10000,
            lifecycle_wall_ms=wall, peak_commit_mb=limit.peak_job/1024**2,
            peak_working_set_mb=max(telemetry['peak_working_set_mb'], memory.peak_ws/1024**2),
            exit_code=code)
        return telemetry
    finally:
        # Close the job first: also kills a timed-out child before releasing handles.
        K.CloseHandle(job)
        if ht: K.CloseHandle(ht)
        if hp: K.CloseHandle(hp)
        output.unlink(missing_ok=True)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--before', type=Path, required=True)
    ap.add_argument('--after', type=Path)
    ap.add_argument('--data', type=Path, default=Path(__file__).parent/'random_data')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--pattern', default='*', help='Optional file glob for focused format profiling')
    ap.add_argument('--no-navigation', action='store_true', help='Measure individual images only')
    ap.add_argument('--iterations', type=int, default=15)
    ap.add_argument('--warmups', type=int, default=2)
    ap.add_argument('--mask', type=lambda s:int(s,0), default=0xf00)
    ap.add_argument('--cpu-rate', type=int, default=3333)
    ap.add_argument('--memory-gib', type=float, default=3)
    args = ap.parse_args()
    if args.iterations < 1 or args.warmups < 0 or args.mask <= 0 or not 1 <= args.cpu_rate <= 10000 or args.memory_gib <= 0:
        ap.error('Invalid iterations, warmups, affinity, CPU rate or memory budget')
    targets = {'before':args.before.resolve()}
    if args.after: targets['after'] = args.after.resolve()
    extensions = {'.jpg','.jpeg','.png','.heic','.heif','.avif','.webp','.bmp','.tif','.tiff','.gif','.jxl','.qoi'}
    images = sorted(p.resolve() for p in args.data.iterdir() if p.is_file() and p.suffix.lower() in extensions and p.match(args.pattern))
    if not images: ap.error('No supported image files in data directory')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = {'environment': {'platform':platform.platform(), 'logical_cpus':os.cpu_count(),
        'affinity_mask':hex(args.mask), 'cpu_rate':args.cpu_rate, 'memory_budget_gib':args.memory_gib,
        'iterations':args.iterations, 'warmups':args.warmups, 'started':time.strftime('%Y-%m-%d %H:%M:%S'),
        'priority':'BELOW_NORMAL_PRIORITY_CLASS',
        'config_sha256': {n:hashlib.sha256((p.parent/'JPEGView.ini').read_bytes()).hexdigest() for n,p in targets.items()},
        'source_commit':subprocess.check_output(['git','rev-parse','HEAD'], text=True).strip(),
        'isolation':'Job Object limits, not exclusive CPU or physical RAM reservation',
        'binary_sha256': {n:hashlib.sha256(p.read_bytes()).hexdigest() for n,p in targets.items()},
        'runtime_dll_sha256': {n:{dll.name:hashlib.sha256(dll.read_bytes()).hexdigest()
            for dll in sorted(p.parent.iterdir()) if dll.suffix.lower() == '.dll'}
            for n,p in targets.items()}},
        'files': {p.name:{'size':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in images},
        'samples': [], 'failures': [], 'summary': {}}
    telemetry = args.output.resolve().with_suffix('.telemetry.json')
    # Include a complete mixed-format traversal, exercising read-ahead and cache lifetime.
    cases = [(p.name, p, 0) for p in images]
    if not args.no_navigation:
        cases.append(('folder_navigation', images[0], len(images)))
    def measure(name, case, image, nav, iteration):
        for attempt in range(3):
            try:
                return run(targets[name],image,telemetry,args.mask,args.cpu_rate,args.memory_gib,nav)
            except (RuntimeError, TimeoutError) as exc:
                result['failures'].append({'target':name, 'case':case,
                    'iteration':iteration,'attempt':attempt,'error':str(exc)})
                args.output.write_text(json.dumps(result,indent=2),encoding='utf-8')
                print(f'FAILED {name} {case}: {exc}', flush=True)
        raise RuntimeError(f'Three failed attempts: {name} {case}')
    for case, image, nav in cases:
        for warmup in range(args.warmups):
            for name in targets: measure(name,case,image,nav,-1-warmup)
        for iteration in range(args.iterations):
            order = list(targets)
            if iteration % 2: order.reverse()
            for name in order:
                sample = measure(name,case,image,nav,iteration)
                result['samples'].append({'target':name,'case':case,'iteration':iteration,**sample})
            time.sleep(0.05)
        print(case, flush=True)
        args.output.write_text(json.dumps(result,indent=2),encoding='utf-8')
    keys = ['process_start_to_first_paint_ms','first_image_load_ms','cpu_ms',
            'peak_working_set_mb','peak_commit_mb','avg_frame_time_ms','lifecycle_wall_ms']
    for name in targets:
        result['summary'][name] = {}
        for case,_,_ in cases:
            rows = [s for s in result['samples'] if s['target']==name and s['case']==case]
            result['summary'][name][case] = {k:{'median':S.median(s[k] for s in rows),
                'min':min(s[k] for s in rows),'max':max(s[k] for s in rows),
                'mean':S.mean(s[k] for s in rows)} for k in keys}
    args.output.write_text(json.dumps(result,indent=2),encoding='utf-8')

if __name__ == '__main__': main()
