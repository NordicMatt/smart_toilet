# Smart Toilet — voice-triggered flush

> ✨ **Say "Abracadabra," and the toilet flushes.** No buttons, no app —
> just your voice and a little magic.

## The story

This started when the electronic capacitive-sensing toilets I have stopped working
correctly, and I felt the need to fix it. I rigged them up to flush from a button
press, monitored by an Arduino, and added a Hall effect sensor to read the motor
shaft position so it always stops at home.

I'd wanted to make it voice-activated for *years*. I tried with an nRF5340 and Edge
Impulse and could never get it working, so I put it away and lived with the button.

Then Nordic released the new **Edge AI Lab** and **Edge AI add-on**, and everything
changed. I built this ML model in about an hour. You can too — it's genuinely that
easy. And with the **Nordic MCP**, you can do nearly all of it without writing much
code. Give it a try!

I designed the enclosure using **Claude** and **Codex** together, iterating a few
times until everything was the way I liked it. It's the 3D-printed "magic chest" the
hardware lives in below.

Everything here is open source. The one exception is Nordic's AI, which is licensed
to run only on Nordic microcontrollers.

---

**Smart Toilet** is a voice-activated flush actuator for the **nRF54LM20 DK**, built
on the nRF Edge AI add-on. Saying the magic word **"Abracadabra"** drives a flush
motor through one rotation and stops it at the home position using a Hall sensor.

Wake-word detection runs fully on-device on the Axon AI accelerator — **no audio
ever leaves the board.** With the **nRF7002 EB II** Wi-Fi shield the device also
connects to **Memfault** over HTTPS for crash/metric monitoring, reports its flush
count as a Memfault metric, and receives firmware updates over the air via
**Memfault Release Management**. The application runs in **wake-word-only mode**.

- Board target: `nrf54lm20dk/nrf54lm20b/cpuapp` (secure, no TF-M)
- Wi-Fi shield: `nrf7002eb2` (on the P17 expansion connector)
- Application mode: `APP_MODE_WW_ONLY`
- Wake word: "Abracadabra" (`APP_WW_MODEL_ABRACADABRA`)

![Assembled enclosure](docs/images/enclosure-assembled.jpeg)

The hardware lives in a 3D-printed "magic chest" enclosure (sources in
[`enclosure/`](enclosure/)) whose lid reads *Say the Magic Word*.

| Lid | Internals |
|-----|-----------|
| ![Lid lettering](docs/images/lid-say-the-magic-word.jpeg) | ![DK wired into the enclosure](docs/images/internals-wiring.jpeg) |

## How it works

