# PPK2 current measurement (low-power branch)

Measured 2026-07-27 on a third nRF54LM20 DK + nRF7002 EB II running
`build_bench` (`app/bench_power.conf`, 300 s DRAIN period). Tool:
`tools/ppk2_capture.py`.

## Results

| Subsystem | State | Current |
| --- | --- | --- |
| **nRF7002** (5 V feed) | radio OFF, between windows | **0.087 mA** |
| **nRF7002** (5 V feed) | radio ON, 34 s window | **43.07 mA** (peak 305.8) |
| **nRF54LM20B** (VDD:nRF @ 1.8 V) | always-on audio + inference | **0.664 mA** |
| **nRF54LM20B** | during a window | ~2.9 mA (peak 20.5) |

**The RPU does power down.** 87 µA on the radio's 5 V feed is regulator
quiescent plus leakage; an nRF7002 left energised sits in the tens of mA.
The ratio between window and sleep is **1:495**. This closes the doubt
`PLAN_low_power.md` step 1 raised, with a measurement instead of an
inference. The `net_if_down ... Error: -120` (`-EALREADY`) logged at window
close is benign.

Projected radio average at the shipped 12 h period:

```
window:  43.07 mA x    34 s =  1464 mA·s
sleep:   0.087 mA x 43200 s =  3758 mA·s
                      total =  5223 mA·s / 43234 s = 0.121 mA
```

Sleep dominates at 12 h, so shortening the window buys almost nothing.
Further gains have to come from lowering the 87 µA floor.

### Budget and what is NOT in it

| Subsystem | Average | Source |
| --- | --- | --- |
| SoC | ~0.68 mA | measured |
| Radio @ 12 h duty | ~0.12 mA | measured |
| **Measured subtotal** | **~0.80 mA** | |
| PDM mic (MP34DT01-M) | ~0.65 mA | datasheet, NOT measured |
| Hall sensor, ungated | ~5 mA | typical part, NOT measured |
| Motor | unknown | NOT measured |

On a 900 mAh cell: SoC + radio ≈ 48 days; add the mic ≈ 26 days; add an
ungated Hall ≈ 6 days.

**`VDD:IO` is a buffered follower of `VDD:nRF`**, deliberately arranged so
shield I/O current is not drawn through the measured rail. The P14 figure is
therefore the **SoC only** — mic, external flash and the nRF7002's IOVDD are
in neither capture.

The 43 mA window figure includes the EB II's 5 V -> 3.6 V regulator, which
the battery design deletes, so the product number will be better.

## Setup as actually used

Both measurements are **ampere meter mode, in series**. The board keeps
supplying itself; the PPK2 only counts electrons. No sourcing, so no
brownout risk from the meter.

### Radio: cut SB31

The EB II's P10/P4 measurement headers are unreachable once the board is
seated, so the radio is measured upstream instead.

`SB31` ("cut to disconnect VBUS from 5V0:CONN") isolates the connector 5 V
rail that feeds the power headers P6-P10/P18, and hence the EB II. Cut it
and bridge the gap with the PPK2 in series: `VIN` to the VBUS side, `VOUT`
to the `5V0:CONN` side.

> **After cutting SB31 the radio only has power while the PPK2 has DUT
> power ON.** Between captures Wi-Fi is dead and every DRAIN window fails to
> connect. That is the rig, not a firmware regression. To run the DK
> normally, bridge the gap with a plain wire.

Non-destructive alternative, if starting over: interrupt **pin 1 only** of
the DK power header the EB II's P2 seats on (bend the pin, or use a riser)
and put the PPK2 in series there. Same measurement, nothing cut. The two
pinouts mate 1:1 — DK header pin 1 is `5V0 from PMIC`, EB II P2 pin 1 is
`VDD_VBAT`.

### SoC: P14, ampere mode

Remove the P14 jumper and connect the PPK2 **between pins 1 and 2**, which
is Nordic's documented ampere-meter setup for this DK. The onboard nPM1300
keeps supplying `VDD:nRF` at its native 1.8 V, so there is no risk of
running the SoC at the wrong voltage.

Source-meter mode at P14 also works (`--source-mode`, 1.7-3.6 V allowed),
but then the PPK2 *is* the supply and the DK must already be powered over
USB before output is enabled, or the onboard circuitry can be damaged.
Ampere mode avoids the ordering hazard entirely.

> With the P14 jumper out and the PPK2 in series, **the SoC only has power
> while DUT power is ON**, so the DK reboots when a capture starts.

## Where NOT to inject

`VBUS` feeds the **nPM1300 PMIC**, which generates every downstream rail.
Driving 5 V into `VBUS` or a `5V0` net while USB is also connected puts two
sources in parallel and current arrives via a path you are not measuring.

There is **no 5 V input that reaches the SoC**: the external supply header
is P14, rated **1.7-3.6 V**. The P6-P10/P18 headers are 5 V *outputs*.

Do not cut `SB18` for this. It separates `VSYS` from `P5V0`, and `VSYS` is
the PMIC's main system rail; it does not isolate the shield supply.

## Running a capture

```
tools/ppk2_capture.py --seconds 380                      # radio, 5 V rail
tools/ppk2_capture.py --seconds 380 --voltage-mv 1800    # SoC at P14
```

Always pass a `/dev/serial/by-id/...` path. **ttyACM numbers get reassigned**
when devices re-enumerate, and the PPK2 and the DK swap places.

380 s covers a full bench cycle (~34 s window + 300 s sleep).

### If the PPK2 returns garbage

