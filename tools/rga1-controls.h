/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* RGA1-plus driver-private V4L2 controls. No addresses or pointer-sized ABI. */
#ifndef RGA1_CONTROLS_H
#define RGA1_CONTROLS_H
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#define V4L2_CID_RGA1_BASE (V4L2_CID_USER_BASE + 0x2100)
#define V4L2_CID_RGA1_OPERATION (V4L2_CID_RGA1_BASE + 0)
#define V4L2_CID_RGA1_SAMPLING (V4L2_CID_RGA1_BASE + 1)
#define V4L2_CID_RGA1_DITHER (V4L2_CID_RGA1_BASE + 2)
#define V4L2_CID_RGA1_ALPHA_MODE (V4L2_CID_RGA1_BASE + 3)
#define V4L2_CID_RGA1_PORTER_DUFF (V4L2_CID_RGA1_BASE + 4)
#define V4L2_CID_RGA1_COLORKEY (V4L2_CID_RGA1_BASE + 5)
#define V4L2_CID_RGA1_KEY_MIN (V4L2_CID_RGA1_BASE + 6)
#define V4L2_CID_RGA1_KEY_MAX (V4L2_CID_RGA1_BASE + 7)
#define V4L2_CID_RGA1_ROP (V4L2_CID_RGA1_BASE + 8)
#define V4L2_CID_RGA1_FADING (V4L2_CID_RGA1_BASE + 9)
#define V4L2_CID_RGA1_LUT (V4L2_CID_RGA1_BASE + 11)
#define V4L2_CID_RGA1_GRADIENT (V4L2_CID_RGA1_BASE + 16)
#define V4L2_CID_RGA1_PATTERN (V4L2_CID_RGA1_BASE + 17)
#define V4L2_CID_RGA1_PATTERN_SIZE (V4L2_CID_RGA1_BASE + 18)
#define V4L2_CID_RGA1_CSC_ALIAS (V4L2_CID_RGA1_BASE + 19)
#define V4L2_CID_RGA1_FG_COLOR (V4L2_CID_RGA1_BASE + 21)
#define V4L2_CID_RGA1_PALETTE_ALPHA (V4L2_CID_RGA1_BASE + 22)
#define V4L2_CID_RGA1_ZERO_KEY (V4L2_CID_RGA1_BASE + 25)
#define V4L2_CID_RGA1_GRADIENT_WRAP (V4L2_CID_RGA1_BASE + 26)
#define V4L2_CID_RGA1_ALPHA_CALC (V4L2_CID_RGA1_BASE + 27)
#define V4L2_CID_RGA1_MIX_DEST (V4L2_CID_RGA1_BASE + 28)
#define V4L2_CID_RGA1_ROP_MODE (V4L2_CID_RGA1_BASE + 29)
/* +30 was diagnostic only and is intentionally not a control. */
struct rga1_mask { __s32 fd; __u32 flags; };
#define VIDIOC_RGA1_SET_MASK _IOW('V', BASE_VIDIOC_PRIVATE, struct rga1_mask)
#define V4L2_CID_RGA1_PATTERN_ORIGIN (V4L2_CID_RGA1_BASE + 31)
#define V4L2_CID_RGA1_ROTATE_AA (V4L2_CID_RGA1_BASE + 32)
#define V4L2_PIX_FMT_RGA1_PAL1 v4l2_fourcc('R', 'G', 'P', '1')
#define V4L2_PIX_FMT_RGA1_PAL2 v4l2_fourcc('R', 'G', 'P', '2')
#define V4L2_PIX_FMT_RGA1_PAL4 v4l2_fourcc('R', 'G', 'P', '4')
/* operation: 0 blit, 1 fill, 5 pattern fill, 6 standalone prescale; 2..4 are absent from RGA1-plus.
 * Arbitrary rotations define a black compose canvas; only packed RGB is
 * accepted, without alpha/key/ROP/fading. Rotation AA requires RGB24/BGR24/565
 * input (or a hardware-expanded palette), since real per-pixel alpha is not
 * supported for arbitrary rotations. Quarter turns ignore ROTATE_AA.
 * sampling: 0 nearest, 1 bilinear, 2 bicubic.
 * alpha mode: 0 disabled, 1 global, 2 pixel, 3 global*pixel.
 * Blending accepts RGB24/BGR24 (opaque) and alpha-bearing 8888 CAPTURE.
 * RGBA Porter-Duff fetches Ad. Its effective As is G (global), As (pixel),
 * or floor(As*G/256) (combined); Ad is the stored destination alpha.
 * Without destination-alpha fetch, hardware uses G for Ad in every mode.
 * The usual opaque pixel-mode call therefore sets G=255, preserving #722.
 * Colors must already be premultiplied by the caller by the effective alpha.
 * Mixed-alpha mode interpolates RGB and passes through the stored source A.
 * MIX_DEST selects SRC*(1-Ad)+DST*Ad in mixed mode, requiring RGBA CAPTURE.
 * ALPHA_CALC: 0 coefficient A+(A>>7), 1 coefficient A. Complements are 256-A'.
 * Products are summed before a single truncating >>8 and 8-bit saturation.
 * Porter-Duff: 0 mixed alpha, 1 CLEAR, 2 SRC, 3 DST, 4 SRC_OVER,
 * 5 DST_OVER, 6 SRC_IN, 7 DST_IN, 8 SRC_OUT, 9 DST_OUT,
 * 10 SRC_ATOP, 11 DST_ATOP, 12 XOR.
 * colorkey: 0 disabled, 1 normal RGB-range stencil, 2 inverted.
 * ROP: 0 disabled; otherwise value-1 holds truth tables, indexed by operand bits.
 * ROP_MODE: 0 ROP2 (P,D, value<=16), 1 ROP3 (P,S,D, value<=256, default),
 * 2 ROP4 (two ROP3 bytes, value<=65536). A one mask bit chooses the low byte;
 * zero chooses the high byte. Blits take P from negotiated-format pattern RAM;
 * solid-fill ROP2 takes its pen from the fill color.
 * VIDIOC_RGA1_SET_MASK: fixed-width {fd,flags}; flags must be zero.
 * DMA-BUF fd for ROP4, -1 releases. Repeated calls reacquire the fd.
 * The driver retains its own reference; the original fd may then be closed.
 * The caller owns synchronization/serialization, including DMA_BUF_SYNC for CPU
 * access. Mask rows are ALIGN(ceil(source_width/8),4) bytes, little-endian words
 * scanned from bit31 to bit0. ROP4 requires full, equal-format/size RGB images,
 * without transforms, alpha, key, fading or dither.
 * fading: RGB addition offsets, 0 disables. Colors: packed 0xRRGGBB.
 * Standard BG_COLOR is the fill color; ALPHA_COMPONENT is global alpha.
 * CSC_ALIAS: use the second BT.601-limited selector (hardware value 3);
 * valid only for BT.601-limited YUV. It never represents BT.709 full.
 * FG_COLOR: 1bpp foreground RGB; BG_COLOR is its background RGB.
 * Packed PAL1/2/4 indices are MSB first in each byte. PAL8 is not advertised.
 * Fill uses ALPHA_COMPONENT as its initial alpha; COMPOSE can be 1x1.
 * Native dithered fill normalizes C to 5/6 bits: q=(C*M+M+K*(M+1))>>8,
 * M=31 or63, K=Bayer4x4>>1 or>>2. This differs from blit zero-fill quantization.
 * PALETTE_ALPHA: LUT alpha mode, 0 eight bits, 1 bit24 (A byte bit0) expanded to 0/255.
 * ZERO_KEY: hardware alpha-zero key mode (MODE_CTRL[30]).
 * LUT: 1024 bytes, 256 entries in little-endian internal ABGR8888 (RGBA bytes).
 * GRADIENT: eight signed 8.8 integers [A_x,A_y,B_x,B_y,G_x,G_y,R_x,R_y].
 * GRADIENT_WRAP: fill-only, 0 clip (default), 1 wrap modulo 65536 before >>8.
 * Clip first saturates the row start (C*256+y*Gy) to 0..65535, then adds x*Gx
 * and saturates again. Wrap evaluates the full expression modulo 65536.
 * PATTERN: 1024 bytes in negotiated CAPTURE pixel format; unused bytes zero.
 * PATTERN_SIZE: (height << 16) | width, area <=255 pixels, each axis 1..255.
 * PATTERN_ORIGIN: (y<<16)|x, each coordinate strictly below its pattern axis.
 * Pattern fill uses ALPHA_COMPONENT for output alpha, ignoring pattern A.
 * Standalone prescale requires full selections and equal hardware formats and
 * colorimetry, with exact 1/2/4/8 factors independently per axis. Select operation
 * 6 before negotiating planar CAPTURE formats. YVU422M uses three memory planes.
 * RGB/YUV colors are averaged; 8888 alpha uses the block's bottom-right sample.
 */
#endif
