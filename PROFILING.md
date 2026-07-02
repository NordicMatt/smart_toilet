# Battery Profiling Runbook — Li-ion, 3.7 V, 900 mAh

Generate a custom battery model for the nPM1300 fuel gauge using the
nPM PowerUP desktop app + nPM Fuel Gauge Board. The resulting `.inc` file
gets dropped into the smart_toilet firmware later for state-of-charge readings.

> Active profile: **`/home/matt/claude/Lipo900`** (capacity 900 mAh, 4.2/3.0 V,
> iChg 400 mA, 25 °C) — running as of 2026-06-26 (`csvReady: false`, no `.inc`
> yet). The old `smart_toilet/Palo/` folder (3000 mAh) is **stale — ignore it.**

## Battery parameters (decided)

| Field | Value | Note |
|---|---|---|
| Name | `Lipo900` | |
| Chemistry | Li-ion | 3.7 V nominal |
| Termination voltage | **4.2 V** | |
| Discharge cutoff | **3.0 V** | |
| Capacity | **900 mAh** | above the 400 mAh wizard minimum |
| Charge current | **400 mA** | ~0.44C (firmware charger matches) |
| Thermistor | **none / ignore** | 2-wire pack |
| Temperature | **25 °C only** | room-temp model; no chamber needed |

## Hardware required

- [ ] nPM1300 EK
- [ ] nPM Fuel Gauge Board (REQUIRED for a custom model — EK alone can't)
- [ ] PALO DY01 battery
- [ ] USB-C power supply, 1.5 A (charges the PMIC)
- [ ] USB-C cable to computer (nPM Controller)
- [ ] A PC that will NOT sleep/hibernate/auto-restart for ~2–3 days (use flight mode)

## Time expectation

Large cell, current-limited profiling. Charge ~7–8 h, discharge ~5–6 h, run
over multiple cycles. Expect **well over 48 h** for the single 25 °C model.
Run it over a long weekend.

## Step 1 — Attach Fuel Gauge Board to EK

1. [ ] Power off nPM Controller — disconnect USB from **J4**.
2. [ ] Plug Fuel Gauge Board's nPM EK header into the **EXT BOARD** socket.
       ⚠️ Align edge connectors **P1→P20** and **P2→P21** carefully.
3. [ ] Reconnect USB to **J4**. Green **LD4** = controller connected.

## Step 2 — Connect battery + power (no NTC)

1. [ ] Connect DY01 to **J1** (correct polarity).
2. [ ] Jumper **NTC (pin 2) ↔ 10 kΩ (pin 3)** on header **P3**.
       ⚠️ Charging will NOT start if this jumper is missing.
3. [ ] Connect USB-C 1.5 A supply to **J3** (USB PMIC) for charging.

(No nRF DK wiring needed for profiling — the Fuel Gauge Board does the discharge.)

## Step 3 — nPM PowerUP

1. [ ] nRF Connect for Desktop → open **nPM PowerUP**.
2. [ ] **Select Device → nPM1300 Evaluation Kit**.
3. [ ] Click **Profile Battery**.

## Step 4 — Enter wizard parameters

Use the table above. Thermistor = ignore/none, single temperature = 25 °C.
Pick an output folder for the model.

## Step 5 — Run

1. [ ] Start profiling. It charges fully, then discharge↔recharge cycles.
2. [ ] Do NOT change config mid-run (aborts). Keep PC awake. Flight mode on.
3. [ ] ⚠️ Heatsink gets HOT — let it cool before handling afterward.
4. [ ] On completion: model saved as both `.json` and `.inc` in your folder.

## After profiling

- The `.inc` file is the battery model for the firmware.
- Next: integrate nPM1300 into smart_toilet (devicetree + Kconfig + nRF Fuel
  Gauge library) and drop in this `.inc`. Modeled on the NCS
  `samples/pmic/native/npm13xx_fuel_gauge` sample.

## Optional: add temperature accuracy later

If the toilet sees temperature swings, profile at 5 °C and 45 °C separately
(needs a temperature chamber) and merge all three in the nPM PowerUP
**Profiles** tab into one multi-temp model.
