#!/usr/bin/env python3
"""Build a Rockchip Android boot image for the board from a zImage, a DTB and an initramfs
(development record #286, docs/RK3128-LINUX.md "Build pipeline"). No mkbootimg, no Pillow, no dtc.

  rkbootimg.py cpio  <manifest> <out.cpio.gz>      deterministic newc cpio (gzip -9, no name, mtime 0)
  rkbootimg.py pack  --kernel K --dtb D --ramdisk R --cmdline STR --out IMG [--pad N] [--max N]
  rkbootimg.py check <img> [--max N]               prove the rules below on an existing image
  rkbootimg.py ramdisk-file <img> <path> [--md5 M] md5 of one file in the image's initramfs (exit 1: missing or not M)

Why not AOSP mkbootimg (HARDWARE.md, GOTCHAS.md): the board's u-boot (bootrk) takes the FDT from the
`second` stage, an RSCE image with an `rk-kernel.dtb` entry (an appended DTB is ignored), checks a
header `id` that is its own SHA1 (tools/fdt.py bootimg_id), and hashes every part on the RK
crypto engine in whole 32-bit words, so each part size must be a multiple of 4. `pack` therefore
writes: header (page size 16384, the stock addresses; bootrk overrides kernel and ramdisk addresses
itself), the zImage, the gzip ramdisk, an RSCE with the one entry `rk-kernel.dtb`, every part
zero-padded to a multiple of 4 and then to the page size, the id, and optionally zero padding up to
--pad (default none: the image is only as long as its content, a few MB, and the partition keeps
whatever follows it; --pad 33554432 makes a full-partition file for a read-back compare). The logos stay in the resource partition (p3), which u-boot
reads for them (cmd_bootrk.c: kernel logo "default load from resource").

`pack` re-opens its output and runs `check`, which proves: magic, id == bootimg_id, kernel/ramdisk/
second sizes multiples of 4, zImage magic 0x016f2818 at +36, ramdisk is a gzip stream, the DTB is
found by fdt.locate_dtb and parses (fdt.parse), the image fits --max (default 32 MiB:
`recovery` is 0x10000 sectors) and the header cmdline is under 512 bytes. Output is deterministic.
"""
import argparse
import gzip
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fdt  # noqa: E402

PAGE = 16384
MAX_IMG = 32 * 1024 * 1024  # the recovery partition (p6), HARDWARE.md
# stock boot.img.orig header values (header v0 as the board's u-boot reads it)
KERNEL_ADDR, RAMDISK_ADDR, SECOND_ADDR, TAGS_ADDR = 0x60408000, 0x62000000, 0x60f00000, 0x60088000
ZIMAGE_MAGIC = 0x016F2818


def die(msg):
    sys.exit(f"rkbootimg: {msg}")


# --- cpio -----------------------------------------------------------------------------------

def parse_manifest(path):
    """Lines (gen_init_cpio style, paths absolute, mode octal):
         dir   /path mode
         file  /path source mode
         slink /path target mode
         nod   /path mode c|b major minor
    """
    ents = []
    base = os.path.dirname(os.path.abspath(path))
    for n, raw in enumerate(open(path), 1):
        line = raw.split("#", 1)[0].split()
        if not line:
            continue
        k = line[0]
        try:
            if k == "dir":
                ents.append((line[1], 0o040000 | int(line[2], 8), b"", (0, 0)))
            elif k == "file":
                src = line[2] if os.path.isabs(line[2]) else os.path.join(base, line[2])
                ents.append((line[1], 0o100000 | int(line[3], 8), open(src, "rb").read(), (0, 0)))
            elif k == "slink":
                ents.append((line[1], 0o120000 | int(line[3], 8), line[2].encode(), (0, 0)))
            elif k == "nod":
                kind = 0o020000 if line[3] == "c" else 0o060000
                ents.append((line[1], kind | int(line[2], 8), b"", (int(line[4]), int(line[5]))))
            else:
                die(f"{path}:{n}: unknown entry kind {k!r}")
        except (IndexError, ValueError, OSError) as e:
            die(f"{path}:{n}: {e}")
    return ents


