#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Render the libtracer-vs-Zenoh comparison as an interactive, absolute-value chart
block for the Performance docs page (and a plain-text table for a PR comment).

Input is the combined ``RESULT`` stream from ``bench_libtracer grid`` +
``bench_zenoh grid`` (the mode-tagged 12-field line in bench_common.hpp). We plot
ABSOLUTE measured values — throughput / latency / bandwidth vs fan-out, payload,
and topic count — with libtracer and Zenoh as two series on shared axes. No speed-up
ratios and no prose claim that isn't a measured point: every "reading" under a chart
is computed from the data's own endpoints at render time.

The chart markup is a self-contained MyST ``:::{raw} html`` block that references the
committed ``docs/_static/ltz_compare.{css,js}`` assets and carries the measured data
as embedded JSON; ltz_compare.js draws the SVGs in the reader's browser (theme-aware).

  cat results.tsv | python3 bench/render_compare.py --html      # the docs block
  cat results.tsv | python3 bench/render_compare.py --md        # a markdown table (PR comment)

Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import sys

REF_SIZE = 64  # the fixed payload for the fan-out / topic sweeps (must be a kGridSizes point)


# ---- formatters (mirror ltz_compare.js so the table and axes read identically) ----
def f_rate(v: float) -> str:
    if v >= 1e6:
        return f"{v/1e6:.1f} M" if v < 1e7 else f"{v/1e6:.0f} M"
    if v >= 1e3:
        return f"{v/1e3:.0f} k"
    return f"{v:.0f}"


def f_ns(v: float) -> str:
    if v >= 1e6:
        return f"{v/1e6:.1f} ms" if v < 1e7 else f"{v/1e6:.0f} ms"
    if v >= 1e3:
        return f"{v/1e3:.1f} µs" if v < 1e4 else f"{v/1e3:.0f} µs"
    return f"{v:.0f} ns"


def f_bytes(v: int) -> str:
    return f"{v//1024} KB" if v >= 1024 else f"{v} B"


def f_mb(v: float) -> str:
    return f"{v/1000:.1f} GB/s" if v >= 1000 else f"{v:.0f} MB/s"


def f_count(v: int) -> str:
    return f"{v//1000}k" if v >= 1000 else f"{v}"


def parse(text: str) -> list[dict]:
    rows = []
    for line in text.splitlines():
        f = line.rstrip("\n").split("\t")
        if len(f) == 12 and f[0] == "RESULT":
            rows.append(dict(sys=f[1], mode=f[2], size=int(f[3]), fan=int(f[4]), ep=int(f[5]),
                             pub=float(f[6]), deliv=float(f[7]), mbps=float(f[8]),
                             p50=int(f[9]), p99=int(f[10]), mean=int(f[11])))
    return rows


def _series(rows, sys, mode, fixed, axis, cols):
    """Sorted [ [axis, *cols] ] for one system/mode slice, deduped on the axis value."""
    sel = [r for r in rows if r["sys"] == sys and r["mode"] == mode
           and all(r[k] == v for k, v in fixed.items())]
    seen, out = set(), []
    for r in sorted(sel, key=lambda r: r[axis]):
        if r[axis] in seen:
            continue
        seen.add(r[axis])
        out.append([r[axis]] + [r[c] for c in cols])
    return out


def has_zenoh(rows) -> bool:
    return any(r["sys"] == "zenoh" for r in rows)


