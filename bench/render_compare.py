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

The charts are emitted in the SAME payload shape as bench/render_history.py and drawn
by the SAME renderer (``docs/_static/perf_history.js``) — one chart idiom for the whole
Performance page. The only difference is the x-axis: a history chart's x is a recorded
commit, a comparison chart's x is the swept parameter, which the payload declares as
``xkind: "param"``. There is deliberately no second chart engine.

  cat results.tsv | python3 bench/render_compare.py --html      # the docs block
  cat results.tsv | python3 bench/render_compare.py --md        # a markdown table (PR comment)

Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys

REF_SIZE = 64  # the fixed payload for the fan-out / topic sweeps (must be a kGridSizes point)


# ---- formatters (mirror perf_history.js so the PR table and the axes read identically) ----
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

    # The engine is the LINE dimension here, exactly as fan-out or payload is the line
    # dimension on a history chart. Labels are spelled out rather than keyed by system id
    # so the legend reads as prose and so the write-vs-deliver distinction — the one that
    # decides whether the Zenoh row is a fair counterpart — is visible on the chart itself
    # instead of only in the prose above it.
    LINES = [("libtracer", "libtracer — write (store+notify+deliver)", 0),
             ("libtracer-deliver", "libtracer — deliver-only (propagate)", 1),
             ("zenoh", "Zenoh — zenoh-c 1.9.0, peer mode (put)", 2)]

    def chart(cid, title, cond, series, x, fmt, ylabel, log, read):
        """One chart in the render_history payload shape (see perf_history.js)."""
        out = []
        for key, label, ci in LINES:
            pts = series.get(key) or []
            if pts:
                out.append({"label": label, "ci": ci, "pts": pts})
        if not out:
            return None
        return {"id": cid, "section": "zenoh", "suite": "compare", "xkind": "param",
                "title": title, "cond": cond, "fmt": fmt, "ylabel": ylabel, "log": log,
                "px": x, "series": out, "reading": read}

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

    def add_deliver(series, col):
        """The deliver-only (propagate) fan sweep as a third series next to the write
        row — same fixed point (REF_SIZE, ep=1), mode `inproc-deliver`."""
        dl = _series(rows, "libtracer", "inproc-deliver", {"size": REF_SIZE, "ep": 1},
                     "fan", [col])
        if dl:
            series["libtracer-deliver"] = [[p[0], p[1]] for p in dl]
        return series

    charts = []

    def add(*a, **kw):
        c = chart(*a, **kw)
        if c:
            charts.append(c)

    X_FAN = {"log": True, "fmt": "count", "label": "subscribers per topic (fan-out)"}
    X_SIZE = {"log": True, "fmt": "bytes", "label": "payload size"}
    X_EP = {"log": True, "fmt": "count", "label": "number of topics"}
    REF = f_bytes(REF_SIZE)

    s = add_deliver(two(**{**fan, "col": "deliv"}), "deliv")
    add("ltz-tp-fan", "Throughput vs fan-out", f"{REF} payload · 1 topic · in-process",
        s, X_FAN, "rate", "deliveries / second", True, reading(s, f_rate))
    s = add_deliver(two(**{**fan, "col": "p50"}), "p50")
    add("ltz-lat-fan", "p50 latency vs fan-out", f"{REF} payload · 1 topic · in-process",
        s, X_FAN, "ns", "p50 latency", True, reading(s, f_ns))
    s = two(**{**pay, "col": "deliv"})
    add("ltz-tp-size", "Throughput vs payload", "1 subscriber · 1 topic · in-process",
        s, X_SIZE, "rate", "deliveries / second", True, reading(s, f_rate, label_x=f_bytes))
    s = two(**{**pay, "col": "mbps"})
    add("ltz-mb-size", "Bandwidth vs payload", "1 subscriber · 1 topic · in-process",
        s, X_SIZE, "mb", "application bandwidth", True, reading(s, f_mb, label_x=f_bytes))
    s = two(**{**top, "col": "pub"})
    add("ltz-tp-ep", "Throughput vs topic count", f"{REF} · 1 subscriber · write-by-path",
        s, X_EP, "rate", "publishes / second", False, reading(s, f_rate))
    s = two(**{**top, "col": "p50"})
    add("ltz-lat-ep", "p50 latency vs topic count", f"{REF} · 1 subscriber · write-by-path",
        s, X_EP, "ns", "p50 latency", False, reading(s, f_ns))

    # --- network transports: per-transport libtracer-vs-Zenoh over the real kernel path ---
    # Present only if the transport benches ran (mode `net-<proto>`); each transport gets a
    # p50 and a p99 chart vs payload, both engines on shared axes.
    #
    # The composition-throughput chart was REMOVED, not restyled. Its Zenoh side declared a
    # publisher with no subscriber and no peer, so `put()` never reached the wire — measured,
    # 5 `sendto` calls for 520 000 puts, and those five were multicast scouting beacons. It
    # then emitted ONE K-independent put rate for every K. The libtracer side measured a real
    # `sendmsg` rate but reported `rate * K`, egress-only with no receiver. So BOTH K-curves
    # were arithmetic rather than measured, only one engine did any I/O, and the page called
    # the scenario "loopback UDP · two processes" when it was one process. A valid version
    # needs a real subscriber in a second process on both sides and delivery counted at the
    # receiver — that is a new benchmark, not a restyle. Tracked separately.
    #
    # With it gone there is no network THROUGHPUT comparison at all, which is the honest
    # state. We DO chart both p50 AND the p99 TAIL per transport: for a latency-first
    # substrate the tail (jitter, determinism) is the load-bearing number, so it earns its
    # own axis rather than a footnote.
    #
    # EVERY transport gets a note when it is not charted — including the case where NEITHER
    # engine produced rows. That case used to emit nothing at all, so a transport the prose
    # promised simply vanished while the WS and QUIC notes stayed visible, implying the
    # missing one HAD been charted and tied.
    net_notes: list[str] = []
    for proto in ("udp", "tcp", "ws", "quic"):
        mode = f"net-{proto}"
        fixed = {"fan": 1, "ep": 1}
        p50 = two(mode, fixed, "size", "p50")
        p99 = two(mode, fixed, "size", "p99")
        have_lt, have_zn = bool(p50["libtracer"]), bool(p50["zenoh"])
        if have_lt and have_zn:
            add(f"ltz-net-lat-{proto}", f"{proto.upper()} — p50 latency vs payload",
                "one-way, same-clock · loopback kernel path · single value",
                p50, X_SIZE, "ns", "p50 latency", True, reading(p50, f_ns, label_x=f_bytes))
            add(f"ltz-net-p99-{proto}", f"{proto.upper()} — p99 tail latency vs payload",
                "one-way, same-clock · loopback · single value · TAIL (jitter / determinism)",
                p99, X_SIZE, "ns", "p99 latency", True, reading(p99, f_ns, label_x=f_bytes))
        elif have_lt or have_zn:
            who = "libtracer" if have_lt else "Zenoh"
            net_notes.append(f"<b>{proto.upper()}</b>: measured for {who} only — not charted "
                             "(a comparison needs both engines on the same transport).")
        elif proto == "ws":
            net_notes.append("<b>WS</b>: not charted — libtracer's WebSocket transport shows large "
                             "single-run p50 latency spikes (held until understood), and Zenoh has no "
                             "WebSocket transport to compare against.")
        elif proto == "quic":
            net_notes.append("<b>QUIC</b>: not charted — needs the optional <code>LIBTRACER_WITH_QUIC</code> "
                             "module (msquic + TLS), gated like the dedicated quic CI job.")
        else:
            net_notes.append(f"<b>{proto.upper()}</b>: not measured in this run — neither engine "
                             "produced rows, so there is nothing to compare (the transport bench "
                             "did not come up).")

    return {"charts": charts, "net_notes": net_notes}


