#!/usr/bin/env python3
"""RK3128 reference board point-algorithm differential oracle, #587 (optional Unicorn for --vendor).

Normal tests generate the deterministic corpus without vendor files or Unicorn.
--vendor IMAGE executes the archived, SHA256-checked ARM code and compares every
output AND raw/stretch/filter history against the production helper's runner.
See docs/research/touch-587.md for provenance, enabled stages and limitations.
"""
import argparse
import hashlib
import json
import random
import struct
import subprocess
from pathlib import Path

IMAGE_SHA = "01a2967904b975307884092778f313213a6de2ef2ecc841233ae5c4fe2a24729"
RESET = 0xFFFFFFFF
INPUT = struct.Struct("<11I")
OUTPUT = struct.Struct("<31I")


def record(points=(), flags=0):
    return (len(points) | flags, *(x << 16 | y for x, y in points), *([0] * (10 - len(points))))


def corpus():
    """Deterministic physical-domain and transport-boundary sequences, no tuning."""
    rng = random.Random(587)
    for limit, cases, span in ((1024, 60, 15), (4096, 20, 130)):
        for _ in range(cases):
            yield (RESET, *([0] * 10))
            points = []
            for n in range(240):
                if n % 30 == 0:
                    points = [(rng.randrange(limit), rng.randrange(640 if limit == 1024 else limit))
                              for _ in range(rng.randrange(6 if limit == 1024 else 11))]
                ymax = 639 if limit == 1024 else 4095
                points = [(max(0, min(limit - 1, x + rng.randrange(-span, span + 1))),
                           max(0, min(ymax, y + rng.randrange(-span, span + 1)))) for x, y in points]
                flags = rng.choice((0, 0, 0, 0x100, 0x200, 0x400, 0x1000, 0x2000, 0x4000, 0x12340000)) \
                    if limit == 4096 else 0
                yield record(points, flags)
    # Slow ramps, reversals, clipping, alternating sensor noise, rapid replacement,
    # reordered contacts, crossings, coincident contacts and all ten internal IDs.
    for count in (1, 2, 3, 5, 10):
        yield (RESET, *([0] * 10))
        for n in range(300):
            points = [((32 + i * 91 + (n if n < 150 else 300 - n)) % 1024,
                       (50 + i * 57 + (8 if n % 2 else -8)) % 640) for i in range(count)]
            if n % 2:
                points.reverse()
            if n % 37 == 0:
                points = []
            yield record(points)
        for _ in range(12):
            yield record()
    for x, y in ((0, 0), (0, 639), (1023, 0), (1023, 639), (512, 320), (4095, 4095)):
        yield (RESET, *([0] * 10))
        for n in range(140):
            yield record([(x, y + (n % 2))])
        for _ in range(12):
            yield record()


