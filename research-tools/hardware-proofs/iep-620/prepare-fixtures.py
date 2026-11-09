#!/usr/bin/env python3
"""Finite synthetic fixtures, run list and readback preservation; no frame access."""
import argparse
from pathlib import Path
import json
import struct


def prepare(out):
    out.mkdir(parents=True, exist_ok=True)
    inputs = out / 'inputs'
    inputs.mkdir(exist_ok=True)
    cases = []

    def add(name, w, h, srgb, drgb, csc=0, filt=0, alpha=8, radius=3, threshold=255, lut=0, src=None):
        dest = f'results/{name}.raw'
        cases.append(dict(name=name, width=w, height=h, srgb=srgb, drgb=drgb, csc=csc,
                          filter=filt, alpha=alpha, radius=radius, threshold=threshold, lut=lut,
                          src=src or f'inputs/{w}x{h}-gray.rgb', dst=dest))
        return dest

    for w, h in ((16, 16), (68, 36), (640, 480)):
        prefix = f'{w}x{h}'
        for pattern in ('gray', 'color', 'patches', 'ramp', 'noise', 'edge'):
            pixels = bytearray()
            nv12 = bytearray()
            for y in range(h):
                for x in range(w):
                    if pattern == 'gray': r, g, b = 112, 112, 112
                    elif pattern == 'color': r, g, b = 137, 105, 80
                    elif pattern == 'patches':
                        r, g, b = ((144, 96, 80), (80, 144, 96), (96, 80, 144), (112, 112, 112))[x * 4 // w]
                    elif pattern == 'ramp': r = g = b = 32 + 160 * x // w
                    elif pattern == 'noise': r = g = b = 112 + ((37*x + 13*y) % 17) - 8
                    else: r = g = b = 64 if x < w // 2 else 192
                    pixels.extend((b, g, r, 255))
                    nv12.append(112 if pattern in ('gray', 'color', 'patches') else r)
            (inputs / f'{prefix}-{pattern}.rgb').write_bytes(pixels)
            # Constant in-gamut YUV fixtures; textures are monochrome.
            uv = bytes((112, 144) if pattern == 'color' else (128, 128)) * (w*h//4)
            (inputs / f'{prefix}-{pattern}.nv12').write_bytes(nv12 + uv)

        # A1: native RGB copy and CSC-only round trip for all four selectors.
        for pattern in ('gray', 'color', 'patches', 'ramp'):
            add(f'{prefix}-copy-{pattern}', w,h,1,1,src=f'inputs/{prefix}-{pattern}.rgb')
        for csc in range(4):
            for pattern in ('gray', 'color', 'ramp'):
                rgb = add(f'{prefix}-csc{csc}-{pattern}-to-rgb', w,h,0,1,csc,
                          src=f'inputs/{prefix}-{pattern}.nv12')
                add(f'{prefix}-csc{csc}-{pattern}-roundtrip', w,h,1,0,csc,src=rgb)
        # Scale-invariant positive-weight hypothesis: 63 (old), 16, 1.
        for lut in (2,3,4):
            for radius in range(4):
                for filt in (1,2,3):
                    for pattern in ('gray','color','patches','ramp','noise','edge'):
                        add(f'{prefix}-r{radius}-lut{lut}-f{filt}-{pattern}',w,h,1,1,filt=filt,radius=radius,lut=lut,
                            src=f'inputs/{prefix}-{pattern}.rgb')
        for lut in (3,4):
            for radius in range(4):
                for filt in (1,2,3):
                    for pattern in ('color','noise'):
                        name=f'{prefix}-r{radius}-lut{lut}-f{filt}-{pattern}'
                        rgb=add(name+'-csc-in',w,h,0,1,radius=radius,lut=lut,
                                src=f'inputs/{prefix}-{pattern}.nv12')
                        filtered=add(name+'-rgb-filter',w,h,1,1,filt=filt,radius=radius,lut=lut,src=rgb)
                        add(name+'-three-pass',w,h,1,0,radius=radius,lut=lut,src=filtered)
                        add(name+'-input-csc',w,h,0,1,filt=filt,radius=radius,lut=lut,src=f'inputs/{prefix}-{pattern}.nv12')
                        add(name+'-output-csc',w,h,1,0,filt=filt,radius=radius,lut=lut,src=rgb)
                        add(name+'-single-pass',w,h,0,0,filt=filt,radius=radius,lut=lut,src=f'inputs/{prefix}-{pattern}.nv12')
        # Boundary coefficient codes and threshold branches on coloured DC.
        if w==68:
            for lut in (3,4):
                for alpha in (0,16,28):
                    for threshold in (0,32,128,255):
                        for filt in (1,2,3):
                            for pattern in ('color','noise'):
                                add(f'{prefix}-lut{lut}-alpha{alpha}-t{threshold}-f{filt}-{pattern}',w,h,1,1,
                                    filt=filt,alpha=alpha,threshold=threshold,lut=lut,src=f'inputs/{prefix}-{pattern}.rgb')

    (out / 'cases.json').write_text(json.dumps(cases, indent=2)+'\n')
    lines=['#!/bin/sh', 'set -eu', 'cd /data/state/iep620c', 'mkdir -p results',
           'echo on > /sys/devices/platform/10108000.iep/power/control',
           "trap 'echo auto > /sys/devices/platform/10108000.iep/power/control' EXIT"]
    for c in cases:
        args = ' '.join(str(c[k]) for k in ('width','height','srgb','drgb','csc','filter','alpha','radius','threshold','lut','src','dst'))
        lines += [f'/tmp/iep620c/exercise "$1" {args} > results/{c["name"]}.json',
                  f'/tmp/iep620c/readback > results/{c["name"]}.regs']
    (out / 'run.sh').write_text('\n'.join(lines)+'\n')
    print(f'{len(cases)} finite one-frame passes prepared')

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('out', type=Path)
    prepare(p.parse_args().out)
