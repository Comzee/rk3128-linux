#!/usr/bin/env python3
"""The per-scene numbers of a scripts/mali-capture.sh run (development record #345), from the board's raw sample lines.

    baseline.py <capture dir> [--json OUT]

Reads <scene>.baseline.txt (the app as the user runs it, no shim) and <scene>/measure.txt (the same scene run by hand under
malitrace) and prints one Markdown table row per scene and pass:
  presented frames per second and the intervals between them (SurfaceFlinger's present times of the app's layer,
  `dumpsys SurfaceFlinger --latency`, sampled every 2 s, de-duplicated), late frames (an interval over 1.5 vsyncs),
  the app's and SurfaceFlinger's CPU (% of one core), all four cores' busy %, the mean CPU clock, the GPU clock and the
  Mali driver's utilisation counters (sysfs `utilisation` and debugfs `utilization_gp`/`_pp`, 0-256 of the last window),
  and the app's peak RSS.
"""
import argparse
import glob
import json
import os
import statistics

HZ = 100  # USER_HZ of the board's 3.10 kernel


def parse(path):
    begin = None
    samples, presents, hwc, mem = [], set(), '', {}
    period = None
    for line in open(path, errors='replace'):
        p = line.split()
        if not p:
            continue
        if p[0] == 'BEGIN':
            begin = float(p[-1].split('=')[1])
        elif p[0] == 'S':
            try:
                d = {'t': float(p[1])}
                i = p.index('app')
                d['app'] = int(p[i + 1]) + int(p[i + 2]) if p[i + 1] != 'sf' else None
                j = p.index('sf')
                d['sf'] = int(p[j + 1]) + int(p[j + 2])
                k = p.index('cpu')
                c = [int(x) for x in p[k + 1:k + 8]]
                d['busy'], d['total'] = sum(c) - c[3] - c[4], sum(c)
                for key in ('cpuf', 'gclk', 'gutil', 'ugp', 'upp', 'ugppp'):
                    if key in p:
                        d[key] = int(p[p.index(key) + 1])
                samples.append(d)
            except (ValueError, IndexError):
                continue
        elif p[0] == 'L':
            if len(p) == 2:
                period = int(p[1])
            elif len(p) == 4 and p[2] != '0':
                presents.add(int(p[2]))
        elif p[0] == 'HWC':
            hwc = p[1] if len(p) > 1 else ''
        elif p[0] == 'M' and len(p) >= 3:
            mem[p[1].rstrip(':')] = int(p[2])
    if not samples:
        return None
    t0, t1 = samples[0]['t'], samples[-1]['t']
    wall = t1 - t0
    pr = sorted(x for x in presents if t0 * 1e9 <= x <= t1 * 1e9)
    iv = [(b - a) / 1e6 for a, b in zip(pr, pr[1:])]
    vs = (period or 19608611) / 1e6
    out = {'seconds': round(wall, 1), 'frames': len(pr), 'fps': round(len(pr) / wall, 2) if wall else 0,
           'vsync_ms': round(vs, 2), 'hwc': hwc, 'vmhwm_kb': mem.get('VmHWM')}
    if iv:
        s = sorted(iv)
        out.update({'iv_mean': round(statistics.mean(s), 2), 'iv_p50': round(s[len(s) // 2], 2),
                    'iv_p95': round(s[min(len(s) - 1, int(len(s) * 0.95))], 2), 'iv_max': round(s[-1], 2),
                    'late': sum(1 for v in s if v > 1.5 * vs)})
    a, b = samples[0], samples[-1]
    if a.get('app') is not None and b.get('app') is not None and wall:
        out['app_cpu'] = round((b['app'] - a['app']) / HZ / wall * 100, 1)
    if wall:
        out['sf_cpu'] = round((b['sf'] - a['sf']) / HZ / wall * 100, 1)
    if b['total'] > a['total']:
        out['all_cpu'] = round((b['busy'] - a['busy']) / (b['total'] - a['total']) * 100, 1)
    for key in ('cpuf', 'gutil', 'ugp', 'upp', 'ugppp'):
        v = [s[key] for s in samples if key in s]
        if v:
            out[key] = round(statistics.mean(v), 1)
    out['gclk_mhz'] = samples[-1].get('gclk', 0) // 1000000
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--json')
    a = ap.parse_args()
    res = {}
    for f in sorted(glob.glob(os.path.join(a.dir, '*.baseline.txt'))):
        sc = os.path.basename(f)[:-len('.baseline.txt')]
        res.setdefault(sc, {})['baseline'] = parse(f)
    for f in sorted(glob.glob(os.path.join(a.dir, '*', 'measure.txt'))):
        sc = os.path.basename(os.path.dirname(f))
        res.setdefault(sc, {})['trace'] = parse(f)
    print('| scene | pass | s | frames | fps | interval ms mean / p50 / p95 / max | late | app CPU % | SF CPU % | '
          'all cores % | CPU MHz | GPU MHz | GPU util /256 (gp, pp) | VmHWM MB |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for sc, passes in res.items():
        for name in ('baseline', 'trace'):
            r = passes.get(name)
            if not r:
                continue
            ivs = (f"{r['iv_mean']} / {r['iv_p50']} / {r['iv_p95']} / {r['iv_max']}" if 'iv_mean' in r else '-')
            print(f"| {sc} | {name} | {r['seconds']} | {r['frames']} | {r['fps']} | {ivs} | {r.get('late', '-')} | "
                  f"{r.get('app_cpu', '-')} | {r.get('sf_cpu', '-')} | {r.get('all_cpu', '-')} | "
                  f"{round(r.get('cpuf', 0) / 1000)} | {r['gclk_mhz']} | {r.get('gutil', '-')} ({r.get('ugp', '-')}, "
                  f"{r.get('upp', '-')}) | {round((r.get('vmhwm_kb') or 0) / 1024, 1)} |")
    if a.json:
        json.dump(res, open(a.json, 'w'), indent=1)


if __name__ == '__main__':
    main()