class Vendor:
    """Read-only execution of the actual RK3128 reference board library and record_point instructions."""

    def __init__(self, image):
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
        from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                                       UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR)
        self.regs = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)
        self.pc, self.sp, self.lr = UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR
        with image.open("rb") as source:
            blob = source.read(20 * 1024 * 1024 + 1)
        if hashlib.sha256(blob).hexdigest() != IMAGE_SHA:
            raise ValueError("requires the documented stock decompressed Image (SHA256 mismatch)")
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.cpu.mem_map(0xC0000000, 0x2000000)
        self.cpu.mem_write(0xC0008000, blob)
        self.cpu.mem_map(0x100000, 0x100000)

        def divide(cpu, _address, _size, _data):
            a, b = (cpu.reg_read(r) for r in self.regs[:2])
            a = a if a < 2**31 else a - 2**32
            b = b if b < 2**31 else b - 2**32
            quotient = abs(a) // abs(b)
            if (a < 0) != (b < 0):
                quotient = -quotient
            cpu.reg_write(self.regs[0], quotient & 0xFFFFFFFF)
            cpu.reg_write(self.pc, cpu.reg_read(self.lr))

        self.cpu.hook_add(UC_HOOK_CODE, divide, begin=0xC02BE87C, end=0xC02BE87C)
        self.reset()

    def call(self, address, *args):
        self.cpu.reg_write(self.sp, 0x1F0000)
        self.cpu.reg_write(self.lr, 0x180000)
        for register, argument in zip(self.regs, args):
            self.cpu.reg_write(register, argument)
        self.cpu.emu_start(address, 0x180000, count=2000000)
        if self.cpu.reg_read(self.pc) != 0x180000:
            raise RuntimeError("vendor call exceeded instruction budget")

    def word(self, address):
        return struct.unpack("<I", self.cpu.mem_read(address, 4))[0]

    def reset(self):
        self.call(0xC0510BB8, 0xC0C4A4B8)
        self.cpu.mem_write(0xC0D8307C, bytes(92))

    def process(self, row, histories=False):
        if row[0] == RESET:
            self.reset()
            return bytes(OUTPUT.size + (1200 if histories else 0))
        points = [0] * 31
        points[30] = row[0]
        for i, packed in enumerate(row[1:]):
            points[i], points[i + 10] = packed >> 16, packed & 0xFFFF
        self.cpu.mem_write(0x100000, OUTPUT.pack(*points))
        self.call(0xC05115E8, 0x100000)
        result = OUTPUT.unpack(self.cpu.mem_read(0x100000, OUTPUT.size))
        out, present = [0] * 31, set()
        out[0] = min(5, result[30])
        for i in range(out[0]):
            ident = result[20 + i]
            self.call(0xC05118CC, result[i], result[10 + i], ident)
            y, x = struct.unpack("<HH", self.cpu.mem_read(0xC0D830D4, 4))
            out[1 + 3 * i:4 + 3 * i] = x, y, ident - 1
            present.add(ident)
        # gslX680_ts_worker clears record_point's count for an unreported ID.
        for ident in range(1, 11):
            if ident not in present:
                self.cpu.mem_write(0xC0D8307C + ident * 4, bytes(4))
        data = OUTPUT.pack(*out)
        if histories:
            for offset in (0x40, 0x518, 0x540):
                for depth in range(10):
                    data += bytes(self.cpu.mem_read(self.word(0xC0D8172C + offset + depth * 4), 40))
        return data


def run_helper(runner, rows, raw=False):
    data = subprocess.run([str(runner.resolve()), "--raw" if raw else "--stream"],
                          input=b"".join(INPUT.pack(*row) for row in rows),
                          stdout=subprocess.PIPE, check=True).stdout
    if len(data) != len(rows) * OUTPUT.size:
        raise ValueError("runner returned an incomplete stream")
    return [OUTPUT.unpack_from(data, i * OUTPUT.size) for i in range(len(rows))]


