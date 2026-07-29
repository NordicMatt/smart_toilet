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
    }
    for m in obj.get("metrics", []) or []:
        if m.get("id") == PROBS_DISTRIBUTION_METRIC_ID:
            rows = m.get("d") or []
            if rows:
                out["bins"] = list(rows[0])
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

    if args.json:
        with open(args.json, "w") as f:
            json.dump(records, f, indent=2)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
