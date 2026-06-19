# FOTA test: pushing a new application image

End-to-end test of nRF Cloud CoAP FOTA on the cloud unit (`51AF63E40BF36313`,
"Toilet #1"). Validates: a new image is downloaded **over the existing CoAP
link**, written to the MCUboot **secondary slot in external MX25R64 flash**,
then swapped into the internal primary slot by MCUboot on reboot.

This requires the spike branch `spike/mcuboot-ext-flash-fota` (MCUboot + FOTA
client). The stock `main` build has no bootloader and cannot do FOTA.

## How a swap is verified

`main.c` logs a boot marker every boot:

```
<inf> main: Smart Toilet firmware v1.0.0+0 starting
```

The version comes from `app/VERSION` (→ `APP_VERSION_EXTENDED_STRING`). If the
banner shows the **new** version after the OTA, the swap worked. The signed
image version (nRF Cloud bundle manifest, downgrade checks) comes from
`CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` in `app/prj.conf` — keep it in sync with
`app/VERSION`.

## Step 1 — Establish the baseline (v1.0.0)

Already current on the branch: `app/VERSION` = 1.0.0,
`CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="1.0.0+0"`. Build, flash the full image
(MCUboot + app) with a chip erase (partition layout differs from `main`), reset:

```sh
nrfutil sdk-manager toolchain launch --ncs-version v3.3.1 --chdir ~/ncs/v3.3.1 -- \
  west build -p always -b nrf54lm20dk/nrf54lm20b/cpuapp -d build app -- \
    -DSHIELD=nrf7002eb2 -DEXTRA_CONF_FILE=$PWD/app/cloud.conf \
    -DZEPHYR_EXTRA_MODULES=~/edge_ai_addon/edge-ai
DLL=~/.local/jlink/opt/SEGGER/JLink_V950/libjlinkarm.so
nrfutil device erase   --jlink-dll "$DLL" --serial-number 1051844848
nrfutil device program --jlink-dll "$DLL" --firmware build/merged.hex --serial-number 1051844848
nrfutil device reset   --jlink-dll "$DLL" --serial-number 1051844848
```

Confirm on VCOM0 (`/dev/ttyACM0`): `Smart Toilet firmware v1.0.0+0 starting`,
then `Connected to nRF Cloud`, then `nrf_cloud_fota_poll: ... No pending FOTA job`.

## Step 2 — Build the new image (v1.1.0)

Bump **both** version sources, then build (NO flash — we want the bundle only):

- `app/VERSION`: `VERSION_MINOR = 1`
- `app/prj.conf`: `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="1.1.0+0"`

Optionally make a visible behaviour change too (e.g. a new log line) to be
extra sure. Then:

```sh
nrfutil sdk-manager toolchain launch --ncs-version v3.3.1 --chdir ~/ncs/v3.3.1 -- \
  west build -p always -b nrf54lm20dk/nrf54lm20b/cpuapp -d build app -- \
    -DSHIELD=nrf7002eb2 -DEXTRA_CONF_FILE=$PWD/app/cloud.conf \
    -DZEPHYR_EXTRA_MODULES=~/edge_ai_addon/edge-ai
```

The upload artifact is **`build/dfu_application.zip`** (verify
`unzip -p build/dfu_application.zip manifest.json` shows `version_MCUBOOT: 1.1.0+0`).

> Keep the v1.0.0 device flashed. Do NOT flash v1.1.0 over the wire — the whole
> point is for it to arrive via FOTA.

## Step 3 — Push the image (nRF Cloud side)

Job creation is **not** automatable from this repo (no API key here; the
nRF Cloud MCP is read-only for FOTA). Use one of:

**A. nRF Cloud portal**
1. Firmware Updates → Bundles → upload `build/dfu_application.zip` (type: APP).
2. Create Update / FOTA job → target device `51AF63E40BF36313` → select the
   1.1.0 bundle → deploy.

**B. nRF Cloud REST API** (needs an API key)
```sh
KEY=<nrf-cloud-api-key>
# upload bundle
curl -sS -X POST https://api.nrfcloud.com/v1/firmwares \
  -H "Authorization: Bearer $KEY" -H "Content-Type: application/zip" \
  --data-binary @build/dfu_application.zip
# create a FOTA job for the device (bundleId from the upload response)
curl -sS -X POST https://api.nrfcloud.com/v1/fota-jobs \
  -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" \
  -d '{"bundleId":"<BUNDLE_ID>","deviceIdentifiers":["51AF63E40BF36313"]}'
```

## Step 4 — Observe the device (VCOM0)

The device polls every ~120 s (`FOTA_CHECK_S` in `cloud.c`). Watch for:

```
nrf_cloud_fota_poll: Checking for FOTA job...
<download progress over CoAP>
... dfu_target_mcuboot: writing to secondary (external flash) ...
cloud: FOTA reboot requested (status ...); rebooting to apply update
*** Booting nRF Connect SDK ... ***
main: Smart Toilet firmware v1.1.0+0 starting   <-- SWAP CONFIRMED
```

Capture: `python tools/uart_monitor.py read --port /dev/ttyACM0 --duration 300`.
(To trigger the poll sooner than 120 s, reset the device after the job is live.)

## Step 5 — Verify

- **Device:** boot marker shows `v1.1.0+0` (was `v1.0.0+0`).
- **nRF Cloud:** FOTA job state = completed/succeeded; device
  `last_seen_software_version` updates to 1.1.0.
- **Rollback note:** this build uses swap (not overwrite-only), so a bad image
  that fails to boot/confirm is reverted by MCUboot on the next boot.

## Known caveats / what this exercises

- First real test of the **mixed-geometry swap** (internal-RRAM primary ↔
  external-SPI 4 KB-sector secondary) — the main unknown flagged in the spike.
- The image download adds traffic on the CoAP link; watch for `net_coap`
  timeouts during the (~900 KB) transfer.
