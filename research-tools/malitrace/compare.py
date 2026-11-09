#!/usr/bin/env python3
"""Blob against lima, program by program, for one decoded malitrace scene (development record #345).

    compare.py <scene dir> <limacompile output> --limaparse PATH

<scene dir>/decoded/dumps.json (decode.py) ties each blob GP and PP binary to the app's program that drew with it;
<limacompile output> is lima compiling the same programs' GLSL (LIMA_DEBUG=gp,pp,shaderdb,precompile through the drm-shim:
lima prints its GP code as "NNN: w w w w" lines and its PP code under "ppir codegen" as "NNN (@ off): w ..." lines).
Writes <scene dir>/compare/ (or --out) prog-<n>.{blob,lima}.{gp,pp}.{bin,txt} (the .txt through lima's own disassembler, so both
sides read alike) and prints a Markdown table: instruction counts and code size of both, per program, and the draws that
used it in the snapshotted frames.
"""
import argparse
import collections
import json
import os
import re
import struct
import subprocess

GP_LINE = re.compile(r'^\d{3}: ((?:[0-9a-f]{8} ){4})\s*$')
PP_HEAD = re.compile(r'^\d{3} \(@ +(\d+)\): ((?:[0-9a-f]{8} ?)+)\s*$')
PP_CONT = re.compile(r'^    ((?:[0-9a-f]{8} ?)+)\s*$')
DB = re.compile(r'SHADER-DB: MESA_SHADER_(VERTEX|FRAGMENT) shader: (\d+) inst, (\d+) loops, (\d+):(\d+) spills:fills')


def parse_lima(path):
    progs = {}
    cur = None
    state = None
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        m = re.match(r'^=== PROGRAM (\d+) ===', line)
        if m:
            cur = progs.setdefault(int(m.group(1)), {'gp': [], 'pp': [], 'db': {}})
            state = None
            continue
        if cur is None:
            continue
        if line.startswith('========ppir codegen'):
            state = 'pp'
            cur['pp'] = []
            continue
        if state == 'pp':
            m = PP_HEAD.match(line)
            if m:
                cur['pp'].append([int(w, 16) for w in m.group(2).split()])
                continue
            m = PP_CONT.match(line)
            if m and cur['pp']:
                cur['pp'][-1] += [int(w, 16) for w in m.group(1).split()]
                continue
            if line.startswith('-----'):
                state = None
            continue
        m = GP_LINE.match(line)
        if m:
            if state != 'gp':
                cur['gp'] = []
                state = 'gp'
            cur['gp'].append([int(w, 16) for w in m.group(1).split()])
            continue
        if state == 'gp' and not GP_LINE.match(line):
            state = None
        m = DB.search(line)
        if m:
            cur['db'][m.group(1)] = {'inst': int(m.group(2)), 'loops': int(m.group(3)),
                                     'spills': int(m.group(4)), 'fills': int(m.group(5))}
    return progs


