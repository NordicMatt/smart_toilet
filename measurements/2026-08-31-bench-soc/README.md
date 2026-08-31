# 2026-08-31 bench SoC captures — inference cost split

Rig: second nRF54LM20 DK (J-Link **1051814190**) + nRF7002 EB II, PPK2 in
**ampere-meter mode at P14** (VDD:nRF, 1.8 V), no microphone attached, no
motor/Hall. This DK does not enumerate on USB until PPK2 DUT power is on.
Firmware: `low-power` branch bench builds (300 s DRAIN period). Capture:
`tools/ppk2_capture.py --seconds 380 --voltage-mv 1800`, P1.13 marker not
wired (threshold split in post).

| Capture | Build | Floor (windows excluded) |
| --- | --- | --- |
| `soc_run1` | `2.2.1-bench` (full pipeline) | **0.648 mA** |
| `soc_noinfer` | `2.2.1-bench-noinfer` (`-DAPP_BENCH_NO_INFERENCE`: capture + DSP, `ww_process()` skipped, watchdog fed) | **0.285 mA** |

**Inference = 0.363 mA, 56 % of the SoC floor.** That is the measured
ceiling on what an audio-level (VAD) gate could save in a quiet room.
Details and interpretation in `docs/POWER_MEASUREMENT.md`.

Files:

- `soc_*_1ms.csv` — 1 ms downsample (`t_s, mean_uA, max_uA`), plot-friendly.
- `raw/soc_*.csv.gz` — full ~85 kS/s captures (~600 MB uncompressed each,
  `index,current_uA,digital`). Kept out of git; do not commit.
