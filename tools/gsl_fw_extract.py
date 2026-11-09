#!/usr/bin/env python3
"""Extract the Silead gslX680 touch firmware from the board's stock 3.10 kernel and write it in the
format the mainline silead driver loads (drivers/input/touchscreen/silead.c). No external tools.

  gsl_fw_extract.py <image> <outdir> [--symbol GSLX670_FW] [--expect-md5 MD5]

<image> is any of: the Android boot image (boot.img.orig, `ANDROID!`), the Rockchip kernel partition
image (kernel.img.orig, `KRNL` header + zImage), a bare zImage, or an already decompressed kernel.
The kernel's own kallsyms table is read to find the firmware array by name, and the driver's
`gsl_load_fw` is read as ARM code to confirm which array the gslX680 driver loads and how many
records it writes (read-only: nothing here talks to the board).

Output `<outdir>/gslx680-d708.fw`: the vendor array `struct fw_data { u32 offset:8; u32 val; }`
re-written as little-endian u32 pairs (offset, val), which is exactly what silead.c writes with
i2c_smbus_write_i2c_block_data(client, offset, 4, &val). The table is the vendor's: keep it out of
git (`build/` is ignored) and record only its md5 (docs/RK3128-LINUX.md "Touch firmware").
"""
import argparse
import hashlib
import re
import struct
import sys
import zlib

KERNEL_BASE = 0xC0008000      # virtual address of file offset 0 of the decompressed kernel
PAGE_REG = 0xF0               # page-select record: {0xf0, page}
PAGE_RECS = 33                # one page = 1 select record + 32 words at offsets 0x00..0x7c
OUT_NAME = "gslx680-d708.fw"


# --- LZO1X (the kernel is `lzop -9` compressed, the boot stub carries the lzop framing) ---------
def lzo1x_decompress(src, ip=0):
    out = bytearray()
    state = 0

    def lits(n):
        nonlocal ip
        out.extend(src[ip:ip + n])
        ip += n

    def match(dist, ln):
        if dist <= 0 or dist > len(out):
            raise ValueError("lzo: bad match distance %d at output %d" % (dist, len(out)))
        s = len(out) - dist
        if dist >= ln:
            out.extend(out[s:s + ln])
        else:
            for i in range(ln):
                out.append(out[s + i])

    if src[ip] > 17:
        t = src[ip] - 17
        ip += 1
        lits(t)
        state = t if t < 4 else 4
    while True:
        t = src[ip]
        ip += 1
        if t < 16:
            if state == 0:
                if t == 0:
                    while src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 15 + src[ip]
                    ip += 1
                lits(t + 3)
                state = 4
                continue
            if state == 4:
                match(1 + 0x800 + (t >> 2) + (src[ip] << 2), 3)
            else:
                match(1 + (t >> 2) + (src[ip] << 2), 2)
            ip += 1
            nxt = t & 3
        elif t >= 64:
            match(1 + ((t >> 2) & 7) + (src[ip] << 3), (t >> 5) + 1)
            ip += 1
            nxt = t & 3
        elif t >= 32:
            ln = t & 31
            if ln == 0:
                while src[ip] == 0:
                    ln += 255
                    ip += 1
                ln += 31 + src[ip]
                ip += 1
            v = src[ip] | (src[ip + 1] << 8)
            ip += 2
            match(1 + (v >> 2), ln + 2)
            nxt = v & 3
        else:
            ln = t & 7
            if ln == 0:
                while src[ip] == 0:
                    ln += 255
                    ip += 1
                ln += 7 + src[ip]
                ip += 1
            v = src[ip] | (src[ip + 1] << 8)
            ip += 2
            dist = ((t & 8) << 11) + (v >> 2)
            if dist == 0:
                return bytes(out)           # end-of-stream marker
            match(dist + 0x4000, ln + 2)
            nxt = v & 3
        lits(nxt)
        state = nxt


