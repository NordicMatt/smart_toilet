#!/usr/bin/env python3
"""Decode edge-AI observability CDRs exported from Memfault.

Each CDR is the CBOR blob staged by nrf_edgeai_obsv_memfault_collect():

    [ { format_version, num_inferences, num_features,
        model: { id, num_classes, num_features, version },
        metrics: [ { id, v, d: [[bin0..binN]] } ] } ]

Metric id 3 is the wake-word probability distribution
(CONFIG_NRF_EDGEAI_OBSV_METRIC_PROBS_DISTRIBUTION), binned over [0,1] with
CONFIG_NRF_EDGEAI_OBSV_PROBS_DISTRIBUTION_BIN_NUM bins (8 here).

IMPORTANT -- the histogram is CUMULATIVE SINCE BOOT. nrf_edgeai_obsv_reset()
is never called, so each CDR is a running total, not a per-interval delta.
Summing CDRs would multiply-count. To get per-interval counts, diff
consecutive recordings from the same device and discard negatives (a reboot
resets the counters).

Usage:
  decode_edgeai_cdr.py <dir-or-files...> [--json out.json]
"""
import argparse
import glob
import json
import os
import re
import sys

import cbor2

PROBS_DISTRIBUTION_METRIC_ID = 3
# Streak-length histogram (CONFIG_NRF_EDGEAI_OBSV_METRIC_CLASS_STREAK_DIST).
# Registered from firmware 2.3.1+0 (2.3.0 enabled the Kconfig but never
# registered the metric, so its CDRs carry only id 3). Cumulative since boot,
# like the probability histogram.
#
# Row layout depends on the firmware's obsv model version:
#   version 1 (2.3.1): one row, the raw single-output model. The streak metric
#     records a streak only when the argmax class CHANGES, and a one-class
#     model never changes class, so every streak bin is zero. Useless.
#   version 2 (2.3.2+): two rows, fed { threshold, p }. Row 1 is the wake-word
#     row: its probability histogram is the same as before, and its streak
#     histogram counts consecutive frames with p > CONFIG_WW_PROBABILITY_THRESHOLD
#     (10 bins: lengths 1..9 exactly, last bin >= 10). Row 0 is the
#     below-threshold complement (constant probability, gap-length streaks).
# We always report the LAST row as the wake-word row.
CLASS_STREAK_DIST_METRIC_ID = 9
# Mel spectral descriptor (CONFIG_NRF_EDGEAI_OBSV_METRIC_MEL_SPECTRAL_DESC,
# firmware 2.3.3+): 8 rows x 8 bins over [0,1], cumulative since boot.
MEL_SPECTRAL_DESC_METRIC_ID = 8
MEL_SPECTRAL_ROWS = ["low_ratio", "mid_ratio", "high_ratio", "centroid",
                     "spread", "entropy", "flatness", "contrast"]
# Filenames look like: <serial>_edgeai-observability_YYYYMMDD-HHMMSS.bin
NAME_RE = re.compile(r"(?P<serial>[0-9A-F]{16})_.*?_(?P<ts>\d{8}-\d{6})")


def decode_file(path):
    with open(path, "rb") as f:
        obj = cbor2.load(f)
    if isinstance(obj, list):
        if not obj:
            return None
        obj = obj[0]

    out = {
        "format_version": obj.get("format_version"),
        "num_inferences": obj.get("num_inferences"),
        "model": obj.get("model"),
        "bins": None,
        "streak_bins": None,
        "mel_spectral": None,
    }
    for m in obj.get("metrics", []) or []:
        rows = m.get("d") or []
        if not rows:
            continue
        if m.get("id") == PROBS_DISTRIBUTION_METRIC_ID:
            out["bins"] = list(rows[-1])
        elif m.get("id") == CLASS_STREAK_DIST_METRIC_ID:
            out["streak_bins"] = list(rows[-1])
            if len(rows) > 1:
                out["gap_streak_bins"] = list(rows[0])
        elif m.get("id") == MEL_SPECTRAL_DESC_METRIC_ID:
            out["mel_spectral"] = {name: list(r) for name, r in zip(MEL_SPECTRAL_ROWS, rows)}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--json")
    args = ap.parse_args()

    files = []
    for p in args.paths:
        if os.path.isdir(p):
            files += glob.glob(os.path.join(p, "**", "*.bin"), recursive=True)
        else:
            files.append(p)

    # Deduplicate: the UI exports overlap between zips, and the same
    # recording can appear more than once under different folders.
    seen, records = set(), []
    for path in sorted(files):
        base = os.path.basename(path)
        if base in seen:
            continue
        seen.add(base)

        m = NAME_RE.search(base)
        if not m:
            print(f"skip (unparsed name): {base}", file=sys.stderr)
            continue
        try:
            rec = decode_file(path)
        except Exception as e:
            print(f"skip (decode failed): {base}: {e}", file=sys.stderr)
            continue
        if not rec or not rec.get("bins"):
            print(f"skip (no distribution metric): {base}", file=sys.stderr)
            continue

        ts = m.group("ts")
        rec["serial"] = m.group("serial")
        rec["timestamp"] = f"{ts[0:4]}-{ts[4:6]}-{ts[6:8]}T{ts[9:11]}:{ts[11:13]}:{ts[13:15]}"
        rec["file"] = base
        records.append(rec)

    records.sort(key=lambda r: (r["serial"], r["timestamp"]))
    print(f"decoded {len(records)} unique recordings "
          f"from {len(seen)} files ({len(files)} paths)")

    for serial in sorted({r["serial"] for r in records}):
        rs = [r for r in records if r["serial"] == serial]
        print(f"\n{serial}: {len(rs)} recordings, "
              f"{rs[0]['timestamp'][:10]} .. {rs[-1]['timestamp'][:10]}")
        print(f"  latest cumulative inferences: {rs[-1]['num_inferences']:,}")
        print(f"  latest bins: {rs[-1]['bins']}")
        if rs[-1].get("streak_bins") is not None:
            print(f"  latest streak bins: {rs[-1]['streak_bins']}"
                  f" (obsv model version {(rs[-1].get('model') or {}).get('version')})")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(records, f, indent=2)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
