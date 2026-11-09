# Source versions and provenance

The recorded RK3128 hardware reference snapshot is dated **2026-11-09**.
The component pins, upstream contribution identifiers and input-file hashes
below describe the source inputs for this collection.

## Recorded source inputs

The `.env` files contain exact source URLs, versions and SHA256 values. Verify downloaded archives against those files before applying anything.

| Component | Recorded baseline | Input record |
| --- | --- | --- |
| Linux | 7.3.1, armhf; source tarball SHA256 `fa595aad93734f829c963c2a90ef7c3673224fd7fdae7b49406e397cde2de231` | `linux/bases/mainline-7.3.env` |
| Debian | 13 / trixie armhf; package snapshot references 2026-11-05 | component pins below |
| Mesa | 26.2.6; Gallium lima, kmsro winsys, no LLVM required for this configuration | `linux/userspace/mesa/mesa.env` |
| alsa-lib | 1.2.14, Debian `1+deb13u1` | `linux/userspace/alsa-lib/alsa-lib.env` |
| GStreamer gst-plugins-bad | 1.26.2, Debian `3+deb13u3` | `linux/userspace/gstreamer/gstreamer.env` |
| wpa_supplicant | 2.10, Debian `2:2.10-24` | `linux/userspace/wpa/wpa.env` |
| FFmpeg | 7.1.5; H.264/HEVC/AAC decoder/parser subset recorded for ABI context | `linux/userspace/ffmpeg/ffmpeg.env` |
| SSV6x5x | derived from `cdhigh/armbian_sv6256p`, revision prefix `43c2ede`; preserved source grants differ per file | `linux/modules/ssv6x5x/`, [notices](../NOTICE.md) |

These are historical pins, not independently fetched version recommendations. No tarballs, upstream kernel tree, vendor executables or firmware blobs are committed here. Source availability and current security support can change independently of this snapshot.

## Upstream contribution provenance

The Hantro G1 JPEG decoder originates from STMicroelectronics' Linux work: `32014c7e991c10bf8142a8888b25b68203cec4d5`, with follow-up identifiers `d143af39`, `b8646639`, `3989afe4`, `6efac825`, from the `v6.18-stm32mp` line. Its implementation and attribution are preserved in patch `395-0001`. Register offsets were cross-checked against Rockchip MPP VDPU1 definitions.

The H1 video encoder uses GPL-2.0 mode-decision tables from Bootlin's Hantro encoder branch at `2e7129a85cfed975c00202e57b1286ad234bbb00`; the VP8 tables include a preserved WebM BSD notice. Rockchip MPP's Apache-2.0 encoder code served as a hardware-contract comparison, not as a copied kernel implementation.

The IEP register programming retains Rockchip's 2013 attribution from its GPL-2.0 `release-3.10` kernel IEP sources. The V4L2/vb2 integration is new code. The first Mesa patch reflects the FP16 varying work identified as upstream MR `!37449`; its existing author/license notices remain. Patch numbers record development ordering and are not upstream submission or acceptance identifiers.

Useful original source locations:

- Linux: <https://www.kernel.org/>
- Rockchip kernel: <https://github.com/rockchip-linux/kernel>
- STMicroelectronics Linux: <https://github.com/STMicroelectronics/linux>
- Rockchip MPP: <https://github.com/rockchip-linux/mpp>
- Bootlin Hantro branch: <https://github.com/bootlin/linux/tree/hantro/h264-encoding>
- Mesa: <https://gitlab.freedesktop.org/mesa/mesa>
- SSV source: <https://github.com/cdhigh/armbian_sv6256p>

These links identify sources already used in the development record; they were not re-fetched as part of publication.

## File records

[The source map](../manifests/source-map.json) associates each current reference path with its recorded input-file SHA256 hash. The input hash identifies the source before reference naming and presentation edits; [SHA256SUMS](../manifests/SHA256SUMS) identifies the exact distributed file bytes. The two hashes therefore serve different purposes.

Original author and copyright attribution, upstream source addresses and component patch order remain in force. Board-specific identifiers describe the supplied RK3128 reference profile. Component source pins retain their archive checksums; explanatory comments do not change the identity of those archives.

The Wi-Fi configuration is a minimal reference containing the required setting names and values. Firmware availability and licensing are separate from the source licenses in this collection; see [touch](TOUCH.md) and [notices](../NOTICE.md).
