# Hardware media blocks and ownership

RK3128 has distinct Hantro VPU1, HEVC/rkvdec, RGA1-plus and IEP blocks. Names shared with newer Rockchip parts do not imply the same registers or supported operations.

## Hantro and rkvdec share a bus

The Hantro VPU1 at **0x10106000** handles the established H.264 decode path and the G1 JPEG decoder; its H1 bank also supplies encoding. The independent HEVC decoder at **0x10104000**, hardware ID **0x6867**, uses the rkvdec HEVC backend. Its MMU banks are at **0x10104440/0x10104480**. HEVC uses SPI66, its MMU SPI65.

GRF_SOC_CON1 **bit 15** selects which decoder owns their shared bus. Patches `299` and `301` supply default mux policy and a runtime-PM arbiter shared by both decoders and their IOMMUs. This is hardware ownership, not a per-application scheduling hint. Preserve the locks, peer-state checks, timeouts and PM ordering. Touching an MMU without all required core/bus clocks caused a reset in early work; MMU register accessibility is not established by enabling one bus clock.

The supplied contiguous-DMA profile leaves decoder IOMMUs disabled for direct VOP scanout; [CPU/memory](CPU-MEMORY.md) explains the CMA/cache requirements. H.264 and HEVC 1080p60 are vendor capability claims; the panel itself refreshes at about51 Hz. Throughput, decode correctness and displayed cadence must be evaluated separately.

## JPEG decode and encoder baseline

Patch 395 ports STMicroelectronics' G1 JPEG decoder and keeps attribution. The verified subset is baseline **4:2:0/4:2:2**, with hardware limits recorded as **48×48 through 8176×8176**. Progressive, grayscale and 4:4:4 need another decoder.

For a tested **4000×3000 (12 MP), 4:2:0 baseline JPEG**, the final corrected path took **385.5 ms** including setup, hardware decode, four-thread RGB conversion and teardown, versus **1698.5 ms with stb_image software decoding** (about **4.4× faster**). Hardware decode itself took about **126 ms**. The comparison used median-of-three measurements at **1008 MHz CPU and 396 MHz DDR**. Decoded Y/Cb/Cr differed from the software reference by at most 1; final RGB differed by at most 3 per channel, with conversion matching the software conversion of the hardware planes. These are image-specific results, not a guarantee for every JPEG or a comparison against every software decoder.

JPEG/optional encoding yields to streaming video: a long still-image job must not seize the shared core/bus from active decode. Failure returns a real error/zero output, leaving caller policy explicit. `examples/hantro-jpeg` provides self-contained V4L2 parsing/decode/conversion and optional RGA preparation helpers, with original API names retained. It includes no UI, daemon or firmware. The CPU conversion includes adapted stb_image routines under preserved MIT attribution.

The existing Hantro JPEG encoder was also exercised with 1920×1080 visible NV12 stored as1920×1088; this is distinct from the added video encoder.

## H1 H.264 and VP8 encode

Patch736 extends the existing Hantro device/scheduler rather than creating a second overlapping platform device. The fitted H1 identifies as **0x48311220**, synthesis **0x1f522780**, with a1920-pixel width field. Do not interpret a vendor JPEG8192 limit as a proven video encode limit.

Supported video encode is **H.264 constrained-baseline IDR only**, GOP1, constant QP0..51, or **VP8 profile0 keyframes only**, constant QP0..127. There are no P/B frames, CABAC, rate control or claimed MVC implementation. Standard stateful V4L2 controls are used; this kernel has no accepted stateless encode-control API for this block.

Raw formats are NV12M/YUV420M, macroblock-aligned storage and V4L2 crop for visible dimensions, up to1920×1080 visible/1920×1088 storage. Visible minima are H.26496×96 and VP8132×96; JPEG's96×32 minimum is different. Capture uses coherent MMAP buffers, not imported capture DMA-BUFs. Short planes, nonzero offsets and inconsistent coded/raw geometry are refused before hardware access. Tools `vepu-encode.c` and `vepu-verify.py` provide explicit finite encode/decode comparisons using caller-supplied fixtures.

## RGA1-plus

Patches619 implement a V4L2 multiplanar mem2mem interface and binding. `tools/rga1-controls.h` documents its experimental UAPI. This is **RGA1-plus**, not mainline RGA2. It supports a verified subset of copy/scale/transform, fill, gradient, palette, transparency, alpha/Porter-Duff, ROP and planar prescale operations with strict format, alignment and shared-register checks.

Ordinary packed/semi-planar formats use one memory plane; YVU422M uses three. Imported masks own their DMA-BUF references transactionally, and producers/CPU consumers must synchronize correctly. There is no raw physical-address ABI. The driver validates rounded tile geometry, halos and private guard regions before mapping or submission.

Some useful boundaries: standalone planar prescale uses exact factors1/2/4/8 with equal hardware format/colorimetry; normal blit planar output is not implied. RGB→BT.709 output is refused because the observed selector's clamp is incompatible; BT.709-full source is not claimed. Several 16-bit destination alpha-layout variants are refused. Point/line/AA-line/blur/sharpen opcodes absent from the RGA1-plus contract are not invented. Arbitrary affine rotation has separate bounded tile/halo rules, and “rotation works” is not a promise for every format/control combination. Use `rga1-test.c`, `rga1-probe.c` and `rga1-rest.c` to inspect the negotiated subset.

## IEP v1

Patches620 provide IEP V4L2/vb2 and binding source with retained Rockchip register-code attribution. The verified interface covers14 RGB/YUV layouts, I2O1/I4O1/I4O2 deinterlacing, RGB mapping tables, colour coefficient/order, output alpha, BCSH, diagnostic colour and selected3×3/5×5 denoise/detail/edge behavior. The IEP MMU is attached; exported MMAP buffers remain contiguous.

**Scaling remains refused.** Larger radius codes2/3 corrupt constant input, combined deinterlace/RGB enhancement can corrupt DC or time out, and direct-display integration is absent. The physical impossibility of larger radii, scaling or YUV denoise has not been established; the implemented API rejects paths without correct numerical results. Do not turn a vendor capability bit or a completed DMA interrupt into a correctness claim. `iep-test.c` and `iep-gpu-bench.c` provide independent finite fixtures/comparisons.

## GStreamer and colour

Apply Debian's recorded package patch series first, then this repository's component series, respecting the package-overlap notes in [tools](TOOLS.md). The included patches preserve HEVC visible crop metadata, handle recorded H.265 SEI parsing bounds fixes, and preserve independent H.264 VUI range when the matrix is unknown. Keep coded storage, crop, strides, matrix and range distinct all the way from decode to KMS. The FFmpeg pin lists a minimal software decoder/parser subset for ABI context; this repository includes no transcoding application or automatic codec conversion.
