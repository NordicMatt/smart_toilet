# Smart Toilet — voice-triggered flush

Voice-activated flush actuator for the **nRF54LM20 DK**, built on the nRF Edge AI
add-on. Saying the wake word **"Shazaam"** runs a flush motor through one
rotation and stops it at the home position via a Hall sensor.

Board target: `nrf54lm20dk/nrf54lm20b/cpuapp`. App mode: `APP_MODE_WW_ONLY`.

## How it works

1. PDM mic (`pdm20`) captures 16 kHz audio (`src/dmic.c`).
2. The wake-word model (Edge AI solution 93499) runs on the Axon accelerator
   (`src/ww/`). A detection requires the per-frame probability to exceed
   `CONFIG_WW_PROBABILITY_THRESHOLD` (0.80) for 10 of the last 20 frames, plus a
   ~1 s refractory period so one utterance fires once.
3. On detection, `actuator_flush()` runs the motor; it stops when the Hall
   sensor sees the shaft magnet return home (`src/actuator.c`).

## Hardware / pinout

| Signal        | Pin    | Notes |
|---------------|--------|-------|
| Motor drive   | P1.06  | Active-high → logic-level MOSFET gate |
| Hall sensor   | P1.07  | Active-low, internal pull-up; **power the sensor from 5 V** (DK headers P6–P10/P18 pin 1) |
| PDM mic CLK   | P1.04  | Adafruit 3492 (ST MP34DT01-M) |
| PDM mic DAT   | P1.05  | mic SEL→GND (left), VDD→1.8 V IO |

> **Do not use P2.00–P2.05** as header GPIO on this DK: the board controller
> mux routes them to the onboard QSPI flash, so the header pins are dead by
> default. P1/P3 pins are plain GPIO and work directly.

Mic sensitivity is boosted with **+12 dB PDM gain** applied via
`nrf_pdm_gain_set()` in `src/dmic.c` (the Zephyr DMIC API doesn't expose gain).

Actuator tuning (in `src/actuator.c`): `HALL_BLANKING_MS=250` ignores the Hall
right after start (the magnet rests on the sensor) so start-up jitter can't end
the flush early; `MOTOR_SAFETY_MS=1000` is a backstop; one rotation ≈ 700 ms.

## Wiring

```
 nRF54LM20 DK                         External circuit
 ------------                         ----------------
 P1.06  ───────────────► gate         logic-level MOSFET
 GND    ───────────────► source       drain ─► motor(-) , motor(+) ─► motor supply +
                                       (flyback diode across the motor)

 5V0 (P6..P18 pin 1) ──► Hall VCC      Hall switch, magnet on the motor shaft
 GND ─────────────────► Hall GND
 P1.07 ◄──────────────── Hall OUT      idles ~3.6 V, pulls to 0 V when magnet present

 P1.04 ──► mic CLK   P1.05 ◄── mic DAT   Adafruit 3492 (SEL→GND, VDD→1.8 V IO)
```

Keep all grounds common (DK GND, motor-supply GND, Hall GND). The Hall idles at
~3.6 V, which is at the nRF GPIO input maximum — add a resistor divider if you
see flaky reads.

## Tuning the mic and wake word

With `CONFIG_APP_AUDIO_STATS=y` (on by default in `prj.conf`), the log UART
(VCOM1) prints one line of each per second:

```
audio: peak -4.2 dBFS, rms -21.7 dBFS, clipped 0/16000
ww: peak prob 0.62 (bar 0.80), peak votes 4/10
```

Say "Shazaam" from the normal use position and read the lines for that second:

- **`clipped` > 0 or peak pinned at 0.0 dBFS** → the mic is distorting; lower
  `CONFIG_APP_PDM_GAIN_DB` (0.5 dB hardware steps, range -20..+20).
- **Speech peaks below about -20 dBFS** → too quiet for the model; raise
  `CONFIG_APP_PDM_GAIN_DB`.
- **`peak prob` never crosses the bar** even with healthy levels → lower
  `CONFIG_WW_PROBABILITY_THRESHOLD` (in 1/1000).
- **`peak prob` crosses but `peak votes` stays short of the target** → the hits
  are too spread out; lower `CONFIG_WW_COUNT_THRESHOLD` or raise
  `CONFIG_WW_HISTORY_SIZE`.

Aim for speech peaks around -6 to -3 dBFS with zero clipped samples, then tune
the thresholds. Set `CONFIG_APP_AUDIO_STATS=n` for the deployed build.

## Notes / lessons learned

- **P2.00–P2.05 are not usable as header GPIO** on this DK — the board
  controller routes them to the onboard QSPI flash. Disabling the flash in the
  devicetree does *not* reconnect the header pin; the SoC pin never reaches it.
  Use P1/P3 GPIO instead.
- **Power the Hall sensor from 5 V.** On the 3 V rail it was below its minimum
  supply and never asserted. The output is **active-low** (0 V present, 3.6 V
  absent) — verify polarity with a meter rather than assuming.
- The flush stop uses a **blanking window**, not a leave-then-return state
  machine: at rest the magnet sits on the sensor, and jitter as it leaves
  otherwise fires a false "rotation complete" within tens of milliseconds.
- With two DKs attached, always pass `--dev-id <JLINK_SN>` to `west flash`.

## License

This project is licensed under the Nordic 5-Clause License
(`LicenseRef-Nordic-5-Clause`) — see [LICENSE](LICENSE). It is intended for use
with Nordic Semiconductor integrated circuits.

## Build & flash

Built against the Edge AI add-on west workspace (bundles its own nrf/zephyr).

Alternative without a dedicated add-on workspace: use a plain NCS install
(v3.3.1 works) and pass the add-on repo as an extra Zephyr module. Fetch the
add-on once with `west init -m https://github.com/nrfconnect/sdk-edge-ai
--manifest-rev v2.1.0 <dir>` (no `west update` needed), then:

```sh
nrfutil sdk-manager toolchain launch --ncs-version v3.3.1 \
  --chdir ~/ncs/v3.3.1 -- west build -b nrf54lm20dk/nrf54lm20b/cpuapp \
  -d /path/to/this/repo/build /path/to/this/app -- \
  -DEXTRA_ZEPHYR_MODULES=<dir>/edge-ai
```

```sh
# Build
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 \
  --chdir /path/to/edge_ai_addon -- west build -d build /path/to/this/app

# Flash (use --dev-id when more than one DK is attached)
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 \
  --chdir /path/to/edge_ai_addon -- west flash -d build --dev-id <JLINK_SN>

# Fast reflash of an already-built image
... -- west flash -d build --dev-id <JLINK_SN> --no-rebuild
```

Output: VCOM0 = control messages (`Wakeword detected`), VCOM1 = Zephyr log
(actuator events). `tools/uart_monitor.py` is a small pyserial reader/monitor.