def newc(ents):
    """Deterministic newc archive: sorted by path, inode = position, uid/gid/mtime 0."""
    out = bytearray()
    for ino, (path, mode, data, (rmaj, rmin)) in enumerate(sorted(ents, key=lambda e: e[0]), 1):
        name = path.lstrip("/").encode() + b"\0"
        nlink = 2 if mode & 0o170000 == 0o040000 else 1
        hdr = "070701" + "".join("%08x" % v for v in (
            ino, mode, 0, 0, nlink, 0, len(data), 0, 0, rmaj, rmin, len(name), 0))
        out += hdr.encode() + name
        out += b"\0" * (-len(out) % 4)
        out += data
        out += b"\0" * (-len(out) % 4)
    tr = b"TRAILER!!!\0"
    out += ("070701" + "".join("%08x" % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(tr), 0))).encode() + tr
    out += b"\0" * (-len(out) % 4)
    out += b"\0" * (-len(out) % 512)
    return bytes(out)


def gz(data):
    """gzip -9 with an empty name and mtime 0 (what `gzip -n -9` writes), so the bytes repeat."""
    return gzip.compress(data, compresslevel=9, mtime=0)


def cmd_cpio(a):
    ents = parse_manifest(a.manifest)
    names = [e[0] for e in ents]
    if len(set(names)) != len(names):
        die("manifest has a path twice")
    if "/init" not in names:
        die("manifest has no /init")
    blob = gz(newc(ents))
    blob += b"\0" * (-len(blob) % 4)
    open(a.out, "wb").write(blob)
    print(f"{a.out}: {len(ents)} entries, {len(blob)} bytes")


# --- RSCE and boot image ----------------------------------------------------------------------

def rsce(dtb):
    """A resource image with the one entry rk-kernel.dtb at block 2 (header and table: 1 block each)."""
    hdr = b"RSCE" + b"\0" * 4 + bytes((1, 1, 1, 0)) + struct.pack("<I", 1)
    hdr += b"\0" * (512 - len(hdr))
    ent = b"ENTR" + b"rk-kernel.dtb".ljust(256, b"\0") + struct.pack("<II", 2, len(dtb))
    ent += b"\0" * (512 - len(ent))
    body = dtb + b"\0" * (-len(dtb) % 512)
    return hdr + ent + body


def pad(b, n):
    return b + b"\0" * (-len(b) % n)


def cmd_pack(a):
    kernel = open(a.kernel, "rb").read()
    dtb = open(a.dtb, "rb").read()
    ramdisk = open(a.ramdisk, "rb").read()
    cmdline = a.cmdline.encode()
    if len(cmdline) > 255:
        die(f"cmdline is {len(cmdline)} bytes: u-boot appends about 300 more to a 512 byte buffer")
    if struct.unpack_from(">I", dtb)[0] != fdt.FDT_MAGIC:
        die("--dtb is not a flattened device tree")
    kernel, ramdisk, second = pad(kernel, 4), pad(ramdisk, 4), pad(rsce(dtb), 4)
    hdr = bytearray(PAGE)
    hdr[0:8] = b"ANDROID!"
    struct.pack_into("<8I", hdr, 8, len(kernel), KERNEL_ADDR, len(ramdisk), RAMDISK_ADDR,
                     len(second), SECOND_ADDR, TAGS_ADDR, PAGE)
    hdr[64:64 + len(cmdline)] = cmdline
    img = bytearray(hdr) + pad(kernel, PAGE) + pad(ramdisk, PAGE) + pad(second, PAGE)
    img[576:596] = fdt.bootimg_id(bytes(img))
    if a.pad:
        if len(img) > a.pad:
            die(f"image is {len(img)} bytes, over --pad {a.pad}")
        img += b"\0" * (a.pad - len(img))
    open(a.out, "wb").write(img)
    try:
        check(a.out, a.max)
    except SystemExit:
        os.unlink(a.out)
        raise
    print(f"{a.out}: {len(img)} bytes, kernel {len(kernel)}, ramdisk {len(ramdisk)}, second {len(second)}")


