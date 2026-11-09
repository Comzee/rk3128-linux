#!/usr/bin/env python3
"""Decode a malitrace capture (tools/malitrace/malitrace.c, development record #345) on the build host.

    decode.py <capture dir> [--limaparse PATH] [--json OUT]

The capture dir holds trace.bin and mem.bin from one app run. Prints the run's GPU numbers (frame intervals, GP and PP job
times per frame, jobs, PP cores, tile heap use, render targets) and, for the frames that were snapshotted, writes into
<capture dir>/decoded/: each job's VS and PLBU command stream, every render state (RSW) and texture descriptor the PLBU
streams point at, and every GP and PP shader binary (shaders/<kind>-<hash>.bin, plus .txt disassembly through Mesa lima's
own parser when --limaparse is given; tools/malitrace/lima-compare.sh builds it). Struct layouts are those of the board's
driver (Mali Utgard r6p0 user-kernel API with Rockchip's additions), read from the capture itself where they can be:
  PP_AND_GP_START_JOB (ioctl 0x84/4): u64 ctx, u64 gp_args, u64 pp_args, the two structs captured beside it;
  GP start: u64 ctx, u64 user_job_ptr, u32 priority, u32 frame_registers[6] (VS start, VS end, PLBU start, PLBU end,
    tile heap start, tile heap end), perf flag/src0/src1, frame_builder_id, flush_id, fence ...;
  PP start: u64 ctx, u64 user_job_ptr, u32 priority, u32 frame[23], addr_frame[7], addr_stack[7], wb0..2[12], dlbu[4],
    num_cores, perf flag/src0/src1, frame_builder_id, flush_id, flags, ...;
  WAIT_FOR_NOTIFICATION (0x82/2): u64 ctx, u32 type, pad, then u64 user_job_ptr, u32 status, (GP) heap_current_addr.
Never commit what this writes: it derives from the closed blob (build/ is gitignored).
"""
import argparse
import collections
import hashlib
import json
import os
import statistics
import struct
import subprocess
import sys
import zlib

R_INFO, R_IOCTL, R_MMAP, R_MUNMAP, R_SWAP, R_SNAP, R_GLSRC, R_GLBIN, R_GLATTACH, R_EXTRA, R_GLUSE, R_GLDRAW = range(1, 13)
BASE = 0x82
GP_FINISHED, GP_STALLED, PP_FINISHED = 0x30010, 0x30020, 0x20010
WB_FORMATS = {0: 'RGB565', 1: 'RGBA5551?', 2: 'RGBA4444?', 3: 'RGBA8888', 4: 'Z24S8?', 5: 'Z16?'}


def records(path):
    d = open(path, 'rb').read()
    if d[:8] != b'MALITRC1':
        sys.exit(f'{path}: not a malitrace file')
    o = 8
    while o + 16 <= len(d):
        t, _, n, ts = struct.unpack_from('<HHIQ', d, o)
        if o + 16 + n > len(d):
            break  # a record cut by the app's end
        yield t, ts, d[o + 16:o + 16 + n]
        o += 16 + n


def u32s(b, off, n):
    return list(struct.unpack_from('<%dI' % n, b, off))


