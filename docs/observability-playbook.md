# Observability Playbook — Finding Fleet Problems Quickly

How the smart-toilet fleet (nRF54LM20 + nRF7002, NCS v3.4.0, Memfault via nRF
Cloud) is instrumented, how to triage an incident in minutes, and the lessons
that were paid for in debugging hours. Distilled from the v1.8.6 → v2.0.6
campaign (June–July 2026): five distinct Wi-Fi/crash root causes, one 14-hour
fleet-wide TLS outage, one FOTA crash loop, and a toddler-induced power event —
all diagnosed remotely except the ones that were physically a plug.

---

## 1. The observability stack (what's on the devices and why)

Layered so that every failure mode leaves evidence, and the evidence arrives
*with* the failure:

| Layer | What it captures | Where configured |
|---|---|---|
| **Reboot reasons** | Every reset with cause + raw `RESETREAS` register | Memfault core (free) |
| **Heartbeat metrics** (10 min) | Audio health (`audio_peak_dbfs`, noise floor, clipping), wake-word quality (`ww_prob_max/mean_pct`, `ww_inferences`, `ww_detections`), usage (`flush_count`, `button_press_count`), network (`wifi_disconnect_count`, `wifi_reconnect_count`), jam detection | `config/memfault_metrics_heartbeat_config.def` + `audio_telemetry.c` |
| **Coredumps** | All-thread backtraces (16 KB RAM-backed, 512 B/thread stacks) on every watchdog/fault | `prj.conf` (`MEMFAULT_RAM_BACKED_COREDUMP_SIZE`) |
| **Logs-in-coredumps** (v2.0.6) | Last ~2 KB of device log lines attached to every coredump — the story leading up to the crash | `CONFIG_MEMFAULT_LOGGING_ENABLE` + 2 KB ring |
| **ML observability CDR** (daily) | Wake-word probability histogram for model-drift monitoring | `CONFIG_NRF_EDGEAI_OBSV*` |
| **Watchdog ladder** | Guarantees every hang becomes a *diagnosed reboot*, not a dead unit | see §3 |
| **Server-side alert** | Device Offline email after 15 min of silence | Memfault alert (API-created) |

The design goal: **a device that fails should explain itself**. When a watchdog
fires, the coredump arrives with the reason, the wedged thread's backtrace, all
other threads, the metrics context, and (since v2.0.6) the log lines that
preceded it.

## 2. Triage: failure signatures → root cause in minutes

The first three API calls for any incident: `device_search` (who's alive, what
version), `device_listReboots` (what happened, when), `issues_list` (is it a
known signature). Then match against this table:

| Signature | Meaning | Go look at |
|---|---|---|
| `Software Watchdog at stall_monitor_expiry` + coredump | Cloud/upload stall ≥ 15–20 min (network outage, TLS failure) | `wifi_reconnect_count` burst in timeseries = outage window; logs in coredump for the TLS/DNS errors |
| `Task Watchdog` (audio) + coredump | Audio pipeline wedged ≥ 60 s (mic dead, model erroring) | Coredump logs will contain the repeating `DMIC read error` / inference error |
| `Hardware Watchdog` **during normal running** | Total lockup — kernel/sysclock dead. If recurring: suspect **memory corruption / ABI mismatch** (library built for a different SDK) | Version history: what library/SDK combination changed? |
| `Hardware Watchdog` **once, at an update** | FIXED in v2.0.11 (WDT budget 120 s → 10 min clears the MCUboot swap against external SPI-NOR). On ≥2.0.11, every HW-watchdog reboot is a real lockup. Historical entries ≤2.0.10 at version boundaries were this artifact | If seen on ≥2.0.11: treat as total lockup |
| `Stack Overflow at <fn>` | A thread's stack is too small for a (new) code path | The named function + coredump `SP` tells you *which* thread's stack Kconfig to bump |
| `Unspecified` / API shows `reason=0, mcu_reason_register=null` | **COLD reset — RAM wiped. This is POWER, not firmware.** Warm resets (all software causes) preserve the reboot-reason RAM | The plug. The USB supply. The child. |
| Offline, **no reboot event**, last heartbeats perfectly healthy | Power loss or total network cut. The device cannot report why it's gone | Physically check LED first; if lit, it self-recovers when network returns |
| `ww_inferences=0` while uptime climbs and uploads flow | Audio pipeline dead but loop alive (pre-v2.0.5 blind spot; now watchdogged) | v2.0.5+ turns this into a Task Watchdog with diagnostic coredump |
| `wifi_disconnect_count` stuck at 0 while the link is clearly dead | Silent half-open drop — radio asleep missed the deauth (power-save). Fixed by `CONFIG_NRF_WIFI_LOW_POWER=n` | Should not recur; if it does, PS got re-enabled |
| Whole fleet dark simultaneously, all versions | **Environment changed, not firmware** — server migration, cert/policy change, router. Rolling back versions will find "all broken" | Test the endpoint from a workstation on the same LAN (§4) |