Symptoms: `UnicodeDecodeError` reading modifiers, ~7 samples/s instead of
~80 000, absurd currents. The device was left mid-stream by a previous
session. `ppk2_capture.py` now quiesces automatically on connect; by hand it
is opcode `0x07` (AVERAGE_STOP) then drain. **`0x0d` is `REGULATOR_SET`, not
average-stop** — sending it by mistake misconfigures the device. `0x20` is a
full RESET, after which the device re-enumerates and the ttyACM number
changes.

### Other gotchas

- Close the serial terminal before taking real numbers; Nordic warn readings
  are noisy while the virtual serial port is open
- The window marker (`CONFIG_APP_POWER_MARKER`, P1.13) was **not wired** for
  these captures, so the split was done on current threshold. With 495x
  separation that is unambiguous, but wiring P1.13 to a PPK2 digital input
  gives exact edges
- LED1 pulses 20 ms every 5 s (`CONFIG_APP_LED_HEARTBEAT`), so expect a small
  periodic blip rather than a DC offset. A wake-word detection blinks it for
  a full second, so avoid talking near the unit mid-capture
- nRF Connect for Desktop polls serial ports for device discovery, which can
  disturb a scripted capture. Close it first

## SoC optimisation attempts (2026-07-27)

All measured at P14, ampere mode, 1.8 V, same 180-380 s method.

| Build | baseline | vs bench |
| --- | --- | --- |
| bench (serial + log + console) | 0.650 mA | — |
| `bench_quiet.conf` (SERIAL/CONSOLE off) | **0.622 mA** | **-28 uA** |
| `bench_pm.conf` (PM_DEVICE + RUNTIME) | 0.649 mA | ~0 |

**`PM_DEVICE_RUNTIME` saves nothing — do not adopt.** Functionally safe
(audio ran, DHCP landed, window completed, watchdog quiet), but the
peripherals that dominate are the ones actively in use. See
`app/bench_pm.conf`.

**`CONFIG_PM` is unavailable.** It depends on `HAS_PM`, which nRF54LM20 does
not advertise in NCS 3.4.0. Not a loss: the idle path is WFI, which is how
nRF54L reaches System ON IDLE (~3 uA).

**Serial-off was NOT promoted to the field build.** 28 uA is ~4% of the SoC
baseline and ~2% of the system budget, and the console has repeatedly been
needed for debugging. See `app/bench_quiet.conf`.

### DC/DC is already on — and your custom board must replicate it

The DK's board DTSI sets `&vregmain { status = "okay";
regulator-initial-mode = <NRF5X_REG_MODE_DCDC>; }`, confirmed as
`regulator-initial-mode = <0x1>` in the generated devicetree. Omitting this
is what left a DevZone custom board at **98 uA instead of ~1 uA** in System
OFF. It is the single highest-consequence line in a custom board's DTS.

### Where the SoC's power actually goes

Nordic quote System ON IDLE at ~3 uA; we measure ~620 uA. So **~99.5% of the
SoC draw is the work itself** — continuous PDM capture plus inference. No
configuration switch touches that. Reducing it means changing the algorithm
(e.g. a cheap voice-activity first stage gating the expensive model), not
the build config. For scale, the mic alone is ~650 uA per its datasheet,
comparable to the whole SoC, and sits outside this measurement on the
VDD:IO follower.

The two changes actually worth making are elsewhere: **gate the Hall
sensor** (~5 mA continuous) and **feed nRF7002 VBAT directly** (deletes the
EB II regulator and most of the 87 uA radio floor).

## Battery architecture (next phase)

The nPM1300 has two 200 mA step-down BUCKs, two 50 mA LDO / 100 mA load
switches, and `VSYS` (unregulated cell voltage). **It has no boost**, so on a
3.0-4.2 V cell there is no 5 V rail without an external converter.

Resolved by not needing 5 V anywhere:

| Load | Source | Rationale |
| --- | --- | --- |
| Motor | battery / `VSYS` direct | runs from cell voltage; via existing low-side MOSFET |
| nRF7002 VBAT | battery direct | VBAT range is 2.9-4.5 V, cell fits; deletes the EB II regulator |
| SoC, mic, flash, nRF7002 IOVDD | BUCK1 @ 1.8 V | ~30 mA worst case, inside 200 mA |
| Hall sensor | **LDO2 as load switch** | gate around `actuator_flush()` |

**The radio must not come off a BUCK.** Measured 306 mA peak at 5 V is
~360-430 mA at cell voltage, well past a 200 mA regulator.

**Gating the Hall sensor is the single biggest win.** At ~5 mA continuous it
exceeds every other load combined, and it only needs to be alive for the
seconds a flush takes.

Risks to design around:

- `VSYS` caps at 1 A from battery. Radio peak plus motor inrush can exceed
  it. Wiring the motor straight to the cell sidesteps the limit, at the cost
  of the fuel gauge not seeing that current
- A small LiPo's protection PCB can trip at 2-3 A. Check it against measured
  **inrush**, not running current
- Set the low-voltage cutoff above **2.9 V** for the nRF7002's VBAT floor,
  which is higher than the cell alone would need
- Motor inrush and radio TX share a rail today. Keep them on separate paths
  from `VSYS`, with bulk capacitance local to the motor

## Still unmeasured

1. **Motor inrush and stall current** — sizes the cell, the protection
   circuit, and whether `VSYS` is viable. The largest remaining unknown
2. **Hall sensor actual draw** — needs the part number
3. **Mic and external flash** — outside the P14 rail via the VDD:IO follower
4. Verify the Lipo900 battery model completed; it was `csvReady: false` as of
   2026-06-26 (see `PROFILING.md`)
