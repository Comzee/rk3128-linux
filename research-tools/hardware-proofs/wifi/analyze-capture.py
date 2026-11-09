#!/usr/bin/env python3
"""Decode and verify #600's saved evidence, without contacting the board."""
import json
import argparse
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
parser.add_argument("--expected-bssid", required=True)
parser.add_argument("--other-bssid", required=True)
args = parser.parse_args()
ROOT = args.directory
TABLE = [int(x, 16) for x in
         "3df5 31f6 35f8 21c1 0000 0000 0000 0bfe "
         "17fe 03ee 0000 0006 0001 0003 0005 0007".split()]
ENABLES = [int(x, 16) for x in
           "2008 1001 0400 0400 2000 800e 0800 0b88 0800".split()]


def sections(filename):
    text = (ROOT / filename).read_text()
    matches = list(re.finditer(
        r"^=== (\S+) (\S+) uptime=([0-9.]+) ===\n", text, re.M))
    result = {}
    for i, match in enumerate(matches):
        name, utc, uptime = match.groups()
        assert name not in result, name
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        result[name] = {"utc": utc, "uptime": float(uptime),
                        "text": text[match.end():end]}
    return text, result


def reg(section, address):
    match = re.search(
        rf"^READ {address} (\d+)\nADDRESS: 0x{address}\n"
        r"((?:[0-9a-f]{8}[ \n]+)+)", section["text"], re.M)
    if match is None:
        return None
    count, words = match.groups()
    values = [int(word, 16) for word in words.split()]
    assert len(values) == int(count), (address, len(values), count)
    return values


def counter(section, name):
    match = re.search(rf"^{name}=([0-9,]+)$", section["text"], re.M)
    assert match, name
    return [int(word) for word in match[1].split(",")]


def delta(before, after, name):
    a, b = counter(before, name), counter(after, name)
    assert len(a) == len(b)
    return [(y - x) & 0xffffffff for x, y in zip(a, b)]


def bsses(section, scan_ms):
    fresh = []
    for block in re.split(r"(?=^BSS )", section["text"], flags=re.M):
        bssid = re.match(r"BSS ([0-9a-f:]{17})", block)
        if bssid is None:
            continue
        age = re.search(r"last seen: (\d+) ms ago", block)
        freq = re.search(r"freq: ([0-9.]+)", block)
        assert age and freq
        if int(age[1]) < scan_ms:
            fresh.append({"bssid": bssid[1], "freq_mhz": float(freq[1]),
                          "last_seen_ms": int(age[1])})
    return fresh


raw, main = sections("capture.txt")
unassoc_raw, unassoc = sections("control.txt")
assoc_raw, assoc = sections("associated-control.txt")
assert "CAPTURE_PASS\n" in raw
assert "CONTROL_PASS\n" in unassoc_raw
assert "ASSOCIATED_CONTROL_PASS\n" in assoc_raw
assert "FINAL_CHECK_PASS\n" in (ROOT / "final-check.txt").read_text()

snapshots = 0
full_snapshots = 0
for group in (main, unassoc, assoc):
    for name, section in group.items():
        if reg(section, "c6000070") is None:
            continue
        snapshots += 1
        assert reg(section, "c6000070") == TABLE, name
        assert reg(section, "c60000b0") == ENABLES, name
        assert reg(section, "c60000e0") == [0x1cb4, 0x14, 0x14], name
        assert reg(section, "0000d898") == [0], name
        if reg(section, "ca00031c") is not None:
            full_snapshots += 1
            assert reg(section, "ca00031c") == [0xce5000, 0x7fbc7f87, 0x13f], name
            assert reg(section, "c6000134") == [0], name
            assert reg(section, "c60001a8") == [0, 0x808, 0x838], name
            for address in ("ca000000", "ca000050", "ca010000", "ca010050",
                            "ca0100a0", "ca0100f0", "ca010140", "ca010190"):
                assert len(reg(section, address)) == 20, (name, address)
                if address != "ca000000":
                    assert reg(section, address)[0] & 1 == 0, (name, address)
            assert reg(section, "ca000340") == [0, 0, 0, 0], name

assert reg(main["start"], "00000000") == [0xf046]
assert reg(main["unassociated-before"], "ca000328")[:2] == [0, 0]
assert reg(main["unassociated-before"], "ca000000")[0] & 1 == 0
assert reg(main["associated-before"], "ca000328")[:2] == [0x6b7a3238, 0x3d78]
assert reg(main["associated-before"], "ca000000")[0] & 1 == 1
assert reg(main["restored-5GHz-after"], "ca000328")[:2] == [0x6b7a3238, 0x3e78]

summary = {"snapshots": snapshots, "full_snapshots": full_snapshots,
           "tb": [f"{value:04x}" for value in TABLE],
           "en": [f"{value:04x}" for value in ENABLES],
           "flows": ["1cb4", "0014", "0014"], "d898": 0, "d899": 0,
           "scans": {}}
for phase in ("associated-5GHz", "associated", "unassociated", "reassociated", "restored-5GHz"):
    before, after = main[phase + "-before"], main[phase + "-after"]
    start, result = main[phase + "-scan-start"], main[phase + "-scan-result"]
    assert "SCAN rc=0" in result["text"], phase
    summary["scans"][phase] = {
        "driver_scan_ms": counter(after, "scan_stats")[4],
        "rx_chan_delta": delta(before, after, "rx_chan_stats"),
        "rx_drain_delta": delta(before, after, "rx_drain_stats"),
        "fresh_bsses": bsses(result, (result["uptime"] - start["uptime"]) * 1000)}

for phase, group, prefix in (("full-unassociated", unassoc, "control-unassociated"),
                              ("full-associated-2GHz", assoc, "associated-control")):
    before, after = group[prefix + "-before"], group[prefix + "-after"]
    scan_prefix = "control" if phase == "full-unassociated" else prefix
    start, result = group[scan_prefix + "-full-passive-start"], group[scan_prefix + "-full-passive-result"]
    summary["scans"][phase] = {
        "driver_scan_ms": counter(after, "scan_stats")[4],
        "driver_scan_delta": delta(before, after, "scan_stats")[:4],
        "rx_chan_delta": delta(before, after, "rx_chan_stats"),
        "rx_drain_delta": delta(before, after, "rx_drain_stats"),
        "fresh_bsses": bsses(result, (result["uptime"] - start["uptime"]) * 1000)}

positive = summary["scans"]["full-unassociated"]
negative = summary["scans"]["full-associated-2GHz"]
assert positive["rx_chan_delta"][3] == 5
assert negative["rx_chan_delta"][3] == 0
assert positive["rx_chan_delta"][7:] == negative["rx_chan_delta"][7:] == [0, 0]
assert any(b["bssid"] == args.expected_bssid for b in positive["fresh_bsses"])
assert not any(b["freq_mhz"] > 5000 for b in negative["fresh_bsses"])
assert any(b["freq_mhz"] == 2462 and b["bssid"] != args.other_bssid
           for b in negative["fresh_bsses"])
print(json.dumps(summary, indent=2))
