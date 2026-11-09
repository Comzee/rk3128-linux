#!/usr/bin/env python3
"""Identify distance-table influence with positive basis perturbations, no owner media."""
import argparse
import json
from pathlib import Path
import numpy as np


def generate(out):
    (out/'inputs').mkdir(parents=True,exist_ok=True)
    cases=[]
    def add(name,r,kind='dc',distance=0,basis=0,w=64,h=64,y=21,filt=1,baseline=None):
        a=np.full((h,w,4),128,dtype=np.uint8);a[:,:,3]=255
        if kind=='impulse': a[y,w//2,:3]=124
        if kind=='rows': a[:,:,:3]=96+np.arange(h,dtype=np.uint8)[:,None,None]
        src=f'inputs/{name}.raw';dst=f'results/{name}.raw';(out/src).write_bytes(a.tobytes())
        cases.append(dict(name=name,radius=r,kind=kind,distance=distance,basis=basis,
                          width=w,height=h,y=y,x=w//2,filter=filt,baseline=baseline,src=src,dst=dst))
    for r in range(4):
        for f in (1,2,3):
            for d in range(4):
                prefix=f'p{r}-{f}-{d}'
                add(prefix+'-base',r,distance=d,filt=f)
                add(prefix+'-rows',r,'rows',distance=d,filt=f)
                for y in range(16,25):add(prefix+f'-imp-{y}',r,'impulse',distance=d,y=y,filt=f,baseline=prefix+'-base')
    for r in range(4):
        indices=range(1,257) if r>=2 else (1,5,9,17,25,26,33,41,49,64,65,81,82,101,129,161,193,225,256)
        for index in indices:
            prefix=f'b{r}-{index}'
            add(prefix+'-base',r,basis=index)
            add(prefix+'-imp',r,'impulse',basis=index,baseline=prefix+'-base')
    for r in (2,3):
        for d in (0,1):
            for w in (8,12,16,32,64,256,640,1920):add(f'w{r}-{d}-{w}',r,distance=d,w=w)
    for r in range(4):
        for d in (0,1):
            for n in range(10):add(f't{r}-{d}-{n}',r,distance=d,w=640,h=480)
    (out/'basis-cases.json').write_text(json.dumps(cases,indent=2)+'\n')
    lines=['#!/bin/sh','set -u','cd /data/state/iep721','mkdir -p results',
           "echo 'file rockchip-iep.c -p' > /sys/kernel/debug/dynamic_debug/control"]
    for c in cases:
        readback=c['name'] in ('p0-1-0-base','p1-1-0-base','p2-1-0-base','p3-1-0-base')
        if readback:lines.append("echo 'format completed +p' > /sys/kernel/debug/dynamic_debug/control")
        opts=f'filter={c["filter"]},radius={c["radius"]},amount=0,spad=64,dpad=128,distance={c["distance"]},basis={c["basis"]}'
        lines.append(f'timeout 5 /tmp/iep721/exercise "$1" {c["width"]} {c["height"]} bgra bgra {c["src"]} {c["dst"]} {opts} > results/{c["name"]}.json 2> results/{c["name"]}.err; echo "$?" > results/{c["name"]}.exit')
        if readback:lines.append("echo 'format completed -p' > /sys/kernel/debug/dynamic_debug/control")
    lines.append('exit 0');(out/'run.sh').write_text('\n'.join(lines)+'\n')
    print(len(cases),'bounded basis jobs')


def analyze(out):
    cases=json.loads((out/'basis-cases.json').read_text());byname={c['name']:c for c in cases}
    failures=[];report=[];cache={}
    def pixels(c):
        path=out/c['dst'];rc=int(path.with_suffix('.exit').read_text())
        if rc or path.stat().st_size!=c['width']*c['height']*4:raise ValueError(f'{c["name"]}: rc={rc}')
        return np.frombuffer(path.read_bytes(),dtype=np.uint8).reshape(c['height'],c['width'],4)[:,:,0].astype(int)
    for c in cases:
        try:p=pixels(c)
        except (OSError,ValueError) as e:failures.append(str(e));continue
        r=dict(name=c['name'],radius=c['radius'],filter=c['filter'],distance=c['distance'],basis=c['basis'],kind=c['kind'])
        if c['kind']=='impulse':
            try:base=pixels(byname[c['baseline']])
            except (OSError,ValueError) as e:failures.append(str(e));continue
            delta=p-base;ys,xs=np.where(delta!=0)
            r.update(support_dx=[int(xs.min()-c['x']),int(xs.max()-c['x'])] if len(xs) else [],
                     support_dy=[int(ys.min()-c['y']),int(ys.max()-c['y'])] if len(xs) else [],
                     response=[[int(y-c['y']),int(x-c['x']),int(delta[y,x])] for y,x in zip(ys,xs)])
            if c['basis']:
                normal_name=f'p{c["radius"]}-1-0-imp-{c["y"]}'
                normal=pixels(byname[normal_name])-pixels(byname[f'p{c["radius"]}-1-0-base'])
                r['diff_from_uniform']=int(np.max(np.abs(delta-normal)))
        else:
            r.update(max_dc_error=int(np.abs(p-128).max()) if c['kind']=='dc' else None,
                     rows=p[:,c['x']].tolist())
            if c['basis']:
                normal=pixels(byname[f'p{c["radius"]}-1-0-base'])
                r['diff_from_uniform']=int(np.max(np.abs(p-normal)))
        if c['name'].startswith('t'):
            r['dma_us']=json.loads((out/c['dst']).with_suffix('.json').read_text())['dma_us']
        report.append(r)
    summary=dict(jobs=len(cases),failures=failures,results=report)
    (out/'basis.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('jobs',len(cases),'failures',len(failures))
    for r in range(4):
        for d in range(4):
            base=next(x for x in report if x['name']==f'p{r}-1-{d}-base')
            impulses=[x for x in report if x['name'].startswith(f'p{r}-1-{d}-imp')]
            print('radius',r,'distance',d,'dc',base['max_dc_error'],'rows',base['rows'][:18],
                  'supports',[(x['support_dx'],x['support_dy']) for x in impulses])
        active=sorted(set(x['basis']-1 for x in report if x['radius']==r and x['basis'] and x['diff_from_uniform']))
        print('active distance indices',r,len(active),active)
    return int(bool(failures))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('action',choices=('generate','analyze'));p.add_argument('directory',type=Path)
    a=p.parse_args()
    if a.action=='generate':generate(a.directory)
    else:raise SystemExit(analyze(a.directory))
