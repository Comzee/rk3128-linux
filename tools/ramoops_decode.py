#!/usr/bin/env python3
"""Decode a ramoops region read back from the board (development record #284, docs/RK3128-LINUX.md "Test slot and ramoops").

  ramoops_decode.py <last_kmsg> <out dir> [--size N] [--record N] [--console N] [--ftrace N] [--pmsg N] [--early N]

<last_kmsg> is /proc/last_kmsg of the Android boot after the test: the vendor kernel's raw, unrotated copy of the
512 KiB at 0x7f100000 (byte N is 0x7f100000 + N). The zones are found from the ramoops LAYOUT, never by searching the
log for "DBGC" (a log line may contain it). The defaults are the test kernel's DT (linux/dts/rk3128-reference.dts,
#286): size 0x80000, record-size 0x10000, console-size 0x40000, ftrace-size 0, pmsg-size 0x10000. Mainline
fs/pstore/ram.c lays them out from the base in this order: dmesg records (size - console - ftrace - pmsg, cut into
record-size zones: 3 here), console, ftrace, pmsg. Every zone (fs/pstore/ram_core.c, ECC off) is a 12-byte header,
u32 sig 0x43474244 "DBGC" (ftrace zones carry another sig and are skipped), u32 start, u32 size, then a ring of
zone - 12 bytes; its text is data[start:size] + data[:start] (persistent_ram_save_old). A dmesg record starts with
"====<sec>.<nsec>-<C|D>\\n"; C means the rest is deflate-compressed (CONFIG_PSTORE_COMPRESS): it is inflated.
Writes <out>/console.txt, pmsg.txt and dmesg-<n>.txt for each valid zone and prints one line per zone.

The early area (#287, docs/RK3128-LINUX.md "First boot") sits in the third record zone, at --early (default 0x20000,
-1 = do not look): at +0x10 the marks of patch 287-0002 (u32 words: "RK3128-ZIMG-RUN!"; where the decompressor ran,
r2 = the DTB, r1, the CPSR; the watchdog's TORR, CR and counter read back after the start; the counter, "DCMP" and the
inflated kernel's address after the decompression; "STXT", the counter, r2 and r1 at the kernel's stext), at +0x80 the
text of the RAM early console (patch 287-0003: a "RK3128-EARLYCON 1" line, the console text, a NUL), at +0x8000 the
low-level debug area (patch 287-0004: u32 "RLOG", u32 write offset, text from +16 up to it, 32 KiB at most). Both are found only at those offsets and only
with their magic; the console goes to <out>/earlycon.txt, the debug text to <out>/debug-ll.txt and the marks to
<out>/zimage-mark.txt.
Exit 0 when at least one zone, the marker or the early console is found, 3 when none is (the file is the vendor
kernel's own last_log text), 2 on bad arguments.
"""
import struct
import sys
import zlib

SIG = 0x43474244
HDR = 12


def zone(d, off, zsize):
    """The text of the zone at off (zsize bytes with its header), or None when its header is not valid."""
    if zsize <= HDR or off + zsize > len(d):
        return None
    sig, start, size = struct.unpack_from("<III", d, off)
    cap = zsize - HDR
    if sig != SIG or size > cap or start >= max(cap, 1) or (size < cap and start != size):
        return None
    data = d[off + HDR:off + HDR + cap]
    return data[start:size] + data[:start]


def dmesg_text(t):
    """A dmesg record: its header line kept, a compressed body inflated."""
    nl = t.find(b"\n")
    if not t.startswith(b"====") or nl < 0:
        return t
    head, body = t[:nl + 1], t[nl + 1:]
    if head.rstrip().endswith(b"-C"):
        for wbits in (-15, 15, 31):
            try:
                return head + zlib.decompressobj(wbits).decompress(body)
            except zlib.error:
                continue
        return head + b"(compressed record that did not inflate)\n"
    return t


ZMAGIC = b"RK3128-ZIMG-RUN!"
EMAGIC = b"RK3128-EARLYCON 1\n"


def early(d, off):
    """(marker lines or None, early console text or None) of the early area at off."""
    mark = text = None
    m = off + 0x10
    if 0 <= off and m + 0x48 <= len(d) and d[m:m + 0x10] == ZMAGIC:
        w = struct.unpack_from("<14I", d, m + 0x10)
        pc, r2, r1, cpsr, torr, cr, ccvr0, ccvr1, dcmp, kaddr, stxt, ccvr2, kr2, kr1 = w
        mark = [f"zImage entered: code at 0x{pc:08x}, r2 (DTB) 0x{r2:08x}, r1 0x{r1:08x}, CPSR 0x{cpsr:08x}",
                f"watchdog after its start: TORR 0x{torr:08x}, CR 0x{cr:08x}, counter 0x{ccvr0:08x}"]
        if dcmp == 0x504D4344:
            mark.append(f"decompressed (DCMP): kernel at 0x{kaddr:08x}, watchdog counter 0x{ccvr1:08x} "
                        f"({ccvr0 - ccvr1} clocks since the start)")
        else:
            mark.append("no DCMP mark: the decompressor did not finish")
        if stxt == 0x54585453:
            mark.append(f"kernel stext reached (STXT): r2 0x{kr2:08x}, r1 0x{kr1:08x}, watchdog counter 0x{ccvr2:08x} "
                        f"({ccvr0 - ccvr2} clocks since the start)")
        else:
            mark.append("no STXT mark: the kernel's head.S was not reached")
    e = off + 0x80
    if 0 <= off and d[e:e + len(EMAGIC)] == EMAGIC:
        end = d.find(b"\0", e)
        limit = min(len(d), off + 0x10000)
        text = d[e:end if 0 <= end < limit else limit]
    return mark, text


