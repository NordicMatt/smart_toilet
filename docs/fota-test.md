# FOTA test: pushing a new application image

End-to-end test of **Memfault FOTA over HTTPS** on the cloud unit
(`51AF63E40BF36313`, "Toilet #1"). Validates: a new image is downloaded directly
from Memfault over HTTPS, written to the MCUboot **secondary slot in external
MX25R64 flash**, swapped into the internal primary slot by MCUboot on reboot, and
**confirmed** so it survives a power-cycle.

This requires the spike branch `spike/mcuboot-ext-flash-fota` (MCUboot + the
external-flash secondary + Memfault FOTA). The stock `main` build has no
bootloader and cannot do FOTA.

## Delivery path: Memfault Release Management (pure HTTPS)

The device talks only to Memfault. FOTA uses `CONFIG_MEMFAULT_ZEPHYR_FOTA` with
the NCS `fota_download` backend (`CONFIG_MEMFAULT_ZEPHYR_FOTA_BACKEND_NCS`,
Memfault SDK 1.40.0): `cloud.c` calls `memfault_zephyr_fota_start()`, which asks
Memfault whether a newer release is deployed to the device's cohort, and if so
downloads the signed image over HTTPS from the Memfault CDN
(`ota-cdn.memfault.com`) straight into the external-flash MCUboot secondary.
There is **no nRF Cloud connection** — a single TLS connection, so no
dual-connection memory pressure. Memfault's root CAs (incl. DigiCert for the CDN)
are provisioned at boot by `CONFIG_MEMFAULT_NCS_PROVISION_CERTIFICATES`.

**Version matching:** Memfault offers an update only when the cohort's active
release version differs from the device's reported version
(`CONFIG_MEMFAULT_NCS_FW_VERSION`, sourced from `app/VERSION`). The OTA-target
build must have a higher `app/VERSION` than what the device is running.

**Durability:** the bootloader runs a freshly-swapped image in *test* mode
(`CONFIG_BOOT_SWAP_USING_MOVE`) and reverts on the next reset unless it is
confirmed. `cloud.c` calls `boot_write_img_confirmed()` once the new image
reaches the network + Memfault, making the update permanent.

## How a swap is verified

Two ways, no UART required:

- **Memfault (remote):** the device reports a reboot with its software version on
  each boot. After the OTA the latest reboot shows the new version:

  ```sh
  ORG=$(cat ~/.memfault_org_slug); PROJ=$(cat ~/.memfault_project_slug)
  TOK=$(cat ~/.memfault_org_token)
  curl -sS -H "Authorization: Bearer $TOK" \
    "https://api.memfault.com/api/v0/organizations/$ORG/projects/$PROJ/devices/51AF63E40BF36313/reboots?per_page=4"
  ```

- **Serial (if attached):** `main.c` logs a boot marker every boot —
  `<inf> main: Smart Toilet firmware v1.2.0+0 starting`. A new version after the
  OTA means the swap worked. Keep `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` in
  `app/prj.conf` in sync with `app/VERSION`.

## Step 1 — Bootstrap the device (one-time, over the wire)

A device can only pull from Memfault once it is already running a build with the
Memfault FOTA client. The first such image must be flashed over J-Link. Build and
flash with a chip erase (partition layout differs from `main`); the Memfault
project key is passed at build time from `~/.memfault_project_key`:

```sh
KEY=$(tr -d '\n' < ~/.memfault_project_key)
nrfutil sdk-manager toolchain launch --ncs-version v3.3.1 --chdir ~/ncs/v3.3.1 -- \
  west build -p always -b nrf54lm20dk/nrf54lm20b/cpuapp -d build app -- \
    -DSHIELD=nrf7002eb2 -DEXTRA_CONF_FILE=$PWD/app/cloud.conf \
    -DZEPHYR_EXTRA_MODULES=~/edge_ai_addon/edge-ai \
    -DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"$KEY\"
DLL=~/.local/jlink/opt/SEGGER/JLink_V950/libjlinkarm.so
nrfutil device erase   --jlink-dll "$DLL" --serial-number 1051844848
nrfutil device program --jlink-dll "$DLL" --firmware build/merged.hex --serial-number 1051844848
nrfutil device reset   --jlink-dll "$DLL" --serial-number 1051844848
```

Confirm on VCOM0 (`/dev/ttyACM0`): `Smart Toilet firmware vX.Y.Z starting`, then
`Network ready; uploading to Memfault over HTTPS`.