def build(rows: list[dict]) -> dict:
    """Assemble the chart configs + raw table from measured rows."""
    def two(mode, fixed, axis, col):
        return {sys: [[p[0], p[1]] for p in _series(rows, sys, mode, fixed, axis, [col])]
                for sys in ("libtracer", "zenoh")}

    fan = {"mode": "inproc", "fixed": {"size": REF_SIZE, "ep": 1}, "axis": "fan"}
    pay = {"mode": "inproc", "fixed": {"fan": 1, "ep": 1}, "axis": "size"}
    top = {"mode": "inproc-path", "fixed": {"size": REF_SIZE, "fan": 1}, "axis": "ep"}

    def reading(series, ffmt, unit_first="at fan-out 1", label_x=f_count):
        lt, zn = series.get("libtracer", []), series.get("zenoh", [])
        if not lt:
            return ""
        parts = [f"libtracer {ffmt(lt[0][1])} → {ffmt(lt[-1][1])}"]
        if zn:
            parts.append(f"Zenoh {ffmt(zn[0][1])} → {ffmt(zn[-1][1])}")
        return (f"Across the sweep ({label_x(lt[0][0])} → {label_x(lt[-1][0])}): "
                + "; ".join(f"<b>{p}</b>" for p in parts) + ".")

    charts = []
    s = two(**{**fan, "col": "deliv"})
    charts.append({"id": "tp-fan", "title": "Throughput vs fan-out",
                   "cond": f"{f_bytes(REF_SIZE)} payload · 1 topic · in-process",
                   "x": {"log": True, "fmt": "count", "label": "subscribers per topic (fan-out)"},
                   "y": {"log": True, "fmt": "rate", "label": "deliveries / second"},
                   "series": s, "reading": reading(s, f_rate)})
    s = two(**{**fan, "col": "p50"})
    charts.append({"id": "lat-fan", "title": "p50 latency vs fan-out",
                   "cond": f"{f_bytes(REF_SIZE)} payload · 1 topic · in-process",
                   "x": {"log": True, "fmt": "count", "label": "subscribers per topic (fan-out)"},
                   "y": {"log": True, "fmt": "ns", "label": "p50 latency"},
                   "series": s, "reading": reading(s, f_ns)})
    s = two(**{**pay, "col": "deliv"})
    charts.append({"id": "tp-size", "title": "Throughput vs payload",
                   "cond": "1 subscriber · 1 topic · in-process",
                   "x": {"log": True, "fmt": "bytes", "label": "payload size"},
                   "y": {"log": True, "fmt": "rate", "label": "deliveries / second"},
                   "series": s, "reading": reading(s, f_rate, label_x=f_bytes)})
    s = two(**{**pay, "col": "mbps"})
    charts.append({"id": "mb-size", "title": "Bandwidth vs payload",
                   "cond": "1 subscriber · 1 topic · in-process",
                   "x": {"log": True, "fmt": "bytes", "label": "payload size"},
                   "y": {"log": True, "fmt": "mb", "label": "application bandwidth"},
                   "series": s, "reading": reading(s, f_mb, label_x=f_bytes)})
    s = two(**{**top, "col": "pub"})
    charts.append({"id": "tp-ep", "title": "Throughput vs topic count",
                   "cond": f"{f_bytes(REF_SIZE)} · 1 subscriber · write-by-path",
                   "x": {"log": True, "fmt": "count", "label": "number of topics"},
                   "y": {"log": False, "fmt": "rate", "label": "publishes / second"},
                   "series": s, "reading": reading(s, f_rate)})
    s = two(**{**top, "col": "p50"})
    charts.append({"id": "lat-ep", "title": "p50 latency vs topic count",
                   "cond": f"{f_bytes(REF_SIZE)} · 1 subscriber · write-by-path",
                   "x": {"log": True, "fmt": "count", "label": "number of topics"},
                   "y": {"log": False, "fmt": "ns", "label": "p50 latency"},
                   "series": s, "reading": reading(s, f_ns)})

    # raw table — every plotted point, absolute
    table = []
    sweeps = [("fan-out", fan, f_count, "deliv", None),
              ("payload", pay, f_bytes, "deliv", "mbps"),
              ("topics", top, f_count, "pub", None)]
    for si, (label, spec, xf, rate_col, bw_col) in enumerate(sweeps):
        for sys in ("libtracer", "zenoh"):
            cols = [rate_col, "p50"] + ([bw_col] if bw_col else [])
            pts = _series(rows, sys, spec["mode"], spec["fixed"], spec["axis"], cols)
            for ri, p in enumerate(pts):
                table.append({
                    "sweep": label if (sys == "libtracer" and ri == 0) else "",
                    "grp": si > 0 and sys == "libtracer" and ri == 0,
                    "system": sys, "x": xf(p[0]),
                    "throughput": f_rate(p[1]) + "/s",
                    "bandwidth": f_mb(p[3]) if bw_col else "—",
                    "p50": f_ns(p[2]),
                })
    return {"charts": charts, "table": table}


