#!/usr/bin/env python3
"""Offline numerical analysis of round-3 raw captures. Requires NumPy; no frame access."""
import argparse
import json
from pathlib import Path
import numpy as np


def load(path, w, h, rgb):
    a = np.frombuffer(path.read_bytes(), dtype=np.uint8)
    expected = w*h*(4 if rgb else 3)/ (1 if rgb else 2)
    if a.size != expected:
        raise ValueError(f'{path}: {a.size} != {expected}')
    return a.reshape(h,w,4) if rgb else a


def yuv_to_rgb(raw, w, h, csc):
    y = raw[:w*h].reshape(h,w).astype(float)
    uv = raw[w*h:].reshape(h//2,w//2,2).astype(float)-128
    u,v = (uv[:,:,i].repeat(2,0).repeat(2,1) for i in range(2))
    kr,kb = (0.2126,0.0722) if csc&2 else (0.299,0.114)
    if not csc&1:
        y=(y-16)*255/219
        u=u*255/224; v=v*255/224
    r=y+2*(1-kr)*v
    b=y+2*(1-kb)*u
    g=(y-kr*r-kb*b)/(1-kr-kb)
    a=np.full_like(r,255)
    return np.clip(np.rint(np.stack((b,g,r,a),axis=-1)),0,255).astype(np.uint8)


def rgb_to_yuv(raw, w, h, csc):
    b,g,r = (raw[:,:,i].astype(float) for i in range(3))
    kr,kb = (0.2126,0.0722) if csc&2 else (0.299,0.114)
    y=kr*r+(1-kr-kb)*g+kb*b
    u=(b-y)/(2*(1-kb)); v=(r-y)/(2*(1-kr))
    if not csc&1:
        y=y*219/255+16; u=u*224/255; v=v*224/255
    uv=np.stack((u,v),axis=-1).reshape(h//2,2,w//2,2,2).mean(axis=(1,3))+128
    return np.concatenate((np.clip(np.rint(y),0,255).astype(np.uint8).ravel(),
                           np.clip(np.rint(uv),0,255).astype(np.uint8).ravel()))


def stats(actual, expected, border=4):
    a=actual.astype(np.int16); e=np.abs(a-expected.astype(np.int16))
    interior=e[border:-border,border:-border] if min(e.shape[:2])>2*border else e
    mask=np.ones(e.shape[:2],dtype=bool)
    if min(e.shape[:2])>2*border: mask[border:-border,border:-border]=False
    vals,counts=np.unique(actual,return_counts=True)
    return dict(max_error=int(e.max()),mae=float(e.mean()),bad_samples=int((e>3).sum()),
                interior_max_error=int(interior.max()),boundary_max_error=int(e[mask].max()),
                min=int(a.min()),max=int(a.max()),span=int(a.max()-a.min()),
                histogram={str(int(k)):int(v) for k,v in zip(vals,counts)},
                row_means=[round(float(x),4) for x in a.mean(axis=1)],
                column_means=[round(float(x),4) for x in a.mean(axis=0)])


def planes(a,w,h,rgb):
    if rgb: return {n:a[:,:,i] for i,n in enumerate(('B','G','R','A'))}
    uv=a[w*h:].reshape(h//2,w//2,2)
    return dict(Y=a[:w*h].reshape(h,w),U=uv[:,:,0],V=uv[:,:,1])


def analyze(base):
    cases=json.loads((base/'cases.json').read_text())
    reports=[]
    for c in cases:
        w,h=c['width'],c['height']
        src=load(base/c['src'],w,h,c['srgb'])
        dst=load(base/c['dst'],w,h,c['drgb'])
        if c['srgb']==c['drgb']: expected=src
        elif c['drgb']: expected=yuv_to_rgb(src,w,h,c['csc'])
        else: expected=rgb_to_yuv(src,w,h,c['csc'])
        pp=planes(dst,w,h,c['drgb']); ep=planes(expected,w,h,c['drgb'])
        report={**c, 'channels':{k:stats(v,ep[k]) for k,v in pp.items()}}
        # Only constant fixtures and no-filter identity/CSC have numerical oracles.
        constant=('-gray' in c['src'] or '-color' in c['src'])
        report['reference_kind']='DC' if constant else 'CPU-CSC/copy' if not c['filter'] else 'diagnostic-only'
        report['pass']=all(p['bad_samples']==0 and (not constant or p['span']<=3)
                           for p in report['channels'].values()) if report['reference_kind']!='diagnostic-only' else None
        # Keep final NV12 comparisons against the original fixture, too: a
        # correct final CSC must not hide corrupted intermediate RGB pixels.
        name=c['name']
        original=None
        if name.endswith('-roundtrip'):
            pattern=name.split('-csc',1)[1].split('-',1)[1].rsplit('-roundtrip',1)[0]
            original=f'inputs/{w}x{h}-{pattern}.nv12'
        elif name.endswith(('-three-pass','-output-csc','-single-pass')):
            pattern='noise' if '-noise-' in name else 'color'
            original=f'inputs/{w}x{h}-{pattern}.nv12'
        if original:
            oracle=planes(load(base/original,w,h,False),w,h,False)
            report['end_to_end_channels']={k:stats(v,oracle[k]) for k,v in pp.items()}
            uniform='-ramp-' not in name and '-noise-' not in name
            report['end_to_end_pass']=all(v['bad_samples']==0 and (not uniform or v['span']<=3)
                                                for v in report['end_to_end_channels'].values())
        report['hardware']=json.loads((base/'results'/f'{c["name"]}.json').read_text())
        # Check exact readback of the routing, geometry, pitch and candidate tables.
        regs={int(k,16):int(v,16) for k,v in (l.split() for l in (base/'results'/f'{c["name"]}.regs').read_text().splitlines())}
        syuv=not c['srgb']; dyuv=not c['drgb']; filt=c['filter']; sel=c['csc']
        cfg0=(0x600 if syuv else 0) | ((filt<<18)|0x10000 if filt else 0)
        cfg1=0xff000000 | (3 if syuv else 0) | (0x300 if dyuv else 0)
        if syuv and (not dyuv or filt): cfg1|=0x100000|(sel<<16)
        if dyuv and (not syuv or filt): cfg1|=0x200000|((sel^1)<<18)
        wanted={0:cfg0,4:cfg1,0x20:(report['hardware']['src_stride']//4)|((report['hardware']['dst_stride']//4)<<16),
                0x28:(w-1)|((h-1)<<16),0x2c:(w-1)|((h-1)<<16),
                0x4c:0x1b000000|(c['threshold']<<16)|(c['alpha']<<8)|c['radius'],0x50:32}
        # All addresses use separate real allocations. RGB has no chroma,
        # and inactive histories stay zero; normalize only the two DMA bases.
        for i in range(4):
            for off,yuv,stride,size in ((0x80,syuv,report['hardware']['src_stride'],report['hardware']['src_size']),
                                        (0xb0,dyuv,report['hardware']['dst_stride'],report['hardware']['dst_size'])):
                base_addr=regs[off]
                active=i==0 or (not filt and syuv and dyuv)
                wanted[off+i*12]=base_addr if active else 0
                wanted[off+i*12+4]=base_addr+stride*h if active and yuv else 0
                wanted[off+i*12+8]=0
        for i,v in enumerate((0x40404040,0x3c3e3f3f,0x3336393b,0x272a2d31,
                               0x181c2023,0x0c0e1215,0x03040609,0x00000001)):
            wanted[0xe0+4*i]=v
        wanted.update({0x40:0 if filt else 0x808000,0x44:0 if filt else 0x8000,0x48:0 if filt else 0x3000000})
        if c['lut']:
            for off in range(0x400,0x600,4):
                weight={1:0,2:63,3:16,4:1}[c['lut']]
                wanted[off]=0 if off>0x550 else weight if off==0x550 else weight*0x01010101
        report['register_mismatches']={hex(off):[hex(val),hex(regs.get(off,0))] for off,val in wanted.items() if regs.get(off)!=val}
        reports.append(report)
    (base/'analysis.json').write_text(json.dumps(reports,indent=2)+'\n')
    compact=[]
    for r in reports:
        compact.append(dict(name=r['name'], reference_kind=r['reference_kind'],pass_=r['pass'],
            max_error={k:v['max_error'] for k,v in r['channels'].items()},
            span={k:v['span'] for k,v in r['channels'].items()},
            hw_us=r['hardware']['hw_us'], register_mismatches=r['register_mismatches'],
            end_to_end_pass=r.get('end_to_end_pass'),
            end_to_end_max_error={k:v['max_error'] for k,v in r.get('end_to_end_channels',{}).items()}))
    (base/'summary.json').write_text(json.dumps(compact,indent=2)+'\n')
    print(json.dumps(dict(passes=sum(r['pass'] is True for r in reports),
                         failures=sum(r['pass'] is False for r in reports),
                         diagnostic_only=sum(r['pass'] is None for r in reports),
                         register_mismatches=sum(bool(r['register_mismatches']) for r in reports))))

def box_reference(base):
    """Constrained alpha-zero oracle from the saved RGB small-radius run."""
    w,h=640,480
    src=load(base/'inputs/640x480-noise.rgb',w,h,True)
    reports=[]
    for radius in (0,1):
        k=3+2*radius;m=k//2
        padded=np.pad(src[:,:,:3].astype(float),((m,m),(m,m),(0,0)),mode='edge')
        mean=np.lib.stride_tricks.sliding_window_view(padded,(k,k),axis=(0,1)).mean(axis=(-1,-2))
        ref=np.rint(mean).astype(np.int16)
        dst=load(base/f'results/640x480-r{radius}-alpha0-noise.raw',w,h,True)
        errors=np.abs(dst[:,:,:3].astype(np.int16)-ref)
        boundary=np.ones((h,w),bool);boundary[m:-m,m:-m]=False
        reports.append(dict(radius=radius,window=k,alpha=0,weights=63,
            reference='rounded uniform RGB box, replicated edge',
            max_error=int(errors.max()),interior_max_error=int(errors[m:-m,m:-m].max()),
            boundary_max_error=int(errors[boundary].max()),mae=float(errors.mean()),
            source_sigma=float(src[8:-8,8:-8,0].std()),output_sigma=float(dst[8:-8,8:-8,0].std())))
    print(json.dumps(reports,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('base',type=Path)
    p.add_argument('--box-reference',action='store_true',help='saved small-radius alpha-zero run only')
    args=p.parse_args()
    if args.box_reference: box_reference(args.base)
    else: analyze(args.base)
