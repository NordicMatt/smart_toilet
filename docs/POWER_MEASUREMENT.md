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

## Powering the board, and where NOT to inject

### The EB II supply chain

Per Nordic's EB II docs, the radio has two feeds arriving over the
expansion headers:

- **VDD_5V** -> regulated down to **3.6 V on the EB II** -> nRF7002 VBAT net
  (EB II power header P2 pin 1 is `VDD_VBAT`, "power to regulator for 3.6 V
  VBAT supply")
- **VDD_IO** (1.8 V default from the DK) -> nRF7002 IOVDD, valid 1.62-3.6 V

So yes, the nRF7002 ultimately draws from 5V0. That does **not** make 5V0 a
good injection point.

### Do not drive 5 V into VBUS or a 5V0 net while USB is connected

VBUS on this DK feeds the **nPM1300 PMIC**, which generates every downstream
rail. Driving 5 V in while the USB host also drives it puts two sources in
parallel with the PMIC's USB-detection logic between them. Best case one
source wins and the measurement is meaningless because current arrives from
a path you are not measuring. Worst case current is pushed back into the
host port.

There is also **no 5 V input that reaches the SoC**. The DK's external
supply header is **P14**, rated **1.7 V to 3.6 V**. The P6-P10/P18 headers
*output* 5 V to shields; they are not inputs.

### Two problems even when the wiring is safe

- **Brownout.** PPK2 source meter tops out near 1 A, is itself USB-powered,
  and is boosting to reach 5 V. Wi-Fi TX draws fast high-current transients;
  if the rail cannot follow them the device browns out. This project has
  already lost time to exactly that failure mode (the Toilet #2 onboarding
  USB brownout). Add bulk capacitance if you go this route.
- **The debugger swamps the signal.** Powering the whole DK also measures
  the J-Link OB interface MCU, LEDs, PMIC losses and external flash. The
  interface MCU alone can draw tens of mA, far more than the difference
  between "RPU off" and "RPU idle" that you are trying to resolve.

### If you do want the regulator input included

Break the `VDD_5V` net going *into the EB II* and put the PPK2 in series
there. Do not backfeed a shared 5 V rail that the PMIC is also driving.

> The EB II spec lists a "footprint for header pins to measure power
> consumption", so **P10 and P4 may be unpopulated footprints**. Check
> whether headers need soldering before planning around them.

> Confidence note: the EB II topology above is documented explicitly. The
> DK's internal 5V0 net -- specifically what sits between VBUS and it -- was
> NOT verified against a schematic. If an isolation device is present the
> contention concern may not apply.

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
- DK LEDs draw current. LED1 used to be held on for the whole uptime, which
  added a constant offset of roughly a couple of mA to every trace. As of
  `CONFIG_APP_LED_HEARTBEAT` (on in `lowpower.conf`) it pulses 20 ms every
  5 s instead, so expect a small periodic blip rather than a DC offset. LED0
  still lights during the KWS window in `WW_GATED_KWS` mode, and a wake-word
  detection blinks LED1 for a full second, so avoid talking near the unit
  during a capture
- The 5-min period is bench-only. Field is 12 h (`app/lowpower.conf`)
- At a 300 s period the upload backstop collapses to ~15 min
  (`2 * period + CONNECT_WAIT_S`), so a wedged upload reboots quickly
  rather than silently polluting a capture
