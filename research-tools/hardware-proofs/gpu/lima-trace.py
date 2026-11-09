#!/usr/bin/env python3
"""lima-trace.py <trace.txt>: per-task GPU time, kernel dispatch gaps and per-frame GPU busy from a tracefs capture.

Capture the listed Lima, scheduler, vblank, IRQ and DMA-fence trace events around an explicit finite GPU workload. Supply its tracefs output to this parser.
Times: run = lima_task_run (the moment lima writes the start command, give or take the register writes after it),
done = drm_sched_job_done (the HW fence callback in the IRQ). busy = done - run (includes the IRQ latency, a few us).
dispatch gap = next run - previous done on the same pipe, counted only when the next task was already queued and its
own dependencies done (queue before previous done): that is the kernel's own cost between two jobs (vendor: < 0.05 ms).
"""
import re, sys, collections, statistics
LINE = re.compile(r'^\s*(.+?)-(\d+)\s+\[(\d+)\]\s+\S+\s+([\d.]+):\s+(\w+):\s+(.*)$')
ev = []
for l in open(sys.argv[1], errors='replace'):
    m = LINE.match(l)
    if m:
        ev.append((float(m.group(4)) * 1e3, m.group(5), m.group(6), m.group(1)))   # ms
tasks = {}   # (ctx, seq) -> dict
vbl = []
irqs = []    # (t, name) GPU interrupt handler entries
waits = []
for t, name, f, comm in ev:
    if name in ('lima_task_submit', 'lima_task_run'):
        d = dict(kv.split('=') for kv in f.split())
        k = (int(d['context']), int(d['seqno']))
        tk = tasks.setdefault(k, {'pipe': d['pipe']})
        tk['submit' if name == 'lima_task_submit' else 'run'] = t
        if name == 'lima_task_run': tk['run_comm'] = comm
    elif name in ('drm_sched_job_queue', 'drm_sched_job_run'):
        m = re.search(r'fence=(\d+):(\d+), ring=(\w+)', f)
        if m:
            k = (int(m.group(1)), int(m.group(2)))
            tk = tasks.setdefault(k, {'pipe': m.group(3)})
            tk['q' if name == 'drm_sched_job_queue' else 'srun'] = t
    elif name == 'drm_sched_job_done':
        m = re.search(r'fence=(\d+):(\d+)', f)
        if m:
            tasks.setdefault((int(m.group(1)), int(m.group(2))), {})['done'] = t
    elif name == 'drm_vblank_event':
        vbl.append(t)
    elif name == 'irq_handler_entry':
        m = re.search(r'name=(\S+)', f)
        if m: irqs.append((t, m.group(1)))
    elif name in ('dma_fence_wait_start', 'dma_fence_wait_end'):
        waits.append((t, name, comm, f))
def st(v):
    v = sorted(v); n = len(v)
    if not n: return '-'
    return f"n={n} mean={statistics.mean(v):.3f} p10={v[n//10]:.3f} p50={v[n//2]:.3f} p90={v[n*9//10]:.3f} max={v[-1]:.3f}"
by = collections.defaultdict(list)
for k, tk in tasks.items():
    if 'run' in tk and 'done' in tk and 'pipe' in tk:
        by[tk['pipe']].append(tk)