BANNER = (
    "Both engines are built <code>-O3</code> and measured in the same pass on the same "
    "runner, so this is like-for-like on identical hardware. Values are absolute and "
    "representative of the CI runner — read trends and orders of magnitude, not the "
    "third digit (shared-runner variance is real). No ratios: every point is a measured "
    "number on an absolute axis."
)


def html_block(rows: list[dict], provenance: str) -> str:
    data = build(rows)
    payload = json.dumps(data, separators=(",", ":"))
    return f"""\
:::{{raw}} html
<link rel="stylesheet" href="_static/ltz_compare.css">
<div class="ltz-compare">
  <div class="ltz-banner"><span class="ic">i</span><span>{BANNER}</span></div>
  <div class="ltz-legend">
    <span class="item"><span class="sw" style="background:var(--ltz-lt)"></span>libtracer <span class="sub">— inproc zero-copy dispatch</span></span>
    <span class="item"><span class="sw" style="background:var(--ltz-zn)"></span>Zenoh <span class="sub">— zenoh-c 1.9.0, peer mode</span></span>
  </div>
  <div class="ltz-grid" id="ltz-charts"></div>
  <div class="ltz-tablewrap"><table class="ltz-raw" id="ltz-raw"><thead><tr>
    <th>sweep</th><th>system</th><th>point</th><th>throughput</th><th>bandwidth</th><th>p50 latency</th>
  </tr></thead><tbody></tbody></table></div>
  <p class="ltz-prov">{provenance}</p>
  <script type="application/json" id="ltz-data">{payload}</script>
</div>
<script src="_static/ltz_compare.js"></script>
:::"""


def standalone_html(rows: list[dict], provenance: str) -> str:
    """A self-contained HTML page (assets inlined) for offline local preview — the same
    charts the docs render, viewable without a Sphinx build. See bench/grid.sh."""
    import pathlib
    static = pathlib.Path(__file__).resolve().parent.parent / "docs" / "_static"
    css = (static / "ltz_compare.css").read_text()
    js = (static / "ltz_compare.js").read_text()
    block = html_block(rows, provenance)
    # strip the MyST fence + the _static asset references; inline css/js instead.
    inner = block.split("\n", 1)[1].rsplit(":::", 1)[0]
    inner = inner.replace('<link rel="stylesheet" href="_static/ltz_compare.css">', "")
    inner = inner.replace('<script src="_static/ltz_compare.js"></script>', "")
    return (f"<!doctype html><html><head><meta charset='utf-8'>"
            f"<title>libtracer vs Zenoh</title><style>body{{margin:2rem;max-width:1100px;"
            f"font-family:system-ui,sans-serif}}{css}</style></head><body>"
            f"<h1>libtracer vs Zenoh — absolute</h1>{inner}<script>{js}</script></body></html>")


def markdown_table(rows: list[dict]) -> str:
    """Absolute-number table for a PR comment (no ratios)."""
    data = build(rows)
    out = ["| sweep | system | point | throughput | bandwidth | p50 latency |",
           "| --- | --- | --- | ---: | ---: | ---: |"]
    for r in data["table"]:
        out.append(f"| {r['sweep']} | {r['system']} | {r['x']} | {r['throughput']} | "
                   f"{r['bandwidth']} | {r['p50']} |")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--html", action="store_true", help="emit the MyST raw-html chart block")
    ap.add_argument("--standalone", action="store_true", help="emit a self-contained HTML preview page")
    ap.add_argument("--md", action="store_true", help="emit a markdown absolute-number table")
    ap.add_argument("--prov", default="", help="provenance line for the html block")
    args = ap.parse_args()
    rows = parse(sys.stdin.read())
    if not has_zenoh(rows):
        sys.stderr.write("render_compare: no zenoh rows (run bench/fetch_zenoh.sh + build)\n")
    prov = args.prov or "local build — not a CI deploy"
    if args.md:
        print(markdown_table(rows))
    elif args.standalone:
        print(standalone_html(rows, prov))
    else:
        print(html_block(rows, prov))
    return 0


if __name__ == "__main__":
    sys.exit(main())
