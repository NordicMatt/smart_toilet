# Plan: Add nRF7002 EB II Wi-Fi + nRF Cloud (CoAP) to the Smart Toilet

> **⚠️ Superseded — historical planning record (2026-06-16).** The shipped design
> diverged from this plan: the build target is **`cpuapp`** (secure, no TF-M, not
> `/ns`), and the cloud side moved off nRF Cloud entirely to **pure Memfault over
> HTTPS** (diagnostics + FOTA), so the flush-count shadow and remote-flush command
> were replaced by a `flush_count` Memfault metric (no remote flush). The Wi-Fi
> bring-up and pin re-jumpering below still reflect the hardware. For the current
> architecture see [README.md](README.md) and [docs/fota-test.md](docs/fota-test.md).

**Decisions (2026-06-16):** Transport = **CoAP**; build target = **`nrf54lm20dk/nrf54lm20b/cpuapp/ns`** (TF-M);
cloud features = **flush-event device message + flush-count shadow + remote-flush command (polled)**.

Workspace: `/home/matt/ncs/edge_ai_addon` (already has shield `nrf7002eb2`, nRF70 driver,
`nrf_wifi` blobs, `nrf_cloud` CoAP lib, `nrf70-wifi` snippet, `/ns` board target). No manifest change needed.

## Hardware changes (must happen on the bench)
- **EB II mounts on P17** (expansion connector). It electrically claims: SPI P3.03/P3.02/P3.00/P3.01,
  BUCKEN **P1.04**, IRQ **P1.05**, IOVDDEN P1.13 (coex-only: GRANT P1.07, REQ P0.04, STATUS P0.03 — we will NOT use coex).
- **Mic conflict:** PDM CLK/DAT are on **P1.04/P1.05** → claimed by Wi-Fi. **Re-jumper the mic to two free GPIOs**
  (candidates: the freed UART20 pins, or other non-P17 header GPIOs). Update `pdm20_default_alt`. Verify with a meter.
- **Motor P1.06 / Hall P1.07:** keep, but jumper them on a header *other than P17* (the EB covers P17).
  Electrically free as long as coexistence stays disabled.
- **UART:** shield disables UART20 (VCOM1, the log port) and moves console to UART30 (VCOM0, our control-output port).
  Consolidate logging + control output on VCOM0. **Must disable VCOM1 in nRF Connect Board Configurator.**

## Phase 0 — Snapshot / git
- This repo is now git-tracked. Branch `wifi-nrfcloud`. Commit current working state first.

## Phase 1 — De-risk `/ns` + Axon (GATING SPIKE, before any Wi-Fi)
Axon is a hardware accelerator peripheral; under TF-M it must be assigned to the **non-secure** domain
(plus PDM20, GPIO1 for motor/Hall). Risk: TF-M default may not expose AXON/PDM IRQs to NS.
- Build the **existing** WW-only app for `nrf54lm20dk/nrf54lm20b/cpuapp/ns`.
- Resolve peripheral domain assignment (TF-M `tfm_manifest` / `nrf,peripheral` NS config / DT `*_ns` overlay).
- **Verify wake word still detects on `/ns`** before proceeding.
- Fallback if Axon can't go NS cleanly: revert to secure `cpuapp` and store cloud creds in NVS/settings
  (`TLS_CREDENTIALS_BACKEND_*` non-PSA). Update this plan if we fall back.

## Phase 2 — Wi-Fi link up (no cloud yet)
- `CMakeLists.txt`: `set(SHIELD nrf7002eb2)`; add `nrf70-wifi` snippet.
- `sysbuild.conf`: `SB_CONFIG_WIFI_NRF70=y`, `SB_CONFIG_WIFI_NRF70_SYSTEM_MODE=y`, `SB_CONFIG_BOOTLOADER_NONE=y`.
- `prj.conf`: nRF70 driver + WPA supplicant + native net stack + conn_mgr + L2_WIFI_CONNECTIVITY +
  WIFI_CREDENTIALS + settings/NVS + nRF Security/mbedTLS-DTLS + stacks/heaps
  (base on `nrf_cloud_multi_service/overlay_nrf700x_wifi_coap_no_lte.conf`).
- Wi-Fi credentials: static (`WIFI_CREDENTIALS_STATIC_SSID/PASSWORD`) for first bring-up,
  then move to wifi_cred shell / stored creds.
- Milestone: device associates to AP + gets DHCP IP (conn_mgr L4 connected event).

## Phase 3 — nRF Cloud over CoAP
- Kconfig: `NRF_CLOUD_COAP=y`, `NRF_CLOUD_MQTT=n`, `NRF_CLOUD_CLIENT_ID_SRC_*`,
  `COAP_CLIENT_*`, `DATE_TIME_NTP=y` (no modem time source), `NRF_CLOUD_ALERT`.
- Credentials/onboarding (host PC, separate from build):
  `pip install nrf-cloud-utils`; `device_credentials_installer.py --coap ...` then `nrf_cloud_onboard.py`.
  Creds land in TF-M Protected Storage (`/ns`).
- App: after L4-up → `nrf_cloud_coap_init()` → `nrf_cloud_coap_connect()`.

## Phase 4 — Application integration (the toilet logic)
- New `src/cloud.c/.h`: connection state machine on a workqueue/thread; conn_mgr event handlers.
- **Flush event:** from `actuator.c` flush-complete, post an nRF Cloud **device message** (e.g. `{"flush": {...}}`).
- **Flush counter:** increment a persisted counter (NVS), report to **device shadow** via `nrf_cloud_coap_shadow_*`.
- **Remote flush:** periodic `nrf_cloud_coap_shadow_get(delta=true)`; on a `flush:true` desired delta,
  call `actuator_flush()` and clear the delta. (CoAP is client-driven → poll, e.g. every N s.)
- Keep wake-word → flush path intact; cloud is additive and must never block the audio loop.

## Phase 5 — Verify
- Flash, watch VCOM0: Wi-Fi connect, CoAP connect, message on flush, shadow update in nRF Cloud portal,
  remote flush from portal triggers the motor. Confirm wake-word latency unaffected.

## Open items to confirm during implementation
- Exact two GPIOs for the relocated mic (meter-verify, avoid P2.00–P2.05 QSPI mux).
- Whether flash budget on nRF54LM20B fits Wi-Fi+WPA+mbedTLS+nrf_cloud without MCUboot (sample drops MCUboot for Wi-Fi).
- TF-M partition sizing (Protected Storage for creds).
