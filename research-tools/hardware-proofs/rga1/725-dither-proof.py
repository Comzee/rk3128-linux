#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent native-fill Bayer model for the archived 256-level basis.
Usage: python3 725-dither-proof.py ARCHIVE/capture-public5
"""
from pathlib import Path
import json
import sys

root = Path(sys.argv[1])
bayer = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5]
errors = 0
for level in range(256):
    raw = (root / f'fill-dither-{level}.bin').read_bytes()
    assert len(raw) == 8192
    for y in range(64):
        for x in range(64):
            d = bayer[(y & 3)*4+(x & 3)]
            rb = (level*31+31+(d >> 1)*32) >> 8
            g = (level*63+63+(d >> 2)*64) >> 8
            expected = (rb << 11) | (g << 5) | rb
            offset = (y*64+x)*2
            errors += int.from_bytes(raw[offset:offset+2], 'little') != expected
assert not errors
result = dict(levels=256, pixels=256*4096, channels=256*4096*3, errors=errors, bayer=bayer)
result['pass'] = True
print(json.dumps(result, indent=2))
