# Compares pngbench.exe dumps against Pillow (independent decoder) for every
# corpus file and frame.
import os, sys, subprocess
from PIL import Image

EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pngbench.exe')
CORPUS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'corpus')
TMP = os.path.join(CORPUS, 'tmp')
os.makedirs(TMP, exist_ok=True)

def to_bgra(im):
    return im.convert('RGBA').tobytes('raw', 'BGRA')

# Files FastPng intentionally rejects (handled by GDI+ in the app):
EXPECTED_FALLBACK = {'pal1.png', 'pal2.png', 'pal4.png', 'pal4_i.png',
                     'pal8.png', 'pal8_trns.png',
                     'gray16.png', 'gray16_i.png', 'gray8_trns.png',
                     'rgb8_trns_key.png', 'apng_bg_over.png', 'apng_partial.png',
                     'apng_prev_mix.png', 'apng_src_src.png', 'apng_hidden.png'}

fails = 0; total = 0
for fn in sorted(os.listdir(CORPUS)):
    if not fn.endswith('.png'): continue
    path = os.path.join(CORPUS, fn)
    im = Image.open(path)
    n = getattr(im, 'n_frames', 1)
    animated = n > 1 or getattr(im, 'is_animated', False)
    # our reader reports displayed frames; hidden defaults are skipped by both
    for i in range(n):
        im.seek(i)
        expected = to_bgra(im)
        out = os.path.join(TMP, f'{fn}.{i}.expected.bin')
        open(out, 'wb').write(expected)
        args = [EXE, path]
        if animated:
            args += ['--dumpframes', str(i + 1), os.path.join(TMP, fn + '.actual')]
            # we compare the LAST dumped frame (= decode cycle up to frame i)
            actual_path = f"{os.path.join(TMP, fn)}.actual.{i}.bin"
        else:
            actual_path = os.path.join(TMP, fn + '.actual.bin')
            args += ['--dump', actual_path]
        r = subprocess.run(args, capture_output=True, text=True)
        total += 1
        ok = False
        msg = ''
        if r.returncode != 0:
            msg = 'exe rc=%d %s' % (r.returncode, r.stdout.strip()[:60] + r.stderr.strip()[:60])
        elif not os.path.exists(actual_path):
            msg = 'no dump produced: ' + r.stdout.strip()[:80]
        else:
            a = open(actual_path, 'rb').read()
            if len(a) != len(expected):
                msg = f'size {len(a)} != {len(expected)}'
            else:
                d = next((j for j in range(len(a)) if a[j] != expected[j]), -1)
                ok = d == -1
                if not ok:
                    px = d // 4; ch = d % 4
                    msg = f'first diff at pixel ({px % 96},{px // 96}) ch{ch}: got {a[d]} want {expected[d]}'
        if fn in EXPECTED_FALLBACK and 'exe rc=' in msg:
            ok = True
            msg = 'fallback to GDI+ (expected)'
        status = 'PASS' if ok else 'FAIL'
        if not ok: fails += 1
        label = f'{fn}#{i}' if animated else fn
        print(f'[{status}] {label} {msg}')
print(f'\n{total - fails}/{total} passed, {fails} failed')
sys.exit(1 if fails else 0)