for pipe, ts in sorted(by.items()):
    ts.sort(key=lambda x: x['run'])
    busy = [x['done'] - x['run'] for x in ts]
    gaps, lat = [], []
    for a, b in zip(ts, ts[1:]):
        ready = max(b.get('q', b['run']), a['run'])
        if ready <= a['done'] and b['run'] >= a['done']:
            gaps.append(b['run'] - a['done'])
    for x in ts:
        if 'q' in x: lat.append(x['run'] - x['q'])
    span = ts[-1]['done'] - ts[0]['run']
    print(f'{pipe}: tasks {len(ts)} busy {sum(busy):.1f} ms of {span:.1f} ms ({100*sum(busy)/span:.0f}%)')
    print(f'  task ms        {st(busy)}')
    print(f'  dispatch gap   {st(gaps)}   (kernel cost between back-to-back jobs)')
    print(f'  queue->run     {st(lat)}')
    pre = [x['run'] - x['srun'] for x in ts if 'srun' in x]
    print(f'  pre-start      {st(pre)}   (drm_sched_job_run -> lima_task_run: fence, pm, L2 flush, MMU)')
    if irqs:
        import bisect
        names = ('gp',) if pipe == 'gp' else ('pp0', 'pp1', 'pp')
        it = [t for t, n in irqs if n in names or (pipe == 'gp' and n == 'gp')]
        irqp, hw = [], []
        for x in ts:
            i = bisect.bisect_right(it, x['done']) - 1
            if i >= 0 and it[i] >= x['run']:
                irqp.append(x['done'] - it[i]); hw.append(it[i] - x['run'])
        print(f'  run->irq       {st(hw)}   (register writes + hardware time)')
        print(f'  irq->done      {st(irqp)}   (IRQ handler to the drm_sched done callback)')
    hop = []
    for a, b in zip(ts, ts[1:]):
        if 'srun' in b and b.get('q', b['srun']) <= a['done'] <= b['srun']:
            hop.append(b['srun'] - a['done'])
    print(f'  done->next run {st(hop)}   (free_job + run_job kworker hop, back-to-back jobs)')
    by_comm = collections.Counter(x.get('run_comm', '?').split('/')[0] for x in ts)
    print(f'  run from       {dict(by_comm)}')
# per vblank interval: GP and PP busy (overlap-clipped)
if len(vbl) > 2:
    rows = []
    for a, b in zip(vbl, vbl[1:]):
        r = {}
        for pipe, ts in by.items():
            r[pipe] = sum(max(0, min(b, x['done']) - max(a, x['run'])) for x in ts)
        rows.append(r)
    for pipe in sorted(by):
        print(f'per vblank interval ({statistics.mean([b-a for a,b in zip(vbl,vbl[1:])]):.2f} ms): {pipe} busy {st([r.get(pipe,0) for r in rows])}')
# GP/PP overlap
if 'gp' in by and 'pp' in by:
    ov = 0.0
    pp = by['pp']
    for g in by['gp']:
        for p in pp:
            ov += max(0, min(g['done'], p['done']) - max(g['run'], p['run']))
    print(f'GP time overlapped with PP: {ov:.1f} ms of {sum(x["done"]-x["run"] for x in by["gp"]):.1f} ms GP')
# Does the app start a frame while the GPU still works on the previous one (pipelined) or only after (serialized)?
subs = sorted(tk['submit'] for tk in tasks.values() if 'submit' in tk)
bursts = [s for i, s in enumerate(subs) if i == 0 or s - subs[i - 1] > 2.0]
pp = by.get('pp', [])
inflight = sum(1 for b in bursts if any(x['run'] <= b < x['done'] or (x.get('q', 1e18) <= b < x['run']) for x in pp))
after = []
for b in bursts:
    prev = [x['done'] for x in pp if x['done'] <= b]
    if prev: after.append(b - max(prev))
print(f'submit bursts (frames): {len(bursts)}, started while a PP task was queued or running: {inflight}')
print(f'  burst start - last PP done before it (ms): {st(after)}')
# CPU threads blocked on GPU fences (Mesa lima_bo_wait / syncobj waits / display)
if waits:
    starts = {}
    dur = collections.defaultdict(list)
    for t, name, comm, f in waits:
        m = re.search(r'timeline=(\S+)', f)
        k = (comm, m.group(1) if m else '?')
        if name == 'dma_fence_wait_start': starts[(comm, f)] = t
        elif (comm, f) in starts: dur[k].append(t - starts.pop((comm, f)))
    span = (ev[-1][0] - ev[0][0]) if ev else 1
    for k, v in sorted(dur.items()):
        print(f'fence waits by {k[0]} on {k[1]}: {len(v)} waits, {sum(v):.1f} ms in {span:.0f} ms traced, {st(v)}')
