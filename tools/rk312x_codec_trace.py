#!/usr/bin/env python3
"""Reproduce the register writes the board's stock kernel makes to the RK312x audio codec on
playback power-up and power-down, and check the Linux port's codec driver against them
(development record #303, docs/RK3128-LINUX.md "Audio").

The stock kernel's rk312x_codec driver keeps `playback_power_up_list` / `playback_power_down_list`
as (register, value) tables, but its register accessor (`rk312x_set_init_value`) does not write
those values as they are: for the registers in `rk312x_init_bit_list` (HPOUT_CTL 0xb0, HPMIX_CTL
0xa8) a write that changes an enable bit becomes two or three hardware writes with the "work" bits
added or removed. This tool finds the tables and the init-bit list in the kernel image, models that
accessor on a register file that starts at 0, prints the hardware write trace, and with --driver
compares it with the arrays `rk3128_power_up[]` / `rk3128_power_down[]` of the port's driver (read
from the driver patch). Back-to-back identical writes are one write on both sides.

  rk312x_codec_trace.py [KERNEL_IMAGE] [--driver linux/patches/303-0002-*.patch]

KERNEL_IMAGE is a boot image, a KRNL image, a zImage or a bare kernel (default: the stock
kernel.img.orig stashed by #286). No dependencies beyond tools/gsl_fw_extract.py (the unwrapper).
Exit 0 when the trace is found and (with --driver) matches, 1 otherwise.
"""
import argparse
import glob
import importlib.util
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_IMAGE = "/var/tmp/linux-port/vendor-images/kernel.img.orig"

# the head of each table, as the driver source has it
UP_HEAD = [(0x18, 0x32), (0xa0, 0x48), (0xa0, 0x6a), (0xa4, 0x88)]
DOWN_HEAD = [(0xb0, 0xdb), (0xa8, 0x44), (0xac, 0x00), (0xb0, 0x92)]
UP_LEN = 15       # entries before the two gain writes (0xb4, 0xb8)
DOWN_LEN = 13     # up to and including the second {0x18, 0x22}
# rk312x_init_bit_list: {reg, power_bit, init2_bit, init1_bit, init0_bit}
INIT_LIST_HEAD = [0xb0, 0x80, 0x40, 0x02, 0x00, 0xb0, 0x10, 0x08, 0x01, 0x00,
                  0xa8, 0x04, 0x01, 0x02, 0x00, 0xa8, 0x40, 0x10, 0x20, 0x00]


def load_kernel(path):
    spec = importlib.util.spec_from_file_location("gsl_fw_extract", os.path.join(HERE, "gsl_fw_extract.py"))
    g = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(g)
    with open(path, "rb") as f:
        return g.unwrap(f.read())[1]


def find_once(d, pat, what):
    hits = [m.start() for m in re.finditer(re.escape(pat), d) if m.start() % 4 == 0]
    if len(hits) != 1:
        sys.exit("%s: expected exactly one match in the kernel, found %d" % (what, len(hits)))
    return hits[0]


def read_pairs(d, off, n):
    return [struct.unpack_from("<II", d, off + 8 * i) for i in range(n)]


class Accessor:
    """rk312x_codec_write() / rk312x_set_init_value() over a register file."""

    def __init__(self, init_list):
        self.init = init_list          # list of (reg, power, init2, init1)
        self.hw = {}
        self.trace = []

    def w(self, reg, val):
        self.hw[reg] = val
        self.trace.append((reg, val))

    def write(self, reg, value):
        idx = [i for i, e in enumerate(self.init) if e[0] == reg]
        if not idx:
            self.w(reg, value)
            return
        read_value = self.hw.get(reg, 0)
        for i in idx:
            _, power, set2, set1 = self.init[i]
            if (read_value & power) != (value & power):
                if value & power:
                    tmp = value | set2 | set1
                    self.w(reg, value)
                    self.w(reg, tmp)
                else:
                    tmp = value & ~set2 & ~set1
                    self.w(reg, tmp)
                    self.w(reg, value)
                value = tmp
            elif read_value != value:
                self.w(reg, value)


