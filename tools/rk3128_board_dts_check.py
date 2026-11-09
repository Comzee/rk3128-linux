#!/usr/bin/env python3
"""Check the safety invariants of the board's built DTB (development record #302, docs/RK3128-LINUX.md
"Board basics"). Needs `dtc`; reads build/linux/out/rk3128-reference.dtb unless given a path.

  rk3128_board_dts_check.py [dtb] [--expect disabled|okay] [--adc-keys disabled|okay] [--ir disabled|okay]

Checks, and exits 1 on the first failure:
  - the PWM regulators' voltage-table is the vendor formula duty% = (1450000 - V) // 5500 for every
    entry (a higher duty is a LOWER voltage on this board), all duties are 0..100 (the kernel's
    table mode scales by 100 and ignores pwm-dutycycle-unit), voltages 950..1450 mV in 25 mV steps;
  - every enabled CPU OPP voltage is a table entry inside the vdd_arm constraints, and the voltage
    never decreases with frequency;
  - the regulators, pwm1/pwm2 and the OPP steps above 816 MHz follow the RK3128_PWM_REGULATORS
    status (--expect: disabled by default, okay for RK3128_PWM_REGULATORS=okay), with only the #638 kernel-gated vendor1200MHz/1.425V point above1008MHz;
  - cpu0 has cpu-supply = vdd_arm, the GPU has no mali-supply (vdd_logic must stay put);
  - adc-keys is disabled unless --adc-keys okay (RK3128_ADC_KEYS, #432: SARADC channel 2 rests in the vol-up window);
  - the IR receiver (gpio-ir-receiver) is disabled unless --ir okay (RK3128_IR, #466: the board has no remote);
  - gpio-keys holds the power key only (the "sensor" key on GPIO3_C5 is the accelerometer's control line, not a button, #466).
"""
import argparse
import re
import subprocess
import sys

DEFAULT = "build/linux/out/rk3128-reference.dtb"


def fail(msg):
    print("rk3128_board_dts_check: FAIL: " + msg)
    sys.exit(1)


def node(text, header):
    """Return the body of the first node whose header line matches (brace-balanced)."""
    m = re.search(r"^(\t*)" + header + r"\s*\{$", text, re.M)
    if not m:
        fail("node %s not found" % header)
    ind = m.group(1)
    end = re.search(r"^" + ind + r"\};$", text[m.end():], re.M)
    return text[m.end():m.end() + end.start()]


def prop(body, name):
    m = re.search(r"^\t*" + re.escape(name) + r"(?: = (.*?))?;$", body, re.M | re.S)
    if not m:
        return None
    return m.group(1) if m.group(1) is not None else True