**Zephyr errno decoder ring** (cost us hours): `errno 113` on Zephyr =
`ECONNABORTED` (TLS handshake aborted) — **not** Linux's `EHOSTUNREACH`. A "TLS
handshake error" log line means TCP already worked; routing/DNS theories are
wrong by construction.

## 3. The watchdog ladder (and its two design rules)

Escalating recovery, lightest first — every tier produces evidence:

| Tier | Trigger | Action |
|---|---|---|
| 0 | No upload 2 min while "connected" | Force Wi-Fi re-associate (no reboot) + `wifi_reconnect_count++` |
| 1 | Audio pipeline no progress 60 s | `MEMFAULT_TASK_WATCHDOG()` → coredump + reboot |
| 2 | Cloud thread no progress 15 min / no upload 20 min | `MEMFAULT_SOFTWARE_WATCHDOG()` → coredump + reboot |
| 3 | Wi-Fi down 10 min after having connected | Reason-marked warm reboot |
| 4 | **Hardware WDT** (nRF54 `wdt31` via `task_wdt`), 120 s | Hard reset even if the kernel is dead |

**Rule 1 — feed on work completed, never on loop-iteration-reached.** The v1.8.6
audio watchdog was fed at the top of the loop; a permanently failing
`dmic_read()` spun through its error path petting the watchdog forever — 45 min
deaf with healthy heartbeats. v2.0.5 feeds only after a mic block is read *and*
accepted by the model. Error paths must not feed.

**Rule 2 — software watchdogs all die together.** Every k_timer-based watchdog
depends on the kernel clock; a hard lockup kills them all silently. The
hardware WDT is not optional — it's the only tier that fires when everything
else is dead. It caught every v1.9.x lockup and contained a regression that
would otherwise have required nightly manual power cycles.

## 4. Debugging techniques that cracked the hard ones