class Capture:
    def __init__(self, cdir):
        self.dir = cdir
        self.info = []
        self.swaps = []          # (t0, t1, frame)
        self.jobs = []           # dicts
        self.notif = []          # (t1, type, user_job_ptr, out bytes)
        self.snaps = []          # (t, seq, why, entries)
        self.src = {}            # shader id -> (type, text)
        self.attach = collections.defaultdict(list)
        self.bins = {}           # program -> (fmt, ok, bytes)
        self.draws = []          # (t, kind, mode, count, program, fbo)
        self.uses = []
        self.ioctl_count = collections.Counter()
        self.allocs = []         # (t, gpu, vsize, psize, flags)
        self.suspends = 0
        self.grown = []
        self.clock_khz = 297000  # the board's Mali clock (sysfs clock, fixed: dvfs offers one step)
        pending = []
        for t, ts, p in records(os.path.join(cdir, 'trace.bin')):
            if t == R_INFO:
                self.info.append(p.decode(errors='replace'))
            elif t == R_EXTRA:
                req, which, ptr = struct.unpack_from('<III', p)
                pending.append((which, p[12:]))
            elif t == R_IOCTL:
                fd, req, ret, err, t1, size = struct.unpack_from('<IIiiQI', p)
                inb, outb = p[28:28 + size], p[28 + size:28 + 2 * size]
                typ, nr = req >> 8 & 0xff, req & 0xff
                self.ioctl_count[(typ - BASE, nr, size)] += 1
                if typ == BASE + 2 and nr == 4:      # PP_AND_GP_START_JOB
                    gp = next((b for w, b in pending if w == 0), None)
                    pp = next((b for w, b in pending if w == 1), None)
                    pending = []
                    self.add_job(ts, t1, gp, pp, ret)
                elif typ == BASE + 3 and nr == 0:    # GP_START_JOB alone
                    self.add_job(ts, t1, inb, None, ret)
                elif typ == BASE + 2 and nr == 0:    # PP_START_JOB alone
                    self.add_job(ts, t1, None, inb, ret)
                elif typ == BASE and nr == 2 and ret == 0:  # WAIT_FOR_NOTIFICATION
                    ntype = struct.unpack_from('<I', outb, 8)[0]
                    ujp = struct.unpack_from('<Q', outb, 16)[0]
                    self.notif.append((t1, ntype, ujp, outb))
                elif typ == BASE + 1 and nr == 0:    # ALLOC_MEM
                    gpu, vsize, psize, flags = struct.unpack_from('<IIII', outb, 8)
                    self.allocs.append((ts, gpu, vsize, psize, flags))
                elif typ == BASE + 3 and nr == 3:    # GP_SUSPEND_RESPONSE: u64 ctx, u32 cookie, u32 code, u32 new heap start, end
                    self.suspends += 1
                    hs, he = struct.unpack_from('<II', inb, 16)
                    if he > hs:
                        self.grown.append(he - hs)
            elif t == R_SWAP:
                frame, t1, ret, surf = struct.unpack_from('<IQII', p)
                self.swaps.append((ts, t1, frame))
            elif t == R_SNAP:
                seq, why, n = struct.unpack_from('<III', p)
                ents = [struct.unpack_from('<IIQQQ', p, 12 + 32 * i) for i in range(n)]
                self.snaps.append((ts, seq, why, ents))
            elif t == R_GLSRC:
                sh, typ = struct.unpack_from('<II', p)
                self.src[sh] = (typ, p[8:].decode(errors='replace'))
            elif t == R_GLATTACH:
                prog, sh = struct.unpack_from('<II', p)
                self.attach[prog].append(sh)
            elif t == R_GLBIN:
                prog, fmt, ok = struct.unpack_from('<IIi', p)
                self.bins[prog] = (fmt, ok, p[12:])
            elif t == R_GLDRAW:
                self.draws.append((ts,) + struct.unpack_from('<5I', p))
            elif t == R_GLUSE:
                self.uses.append((ts, struct.unpack_from('<I', p)[0]))
        self.match()

    def add_job(self, t0, t1, gp, pp, ret):
        j = {'t0': t0, 't_submit_end': t1, 'ret': ret}
        if gp and len(gp) >= 44:
            g = u32s(gp, 0, 11)
            j['gp_ujp'] = struct.unpack_from('<Q', gp, 8)[0]
            j['vs'] = (g[5], g[6])
            j['plbu'] = (g[7], g[8])
            j['heap'] = (g[9], g[10])
            j['frame_builder'] = u32s(gp, 56, 1)[0]
        if pp and len(pp) >= 340:
            j['pp_ujp'] = struct.unpack_from('<Q', pp, 8)[0]
            j['frame'] = u32s(pp, 20, 23)
            j['addr_frame'] = u32s(pp, 112, 7)
            j['addr_stack'] = u32s(pp, 140, 7)
            j['wb'] = [u32s(pp, 168 + 48 * i, 12) for i in range(3)]
            j['dlbu'] = u32s(pp, 312, 4)
            j['num_cores'] = u32s(pp, 328, 1)[0]
            j['pp_frame_builder'] = u32s(pp, 344, 1)[0]
            j['pp_flags'] = u32s(pp, 352, 1)[0]
        self.jobs.append(j)

    def match(self):
        """GP and PP finish times from the notifications (first one after the start with the job's user pointer)."""
        by = collections.defaultdict(list)
        for t1, ntype, ujp, outb in self.notif:
            by[(ntype, ujp)].append((t1, outb))
        for j in self.jobs:
            for key, ntype, name in (('gp_ujp', GP_FINISHED, 'gp'), ('pp_ujp', PP_FINISHED, 'pp')):
                if key not in j:
                    continue
                for t1, outb in by.get((ntype, j[key]), []):
                    if t1 > j['t0']:
                        j[name + '_done'] = t1
                        j[name + '_status'] = struct.unpack_from('<I', outb, 24)[0]
                        if name == 'gp':   # u64 ujp, status, heap_current_addr, perf_counter0, perf_counter1
                            j['gp_cnt'] = struct.unpack_from('<II', outb, 32)
                        else:              # u64 ujp, status, perf_counter0[8], perf_counter1[8], src0, src1
                            j['pp_cnt0'] = struct.unpack_from('<8I', outb, 28)
                            j['pp_cnt1'] = struct.unpack_from('<8I', outb, 60)
                        break
        # Time on the hardware: from the later of the job's submission, its GP part's end (PP waits for it) and the end of
        # the job of the same kind before it (the kernel queues them; one GP core, the PP job holds both PP cores), to its
        # finish notification. The notification is read by the blob's waiting thread, so this is an upper bound by the
        # wake-up latency (tens of microseconds).
        last = {'gp': 0, 'pp': 0}
        for j in sorted(self.jobs, key=lambda x: x['t0']):
            if 'gp_done' in j:
                j['gp_ms'] = (j['gp_done'] - max(j['t0'], last['gp'])) / 1e6
                last['gp'] = max(last['gp'], j['gp_done'])
            if 'pp_done' in j:
                start = max(j['t0'], j.get('gp_done', j['t0']), last['pp'])
                j['pp_ms'] = max(0.0, (j['pp_done'] - start) / 1e6)
                j['pp_latency_ms'] = (j['pp_done'] - j['t0']) / 1e6
                last['pp'] = max(last['pp'], j['pp_done'])
        stalled = sum(1 for n in self.notif if n[1] == GP_STALLED)
        self.stalled = stalled

    # ---- the numbers ----
    def counters(self, j):
        """GPU time from the hardware counters the shim asked for (MALITRACE_PPCNT/GPCNT defaults: active cycles first)"""
        out = {}
        if 'gp_cnt' in j:
            out['gp_hw_ms'] = j['gp_cnt'][0] / self.clock_khz
        if 'pp_cnt0' in j:
            n = max(1, j.get('num_cores', 1))
            cyc = j['pp_cnt0'][:n]
            out['pp_hw_ms'] = max(cyc) / self.clock_khz
            out['pp_core_cycles'] = cyc
            out['pp_instr'] = sum(j['pp_cnt1'][:n])
        return out

    def summary(self, skip_first=30):
        sw = self.swaps
        out = {'info': self.info, 'frames': len(sw), 'jobs': len(self.jobs), 'gp_heap_stalls': self.stalled,
               'gp_suspend_responses': self.suspends}
        ivals = [(b[1] - a[1]) / 1e6 for a, b in zip(sw, sw[1:])][skip_first:]
        dump_frames = set()
        for s in self.snaps:
            f = self.frame_of(s[0])
            dump_frames.update({f, f + 1})
        if sw and ivals:
            keep = [iv for k, iv in enumerate(ivals) if (k + skip_first + 1) not in dump_frames]
            if keep:
                keep.sort()
                out['interval_ms'] = {'n': len(keep), 'mean': round(statistics.mean(keep), 2),
                                      'p50': round(keep[len(keep) // 2], 2),
                                      'p95': round(keep[min(len(keep) - 1, int(len(keep) * 0.95))], 2),
                                      'max': round(keep[-1], 2),
                                      'over_1.5_vsync': sum(1 for v in keep if v > 1.5 * 19.6)}
                out['fps'] = round(1000.0 / statistics.mean(keep), 2)
        # per frame: GPU job times (frames with a snapshot are left out)
        per = collections.defaultdict(lambda: {'gp': 0.0, 'pp': 0.0, 'jobs': 0, 'gphw': 0.0, 'pphw': 0.0, 'ppi': 0})
        balance = []
        for j in self.jobs:
            f = self.frame_of(j['t0'])
            if f in dump_frames or f < skip_first:
                continue
            per[f]['jobs'] += 1
            per[f]['gp'] += j.get('gp_ms', 0.0)
            per[f]['pp'] += j.get('pp_ms', 0.0)
            c = self.counters(j)
            per[f]['gphw'] += c.get('gp_hw_ms', 0.0)
            per[f]['pphw'] += c.get('pp_hw_ms', 0.0)
            per[f]['ppi'] += c.get('pp_instr', 0)
            cc = c.get('pp_core_cycles')
            if cc and len(cc) == 2 and max(cc) > 0:
                balance.append(min(cc) / max(cc))

        def st(vals):
            if not vals:
                return None
            v = sorted(vals)
            return {'mean': round(statistics.mean(v), 2), 'p50': round(v[len(v) // 2], 2),
                    'p95': round(v[min(len(v) - 1, int(len(v) * 0.95))], 2), 'max': round(v[-1], 2)}
        if per:
            out['gp_ms_per_frame'] = st([p['gp'] for p in per.values()])
            out['pp_ms_per_frame'] = st([p['pp'] for p in per.values()])
            out['jobs_per_frame'] = st([p['jobs'] for p in per.values()])
            if any(p['pphw'] for p in per.values()):
                out['gp_hw_ms_per_frame'] = st([p['gphw'] for p in per.values()])
                out['pp_hw_ms_per_frame'] = st([p['pphw'] for p in per.values()])
                out['pp_instr_per_frame'] = st([p['ppi'] for p in per.values()])
            if balance:
                out['pp_core_balance'] = st(balance)
        js = [j for j in self.jobs if 'pp_ms' in j]
        out['pp_ms_per_job'] = st([j['pp_ms'] for j in js])
        out['gp_ms_per_job'] = st([j['gp_ms'] for j in self.jobs if 'gp_ms' in j])
        out['submit_to_pp_done_ms'] = st([j['pp_latency_ms'] for j in js])
        out['pp_num_cores'] = dict(collections.Counter(j.get('num_cores') for j in self.jobs if 'num_cores' in j))
        out['pp_subjobs'] = dict(collections.Counter(1 + sum(1 for a in j['addr_frame'] if a)
                                                     for j in self.jobs if 'addr_frame' in j))
        heaps = [j['heap'][1] - j['heap'][0] for j in self.jobs if 'heap' in j]
        if heaps:
            out['tile_heap_kb_at_start'] = st([h / 1024 for h in heaps])
        if self.grown:
            out['tile_heap_kb_after_growth'] = st([g / 1024 for g in self.grown])
        # the blob writes its VS and PLBU lists in chunks of one command pool and jumps between them, so end - start is
        # the list's size only when the two ranges are ordered and apart; the rest count as chained
        vs, pl, chained = [], [], 0
        for j in self.jobs:
            if 'vs' not in j or 'plbu' not in j:
                continue
            (v0, v1), (p0, p1) = j['vs'], j['plbu']
            if v0 <= v1 and p0 <= p1 and (v1 <= p0 or p1 <= v0):
                vs.append(v1 - v0)
                pl.append(p1 - p0)
            else:
                chained += 1
        out['vs_cmd_bytes'] = st(vs)
        out['plbu_cmd_bytes'] = st(pl)
        out['cmd_lists_chained'] = chained
        rts = collections.Counter()
        for j in self.jobs:
            if 'wb' not in j:
                continue
            for i, w in enumerate(j['wb']):
                if w[0]:
                    rts[f'wb{i} type={w[0]} fmt={WB_FORMATS.get(w[2], w[2])} layout={w[4]} pitch={w[5]} mrt={w[6]}'] += 1
        out['render_targets'] = dict(rts)
        frames = collections.Counter()
        for j in self.jobs:
            if 'frame' in j:
                fr = j['frame']
                frames[f'flags=0x{fr[3]:x} stack=0x{fr[12]:x}/{fr[13]} one={fr[16]} sub={fr[17]} dubya=0x{fr[18]:x} '
                       f'onscreen={fr[19]} blocking=0x{fr[20]:x} scale=0x{fr[21]:x} foureight=0x{fr[22]:x}'] += 1
        out['pp_frame_regs'] = dict(frames)
        out['ioctls'] = {f'{s}/{n}/{z}': c for (s, n, z), c in sorted(self.ioctl_count.items())}
        out['allocs'] = len(self.allocs)
        out['programs'] = len(self.bins)
        return out

    def frame_of(self, t):
        """the number of the frame a time falls in: the swaps before it, plus one"""
        lo, hi = 0, len(self.swaps)
        while lo < hi:
            m = (lo + hi) // 2
            if self.swaps[m][1] <= t:
                lo = m + 1
            else:
                hi = m
        return lo + 1

    # ---- memory at a snapshot ----
    def memory_at(self, snap):
        raw = os.path.join(self.dir, 'mem.bin')
        if not os.path.exists(raw) and os.path.exists(raw + '.gz'):
            # a run ended by a signal can leave the gzip stream without its trailer: inflate what is there
            z = zlib.decompressobj(16 + zlib.MAX_WBITS)
            with open(raw + '.gz', 'rb') as f, open(raw, 'wb') as o:
                while True:
                    c = f.read(1 << 20)
                    if not c:
                        break
                    o.write(z.decompress(c))
        mem = open(raw, 'rb')
        cache = getattr(self, '_cache', {})
        self._cache = cache
        regions = []
        for cpu, ln, gpu, moff, h in snap[3]:
            if moff != 0xFFFFFFFFFFFFFFFF:
                mem.seek(moff)
                cache[h] = mem.read(ln)
            if h in cache:
                regions.append((gpu, cache[h]))
        return Memory(regions)


class Memory:
    def __init__(self, regions):
        self.regions = sorted(regions)

    def read(self, va, n):
        for gpu, b in self.regions:
            if gpu <= va and va + n <= gpu + len(b):
                return b[va - gpu:va - gpu + n]
        return None

    def region_end(self, va):
        for gpu, b in self.regions:
            if gpu <= va < gpu + len(b):
                return gpu + len(b)
        return None


def walk_vs(data, start):
    """(shader address, size in instructions) of each SHADER_ADDRESS, and the draw count, from a VS command stream"""
    out = []
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    for i in range(0, len(w) - 1, 2):
        v1, v2 = w[i], w[i + 1]
        if v2 & 0xff000000 == 0x40000000:
            out.append(('shader', v1, (v2 & 0x0fffffff) >> 12))
        elif v2 & 0xff000000 == 0x30000000:
            out.append(('uniforms', v1, (v2 & 0x0fffffff) >> 12))
        elif v2 & 0xff0000ff == 0x20000000:
            out.append(('attributes', v1, (v2 & 0x0fffffff) >> 17))
        elif v2 & 0xff0000ff == 0x20000008:
            out.append(('varyings', v1, (v2 & 0x0fffffff) >> 17))
        elif v2 & 0xffff0000 == 0:
            out.append(('draw', v1, v2))
    return out


def walk_plbu(data):
    out = []
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    for i in range(0, len(w) - 1, 2):
        v1, v2 = w[i], w[i + 1]
        if v2 & 0xf0000000 == 0x80000000:
            out.append(('rsw', v1, (v2 & 0x0fffffff) << 4))
        elif v2 & 0xffe00000 == 0:
            out.append(('draw_arrays', v1, v2))
        elif v2 & 0xffe00000 == 0x00200000:
            out.append(('draw_elements', v1, v2))
        elif v2 & 0xff000fff == 0x10000109:
            out.append(('tiled_dimensions', v1, v2))
        elif v2 & 0xff000fff == 0x1000010c:
            out.append(('block_step', v1, v2))
        elif v2 & 0xf0000000 == 0x30000000:
            out.append(('block_stride', v1, v2))
        elif v2 & 0xff000000 == 0x28000000:
            out.append(('array_address', v1, v2))
    return out


def pp_instr_count(code):
    n, o = 0, 0
    while o < len(code) // 4:
        ctrl = struct.unpack_from('<I', code, 4 * o)[0]
        if ctrl & 0x1f == 0:
            break
        o += ctrl & 0x1f
        n += 1
    return n


def pp_code_len(mem, va, limit=16384):
    """the words of a PP program: each instruction's control word holds its length and the next one's (0 = the last)"""
    n = 0
    while n < limit // 4:
        w = mem.read(va + 4 * n, 4)
        if w is None:
            return None
        ctrl = struct.unpack('<I', w)[0]
        count, next_count = ctrl & 0x1f, (ctrl >> 19) & 0x3f
        if count == 0:
            return None
        n += count
        if next_count == 0:
            return n
    return None


def texdesc_fields(b):
    """the texture descriptor fields the comparison needs (genxml common.xml "Texture Descriptor")"""
    w = struct.unpack('<16I', b)
    bits = int.from_bytes(b, 'little')

    def f(word, bit, size):
        return (bits >> (word * 32 + bit)) & ((1 << size) - 1)
    return {'format': f(0, 0, 6), 'swap_rb': f(0, 7, 1), 'stride': f(0, 16, 15), 'has_stride': f(2, 8, 1),
            'min_lod': f(1, 12, 8), 'max_lod': f(1, 20, 8), 'mipfilter': f(2, 9, 2), 'min_nearest': f(2, 11, 1),
            'mag_nearest': f(2, 12, 1), 'wrap_s': f(2, 13, 3), 'wrap_t': f(2, 16, 3), 'width': f(2, 22, 13),
            'height': f(3, 3, 13), 'layout': f(6, 13, 2), 'mip0': f(6, 30, 26) << 6, 'unknown5': f(6, 15, 15),
            'words': ['%08x' % x for x in w]}


def rsw_fields(b):
    w = struct.unpack('<16I', b)
    return {'shader': w[9] & ~0x1f, 'first_len': w[9] & 0x1f, 'uniforms': w[11] & ~0xf, 'uniform_count': w[11] & 0xf,
            'textures': w[12] & ~0xf, 'sampler_count': (w[13] >> 14) & 0x3fff, 'varyings': w[15] & ~0xf,
            'varying_stride': (w[13] & 0x1f) << 3, 'early_z': (w[13] >> 9) & 1, 'pixel_kill': (w[13] >> 12) & 1,
            'blend': '%08x' % w[2], 'depth': '%08x' % w[3], 'words': ['%08x' % x for x in w]}


def run_parse(lp, kind, path, va, extra=None):
    if not lp:
        return ''
    cmd = [lp, kind, path, hex(va)] + ([str(extra)] if extra is not None else [])
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        return f'limaparse failed: {e}\n'


def decode_dumps(cap, lp):
    out = os.path.join(cap.dir, 'decoded')
    sdir = os.path.join(out, 'shaders')
    os.makedirs(sdir, exist_ok=True)
    shaders = {}   # (kind, hash) -> info
    textures = {}
    jobs_done = 0
    report = []
    for j in cap.jobs:
        snaps = [s for s in cap.snaps if s[0] <= j['t0'] and j['t0'] - s[0] < 2e9]
        if not snaps or 'vs' not in j:
            continue
        snap = snaps[-1]
        if cap.frame_of(snap[0]) != cap.frame_of(j['t0']):
            continue
        mem = cap.memory_at(snap)
        jobs_done += 1
        tag = f'job{jobs_done:03d}'
        jd = {'job': tag, 'frame': cap.frame_of(j['t0']), 'gp_ms': j.get('gp_ms'), 'pp_ms': j.get('pp_ms'),
              'num_cores': j.get('num_cores'), 'wb0': j['wb'][0] if 'wb' in j else None}
        jd.update(cap.counters(j))
        for name, (a, b) in (('vs', j['vs']), ('plbu', j['plbu'])):
            data = mem.read(a, b - a)
            if data is None:
                jd[name] = 'not in the snapshot'
                continue
            p = os.path.join(out, f'{tag}-{name}.bin')
            open(p, 'wb').write(data)
            open(p[:-4] + '.txt', 'w').write(run_parse(lp, name, p, a))
            jd[name + '_cmds'] = len(data) // 8
            if name == 'vs':
                vsw = walk_vs(data, a)
                jd['vs_draws'] = sum(1 for x in vsw if x[0] == 'draw')
                jd['gp_per_draw'] = []
                cur = None
                for kind, va, n in vsw:
                    if kind == 'draw':
                        jd['gp_per_draw'].append(cur)
                        continue
                    if kind != 'shader':
                        continue
                    cur = hashlib.sha1(mem.read(va, n) or b'').hexdigest()[:12]
                    code = mem.read(va, n)  # SHADER_ADDRESS carries the size in bytes (lima_gpu.h), 16 per instruction
                    if code is None:
                        continue
                    h = hashlib.sha1(code).hexdigest()[:12]
                    key = ('gp', h)
                    if key not in shaders:
                        sp = os.path.join(sdir, f'gp-{h}.bin')
                        open(sp, 'wb').write(code)
                        open(sp[:-4] + '.txt', 'w').write(run_parse(lp, 'gp', sp, va))
                        shaders[key] = {'kind': 'gp', 'hash': h, 'instr': n // 16, 'va': va, 'jobs': []}
                    shaders[key]['jobs'].append(tag)
            else:
                pw = walk_plbu(data)
                jd['plbu_draws'] = sum(1 for x in pw if x[0].startswith('draw'))
                jd['plbu_tiling'] = ['%s %08x %08x' % x for x in pw if x[0] in ('tiled_dimensions', 'block_step', 'block_stride')]
                rsws = []
                jd['pp_per_draw'] = []
                cur = None
                for kind, rva, gl_pos in pw:
                    if kind.startswith('draw'):
                        jd['pp_per_draw'].append(cur)
                        continue
                    if kind != 'rsw':
                        continue
                    cur = None
                    rb = mem.read(rva, 64)
                    if rb is None:
                        continue
                    rs = rsw_fields(rb)
                    rsws.append(rs)
                    rp = os.path.join(out, f'{tag}-rsw-{rva:08x}.bin')
                    open(rp, 'wb').write(rb)
                    open(rp[:-4] + '.txt', 'w').write(run_parse(lp, 'rsw', rp, rva))
                    n = pp_code_len(mem, rs['shader'])
                    if n:
                        code = mem.read(rs['shader'], 4 * n)
                        h = hashlib.sha1(code).hexdigest()[:12]
                        key = ('pp', h)
                        if key not in shaders:
                            sp = os.path.join(sdir, f'pp-{h}.bin')
                            open(sp, 'wb').write(code)
                            open(sp[:-4] + '.txt', 'w').write(run_parse(lp, 'fs', sp, rs['shader']))
                            shaders[key] = {'kind': 'pp', 'hash': h, 'words': n, 'instr': pp_instr_count(code), 'va': rs['shader'],
                                            'first_len': rs['first_len'], 'jobs': [], 'rsw': rs}
                        shaders[key]['jobs'].append(tag)
                        rs['pp_hash'] = h
                        cur = h
                    for t in range(rs['sampler_count']):
                        # the RSW's textures address holds one u32 address per sampler, each a 64-byte descriptor
                        pa = mem.read(rs['textures'] + 4 * t, 4)
                        if pa is None:
                            continue
                        dva = struct.unpack('<I', pa)[0]
                        db = mem.read(dva, 64)
                        if db is None:
                            continue
                        tf = texdesc_fields(db)
                        tk = hashlib.sha1(db).hexdigest()[:12]
                        if tk not in textures:
                            tp = os.path.join(out, f'tex-{tk}.bin')
                            open(tp, 'wb').write(db)
                            open(tp[:-4] + '.txt', 'w').write(run_parse(lp, 'tex', tp, dva, 0))
                            textures[tk] = dict(tf, jobs=[])
                        textures[tk]['jobs'].append(tag)
                jd['rsw'] = [{k: v for k, v in r.items() if k != 'words'} for r in rsws]
        if 'frame' in j:
            fp = os.path.join(out, f'{tag}-ppframe.bin')
            open(fp, 'wb').write(struct.pack('<23I', *j['frame']))
            open(fp[:-4] + '.txt', 'w').write(run_parse(lp, 'frame', fp, 0))
        report.append(jd)
    # Tie the blob's shaders to the app's programs: in each snapshotted frame the app's draws (R_GLDRAW, in call order)
    # and the draws of the frame's jobs (in submission order) are zipped. The blob submits a framebuffer's job when the
    # app moves to another framebuffer or swaps, so the orders agree; a frame whose counts differ is reported, not zipped.
    by_frame = collections.defaultdict(list)
    for d in cap.draws:
        by_frame[cap.frame_of(d[0])].append(d)
    jobs_by_frame = collections.defaultdict(list)
    for jd in report:
        jobs_by_frame[jd['frame']].append(jd)
    prog_map = collections.defaultdict(lambda: collections.Counter())
    res_frames = []
    for f, jl in sorted(jobs_by_frame.items()):
        hw = []
        for jd in jl:
            g, p = jd.get('gp_per_draw', []), jd.get('pp_per_draw', [])
            hw += [(jd['job'], g[i] if i < len(g) else None, p[i] if i < len(p) else None) for i in range(max(len(g), len(p)))]
        app = by_frame.get(f, [])
        ok = len(hw) == len(app)
        res_frames.append({'frame': f, 'gl_draws': len(app), 'hw_draws': len(hw), 'zipped': ok})
        if ok:
            for (job, gh, ph), d in zip(hw, app):
                prog_map[d[4]][(gh, ph)] += 1
    for prog, c in prog_map.items():
        for (gh, ph), n in c.items():
            for key in (('gp', gh), ('pp', ph)):
                if key in shaders:
                    shaders[key].setdefault('programs', set()).add(prog)
    for v in shaders.values():
        v['programs'] = sorted(v.get('programs', set()))
    for v in textures.values():
        v['jobs'] = sorted(set(v['jobs']))
    for v in shaders.values():
        v['jobs'] = sorted(set(v['jobs']))
    res = {'jobs': report, 'shaders': list(shaders.values()), 'textures': textures, 'frames': res_frames,
           'program_shaders': {str(p): [{'gp': k[0], 'pp': k[1], 'draws': n} for k, n in c.items()]
                               for p, c in prog_map.items()}}
    # GLSL per program, for the lima side (tools/malitrace/lima-compare.sh)
    progs = {}
    for prog, shs in cap.attach.items():
        progs[prog] = {'shaders': [{'id': s, 'type': cap.src.get(s, (0, ''))[0], 'glsl': cap.src.get(s, (0, ''))[1]}
                                   for s in shs],
                       'binary_bytes': len(cap.bins.get(prog, (0, 0, b''))[2]),
                       'binary_magic': cap.bins.get(prog, (0, 0, b''))[2][:4].hex()}
    res['programs'] = progs
    res['draws'] = [{'t': d[0], 'kind': d[1], 'mode': d[2], 'count': d[3], 'program': d[4], 'fbo': d[5]}
                    for d in cap.draws]
    json.dump(res, open(os.path.join(out, 'dumps.json'), 'w'), indent=1, default=str)
    return res


def write_glsl(cap):
    """decoded/glsl/prog-<program>.vert/.frag: the GLSL of every linked program, for limacompile"""
    g = os.path.join(cap.dir, 'decoded', 'glsl')
    os.makedirs(g, exist_ok=True)
    n = 0
    for prog, shs in cap.attach.items():
        parts = {cap.src[s][0]: cap.src[s][1] for s in shs if s in cap.src}
        if 0x8B31 in parts and 0x8B30 in parts:   # GL_VERTEX_SHADER, GL_FRAGMENT_SHADER
            open(os.path.join(g, f'prog-{prog}.vert'), 'w').write(parts[0x8B31])
            open(os.path.join(g, f'prog-{prog}.frag'), 'w').write(parts[0x8B30])
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--limaparse')
    ap.add_argument('--json')
    ap.add_argument('--skip', type=int, default=-1,
                    help='frames at the start left out of the timing (start-up); default 30, or a tenth of a short run')
    a = ap.parse_args()
    cap = Capture(a.dir)
    skip = a.skip if a.skip >= 0 else min(30, len(cap.swaps) // 10)
    s = cap.summary(skip)
    s['skipped_frames'] = skip
    s['glsl_programs'] = write_glsl(cap)
    if cap.snaps:
        d = decode_dumps(cap, a.limaparse)
        s['dumped_jobs'] = len(d['jobs'])
        s['shaders'] = {'gp': sum(1 for x in d['shaders'] if x['kind'] == 'gp'),
                        'pp': sum(1 for x in d['shaders'] if x['kind'] == 'pp')}
        s['textures'] = len(d['textures'])
    if a.json:
        json.dump(s, open(a.json, 'w'), indent=1)
    print(json.dumps(s, indent=1))


if __name__ == '__main__':
    main()