def collapse(trace):
    out = []
    for t in trace:
        if not out or out[-1] != t:
            out.append(t)
    return out


def parse_driver(patch):
    text = open(patch).read()
    names = dict(re.findall(r"^\+#define\s+(RK3128_[A-Z0-9_]+)\s+(0x[0-9a-fA-F]+)\s*$", text, re.M))
    out = {}
    for arr in ("rk3128_power_up", "rk3128_power_down"):
        m = re.search(r"\+static const struct rk3128_step %s\[\] = \{\n(.*?)\n\+\};" % arr, text, re.S)
        if not m:
            sys.exit("no %s[] in %s" % (arr, patch))
        steps = []
        for reg, val in re.findall(r"\{\s*(\w+),\s*(0x[0-9a-fA-F]+)\s*\}", m.group(1)):
            steps.append((int(names[reg], 16) if reg in names else int(reg, 0), int(val, 16)))
        out[arr] = steps
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", nargs="?", default=DEFAULT_IMAGE)
    ap.add_argument("--driver", help="driver patch to check (default: linux/patches/303-0002-*.patch if present)")
    a = ap.parse_args()

    d = load_kernel(a.image)
    pk = lambda pairs: b"".join(struct.pack("<II", r, v) for r, v in pairs)
    up_off = find_once(d, pk(UP_HEAD), "playback_power_up_list")
    down_off = find_once(d, pk(DOWN_HEAD), "playback_power_down_list")
    il_off = find_once(d, struct.pack("<20I", *INIT_LIST_HEAD), "rk312x_init_bit_list")
    up = read_pairs(d, up_off, UP_LEN)
    down = read_pairs(d, down_off, DOWN_LEN)
    gains = read_pairs(d, up_off + 8 * UP_LEN, 2)
    if [g[0] for g in gains] != [0xb4, 0xb8]:
        sys.exit("the up list is not followed by the gain writes: %r" % gains)
    il = [struct.unpack_from("<5I", d, il_off + 20 * i)[:4] for i in range(4)]
    print("stock kernel tables: up list %d entries, down list %d, init-bit list %d, gain %#x"
          % (len(up), len(down), len(il), gains[0][1]))

    traces = {}
    hw = None
    for name, tbl in (("rk3128_power_up", up), ("rk3128_power_down", down)):
        acc = Accessor(il)
        if hw is not None:
            acc.hw = dict(hw)           # power-down starts from the powered-up state
        for reg, val in tbl:
            acc.write(reg, val)
        if name == "rk3128_power_up":
            hw = acc.hw
        # the gain writes (0xb4/0xb8) are the driver's separate volume handling
        traces[name] = collapse([t for t in acc.trace if t[0] not in (0xb4, 0xb8)])
        print("\n%s: list %s" % (name, " ".join("{%#x,%#x}" % t for t in tbl)))
        print("%s: hardware writes (%d):" % (name, len(traces[name])))
        print("  " + " ".join("%#x=%#x" % t for t in traces[name]))

    drv = a.driver
    if drv is None:
        found = sorted(glob.glob(os.path.join(HERE, "..", "linux", "patches", "303-0002-*.patch")))
        drv = found[0] if found else None
    if drv is None:
        return 0
    want = parse_driver(drv)
    rc = 0
    for name in ("rk3128_power_up", "rk3128_power_down"):
        mine = collapse(want[name])
        ok = mine == traces[name]
        print("\n%s: driver %s the stock trace (%d writes)" % (name, "MATCHES" if ok else "DIFFERS FROM", len(mine)))
        if not ok:
            rc = 1
            print("  stock: " + " ".join("%#x=%#x" % t for t in traces[name]))
            print("  driver: " + " ".join("%#x=%#x" % t for t in mine))
    return rc


if __name__ == "__main__":
    sys.exit(main())
