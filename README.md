# Smart Toilet — voice-triggered flush + cloud telemetry

Voice-activated flush actuator for the **nRF54LM20 DK**, built on the nRF Edge AI
add-on. Saying the wake word **"abracadabra"** runs a flush motor through one
rotation and stops it at the home position via a Hall sensor.

With the **nRF7002 EB II** Wi-Fi shield it also connects to **nRF Cloud** over
CoAP to report flush events, keep a flush-count shadow, and accept a remote
flush, and runs **Memfault** for on-device crash/metric capture forwarded
through nRF Cloud.

Board target: `nrf54lm20dk/nrf54lm20b/cpuapp` (secure, no TF-M). Shield:
`nrf7002eb2`. App mode: `APP_MODE_WW_ONLY`.

## How it works

1. PDM mic (`pdm20`) captures 16 kHz audio (`src/dmic.c`).
2. The wake-word model (Edge AI solution 93800, "abracadabra") runs on the Axon
   accelerator (`src/ww/`). A detection requires the per-frame probability to
   exceed `CONFIG_WW_PROBABILITY_THRESHOLD` (0.80) for 10 of the last 20 frames,
   plus a ~1 s refractory period so one utterance fires once.
3. On detection, `actuator_flush()` runs the motor; it stops when the Hall
   sensor sees the shaft magnet return home (`src/actuator.c`).
4. In parallel, `src/cloud.c` brings up Wi-Fi, connects to nRF Cloud, reports
   flushes, and forwards Memfault data. Audio capture only starts **after** the
   cloud connection is up, so the DTLS/JWT bring-up never competes with the
   wake-word inference.

## Hardware / pinout

The EB II mounts on the **P17** expansion connector and claims several pins, so
the toilet peripherals live on header **P2** (GPIO port P1):

| Signal        | Pin    | Notes |
|---------------|--------|-------|
| Motor drive   | P1.06  | Active-high → logic-level MOSFET gate |
| Hall sensor   | P1.12  | Active-low, internal pull-up; **power the sensor from 5 V** (DK headers P6–P10/P18 pin 1) |
| PDM mic CLK   | P1.14  | Adafruit 3492 (ST MP34DT01-M) |
| PDM mic DAT   | P1.15  | mic SEL→GND (left), VDD→IO rail |
| Wi-Fi (SPI/EN/IRQ) | P17 | nRF7002 EB II — do not wire anything else here |

> **Keep all peripheral wires off the `P1` connector block** (that block carries
> GPIO port P0, including the console UART on `P0.06`/`P0.07`). A stray wire
> there kills the console. The mic/motor/Hall belong on the `P2` block.
>
> **Do not use P2.00–P2.05** as header GPIO: the board controller mux routes
> them to the onboard QSPI flash, so the header pins are dead by default.

