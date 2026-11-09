#!/bin/bash
# #492: compile the driver's actual scan/drain functions against deterministic host interleavings.
# No RF or root access. Exercise stale RX capture, error/timeout drains, TX worker handoffs,
# PM ACK failure/late completion, and deferred home-channel/width changes on completion/cancel.
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/../../.."
T=$(mktemp -d "${TMPDIR:-/var/tmp}/ssv-scan-test.XXXXXX")
trap 'rm -rf "$T"' EXIT
python3 -I - "$T/driver.inc" <<'PY'
from pathlib import Path
import re, sys
root = Path('linux/modules/ssv6x5x/smac')
def function(file, name):
    s = (root / file).read_text()
    m = re.search(r'^(?:static )?(?:bool|void|int) ' + name + r'\s*\([^;]*?\)\n\{', s, re.M)
    assert m, name
    start = m.start(); pos = m.end(); depth = 1
    while depth:
        if s[pos] == '{': depth += 1
        if s[pos] == '}': depth -= 1
        pos += 1
    return s[start:pos] + '\n'
names = [('../hci/ssv_hci.c', '_do_rx'), ('../hci/ssv_hci.c', 'ssv6xxx_hci_rx_work')]
names += [('dev.c', n) for n in ('ssv_rx_flush_stale', 'ssv_tx_idle', 'ssv_tx_wait_idle')]
names += [('scan.c', n) for n in ('ssv_hwscan_defer_channel', 'ssv_scan_null_report', 'ssv_scan_null', 'ssv_scan_home_channel', 'ssv_scan_go_home', 'ssv_scan_work')]
Path(sys.argv[1]).write_text('\n'.join(function(*n) for n in names))
PY
cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O1 -fsanitize=undefined \
  -I"$T" research-tools/hardware-proofs/host/ssv-scan-test.c -o "$T/test"
"$T/test"
