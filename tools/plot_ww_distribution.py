#!/usr/bin/env python3
"""Render the wake-word confidence distribution from decoded edge-AI CDRs.

Input is the aggregated per-device 8-bin histogram produced by
tools/decode_edgeai_cdr.py (diffed across recordings, since the on-device
histogram is cumulative since boot).

Form note: this is a LOLLIPOP chart, not bars. The counts span ~6 decades,
so the axis has to be logarithmic -- and a bar's length encodes a ratio from
zero, which a log axis does not have. Bars on a log scale overstate small
values and understate large ones. Dots sit honestly on a log axis.

Usage:
  plot_ww_distribution.py --agg cdr_agg.json --out ww_distribution.svg [--dark]
"""
import argparse
import json
import math

# Categorical slots 1 and 2 from the validated reference palette.
# validate_palette.js "#2a78d6,#eb6834" --mode light -> ALL CHECKS PASS
LIGHT = {
    "surface": "#fcfcfb", "text": "#0b0b0b", "text2": "#52514e",
    "muted": "#8a8880", "grid": "#e6e5e0",
    "s1": "#2a78d6", "s2": "#eb6834",
}
DARK = {
    "surface": "#1a1a19", "text": "#ffffff", "text2": "#c3c2b7",
    "muted": "#8a8880", "grid": "#33322e",
    "s1": "#3987e5", "s2": "#d95926",
}

W, H = 940, 600
M = {"t": 96, "r": 34, "b": 132, "l": 84}
LOG_MIN, LOG_MAX = 1, 8  # decades 10^1 .. 10^8


def human(n):
    if n >= 1_000_000:
        return f"{n/1_000_000:.1f}M".replace(".0M", "M")
    if n >= 1_000:
        return f"{n/1_000:.1f}k".replace(".0k", "k")
    return str(n)


def build(agg, c):
    pw = W - M["l"] - M["r"]
    ph = H - M["t"] - M["b"]
    x0, y0 = M["l"], M["t"]

    def ypos(v):
        v = max(v, 10 ** LOG_MIN)
        return y0 + ph - (math.log10(v) - LOG_MIN) / (LOG_MAX - LOG_MIN) * ph

    series = [("Toilet #1", agg["Toilet #1"]["bins"], c["s1"]),
              ("Toilet #2", agg["Toilet #2"]["bins"], c["s2"])]
    nbins = 8
    slot = pw / nbins
    # 2px surface gap between the two marks in a slot
    off = 11

    total = sum(agg["Toilet #1"]["bins"]) + sum(agg["Toilet #2"]["bins"])
    fires = agg["Toilet #1"]["bins"][7] + agg["Toilet #2"]["bins"][7]

    s = []
    s.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
             f'viewBox="0 0 {W} {H}" font-family="system-ui,-apple-system,Segoe UI,Roboto,sans-serif">')
    s.append(f'<rect width="{W}" height="{H}" fill="{c["surface"]}"/>')

    # Title block
    s.append(f'<text x="{M["l"]}" y="40" font-size="21" font-weight="600" '
             f'fill="{c["text"]}">Edge AI Observability Wake Word Data</text>')
    # Factual only -- no interpretation. Avoid the "greater-or-equal" glyph:
    # it falls back to a missing-glyph box in renderers lacking it.
    s.append(f'<text x="{M["l"]}" y="64" font-size="13.5" fill="{c["text2"]}">'
             f'{total:,} inferences &#183; 2 devices &#183; 14 days</text>')

    # Legend (always present for 2 series)
    lx = W - M["r"] - 232
    for i, (name, _, col) in enumerate(series):
        ex = lx + i * 116
        s.append(f'<circle cx="{ex+5}" cy="60" r="5.5" fill="{col}"/>')
        s.append(f'<text x="{ex+17}" y="64" font-size="12.5" fill="{c["text2"]}">{name}</text>')

    # Recessive log gridlines
    for d in range(LOG_MIN, LOG_MAX + 1):
        y = ypos(10 ** d)
        s.append(f'<line x1="{x0}" y1="{y:.1f}" x2="{x0+pw}" y2="{y:.1f}" '
                 f'stroke="{c["grid"]}" stroke-width="1"/>')
        lab = f'10<tspan font-size="9" dy="-5">{d}</tspan>'
        s.append(f'<text x="{x0-12}" y="{y+4:.1f}" font-size="11.5" text-anchor="end" '
                 f'fill="{c["muted"]}">{lab}</text>')

    # Baseline
    s.append(f'<line x1="{x0}" y1="{y0+ph}" x2="{x0+pw}" y2="{y0+ph}" '
             f'stroke="{c["grid"]}" stroke-width="1.5"/>')

    # Marks
    for b in range(nbins):
        cx = x0 + slot * (b + 0.5)
        for i, (name, bins, col) in enumerate(series):
            v = bins[b]
            mx = cx + (off if i else -off)
            y = ypos(v)
            # stem
            s.append(f'<line x1="{mx:.1f}" y1="{y0+ph}" x2="{mx:.1f}" y2="{y:.1f}" '
                     f'stroke="{col}" stroke-width="2" opacity="0.45"/>')
            # 2px surface ring so overlapping marks stay separable
            s.append(f'<circle cx="{mx:.1f}" cy="{y:.1f}" r="6.5" fill="{col}" '
                     f'stroke="{c["surface"]}" stroke-width="2"/>')

    # Every bin gets its exact count. These go in per-series rows UNDER the
    # axis rather than above each dot: in bin 0 the two values differ by only
    # ~0.1 of a decade, so stacked labels would collide.

    # X axis labels
    edges = [f"{i/8:.3f}".rstrip("0").rstrip(".") for i in range(9)]
    for b in range(nbins):
        cx = x0 + slot * (b + 0.5)
        s.append(f'<text x="{cx:.1f}" y="{y0+ph+22}" font-size="11" text-anchor="middle" '
                 f'fill="{c["text2"]}">{edges[b]}&#8211;{edges[b+1]}</text>')

    # Per-series value rows (exact counts for every bin)
    for i, (name, bins, col) in enumerate(series):
        ry = y0 + ph + 46 + i * 20
        s.append(f'<circle cx="{x0-30}" cy="{ry-4:.1f}" r="4.5" fill="{col}"/>')
        s.append(f'<text x="{x0-20}" y="{ry:.1f}" font-size="10.5" '
                 f'fill="{c["text2"]}">{name.replace("Toilet ", "T")}</text>')
        for b in range(nbins):
            cx = x0 + slot * (b + 0.5)
            s.append(f'<text x="{cx:.1f}" y="{ry:.1f}" font-size="10.5" '
                     f'text-anchor="middle" fill="{c["text2"]}">{bins[b]:,}</text>')

    s.append(f'<text x="{x0+pw/2:.1f}" y="{y0+ph+96}" font-size="12.5" text-anchor="middle" '
             f'fill="{c["text2"]}">P(wake word) &#8212; model output, binned</text>')
    s.append(f'<text x="20" y="{y0+ph/2:.1f}" font-size="12.5" text-anchor="middle" '
             f'fill="{c["text2"]}" transform="rotate(-90 20 {y0+ph/2:.1f})">'
             f'Inferences (log scale)</text>')


    s.append('</svg>')
    return "\n".join(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--agg", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dark", action="store_true")
    a = ap.parse_args()

    agg = json.load(open(a.agg))
    svg = build(agg, DARK if a.dark else LIGHT)
    with open(a.out, "w") as f:
        f.write(svg)
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
