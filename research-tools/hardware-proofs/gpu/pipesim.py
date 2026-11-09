#!/usr/bin/env python3
"""Frame-pipeline model of the Demo on Lima and a display broker (mali-re-sync spike, 2026-11-06). Throwaway.

    pipesim.py <blob-frames.json>

Per frame it replays the blob capture's own GPU jobs (hardware time from the counters: glow A, blur h B, blur v C,
on-screen D), scaled to lima per scene by the factors measured on the board (w375's GPUT, test slot 20261107T021545Z and
020041Z: lima PP per frame / blob PP per frame), and runs them through:
  - the CPU: CPU_MS of pure work per frame, the GL calls in the Demo's order (demo_scene.c demo_draw), with lima's job
    submissions where lima makes them (repro/splitrepro.c, measured on lima 26.2.6's drm-shim: A when blur h samples
    glow0, B when blur v samples glow1, C when the post pass samples glow0, D at the swap; galaxy splits D into D1/D2 at
    its second sprite upload) and lima's CPU waits (every glBufferSubData on the sprite/vertex VBO waits for the GP jobs
    that still read it: lima_transfer_map -> lima_bo_wait);
  - Mesa's swap throttle (dri_flush: swap n waits for frame n-1's last PP fence);
  - the GPU: one GP and one PP queue, FIFO (drm_sched, one job per ring); GP of job k waits for the PP of job k-NPLB
    (lima_update_job_bo: GP writes plb[plb_index] and gp_tile_heap[plb_index], PP reads them; plb_index turns per job);
  - the display broker: a POST is committed at once unless a flip is in flight (then at that flip); the kernel waits for
    the buffer's fence and flips at the first vblank after it; DONE at the flip event (display.c commit/on_flip);
  - presentation policy: 0 = today (the presentation runtime waits for the DONE of the buffer it just posted), 1 = the patch (waits
    for the DONE of the buffer posted before it, so one frame is queued).
Prints fps per scene (the Demo's own measure: the interval between swap returns).
"""
import json
import sys
import statistics

V = 1000.0 / 51.0          # 19.61 ms vblank
EPS = 0.3                  # a fence this close before a vblank misses it (commit_tail, register latch)
EVT = 0.2                  # flip event -> DONE at the client
GP_F = 1.11                # lima GP / blob GP (ignition, GPUT 020041Z: 4.54 vs 4.08 ms a frame)
# lima PP / blob PP per frame, measured per scene (GPUT pp ms per second / frames, 021545Z and 020041Z); galaxy's 26.7
# includes lima's job split (below), modelled apart: its base factor is the tunnel/chrome mean
PP_F = {'ignition': 1.22, 'tunnel': 1.40, 'chrome': 1.16, 'galaxy': 1.28, 'horizon': 1.10, 'finale': 1.28}  # horizon: fitted to its measured 50 fps today; finale: unmeasured (a guess)
SPLIT_W, SPLIT_R = 4.2, 5.7  # galaxy's split: D1 writes colour + Z16 back (5.5 MB), D2 reloads both (5.5 MB + reload draw)
CPU_MS = 3.6               # the Demo's CPU per frame at 1008 MHz without waits (work_ms 3.2-4.5 in horizon, 021545Z)