def disasm(lp, kind, path):
    try:
        return subprocess.run([lp, kind, path, '0'], capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        return f'limaparse failed: {e}\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scene')
    ap.add_argument('lima')
    ap.add_argument('--limaparse', required=True)
    ap.add_argument('--out', help='where the side-by-side files go (default <scene dir>/compare)')
    ap.add_argument('--json')
    a = ap.parse_args()
    dj = json.load(open(os.path.join(a.scene, 'decoded', 'dumps.json')))
    lima = parse_lima(a.lima)
    sh = {(s['kind'], s['hash']): s for s in dj['shaders']}
    sdir = os.path.join(a.scene, 'decoded', 'shaders')
    out = a.out or os.path.join(a.scene, 'compare')
    os.makedirs(out, exist_ok=True)
    glsl = os.path.join(a.scene, 'decoded', 'glsl')
    # lima compiles a vertex shader once per source: a program whose VS was already compiled borrows that code
    vs_src = {}
    for p in lima:
        f = os.path.join(glsl, f'prog-{p}.vert')
        if os.path.exists(f):
            vs_src.setdefault(open(f).read(), []).append(p)
    for src, ps in vs_src.items():
        donor = next((p for p in ps if lima[p]['gp']), None)
        for p in ps:
            if donor is not None and not lima[p]['gp']:
                lima[p]['gp'] = lima[donor]['gp']
                lima[p]['db'].setdefault('VERTEX', lima[donor]['db'].get('VERTEX'))
    rows = []
    for prog, uses in sorted(dj.get('program_shaders', {}).items(), key=lambda x: int(x[0])):
        prog = int(prog)
        best = max(uses, key=lambda u: u['draws'])
        r = {'program': prog, 'draws': sum(u['draws'] for u in uses), 'variants': len(uses)}
        b_gp, b_pp = sh.get(('gp', best['gp'])), sh.get(('pp', best['pp']))
        if b_gp:
            r['blob_gp'] = b_gp['instr']
            src = os.path.join(sdir, f"gp-{b_gp['hash']}.bin")
            if os.path.exists(src):
                d = open(src, 'rb').read()
                p = os.path.join(out, f'prog-{prog}.blob.gp.bin')
                open(p, 'wb').write(d)
                open(p[:-4] + '.txt', 'w').write(disasm(a.limaparse, 'gp', p))
        if b_pp:
            r['blob_pp'] = b_pp['instr']
            r['blob_pp_words'] = b_pp['words']
            src = os.path.join(sdir, f"pp-{b_pp['hash']}.bin")
            if os.path.exists(src):
                d = open(src, 'rb').read()
                p = os.path.join(out, f'prog-{prog}.blob.pp.bin')
                open(p, 'wb').write(d)
                open(p[:-4] + '.txt', 'w').write(disasm(a.limaparse, 'fs', p))
            rs = b_pp.get('rsw', {})
            r['blob_varying_stride'] = rs.get('varying_stride')
        l = lima.get(prog)
        if l:
            if l['gp']:
                r['lima_gp'] = len(l['gp'])
                p = os.path.join(out, f'prog-{prog}.lima.gp.bin')
                open(p, 'wb').write(b''.join(struct.pack('<4I', *w) for w in l['gp']))
                open(p[:-4] + '.txt', 'w').write(disasm(a.limaparse, 'gp', p))
            if l['pp']:
                r['lima_pp'] = len(l['pp'])
                r['lima_pp_words'] = sum(len(w) for w in l['pp'])
                p = os.path.join(out, f'prog-{prog}.lima.pp.bin')
                open(p, 'wb').write(b''.join(struct.pack('<%dI' % len(w), *w) for w in l['pp']))
                open(p[:-4] + '.txt', 'w').write(disasm(a.limaparse, 'fs', p))
            r['lima_db'] = l['db']
        f = os.path.join(glsl, f'prog-{prog}.frag')
        if os.path.exists(f):
            r['frag_lines'] = sum(1 for x in open(f) if x.strip())
        rows.append(r)
    print('| program | draws | blob GP instr | lima GP instr | blob PP instr (words) | lima PP instr (words) |')
    print('|---|---|---|---|---|---|')
    for r in rows:
        print(f"| {r['program']} | {r['draws']} | {r.get('blob_gp', '-')} | {r.get('lima_gp', '-')} | "
              f"{r.get('blob_pp', '-')} ({r.get('blob_pp_words', '-')}) | {r.get('lima_pp', '-')} ({r.get('lima_pp_words', '-')}) |")
    both = [r for r in rows if 'blob_pp' in r and 'lima_pp' in r]
    if both:
        # the sums over programs both sides compiled; "per draw" weighs each program by the draws that used it
        def tot(k, w=False):
            return sum(r.get(k, 0) * (r['draws'] if w else 1) for r in both)
        print(f"| sum ({len(both)}) | {tot('draws')} | {tot('blob_gp')} | {tot('lima_gp')} | {tot('blob_pp')} | "
              f"{tot('lima_pp')} |")
        print(f"| per draw | | {tot('blob_gp', True)} | {tot('lima_gp', True)} | {tot('blob_pp', True)} | "
              f"{tot('lima_pp', True)} |")
    if a.json:
        json.dump(rows, open(a.json, 'w'), indent=1)


if __name__ == '__main__':
    main()