def measure(runner, directory):
    """Controlled replay, NOT a new hardware Android or finger measurement."""
    from touch_parity import analyse
    directory.mkdir(parents=True, exist_ok=True)
    period = 0.02097  # #434 same-controller measured report interval
    report = {"method": "controlled pre-evdev replay; all input report timestamps retained",
              "period_ms": period * 1000, "steps": {}, "ramps": {}}
    for axis in range(2):
        endpoint = (576, 320) if axis == 0 else (512, 384)
        rows = [record([(512, 320)])] * 64 + [record([endpoint])] * 96
        filtered, raw = run_helper(runner, rows), run_helper(runner, rows, raw=True)
        values = [r[1 + axis] for r in filtered]
        start, target = values[63], raw[64][1 + axis]
        response = {}
        for percent in (10, 50, 90, 99):
            offset = next((i for i, v in enumerate(values[64:])
                           if abs(v - start) >= abs(target - start) * percent / 100), None)
            response[str(percent)] = {"reports": offset, "ms": round(offset * period * 1000, 2) if offset is not None else None}
        report["steps"]["xy"[axis]] = {"raw_step": 64, "response_percent": response,
                                           "final_error_evdev": values[-1] - target,
                                           "note": "deadband may intentionally retain a static residual"}
    for speed in (1, 4, 8):
        rows = [record([(128, 320)])] * 32 + [record([(128 + i * speed, 320)]) for i in range(96)]
        filtered, raw = run_helper(runner, rows), run_helper(runner, rows, raw=True)
        # Calibrated X slope is approximately -800*4161/(1024*4096).
        lag = [(a[1] - b[1]) / (speed * 800 * 4161 / (1024 * 4096))
               for a, b in zip(filtered[-40:], raw[-40:])]
        report["ramps"][str(speed)] = {"raw_units_per_report": speed,
                                             "mean_lag_reports": round(sum(lag) / len(lag), 3),
                                             "mean_lag_ms": round(sum(lag) / len(lag) * period * 1000, 2)}
    rows = [record([(512, 320 + (8 if i % 2 else -8))]) for i in range(240)]
    rows += [record()] * 12
    rows += [record([(96 + i * 3, 90 + i + (8 if i % 2 else -8))]) for i in range(280)]
    rows += [record()] * 12
    for name, values in (("raw", run_helper(runner, rows, raw=True)),
                         ("filtered", run_helper(runner, rows))):
        path = directory / (name + ".evdump")
        lines = ["# Controlled pre-evdev replay: every controller sample, before input-core deduplication.",
                 "evdump: replay x range 0..800", "evdump: replay y range 0..1280"]
        for i, row in enumerate(values):
            contacts = " ".join(f"[slot {row[3 + j * 3]} id {row[3 + j * 3] + 1} x {row[1 + j * 3]} y {row[2 + j * 3]}]"
                                for j in range(row[0]))
            lines.append(f"evdump: t={i * period:.6f} ts={100 + i * period:.6f} frame {i}: {contacts or 'no contact'}")
        path.write_text("\n".join(lines) + "\n")
        report[name] = analyse(str(path), (1920, 1080))
        report[name]["file"] = path.name
    (directory / "metrics.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, help="write the deterministic raw-report corpus")
    parser.add_argument("--vendor", type=Path, help="archived stock decompressed Image; needs Unicorn")
    parser.add_argument("--runner", type=Path, help="production C helper runner to compare with ARM")
    parser.add_argument("--expected", type=Path, help="write ARM output for independent replay")
    parser.add_argument("--measure", type=Path, help="production runner for controlled latency/jitter replay")
    parser.add_argument("--measure-dir", type=Path, help="write labelled replay captures and metrics here")
    args = parser.parse_args()
    if args.measure:
        if not args.measure_dir:
            parser.error("--measure needs --measure-dir")
        measure(args.measure, args.measure_dir)
    if not args.input:
        if args.vendor or not args.measure:
            parser.error("--input is required for corpus/oracle output")
        return
    rows = list(corpus())
    args.input.write_bytes(b"".join(INPUT.pack(*row) for row in rows))
    print(f"corpus: {len(rows)} records; SHA256 {hashlib.sha256(args.input.read_bytes()).hexdigest()}")
    if not args.vendor:
        return
    oracle = Vendor(args.vendor)
    expected = b"".join(oracle.process(row, histories=True) for row in rows)
    print(f"vendor outputs + histories SHA256 {hashlib.sha256(expected).hexdigest()}")
    if args.expected:
        args.expected.write_bytes(expected)
    if args.runner:
        actual = subprocess.run([str(args.runner.resolve()), "--histories"], input=args.input.read_bytes(),
                                stdout=subprocess.PIPE, check=True).stdout
        stride = OUTPUT.size + 1200
        if actual != expected:
            for i in range(len(rows)):
                if actual[i * stride:(i + 1) * stride] != expected[i * stride:(i + 1) * stride]:
                    raise AssertionError(f"first mismatch record {i}, raw={rows[i]}, "
                                         f"output expected={OUTPUT.unpack_from(expected, i * stride)} "
                                         f"actual={OUTPUT.unpack_from(actual, i * stride)}")
            raise AssertionError("runner output length mismatch")
        print(f"PASS: {len(rows)} records; every output and 300 history words byte-identical to stock ARM")


if __name__ == "__main__":
    main()