def check(path, max_size):
    d = open(path, "rb").read()
    if d[:8] != b"ANDROID!":
        die("not an Android boot image")
    (ko, ks), (ro, rs), (so, ss) = fdt.bootimg_parts(d)
    if fdt.bootimg_id(d) != d[576:596]:
        die("header id does not match the contents")
    for name, n in (("kernel", ks), ("ramdisk", rs), ("second", ss)):
        if n % 4:
            die(f"{name} size {n} is not a multiple of 4 (u-boot refuses it)")
    if struct.unpack_from("<I", d, ko + 36)[0] != ZIMAGE_MAGIC:
        die("kernel is not a zImage (magic at +36)")
    try:
        zlib.decompressobj(31).decompress(d[ro:ro + rs])
    except zlib.error as e:
        die(f"ramdisk is not a gzip stream: {e}")
    if len(d) > max_size:
        die(f"image is {len(d)} bytes, over the {max_size} byte partition")
    cmd = d[64:576].split(b"\0")[0]
    if len(cmd) > 255:
        die("header cmdline too long")
    off, size = fdt.locate_dtb(d)
    fdt.parse(d[off:off + size])
    return d


def cmd_check(a):
    d = check(a.img, a.max)
    (ko, ks), (ro, rs), (so, ss) = fdt.bootimg_parts(d)
    print(f"{a.img}: OK, {len(d)} bytes, kernel {ks}, ramdisk {rs}, second {ss}, "
          f"id {d[576:596].hex()}, cmdline {d[64:576].split(chr(0).encode())[0].decode()!r}")


def cpio_files(blob):
    """name -> data of the regular files of a newc cpio stream."""
    files, pos = {}, 0
    while pos + 110 <= len(blob):
        if blob[pos:pos + 6] != b"070701":
            die("the initramfs is not a newc cpio")
        f = [int(blob[pos + 6 + 8 * i:pos + 14 + 8 * i], 16) for i in range(13)]
        mode, filesize, namesize = f[1], f[6], f[11]
        name = blob[pos + 110:pos + 110 + namesize - 1].decode()
        pos += (110 + namesize + 3) & ~3
        if name == "TRAILER!!!":
            break
        if (mode & 0o170000) == 0o100000:
            files[name] = blob[pos:pos + filesize]
        pos += (filesize + 3) & ~3
    return files


def cmd_ramdisk_file(a):
    import hashlib
    d = check(a.img, MAX_IMG)
    (ko, ks), (ro, rs), (so, ss) = fdt.bootimg_parts(d)
    files = cpio_files(zlib.decompressobj(31).decompress(d[ro:ro + rs]))
    want = a.path.lstrip("/")
    data = files.get(want)
    if data is None:
        sys.exit(f"rkbootimg: {a.img}: the initramfs has no file /{want}")
    got = hashlib.md5(data).hexdigest()
    if a.md5 and got != a.md5:
        sys.exit(f"rkbootimg: {a.img}: /{want} has md5 {got}, not {a.md5}")
    print(f"{got}  /{want} ({len(data)} bytes)")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sp = p.add_subparsers(dest="cmd", required=True)
    c = sp.add_parser("cpio"); c.add_argument("manifest"); c.add_argument("out"); c.set_defaults(f=cmd_cpio)
    k = sp.add_parser("pack")
    for n in ("kernel", "dtb", "ramdisk", "cmdline", "out"):
        k.add_argument("--" + n, required=True)
    k.add_argument("--pad", type=int, default=0)
    k.add_argument("--max", type=int, default=MAX_IMG)
    k.set_defaults(f=cmd_pack)
    h = sp.add_parser("check"); h.add_argument("img"); h.add_argument("--max", type=int, default=MAX_IMG)
    h.set_defaults(f=cmd_check)
    r = sp.add_parser("ramdisk-file"); r.add_argument("img"); r.add_argument("path"); r.add_argument("--md5")
    r.set_defaults(f=cmd_ramdisk_file)
    a = p.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
