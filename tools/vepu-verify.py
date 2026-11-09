#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Generate bounded raw fixtures and verify hardware H.264/VP8 with FFmpeg software decode.

No board access or media upload is implicit. Encode using vepu-encode on the board,
copy the elementary stream back, then run verify on the build host.
"""
import argparse
import json
import math
import pathlib
import subprocess
import tempfile


def run(args):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=90).stdout


def geometry(args):
    if not (96 <= args.width <= 1920 and 96 <= args.height <= 1088
            and args.width % 4 == 0 and args.height % 2 == 0
            and 1 <= args.frames <= 120):
        raise ValueError("requires even 96..1920 x 96..1088, width % 4 = 0, 1..120 frames")
    return args.width * args.height * 3 // 2 * args.frames


def generate(args):
    geometry(args)
    run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-threads", "1",
         "-f", "lavfi", "-i", f"testsrc2=size={args.width}x{args.height}:rate=30",
         "-frames:v", str(args.frames), "-vf",
         "geq=lum='mod(lum(X,Y)+N*31,220)+16':cb='mod(cb(X,Y)+N*17,200)+28':cr='mod(cr(X,Y)+N*23,200)+28'",
         "-pix_fmt", args.format,
         "-threads", "1", "-f", "rawvideo", str(args.output)])
    if args.output.stat().st_size != geometry(args):
        raise ValueError("fixture size mismatch")
    return {"fixture": str(args.output), "width": args.width, "height": args.height,
            "frames": args.frames, "format": args.format, "bytes": geometry(args)}


def verify(args):
    expected = geometry(args)
    if args.input.stat().st_size != expected:
        raise ValueError("source byte count does not match geometry/frame count")
    if not 0 < args.encoded.stat().st_size <= 16 * 1024 * 1024 * args.frames:
        raise ValueError("empty/oversized elementary stream")
    probe = json.loads(run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                           "-show_entries", "stream=codec_name,width,height", "-of", "json",
                           str(args.encoded)]))
    streams = probe.get("streams", [])
    if (len(streams) != 1 or streams[0].get("codec_name") != args.codec
            or streams[0].get("width") != args.width
            or streams[0].get("height") != args.height):
        raise ValueError(f"wrong encoded codec/visible geometry: {streams}")
    with tempfile.TemporaryDirectory(prefix="vepu-", dir=args.work_dir) as temp:
        temp = pathlib.Path(temp)
        decoded, reference = temp / "decoded.yuv", temp / "reference.yuv"
        run(["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-y", "-threads", "1",
             "-c:v", args.codec, "-i", str(args.encoded), "-map", "0:v:0", "-frames:v", str(args.frames + 1), "-fps_mode", "passthrough",
             "-pix_fmt", "yuv420p", "-threads", "1", "-f", "rawvideo", str(decoded)])
        if decoded.stat().st_size != expected:
            raise ValueError(f"decoded {decoded.stat().st_size} bytes, expected {expected}")
        run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-threads", "1", "-f", "rawvideo",
             "-pixel_format", args.format, "-video_size", f"{args.width}x{args.height}",
             "-i", str(args.input), "-pix_fmt", "yuv420p", "-threads", "1",
             "-f", "rawvideo", str(reference)])
        sizes = [args.width * args.height, args.width * args.height // 4,
                 args.width * args.height // 4]
        errors, counts = [0, 0, 0], [0, 0, 0]
        per_frame = []
        with decoded.open("rb") as dec, reference.open("rb") as ref:
            for frame in range(args.frames):
                frame_psnr = []
                for p, size in enumerate(sizes):
                    a, b = dec.read(size), ref.read(size)
                    if len(a) != size or len(b) != size:
                        raise ValueError("short decoded/reference plane")
                    error = sum((x - y) ** 2 for x, y in zip(a, b))
                    score = 10 * math.log10(255 ** 2 * size / error) if error else None
                    if score is not None and score < args.min_psnr:
                        raise ValueError(f"frame {frame} plane {p} PSNR {score:.3f} below {args.min_psnr} dB")
                    frame_psnr.append(score)
                    errors[p] += error
                    counts[p] += size
                per_frame.append(frame_psnr)
        psnr = [10 * math.log10(255 ** 2 * n / e) if e else None
                for e, n in zip(errors, counts)]
        average = (10 * math.log10(255 ** 2 * sum(counts) / sum(errors))
                   if sum(errors) else None)
        if any(x is not None and x < args.min_psnr for x in psnr):
            raise ValueError(f"PSNR below {args.min_psnr} dB: {psnr}")
        return {"codec": args.codec, "decoder": "FFmpeg software " + args.codec, "width": args.width,
                "height": args.height, "frames": args.frames, "psnr_y_u_v_db": psnr,
                "psnr_average_db": average, "min_psnr_db": args.min_psnr,
                "per_frame_psnr_y_u_v_db": per_frame,
                "encoded_bytes": args.encoded.stat().st_size, "result": "PASS"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("generate", "verify"):
        p = sub.add_parser(name)
        p.add_argument("--width", type=int, required=True)
        p.add_argument("--height", type=int, required=True)
        p.add_argument("--frames", type=int, default=6)
        p.add_argument("--format", choices=("nv12", "yuv420p"), default="nv12")
        if name == "generate":
            p.add_argument("--output", type=pathlib.Path, required=True)
        else:
            p.add_argument("--input", type=pathlib.Path, required=True)
            p.add_argument("--encoded", type=pathlib.Path, required=True)
            p.add_argument("--min-psnr", type=float, default=30)
            p.add_argument("--codec", choices=("h264", "vp8"), default="h264")
            p.add_argument("--work-dir", type=pathlib.Path, default=pathlib.Path("/var/tmp/736"))
    args = parser.parse_args()
    try:
        result = generate(args) if args.command == "generate" else verify(args)
        print(json.dumps(result, sort_keys=True))
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        detail = exc.stderr.decode(errors="replace")[-3000:] if isinstance(exc, subprocess.CalledProcessError) else ""
        parser.exit(1, f"vepu-verify: {exc}\n{detail}\n")


if __name__ == "__main__":
    main()