- **`app/debug_net.conf` overlay** — DNS/DHCP/socket debug + mbedTLS handshake
  internals. Gotcha: you need all three of `CONFIG_MBEDTLS_DEBUG=y` (Zephyr
  glue), `CONFIG_MBEDTLS_LOG_LEVEL_DBG=y`, **and** `CONFIG_MBEDTLS_DEBUG_LEVEL=3`
  (nrf_security's prompted default of 0 silently wins otherwise). The payoff is
  decisive: it showed the server answering our ClientHello with `close_notify`
  — the whole TLS outage in one log line.
- **Workstation-as-control**: `openssl s_client` from the same LAN, constrained
  to the device's exact TLS parameters (`-tls1_2 -curves prime256v1 -cipher …
  -sigalgs …`, even `-noservername`), separates "server rejects everyone" from
  "server rejects *this device's* ClientHello".
- **The unrelated-host discriminator**: point the device's client at
  `www.google.com`. Google rejecting the ClientHello too (fatal alert 40)
  proved the ClientHello itself was defective — flipping the investigation from
  "who is blocking us" to "what are we sending". Found PSK-only ciphersuites in
  one step.
- **Raw reboot API beats the pretty UI**: `GET .../devices/<serial>/reboots`
  exposes `mcu_reason_register`. `reason=0` + `register=null` = RAM wiped =
  cold/power reset. This is the definitive power-vs-software discriminator.
- **Verify the binary, not the build log**: `strings zephyr.signed.bin | grep
  memfault.com` caught a "successful" build still carrying old endpoint
  hostnames. Build scripts should end with a self-verification block grepping
  `.config` and the binary for the change just made.
- **Upload symbols for *every* image that touches hardware** — including bench
  builds — or coredumps read "Unknown Location".

## 5. Release & deploy workflow

1. Bump `app/VERSION` **and** `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` (they must
   match; Memfault compares reported vs release version to offer OTA).
2. Pristine build with **`CCACHE_DISABLE=1`** (see lesson below), self-verify
   `.config` + binary.
3. `upload-mcu-symbols` (org token) → `upload-ota-payload` → `deploy-release`
   per cohort (**manager** email+token auth).
4. Watch pickup (~5–10 min: 60 s FOTA check → ~780 KB download → MCUboot swap →
   boot → report). Expect one benign HW-watchdog blip at the swap.
5. **Tag + push only when the version is actually on hardware.** Every flashed
   or OTA'd version gets a git tag — the reboot history is only interpretable
   against exact sources.

**Emergency stop for a bad OTA** (from the FOTA crash-loop incident): offering
*any* release to devices whose download path is broken restarts the loop, so —
`GET /deployments` → `DELETE /deployments/{id}` (manager auth; the org token
gets 403; the release-archive PATCH does not do the job). `deploy-release` of
the old version fails with "already active" when Memfault has auto-reverted —
that error is *good news*. When the download path itself is the bug, cohort
deploys must happen **after** each unit is bench-flashed, never before.

## 6. Lessons learned (the expensive ones)

**Platform & build**
- **Matched versions or memory corruption**: running a prebuilt add-on
  (edge-ai 2.2.0, built for NCS 3.4.0) on a different SDK (3.3.1) produced
  random total lockups — no crash pattern, just a dead kernel every few hours.
  If lockups correlate with a library update, check the ABI pairing first.
- **NCS 3.4.0 / mbedTLS 4.x makes crypto opt-in**: nothing you don't
  `PSA_WANT_*` exists. A Wi-Fi-personal build gets no ECDHE unless the app asks
  (`PSA_WANT_ALG_ECDH`, `ECC_SECP_R1_256`, `RSA_PKCS1V15_SIGN`,
  `ECC_KEY_PAIR_GENERATE/EXPORT`) — without them the TLS client offers
  PSK-only ciphersuites and every public server rejects it. Each missing want
  fails exactly one handshake step further: debug iteratively.
- **The same migration resizes stacks**: PSA-dispatched TLS (esp. SHA-384 PRF)
  needs more stack than legacy mbedTLS. Any thread that does TLS needs a
  re-audit — the `downloader` thread's 4352 B (fine on 3.3.1) overflowed
  fleet-wide on the first 3.4.0 OTA. 8 KB now.
- **ccache does not track `__has_include`**: after *adding* a header that an
  existing `#if __has_include(...)` probes for, cached objects keep the old
  branch — even `-p always` pristine builds. `CCACHE_DISABLE=1` or clear the
  cache.
- **Kconfig hygiene in 3.4.0**: warnings are hard errors; promptless/derived
  symbols (`NRF_SECURITY`, `PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC`) must not be
  assigned in `.conf` files.

**Networking**
- The NCS Memfault port defaults to **deprecated** hosts
  (`chunks-nrf.memfault.com`); the documented current ones are
  `chunks.memfault.com` / `device.memfault.com` — overridden in
  `config/memfault_platform_config.h`.
- `DNS_RESOLVER_MAX_SERVERS=1`: a single static DNS server *evicts* the
  router's DHCP-provided resolver. Prefer the local resolver + TTL cache.
- Wi-Fi power save on a mesh network = silent half-open drops (radio sleeps
  through the deauth; `wifi_disconnect_count` stays 0). `CONFIG_NRF_WIFI_LOW_POWER=n`
  turned drops into *noticed* disconnects that conn_mgr auto-heals.
- All cloud calls must be time-bounded and are: HTTP poll 5 s, TLS connect
  10 s, DNS 30 s, TCP retransmit ~100 s. A 20-min "stall" is therefore
  *repeated fast failures during a real outage* (routers do overnight
  maintenance), not a hung call — the correct response is visibility
  (`wifi_reconnect_count`), not more timeouts.

**Observability itself**
- **Logs are only as good as their signal density**: a 1 Hz stats log wraps a
  2 KB ring in ~20 s, evicting the error lines the coredump exists to carry.
  Audit what's in the ring before shipping log capture ("do we have logs worth
  grabbing?" is the right question).
- **Per-interval event counters beat lifetime totals** for dashboard metrics
  (`heartbeat_add(1)` per event; keep lifetime totals in NVS if the device
  needs them).
- ISR-context events (button GPIO) stage through an atomic + semaphore; only
  thread context may touch Memfault metric APIs.
- Server-side CDR retention is 1/device/day — collect on that cadence, not
  faster.
- **Physical failures masquerade as software**: a wonky USB plug produced
  "Unspecified" resets and offline gaps that consumed hours as firmware
  theories. Cold-reset signature (§2) + "check the plug first" is the rule.
  Fleet postmortem: since v2.0.1, software outages: 0; power-connector
  outages: 3.

## 7. Incident checklist

```
1. date -u                                    # server clocks may skew from local
2. device_search                              # who's alive, what version
3. device_listReboots (both units)            # what + when; match §2 table
4. issues_list                                # known signature? trace_count growing?
5. If reboot has coredump  → trace_get: faulting thread, backtrace, attached logs
6. If no reboot event      → timeseries: last heartbeats healthy?
     abrupt-stop + no Wi-Fi events = power → check the plug/LED before any theory
7. If network-ish          → wifi_reconnect_count / wifi_disconnect_count timeseries;
     openssl from workstation on the same LAN as control
8. Fix → version bump → pristine CCACHE_DISABLE build → verify .config+binary
   → symbols → payload → deploy → watch pickup → tag + push
```

---

*Every version that ever ran on hardware is a git tag (`v1.8.6` … `v2.0.6`).
The full debugging narrative behind each is in the tagged commit messages —
they are the long-form changelog of this document.*
