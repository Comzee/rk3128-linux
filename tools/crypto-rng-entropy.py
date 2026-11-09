#!/usr/bin/env python3
"""Bounded raw-source estimates and health tests; not SP800-90B certification.

MCV confidence bounds assume IID samples; context/position/compression
checks expose some dependencies, not all possible predictors. FIPS 140-2 is
intentionally absent: an unconditioned source may be biased and still add entropy.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import zlib

ALPHA = 2 ** -20
RCT = 61                    # 1 + ceil(30 / 0.5)
APT = 422                   # 1 + Binomial(511, 2**-0.5) upper-tail cutoff
WINDOW = 512
BIT_BYTES = tuple(bytes((x >> i) & 1 for i in range(8)) for x in range(256))


def bound(success, total, hypotheses):
    """Per-family simultaneous Hoeffding upper bound under an IID model."""
    return min(1.0, success / total + math.sqrt(math.log(hypotheses / ALPHA) / (2 * total)))


def analyze(data):
    bits = b"".join(BIT_BYTES[x] for x in data)
    total = len(bits)
    ones = sum(bits)
    bit_p = bound(max(ones, total - ones), total, 2)
    byte_p = bound(max(Counter(data).values()), len(data), 256)
    pos_p = max(bound(max(sum(bits[i::256]), len(bits[i::256]) - sum(bits[i::256])),
                      len(bits[i::256]), 512) for i in range(256))
    counts = [[0, 0] for _ in range(256)]
    context = 0
    last, run, longest, apt_ref, apt_n, apt_count, apt_max, apt_fail = -1, 0, 0, 0, 0, 0, 0, 0
    for i, bit in enumerate(bits):
        # Disjoint 9-bit groups make the conditional observations independent
        # under the stated IID model; overlapping histories would not do so.
        if i % 9 == 8:
            counts[context][bit] += 1
        context = ((context << 1) | bit) & 255
        run = run + 1 if bit == last else 1
        longest = max(longest, run)
        last = bit
        if not apt_n:
            apt_ref, apt_count = bit, 0
        apt_n += 1
        apt_count += bit == apt_ref
        if apt_n == WINDOW:
            apt_max = max(apt_max, apt_count)
            apt_fail += apt_count >= APT
            apt_n = 0
    # Short contexts are formed by merging the low bits of the same 8-bit table.
    context_h = {}
    for width in range(1, 9):
        c = [[0, 0] for _ in range(1 << width)]
        for i, row in enumerate(counts):
            c[i & ((1 << width) - 1)][0] += row[0]
            c[i & ((1 << width) - 1)][1] += row[1]
        p = max(bound(max(row), sum(row), 1024) for row in c if sum(row))
        context_h[str(width)] = -math.log2(p)
    lag_h = {}
    for lag in (1, 2, 4, 8, 16, 32, 64, 256):
        same = pairs = 0
        # Pair disjoint bits in the two halves of each 2*lag block.
        for offset in range(lag):
            a, b = bits[offset::2*lag], bits[offset+lag::2*lag]
            pairs += min(len(a), len(b))
            same += sum(x == y for x, y in zip(a, b))
        p = bound(max(same, pairs - same), pairs, 16)
        lag_h[str(lag)] = -math.log2(p)
    estimates = {'bit_mcv': -math.log2(bit_p), 'byte_mcv_per_bit': -math.log2(byte_p) / 8,
                 'worst_256bit_position': -math.log2(pos_p),
                 'worst_context_1_to_8': min(context_h.values()),
                 'worst_lag_predictor': min(lag_h.values())}
    floor = min(estimates.values())
    repeated = sum(data[i:i+32] == data[i-32:i] for i in range(32, len(data), 32))
    health_ok = longest < RCT and apt_fail == 0 and not repeated
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'ones_fraction': ones / total, 'estimates_bits_per_bit': estimates,
            'context_bits_per_bit': context_h, 'lag_bits_per_bit': lag_h,
            'context_sampling': 'Disjoint 9-bit groups: 8 history bits and one outcome.',
            'lag_sampling': 'Disjoint pairs within blocks of twice the lag.',
            'minimum_estimate': floor, 'iid_confidence_failure_bound_per_family': ALPHA,
            'iid_confidence_failure_bound_all_five_families': 5 * ALPHA,
            'zlib_bytes_per_input_byte': len(zlib.compress(data, 9)) / len(data),
            'rct_cutoff': RCT, 'longest_identical_bit_run': longest,
            'apt_window_bits': WINDOW, 'apt_cutoff': APT,
            'apt_max_reference_count': apt_max, 'apt_fail_windows': apt_fail,
            'adjacent_equal_256bit_blocks': repeated,
            'raw_health_pass': health_ok, 'supports_half_bit_design_floor': health_ok and floor >= 0.5,
            'limitation': 'IID-model MCV bounds plus finite dependency diagnostics; no proof of IID, entropy rate or certification.'}


def restart(data):
    """1000 independent oscillator restarts, 1024 retained bits per row."""
    rows = [data[i:i+128] for i in range(0, len(data), 128)]
    if len(rows) != 1000 or any(len(row) != 128 for row in rows):
        raise ValueError('restart capture must be exactly 1000 rows of 128 bytes')
    row_bits = [b''.join(BIT_BYTES[x] for x in row) for row in rows]
    worst = max(max(sum(r[c] for r in row_bits), len(rows) - sum(r[c] for r in row_bits))
                for c in range(1024))
    unique = len(set(rows))
    return {'rows': len(rows), 'bits_per_row': 1024, 'unique_rows': unique,
            'worst_column_majority': worst, 'sha256': hashlib.sha256(data).hexdigest(),
            # Deliberately conservative rejection diagnostic; no entropy credit estimate.
            'pass': unique == len(rows) and worst < 750 and all(256 < sum(r) < 768 for r in row_bits),
            'limitation': 'Restart/stuck diagnostic, not the full SP800-90B restart procedure.'}


def read_sample(path, lo, hi):
    p = Path(path)
    if not p.is_file() or not lo <= p.stat().st_size <= hi:
        raise ValueError(f'{p}: regular sample must be {lo}..{hi} bytes')
    with p.open('rb') as f:
        data = f.read(hi + 1)
    if not lo <= len(data) <= hi:
        raise ValueError('sample size changed')
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sample', nargs='?')
    ap.add_argument('--restart', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        data = b''.join(hashlib.sha256(i.to_bytes(4, 'little')).digest() for i in range(32768))
        assert analyze(data)['supports_half_bit_design_floor']
        assert not analyze(bytes(1024 * 1024))['raw_health_pass']
        assert not analyze(bytes(range(256)) * 4096)['supports_half_bit_design_floor']
        rows = data[:128000]
        assert restart(rows)['pass']
        assert not restart(rows[:128] * 1000)['pass']
        print('PASS: raw MCV/predictor health and restart/stuck diagnostics (deterministic fixtures are not entropy)')
        return
    if not args.sample:
        ap.error('sample required')
    try:
        data = read_sample(args.sample, 128000 if args.restart else 1024*1024,
                           128000 if args.restart else 8*1024*1024)
        result = restart(data) if args.restart else analyze(data)
    except ValueError as err:
        ap.error(str(err))
    print(json.dumps(result, indent=2, sort_keys=True))
    raise SystemExit(0 if result['pass' if args.restart else 'supports_half_bit_design_floor'] else 1)


if __name__ == '__main__':
    main()
