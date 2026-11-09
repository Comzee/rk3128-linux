#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recheck #725's archived raw negative captures; no driver/emitter dependency.
Usage: python3 725-negative-proof.py ARCHIVE (contains capture-diag{,2}/).
"""
from pathlib import Path
import json
import sys

root = Path(sys.argv[1])
result = {}
counts = [0, 1, 2, 15, 16, 254, 255, 256, 65535, 0xff0001, 0xffff0001]
last = set()
checked = 0
for opcode in range(2):
    for j, count in enumerate(counts):
        for ordinal, start in enumerate([0, 1, 255]):
            actual = (root / 'capture-diag2' / f'loads-op{opcode}-count{count}-start{start}.bin').read_bytes()
            expected = bytes((i*37+i//4+j*47+ordinal*13+opcode*41) & 255 for i in range(1024))
            loaded = 255 if opcode == 0 else count & 255
            assert actual[:loaded*4] == expected[:loaded*4]
            last.add(tuple(actual[1020:1023]))
            assert actual[1020:1023] != expected[1020:1023]
            checked += 1
assert len(last) == 1
result['lut'] = dict(cases=checked, last_rgb=list(next(iter(last))),
                     opcode6_loads=255, opcode7_count_mask=255, start_ignored=True)

packing = []
for mode in range(4):
    raw = (root / 'capture-diag' / f'packing-input{mode}.bin').read_bytes()
    pixels = [int.from_bytes(raw[i:i+4], 'little') for i in range(0, len(raw), 4)]
    basis = [pixels[1 << b] for b in range(16)]
    assert pixels[0] == 0
    # Exhaustive superposition proves the observed bit permutation, including
    # expansion widths, rather than fitting just a few chosen colors.
    native_errors = []
    for swapped in [False, True]:
        errors = 0
        for i in range(65536):
            observed = 0
            for b in range(16):
                if i & (1 << b):
                    observed |= basis[b]
            assert observed == pixels[i]
            value = ((i & 255) << 8 | i >> 8) if swapped else i
            channels = [(value >> 10 & 31) << 3, (value >> 5 & 31) << 3,
                        (value & 31) << 3, 255 if value & 32768 else 0]
            if mode & 1:
                channels[0], channels[2] = channels[2], channels[0]
            errors += raw[4*i:4*i+4] != bytes(channels)
        assert errors
        native_errors.append(errors)
    packing.append(dict(mode=mode, bit_basis=basis, native1555_mismatches=native_errors))
for kind in range(2):
    for rb in range(2):
        a = (root / 'capture-diag' / f'packing-output{kind}-f{rb}.bin').read_bytes()
        b = (root / 'capture-diag' / f'packing-output{kind}-f{rb+2}.bin').read_bytes()
        assert a == b
result['packing'] = dict(exhaustive_inputs=packing, destination_alpha_swap_ignored=True)

csc = []
for selector in [1, 2, 3]:
    for clip in [0, 1]:
        raw = (root / 'capture-diag2' / f'csc{selector}-clip{clip}.bin').read_bytes()
        actual = [[raw[2*i], raw[8192+2*i], raw[8192+2*i+1]] for i in range(4096)]
        models = []
        for standard, kr, kb in [(601, .299, .114), (709, .2126, .0722)]:
            for full in [False, True]:
                maximum = [0, 0, 0]
                errors = [0, 0, 0]
                for i, pixel in enumerate(actual):
                    r, g, b = (i % 16)*17, (i//16 % 16)*17, (i//256)*17
                    y = kr*r+(1-kr-kb)*g+kb*b
                    expected = [y if full else 16+y*219/255,
                                128+(b-y)/(2*(1-kb))*(1 if full else 224/255),
                                128+(r-y)/(2*(1-kr))*(1 if full else 224/255)]
                    for channel in range(3):
                        error = abs(pixel[channel]-max(0, min(255, round(expected[channel]))))
                        maximum[channel] = max(maximum[channel], error)
                        errors[channel] += error > 1
                models.append(dict(standard=standard, full=full, max_error=maximum, errors_gt1=errors))
        if selector == 3:
            assert all(any(m['errors_gt1']) for m in models if m['standard'] == 709)
        clamped_errors = None
        if selector == 3 and not clip:
            clamped_errors = [0, 0, 0]
            for i, pixel in enumerate(actual):
                r, g, b = (i % 16)*17, (i//16 % 16)*17, (i//256)*17
                y = .2126*r+.7152*g+.0722*b
                expected = [max(16, min(235, round(y))),
                            max(19, min(237, round(128+(b-y)/(2*(1-.0722))))),
                            max(19, min(237, round(128+(r-y)/(2*(1-.2126)))))]
                for c in range(3):
                    clamped_errors[c] += abs(pixel[c]-expected[c]) > 1
            assert not any(clamped_errors)
        csc.append(dict(selector=selector, clip=clip, clamped709_errors_gt1=clamped_errors,
                        ranges=[[min(p[c] for p in actual), max(p[c] for p in actual)] for c in range(3)],
                        references=models))
result['destination_csc'] = csc
print(json.dumps(result, indent=2))
