#!/usr/bin/env python3
"""Bounded native-RGB black-box identification; only synthetic private inputs."""
import argparse
import json
from pathlib import Path
import numpy as np


def generate(out,profile="gray"):
    (out/'inputs').mkdir(parents=True,exist_ok=True)
    (out/'expected').mkdir(exist_ok=True)
    cases=[]
    def add(name,w,h,r,f,kind='dc',amount=0,x=0,y=0):
        level=128 if profile=="gray" else 112
        rgb=np.full((h,w,4),level,dtype=np.uint8);rgb[:,:,3]=255
        if kind=='impulse':
            if profile=='gray': rgb[y,x,:3]=124
            elif profile=='white': rgb[y,x,:3]=255
            else: rgb[y,x,0]=48
        if kind=='impulse-bright': rgb[y,x,0]=255
        if kind=='impulse-gray': rgb[y,x,:3]=196
        if kind=='rows': rgb[:,:,:3]=96+np.arange(h,dtype=np.uint8)[:,None,None]
        src=f'inputs/{name}.raw';dst=f'results/{name}.raw'
        (out/src).write_bytes(rgb.tobytes())
        cases.append(dict(name=name,width=w,height=h,radius=r,filter=f,amount=amount,
                          kind=kind,x=x,y=y,src=src,dst=dst,level=level,profile=profile))
    for r in range(4):
        for f in (1,2,3):
            for a in (0,32):
                add(f'base-{r}-{f}-{a}',64,64,r,f,amount=a)
                add(f'rows-{r}-{f}-{a}',64,64,r,f,'rows',amount=a)
                for x,y in [(32,y) for y in range(16,25)]+[(16,32),(47,32),(2,2)]:
                    add(f'impulse-{r}-{f}-{a}-{x}-{y}',64,64,r,f,'impulse',amount=a,x=x,y=y)
                for kind in ('impulse-bright','impulse-gray'):
                    add(f'{kind}-{r}-{f}-{a}',64,64,r,f,kind,amount=a,x=32,y=20)
            for w in (8,12,16,20,24,28,32,36,40,48,64,96,128,192,256,320,480,640,960,1280,1920):
                add(f'width-{r}-{f}-{w}',w,64,r,f)
            for n in range(10): add(f'timing-{r}-{f}-{n}',640,480,r,f)
    # A clean driver rebind runs its initial bounded interface reset before
    # radius3/radius2 is the first DDE job. No other client is allowed here.
    cold=[]
    for r in (3,2):
        for stem in ('base','impulse','rows'):
            original=f'impulse-{r}-1-0-32-20' if stem=='impulse' else f'{stem}-{r}-1-0'
            c=next(c for c in cases if c['name']==original).copy()
            c.update(name=f'cold{r}-{stem}',dst=f'results/cold{r}-{stem}.raw',baseline=f'cold{r}-base')
            cold.append(c)
    cases=cold+cases
    (out/'identify-cases.json').write_text(json.dumps(cases,indent=2)+'\n')
    lines=['#!/bin/sh','set -u','cd /data/state/iep721','mkdir -p results',
           "echo 'format physical_spans= -p' > /sys/kernel/debug/dynamic_debug/control"]
    for c in cases:
        if c['name'] in ('cold3-base','cold2-base'):
            lines.extend(("echo 10108000.iep > /sys/bus/platform/drivers/rockchip-iep/unbind",
                          "echo 10108000.iep > /sys/bus/platform/drivers/rockchip-iep/bind",
                          'test "$(cat /sys/class/video4linux/${1##*/}/name)" = rockchip-iep || exit 1'))
        opts=f'filter={c["filter"]},radius={c["radius"]},amount={c["amount"]},spad=64,dpad=128'
        lines.append(f'timeout 5 /tmp/iep721/exercise "$1" {c["width"]} {c["height"]} bgra bgra {c["src"]} {c["dst"]} {opts} > results/{c["name"]}.json 2> results/{c["name"]}.err; echo "$?" > results/{c["name"]}.exit')
    lines.append('exit 0');(out/'run.sh').write_text('\n'.join(lines)+'\n')
    print(f'{len(cases)} identification jobs')


def analyze(out):
    cases=json.loads((out/'identify-cases.json').read_text());byname={c['name']:c for c in cases}
    rows=[];impulses=[];widths=[];timings=[];failures=[]
    def pixels(c):
        path=out/c['dst'];rc=int(path.with_suffix('.exit').read_text())
        if rc or not path.exists() or path.stat().st_size!=c['width']*c['height']*4:
            raise ValueError(f'{c["name"]}: rc={rc}, bytes={path.stat().st_size if path.exists() else 0}')
        return np.frombuffer(path.read_bytes(),dtype=np.uint8).reshape(c['height'],c['width'],4)[:,:,:3].astype(int)
    for c in cases:
        try: p=pixels(c)
        except (ValueError,OSError) as e: failures.append(str(e));continue
        meta={k:c[k] for k in ('name','radius','filter','amount','width')}
        if c['kind'].startswith('impulse'):
            try: b=pixels(byname[c.get('baseline',f'base-{c["radius"]}-{c["filter"]}-{c["amount"]}')])
            except (ValueError,OSError) as e: failures.append(str(e));continue
            delta=p-b;ys,xs=np.where(np.any(delta!=0,axis=2))
            meta.update(x=c['x'],y=c['y'],changed=len(xs),
                        support_dx=[int(xs.min()-c['x']),int(xs.max()-c['x'])] if len(xs) else [],
                        support_dy=[int(ys.min()-c['y']),int(ys.max()-c['y'])] if len(ys) else [],
                        # Keep the entire response for row-phase/position analysis.
                        response=[[int(y-c['y']),int(x-c['x']),int(delta[y,x,0])] for y,x in zip(ys,xs)])
            impulses.append(meta)
        elif c['kind']=='rows':
            meta['rows']=p[:,32,0].tolist();rows.append(meta)
        elif c['name'].startswith('width-'):
            meta.update(max_dc_error=int(np.abs(p-c.get('level',112)).max()),rows=p[:,c['width']//2,0].tolist());widths.append(meta)
        elif c['name'].startswith('timing-'):
            meta['dma_us']=json.loads((out/c['dst']).with_suffix('.json').read_text())['dma_us'];timings.append(meta)
    report=dict(jobs=len(cases),failures=failures,impulses=impulses,rows=rows,widths=widths,timings=timings)
    (out/'identify.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(jobs=len(cases),failures=failures)))
    for r in range(4):
        subset=[x for x in impulses if x['radius']==r and x['filter']==1 and x['amount']==0]
        print('radius',r,'denoise support',[(x['x'],x['y'],x['support_dx'],x['support_dy'],x['changed']) for x in subset])
        for f in (1,2,3):
            ts=[x['dma_us'] for x in timings if x['radius']==r and x['filter']==f]
            if ts: print('timing',r,f,'median/min/max',np.median(ts),min(ts),max(ts))
    return int(bool(failures))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('action',choices=('generate','analyze'));p.add_argument('directory',type=Path);p.add_argument('--profile',choices=('gray','blue','white'),default='gray')
    a=p.parse_args()
    if a.action=='generate': generate(a.directory,a.profile)
    else: raise SystemExit(analyze(a.directory))