def simulate(frames, policy, nplb, split_fix, cpu_ms=CPU_MS):
    gp_free = pp_free = 0.0
    gp_done, pp_done = {}, {}
    k = 0
    t = 0.0
    prev_fence = 0.0
    flip_prev = -V
    done = {}
    swaps = []
    vbo_readers = []       # GP jobs of the last frame that read the per-frame VBO
    for n, f in enumerate(frames):
        sc = f['scene']
        pf, jobs = PP_F[sc], f['jobs']
        A, B, C, D = [(j['gp'] * GP_F, j['pp'] * pf) for j in jobs]
        split = sc == 'galaxy' and not split_fix
        c = cpu_ms

        def submit(gp, pp, at):
            nonlocal gp_free, pp_free, k
            dep = pp_done.get(k - nplb, 0.0)
            s = max(at, gp_free, dep)
            gp_done[k] = gp_free = s + gp
            pp_done[k] = pp_free = max(gp_free, pp_free) + pp
            k += 1
            return k - 1

        def vbo_wait(at, readers):
            return max([at] + [gp_done[r] for r in readers]) if not split_fix else at

        t = vbo_wait(t, vbo_readers)          # glow pass: the sprite/vertex upload waits for last frame's readers
        t += 0.15 * c
        a = submit(*A, t)                     # blur h samples glow0
        t += 0.05 * c
        submit(*B, t)                         # blur v samples glow1
        t += 0.10 * c
        t = vbo_wait(t, [a])                  # picture: upload waits for the glow job's GP
        if split:
            t += 0.40 * c
            d1 = submit(D[0] * 0.6, D[1] * 0.5 + SPLIT_W, t)   # the second upload flushes the picture so far
            t = vbo_wait(t, [d1])
            t += 0.25 * c
            submit(*C, t)                     # post samples glow0
            t += 0.05 * c
            d2 = submit(D[0] * 0.4, D[1] * 0.5 + SPLIT_R, t)
            readers, last = [d1, d2], d2
        else:
            t += 0.65 * c
            submit(*C, t)
            t += 0.05 * c
            last = submit(*D, t)
            readers = [last]
        vbo_readers = readers
        fence = pp_done[last]
        # swap: Mesa throttles on the previous frame's fence, then the presentation runtime posts
        t = max(t, prev_fence)
        prev_fence = fence
        post = t
        commit = max(post, flip_prev)
        ready = max(commit, fence) + EPS
        flip = (int(ready // V) + 1) * V
        done[n] = flip + EVT
        flip_prev = flip
        if policy == 0:
            t = done[n]
        else:
            t = max(t, done.get(n - 1, 0.0))
        swaps.append((sc, t))
    return swaps


def fps_by_scene(swaps):
    out = {}
    for (s0, t0), (s1, t1) in zip(swaps, swaps[1:]):
        if s0 == s1:
            out.setdefault(s1, []).append(t1 - t0)
    return {s: (1000.0 / statistics.mean(v), sum(1 for x in v if x > 1.5 * V) / len(v)) for s, v in out.items()}


def main():
    fr = json.load(open(sys.argv[1]))
    fr.sort(key=lambda f: f['song'])  # one pass through the loop in song order (the capture wrapped once)
    scenes = ['ignition', 'tunnel', 'chrome', 'galaxy', 'horizon', 'finale']
    cases = [('today: wait own DONE, 2 PLBs, split', 0, 2, False),
             ('today + LIMA_CTX_NUM_PLB=4', 0, 4, False),
             ('presentation depth 1, 2 PLBs, split', 1, 2, False),
             ('presentation depth 1, LIMA_CTX_NUM_PLB=4, split', 1, 4, False),
             ('presentation depth 1, 8 PLBs (patch), split', 1, 8, False),
             ('today + app VBO fix', 0, 2, True),
             ('presentation depth 1, 2 PLBs, app VBO fix', 1, 2, True),
             ('presentation depth 1, 4 PLBs, app VBO fix', 1, 4, True),
             ('presentation depth 1, 8 PLBs, app VBO fix', 1, 8, True)]
    print('lima model, fps (late share) per scene; measured today (021545Z): ignition ~51, tunnel/chrome/galaxy 25.5-26 '
          '(all late), horizon 34->50')
    print('%-42s ' % 'case' + ' '.join('%-14s' % s for s in scenes))
    for name, pol, nplb, fix in cases:
        r = fps_by_scene(simulate(fr, pol, nplb, fix))
        print('%-42s ' % name + ' '.join('%5.1f (%3.0f%%)  ' % (r[s][0], 100 * r[s][1]) if s in r else '%-14s' % '-' for s in scenes))
    print()
    print('sensitivity: CPU per frame 6.3 ms (the 600 MHz run, 20261107T014341Z)')
    for name, pol, nplb, fix in cases[:1] + cases[3:4] + cases[7:]:
        r = fps_by_scene(simulate(fr, pol, nplb, fix, cpu_ms=6.3))
        print('%-42s ' % name + ' '.join('%5.1f (%3.0f%%)  ' % (r[s][0], 100 * r[s][1]) if s in r else '%-14s' % '-' for s in scenes))


if __name__ == '__main__':
    main()
