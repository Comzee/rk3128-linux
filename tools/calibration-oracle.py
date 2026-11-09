#!/usr/bin/env python3
"""Read-only #579 calibration oracle (needs the optional Python unicorn package).

calibration-oracle.py <decompressed stock Image> <output.bin>
See docs/research/touch-579.md for binary provenance, addresses and limitations.
No vendor code or configuration table is distributed with this tool.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR

EXPECTED_IMAGE = "01a2967904b975307884092778f313213a6de2ef2ecc841233ae5c4fe2a24729"
BASE = 0xC0008000
STATE = 0xC0D8172C
CONFIG = 0xC0C4A4B8
STOP = 0x180000
SCRATCH = 0x100000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--filters", action="store_true", help="also measure the vendor point/filter chain")
    args = parser.parse_args()
    with args.image.open("rb") as source:
        blob = source.read(20 * 1024 * 1024 + 1)
    if hashlib.sha256(blob).hexdigest() != EXPECTED_IMAGE:
        parser.error("requires the documented stock decompressed kernel Image (SHA256 mismatch)")
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.mem_map(0xC0000000, 0x2000000)
    cpu.mem_write(BASE, blob)
    cpu.mem_map(SCRATCH, 0x100000)

    def signed(value):
        return value if value < 2**31 else value - 2**32

    def divide(uc, _address, _size, _data):
        # Kernel __aeabi_idiv: the only external helper reached by ScreenResolution.
        a = signed(uc.reg_read(UC_ARM_REG_R0))
        b = signed(uc.reg_read(UC_ARM_REG_R1))
        quotient = abs(a) // abs(b)
        if (a < 0) != (b < 0):
            quotient = -quotient
        uc.reg_write(UC_ARM_REG_R0, quotient & 0xFFFFFFFF)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    cpu.hook_add(UC_HOOK_CODE, divide, begin=0xC02BE87C, end=0xC02BE87C)

    def call(address, *arguments):
        cpu.reg_write(UC_ARM_REG_SP, 0x1F0000)
        cpu.reg_write(UC_ARM_REG_LR, STOP)
        for register, argument in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), arguments):
            cpu.reg_write(register, argument)
        cpu.emu_start(address, STOP, count=2000000)
        if cpu.reg_read(UC_ARM_REG_PC) != STOP:
            raise RuntimeError("vendor function exceeded instruction limit")
        return cpu.reg_read(UC_ARM_REG_R0)

    # This kernel's default gsl_config selector is 1, selecting CONFIG, not _1680f.
    assert blob[0xC0C4A444 + 0x880 - BASE] == 1
    call(0xC0510BB8, CONFIG)  # gsl_DataInit, initializes BSS from the real config
    for offset in range(0x7C0, 0x804, 4):
        value, = struct.unpack("<I", cpu.mem_read(STATE + offset, 4))
        print(f"state+0x{offset:x}: 0x{value:x}")
    output = bytearray()
    for axis in range(2):
        for raw in range(4096):
            x, y = (512, raw) if axis else (raw, 320)
            cpu.mem_write(SCRATCH, struct.pack("<I", x << 16 | y))
            word = call(0xC050DC9C, SCRATCH)  # ScreenResolution
            # Host packed result is Y:high16 / X:low16; worker reports these as evdev XY.
            output.extend(struct.pack("<HH", word & 0xFFFF, word >> 16))
    args.output.write_bytes(output)
    print(f"{len(output)} bytes; SHA256 {hashlib.sha256(output).hexdigest()}")
    if args.filters:
        filter_probe(cpu, call, args.output.with_suffix(".filters.json"))


def filter_probe(cpu, call, output):
    """Controlled input, not a hardware Android performance/jitter measurement."""
    def word(address):
        return struct.unpack("<I", cpu.mem_read(address, 4))[0]

    def reset():
        call(0xC0510BB8, CONFIG)
        # The driver's record_point counters/history, separate from library init.
        cpu.mem_write(0xC0D8307C, bytes(96))

    def direct(x, y):
        cpu.mem_write(SCRATCH + 0x200, struct.pack("<I", x << 16 | y))
        result = call(0xC050DC9C, SCRATCH + 0x200)
        return result & 0xFFFF, result >> 16

    def sample(x, y, record=False):
        contact = [0] * 31
        contact[0], contact[10], contact[30] = x, y, 1
        cpu.mem_write(SCRATCH, struct.pack("<31I", *contact))
        call(0xC05115E8, SCRATCH)
        result = struct.unpack("<31I", cpu.mem_read(SCRATCH, 124))
        if not result[30]:
            return None
        if record:
            call(0xC05118CC, result[0], result[10], result[20])
            y, x = struct.unpack("<HH", cpu.mem_read(0xC0D8307C + 88, 4))
            return x, y
        return result[10], result[0]

    reset()
    report = {"filter_mode": word(STATE + 0x700), "grid_count": 0, "grid_mismatches": 0}
    # Settle each point independently; check whether this config adds a static warp.
    for x in range(64, 961, 32):
        for y in range(32, 609, 32):
            reset()
            for _ in range(32):
                value = sample(x, y)
            report["grid_count"] += 1
            report["grid_mismatches"] += value != direct(x, y)
    for mode in ("raw", "library", "library+record_point"):
        reset()
        record = mode.endswith("record_point")
        for _ in range(32):
            sample(512, 320, record)
        values = []
        for n in range(96):
            y = 320 + (8 if n % 2 else -8)
            value = direct(512, y) if mode == "raw" else sample(512, y, record)
            if value is not None:
                values.append(value[1] * 1080 / 1281)
        report[mode] = {"samples": len(values), "span_px": max(values) - min(values),
                        "stddev_px": statistics.pstdev(values)}
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
