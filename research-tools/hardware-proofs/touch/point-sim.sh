#!/bin/bash
# #587: the production RK3128 reference board kernel helper, against a stock-ARM oracle digest.
# Optional: TOUCH_ALGO_VENDOR_IMAGE=<archived Image> TOUCH_ALGO_PYTHON=<venv python>
# adds live Unicorn differential replay. Normal validation needs neither.
set -euo pipefail
cd "$(dirname "$0")/../../.."
T=$(mktemp -d "${TMPDIR:-/var/tmp}/touch-algo.XXXXXX")
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/drivers/input/touchscreen"
for spec in '579-0001-input-silead-vendor-calibration.patch:silead_d708.h' \
            '587-0001-input-silead-rk3128-reference-point-algorithm.patch:silead_rk3128_reference.h'; do
  awk -v name="${spec#*:}" '/^diff --git / {keep = ($NF == "b/drivers/input/touchscreen/" name)} keep' \
    "linux/patches/${spec%:*}" >"$T/helper.patch"
  patch -s -d "$T" -p1 --batch <"$T/helper.patch"
done
awk '/^\+static void silead_ts_read_rk3128_reference/ {keep=1} keep {sub(/^\+/, ""); print} keep && /^}/ {exit}' \
  linux/patches/587-0001-input-silead-rk3128-reference-point-algorithm.patch >"$T/rk3128_reference_driver.inc"
cc -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$T" -I"$T/drivers/input/touchscreen" -Iresearch-tools/hardware-proofs/touch \
  research-tools/hardware-proofs/touch/touch_algo_test.c research-tools/hardware-proofs/touch/touch_gesture.c -lm -o "$T/check"
ASAN_OPTIONS=detect_leaks=1 "$T/check"
python3 research-tools/hardware-proofs/touch/point-oracle.py --input "$T/input.bin"
ASAN_OPTIONS=detect_leaks=1 "$T/check" --histories <"$T/input.bin" >"$T/actual.bin"
# This digest comes from an independent ARM execution oracle and all
# 300 history words per report; it is not generated from the C implementation.
expected=50af5597750452baa37f12b29b0bd621df4935c9493c007569650969cec2ba1c
actual=$(sha256sum "$T/actual.bin" | cut -d' ' -f1)
[ "$actual" = "$expected" ] || { echo "FAIL RK3128 reference board ARM oracle: $actual != $expected" >&2; exit 1; }
echo 'RK3128 reference board point chain: reports and 300 history words per record bit-identical to stock ARM PASS'
if [ -n "${TOUCH_ALGO_VENDOR_IMAGE:-}" ]; then
  "${TOUCH_ALGO_PYTHON:-python3}" research-tools/hardware-proofs/touch/point-oracle.py --input "$T/input.bin" \
    --vendor "$TOUCH_ALGO_VENDOR_IMAGE" --runner "$T/check"
fi
