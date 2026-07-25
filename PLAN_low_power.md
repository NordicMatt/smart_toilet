# Low-Power Branch Plan

Goal: take the full-connectivity, full-power smart toilet (main branch, v2.0.18)
and find out how low we can push average current, ending with battery operation
on the nPM1300. Voice function stays always-on; connectivity becomes periodic.

## Architecture: duty-cycled connectivity

Replace the always-connected model with a connection window state machine:

- **OFFLINE** (default state): Wi-Fi interface down, nRF70 powered off. No
  association exists, which entirely sidesteps the Nest-mesh silent-drop
  class of problems (nothing to drop) and the link-flap heap pressure
  (no packets). Memfault data (heartbeats, metrics, any coredumps from
  reboots) buffers on-device.
- **DRAIN** (periodic, e.g. every N hours or M heartbeats): bring the
  interface up, connect, upload all buffered Memfault chunks, run one FOTA
  check, then disconnect and power the radio back down. Window closes on
  completion or on a hard timeout so a bad Wi-Fi day cannot pin the radio on.

Audio (PDM capture + AXON inference + flush actuation) runs continuously in
both states. The toilet always hears; it just phones home on a schedule.

## Work items

### Phase 1: connectivity duty cycle (biggest win, most rework)
1. Connection-window state machine in cloud.c: net_if_down/up + conn_mgr,
   nRF70 power off between windows (confirm the driver's power path when the
   interface is administratively down; check whether iface down alone gates
   the RPU or if we need pm_device on the nrf70 node).
2. Memfault buffering between windows: today chunks drain continuously.
   Size the event/chunk storage for the offline period (RAM first; consider
   flash-backed storage if windows get long). Lost-heartbeat behavior after
   watchdog reboots already noted on main; offline buffering makes storage
   sizing matter more.
3. Watchdog semantics rework: the cloud stall monitor and upload-success
   proofs assume continuous connectivity. They must arm only inside DRAIN
   windows. The audio watchdog is untouched.
4. Fleet alerting: the Memfault Device Offline alert (id 21563) fires at
   15 min silent. Raise its threshold above the duty-cycle period (or key it
   to greater than 2x the period) or every sleep window emails everyone.
5. FOTA within the window: check runs once per DRAIN. A pending update
   extends the window until the download/swap completes. Deploy-to-device
   latency becomes up to one period; that is the accepted tradeoff.

### Phase 2: local power reductions (from the axon_low_power sample + earlier findings)
6. CONFIG_PM_DEVICE + CONFIG_PM_DEVICE_RUNTIME, carefully: verify no
   interaction with continuous PDM capture; suspend UARTs and unused
   peripherals. One change at a time with measurements.
7. Quiet build variant: SERIAL/CONSOLE/LOG/PRINTK off (sample's approach).
   Memfault remains the only diagnostics path; acceptable because that is
   how the fleet is debugged anyway. Keep a debug overlay that restores UART.
8. Energy-gated inference (VAD): skip NN passes while block energy sits at
   the room noise floor, with a hangover period and ww_reset() on resume
   (model is stateful). The audio_stats energy accumulator already computes
   what the gate needs. This cuts the AXON + CPU duty cycle during the many
   silent hours; the mic keeps capturing.
9. Audit thread priorities/periods for wakeup reduction (logging thread,
   timers) once measurements identify who wakes the CPU.

### Phase 3: measurement rig (before/throughout, not after)
10. Borrow the axon_low_power GPIO trace pattern: pins high during inference
    and during DRAIN windows, correlated on PPK2. Add a Kconfig'd overlay.
11. PPK2 profiling runbook for the whole board (DK + nRF7002 EB): baseline
    current on main vs low-power branch per phase. Record numbers in this
    file as they land.
12. New heartbeat metrics: time-in-DRAIN per heartbeat, buffered-chunk count,
    connect duration, plus the existing wifi_heap watermark.

### Phase 4: battery (nPM1300)
13. nPM1300 EK integration: devicetree + charger config (400 mA, 4.2 V
    termination per PROFILING.md) + nRF Fuel Gauge library, modeled on the
    NCS npm13xx_fuel_gauge sample.
14. Drop in the Lipo900 battery model .inc from the nPM PowerUP profiling
    run (see PROFILING.md; verify the profile finished and export the model).
15. Battery metrics to Memfault: state of charge, voltage, est. runtime.
    Low-battery behavior: extend duty cycle, then graceful shutdown.

## Open questions (decide before Phase 1 code)
- DRAIN period: hourly? every 6 h? Drives Memfault storage sizing, FOTA
  latency, and the offline-alert threshold. Proposal: start at 1 h.
- Coredump upload urgency: after a crash reboot, connect immediately rather
  than waiting out the period? Proposal: yes, one immediate window on boot.
- Remote knob: duty-cycle period as a Memfault-delivered setting or
  build-time constant? Build-time is simpler; no command channel exists.
- Keep WW-only mode as the low-power baseline (KWS stage stays off).

## Non-goals for this branch
- No changes to wake-word model or thresholds.
- Wi-Fi power-save (PS) stays off even in DRAIN windows; radio-off beats PS
  and avoids the whole silent-drop history.
- No hardware changes until Phase 4.
