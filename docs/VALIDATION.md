# Component validation record

These checks ran on the selected source snapshot on **2026-11-09**. They establish the specific compilation and host-test results below. They do not establish a complete kernel build, a Debian system image, a device boot or hardware correctness on another board. Historical device measurements are described separately in the topic notes.

## Kernel and board description

The Linux **7.3.1** archive matched the SHA256 in `linux/bases/mainline-7.3.env`. The selected kernel patches applied with GNU `patch --batch --fuzz=0 -p1` to a fresh extraction. The active manifest and historical alternatives are distinguished in [the kernel recipe](KERNEL.md).

Nine selected ARM kernel objects compiled successfully. Configuration started with `allnoconfig`, then merged `rk3128.config`, the other top-level `linux/config/*.config` fragments in lexical order, and these selected knobs: `display-lvds.config`, `crypto.config`, `crypto-hash-pio.config`, `root.config`. Configuration finished with `olddefconfig`. This command compiled the selected objects:

```sh
make -s -C "$kernel" O="$objects" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j8 \
    drivers/input/touchscreen/silead.o \
    drivers/pmdomain/rockchip/pm-domains.o \
    drivers/soc/rockchip/rk3128-dmc.o \
    drivers/gpu/drm/rockchip/rockchip_drm_vop.o \
    drivers/gpu/drm/rockchip/rockchip_vop_reg.o \
    drivers/gpu/drm/rockchip/rockchip_lvds.o \
    drivers/media/platform/verisilicon/hantro_h1_h264_enc.o \
    drivers/media/platform/verisilicon/hantro_h1_vp8_enc.o \
    drivers/crypto/rockchip/rk3128-crypto.o
```

The touchscreen object includes calibration and point processing. Cross `objdump` inspection found no ARM relocations or indirect calls in the copied `rk3128_dmc_sram_run` and `rk3128_dmc_sram_park` ranges. That static check is necessary when changing compiler flags; compilation alone cannot prove a DDR transition electrically safe.

The wrapper emitted by `scripts/prepare-dts.py` contains all 23 profile definitions and 21 board/fragment includes. The full LVDS-enabled RK3128 reference board board wrapper and sorted fragments were preprocessed against patched kernel headers and compiled with `dtc`. The three unit-address warnings were `/memory` and the two ramoops pad nodes. This was a DT compilation check, not binding-schema validation. Both `rk3128_board_dts_check.py --expect okay` and `rk3128_pmic_dts_check.py` passed for that DTB.

## Standalone C sources

ARM object compilation passed for these fourteen sources:

- `tools/vepu-encode.c`, `tools/vop-colour-read.c`, `tools/crypto-test.c`, `tools/crypto-probe.c`.
- `tools/rga1-test.c`, `tools/rga1-probe.c`, `tools/rga1-rest.c`.
- `tools/iep-test.c`, `tools/iep-gpu-bench.c`.
- Both C sources in `examples/hantro-jpeg/`.
- `linux/fbtest/fbtest.c`, `linux/tests/memory/ram-account.c`, `linux/tests/launch/drm_handoff_probe.c`.

The invocation used `arm-linux-gnueabihf-gcc`, GNU C11, `-O2 -Wall -Wextra -mfpu=neon -marm`, and the required development-header include paths. This was object compilation, without linking ARM GPU libraries or executing device operations.

`rk3128bench` separately compiled and linked statically with its `rk3128-mem.o` dependency using:

```sh
arm-linux-gnueabihf-gcc -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -fno-builtin -fno-tree-loop-distribute-patterns -Wall -Werror \
    -c linux/userspace/rk3128-mem/rk3128-mem.c -o rk3128-mem.o
arm-linux-gnueabihf-gcc -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -static -pthread -Wall -Werror linux/tests/cpu/rk3128bench.c rk3128-mem.o -o rk3128bench
```

The VFPv4 selection is required by the benchmark's inline fused multiply-add instructions. The randomized `rk3128-mem/test.c` guard/alignment/content test linked with that object and ran under `qemu-arm`: **97,362 checks passed**.

## Host checks of driver behavior

The relocated touchscreen `point-sim.sh` ran through `bash`, using AddressSanitizer and UndefinedBehaviorSanitizer. Controller-state, gesture, packet-decoding, truncated/oversized-report and release assertions passed. The deterministic corpus had **21,763 records** and matched the recorded digest for all outputs and 300 history words per report. This replay used the included expected digest; no external device image was executed for this check.

The relocated `lima-timeout-sim.py` checked functions from the prepared kernel tree against ownership/ordering mocks: **nine recovery interleavings plus GP/PP accounting passed**, with AddressSanitizer and UndefinedBehaviorSanitizer. These tests exercise selected driver logic, independently of GPU hardware.

## Shader and boot-container checks

The standalone `linux/tests/launch/shader_probe.c` compiled and linked on the host with `-O2 -Wall -Wextra -Werror` and `pkg-config --cflags --libs egl glesv2 gbm`. Running with `LIBGL_ALWAYS_SOFTWARE=1` and `--surfaceless` passed on **llvmpipe, Mesa 26.0.8**. The assertions cover red-pixel rendering, builtin shader functions, rejection of invalid GLSL, compiler release, repeated compilation/linking and a changed attribute binding. This new standalone context setup was not executed on Mali-400 hardware during publication checks.

`rkbootimg.py` passed a synthetic fixture check: repeated newc/gzip/boot-container builds were byte-identical, the embedded file hash matched, and header/id/FDT/alignment validation accepted the output. Deliberately corrupting a kernel payload byte was rejected. The synthetic kernel carried only a format marker; it was never booted.

Firmware, full upstream source trees and generated test outputs are outside the distributed source collection.
