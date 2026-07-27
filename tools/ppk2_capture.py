#!/usr/bin/env python3
"""Capture PPK2 current and split it by the firmware's DRAIN-window marker.

Answers the question PLAN_low_power.md step 1 left open: does
conn_mgr_all_if_down() actually power down the nRF7002 RPU, or only mark the
interface down? Connectivity logs cannot tell those apart; current can.

Setup (see docs/POWER_MEASUREMENT.md):
  - PPK2 in AMPERE METER mode, in series on the radio's 5 V feed. The DK's
    PMIC still supplies; the PPK2 only measures.
  - CONFIG_APP_POWER_MARKER drives P1.13 HIGH across the whole radio-on
    region. Wire it to a PPK2 digital input for an exact split.

Usage:
  ppk2_capture.py [--port PATH] [--seconds N] [--csv OUT] [--marker-bit N]

Always pass a /dev/serial/by-id/... path: ttyACM numbers get reassigned when
devices re-enumerate, and the PPK2 and the DK swap places.
"""
import argparse
import sys
import time

from ppk2_api.ppk2_api import PPK2_API

DEFAULT_PORT = (
    "/dev/serial/by-id/usb-Nordic_Semiconductor_PPK2_F1F4EF40E14A-if01"
)


def quiesce(port):
    """Stop any stream left running by a previous session and drain the port.

    Without this, a connect that follows an earlier capture reads mid-stream
    binary instead of the metadata block and get_modifiers() dies on a
    UnicodeDecodeError. Opcode 0x07 is AVERAGE_STOP -- note 0x0d is
    REGULATOR_SET, which will misconfigure the device if sent by mistake.
    """
    import serial

    try:
        s = serial.Serial(port, timeout=0.5)
    except Exception:
        return
    try:
        s.reset_input_buffer()
        s.reset_output_buffer()
        s.write(bytes([0x07]))
        s.flush()
        time.sleep(0.3)
        drained = 0
        while True:
            chunk = s.read(8192)
            if not chunk:
                break
            drained += len(chunk)
            if drained > 8 << 20:
                break
    finally:
        s.close()
    time.sleep(0.2)


def connect(port, voltage_mv, source_mode=False):
    quiesce(port)
    ppk = PPK2_API(port)
    for attempt in range(6):
        try:
            if ppk.get_modifiers():
                break
        except Exception:
            pass
        time.sleep(0.4)
    else:
        print(
            "ERROR: could not read calibration modifiers.\n"
            "  The PPK2 is probably stuck mid-stream. Recover with:\n"
            "    write 0x07 (AVERAGE_STOP) then 0x20 (RESET), wait ~8 s,\n"
            "    then re-resolve the by-id path (ttyACM numbers change).",
            file=sys.stderr,
        )
        sys.exit(1)

    if source_mode:
        # PPK2 BECOMES the supply. Only for the P14 (VDD:nRF) measurement,
        # and only with the DK already powered over USB -- Nordic warn that
        # energising P14 before the DK is up can damage onboard circuitry.
        ppk.use_source_meter()
    else:
        ppk.use_ampere_meter()
    # In ampere mode this is only used for range computation and does NOT
    # drive the rail; in source mode it is the output voltage.
    ppk.set_source_voltage(voltage_mv)
    ppk.toggle_DUT_power("ON")  # closes the path / enables output
    return ppk


def summarize(label, vals):
    if not vals:
        return f"  {label:<22} (no samples)"
    n = len(vals)
    mean = sum(vals) / n
    return (
        f"  {label:<22} n={n:>9}  mean={mean/1000:>9.3f} mA"
        f"  min={min(vals)/1000:>8.3f}  max={max(vals)/1000:>9.3f}"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--seconds", type=float, default=400.0)
    ap.add_argument("--voltage-mv", type=int, default=5000)
    ap.add_argument("--marker-bit", type=int, default=0)
    ap.add_argument("--csv")
    ap.add_argument(
        "--source-mode",
        action="store_true",
        help="PPK2 supplies the rail (P14 / VDD:nRF). Default is ampere "
             "meter, in series, for the radio's 5 V feed.",
    )
    args = ap.parse_args()

    mode = "SOURCE METER" if args.source_mode else "AMPERE METER"
    print(f"mode: {mode}  voltage: {args.voltage_mv} mV")
    ppk = connect(args.port, args.voltage_mv, source_mode=args.source_mode)
    ppk.start_measuring()

    samples, digital = [], []
    t0 = time.time()
    next_report = 30.0
    try:
        while time.time() - t0 < args.seconds:
            raw = ppk.get_data()
            if raw != b"":
                s, d = ppk.get_samples(raw)
                samples.extend(s)
                digital.extend(d)
            elapsed = time.time() - t0
            if elapsed >= next_report:
                recent = samples[-200000:] or [0]
                print(
                    f"  [{elapsed:5.0f}s] {len(samples):>9} samples, "
                    f"recent mean {sum(recent)/len(recent)/1000:.3f} mA",
                    flush=True,
                )
                next_report += 30.0
            time.sleep(0.01)
    except KeyboardInterrupt:
        print("\ninterrupted, summarizing what we have")
    finally:
        ppk.stop_measuring()

    if not samples:
        print("NO SAMPLES -- check wiring and that DUT power is ON")
        sys.exit(1)

    dur = time.time() - t0
    print(f"\n{len(samples)} samples over {dur:.0f}s (~{len(samples)/dur:.0f}/s)")
    print(summarize("ALL", samples))

    # Split on the marker if it actually toggles. A channel that never
    # changes is more likely unwired than genuinely constant, so say so
    # rather than silently reporting a bogus split.
    mask = 1 << args.marker_bit
    if digital and len(set(digital)) > 1:
        n = min(len(samples), len(digital))
        on = [samples[i] for i in range(n) if digital[i] & mask]
        off = [samples[i] for i in range(n) if not (digital[i] & mask)]
        if on and off:
            print(f"\nsplit on marker D{args.marker_bit}:")
            print(summarize("RADIO ON (window)", on))
            print(summarize("RADIO OFF (sleep)", off))
            d = (sum(on)/len(on) - sum(off)/len(off)) / 1000
            print(f"\n  delta (window - sleep): {d:.3f} mA")
            print(
                "\n  The sleep figure is the answer: if it sits at leakage the RPU\n"
                "  powered down; a persistent plateau means if_down did not gate it."
            )
        else:
            print(f"\nmarker D{args.marker_bit} never asserted in this capture")
    else:
        print(
            f"\nNo digital activity seen -- marker D{args.marker_bit} is probably"
            " not wired.\n  Falling back to a threshold split."
        )
        thr = (max(samples) + min(samples)) / 2
        hi = [v for v in samples if v >= thr]
        lo = [v for v in samples if v < thr]
        print(summarize(f"above {thr/1000:.2f} mA", hi))
        print(summarize(f"below {thr/1000:.2f} mA", lo))

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("index,current_uA,digital\n")
            for i, v in enumerate(samples):
                dv = digital[i] if i < len(digital) else ""
                f.write(f"{i},{v:.3f},{dv}\n")
        print(f"\nwrote {args.csv}")


if __name__ == "__main__":
    main()