def cells(v):
    return [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", v)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dtb", nargs="?", default=DEFAULT)
    ap.add_argument("--expect", choices=("disabled", "okay"), default="disabled")
    ap.add_argument("--adc-keys", choices=("disabled", "okay"), default="disabled",
                    help="RK3128_ADC_KEYS (#432): disabled by default, SARADC channel 2 rests in the vol-up window")
    ap.add_argument("--ir", choices=("disabled", "okay"), default="disabled",
                    help="RK3128_IR (#466): disabled by default, the board has no remote")
    a = ap.parse_args()
    text = subprocess.run(["dtc", "-I", "dtb", "-O", "dts", a.dtb], capture_output=True, text=True).stdout
    if not text:
        fail("dtc produced nothing for " + a.dtb)
    want = '"%s"' % a.expect

    tables = {}
    for name in ("regulator-vdd-arm", "regulator-vdd-logic"):
        body = node(text, name)
        t = cells(prop(body, "voltage-table"))
        pairs = list(zip(t[0::2], t[1::2]))
        if [v for v, _ in pairs] != list(range(950000, 1450001, 25000)):
            fail("%s: voltage map is not 950..1450 mV in 25 mV steps" % name)
        for v, d in pairs:
            if not 0 <= d <= 100:
                fail("%s: duty %d for %d uV is outside 0..100 (percent)" % (name, d, v))
            if d != (1450000 - v) // 5500:
                fail("%s: %d uV has duty %d, the vendor formula gives %d" % (name, v, d, (1450000 - v) // 5500))
        if prop(body, "pwm-dutycycle-unit") is not None:
            fail("%s still has pwm-dutycycle-unit (ignored in table mode)" % name)
        if prop(body, "status") != want:
            fail("%s status is %s, expected %s" % (name, prop(body, "status"), want))
        tables[name] = (pairs, cells(prop(body, "regulator-min-microvolt"))[0],
                        cells(prop(body, "regulator-max-microvolt"))[0])
        print("ok  %s: 21 steps, duty = (1450000 - V) // 5500, status %s, %d..%d uV"
              % (name, a.expect, tables[name][1], tables[name][2]))

    for pwm in ("pwm@20050010", "pwm@20050020"):
        if prop(node(text, pwm), "status") != want:
            fail("%s status is not %s" % (pwm, a.expect))
    print("ok  pwm1/pwm2 status %s" % a.expect)

    volts = {v for v, _ in tables["regulator-vdd-arm"][0]}
    lo, hi = tables["regulator-vdd-arm"][1:]
    opp = node(text, "opp-table-0")
    steps = []
    for m in re.finditer(r"^\t\topp-(\d+) \{$", opp, re.M):
        body = opp[m.end():opp.index("\n\t\t};", m.end())]
        hz = int(m.group(1))
        encoded_hz = cells(prop(body, "opp-hz") or "")
        if len(encoded_hz) != 2 or (encoded_hz[0] << 32 | encoded_hz[1]) != hz:
            fail("CPU OPP node name and opp-hz disagree")
        mv = cells(prop(body, "opp-microvolt"))
        st = prop(body, "status")
        steps.append((hz, mv, st))
    if not steps:
        fail("no CPU OPP steps")
    cpu0 = node(text, r"cpu@f00")
    caps = prop(cpu0, "rockchip,busy-core-caps-khz")
    guarded = caps is not None and cells(caps) == [1200000, 1008000, 912000, 912000]
    steps.sort()
    last = 0
    for hz, mv, st in steps:
        if len(mv) != 3 or len(set(mv[:2])) != 1 or mv[0] not in volts or not lo <= mv[0] <= hi or mv[2] > hi:
            fail("OPP %d Hz: voltage %s is not a vdd_arm table entry inside %d..%d" % (hz, mv, lo, hi))
        if mv[0] < last:
            fail("OPP %d Hz: voltage drops below a slower step" % hz)
        last = mv[0]
        needs = hz > 816000000
        if hz > 1008000000:
            if hz != 1200000000:
                fail("only the vendor 1200 MHz point is allowed above 1008 MHz")
            if mv != [1425000, 1425000, 1425000]:
                fail("1200 MHz requires exactly the vendor 1.425 V")
            if st != '"disabled"' and (st != want or not guarded):
                fail("enabled1200MHz requires the #638 kernel busy-core caps1200/1008/912/912")
        elif needs and st != want:
            fail("OPP %d Hz must have status %s (needs more than u-boot's voltage)" % (hz, a.expect))
        if not needs and st not in (None, '"okay"'):
            fail("OPP %d Hz is disabled" % hz)
    if guarded:
        lower = next((s for s in steps if s[0] == 912000000), None)
        if lower != (912000000, [1350000, 1350000, 1425000], want):
            fail("the busy-core policy requires912MHz at the vendor ceiling lookup of1.35V")
        print("ok  kernel busy-core caps1200/1008/912/912; defaultOFF1008MHz")
    if 696000000 in [s[0] for s in steps]:
        fail("696 MHz step is back (the fork's hang)")
    print("ok  CPU OPPs: " + ", ".join("%d MHz %.3f V%s" % (hz // 10**6, mv[0] / 1e6,
          "" if st is None or st == '"okay"' else " [" + st.strip('"') + "]") for hz, mv, st in steps))

    cpu0 = node(text, r"cpu@f00")
    arm = cells(prop(node(text, "regulator-vdd-arm"), "phandle"))[0]
    if cells(prop(cpu0, "cpu-supply"))[0] != arm:
        fail("cpu0 cpu-supply is not vdd_arm")
    if prop(node(text, r"gpu@10090000"), "mali-supply") is not None:
        fail("the GPU has a mali-supply (it would move vdd_logic)")
    print("ok  cpu0 cpu-supply = vdd_arm, GPU without mali-supply")

    ak = node(text, "adc-keys")
    if prop(ak, "status") != '"%s"' % a.adc_keys:
        fail("adc-keys status is %s, expected %s (RK3128_ADC_KEYS, #432: the line rests in the vol-up window)"
             % (prop(ak, "status"), a.adc_keys))
    print("ok  adc-keys status %s" % a.adc_keys)

    ir = node(text, "ir-receiver")
    if prop(ir, "status") != '"%s"' % a.ir:
        fail("ir-receiver status is %s, expected %s (RK3128_IR, #466: the board has no remote)"
             % (prop(ir, "status"), a.ir))
    print("ok  ir-receiver status %s" % a.ir)

    gk = node(text, "gpio-keys")
    codes = [cells(c)[0] for c in re.findall(r"linux,code = (<[^>]*>);", gk)]
    if codes != [116]:
        fail("gpio-keys codes are %s, expected only KEY_POWER (116): the board's one button is the power button (#466)" % codes)
    print("ok  gpio-keys: the power key only")


if __name__ == "__main__":
    main()
