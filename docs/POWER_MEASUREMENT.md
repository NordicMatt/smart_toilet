# PPK2 current measurement runbook (low-power branch)

Bench procedure for measuring the duty-cycled connectivity introduced in
v2.1.0 and tuned in v2.2.1. Build: `build_bench` (see `app/bench_power.conf`).

## The question this answers

`run_drain_window()` ends with `conn_mgr_all_if_disconnect()` +
`conn_mgr_all_if_down()`. That takes the interface *administratively* down,
but it has never been confirmed that it powers down the **nRF7002 RPU**.
`PLAN_low_power.md` step 1 flagged the doubt and it is still open:

> confirm the driver's power path when the interface is administratively
> down; check whether iface down alone gates the RPU or if we need
> `pm_device` on the nrf70 node

Window close also logs `net_if_down failed for iface 1. Error: -120`
(`-EALREADY`), which is probably benign but proves nothing either way.

**If the between-window current sits meaningfully above the audio-only
baseline, the radio is not really off and Phase 1 has not delivered its
headline claim.**

## Measure the right chip

This is the easiest way to get a meaningless answer.

| What | Where | Measures |
| --- | --- | --- |
| nRF54LM20B SoC | DK header **P14** (VDD nRF CURRENT MEASURE) | MCU only, **not the radio** |
| nRF7002 VBAT | EB II header **P10** | **the Wi-Fi radio** |
| nRF7002 IOVDD | EB II header **P4** | radio I/O rail |

The nRF7002 is on the EB II expansion board and is **not** powered through
the DK's P14. Measuring P14 alone tells you nothing about whether the radio
powered down. **Start with P10 on the EB II.**

With two PPK2s you can take both at once; with one, do P10 first.

## Setup

### nRF7002 EB II, VBAT (the important one)

PPK2 in **ampere meter mode**, in series across the P10 pins. Ground to
P9 or P10 GND. The EB II stays powered by the DK.

### nRF54LM20 DK SoC (optional, second pass)

PPK2 in **source meter mode**:

1. Remove the jumper on **P14** (VDD nRF CURRENT MEASURE)
2. PPK2 `VOUT` to the **middle** pin (marked with a down arrow)
3. PPK2 `VIN` to the **leftmost** pin (marked with an up arrow)
4. PPK2 `GND` to `GND`
5. Power Profiler: Source Meter, **3000 mV**, **100 000 samples/s**, power output ON

Keep the DK USB cable connected. Per Nordic, the DK is not designed to run
without it and you will see leakage current otherwise.

### Window marker (do this, it makes the trace readable)

The bench build drives **P1.13** HIGH for the entire radio-on region:
raised before `conn_mgr_all_if_up()`, dropped only after
`conn_mgr_all_if_down()` returns.

Wire **P1.13** (header P2) to a PPK2 **digital input D0**, with a common
ground. The logic channel then brackets exactly the radio-on period, so
"did it power down" is a direct read next to the current trace rather than
an inference from timestamps.

> Verify P1.13 on the DK pinout before wiring. It is free in firmware
> (P1.12 is the Hall sensor, P1.14/P1.15 are the PDM mic, and the EB II
> uses P1.04/05/07/10/11), and its neighbours are broken out on P2, but the
> header mapping has not been physically confirmed.

## Flash

Wired only. Never upload this image to Memfault or deploy it to a cohort.

```
nrfutil device program --firmware build_bench/merged.hex \
  --options chip_erase_mode=ERASE_ALL \
  --jlink-dll ~/.local/jlink/opt/SEGGER/JLink_V950/libjlinkarm.so --traits jlink
nrfutil device reset \
  --jlink-dll ~/.local/jlink/opt/SEGGER/JLink_V950/libjlinkarm.so --traits jlink
```

The bench image reports itself as **`2.2.1-bench`**, deliberately different
from the field `2.2.1+0`, so the unit can still be recovered over the air
afterwards (Memfault only offers an OTA when the reported version differs
from the cohort's active release) and its heartbeats do not pollute real
fleet metrics.

## Running it

Cycle time is ~5.5 min: ~20-30 s window, then 300 s radio off.

1. Flash, reset, confirm on the console that `DRAIN window: radio on` /
   `DRAIN window done` line up with the marker edges
2. **Close the serial terminal.** Nordic warns readings are noisy while the
   virtual serial port is open
3. Capture several full cycles

## Reading the result

Three regimes:

- **Marker HIGH, ~20-30 s** — associating, TLS, upload. Highest draw
- **Marker LOW, 300 s** — the audio pipeline (PDM mic + inference) runs
  continuously by design. This is the floor
- **The tell** — on the P10 (nRF7002) trace, current immediately right of
  the marker's falling edge should collapse to leakage. A persistent
  plateau means the RPU is still energised and `if_down` is not gating it

If it does not power down, the fix direction is `pm_device` on the nrf70
node, per the PLAN step above.

## Caveats

- Serial terminal open makes readings noisy: close it for real numbers
- For the cleanest SoC-side figure rebuild with `CONFIG_SERIAL=n`,
  `CONFIG_CONSOLE=n`, `CONFIG_LOG=n`, `CONFIG_PRINTK=n`
  (`PLAN_low_power.md` step 50). Matters much less for the P10 measurement
- DK LEDs draw current. LED1 is on whenever audio capture is running
- The 5-min period is bench-only. Field is 12 h (`app/lowpower.conf`)
- At a 300 s period the upload backstop collapses to ~15 min
  (`2 * period + CONNECT_WAIT_S`), so a wedged upload reboots quickly
  rather than silently polluting a capture