def raw_table(rows: list[dict]) -> list[dict]:
    """@brief Every plotted point as absolute numbers, for the PR-comment table.

    This lives outside `build` because only `markdown_table` consumes it. The docs page
    charts hover every point — including the interior ones, which carry no printed label
    and which the generated one-line reading (endpoints only) does not name — so an
    in-page duplicate of the same numbers is weight. That hover is a precondition for
    dropping the table, not a happy accident: without it a five-point sweep would publish
    three numbers and hide two. `docs.yml` shells out to `--md` for the sticky PR comment,
    which has no JavaScript and therefore does still need the table.
    """
    fan = {"mode": "inproc", "fixed": {"size": REF_SIZE, "ep": 1}, "axis": "fan"}
    pay = {"mode": "inproc", "fixed": {"fan": 1, "ep": 1}, "axis": "size"}
    top = {"mode": "inproc-path", "fixed": {"size": REF_SIZE, "fan": 1}, "axis": "ep"}
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
    return table



def _net_notes_html(net_notes: list[str]) -> str:
    """A labeled 'transport coverage' note — what could NOT be charted, and why. Surfaced
    rather than silently dropped, so an absent transport is never read as a tie."""
    if not net_notes:
        return ""
    items = "".join(f"<li>{n}</li>" for n in net_notes)
    return ('<div class="ph-note ph-coverage"><b>Transport coverage</b>'
            f"<ul>{items}</ul></div>")


