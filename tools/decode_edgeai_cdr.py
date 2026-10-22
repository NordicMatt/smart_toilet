#!/usr/bin/env python3
"""Decode edge-AI observability CDRs exported from Memfault.

Each CDR is the CBOR blob staged by nrf_edgeai_obsv_memfault_collect():

    [ { format_version, num_inferences, num_features,
        model: { id, num_classes, num_features, version },
        metrics: [ { id, v, d: [[bin0..binN], ...], c: [[cfg..]] } ] } ]

Two wire formats are in the exports, told apart by format_version:

format_version 3 (firmware 3.0.0+, Edge AI add-on v3.0.0; obsv model id 3 =
3-class KWS abracadabra, classes OTHER, SILENCE, abracadabra):
  INCREMENTAL. Every collect encodes and then resets the metrics, so each CDR
  covers only the interval since the previous one; sum CDRs to aggregate.
  Metric ids (nrf_edgeai_obsv_metric_id in the add-on):
    1 model certainty descriptor (enabled from 3.0.3)
    2 class predictions distribution: 2*N rows x 10 bins. Rows [0,N) are the
      per-class probability histograms over [0,1]; rows [N,2N) the per-class
      streak-length histograms (lengths 1..9, last bin >= 10), with
      c = [[streak_top, streak_tol]].
    3 transition matrix: N x N class-to-class counts, rows = from, cols = to.
    5 mel spectral descriptor: 8 rows x 8 bins over [0,1].
  The wake-word class is the LAST class (abracadabra). Streaks follow the
  model's own argmax with flicker tolerance 1, so they are not comparable to
  the v2 above-threshold streaks.

Older formats (firmware 2.3.x, add-on v2.x):
  CUMULATIVE SINCE BOOT. nrf_edgeai_obsv_reset() was never called, so each CDR
  is a running total, not a per-interval delta. Summing CDRs would
  multiply-count. To get per-interval counts, diff consecutive recordings
  from the same device and discard negatives (a reboot resets the counters).
  Metric ids: 3 probability distribution (8 bins), 9 class streak
  distribution, 8 mel spectral descriptor. Their mel data was fed from an
  int16 view of the feature buffer and is probably garbled.

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

# --- format_version 3 (firmware 3.0.0+) ---
V3_FORMAT_VERSION = 3
V3_MODEL_CERTAINTY_DESC_ID = 1
V3_CLASS_PRED_DIST_ID = 2
V3_TRANSITION_MATRIX_ID = 3
V3_MEL_SPECTRAL_DESC_ID = 5
V3_CLASS_NAMES = ["OTHER", "SILENCE", "abracadabra"]

# --- older formats (firmware 2.3.x) ---
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


def decode_v3(obj):
    """Decode one format_version 3 (incremental) payload."""
    out = {
        "format_version": obj.get("format_version"),
        "incremental": True,
        "num_inferences": obj.get("num_inferences"),
        "num_features": obj.get("num_features"),
        "model": obj.get("model"),
        "bins": None,
        "streak_bins": None,
        "mel_spectral": None,
    }
    n = (obj.get("model") or {}).get("num_classes")
    names = V3_CLASS_NAMES if n == len(V3_CLASS_NAMES) else [f"class{i}" for i in range(n or 0)]
    for m in obj.get("metrics", []) or []:
        rows = [list(r) for r in (m.get("d") or [])]
        if not rows:
            continue
        mid = m.get("id")
        if mid == V3_CLASS_PRED_DIST_ID:
            half = len(rows) // 2
            out["prob_rows"] = dict(zip(names, rows[:half]))
            out["streak_rows"] = dict(zip(names, rows[half:]))
            # The wake-word class is the last one.
            out["bins"] = rows[half - 1]
            out["streak_bins"] = rows[-1]
            cfg = m.get("c")
            if isinstance(cfg, list) and len(cfg) == 1 and len(cfg[0]) == 2:
                out["streak_config"] = {"streak_top": cfg[0][0], "streak_tol": cfg[0][1]}
        elif mid == V3_TRANSITION_MATRIX_ID:
            out["transition_matrix"] = {"classes": names, "counts": rows}
        elif mid == V3_MEL_SPECTRAL_DESC_ID:
            out["mel_spectral"] = {name: r for name, r in zip(MEL_SPECTRAL_ROWS, rows)}
        elif mid == V3_MODEL_CERTAINTY_DESC_ID:
            out["model_certainty"] = {"entropy": rows[0] if rows else None,
                                      "margin": rows[1] if len(rows) > 1 else None,
                                      "stability": rows[2] if len(rows) > 2 else None}
    return out


def decode_legacy(obj):
    """Decode one firmware 2.3.x (cumulative since boot) payload."""
    out = {
        "format_version": obj.get("format_version"),
        "incremental": False,
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


def decode_file(path):
    with open(path, "rb") as f:
        obj = cbor2.load(f)
    if isinstance(obj, list):
        if not obj:
            return None
        obj = obj[0]
    if obj.get("format_version") == V3_FORMAT_VERSION:
        return decode_v3(obj)
    return decode_legacy(obj)


def add_rows(acc, rows):
    """Element-wise sum of equal-shape lists (or lists of lists)."""
    if acc is None:
        return json.loads(json.dumps(rows))
    if rows and isinstance(rows[0], list):
        return [add_rows(a, r) for a, r in zip(acc, rows)]
    return [a + r for a, r in zip(acc, rows)]


def summarize_v3(rs):
    """Sum incremental v3 recordings into one per-device total."""
    total = {"num_inferences": 0, "bins": None, "streak_bins": None,
             "transition_matrix": None}
    for r in rs:
        total["num_inferences"] += r.get("num_inferences") or 0
        if r.get("bins"):
            total["bins"] = add_rows(total["bins"], r["bins"])
        if r.get("streak_bins"):
            total["streak_bins"] = add_rows(total["streak_bins"], r["streak_bins"])
        if r.get("transition_matrix"):
            total["transition_matrix"] = add_rows(total["transition_matrix"],
                                                  r["transition_matrix"]["counts"])
    return total


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

        legacy = [r for r in rs if not r.get("incremental")]
        if legacy:
            print(f"  2.3.x (cumulative): {len(legacy)} recordings")
            print(f"    latest cumulative inferences: {legacy[-1]['num_inferences']:,}")
            print(f"    latest bins: {legacy[-1]['bins']}")
            if legacy[-1].get("streak_bins") is not None:
                print(f"    latest streak bins: {legacy[-1]['streak_bins']}"
                      f" (obsv model version {(legacy[-1].get('model') or {}).get('version')})")

        v3 = [r for r in rs if r.get("incremental")]
        if v3:
            t = summarize_v3(v3)
            cfg = v3[-1].get("streak_config") or {}
            print(f"  3.0.0+ (incremental, summed): {len(v3)} recordings, "
                  f"{t['num_inferences']:,} inferences")
            print(f"    abracadabra prob bins (10 x 0.1): {t['bins']}")
            print(f"    abracadabra streak bins (1..9, >=10; tol "
                  f"{cfg.get('streak_tol')}): {t['streak_bins']}")
            if t["transition_matrix"]:
                print("    transitions (from -> to " + ", ".join(V3_CLASS_NAMES) + "):")
                for name, row in zip(V3_CLASS_NAMES, t["transition_matrix"]):
                    print(f"      {name:>11}: {row}")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(records, f, indent=2)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