Mic sensitivity is boosted with **+12 dB PDM gain** applied via
`nrf_pdm_gain_set()` in `src/dmic.c` (the Zephyr DMIC API doesn't expose gain).

Actuator tuning (`src/actuator.c`): `HALL_BLANKING_MS=250` ignores the Hall
right after start (the magnet rests on the sensor) so start-up jitter can't end
the flush early; `MOTOR_SAFETY_MS=1000` is a backstop; one rotation ≈ 700 ms.

## Wi-Fi

Connectivity comes from the **nRF7002 EB II** on P17. `src/cloud.c` uses
`conn_mgr` to bring the interface up and connect, then obtains time over NTP
before connecting to the cloud.

The app connects with Wi-Fi credentials stored in NVS (Zephyr
`wifi_credentials`). Provision them once with either:

- the `wifi cred add -s "<SSID>" -k <keymgmt> -p "<PASSWORD>"` shell command (on
  a build that enables `CONFIG_SHELL`), or
- compiled-in static credentials
  (`CONFIG_WIFI_CREDENTIALS_STATIC_SSID` / `_PASSWORD`).

Credentials persist across reboots and a `west flash` without `--erase`.

## nRF Cloud (CoAP)

Transport is **CoAP**; the device authenticates with a per-device JWT (APP_JWT,
signed with the device key). The CA + device cert/key are compiled in
(`CONFIG_NRF_CLOUD_PROVISION_CERTIFICATES=y`, C-string headers in `app/certs/`,
sec tag 16842753). The nRF Cloud device ID is the hardware ID
(`CONFIG_NRF_CLOUD_CLIENT_ID_SRC_HW_ID`), e.g. `51AF63E40BF36313`.

Onboard the device to your nRF Cloud account once with
[`nrfcloud-utils`](https://pypi.org/project/nrfcloud-utils/)
(`create_ca_cert` → `device_credentials_installer` → `nrf_cloud_onboard`).

What it reports / accepts (`src/cloud.c`):

- **Flush event** — each completed flush is sent as a `FLUSH` device message
  (`nrf_cloud_coap_sensor_send`), value = the running flush count.
- **Flush count shadow** — the lifetime count is pushed to the device shadow
  (`{"flushCount":N}`) and persisted in NVS (settings key `toilet/flush_count`),
  so it is restored and re-reported after a reboot.
- **Remote flush** — the device polls the shadow delta every 15 s; setting the
  desired state `{"flush": true}` in the portal triggers `actuator_flush()`.

## Memfault

`CONFIG_MEMFAULT=y` captures coredumps (RAM-backed, survives the warm reset that
`CONFIG_RESET_ON_FATAL_ERROR=y` performs after a fault), reboot reasons, and
metrics, plus a GNU build ID for symbolication. The Memfault device serial is
the same hardware ID as the nRF Cloud device.

Memfault data is **forwarded through nRF Cloud** — `src/cloud.c` drains the
Memfault packetizer and POSTs chunks to nRF Cloud's `chunks` CoAP resource over
the existing connection (the built-in `MEMFAULT_USE_NRF_CLOUD_COAP` path assumes
a cellular modem, so we do it ourselves). No device-side Memfault project key is
needed.

One-time cloud setup to view the data:

1. Link your nRF Cloud account to a Memfault project ("nRF Cloud powered by
   Memfault").
2. Upload the ELF symbol file (`build/app/zephyr/zephyr.elf`) to Memfault under
   **Software → Symbol Files** so coredumps/traces symbolicate; Memfault matches
   it by the GNU build ID logged at boot (`mflt: GNU Build ID: …`).

## Logging / console output

With the EB II shield the application console (logs **and** any shell) is on
**VCOM0** (UART30); VCOM1 is disabled by the shield. Use the lower-numbered
`ttyACM`/COM port. `tools/uart_monitor.py` is a small pyserial reader/monitor.

Representative log lines:

```
dmic: PDM gain set to +12 dB (reg 0x40)
main: Waiting for nRF Cloud connection before starting audio...
nrf_cloud_coap_transport: Authorized
cloud: Connected to nRF Cloud
main: nRF Cloud connected; starting audio capture
Waiting for wakeword
Wakeword detected
actuator: Flush: motor on (magnet clear at start)
cloud: Reported flush #1 to nRF Cloud
cloud: Memfault chunk uploaded (2.01)
mflt: Reset Reason, RESETREAS=0x1 / Pin Reset
```

## Wiring

```
 nRF54LM20 DK                         External circuit
 ------------                         ----------------
 P1.06  ───────────────► gate         logic-level MOSFET
 GND    ───────────────► source       drain ─► motor(-) , motor(+) ─► motor supply +
                                       (flyback diode across the motor)

 5V0 (P6..P18 pin 1) ──► Hall VCC      Hall switch, magnet on the motor shaft
 GND ─────────────────► Hall GND
 P1.12 ◄──────────────── Hall OUT      idles ~3.6 V, pulls to 0 V when magnet present

 P1.14 ──► mic CLK   P1.15 ◄── mic DAT   Adafruit 3492 (SEL→GND, VDD→IO rail)

 P17 expansion connector ◄───────────► nRF7002 EB II (Wi-Fi)
```

Keep all grounds common (DK GND, motor-supply GND, Hall GND). The Hall idles at
~3.6 V, which is at the nRF GPIO input maximum — add a resistor divider if you
see flaky reads.

## Notes / lessons learned

- **Keep mic/motor/Hall wires on the `P2` connector block, not `P1`.** The
  console UART (`P0.06`/`P0.07`) is on the `P1` block; a stray wire there makes
  the console go silent for any firmware.
- **P2.00–P2.05 are not usable as header GPIO** — the board controller routes
  them to the onboard QSPI flash; disabling the flash in devicetree does not
  reconnect the header pin.
- **Power the Hall sensor from 5 V.** On the 3 V rail it never asserted. The
  output is **active-low** — verify polarity with a meter.
- The flush stop uses a **blanking window**, not a leave-then-return state
  machine, because the magnet rests on the sensor at home.
- **Single heap, bounded:** the app uses newlib (required by Edge AI) while the
  Wi-Fi supplicant force-selects `COMMON_LIBC_MALLOC`. Both default to a heap at
  `_end`, which collide; `CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE` is set to a fixed
  size so the common heap is a static buffer and newlib's `_sbrk` heap gets the
  region above `_end`.
- **The onboard J-Link can wedge** (VCOM goes silent / `west flash` reports a
  J-Link DLL error / `Unknown part number 0x33`). Retry `west flash` 1–3×; a USB
  replug clears a stuck probe. A successful flash proves the console is alive.
- With two DKs attached, always pass `--dev-id <JLINK_SN>` to `west flash`.

## Build & flash

Built against the Edge AI add-on modules layered onto an NCS **v3.3.1** tree
(secure target, Wi-Fi shield).

```sh
# Build
nrfutil sdk-manager toolchain launch --ncs-version v3.3.1 --chdir /path/to/ncs/v3.3.1 -- \
  west build -p always -b nrf54lm20dk/nrf54lm20b/cpuapp -d build /path/to/this/app -- \
    -DSHIELD=nrf7002eb2 \
    -DEXTRA_CONF_FILE=/path/to/this/app/cloud.conf \
    -DZEPHYR_EXTRA_MODULES="/path/to/edge_ai_addon/edge-ai;/path/to/edge_ai_addon/modules/edge-impulse-sdk-zephyr"

# Flash (use --dev-id when more than one DK is attached; add --erase when the
# partition layout changed, e.g. switching from a TF-M /ns image)
nrfutil sdk-manager toolchain launch --ncs-version v3.3.1 --chdir /path/to/ncs/v3.3.1 -- \
  west flash -d build --dev-id <JLINK_SN>
```

Footprint: FLASH ~45 %, RAM ~76 % of the nRF54LM20B.

## License

This project is licensed under the Nordic 5-Clause License
(`LicenseRef-Nordic-5-Clause`) — see [LICENSE](LICENSE). It is intended for use
with Nordic Semiconductor integrated circuits.