def lzop_decompress(buf, off):
    """Decompress the lzop file that starts at buf[off] (magic included)."""
    p = off + 9
    p += 6 + 2                                  # version, lib version, version needed, method, level
    flags, = struct.unpack_from(">I", buf, p)
    p += 4
    if flags & 0x800:
        p += 4                                  # filter
    p += 12                                     # mode, mtime low, mtime high
    p += 1 + buf[p] + 4                         # name, header checksum
    out = bytearray()
    while True:
        usz, = struct.unpack_from(">I", buf, p)
        p += 4
        if usz == 0:
            return bytes(out)
        csz, = struct.unpack_from(">I", buf, p)
        p += 4
        p += 4 * bool(flags & 0x001) + 4 * bool(flags & 0x100)          # uncompressed checksums
        if csz < usz:
            p += 4 * bool(flags & 0x002) + 4 * bool(flags & 0x200)      # compressed checksums
        blk = buf[p:p + csz]
        p += csz
        if csz == usz:
            out += blk
        else:
            o = lzo1x_decompress(blk)
            if len(o) != usz:
                raise ValueError("lzo block: %d bytes, header says %d" % (len(o), usz))
            out += o


# --- image unwrapping ----------------------------------------------------------------------
def unwrap(data):
    """Boot image / KRNL image / zImage / kernel -> (kind, decompressed kernel)."""
    kind = "raw"
    if data[:8] == b"ANDROID!":
        ks, _, _, _, _, _, _, ps = struct.unpack_from("<8I", data, 8)
        data = data[ps:ps + ks]
        kind = "boot.img"
    elif data[:4] == b"KRNL":
        size, = struct.unpack_from("<I", data, 4)
        data = data[8:8 + size]
        kind = "KRNL"
    if data[0x24:0x28] == b"\x18\x28\x6f\x01":  # ARM zImage magic 0x016f2818
        kind += "+zImage"
        for m in re.finditer(re.escape(b"\x89LZO\x00\r\n\x1a\n"), data):
            try:
                k = lzop_decompress(data, m.start())
            except (ValueError, IndexError, struct.error, AssertionError):
                continue
            if len(k) > 1 << 20:
                return kind + "+lzo", k
        for m in re.finditer(b"\x1f\x8b\x08", data):
            try:
                k = zlib.decompressobj(31).decompress(data[m.start():])
            except zlib.error:
                continue
            if len(k) > 1 << 20:
                return kind + "+gzip", k
        raise SystemExit("zImage payload: no lzo/gzip stream decompressed")
    return kind, data


