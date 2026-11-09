# Mali-400 MP2: Lima and Mesa

The fitted GPU is **Mali-400 MP2**, driven by upstream **lima** and Mesa's lima Gallium driver. It provides GLES 2.0; it is not a Vulkan GPU. The measured bus/GPU clock is **297 MHz** even when a nominal configuration requests 300 MHz. Report actual clocks in performance comparisons.

## Kernel patches

The `478-0101` through `478-0111` series records the practical MMU/cache/scheduler changes developed against this board: L2 MAX_READS observation/override, remembered page-directory ownership, page-table cache invalidation, conditional L2 work, ordered relaxed writes, deferred next-task scheduling from IRQ, workqueue priority and GP/PP hardware counters. Later fixes preserve deferred-task ordering and quiesce deferred starts before a timeout reset. Do not cherry-pick the early scheduling optimization without its ordering/reset follow-ups.

The diagnostic MAX_READS override is an experiment mechanism, not a universal tuning recommendation. Correctness still requires completed L2 commands, correct MMU page-table visibility and bounded timeout/reset ownership. Patch 604 adds client engine-time/memory fdinfo accounting; patch 666 adds a buffer-memory snapshot. GP and PP load describe different engines and cannot be treated as a single CPU-like utilization number.

## Mesa stack

The reference pin is Mesa **26.2.6**. Apply `linux/userspace/mesa/series` in order to the pinned source. A minimal hardware profile builds the lima driver; kmsro's winsys support is built with DRM drivers and is not a separate `-Dgallium-drivers=kmsro` selection in this release. LLVM is unnecessary for this chosen hardware-only configuration; that is not a claim about every Mesa build.

| Patch group | Purpose |
| --- | --- |
| 0001–0002 | FP16 varying preparation and its build/control-flow fix; also addresses the recorded large-texture sampling issue |
| 0003–0005 | Split input loads, packed PP uniforms and aligned whole-slot fragment varyings |
| 0006–0007 | Rename still-busy partially updated buffers instead of forcing a flush; four PLBs by default for overlap |
| 0010–0015 | Result combiner multiply scheduling, preserve partially constant varying slots, selective FP16 varying storage, tighter varying stride, GP critical-path slack and texture-read scheduling |
| 0016 | Lazy GLSL builtin work on shader-cache hits |

Some compiler changes are intentionally not bit-identical: selective FP16 colour varyings and partial-constant interpolation moved measured scene values by up to **3/255**. Texture-coordinate precision is deliberately protected. Other rejected arithmetic/dither experiments are not included as accepted patches. Numerical comparison, instruction counts, actual GP/PP counters and displayed output are separate checks; a reduction in instruction count does not establish image correctness or end-to-end frame delivery.

## Measured graphics improvements

The compiler comparison used the same 17 captured demo fragment programs: unmodified Mesa 26.2.6 and the checked upstream development snapshots produced **76 instructions in total**, versus **42** with the accepted compiler patches (about **45% fewer**). This is a static sum across those programs, not a dynamic instruction rate or a 45% frame-rate claim. Some accepted precision changes have the numerical limits described above.

The historical frame-rate comparison used a six-scene, 48-second demo loop, rendering internally at **1280×720** and scaling to a **1920×1080** display. GPU, DDR and CPU clocks stayed at **297 MHz, 396 MHz and 1008 MHz**. Three bottlenecked scenes ran at **25.5, 25.5 and 25.4 FPS** in the earlier custom Mesa 26.2.6 integration, which already carried patches 0001–0004 and used blocking presentation. The optimized full graphics stack reached **51 FPS in all six scenes**, with no frame later than 1.5 refresh intervals in that recorded loop. It combined the Mesa changes with one-frame-ahead presentation in an application runtime **outside this collection**; this is not an isolated driver-patch or stock Debian benchmark.

A controlled comparison kept that one-frame-ahead runtime and all clocks unchanged: adding only the included Mesa **0006 busy-buffer rename** patch raised one scene from **34.7 to 48.9 FPS (+41%)**, while fragment-processor job wall time fell from **25.6 to 14.7 ms per frame**. The four-PLB and shader changes then brought that scene to the display's approximately 51 Hz cadence. These are workload-specific results, not general throughput guarantees for every Mali GPU or application.

## Historical driver observation tools

`research-tools/malitrace` contains source for a historical **stock Mali ioctl interceptor**, capture decoder and Mesa shader compiler/comparison helpers. It is useful for observing a binary driver's hardware contract when you possess that driver under its own terms. No proprietary driver or captured executable is distributed here. These tools are not a native-Linux runtime dependency and do not install a vendor GPU stack.

The recorder/compiler's old comments may refer to historical capture layouts or helper scripts outside this collection. The programs operate on explicit capture/shader inputs; adapt their paths and Mesa drm-shim profile to your environment. A Mali-450 shim profile does not substitute for the measured Mali-400 MP2 when comparing GP/PP instruction scheduling.
