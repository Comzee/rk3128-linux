# RK3128 reference board touch calibration and contact processing

The RK3128 reference board has a Silead GSLX680-family controller on I2C0 at address0x40. The included driver patches implement its calibration, contact tracking, prediction, temporal filtering and report ordering. Firmware remains a separate input: this repository contains no touch firmware or original device image.

## Source and firmware inputs

Patch579 adds the calibration mapping and board-compatible path. Patch587 adds the RK3128 reference board host point algorithm, selected only for `rockchip,rk3128-reference-gslx680`. The underlying mainline driver still owns firmware loading, I2C, IRQ and input-device registration. Generic Silead boards retain their existing path.

The DT's firmware name is `silead/gslx680-d708.fw`. `tools/gsl_fw_extract.py` converts the matching firmware table from a caller-supplied image into the loader format. `tools/calibration-oracle.py` and `tools/touch-algorithm-test.py` provide optional independent comparisons and a deterministic input corpus. External-image comparisons require the matching input and optional Unicorn package; generating the normal corpus does not. The tools grant no redistribution rights for external inputs or extracted firmware.

## Coordinate and report contract

The calibrated endpoint ranges are X0..800 and Y0..1280, represented by DT size801×1281. These are touch coordinates, not1920×1080 display pixels. Preserve the order of calibration, clipping, inversion/swap and downstream screen mapping. A second inversion changes the result.

The point processor maintains separate raw/predicted, stretched and filtered/reported histories. It uses ten internal IDs, emits at most five contacts and prioritizes IDs already reported. Firmware high-nibble IDs are not trusted. Linux assigns public MT tracking IDs normally; internal numeric IDs are not a userspace ABI.

| Stage | RK3128 reference board behavior |
| --- | --- |
| Header flags | Reset/single-contact/prediction-suppression flags affect state; preserve the full four-byte header |
| Coordinate correction / first-edge / diagonal | Disabled in the selected configuration; do not add an inferred panel warp |
| Duplicate merge | Strict raw-axis differences below32, temporarily below96 after a contact-count decrease or merge |
| Prediction and matching | Separate actual/fill histories, bounded nearest-distance matching and one missing-report prediction |
| Temporal stretch | Shake activation squared distance576; distance/weight entries(128,128),(8,9),(1,9) |
| Recursive filter | Mode3 combines current and previous stretched coordinates with the previous filtered result |
| New-contact delay |0/1/2/3 preceding reports for the first four contact counts, capped at3 thereafter |
| Driver output smoothing | Separate calibrated-coordinate history and coupled3/1-count deadbands |

The integer rounding order is part of behavior, including truncation toward zero for signed divisions. Replacing recursive filtering with a raw-sample moving average or symmetrically rounding every delta changes output.

## State ownership and validation

The algorithm's1,456-byte state belongs to one controller instance. Startup/resume clears it. Library reset flags and the later per-ID report history have separate lifetimes. Fixed arrays and bounded loops avoid unbounded packet/history access; short reads and invalid contact counts clear state and emit release rather than reading incomplete data. Allocation failure fails probe.

The deterministic comparison corpus contains **21,763 records**, spanning resets, contact-count changes through ten inputs, header flags, axis extremes, motion/reversals, duplicates, ordering, crossings, intermittent loss and reacquisition. Comparisons include all output contacts and300 history words per report. The input corpus SHA256 is `d22a99f85fda376460f9b149ecd8919e94160a38311fa87e418b89efbfc80165`; matching reference output/history SHA256 is `50af5597750452baa37f12b29b0bd621df4935c9493c007569650969cec2ba1c`.

These finite algorithm checks establish the tested processing contract. They do not replace physical touch-accuracy/gesture testing on a panel, nor establish equivalent behavior for a different controller firmware or board configuration.