## Step 2 — Build the OTA-target image (higher version)

Bump **both** version sources so the target is newer than the running image, then
build (NO flash — we want the signed binary only):

- `app/VERSION`: bump `VERSION_MINOR` (or another field)
- `app/prj.conf`: set `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` to match (e.g. `"1.2.0+0"`)

```sh
KEY=$(tr -d '\n' < ~/.memfault_project_key)
nrfutil sdk-manager toolchain launch --ncs-version v3.3.1 --chdir ~/ncs/v3.3.1 -- \
  west build -p always -b nrf54lm20dk/nrf54lm20b/cpuapp -d build app -- \
    -DSHIELD=nrf7002eb2 -DEXTRA_CONF_FILE=$PWD/app/cloud.conf \
    -DZEPHYR_EXTRA_MODULES=~/edge_ai_addon/edge-ai \
    -DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"$KEY\"
```

The OTA payload is the **raw signed image** `app.signed.bin` (inside
`build/dfu_application.zip`). Memfault serves it as-is and `dfu_target` writes it
to the MCUboot secondary, so upload the `.bin`, **not** the zip:

```sh
unzip -o build/dfu_application.zip app.signed.bin -d build/
```

> Do NOT flash the target over the wire — the point is for it to arrive via FOTA.

## Step 3 — Upload + deploy to Memfault

Uses the Memfault CLI (`~/.memfault-venv/bin/memfault`). Credentials live in
`~/.memfault_*` (see the credentials memory). Upload accepts the org token;
`deploy-release` needs a Manager-role user key (`--email`/`--password`).

```sh
ORG=$(tr -d '\n' < ~/.memfault_org_slug); PROJ=$(tr -d '\n' < ~/.memfault_project_slug)
TOK=$(tr -d '\n' < ~/.memfault_org_token); MTOK=$(tr -d '\n' < ~/.memfault_manager_token)
VER="1.2.0+0"

~/.memfault-venv/bin/memfault --org-token "$TOK" --org "$ORG" --project "$PROJ" \
  upload-ota-payload --hardware-version nrf54lm20dk --software-type smart-toilet \
  --software-version "$VER" build/app.signed.bin

~/.memfault-venv/bin/memfault --email matthew.heins@nordicsemi.no --password "$MTOK" \
  --org "$ORG" --project "$PROJ" \
  deploy-release --release-version "$VER" --cohort default
```

`--hardware-version nrf54lm20dk` and `--software-type smart-toilet` must match the
device's `CONFIG_MEMFAULT_NCS_HW_VERSION` / `CONFIG_MEMFAULT_NCS_FW_TYPE`, or the
device's OTA query won't match the release.

## Step 4 — The device updates itself

The device polls Memfault every ~120 s (`FOTA_CHECK_S` in `cloud.c`); on the next
poll it sees the newer active release and downloads it. No action needed — just
keep the unit powered and on Wi-Fi. If serial is attached you'll see:

```
mflt: FOTA Update Available. Starting Download with URL: https://ota-cdn.memfault.com/...
downloader: Downloaded .../... bytes (NN%)
... reboot ...
main: Smart Toilet firmware v1.2.0+0 starting   <-- SWAP CONFIRMED
cloud: Running image confirmed (FOTA update made permanent)
```

End-to-end takes ~3.5–5 min (≈30 s to first poll + ~2–2.5 min download + swap).

## Step 5 — Verify

- **Device version flips:** the latest reboot in Memfault (or the boot banner)
  shows the new version.
- **Durability:** after the new image confirms, a power-cycle leaves it on the new
  version (it does **not** revert to the previous one). A swapped-but-unconfirmed
  image — e.g. one that never reaches the network — is reverted by MCUboot on the
  next reset, which is the intended safety net.

## Known caveats / what this exercises

- The **mixed-geometry swap** (internal-RRAM primary ↔ external-SPI 4 KB-sector
  secondary) — the main unknown from the spike; confirmed working.
- Download speed: keep `CONFIG_LOG_MODE_DEFERRED` (not immediate) — immediate
  logging formats a line per 1 KB block inline on the downloader thread and
  throttled the transfer to ~6 KB/s.
- `deploy-release` (activating a release for a cohort) requires the **Manager**
  role; the plain org token is upload/read-only (HTTP 403 otherwise).
