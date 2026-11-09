# Display, panel handoff and memory priority

## Display path and timing

The RK3128 reference board uses RK3128 VOP (the RK3126 register layout) → integrated LVDS PHY/transmitter → the fitted panel path. The board has a bridge identified as LT8911EX, but the stock DT and working Linux path do not use a separate OS I2C driver for it. HDMI and composite output are not wired on this board.

The panel is 1920×1080. The vendor timing asks for 120 MHz; the actual pixel clock is **118.8 MHz**, with totals **2080×1120**, giving about **51 Hz**, not 60 Hz. Preserve this distinction when judging frame pacing. The included panel/PHY/VOP changes cover LVDS analog programming, sample clock, VOP pixel-clock polarity/enable, warm takeover and explicit panel power sequencing.

The bootloader leaves a lit framebuffer. Simplefb bring-up preserves its clocks/domains; real DRM takeover disables simplefb and owns the VOP/LVDS path. The loader logo's 32 MiB reservation may be returned to normal memory only after successful takeover, with page state initialized and reserved pages released. The DT node may remain after those pages are freed; do not count its static presence as permanently lost RAM.

## The stability bug: missing external LCDC NoC priority

A display can receive every expected vblank while fetching bad or incomplete scanout pixels. KMS timing and decoder drop counters do not measure VOP FIFO starvation. This was observed during ordinary UI/media workloads on this board, independently of changes to rendering cost.

The stock board DT supplied `rockchip,priority = <3 3>` to the **external LCDC NoC port**, whose register block is `0x1012f180 + 0x20`. The vendor early-init code programmed that policy. Mainline's power-domain driver saved/restored QoS register state around power transitions but did not initialize this board-selected priority. Preserving a reset priority of zero therefore kept the missing policy.

The fix has three related source changes:

| Input | Role |
| --- | --- |
| `623-0001-drm-rockchip-vop-rk3126-error-counters.patch` | Measures win0/win1 FIFO-empty interrupts and bus errors; corrects their separate IRQ layout and keeps W1C acknowledgements out of cached register state |
| `623-0002-dt-bindings-soc-rockchip-rk3128-qos.patch` | Describes an optional, validated high/low pair, each 0..3 |
| `623-0003-pmdomain-rockchip-initialize-noc-priority.patch` | Parses and applies the policy through the existing PM-domain owner; uses checked masked writes at probe for powered domains and after power-on/restore |
| `linux/dts/rk3128-scanout-qos.dtsi` | Selects `<3 3>` for `&qos_lcdc` only |

The driver leaves ports without the property alone. It preserves inherited live-display clock gates at probe and checks reapplication on an already-powered power-on retry. Kernel support and the DT policy must be carried together. A one-off `/dev/mem` write is not the reference fix: it loses driver ownership, error handling and resume behavior.

### Register encoding and the readback trap

The BSP-compatible encoding is `(high << 8) | (high << 2) | low`. For `<3 3>` it writes **0x30f** at **0x1012f188** (PRIORITY offset `+0x08`). On the measured RK3128, the duplicate high field in bits 9:8 reads zero: actual readback is **0x0f**. Validate effective high/low bits 3:2/1:0 as 3/3, rather than falsely failing a working configuration by requiring exact `0x30f` readback.

The MODE, BANDWIDTH, SATURATION and EXTCONTROL registers were not changed by this fix. GPU/VPU priorities, DDR/GPU clocks, rails, bursts, MMU and buffer ordering were also held unchanged. Register reads from gated NoC ports may be misleading; inspect through the owning driver with a powered domain rather than probing unrelated clock-gated windows.

### Finite evidence and limits

Before the fix, three matched media/control/library workloads recorded win0 errors **+71 / +134 / +132**; win1 errors were **0 / +5 / +2**. Zero decoder drops did not prevent these underflows. With the priority fix, all three workloads recorded **zero win0, win1 and bus errors**. The final retry/error-handling revision repeated **120 control toggles, 24 scroll pairs and six screenshot cycles**, with **2,137 advancing frame IRQs** and zero FIFO/bus errors or playback drops in those finite workloads.

A separate graphics/interaction validation had **993 / 993 / 1,113 frame IRQs** with zero errors across the three measured sequences. Before the policy fix it had exposed **+2 / +7 / +8** win0 underflows despite zero KMS misses in several windows. The measured panel cadence was about **19.609 ms** per refresh. These are completed, finite observations on one board, not a guarantee for every workload or RK3128 device.

## Colour and scanout ownership

Patch 616 exposes RK3126 video colour encoding/range handling. GStreamer H.264 parsing preserves an explicitly known range independently of a missing matrix; HEVC crop metadata must distinguish visible dimensions from decoder storage. Do not identify a colour conversion by picture size alone.

A direct-scanout userspace design needs one DRM master/atomic-KMS owner. Renderers can use non-master card/render descriptors, GBM/EGL and DMA-BUF sharing. A posted buffer stays owned until the page flip and release protocol are complete; a screenshot/import consumer must also retain its reference. Avoid freeing a BO because its producer submitted a newer frame. The product launcher/display service is outside this repository; these ownership requirements remain relevant to any replacement.
