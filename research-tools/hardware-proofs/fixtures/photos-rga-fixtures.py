#!/usr/bin/env python3
"""#730 generated inputs only. Usage: photos-rga-fixtures.py DIRECTORY.
Large binary inputs/PPMs stay outside git. Never reads the owner's photos.
"""
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)

def picture(w, h, kind):
    y, x = np.ogrid[:h, :w]
    rgb = np.empty((h, w, 3), dtype=np.uint8)
    if kind == 'smooth':
        rgb[:, :, 0] = 80 + 70 * x // w + 10 * np.sin(y / 70)
        rgb[:, :, 1] = 70 + 80 * y // h + 10 * np.cos(x / 61)
        rgb[:, :, 2] = 60 + 60 * (x + y) // (w + h)
    elif kind == 'gray':
        rgb[:] = np.broadcast_to((x * 255 // max(1, w - 1))[:, :, None], (h, w, 3))
    elif kind == 'flat':
        rgb[:] = (80, 130, 170)
    else:
        rgb[:, :, 0] = ((x * 13 + y * 7) % 256)
        rgb[:, :, 1] = ((x ^ y) % 256)
        rgb[:, :, 2] = np.where((x // 7 + y // 5) & 1, 255, (x + 3 * y) % 256)
    im = Image.fromarray(rgb)
    if kind == 'edges':
        d = ImageDraw.Draw(im)
        for i, color in enumerate(('red', 'green', 'blue', 'white')):
            xx = (i % 2) * (w - 128)
            yy = (i // 2) * (h - 128)
            d.rectangle((xx, yy, xx + 127, yy + 127), fill=color)
    return im

def save(name, image, sub=2, orientation=1, progressive=False):
    exif = Image.Exif()
    exif[274] = orientation
    image.save(out / (name + '.jpg'), quality=90, subsampling=sub, exif=exif, progressive=progressive)

for w, h in ((1920, 1080), (2560, 1440), (4000, 3000)):
    for kind in ('edges', 'smooth', 'gray', 'flat'):
        save(f'{kind}-{w}', picture(w, h, kind))
for o in range(1, 9):
    save(f'orient-{o}', picture(1920, 1080, 'edges'), orientation=o)
save('422-2560', picture(2560, 1440, 'edges'), sub=1)
save('422-12mp', picture(4000, 3000, 'smooth'), sub=1)
save('progressive', picture(1920, 1080, 'edges'), progressive=True)
save('odd-420', picture(1921, 1081, 'edges'))
save('odd-422', picture(1921, 1081, 'edges'), sub=1)
save('portrait', picture(1440, 2560, 'edges'), orientation=6)
save('square', picture(2048, 2048, 'edges'))
save('refuse-cma', Image.new('RGB', (7008, 5008), '#807060'))
save('refuse-side', Image.new('RGB', (8192, 64), '#807060'))
save('refuse-50mp', Image.new('RGB', (10000, 5001), '#807060'))
print(f'photos-rga-fixtures: {len(list(out.glob("*.jpg")))} files')
