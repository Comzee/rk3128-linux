# Source origins and license scope

This RK3128 hardware source reference preserves original author attribution
and third-party license notices. The sections below describe the licenses and
upstream origins of its kernel, userspace and diagnostic components.

## Kernel and board support

The patches target the Linux kernel pinned in `linux/bases/`. Linux target files
retain their original licenses, copyright notices and license alternatives.
New original kernel code is GPL-2.0-only unless its header says otherwise.
Historical GPL-2.0 and GPL-2.0+ headers remain unchanged. Bindings commonly retain
GPL-2.0-only OR BSD-2-Clause; UAPI headers retain the Linux-syscall-note exception.
Lima kernel target files include GPL-2.0 OR MIT grants. Attribution for patch
context is collected in [kernel-targets.txt](notices/kernel-targets.txt).

Specific source origins include:

- RK3128 DDR3 controller support: Rockchip's `ddr_rk3126.c` in
  `rockchip-linux/kernel`, release-3.10; Copyright (C) 2012 Fuzhou Rockchip
  Electronics Co.,Ltd; authors hcy, yk and typ at rock-chips.com. The port
  preserves its GPL-2.0 provenance and adds bounded synchronization and checks.
- IEP register programming: Rockchip's GPL-2.0 IEP driver,
  Copyright (C) 2013 ROCKCHIP, Inc.; adapted into a V4L2 memory-to-memory driver.
- G1 JPEG decoder: STMicroelectronics' GPL-2.0 downstream Linux driver,
  Copyright (C) STMicroelectronics SA 2024, adapted for RK3128.
- Panel source ancestry: Copyright (C) 2016 Laurent Pinchart and
  Copyright (C) 2016 Renesas Electronics Corporation; retained in the patch.
- Hantro H1 encoding: Rockchip Electronics Co., Ltd. 2018 attribution remains
  in the patch. The embedded WebM probability table is BSD-3-Clause,
  Copyright (c) 2010 The WebM Project authors; its full notice stays in patch 736.

## Wi-Fi reference source

`linux/modules/ssv6x5x` derives from
[cdhigh/armbian_sv6256p](https://github.com/cdhigh/armbian_sv6256p), revision
`43c2ede`, SDK generation SMAC.0000.1807.12.SPUR.90, with the owner's API,
configuration, scanning and hardware adaptations. Original iComm-semi Ltd.
Copyright (c) 2015 notices and individual grants are retained.

Many files explicitly use GPL-3.0-or-later and the directory's `LICENSE` is the
GPL version 3 text. Other files have GPL-2.0-or-later or GPL-2.0 OR BSD-3-Clause
grants. Kernel module metadata such as `MODULE_LICENSE("GPL")` is not a
replacement for those source grants. This separately collected reference source
does not assert that its GPLv3 portions form a GPLv2-compatible combined kernel.

The firmware executable is not included. The example configuration is written
from hardware parameter facts; it does not copy the vendor configuration's prose
or device identifiers. Supply separately licensed firmware for your hardware.

## Userspace patches

Accepted and experimental userspace patches retain their target-file grants,
including the original changes. Upstream file notices for the code context
distributed in patch files are in
[userspace-upstream.txt](notices/userspace-upstream.txt). Keep them together with
the patches; hunk context often omits the original top-of-file notices.

- Mesa Lima/NIR/GLSL patch targets use MIT grants. Original notices include the
  Lima Project, Luc Verhaegen, Vasily Khoruzhick, Connor Abbott, Advanced Micro
  Devices, Intel and the other file authors. Upstream patch author and revision
  metadata is retained.
- ALSA PCM patches follow LGPL-2.1-or-later, retaining Jaroslav Kysela and the
  target files' original notices.
- GStreamer modified files retain their GNU Library GPL version 2 or later
  grants (LGPL-2.0-or-later). The release's COPYING also supplies LGPL version
  2.1. Both license texts are included. Original file and backport authors
  remain attributed; upstream CVE patch metadata is preserved.
- WPA supplicant patches follow BSD-3-Clause. Jouni Malinen and contributors'
  copyright and full license are in
  [wpa-supplicant.txt](notices/wpa-supplicant.txt). Its former GPL option was
  retired upstream in 2012; no GPL-only relabeling is intended.

## Standalone tools and examples

Existing MIT identifiers on RGA/IEP/crypto probes, memory accounting and JPEG
RGA helpers are retained. The RGA UAPI header retains GPL-2.0 WITH
Linux-syscall-note. VEPU tools and kernel test modules retain their GPL grants.
Other original supplemental code and documentation uses GPL-2.0-only as stated
in LICENSE. The source manifest records the applicable category and any explicit
identifiers without replacing per-file notices.

The Hantro JPEG helper adapts stb_image v2.30's resampling and YCbCr-to-RGB
conversion. The MIT option is selected for those portions:
Copyright (c) 2017 Sean Barrett, full notice in
[stb-image.txt](notices/stb-image.txt). Original helper portions follow LICENSE;
separately MIT-marked RGA helper files keep their existing grant.

## Documentation image

The original screenshot contribution in `assets/images/fastfetch-rk3128.png`
is licensed under GPL-2.0-only, following the documentation grant in LICENSE.
Names and logos visible in the screenshot retain their owners' rights.

## External inputs

Original implementations, including touchscreen calibration and point processing
in patches 579 and 587, are published with the owner's authorization under the
stated open-source grants. Source licenses do not license separately supplied
vendor inputs.

Firmware executables, Android images, proprietary source, manual PDFs and raw
memory dumps are not supplied. Extractors and optional oracle tools require
independently supplied inputs. Hardware notes describe the supported registers,
protocols, settings, observed behavior and limitations.
