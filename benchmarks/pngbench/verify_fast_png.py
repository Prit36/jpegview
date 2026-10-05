"""Verify production FastPng through a test DLL against Pillow and known pixels.

Build fastpng_test_bridge.cpp + src/JPEGView/FastPng.cpp with MSVC /LD /O2
/arch:AVX2, linking libdeflate.lib. Pass the DLL path as the first argument.
Includes every predictor, RGB/RGBA, SIMD tails, overlap, malformed filters,
and real PNGs in random_data plus existing fallback fixtures.
"""
import ctypes as C
import io
from pathlib import Path
import random
import struct
import sys
import zlib
from PIL import Image

lib = C.CDLL(str(Path(sys.argv[1]).resolve()))
lib.decode_png.argtypes = [C.c_void_p, C.c_size_t, C.POINTER(C.c_int), C.POINTER(C.c_int)]
lib.decode_png.restype = C.c_void_p
lib.free_png.argtypes = [C.c_void_p]

def decode(data):
    buf = C.create_string_buffer(data)
    w, h = C.c_int(), C.c_int()
    p = lib.decode_png(buf, len(data), C.byref(w), C.byref(h))
    if not p: return None
    try: return w.value, h.value, C.string_at(p, w.value*h.value*4)
    finally: lib.free_png(p)

def chunk(kind, data):
    return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))

def paeth(a,b,c):
    p = a+b-c
    ds = [abs(p-a),abs(p-b),abs(p-c)]
    return [a,b,c][ds.index(min(ds))]

def png(w,h,bpp,filters,rng,split=17):
    pixels = bytes(rng.randrange(256) for _ in range(w*h*bpp))
    stride = w*bpp
    raw = bytearray()
    for y in range(h):
        ft = filters[y % len(filters)]; raw.append(ft)
        for x in range(stride):
            i = y*stride+x
            a = pixels[i-bpp] if x>=bpp else 0
            b = pixels[i-stride] if y else 0
            c = pixels[i-stride-bpp] if y and x>=bpp else 0
            predictor = [0,a,b,(a+b)//2,paeth(a,b,c)][min(ft,4)]
            raw.append((pixels[i]-predictor)&255)
    header = struct.pack('>IIBBBBB',w,h,8,2 if bpp==3 else 6,0,0,0)
    compressed = zlib.compress(raw)
    # Many IDAT chunks also exercise the join path.
    idats = b''.join(chunk(b'IDAT',compressed[i:i+split]) for i in range(0,len(compressed),split))
    data = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',header)+idats+chunk(b'IEND',b'')
    expected = Image.frombytes('RGB' if bpp==3 else 'RGBA',(w,h),pixels).convert('RGBA').tobytes('raw','BGRA')
    return data, expected

rng = random.Random(2101)
passed = skipped = 0
for bpp in [3,4]:
    for w in list(range(1,66))+[127,128,129,511,512,513]:
        for filters in [[0],[1],[2],[3],[4],[0,1,2,3,4],[4,3,2,1,0]]:
            for h in [1,2,11]:
                data, expected = png(w,h,bpp,filters,rng)
                result = decode(data)
                assert result == (w,h,expected), (w,h,bpp,filters)
                assert Image.open(io.BytesIO(data)).convert('RGBA').tobytes('raw','BGRA') == expected
                passed += 1
    data,_ = png(33,9,bpp,[0,1,2,3,4,5],rng)
    assert decode(data) is None, 'invalid filter must be rejected without double-free'
    passed += 1
    data, expected = png(129,11,bpp,[0,1,2,3,4],rng,split=1000000)
    assert decode(data) == (129,11,expected), 'single IDAT borrowed input'
    passed += 1
    for w,h,interlace in [(0,11,0),(11,0,0),(65536,11,0),(11,65536,0),(11,11,1)]:
        header = struct.pack('>IIBBBBB',w,h,8,2 if bpp==3 else 6,0,0,interlace)
        malformed = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',header)+chunk(b'IDAT',zlib.compress(b'\0'))+chunk(b'IEND',b'')
        assert decode(malformed) is None
        passed += 1
root = Path(__file__).resolve().parents[1]
files = list((root/'random_data').glob('*.png'))+list((Path(__file__).parent/'test_pngs').glob('*.png'))
for path in files:
    result = decode(path.read_bytes())
    if result is None:
        skipped += 1  # intentionally unsupported; integrated fallback tested separately
        continue
    image = Image.open(path).convert('RGBA')
    assert result == (*image.size,image.tobytes('raw','BGRA')), path.name
    passed += 1
print(f'PASS: {passed} byte-exact comparisons/rejection checks; {skipped} unsupported fallback fixtures')
