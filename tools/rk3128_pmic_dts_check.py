#!/usr/bin/env python3
"""Check the power-control invariants of the board's built DTB (development record #311,
docs/RK3128-LINUX.md "PMIC (RT5036)"). Needs `dtc`; reads build/linux/out/rk3128-reference.dtb
unless given a path.

  rk3128_pmic_dts_check.py [dtb] [--expect disabled|okay] [--expect-halt okay|disabled]

Checks, and exits 1 on the first failure:
  - no enabled node is a PMIC (rt5036, rt5025, rk8xx, act88xx, ricoh619): the board has none, and
    a PMIC driver that probes would write to whatever answers on its address;
  - every regulator node that is enabled is one this port knows (the PWM rails, the fixed
    supplies), so a new regulator cannot slip in unseen;
  - the gpio-poweroff node is GPIO3_C7 (the gpio controller at 0x20088000, pin 23) active low,
    delays 100/100/3000 ms (the stock driver's), and its status is --expect (default disabled,
    okay for RK3128_PMIC_CONTROL=okay);
  - the halt-poweroff node (#390, the power-off that works on mains) resets the watchdog through
    the CRU (SRST_WDT, 86), reads the power key GPIO3_C6 (pin 22) active low, and its status is
    --expect-halt (default okay, disabled for RK3128_HALT_POWEROFF=disabled).
"""
import argparse
import re
import subprocess
import sys

DEFAULT = "build/linux/out/rk3128-reference.dtb"
PMIC = re.compile(r'"(rt,rt50|richtek,rt50|rockchip,rk8|active-semi,act8|ricoh,rn5t|ricoh,rc5t)')


def fail(msg):
    print("rk3128_pmic_dts_check: FAIL: " + msg)
    sys.exit(1)


def nodes(text):
    """Yield (path, body) for every node; body excludes child nodes."""
    stack, bodies = [], []
    for line in text.splitlines():
        s = line.strip()
        if s.endswith("{"):
            name = s[:-1].strip().split(":")[-1].strip()
            stack.append(name)
            bodies.append([])
        elif s == "};":
            yield "/".join(stack).replace("//", "/"), "\n".join(bodies.pop())
            stack.pop()
        elif bodies:
            bodies[-1].append(s)


def prop(body, name):
    m = re.search(r"^" + re.escape(name) + r"(?: = (.*?))?;$", body, re.M)
    if not m:
        return None
    return m.group(1) if m.group(1) is not None else True


def cells(v):
    return [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", v)]


def enabled(body):
    st = prop(body, "status")
    return st is None or st in ('"okay"', '"ok"')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dtb", nargs="?", default=DEFAULT)
    ap.add_argument("--expect", choices=("disabled", "okay"), default="disabled")
    ap.add_argument("--expect-halt", choices=("disabled", "okay"), default="okay")
    a = ap.parse_args()
    text = subprocess.run(["dtc", "-I", "dtb", "-O", "dts", a.dtb], capture_output=True, text=True).stdout
    if not text:
        fail("dtc produced nothing for " + a.dtb)
    all_nodes = list(nodes(text))
    phandles = {}
    for path, body in all_nodes:
        ph = prop(body, "phandle")
        if ph and ph is not True:
            phandles[cells(ph)[0]] = (path, body)

    for path, body in all_nodes:
        comp = prop(body, "compatible")
        if comp and comp is not True and PMIC.search(comp) and enabled(body):
            fail("enabled PMIC node %s (%s): the board has no PMIC" % (path, comp))
        if comp and comp is not True and "regulator" in comp and enabled(body):
            if comp not in ('"pwm-regulator"', '"regulator-fixed"'):
                fail("unknown enabled regulator %s (%s)" % (path, comp))

    po = [(p, b) for p, b in all_nodes if prop(b, "compatible") == '"gpio-poweroff"']
    if len(po) != 1:
        fail("expected one gpio-poweroff node, found %d" % len(po))
    path, body = po[0]
    st = prop(body, "status")
    if st != '"%s"' % a.expect:
        fail("%s status %s, expected \"%s\"" % (path, st, a.expect))
    g = cells(prop(body, "gpios") or "")
    if len(g) != 3:
        fail("%s gpios is not <phandle pin flags>" % path)
    ctl = phandles.get(g[0])
    if not ctl or not ctl[0].split("/")[-1].startswith("gpio@20088000"):
        fail("%s gpios controller is %s, not gpio3 (gpio@20088000)" % (path, ctl[0] if ctl else "?"))
    if g[1] != 23 or g[2] != 1:
        fail("%s gpios pin %d flags %d, expected 23 (RK_PC7) and 1 (GPIO_ACTIVE_LOW)" % (path, g[1], g[2]))
    for name, want in (("active-delay-ms", 100), ("inactive-delay-ms", 100), ("timeout-ms", 3000)):
        v = prop(body, name)
        if not v or v is True or cells(v) != [want]:
            fail("%s %s = %s, expected %d" % (path, name, v, want))
    hp = [(p, b) for p, b in all_nodes if prop(b, "compatible") == '"rockchip,rk3128-halt-poweroff"']
    if len(hp) != 1:
        fail("expected one halt-poweroff node, found %d" % len(hp))
    path, body = hp[0]
    st = prop(body, "status")
    if st != '"%s"' % a.expect_halt:
        fail("%s status %s, expected \"%s\"" % (path, st, a.expect_halt))
    r = cells(prop(body, "resets") or "")
    cru = phandles.get(r[0]) if r else None
    if len(r) != 2 or not cru or not cru[0].split("/")[-1].startswith("clock-controller@20000000") or r[1] != 86:
        fail("%s resets is not <&cru SRST_WDT> (86): %s" % (path, prop(body, "resets")))
    if prop(body, "reset-names") != '"watchdog"':
        fail("%s reset-names is not \"watchdog\"" % path)
    k = cells(prop(body, "power-key-gpios") or "")
    ctl = phandles.get(k[0]) if k else None
    if len(k) != 3 or not ctl or not ctl[0].split("/")[-1].startswith("gpio@20088000") or k[1:] != [22, 1]:
        fail("%s power-key-gpios is not GPIO3_C6 (gpio@20088000 pin 22) active low" % path)
    print("rk3128_pmic_dts_check: OK (%s): no PMIC node enabled, gpio-poweroff GPIO3_C7 active low %s, "
          "halt-poweroff (SRST_WDT, power key GPIO3_C6) %s" % (a.dtb, a.expect, a.expect_halt))


if __name__ == "__main__":
    main()
