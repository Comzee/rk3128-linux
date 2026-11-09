# Tools, examples and validation boundaries

The source collection includes small hardware tools rather than the original product services. Most expect explicit input/device paths; run `--help` where available or read the usage comment. A number are hardware probes that necessarily change the selected device's state. Compiling or parsing them on a host does not prove hardware behavior.

## Tool map

| Path | Purpose / dependency |
| --- | --- |
| `tools/rkbootimg.py`, `tools/fdt.py` | Rockchip boot-container and read-only DT/FDT inspection; Python standard library |
| `tools/ramoops_decode.py` | Decode a caller-supplied preserved RAM region; Python standard library |
| `tools/rkusb.py` | Historical loader-mode Rockusb protocol helper; PyUSB/libusb-package; contains write/reset commands, not a generic updater |
| `tools/gsl_fw_extract.py`, `calibration-oracle.py` | Firmware extraction / optional Unicorn calibration reference; no original firmware or image included |
| `tools/rk3128_board_dts_check.py`, `rk3128_pmic_dts_check.py` | Board voltage/power/input invariants against an explicit DTB; requires dtc |
| `tools/rga1-*.c`, `rga1-controls.h` | RGA V4L2 operation probes and experimental control definitions |
| `tools/iep-test.c`, `iep-gpu-bench.c` | IEP fixtures and GLES comparison; GPU benchmark needs EGL/GLES development files |
| `tools/vepu-encode.c`, `vepu-verify.py` | Explicit V4L2 H.264/VP8 encode and software round-trip checks; verifier needs FFmpeg |
| `tools/crypto-*.c`, `crypto-*.py` | Kernel/user API probes and independent finite vectors/RNG analysis; see [crypto](CRYPTO.md) |
| `tools/vop-colour-*`, `rk312x_codec_trace.py`, `audio_tone.py` | Register/codec/audio diagnostics; inspect source before device access |
| `linux/fbtest/fbtest.c` | Framebuffer pattern test |
| `linux/tests/cpu/rk3128bench.c` | Matched CPU/memory benchmark kernels; ARM-specific paths |
| `linux/tests/memory/ram-account.c` | Runtime RAM/reservation accounting; read access to kpageflags may need privilege |
| `linux/tests/launch/drm_handoff_probe.c` | Read-only non-master DRM descriptor handoff timing; libdrm |
| `linux/tests/launch/shader_probe.c` | Standalone offscreen GLES shader/cache assertions using GBM/EGL; `--surfaceless` enables a host check |
| `linux/userspace/rk3128-mem/` | ARMv7 copy/set implementation and finite guard/alignment test |
| `research-tools/malitrace/` | Historical binary-driver observation and shader analysis source; no driver blobs |

Product-dependent launch/UI probes and product test runners were excluded because they require the absent application framework. The included shader probe uses a standalone context. Historical Mali tools retain some old helper/layout references; they are research source to adapt, not a supported end-to-end capture pipeline.

## JPEG example

`examples/hantro-jpeg` preserves four isolated library source/header files with `rk3128_jpeg*` APIs. They use libc/pthreads and Linux V4L2/DMA-BUF APIs and do not require the original UI runtime. Compile the C files into your own caller; keep each header's lifetime/error contract. RGA acceleration requires this repository's RGA driver and supported format/control subset. A CPU implementation/policy choice for refused formats belongs to the caller.

## Component patch baselines

For ALSA and wpa_supplicant, unpack the exact orig tarball and Debian packaging in their `.env`, apply Debian's `debian/patches/series`, then local `series`. For Mesa, apply the local series to the upstream tarball. GStreamer has a packaging-overlap wrinkle: the local H.265 security patches are retained as explicit references, but the pinned Debian packaging may already carry their corrections. Inspect whether each exact change is present before applying it; do not double-apply or reverse an already-fixed package. The HEVC crop/H.264 range changes remain separate. Preserve local order and stop at any unexpected reject.

`series` files list local patches only. They are not replacements for a matching Debian source package's own series. Pin source/runtime ABI together; do not replace libraries purely because a symlink name looks compatible.

## Checks that can run now

Meaningful host checks include compiling isolated tools, compiling the JPEG objects, preparing the full kernel series against the pinned tarball, checking DTS voltage invariants and running the finite rk3128-mem test under qemu-arm. Kernel source preparation/DTS compile establish compatibility; they do not validate DDR transitions, scanout timing, a power cycle, audio output, touch quality or crypto transition safety. [Validation record](VALIDATION.md) states exactly what was checked for this publication.

Hardware validation should use finite fixtures, check errors as well as throughput, and stop when the command completes. No observation/soak period is part of this reference. This collection deliberately supplies no automatic flash/deployment route.

Original numerical proofs and historical experiment patches are documented in [research-tools/README.md](../research-tools/README.md). They are outside the accepted default series; their source-state dependencies and rejected/partial results are explicit.
