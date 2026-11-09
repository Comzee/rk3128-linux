#!/usr/bin/env python3
"""#730 independent quality report; no device access. Usage: ... DIRECTORY [FIXTURE-DIRECTORY].
Reads generated current/rga PPM pairs, optional Hantro .planes captures, and
JSONL records. Zero pixel differences is the existing conversion/filter contract.
Colour/model statistics explain differences; they never relax that contract.
"""
from pathlib import Path
import json
import sys
import numpy as np
from PIL import Image

root = Path(sys.argv[1])
fixtures = Path(sys.argv[2]) if len(sys.argv) > 2 else None

def pixels(path):
    return np.asarray(Image.open(path), dtype=np.int16)

def difference(a, b):
    assert a.shape == b.shape, (a.shape, b.shape)
    d = np.abs(a.astype(np.float64) - b)
    mse = np.mean(d * d)
    return dict(max=int(d.max()), mae=round(float(d.mean()), 6),
                psnr_db=round(float(10 * np.log10(255 ** 2 / mse)), 4) if mse else None,
                changed=int(np.count_nonzero(d)), over3=int(np.count_nonzero(d > 3)),
                samples=int(d.size), exact=not bool(d.any()))

def orient(rgb, o):
    # Pillow implements EXIF transpose independently of the C helper.
    methods = [None, Image.Transpose.FLIP_LEFT_RIGHT, Image.Transpose.ROTATE_180,
               Image.Transpose.FLIP_TOP_BOTTOM, Image.Transpose.TRANSPOSE,
               Image.Transpose.ROTATE_270, Image.Transpose.TRANSVERSE, Image.Transpose.ROTATE_90]
    im = Image.fromarray(np.uint8(rgb))
    return np.asarray(im.transpose(methods[o-1]) if o != 1 else im).astype(np.int16)

def box(rgb, dw, dh):
    # Independent summed-area implementation of the production integer box definition.
    sh, sw = rgb.shape[:2]
    integral = np.pad(rgb.astype(np.int64).cumsum(0).cumsum(1), ((1,0),(1,0),(0,0)))
    x0 = np.arange(dw) * sw // dw
    x1 = np.maximum((np.arange(dw)+1) * sw // dw, x0+1)
    y0 = np.arange(dh) * sh // dh
    y1 = np.maximum((np.arange(dh)+1) * sh // dh, y0+1)
    sums = (integral[y1[:,None],x1] - integral[y0[:,None],x1] -
            integral[y1[:,None],x0] + integral[y0[:,None],x0])
    n = (y1-y0)[:,None] * (x1-x0)
    return ((sums + n[:,:,None]//2) // n[:,:,None]).astype(np.int16)

rows = []
for cur in sorted(root.glob('*-current.ppm')):
    name = cur.name.removesuffix('-current.ppm')
    candidate = root / (name + '-rga.ppm')
    if not candidate.exists():
        continue
    a, b = pixels(cur), pixels(candidate)
    r = dict(fixture=name, photos_comparison=difference(a,b))
    ref = root / (name + '-software.ppm')
    if ref.exists():
        r['stb_vs_hantro'] = difference(a,pixels(ref))
        r['stb_vs_rga'] = difference(pixels(ref),b)
    if fixtures:
        source = fixtures / (name + '.jpg')
        for suffix in ('-square', '-thumb', '-portrait'):
            if not source.exists() and name.endswith(suffix):
                source = fixtures / (name.removesuffix(suffix) + '.jpg')
        if source.exists():
            with Image.open(source) as im:
                o = im.getexif().get(274, 1)
                raw = np.asarray(im.convert('RGB'), dtype=np.int16)
            uh, uw = a.shape[:2]
            dw, dh = (uh, uw) if o >= 5 else (uw, uh)
            decoded = orient(box(raw, dw, dh), o)
            r['pillow_jpeg_vs_current'] = difference(decoded, a)
            r['pillow_jpeg_vs_rga'] = difference(decoded, b)
    rows.append(r)

# Whole-slide EXIF outputs at equal pre-orientation dimensions are judged against Pillow.
base = root / 'orient-1-current.ppm'
if base.exists():
    for o in range(1,9):
        for mode in ('current','rga'):
            p = root / f'orient-{o}-{mode}.ppm'
            if p.exists():
                # Compare RGA geometry with RGA orientation=1: keeps its CSC/filter differences separate.
                source = pixels(root / f'orient-1-{mode}.ppm')
                rows.append(dict(fixture=f'orientation-{o}-{mode}', pillow_transform=difference(orient(source,o),pixels(p))))

for path in sorted(root.glob('*.planes')):
    with path.open('rb') as f:
        w,h,stride,padded_h,vs,length = map(int, f.readline().split())
        raw = np.frombuffer(f.read(), dtype=np.uint8)
    assert len(raw) == length
    yn = stride * padded_h
    yy = raw[:yn].reshape(padded_h,stride)[:h,:w].astype(np.float64)
    uv = raw[yn:yn+yn*2//vs].reshape(padded_h//vs,stride)
    u = uv[:,0::2].repeat(vs,0).repeat(2,1)[:h,:w].astype(np.float64)-128
    v = uv[:,1::2].repeat(vs,0).repeat(2,1)[:h,:w].astype(np.float64)-128
    # ITU BT.601 full-range matrix, independent floating-point CSC and clipping.
    ref = np.clip(np.floor(np.stack((yy+1.402*v, yy-.344136*u-.714136*v, yy+1.772*u),2)+.5),0,255)
    name = path.stem
    dest = root / (name+'-rga.ppm')
    if dest.exists() and pixels(dest).shape == ref.shape:
        rows.append(dict(fixture=name, independent_601_nearest_chroma=difference(ref,pixels(dest))))
    # Current full-size upright RGB allows exact independent box tests for square/thumb cases.
    full = root / (name+'-current.ppm')
    if full.exists() and pixels(full).shape == ref.shape:
        for target in ('square','thumb'):
            small = root / (name+'-'+target+'-current.ppm')
            if small.exists():
                b = pixels(small)
                rows.append(dict(fixture=name+'-'+target, independent_integer_box=difference(box(pixels(full),b.shape[1],b.shape[0]),b)))
print(json.dumps(rows, indent=2))
