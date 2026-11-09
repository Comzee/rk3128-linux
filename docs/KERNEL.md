# Kernel and device-tree integration

The recorded kernel baseline is **Linux 7.3.1**. The patch stack is not promised to apply to 6.6, 6.18 or another stable release. In particular, the HEVC backend depends on rkvdec infrastructure introduced after the earlier LTS baseline. Native code is ARMv7 hard-float with NEON; cross builds use `ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-`.

This repository supplies source inputs and component recipes, not an image/deployment builder. Nothing here creates a Debian system image or changes a device's partitions.

## Patch ordering

[manifests/kernel.series](../manifests/kernel.series) is authoritative: the active project patches are lexical by filename. The `zz-761` runtime-PM diagnostic follow-up deliberately comes **after** `764-crypto-transition`; sorting numeric ticket prefixes would break its dependency.

The separate `linux/upstream/292-0004` DT addition is a historical alternative listed in `manifests/kernel-historical.series`; the current `rk3128-display-lvds.dtsi` already supplies the LVDS node and graph, so the active recipe does not apply that patch. Touch calibration and the point-algorithm patch numbered 587 are both included.

Example in a fresh source tree, with `reference` set to this checkout's absolute path:

```sh
while IFS= read -r patch_name; do
    patch --batch --forward -d "$kernel" -p1 < "$reference/$patch_name" || exit 1
done < "$reference/manifests/kernel.series"
```

Use a clean tree, preserve the recorded order and stop at the first failure. Do not reuse name-only “already applied” markers after changing inputs. Hash the complete patch/config/DTS input set when caching a prepared tree.

## Assembling the device tree

`linux/dts/rk3128-reference.dts` is a **base board description**. The complete integration is a preprocessor wrapper: define the selected `RK3128_*` values, include a renamed copy of that base, then include every `rk3128-*.dtsi` in lexical order. The fragments add media, regulators, input, USB and scanout policy; compiling the base alone loses those changes. The `rockchip,rk3128-reference*` compatible names describe this local reference profile, not an official upstream board identity. The root description retains the `rockchip,rk3128` fallback; review the documented wiring and electrical constraints when adapting it.

The helper below writes the board wrapper and fragment copies into an output directory, using the kernel tree only to check the expected RK3128 source path. It does not apply patches, select Kconfig, build, pack or install anything:

```sh
python3 scripts/prepare-dts.py --kernel "$kernel" --output /var/tmp/rk3128-board
```

Read its emitted wrapper before copying it and the emitted fragments into `arch/arm/boot/dts/rockchip/`. Add a local Makefile entry for `rk3128-reference.dtb` if absent. The helper defaults to the recorded DRM/LVDS profile, PWM regulators enabled, DDR396, DMA eMMC DDR52 and guarded crypto hash/PIO. These are **RK3128 reference board-specific selections**, not safe defaults for unknown boards. Review [CPU/memory](CPU-MEMORY.md) before using them.

The original base file still has conservative bring-up defaults (simplefb, regulators disabled, DDR unchanged). The wrapper deliberately overrides these. Other `RK3128_*` definitions have local fallback defaults in their topic fragment. Inspect the emitted wrapper when adapting a board rather than inferring a profile from a filename.

## Configuration fragments

Start with `allnoconfig` and merge **`linux/config/rk3128.config` first**. Merge the remaining `linux/config/*.config` files in lexical order, excluding that base file, then selected `knobs/*.config` last. Do not sort the base into the topic list or merge it a second time: its reset values can disable options enabled by earlier topic fragments. Fragments are reference policy, including cgroups, diagnostics and filesystem options; you may select a smaller board-specific subset. They are not a universal RK3128 defconfig.

For the emitted LVDS/guarded-crypto profile, the additional knobs are:

- `linux/config/knobs/display-lvds.config`
- `linux/config/knobs/crypto.config`
- `linux/config/knobs/crypto-hash-pio.config`

Do not merge every knob: diagnostic/full/module/rng profiles select alternative crypto paths; QEMU/debug settings are separate test profiles. Finish with upstream `olddefconfig`, then inspect the resulting `.config`. Verify each requested `CONFIG_*=y` survived dependency resolution; successful fragment merging alone does not establish that its symbols are active.

Build with the upstream kernel commands for `zImage` and `rockchip/rk3128-reference.dtb`. A generated cmdline fragment belongs **last** when you supply one.

## Boot format and early debugging

The fitted vendor bootloader does not honor the Android boot header command line as a normal upstream bootloader would. Kernel arguments must be compiled with `CONFIG_CMDLINE` and `CONFIG_CMDLINE_EXTEND`. Its existing arguments consume much of the 1024-byte ARM command-line buffer; the measured integration budget was about **337 additional bytes**, so count the actual inherited string on your device.

`tools/rkbootimg.py` handles the observed Rockchip boot-image header/id, DTB carried as an `rk-kernel.dtb` entry in an RSCE resource image in the boot header second stage and alignment checks. `tools/fdt.py` parses FDT read-only. Neither implies generic partition offsets or a supported flashing procedure. Preserve your loader, parameter, trust and bootloader regions; establish partition names/geometry from the actual device. The historical Rockusb helper includes low-level write/reset commands and must not be used as an automatic update route.

Early-debug patches provide bounded RAM breadcrumbs, an early RAM console, watchdog setup and ramoops. The RK3128 reference board's preserved ramoops region is **512 KiB at 0x7f100000**; u-boot's logo allocation is separate at **0x9dc00000, 32 MiB**. These addresses are board/loader measurements. Verify reserved-memory overlap before reusing them.
