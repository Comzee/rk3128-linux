# Historical experiments

These patches are **outside the default kernel and userspace patch series**. They preserve useful implementation alternatives, instrumentation and negative results. They target their recorded intermediate source state and may require a manual rebase; they are not a second supported release stack. Do not combine all experiments or assume that a writable control has a verified hardware contract.

| Directory | Baseline and purpose | Status |
|---|---|---|
| `gpu-demo-gap/kernel/` | Early Lima task dispatch, cache maintenance, counter and runtime-switch experiments against the intermediate kernel driver | Superseded by the accepted `linux/patches/478-*` series. Retained for controlled comparison; not extra fixes to apply afterward. |
| `gpu-demo-gap/shader/` | Aligned fragment-varying layout against Mesa 26.2.6 plus the early local Lima changes | Superseded by the corresponding public userspace patch. |
| `gpu-demo-gap/reload/` | Busy-buffer renaming against the intermediate Mesa source | Superseded by the accepted buffer-renaming patch. |
| `gpu-demo-gap/midpass/` | Mesa 26.2.6 plus local patches 0001–0007; runtime switches for reload, depth and dependency behavior | Instrumentation and rejected/alternative modes. No blanket image-quality or speed acceptance. |
| `gpu-demo-gap/sync/` | Eight PLBs against the intermediate Lima source | Experimental allocation/synchronization tradeoff. The accepted default uses four PLBs. |
| `gpu-beat-blob/` | Additional Lima arithmetic/dither changes against Mesa 26.2.6 and the local varying/compiler series | Excluded from default: pixel changes require separate assessment. Neither instruction-count reduction nor a successful compile establishes equivalent output. |
| `crypto-init/` | Word-copy and transition trace instrumentation against an intermediate RK3128 crypto implementation | Diagnostic alternatives. They do not admit cipher DMA or runtime power gating; the guarded default remains hash DMA, health-tested RNG and low-priority PIO ciphers. |
| `iep-620/` | Intermediate IEP table readback/programming controls | Diagnostic-only interface superseded by the accepted bounded IEP contract. Larger spatial radii remain refused. |
| `compiler/` | A measured intermediate hardening/configuration comparison | Only the kernel configuration and SSV source portions are retained; launcher, application service and native runtime changes are outside this collection. Current accepted hardening source remains authoritative. |

The historical numerical checks and synthetic inputs are under `../hardware-proofs/`. Experimental success is bounded by the stated interface and tested cases. No experiment here changes the production admission rules documented with the main drivers.
