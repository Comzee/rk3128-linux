#!/usr/bin/env python3
"""Generate private synthetic fixtures and finite frame commands; no owner media."""
import argparse
import json
from pathlib import Path
import numpy as np

FORMATS = ('nv12','nv21','nv16','nv61','yu12','yv12','422p','bgra','rgba','abgr','argb','rgb565','bgr565','yv16')


def pack(rgb, yuv, fmt):
    rgb = np.clip(np.rint(rgb),0,255).astype(np.uint8)
    yuv = np.clip(np.rint(yuv),0,255).astype(np.uint8)
    h,w,_=rgb.shape
    if fmt in ('rgb565','bgr565'):
        r,g,b=(rgb[:,:,i].astype(np.uint16) for i in range(3))
        if fmt=='bgr565': r,b=b,r
        return ((r>>3)<<11 | (g>>2)<<5 | b>>3).astype('<u2').tobytes()
    if fmt in ('bgra','rgba','abgr','argb'):
        planes=dict(zip('rgb',(rgb[:,:,i] for i in range(3))))
        planes['a']=np.full((h,w),255,dtype=np.uint8)
        return np.stack([planes[c] for c in fmt],axis=-1).tobytes()
    y,u,v=(yuv[:,:,i] for i in range(3))
    sub=1 if fmt in ('nv16','nv61','422p','yv16') else 2
    u=u.reshape(h//sub,sub,w//2,2).mean(axis=(1,3)).astype(np.uint8)
    v=v.reshape(h//sub,sub,w//2,2).mean(axis=(1,3)).astype(np.uint8)
    if fmt in ('nv21','nv61','yv12','yv16'): u,v=v,u
    if fmt.startswith('nv'): return y.tobytes()+np.stack((u,v),axis=-1).tobytes()
    return y.tobytes()+u.tobytes()+v.tobytes()


def yuv2rgb(yuv,csc):
    y,u,v=(yuv[:,:,i].astype(float) for i in range(3)); u-=128;v-=128
    kr,kb=(.2126,.0722) if csc&2 else (.299,.114)
    if not csc&1: y=(y-16)*255/219;u*=255/224;v*=255/224
    r=y+2*(1-kr)*v;b=y+2*(1-kb)*u;g=(y-kr*r-kb*b)/(1-kr-kb)
    return np.clip(np.rint(np.stack((r,g,b),axis=-1)),0,255)


def rgb2yuv(rgb,csc):
    r,g,b=(rgb[:,:,i].astype(float) for i in range(3))
    kr,kb=(.2126,.0722) if csc&2 else (.299,.114)
    y=kr*r+(1-kr-kb)*g+kb*b;u=(b-y)/(2*(1-kb));v=(r-y)/(2*(1-kr))
    if not csc&1: y=y*219/255+16;u*=224/255;v*=224/255
    return np.clip(np.rint(np.stack((y,u+128,v+128),axis=-1)),0,255)


def generate(out, research=False):
    (out/'inputs').mkdir(parents=True,exist_ok=True)
    (out/'expected').mkdir(exist_ok=True)
    cases=[]
    def add(name,sf,df,opts=None,w=68,h=36,kind='ramp',oracle=True,tolerance=0):
        opts=opts or {}
        if not research and (opts.get('radius',0)>1 or opts.get('amount',0)>127 or
                             any(opts.get(k) for k in ('scale','probe','reset','aliases','hflip','vflip'))):
            return
        csc=opts.get('csc',0)
        yy,xx=np.indices((h,w))
        rgb=np.stack((48+(xx//8%4)*32,64+(yy//4%4)*24,80+(xx//16%4)*24),axis=-1)
        yuv=np.stack((48+(xx//8%8)*20,112+(xx//16%3)*16,112+(yy//4%3)*16),axis=-1)
        if kind=='lut':
            rgb=np.stack((xx,(xx+85)%256,(xx+171)%256),axis=-1)
        if kind=='dc': rgb[:]=(80,112,144);yuv[:]=(112,112,144)
        if kind=='scale':
            rgb=np.repeat(np.where(xx[:,:,None]<w//2,64,192),3,axis=2)
            yuv=np.stack((np.where(xx<w//2,64,192),np.full_like(xx,128),np.full_like(xx,128)),axis=-1)
        if kind=='gray': rgb[:]=112;yuv[:]=(112,128,128)
        if kind=='lowcontrast':
            rgb=np.stack((116+(xx%7)*4,116+(yy%7)*4,116+((xx+yy)%7)*4),axis=-1)
        if kind=='static':
            rgb=np.repeat((48+yy[:,:,None]*3),3,axis=2)
            yuv=np.stack((48+yy*3,np.full_like(yy,128),np.full_like(yy,128)),axis=-1)
        if kind=='noise':
            rng=np.random.default_rng(721)
            rgb=np.repeat(rng.integers(124,133,size=(h,w,1)),3,axis=2)
        if kind=='fields':
            yuv=np.stack((np.where(yy%2,192,64),np.full_like(yy,128),np.full_like(yy,128)),axis=-1)
        srgb=sf in ('bgra','rgba','abgr','argb','rgb565','bgr565')
        drgb=df in ('bgra','rgba','abgr','argb','rgb565','bgr565')
        data=pack(rgb,yuv,sf)
        count=3 if opts.get('dei',0) else 1
        src=f'inputs/{name}.raw';dst=f'results/{name}.raw'
        if kind=='timeline':
            frames=[]
            for n in range(count):
                yuv[:,:,0]=np.where(yy%2==int(opts.get('field')==2),32+64*n,64+64*n)
                yuv[:,:,1:]=128
                frames.append(pack(rgb,yuv,sf))
            (out/src).write_bytes(b''.join(frames))
        else: (out/src).write_bytes(data*count)
        expected_rgb=rgb.copy();expected_yuv=yuv.copy()
        if srgb and not drgb: expected_yuv=rgb2yuv(rgb,csc)
        if not srgb and drgb: expected_rgb=yuv2rgb(yuv,csc)
        rgb_stage=bool(opts.get('filter') or opts.get('curve') or opts.get('color'))
        if rgb_stage and not srgb:
            expected_rgb=yuv2rgb(yuv,csc)
        if opts.get('color',0):
            m=expected_rgb.astype(float).mean(axis=-1,keepdims=True)
            expected_rgb=np.clip(np.rint(m+(expected_rgb-m)*opts.get('coef',32)/32),0,255)
        def box(a):
            r=opts.get('radius',1)+1;k=2*r+1
            padded=np.pad(a.astype(float),((r,r),(r,r),(0,0)),mode='edge')
            total=sum(padded[y:y+h,x:x+w] for y in range(k) for x in range(k))
            return np.clip(np.floor(total/(k*k)),0,255)
        if opts.get('filter')==1 and opts.get('order',0)==1:
            filtered=box(expected_rgb)
            expected_rgb=np.clip(np.floor(filtered+(expected_rgb-filtered)*(opts.get('amount',8)&127)/32+.5),0,255)
        curve=opts.get('curve',0)
        if curve:
            # Mapping is B,G,R tables, independent of endpoint byte order.
            expected_rgb=expected_rgb.copy().astype(int)
            if curve==2: expected_rgb=255-expected_rgb
            elif curve==3: expected_rgb=expected_rgb**2//255
            elif curve==4:
                expected_rgb[:,:,2]//=2
                expected_rgb[:,:,1]=255-expected_rgb[:,:,1]
                expected_rgb[:,:,0]=expected_rgb[:,:,0]**2//255
        if opts.get('filter')==1 and opts.get('order',0)==0:
            filtered=box(expected_rgb)
            expected_rgb=np.clip(np.floor(filtered+(expected_rgb-filtered)*(opts.get('amount',8)&127)/32+.5),0,255)
        if opts.get('hflip'):
            expected_rgb=expected_rgb[:,::-1];expected_yuv=expected_yuv[:,::-1]
        if opts.get('vflip'):
            expected_rgb=expected_rgb[::-1];expected_yuv=expected_yuv[::-1]
        if opts.get('pattern'):
            expected_yuv[:]={1:(16,128,128),2:(163,167,105),3:(96,112,144)}[opts['pattern']]
        if rgb_stage and not drgb:
            expected_yuv=rgb2yuv(expected_rgb,csc)
        if opts.get('dw') or opts.get('dh'):
            ow,oh=opts.get('dw',w),opts.get('dh',h)
            ys=np.minimum((np.arange(oh)+.5)*h/oh,h-1).astype(int)
            xs=np.minimum((np.arange(ow)+.5)*w/ow,w-1).astype(int)
            expected_rgb=expected_rgb[ys][:,xs];expected_yuv=expected_yuv[ys][:,xs]
        if opts.get('alpha',255)!=255 and drgb and df not in ('rgb565','bgr565'):
            expected=bytearray(pack(expected_rgb,expected_yuv,df))
            expected[df.index('a')::4]=bytes([opts['alpha']])*(w*h)
            expected=bytes(expected)
        else: expected=pack(expected_rgb,expected_yuv,df)
        outputs=4 if opts.get('dei',0)==2 else 2 if opts.get('dei',0) else 1
        exp=f'expected/{name}.raw'
        if kind=='timeline':
            expected_frames=[]
            for n in range(outputs):
                pair=n//2 if opts['dei']==2 else n
                second=opts['dei']==2 and n%2==1
                value=(96 if second else 64)+64*pair
                other=(128 if second else 32)+64*pair
                yuv[:,:,0]=value;yuv[:,:,1:]=128
                edge=(h-1 if second else 0) if opts['field']==1 else (0 if second else h-1)
                yuv[edge,:,0]=other
                expected_frames.append(pack(rgb,yuv,df))
            (out/exp).write_bytes(b''.join(expected_frames))
        else:
            (out/exp).write_bytes(b'' if opts.get('reject') or opts.get('abort') else expected*outputs)
        options=','.join(f'{k}={v}' for k,v in opts.items()) or '-'
        cases.append(dict(name=name,src_format=sf,dst_format=df,width=w,height=h,
                          src=src,dst=dst,expected=exp,options=options,
                          oracle=oracle,tolerance=tolerance))
    for sf in FORMATS:
        add('copy-'+sf,sf,sf,tolerance=0 if sf not in ('rgb565','bgr565') else 8)
    for sf in FORMATS[:7]:
        for df in FORMATS[:7]:
            if sf!=df: add('layout-'+sf+'-'+df,sf,df,kind='dc')
    for sf in FORMATS[7:11]:
        for df in FORMATS[7:11]:
            if sf!=df: add('layout-'+sf+'-'+df,sf,df)
    for csc in range(4):
        add('yuv-rgb-'+str(csc),'nv12','bgra',dict(csc=csc),tolerance=3)
        add('rgb-yuv-'+str(csc),'bgra','nv12',dict(csc=csc),tolerance=3)
    for curve in range(1,5):
        for order in range(2):
            add(f'gamma-{curve}-{order}','bgra','bgra',dict(curve=curve,order=order),tolerance=1)
    for coef in (0,16,32,48,64,127):
        add('color-'+str(coef),'bgra','bgra',dict(color=1,coef=coef),tolerance=1,kind='lowcontrast')
    add('alpha','bgra','argb',dict(alpha=37))
    # Strong DC oracle, identical to #620's unrelaxed tolerance.
    for radius in range(4):
        for filt in (1,2,3):
            for weights in (1,16,63,64):
                add(f'dde-{radius}-{filt}-{weights}','bgra','bgra',
                    dict(filter=filt,radius=radius,weights=weights),kind='dc',tolerance=3,w=320,h=240)
    for radius in (2,3):
        for weights in range(65,71):
            add(f'dde-sparse-{radius}-{weights}','bgra','bgra',
                dict(filter=1,radius=radius,weights=weights),kind='dc',tolerance=3,w=320,h=240)
    for radius in (0,1):
        for filt in (1,2,3):
            add(f'positive-profile-{radius}-{filt}','bgra','bgra',dict(filter=filt,radius=radius,weights=65),kind='dc',tolerance=3)
    for mode in (1,2):
        for field in (1,2):
            for kind in ('gray','static','fields'):
                add(f'temporal-{mode}-{field}-{kind}','nv12','nv12',dict(dei=mode,field=field),
                    kind=kind,tolerance=3,oracle=kind!='fields')
            add(f'temporal-ei-{mode}-{field}','nv12','nv12',dict(dei=mode,field=field,ei=31,hf=64),kind='gray',tolerance=3)
    for sf in ('rgb565','bgr565'):
        add('exact-'+sf,sf,sf,dict(dither=0))
        add('expand-'+sf,sf,'bgra',dict(dither=0),tolerance=1)
    add('yv16-planar','yv16','422p',kind='dc')
    add('yv16-nv16','yv16','nv16',kind='dc')
    add('nv16-yv16','nv16','yv16',kind='dc')
    for sf in ('bgra','nv12','yu12','rgb565'):
        for hf,vf in ((1,0),(0,1),(1,1)):
            add(f'flip-{sf}-{hf}-{vf}',sf,sf,dict(hflip=hf,vflip=vf,dither=0))
    for pattern in (1,2,3):
        add('pattern-'+str(pattern),'nv12','nv12',dict(pattern=pattern),oracle=True)
    for probe in (1,2,3):
        add('probe-'+str(probe),'nv12','nv12',dict(probe=probe))
    for mode in (0,1,2):
        for field in (1,2):
            for filt in (1,2,3):
                add(f'combined-{mode}-{field}-{filt}','nv12','nv12',
                    dict(dei=mode,field=field,filter=filt),kind='gray',tolerance=3)
    for radius in (0,1):
        for order in (0,1):
            add(f'order-{radius}-{order}','bgra','bgra',
                dict(filter=1,radius=radius,amount=0,curve=3,order=order),kind='noise',tolerance=2)
    for radius in (2,3):
        for aliases in (0,1):
            for w in (16,64,128,256,640,1920):
                add(f'width-{radius}-{aliases}-{w}','bgra','bgra',
                    dict(filter=1,radius=radius,aliases=aliases),w=w,h=36,kind='dc',tolerance=3)
        for amount in (0,1,4,16,28):
            add(f'alpha-{radius}-{amount}','bgra','bgra',
                dict(filter=1,radius=radius,amount=amount),kind='dc',tolerance=3)
        add(f'large565-{radius}','rgb565','rgb565',
            dict(filter=1,radius=radius,dither=0),kind='dc',tolerance=8)
    for amount in (0,1,4,8,16,28):
        add(f'alpha-model-{amount}','bgra','bgra',dict(filter=1,radius=0,amount=amount),
            kind='noise',tolerance=2,oracle=amount==0)
    for mode in (1,2):
        for field in (1,2):
            add(f'timeline-{mode}-{field}','nv12','nv12',dict(dei=mode,field=field),kind='timeline',oracle=True)
    yuv_formats=(*FORMATS[:7],'yv16')
    for sf in yuv_formats:
        for mode in (0,1,2):
            for field in (1,2):
                add(f'dei-format-{sf}-{mode}-{field}',sf,sf,dict(dei=mode,field=field),kind='gray',tolerance=0)
    for sf in FORMATS:
        for radius in (0,1):
            for filt in (1,2,3):
                add(f'dde-format-{sf}-{radius}-{filt}',sf,sf,
                    dict(filter=filt,radius=radius,dither=0),kind='dc',tolerance=3)
    for sf in yuv_formats:
        add(f'gamma-yuv-{sf}',sf,sf,dict(curve=3),tolerance=3)
    # Reverse bits have no validated bounded m2m contract. Combined DEI+RGB
    # corrupts DC (I2/I4O1) or hangs (I4O2): test rejection, never START.
    cases=[c for c in cases if not c['name'].startswith(('flip-','combined-'))]
    for mode in (0,1,2):
        add(f'reject-combined-{mode}','nv12','nv12',dict(dei=mode,field=1,filter=1,reject=1),kind='gray')
    for n in range(12):
        add(f'abort-onecap-{n}','nv12','nv12',dict(dei=2,field=1,onecap=1,abort=1),kind='gray')
    for radius in range(4):
        add(f'reset-{radius}','bgra','bgra',dict(filter=1,radius=radius,reset=1),kind='dc',tolerance=3)
    for amount in (29,32,48,63,64,80,96):
        add(f'alpha-high-dc-{amount}','bgra','bgra',dict(filter=1,radius=1,amount=amount),kind='dc',tolerance=3)
        add(f'alpha-high-noise-{amount}','bgra','bgra',dict(filter=1,radius=0,amount=amount),kind='noise',oracle=False)
    for coef in range(128):
        add(f'color-q5-{coef}','bgra','bgra',dict(color=1,coef=coef),kind='lowcontrast',tolerance=1)
    add('scale-down-nv12','nv12','nv12',dict(scale=1,dw=32,dh=16),kind='scale',oracle=False)
    for radius in (0,1,2,3):
        add(f'scale-unity-dde-{radius}','bgra','bgra',dict(scale=1,filter=1,radius=radius),kind='dc',tolerance=3)
    for amount in range(256):
        add(f'alpha-q5-{amount}','bgra','bgra',dict(filter=1,radius=0,amount=amount),kind='noise',tolerance=2)
    for curve in range(1,5):
        # Every byte index in B, G and R is exercised in each curve.
        add(f'lut-coverage-{curve}','bgra','bgra',dict(curve=curve),kind='lut',w=256,h=16)
    for sf in FORMATS:
        add(f'hd-copy-{sf}',sf,sf,w=1920,h=1080,tolerance=8 if sf in ('rgb565','bgr565') else 0)
    add('maximum-bgra','bgra','bgra',w=1920,h=1088)
    for mode in (1,2):
        for field in (1,2):
            add(f'dei-hd-{mode}-{field}','nv12','nv12',dict(dei=mode,field=field),kind='gray',w=1920,h=1080)
    for amount in range(128):
        add(f'alpha-r1-q5-{amount}','bgra','bgra',dict(filter=1,radius=1,amount=amount),kind='noise',tolerance=0)
    for radius in (0,1,2,3):
        for w in (16,64,640):
            for profile in (dict(spad=0,dpad=0),dict(spad=64,dpad=128),dict(align=64),dict(align=128)):
                label='-'.join(f'{k}{v}' for k,v in profile.items())
                add(f'pitch-{radius}-{w}-{label}','bgra','bgra',dict(filter=1,radius=radius,**profile),kind='dc',w=w,h=64,tolerance=3)
    for mode in (0,1,2):
        add(f'heap-nv12-{mode}','nv12','nv12',dict(heap=1,dei=mode,field=1 if mode else 0),
            kind='gray',w=1920,h=1080)
    add('heap-alias','nv12','nv12',dict(heap=1,alias=1,reject=1,spad=16,dpad=16),kind='gray',w=1920,h=1080)
    for sf in FORMATS:
        for df in FORMATS:
            if sf!=df:
                add(f'cross-{sf}-{df}',sf,df,dict(dither=0),tolerance=8 if df in ('rgb565','bgr565') else 3)
    for mode in (0,1,2):
        for field in (1,2):
            for df in ('bgra','rgb565'):
                add(f'dei-rgb-{mode}-{field}-{df}','nv12',df,dict(dei=mode,field=field,dither=0,**({'reject':1} if mode==2 else {})),kind='gray',tolerance=3)
    for alias in (1,2,3):
        add(f'heap-temporal-alias-{alias}','nv12','nv12',dict(heap=1,dei=2,field=1,alias=alias,reject=1,spad=16,dpad=16),kind='gray')
    (out/'cases.json').write_text(json.dumps(cases,indent=2)+'\n')
    lines=['#!/bin/sh','set -u','cd /data/state/iep721','mkdir -p results']
    lines.append("echo 'file rockchip-iep.c -p' > /sys/kernel/debug/dynamic_debug/control")
    for c in cases:
        if 'heap=1' in c['options']:
            lines.append("echo 'format physical_spans= +p' > /sys/kernel/debug/dynamic_debug/control")
        args=f'"$1" {c["width"]} {c["height"]} {c["src_format"]} {c["dst_format"]} {c["src"]} {c["dst"]} {c["options"]}'
        lines.append(f'timeout 5 /tmp/iep721/exercise {args} > results/{c["name"]}.json 2> results/{c["name"]}.err; echo "$?" > results/{c["name"]}.exit')
        if 'heap=1' in c['options']:
            lines.append("echo 'format physical_spans= -p' > /sys/kernel/debug/dynamic_debug/control")
    lines.append('exit 0')
    (out/'run.sh').write_text('\n'.join(lines)+'\n')
    (out/'regressions.sh').write_text(Path(__file__).with_name('regressions.sh').read_text())
    print(f'{len(cases)} finite cases')


def analyze(out):
    results=[]
    for c in json.loads((out/'cases.json').read_text()):
        prefix=out/'results'/c['name']
        exit_path=prefix.with_suffix('.exit')
        rc=int(exit_path.read_text()) if exit_path.exists() else -1
        actual=(out/c['dst']).read_bytes() if (out/c['dst']).exists() else b''
        expected=(out/c['expected']).read_bytes()
        r=dict(name=c['name'],ioctl_exit=rc,oracle=c['oracle'])
        if rc or len(actual)!=len(expected):
            r.update(pass_=False,error=prefix.with_suffix('.err').read_text() if prefix.with_suffix('.err').exists() else 'result not created',bytes=len(actual),expected_bytes=len(expected))
        elif not expected:
            r.update(pass_=True,max_error=0,bad_samples=0)
        else:
            a=np.frombuffer(actual,dtype=np.uint8).astype(int)
            e=np.frombuffer(expected,dtype=np.uint8).astype(int)
            if c['dst_format'] in ('rgb565','bgr565') and c['tolerance']:
                aa=np.frombuffer(actual,dtype='<u2').astype(int)
                ee=np.frombuffer(expected,dtype='<u2').astype(int)
                a=np.stack(((aa>>11)*8,((aa>>5)&63)*4,(aa&31)*8),axis=-1)
                e=np.stack(((ee>>11)*8,((ee>>5)&63)*4,(ee&31)*8),axis=-1)
            diff=np.abs(a-e)
            r.update(max_error=int(diff.max()),mae=float(diff.mean()),bad_samples=int((diff>c['tolerance']).sum()),
                     pass_=bool(np.all(diff<=c['tolerance'])) if c['oracle'] else None)
        results.append(r)
    (out/'numeric.json').write_text(json.dumps(results,indent=2)+'\n')
    summary=dict(total=len(results),numeric_pass=sum(r['pass_'] is True for r in results),
                 failures=sum(r['pass_'] is False for r in results),diagnostic=sum(r['pass_'] is None for r in results))
    print(json.dumps(summary))
    for r in results:
        if r['pass_'] is False: print(r)
    return int(summary['failures']!=0)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('action',choices=('generate','analyze'));p.add_argument('directory',type=Path);p.add_argument('--research',action='store_true',help='include unvalidated hardware probes; requires archived research driver')
    a=p.parse_args()
    if a.action=='generate': generate(a.directory,a.research)
    else: raise SystemExit(analyze(a.directory))