# --- kallsyms (Linux 3.10 layout: addresses, num_syms, names, markers, token table) ----------
def kallsyms(d):
    n = len(d) // 4
    w = struct.unpack("<%dI" % n, d[:n * 4])
    best = None
    i = 0
    while i < n:
        if KERNEL_BASE <= w[i] < 0xC2000000:
            j = i
            while j + 1 < n and w[j] <= w[j + 1] < 0xD0000000:
                j += 1
            if j - i > 10000 and (best is None or j - i > best[1] - best[0]):
                best = (i, j)
            i = j + 1
        else:
            i += 1
    if best is None:
        raise SystemExit("kallsyms: no address table (kernel built without CONFIG_KALLSYMS?)")
    s, e = best
    num = e - s + 1
    if w[s + num] != num:
        raise SystemExit("kallsyms: num_syms does not follow the address table")
    p = (s + num + 1) * 4
    while d[p] == 0:
        p += 1
    ents, offs = [], []
    p0 = p
    for _ in range(num):
        offs.append(p - p0)
        ents.append(d[p + 1:p + 1 + d[p]])
        p += 1 + d[p]
    p = d.index(struct.pack("<II", 0, offs[256]), p - 8) + ((num + 255) // 256) * 4
    while d[p] == 0:
        p += 1
    toks = []
    for _ in range(256):
        q = d.index(b"\0", p)
        toks.append(d[p:q])
        p = q + 1
    syms = []
    for a, e1 in zip(w[s:s + num], ents):
        t = b"".join(toks[b] for b in e1)
        syms.append((a, chr(t[0]), t[1:].decode("latin1")))
    return syms


# --- firmware table ------------------------------------------------------------------------
def parse_table(d, off):
    """Records from file offset `off` while they keep the page layout; returns [(offset, val)]."""
    recs = []
    while off + 8 * PAGE_RECS <= len(d):
        page = [struct.unpack_from("<II", d, off + 8 * k) for k in range(PAGE_RECS)]
        if page[0][0] != PAGE_REG or page[0][1] > 0xFF:
            break
        if [r[0] for r in page[1:]] != list(range(0, 0x80, 4)):
            break
        recs += page
        off += 8 * PAGE_RECS
    return recs


def loader_check(d, syms, table_va):
    """Find the function whose literal pool holds table_va; return (name, movw immediates)."""
    text = [(a, nm) for a, t, nm in syms if t in "tT"]
    text.sort()
    for off in range(0, len(d) - 3, 4):
        if struct.unpack_from("<I", d, off)[0] != table_va:
            continue
        va = KERNEL_BASE + off
        func = [x for x in text if x[0] <= va][-1]
        imms = []
        for o in range(func[0] - KERNEL_BASE, off, 4):
            wd, = struct.unpack_from("<I", d, o)
            if wd >> 20 == 0xE30:                                   # movw rd, #imm16
                imms.append(((wd >> 4) & 0xF000) | (wd & 0xFFF))
        if imms:
            return func[1], func[0], imms
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("outdir")
    ap.add_argument("--symbol", default="GSLX670_FW",
                    help="kallsyms name of the array the gslX680 driver loads (default GSLX670_FW)")
    ap.add_argument("--expect-md5", help="fail unless the .fw file has this md5")
    a = ap.parse_args()

    raw = open(a.image, "rb").read()
    kind, kern = unwrap(raw)
    print("image: %s, %d bytes; kernel %d bytes (%s)" % (a.image, len(raw), len(kern), kind))
    m = re.search(rb"Linux version [^\n\0]+", kern)
    print("kernel:", m.group(0).decode() if m else "version string not found")

    syms = kallsyms(kern)
    cands = {nm: va for va, t, nm in syms if t in "rR" and nm.endswith("_FW")}
    print("firmware arrays in kallsyms:", ", ".join("%s@0x%08x" % kv for kv in sorted(cands.items())))
    if a.symbol not in cands:
        raise SystemExit("symbol %s not found among %s" % (a.symbol, sorted(cands)))
    va = cands[a.symbol]
    recs = parse_table(kern, va - KERNEL_BASE)
    pages = len(recs) // PAGE_RECS
    print("%s: %d records = %d pages of %d (page numbers %s..%s)" % (
        a.symbol, len(recs), pages, PAGE_RECS, recs[0][1], recs[-33][1]))

    # sanity checks: layout, 8-bit offsets, then agreement with the driver's loader
    assert recs and len(recs) % PAGE_RECS == 0
    raw_off = va - KERNEL_BASE
    for i in range(len(recs)):
        if struct.unpack_from("<I", kern, raw_off + 8 * i)[0] > 0xFF:
            raise SystemExit("record %d: offset word has bits above the 8-bit bitfield" % i)
    chk = loader_check(kern, syms, va)
    if chk is None:
        print("loader: no code references %s (cannot confirm the record count)" % a.symbol)
    else:
        name, fva, imms = chk
        print("loader: %s at 0x%08x references it; movw immediates in it: %s" % (name, fva, imms))
        if len(recs) not in imms:
            raise SystemExit("record count %d is not a loop bound of the loader (%s)" % (len(recs), imms))
        print("loader loop bound %d matches the table" % len(recs))

    blob = b"".join(struct.pack("<II", o, v) for o, v in recs)
    # the file must parse the way silead_ts_load_fw reads it: u32 offset, u32 val pairs
    back = [struct.unpack_from("<II", blob, 8 * i) for i in range(len(blob) // 8)]
    assert back == recs and len(blob) % 8 == 0 and all(o <= 0xFF for o, _ in back)

    import os
    os.makedirs(a.outdir, exist_ok=True)
    path = os.path.join(a.outdir, OUT_NAME)
    with open(path, "wb") as f:
        f.write(blob)
    md5 = hashlib.md5(blob).hexdigest()
    print("wrote %s: %d bytes, %d records, md5 %s" % (path, len(blob), len(recs), md5))
    if a.expect_md5 and a.expect_md5.lower() != md5:
        raise SystemExit("md5 mismatch: expected %s" % a.expect_md5)


if __name__ == "__main__":
    sys.exit(main())