def html_block(rows: list[dict], provenance: str) -> str:
    """@brief The comparison charts, in the page's ONE chart idiom.

    Emits a `.ph-hist` root exactly like a history section, so `perf_history.js` picks it
    up with no special case and the charts look and behave identically to every other
    chart on the page — same axes, same legend, same colors, same formatters.

    The CSS and JS are NOT inlined here: the history sections above this one already
    inlined both, and `perf_history.js` defers its bootstrap to `DOMContentLoaded`, so a
    root emitted after the script tag is still drawn. That is the whole benefit of there
    being one engine — this block is data, not machinery.
    """
    data = build(rows)
    payload = json.dumps(data, separators=(",", ":"))
    return f""":::{{raw}} html
<div class="ph-hist">
  <p class="ph-note">Both engines built <code>-O3</code> and measured in the SAME pass on the
  same runner, so this is like-for-like on identical hardware. Absolute values on absolute
  axes — no ratios, and every "reading" under a chart is computed from that chart's own
  endpoints at render time. Read trends and orders of magnitude, not the third digit;
  shared-runner variance is real. x-axis = the swept parameter (these are the only charts on
  the page whose x-axis is not a commit).</p>
  <div class="ph-grid ph-charts"></div>
  {_net_notes_html(data.get("net_notes", []))}
  <p class="ph-prov">{provenance}</p>
  <script type="application/json" class="ph-data">{payload}</script>
</div>
:::"""


def standalone_html(rows: list[dict], provenance: str) -> str:
    """A self-contained HTML page for offline local preview — the same charts the docs
    render, viewable without a Sphinx build. Unlike the docs block this one MUST carry
    the assets, since there is no history section above it to have inlined them.
    See bench/grid.sh."""
    static = pathlib.Path(__file__).resolve().parent.parent / "docs" / "_static"
    css = (static / "perf_history.css").read_text()
    js = (static / "perf_history.js").read_text()
    inner = html_block(rows, provenance).split("\n", 1)[1].rsplit(":::", 1)[0]
    return (f"<!doctype html><html><head><meta charset='utf-8'>"
            f"<title>libtracer vs Zenoh</title><style>{css}</style>"
            f"<style>:root{{color-scheme:light dark}}"
            f"body{{margin:2rem;max-width:1100px;font-family:system-ui,sans-serif}}</style>"
            f"</head><body><h1>libtracer vs Zenoh — absolute</h1>{inner}"
            f"<script>{js}</script></body></html>")


def markdown_table(rows: list[dict]) -> str:
    """Absolute-number table for a PR comment (no ratios)."""
    table = raw_table(rows)
    out = ["| sweep | system | point | throughput | bandwidth | p50 latency |",
           "| --- | --- | --- | ---: | ---: | ---: |"]
    for r in table:
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
