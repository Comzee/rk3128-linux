# CPU, DDR, DMA and memory

## CPU and regulators

RK3128 has four Cortex-A7 cores, per-core 32 KiB instruction/data L1, shared 256 KiB L2, NEON and VFPv4. The measured RK3128 reference board baseline supports 216–1008 MHz CPU DVFS. Its rails use PWM regulators, with **higher duty meaning lower voltage**; reversing that table can overvolt the chip.

The board data records 25,000 ns PWM periods and a 950–1450 mV table. The stock integer relationship is `duty_percent = floor((1450000 - requested_uV) / 5500)`. The mainline PWM regulator table representation has its own scaling behavior; the supplied DT/checker reflects that, rather than naively copying the vendor coefficient into a different binding.

Patch 638 adds an optional **kernel-enforced** RK3128 reference board busy-core policy. It defaults off at a 1008 MHz ceiling. When explicitly enabled, the stock busy-core ceilings are **1200 / 1008 / 912 / 912 MHz** for one through four busy cores, sampled every 100 ms. The 1200 MHz point requires **1.425 V**; 912 MHz uses the stock next-OPP voltage **1.35 V**. Classification failure or CPU-online changes select the conservative four-core cap. This is a recovered board policy, not a thermal sensor/model or an unconditional 1.2 GHz overclock. Do not apply the voltage/boost settings to another board without its electrical evidence.

## DDR396 and execution from SRAM

The loader leaves DDR at approximately 300 MHz. The working board profile changes to **396 MHz** after setting **vdd_logic to 1.25 V**, matching its stock Android configuration. This is a board-specific, narrow DDR3 transition; it is not a generic DDR devfreq implementation.

Patch `379-0002` validates the detected geometry and controller state, prepares timing/training information and performs the self-refresh/PLL/PHY transition from SRAM. The measured geometry is one chip select, row 15, eight banks, column 10, 32-bit width, 1 GiB. At 396 MHz the recorded DDR3 timing includes CL6, CWL5 and tRFC119 for the fitted 4 Gb dies. Raising the clock on a different memory population is not established by these numbers.

The critical sequence idles NoC ports, enters controller configuration, updates timing/mode state, enters self-refresh, adjusts DPLL/PHY state, trains, returns to access and releases the ports. It uses bounded handshakes and stops competing CPUs. Timeout/error paths must remain intact. There is no safe assumption that all other CPUs or interrupt handlers can continue to fetch DRAM while the transition runs.

Two SRAM functions must be self-contained: no call, literal/global reference, tracing thunk, instrumentation or stack-protector dependency into inaccessible DRAM. The original build audited object relocations and indirect calls in those functions. **A successful kernel compile is insufficient:** inspect `drivers/soc/rockchip/rk3128-dmc.o` with the cross `objdump`/`readelf` and prove that the copied ranges have no such external dependency. Preserve this audit when changing compiler flags or rebasing.

## CMA and direct video

The supplied integration uses a **160 MiB reusable CMA pool**. This supports physically contiguous decoder/scanout surfaces on a VOP path without an enabled IOMMU. The VPU/HEVC MMUs remain described but disabled in this profile; the decode buffers can then be imported for direct scanout without a full-frame copy. Other board designs may choose different IOMMU policy, but must prove end-to-end buffer layout/import and bus arbitration.

CMA is reusable memory and already contributes to MemTotal. It is not the same as a permanent `no-map` hole. RAM accounting must union overlapping reserved ranges, inspect runtime PageReserved state and distinguish boot-logo pages already returned after DRM handoff. `linux/tests/memory/ram-account.c` implements a read-only accounting probe using `/proc/kpageflags` and DT data, with no `/dev/mem` dependency.

## DMA cache coherency

Patch `395-0002-dma-direct-no-kernel-mapping-on-arch-dma-alloc.patch` addresses a 32-bit ARM noncoherent allocation problem for `DMA_ATTR_NO_KERNEL_MAPPING`: stale CPU cache lines could later overwrite freshly allocated DMA output. The symptom was corrupted first JPEG decodes and reference-video frames; it was not fixed by altering colour conversion. A finite reproducer changed from **28 bad decodes out of 70** to **0 out of 70** with the allocation/cache fix. Carry correct DMA-API synchronization and buffer ownership through every hardware path rather than introducing userspace physical-address shortcuts.

## Cortex-A7 memory routines

`linux/userspace/rk3128-mem` contains standalone `fm_memcpy`/`fm_memset`, with small-copy cases, NEON unaligned paths and aligned LDM/STM blocks. `memcpy` retains the C nonoverlap contract; `memmove` is untouched. It is ARMv7-specific source, not a portable replacement for libc on every platform.

Build the finite guard/alignment test with a hard-float cross compiler, NEON flags, `-fno-builtin` and `-fno-tree-loop-distribute-patterns`; run under qemu-arm or on compatible hardware. The test checks 97,362 copy/set combinations across sizes and offsets, compares against byte-loop references and checks surrounding guard bytes. Optional preload exports exist, but global deployment policy is deliberately outside this reference.
