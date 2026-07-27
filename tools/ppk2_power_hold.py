#!/usr/bin/env python3
"""Hold the PPK2's measurement path closed so the DUT stays powered.

With the P14 jumper removed (or SB31 cut), the PPK2 is the only path
feeding that rail, so the target is dead whenever no capture is running --
which means you cannot flash it. Run this in the background to keep the
rail up for programming, then stop it.

  tools/ppk2_power_hold.py --seconds 300 &
  nrfutil device program ...

Ampere mode by default: the board still supplies itself, the PPK2 just
closes the switch.
"""
import argparse
import time

import serial
from ppk2_api.ppk2_api import PPK2_API

DEFAULT_PORT = (
    "/dev/serial/by-id/usb-Nordic_Semiconductor_PPK2_F1F4EF40E14A-if01"
)


def quiesce(port):
    """Stop a stream left running by a previous session (0x07 = AVERAGE_STOP)."""
    try:
        s = serial.Serial(port, timeout=0.5)
    except Exception:
        return
    try:
        s.reset_input_buffer()
        s.write(bytes([0x07]))
        s.flush()
        time.sleep(0.3)
        while s.read(8192):
            pass
    finally:
        s.close()
    time.sleep(0.2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--seconds", type=float, default=300.0)
    ap.add_argument("--voltage-mv", type=int, default=1800)
    ap.add_argument("--source-mode", action="store_true")
    args = ap.parse_args()

    quiesce(args.port)
    ppk = PPK2_API(args.port)
    for _ in range(6):
        try:
            if ppk.get_modifiers():
                break
        except Exception:
            pass
        time.sleep(0.4)

    if args.source_mode:
        ppk.use_source_meter()
    else:
        ppk.use_ampere_meter()
    ppk.set_source_voltage(args.voltage_mv)
    ppk.toggle_DUT_power("ON")
    print(f"DUT power ON for {args.seconds:.0f}s -- flash now", flush=True)
    try:
        time.sleep(args.seconds)
    except KeyboardInterrupt:
        pass
    print("releasing")


if __name__ == "__main__":
    main()
