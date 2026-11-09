#!/usr/bin/env python3
"""#739 RGB comparisons + natural-photo metrics; preserve #730's strict result.

Usage: quality.py INPUT OUTPUT [--crops DIRECTORY] [--maps DIRECTORY] [--selftest]
INPUT contains NAME-current.ppm and NAME-{default,rga,gpu,yuv,hybrid}.ppm.
The owner separately approved RGA visually; exact reports remain descriptive
of the original numerical gate and do not relabel different pixels as exact.
SSIM: RGB channel mean, Gaussian 11x11 sigma=1.5, population covariance,
K1=.01, K2=.03, data_range=255, five-pixel border excluded (Wang et al.).
Edge/chroma masks are descriptive only; they are never admission criteria.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import convolve1d


def metrics(a, b):
    assert a.shape == b.shape and a.ndim == 3 and a.shape[2] == 3
    a, b = a.astype(np.float64), b.astype(np.float64)
    d = np.abs(a - b)
    mse = np.mean(d * d)
    axis = np.arange(-5, 6)
    g = np.exp(-(axis * axis) / (2 * 1.5**2))
    g /= g.sum()
    def window(z):
        return convolve1d(convolve1d(z, g, axis=0, mode='reflect'), g, axis=1, mode='reflect')

    mu_a, mu_b = window(a), window(b)
    va = window(a*a) - mu_a*mu_a
    vb = window(b*b) - mu_b*mu_b
    cov = window(a*b) - mu_a*mu_b
    c1, c2 = (.01*255)**2, (.03*255)**2
    ss = ((2*mu_a*mu_b+c1)*(2*cov+c2) /
          ((mu_a*mu_a+mu_b*mu_b+c1)*(va+vb+c2)))
    ss = ss[5:-5, 5:-5]
    # Full-range BT.601 luma and colour differences; masks use the CURRENT output.
    y = a @ np.array([.299, .587, .114])
    cb = (a[:, :, 2]-y)/1.772
    cr = (a[:, :, 0]-y)/1.402

    def gradient(z):
        dx = np.abs(np.diff(z, axis=1, prepend=z[:, :1]))
        dy = np.abs(np.diff(z, axis=0, prepend=z[:1]))
        return np.maximum(dx, dy)

    edges = gradient(y) >= 12
    chroma = np.maximum(gradient(cb), gradient(cr)) >= 8
    per_pixel = d.mean(2)

    def masked(mask):
        total = per_pixel.sum()
        return dict(pixels=int(mask.sum()), fraction=round(float(mask.mean()), 6),
                    mae=round(float(per_pixel[mask].mean()), 6) if mask.any() else 0.,
                    error_share=round(float(per_pixel[mask].sum()/total), 6) if total else 0.)

    return dict(max=int(d.max()), mae=round(float(d.mean()), 6),
                psnr_db=round(float(10*np.log10(255**2/mse)), 6) if mse else None,
                ssim=round(float(ss.mean()), 9), changed=int(np.count_nonzero(d)),
                samples=int(d.size), over3=int(np.count_nonzero(d > 3)), exact=not bool(d.any()),
                luma_edges=masked(edges), chroma_edges=masked(chroma),
                non_edges=masked(~(edges | chroma))), edges, chroma


def pixels(p):
    return np.asarray(Image.open(p).convert('RGB'))


def crop_artifact(a, b, path, title):
    # Fixed native-pixel crop centred on the 128x128 tile with the largest mean
    # RGB error. A matching zero-error path uses the middle tile, labelled exact.
    side = min(128, a.shape[0], a.shape[1])
    d = np.abs(a.astype(float)-b).mean(2)
    choices = [(float(d[y:y+side, x:x+side].mean()), x, y)
               for y in range(0, a.shape[0]-side+1, max(1, side//2))
               for x in range(0, a.shape[1]-side+1, max(1, side//2))]
    _, x, y = max(choices)
    aa, bb = a[y:y+side, x:x+side], b[y:y+side, x:x+side]
    dd = np.uint8(np.clip(np.abs(aa.astype(float)-bb)*4, 0, 255))
    # 2x nearest-neighbour display makes individual pixel changes inspectable;
    # the label records this scale so it is not mistaken for a viewing-distance proof.
    width = side*2
    im = Image.new('RGB', (width*3, width+60), 'white')
    draw = ImageDraw.Draw(im)
    draw.text((4, 2), title, fill='black')
    draw.text((4, 17), f'crop ({x},{y}) {side}x{side}; 2x nearest display', fill='black')
    for i, (name, data) in enumerate(zip(('Current', 'Candidate', 'abs difference x4'), (aa, bb, dd))):
        draw.text((i*width+4, 35), name, fill='black')
        im.paste(Image.fromarray(data).resize((width,width), Image.Resampling.NEAREST), (i*width,60))
    im.save(path)
    return dict(x=x, y=y, side=side, display_scale=2)


def edge_map(a, b, edges, chroma, path):
    d = np.abs(a.astype(float)-b).max(2)
    mask = np.zeros_like(a)
    mask[edges] = (255,255,255)
    mask[chroma] = (0,255,255)
    heat = np.zeros_like(a)
    heat[:,:,0] = np.uint8(np.clip(d*4,0,255))
    w = min(a.shape[1],768)
    h = round(a.shape[0]*w/a.shape[1])
    combo = Image.new('RGB',(w*3,h+24),'white')
    for i, data in enumerate((a,mask,heat)):
        im = Image.fromarray(data)
        if im.size != (w,h):
            im = im.resize((w,h),Image.Resampling.NEAREST)
        combo.paste(im,(i*w,24))
    ImageDraw.Draw(combo).text((4,4),'Current | white=luma/cyan=chroma edges | red=max error x4',fill='black')
    combo.save(path)


def selftest():
    a = np.arange(64*48*3, dtype=np.uint8).reshape(48,64,3)
    r, _, _ = metrics(a, a)
    assert r['exact'] and r['ssim'] == 1 and r['psnr_db'] is None
    b = np.uint8(np.clip(a.astype(int)+10,0,255))
    r, _, _ = metrics(a,b)
    reverse, _, _ = metrics(b,a)
    assert not r['exact'] and r['max'] == 10 and 0 < r['ssim'] < 1
    assert r['ssim'] == reverse['ssim'] and r['mae'] == reverse['mae']
    # Constant-channel case independently reduces SSIM to its luminance term.
    a = np.full((32,32,3), 80, np.uint8)
    b = np.full_like(a, 90)
    r, _, _ = metrics(a,b)
    expected = (2*80*90+(.01*255)**2)/(80**2+90**2+(.01*255)**2)
    assert abs(r['ssim']-expected) < 1e-8
    print('quality: identity, symmetry and independent constant SSIM checks pass')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', nargs='?', type=Path)
    parser.add_argument('output', nargs='?', type=Path)
    parser.add_argument('--crops', type=Path)
    parser.add_argument('--maps', type=Path)
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return
    if not args.input or not args.output:
        parser.error('INPUT OUTPUT required')
    if args.crops:
        args.crops.mkdir(parents=True, exist_ok=True)
    if args.maps:
        args.maps.mkdir(parents=True, exist_ok=True)
    rows, best = [], {}
    for current in sorted(args.input.glob('*-current.ppm')):
        name = current.name.removesuffix('-current.ppm')
        a = pixels(current)
        for mode in ('default','rga','gpu','yuv','hybrid'):
            path = args.input/(name+f'-{mode}.ppm')
            if not path.exists():
                continue
            b = pixels(path)
            result, edges, chroma = metrics(a,b)
            row = dict(fixture=name, mode=mode, size=[a.shape[1],a.shape[0]], **result)
            if args.maps:
                row['edge_chroma_map'] = name+'-'+mode+'.png'
                edge_map(a,b,edges,chroma,args.maps/row['edge_chroma_map'])
            rows.append(row)
            if args.crops and (mode not in best or result['mae'] > best[mode][0]['mae']):
                best[mode] = row, a, b, edges, chroma
    if args.crops:
        for mode, (row,a,b,edges,chroma) in best.items():
            row['crop'] = crop_artifact(a,b,args.crops/(mode+'-crop.png'), row['fixture']+' / '+mode)
            # White: luma edge; cyan: chroma edge; red: amplified max-channel error.
            edge_map(a,b,edges,chroma,args.crops/(mode+'-edge-chroma-map.png'))
    args.output.write_text(json.dumps(rows,indent=2)+'\n')
    print(f'{len(rows)} comparisons; exact {sum(r["exact"] for r in rows)}; gate unchanged')


if __name__ == '__main__':
    main()
