#!/usr/bin/env python3
"""Historical #734 conditioned-output checks; these do not certify entropy.

For raw oscillator estimates and RCT/APT use crypto-rng-entropy.py. These FIPS
windows describe old captures and must not gate raw-source entropy credit.

Reports byte/bit balance, byte Shannon entropy, repeated 256-bit outputs, and
the historical FIPS 140-2 20,000-bit tests (monobit/poker/runs/long run).
The input must contain between 1 and 8 MiB of generated diagnostic samples.
"""
import argparse
import collections
import hashlib
import json
import math
from pathlib import Path

LOW = (2315, 1114, 527, 240, 103, 103)
HIGH = (2685, 1386, 723, 384, 209, 209)


def check(data):
    counts = collections.Counter(data)
    ones = sum(x.bit_count() for x in data)
    nbits = len(data) * 8
    z = (ones - nbits / 2) / math.sqrt(nbits / 4)
    entropy = -sum((n / len(data)) * math.log2(n / len(data)) for n in counts.values())
    blocks = len(data) // 32
    repeats = sum(data[i:i + 32] == data[i - 32:i] for i in range(32, blocks * 32, 32))
    constant = sum(len(set(data[i:i + 32])) == 1 for i in range(0, blocks * 32, 32))
    passed = windows = 0
    failures = collections.Counter()
    for off in range(0, len(data) - 2499, 2500):
        window = data[off:off + 2500]
        windows += 1
        bitcount = sum(x.bit_count() for x in window)
        nibs = [0] * 16
        runs = [[0] * 6 for _ in range(2)]
        last = -1
        length = longest = 0
        for x in window:
            nibs[x & 15] += 1
            nibs[x >> 4] += 1
            for shift in range(7, -1, -1):
                bit = (x >> shift) & 1
                if bit == last:
                    length += 1
                else:
                    if length:
                        runs[last][min(length, 6) - 1] += 1
                        longest = max(longest, length)
                    last, length = bit, 1
        runs[last][min(length, 6) - 1] += 1
        longest = max(longest, length)
        poker = 16 / 5000 * sum(x * x for x in nibs) - 5000
        tests = {
            "monobit": 9725 < bitcount < 10275,
            "poker": 2.16 < poker < 46.17,
            "runs": all(LOW[i] <= r[i] <= HIGH[i] for r in runs for i in range(6)),
            "long_run": longest < 26,
        }
        for test, ok in tests.items():
            if not ok:
                failures[test] += 1
        passed += all(tests.values())
    ok = abs(z) < 6 and entropy > 7.99 and not repeats and not constant and passed / windows >= 0.99
    return {
        "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
        "ones_fraction": ones / nbits, "monobit_z": z,
        "byte_shannon_bits": entropy, "max_byte_frequency": max(counts.values()) / len(data),
        "blocks_256": blocks, "adjacent_repeated_blocks": repeats, "constant_blocks": constant,
        "fips_windows": windows, "fips_pass": passed, "fips_failures": dict(failures),
        "fips_pass_fraction": passed / windows, "pass": ok,
        "limit": "Descriptive health only; statistical success does not establish physical entropy or certification.",
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("sample", nargs="?")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        good = b"".join(hashlib.sha256(i.to_bytes(4, "little")).digest() for i in range(32768))
        assert check(good)["pass"]
        bad = check(bytes(1024 * 1024))
        assert not bad["pass"] and bad["adjacent_repeated_blocks"] == 32767
        assert not check(bytes(range(256)) * 4096)["pass"]
        print("PASS: balanced deterministic reference accepted; zero and periodic failures rejected (health is not entropy)")
        return
    if not a.sample:
        ap.error("sample required")
    p = Path(a.sample)
    if not p.is_file() or not 1024 * 1024 <= p.stat().st_size <= 8 * 1024 * 1024:
        ap.error("sample must be a regular file of 1..8 MiB")
    with p.open("rb") as f:
        data = f.read(8 * 1024 * 1024 + 1)
    if not 1024 * 1024 <= len(data) <= 8 * 1024 * 1024:
        ap.error("sample size changed")
    result = check(data)
    print(json.dumps(result, indent=2, sort_keys=True))
    raise SystemExit(0 if result["pass"] else 1)


if __name__ == "__main__":
    main()
