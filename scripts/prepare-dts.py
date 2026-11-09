#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Emit the complete RK3128 reference board DT wrapper and fragments; do not modify/build a kernel."""
import argparse
from pathlib import Path
import shutil

DEFINES = {
    'RK3128_RAMOOPS_ADDR': '0x7f100000',
    'RK3128_RAMOOPS_SIZE': '0x00080000',
    'RK3128_FB_FORMAT': '"r5g6b5"',
    'RK3128_FB_BPP': '2',
    'RK3128_PWM_REGULATORS': '"okay"',
    'RK3128_PMIC_CONTROL': '"disabled"',
    'RK3128_ADC_KEYS': '"disabled"',
    'RK3128_IR': '"disabled"',
    'RK3128_PROBE_I2C2': '"disabled"',
    'RK3128_HALT_POWEROFF': '"okay"',
    'RK3128_DDR_MHZ': '396',
    'RK3128_USB': '"okay"',
    'RK3128_IEP': '"okay"',
    'RK3128_RGA': '"okay"',
    'RK3128_CRYPTO': '"okay"',
    'RK3128_CRYPTO_DIAG': '0',
    'RK3128_DISPLAY_LVDS': '1',
    'RK3128_SIMPLEFB_STATUS': '"disabled"',
    'RK3128_SDIO_DMA': '1',
    'RK3128_SDIO_MAX_HZ': '37500000',
    'RK3128_EMMC_DMA': '1',
    'RK3128_EMMC_HZ': '50000000',
    'RK3128_EMMC_DDR': '1',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True,
                        help='prepared Linux 7.3.1 source, read-only check')
    parser.add_argument('--output', type=Path, required=True,
                        help='separate output directory for generated board inputs')
    parser.add_argument('--define', action='append', default=[], metavar='RK3128_NAME=VALUE',
                        help='override an existing define with a single preprocessor value; quote strings')
    args = parser.parse_args()
    kernel = args.kernel.resolve()
    if not (kernel / 'arch/arm/boot/dts/rockchip/rk3128.dtsi').is_file():
        parser.error('kernel lacks the expected RK3128 source path')
    definitions = dict(DEFINES)
    for override in args.define:
        name, sep, value = override.partition('=')
        if not sep or name not in definitions or not value or '\n' in value or '\r' in value:
            parser.error('each --define must replace one known RK3128_* name with one nonempty line')
        definitions[name] = value
    output = args.output.resolve()
    if output == kernel or kernel in output.parents:
        parser.error('output must be separate from the kernel tree; inspect generated files before copying')
    reference = Path(__file__).resolve().parents[1]
    inputs = reference / 'linux/dts'
    output.mkdir(parents=True, exist_ok=True)
    board = 'rk3128-reference-board.dts'
    shutil.copyfile(inputs / 'rk3128-reference.dts', output / board)
    lines = ['/* RK3128 reference board reference profile: review rails, DDR and wiring for your board. */']
    lines.extend(f'#define {name} {value}' for name, value in definitions.items())
    lines.append(f'#include "{board}"')
    for fragment in sorted(inputs.glob('rk3128-*.dtsi')):
        shutil.copyfile(fragment, output / fragment.name)
        lines.append(f'#include "{fragment.name}"')
    (output / 'rk3128-reference.dts').write_text('\n'.join(lines) + '\n')
    print(f'Wrote {len(lines) - len(definitions) - 1} board/fragment includes to {output}')
    print('Kernel was not modified. Review profile and copy generated inputs only when ready.')


if __name__ == '__main__':
    main()
