"""Read Rockchip boot headers, RSCE entries and flattened device trees.

Used by the Linux boot-image builder; no vendor-image patching commands.
"""
import hashlib
import struct
import sys

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def bootimg_parts(data):
    """Offsets and sizes of kernel, ramdisk and second in an Android boot image (header v0)."""
    ks, _ka, rs, _ra, ss, _sa, _ta, ps = struct.unpack_from("<8I", data, 8)
    pages = lambda n: (n + ps - 1) // ps
    ko = ps
    ro = ko + pages(ks) * ps
    so = ro + pages(rs) * ps
    return (ko, ks), (ro, rs), (so, ss)


def bootimg_id(data):
    """The 20-byte id the board's u-boot computes for a boot image."""
    (ko, ks), (ro, rs), (so, ss) = bootimg_parts(data)
    h = hashlib.sha1()
    h.update(data[ko:ko + ks]); h.update(data[8:12])
    h.update(data[ro:ro + rs]); h.update(data[16:20])
    h.update(data[so:so + ss]); h.update(data[24:28])
    h.update(data[32:576])
    return h.digest()


def locate_dtb(data):
    """Return (offset, size) of the DTB in data: the whole file, an RSCE entry, or the RSCE
    entry in a boot image's second stage."""
    if data[:8] == b"ANDROID!":
        if bootimg_id(data) != data[576:596]:
            sys.exit("boot image id does not match its contents")
        _k, _r, (so, ss) = bootimg_parts(data)
        if ss == 0:
            sys.exit("boot image has no second stage")
        off, size = locate_dtb(data[so:so + ss])
        return so + off, size
    if data[:4] == b"RSCE":
        tbl_off, ent_size = data[9], data[10]
        (num,) = struct.unpack_from("<I", data, 12)
        for i in range(num):
            o = (tbl_off + i * ent_size) * 512
            if data[o:o + 4] != b"ENTR":
                sys.exit(f"RSCE entry {i} has no ENTR tag")
            name = data[o + 4:o + 260].split(b"\0")[0].decode()
            off, size = struct.unpack_from("<II", data, o + 260)
            if name == "rk-kernel.dtb":
                return off * 512, size
        sys.exit("no rk-kernel.dtb entry in the RSCE image")
    return 0, len(data)


def parse(dtb):
    """Walk the struct block. Returns {path: [(name, value_offset_in_dtb, value_bytes)]}."""
    magic, total, off_struct, off_strings, _rsv, version = struct.unpack_from(">6I", dtb, 0)
    if magic != FDT_MAGIC:
        sys.exit("not an FDT (bad magic)")
    if total != len(dtb):
        sys.exit(f"FDT totalsize {total} != blob size {len(dtb)}")
    size_struct = struct.unpack_from(">I", dtb, 36)[0] if version >= 17 else total - off_struct

    def string(o):
        s = off_strings + o
        return dtb[s:dtb.index(b"\0", s)].decode()

    nodes, stack, p = {}, [], off_struct
    end = off_struct + size_struct
    while p < end:
        (tok,) = struct.unpack_from(">I", dtb, p)
        p += 4
        if tok == FDT_BEGIN_NODE:
            n = dtb.index(b"\0", p)
            name = dtb[p:n].decode()
            p = (n + 1 + 3) & ~3
            stack.append(name)
            path = "/" + "/".join(stack[1:])
            if path in nodes:
                sys.exit(f"duplicate node path {path}")
            nodes[path] = []
        elif tok == FDT_END_NODE:
            stack.pop()
        elif tok == FDT_PROP:
            ln, nameoff = struct.unpack_from(">II", dtb, p)
            p += 8
            nodes["/" + "/".join(stack[1:])].append((string(nameoff), p, dtb[p:p + ln]))
            p = (p + ln + 3) & ~3
        elif tok == FDT_NOP:
            pass
        elif tok == FDT_END:
            break
        else:
            sys.exit(f"bad FDT token {tok:#x} at {p - 4:#x}")
    if stack:
        sys.exit("unbalanced FDT nodes")
    return nodes