1. The PDM microphone (`pdm20`) captures single-channel 16 kHz audio (`src/dmic.c`).
2. Each audio block passes through the front-end cleanup chain (`src/audio_proc.c`):
   a fixed PDM hardware gain, a high-pass filter, then software AGC — see
   [Audio front-end](#audio-front-end) below.
3. The wake-word model (an nRF Edge AI Lab model, selected by the
   `APP_WW_MODEL` Kconfig choice) runs on the Axon accelerator (`src/ww/`). A
   detection requires the per-frame probability to exceed
   `CONFIG_WW_PROBABILITY_THRESHOLD` (0.60) for 7 of the last 20 ~30 ms frames,
   plus a ~1 s refractory period so one utterance fires exactly once.
4. On detection, `actuator_flush()` runs the motor and stops it when the Hall
   sensor sees the shaft magnet return home (`src/actuator.c`). LED0 blinks for
   one second and `Wakeword detected` is printed on the control UART (VCOM0).
5. In parallel, `src/cloud.c` brings up Wi-Fi and connects to **Memfault over
   HTTPS** to upload diagnostics, publish the flush count, and check for firmware
   updates. Audio capture only starts **after** the network is up, so the TLS
   bring-up never competes with the wake-word inference for CPU and DMIC buffers.

**Why "Abracadabra":** wake words with more syllables and plosive consonants
survive far-field room reverb far better. The original 2-syllable "Shazaam"
managed only ~50% detection at the deployment distance; "Abracadabra"
(5 syllables, plosive-rich with alternating open vowels) reaches ~95–100%.

## Audio front-end

Each 10 ms audio block is cleaned up before it reaches the model. The chain is,
in order:

1. **PDM hardware gain** — `CONFIG_APP_PDM_GAIN_DB` (default **+20 dB**) applied
   in the PDM peripheral via `nrf_pdm_gain_set()` in `src/dmic.c` (the Zephyr
   DMIC API does not expose gain). Sized for far-field use; without it, speech at
   the deployment distance peaks around −25 dBFS.
2. **High-pass filter** — `CONFIG_APP_AUDIO_HPF`, a 2nd-order Butterworth
   high-pass at **120 Hz**. There is little speech energy below the cutoff, so it
   strips low-frequency rumble and handling noise at negligible cost to the model
   input.
3. **Automatic gain control (AGC)** — `CONFIG_APP_AGC` tracks the speech peak
   envelope and applies a smoothed software gain (±12 dB) that pulls speech
   toward `CONFIG_APP_AGC_TARGET_DBFS` (**−20 dBFS**), so detection is far less
   sensitive to how far the speaker is from the mic.

The AGC acts *after* the PDM hardware gain, so it cannot undo saturation that
already happened in the peripheral — keep `APP_PDM_GAIN_DB` low enough that close
or loud speech does not clip (the model returns ~0 probability on saturated
audio). See [Tuning the mic and wake word](#tuning-the-mic-and-wake-word).

## Hardware / pinout

The EB II mounts on the **P17** expansion connector and claims several pins, so the
toilet peripherals live on header **P2** (GPIO port P1):

| Signal      | Pin   | Notes |
|-------------|-------|-------|
| Motor drive | P1.06 | Active-high → logic-level MOSFET gate |
| Hall sensor | P1.12 | Active-low, internal pull-up; **power the sensor from 5 V** (DK headers P6–P10/P18 pin 1) |
| PDM mic CLK | P1.14 | Adafruit 3492 (ST MP34DT01-M) |
| PDM mic DAT | P1.15 | mic SEL→GND (left channel), VDD→IO rail |
| Wi-Fi       | P17   | nRF7002 EB II — don't wire anything else here |

> **Keep all peripheral wires off the `P1` connector block** — that block carries
> GPIO port P0, including the console UART (`P0.06`/`P0.07`). A stray wire there
> makes the console go silent for any firmware.
>
> **Do not use P2.00–P2.05** as header GPIO: the board controller mux routes them
> to the onboard QSPI flash, so the header pins are dead by default.

### Wiring

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

### Actuator tuning

In `src/actuator.c`:

- `HALL_BLANKING_MS` (250) ignores the Hall sensor right after start — the magnet
  rests on the sensor at home, so without blanking, start-up jitter would end the
  flush immediately.
- `MOTOR_SAFETY_MS` (1000) forces the motor off if the magnet is never sensed
  (jam, misaligned magnet, faulty sensor) so it cannot burn out. One rotation is
  ≈ 700 ms.

## Wi-Fi

Connectivity comes from the **nRF7002 EB II** on P17. `src/cloud.c` uses `conn_mgr`
to bring the interface up and connect, then obtains time over NTP (needed for TLS
certificate validity checks) before connecting to Memfault. The app connects with
Wi-Fi credentials stored in NVS (Zephyr
`wifi_credentials`); provision them with the `wifi cred add -s "<SSID>" -k <keymgmt>
-p "<PASSWORD>"` shell command (on a build with `CONFIG_SHELL`) or compiled-in
static `CONFIG_WIFI_CREDENTIALS_STATIC_SSID`/`_PASSWORD`. Credentials persist across
reboots and a `west flash` without `--erase`.

## Memfault (diagnostics + FOTA)

The device talks only to **Memfault**, directly over **HTTPS** — there is no
nRF Cloud connection. `CONFIG_MEMFAULT_HTTP_ENABLE=y` authenticates with the
Memfault project key (passed at build time — see [Build & flash](#build--flash));
the device serial is the hardware ID. Memfault's TLS root certificates are
provisioned at boot (`CONFIG_MEMFAULT_NCS_PROVISION_CERTIFICATES`), so there are
no application-managed certificates.

What `src/cloud.c` does:

- **Diagnostics** — `CONFIG_MEMFAULT=y` captures coredumps (RAM-backed, surviving
  the warm reset that `CONFIG_RESET_ON_FATAL_ERROR=y` performs after a fault),
  reboot reasons, and metrics, plus a GNU build ID for symbolication. The cloud
  thread drains the Memfault packetizer and POSTs chunks to Memfault over HTTPS.
- **Flush count** — the lifetime count is persisted in NVS (`toilet/flush_count`)
  and published as the Memfault metric **`flush_count`** in each heartbeat, so it
  survives reboots and shows up in the device timeline. (There is no nRF Cloud
  shadow channel, so the old remote-flush command was dropped.)
- **FOTA** — firmware updates are delivered by **Memfault Release Management**.
  `memfault_zephyr_fota_start()` checks the device's cohort for a newer release
  and downloads the signed image over HTTPS into the external-flash MCUboot
  secondary slot; MCUboot swaps it on reboot and the app confirms it once it is
  back online (so it survives a power-cycle). See
  [docs/fota-test.md](docs/fota-test.md) for the full push-an-update flow.

One-time cloud setup: upload the ELF (`build/app/zephyr/zephyr.elf`) under
**Software → Symbol Files** so coredumps symbolicate (matched by the GNU build ID
logged at boot, `mflt: GNU Build ID: …`).

## Tuning the mic and wake word

With `CONFIG_APP_AUDIO_STATS=y` (on by default in `prj.conf`), the log — on **VCOM0**
with the shield (it disables VCOM1) — prints one line of each per second:

```
audio: peak -4.2 dBFS, rms -21.7 dBFS, clipped 0/16000
ww: peak prob 0.62 (bar 0.60), peak votes 4/7
```

Say the wake word from the normal use position and read the lines for that second:

- **`clipped` > 0 or peak pinned at 0.0 dBFS** → the mic is distorting; lower
  `CONFIG_APP_PDM_GAIN_DB` (0.5 dB hardware steps, range −20..+20).
- **Speech peaks below about −20 dBFS** → too quiet for the model; raise
  `CONFIG_APP_PDM_GAIN_DB`.
- **`peak prob` never crosses the bar** even with healthy levels → lower
  `CONFIG_WW_PROBABILITY_THRESHOLD` (in 1/1000).
- **`peak prob` crosses but `peak votes` stays short of the target** → the hits
  are too spread out; lower `CONFIG_WW_COUNT_THRESHOLD` or raise
  `CONFIG_WW_HISTORY_SIZE`.

Set `CONFIG_APP_AUDIO_STATS=n` for the deployed build. (The `CONFIG_APP_AUDIO_SNAP`
audio-recording feature is **disabled in the Wi-Fi build** — its multi-second
capture buffer does not fit alongside the Wi-Fi/cloud/Memfault stacks, and its raw
UART dump conflicts with the shield's VCOM0 console.)

## Build & flash

Built against an NCS **v3.3.1** tree with the nRF Edge AI add-on passed as extra
Zephyr modules, the Wi-Fi shield, the cloud config overlay, and three secrets kept
out of the repo entirely (`~/.memfault_project_key`, `~/.wifi_ssid`,
`~/.wifi_password` — none of these are committed; cloud.conf only enables
`CONFIG_WIFI_CREDENTIALS_STATIC`, it never contains the actual SSID/password):

```sh
KEY=$(tr -d '\n' < ~/.memfault_project_key)
SSID=$(tr -d '\n' < ~/.wifi_ssid)
WIFI_PW=$(tr -d '\n' < ~/.wifi_password)
nrfutil sdk-manager toolchain launch --ncs-version v3.3.1 --chdir ~/ncs/v3.3.1 -- \
  west build -p always -b nrf54lm20dk/nrf54lm20b/cpuapp -d build app -- \
    -DSHIELD=nrf7002eb2 \
    -DEXTRA_CONF_FILE=cloud.conf \
    -DZEPHYR_EXTRA_MODULES=<addon>/edge-ai \
    -DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"$KEY\" \
    -DCONFIG_WIFI_CREDENTIALS_STATIC_SSID=\"$SSID\" \
    -DCONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD=\"$WIFI_PW\"
```

Flash and reset (pass `--dev-id <JLINK_SN>` when more than one DK is attached; add
`--erase` when the partition layout changed, e.g. switching from a TF-M /ns image):

```sh
nrfutil sdk-manager toolchain launch --ncs-version v3.3.1 -- \
  west flash -d build --dev-id <JLINK_SN>
```

Footprint: FLASH ~45 %, RAM ~85 % of the nRF54LM20B.

### Output

With the EB II shield the application console (logs **and** any shell) is on **VCOM0**
(UART30); VCOM1 is disabled by the shield. Use the lower-numbered `ttyACM`/COM port.
`tools/uart_monitor.py` is a small pyserial reader/monitor. Representative lines:

```
cloud: Network ready; uploading to Memfault over HTTPS
main: Network connected; starting audio capture
Waiting for wakeword
Wakeword detected
cloud: Flush #1 recorded
```

## Notes / lessons learned

- **Keep mic/motor/Hall wires on the `P2` connector block, not `P1`.** The console
  UART (`P0.06`/`P0.07`) is on the `P1` block; a stray wire there makes the console
  go silent for any firmware.
- **P2.00–P2.05 are not usable as header GPIO** — the board controller routes them
  to the onboard QSPI flash; disabling the flash in devicetree does not reconnect
  the header pin.
- **Power the Hall sensor from 5 V.** On the 3 V rail it never asserted. The output
  is **active-low** — verify polarity with a meter.
- The flush stop uses a **blanking window**, not a leave-then-return state machine,
  because the magnet rests on the sensor at home.
- **Pick a long, plosive-rich wake word.** Short fricative-led words lose the
  high-frequency energy room reverb destroys. See the `APP_WW_MODEL` choice.
- **Single, bounded heap:** the app uses newlib (required by Edge AI) while the Wi-Fi
  supplicant force-selects `COMMON_LIBC_MALLOC`; both default to a heap at `_end` and
  collide. `CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE` is fixed so the common heap is a
  static buffer and newlib's `_sbrk` heap gets the region above `_end`.
- **The onboard J-Link can wedge** (VCOM silent / `west flash` J-Link DLL error /
  `Unknown part number 0x33`). Retry `west flash` 1–3×; replug USB if it stays stuck.
- With two DKs attached, always pass `--dev-id <JLINK_SN>` to `west flash`.

## License

This project is licensed under the Nordic 5-Clause License
(`LicenseRef-Nordic-5-Clause`) — see [LICENSE](LICENSE). It is intended for use
with Nordic Semiconductor integrated circuits.