def debug_ll(d, off):
    """The low-level debug text of the area at off + 0x8000, or None without its header."""
    a = off + 0x8000
    if off < 0 or a + 16 > len(d) or d[a:a + 4] != b"RLOG":
        return None
    n = struct.unpack_from("<I", d, a + 4)[0]
    n = max(16, min(n, 0x8000, len(d) - a))
    return d[a + 16:a + n]


def layout(size, record, console, ftrace, pmsg):
    """[(name, offset, zone size)] in mainline's order."""
    dm = size - console - ftrace - pmsg
    if dm < 0:
        raise ValueError("console + ftrace + pmsg are larger than the region")
    z, off = [], 0
    if record > 0 and dm >= record:
        n = dm // record
        zs = dm // n
        for i in range(n):
            z.append((f"dmesg-{i + 1}", off + i * zs, zs))
    off += dm
    for name, s in (("console", console), ("ftrace", ftrace), ("pmsg", pmsg)):
        if s > 0:
            z.append((name, off, s))
        off += s
    return z


def main(argv):
    args, opt = [], {"size": 0x80000, "record": 0x10000, "console": 0x40000, "ftrace": 0, "pmsg": 0x10000, "early": 0x20000}
    it = iter(argv[1:])
    for a in it:
        if a.startswith("--") and a[2:] in opt:
            try:
                opt[a[2:]] = int(next(it), 0)
            except (StopIteration, ValueError):
                sys.exit(print(__doc__, file=sys.stderr) or 2)
        else:
            args.append(a)
    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    d = open(args[0], "rb").read()
    try:
        zones = layout(opt["size"], opt["record"], opt["console"], opt["ftrace"], opt["pmsg"])
    except ValueError as e:
        print(f"ramoops: {e}", file=sys.stderr)
        return 2
    if len(d) < opt["size"]:
        print(f"ramoops: note: the file is {len(d)} bytes, the layout {opt['size']}")
    good = 0
    for name, off, zs in zones:
        if name == "ftrace":
            print(f"ramoops: {name} at +0x{off:x}: skipped (not decoded)")
            continue
        t = zone(d, off, zs)
        if t is None:
            print(f"ramoops: {name} at +0x{off:x} ({zs} bytes): no valid zone")
            continue
        if not t:
            print(f"ramoops: {name} at +0x{off:x}: valid, empty")
            good += 1
            continue
        if name.startswith("dmesg"):
            t = dmesg_text(t)
        f = f"{args[1]}/{name}.txt"
        open(f, "wb").write(t)
        good += 1
        print(f"ramoops: {name} at +0x{off:x}: {len(t)} bytes -> {f}")
    mark, text = early(d, opt["early"]) if opt["early"] >= 0 else (None, None)
    if mark:
        open(f"{args[1]}/zimage-mark.txt", "w").write("\n".join(mark) + "\n")
        for line in mark:
            print(f"ramoops: early +0x{opt['early'] + 0x10:x}: {line}")
        good += 1
    elif opt["early"] >= 0:
        print(f"ramoops: early +0x{opt['early'] + 0x10:x}: no zImage marker")
    if text is not None:
        f = f"{args[1]}/earlycon.txt"
        open(f, "wb").write(text)
        print(f"ramoops: early console at +0x{opt['early'] + 0x80:x}: {len(text)} bytes -> {f}")
        good += 1
    elif opt["early"] >= 0:
        print(f"ramoops: early console at +0x{opt['early'] + 0x80:x}: none")
    dl = debug_ll(d, opt["early"]) if opt["early"] >= 0 else None
    if dl is not None:
        f = f"{args[1]}/debug-ll.txt"
        open(f, "wb").write(dl)
        print(f"ramoops: low-level debug at +0x{opt['early'] + 0x8000:x}: {len(dl)} bytes -> {f}")
        good += 1
    elif opt["early"] >= 0:
        print(f"ramoops: low-level debug at +0x{opt['early'] + 0x8000:x}: none")
    if not good:
        print("ramoops: no valid zone: this is the vendor kernel's own last_log text (or another layout: --record/--console/--pmsg)")
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
