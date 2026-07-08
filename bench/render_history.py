#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Render UNIFIED per-family benchmark-history charts for the Performance page.

The benchmark-action store (gh-pages ``dev/bench/data.js``) tracks >130 flat
series; the stock /dev/bench page draws one tiny chart per series, which makes
comparing related series (fan-out sweep, payload sweep, MT scaling, ...) nearly
impossible. This module groups the series into FAMILIES and emits one chart per
family with every series as a line on shared axes — plus, for families whose
series differ by a *numeric* parameter (fan-out, payload bytes, topic count,
fold width, threads), the full three-axis view set drawn client-side by
``docs/_static/perf_history.js``: trend (value vs commit, one line per
parameter), sweep (value vs parameter, one line per commit, recency-faded),
heatmap (commit x parameter, color = value), and an isometric 3D surface
(commit x parameter x value).

Release tags (``v*``) are resolved to recorded commits via git and drawn as
labeled vertical markers on every history chart: an exact marker when the tag's
commit is itself a recorded point, else a '≈' marker at the nearest FOLLOWING
recorded commit (ancestor test). Everything degrades to a note, never a crash:
no store -> the caller skips the block; no git/tags -> charts without markers.

The emitted markup is a self-contained MyST ``:::{raw} html`` block with the
committed ``docs/_static/perf_history.{css,js}`` assets INLINED (same rationale
as render_compare: path-independent, no CDN, self-contained page). Stdlib only.
"""
from __future__ import annotations

import json
import pathlib
import re
import subprocess

REPO = pathlib.Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# family specs
# ---------------------------------------------------------------------------
# Each family: a regex over series names in ONE suite ("latency"/"throughput"),
# a label + numeric-parameter extractor for the capture, and axis metadata.
# `px` (the parameter axis) is present only when the captured key is numeric —
# it unlocks the sweep/heatmap/3D views; label-keyed families stay trend-only.


def _num(m: re.Match) -> float:
    return float(m.group(1))


FAMILIES: list[dict] = [
    # -- latency suite ------------------------------------------------------
    dict(id="lat-fan", suite="latency", title="In-process p50 latency by fan-out",
         cond="inproc · 64 B payload · 1 topic — one line per fan-out",
         pat=r"^inproc 64B/fan(\d+)/1ep p50 latency$",
         label=lambda m: f"fan {m.group(1)}", key=_num, log=True,
         fmt="ns", ylabel="p50 latency",
         px=dict(label="fan-out (subscribers)", log=True, fmt="count")),
    dict(id="lat-payload", suite="latency", title="In-process p50 latency by payload size",
         cond="inproc · fan-out 1 · 1 topic — one line per payload",
         pat=r"^inproc (\d+)B/fan1/1ep p50 latency$",
         label=lambda m: f"{m.group(1)} B", key=_num, log=False,
         fmt="ns", ylabel="p50 latency",
         px=dict(label="payload size", log=True, fmt="bytes")),
    dict(id="lat-borrow-payload", suite="latency",
         title="Loaned-path (borrow) p50 latency by payload size",
         cond="inproc-borrow · fan-out 1 · 1 topic — one line per payload",
         pat=r"^inproc-borrow (\d+)B/fan1/1ep p50 latency$",
         label=lambda m: f"borrow {m.group(1)} B", key=_num, log=False,
         fmt="ns", ylabel="p50 latency",
         px=dict(label="payload size", log=True, fmt="bytes")),
    dict(id="lat-topics", suite="latency", title="Write-by-path p50 latency by topic count",
         cond="inproc-path · 64 B · fan-out 1 — one line per registry size (resolver canary)",
         pat=r"^inproc-path 64B/fan1/(\d+)ep p50 latency$",
         label=lambda m: f"{m.group(1)} topics", key=_num, log=False,
         fmt="ns", ylabel="p50 latency",
         px=dict(label="topic count", log=True, fmt="count")),
    dict(id="lat-dispatch", suite="latency", title="Dispatch modes — p50 latency",
         cond="64 B · fan-out 1 · 1 topic — the four dispatch paths on one axes",
         pat=r"^(inproc|inproc-borrow|inproc-path|inproc-mt1) 64B/fan1/1ep p50 latency$",
         label=lambda m: m.group(1), key=lambda m: m.group(1), log=False,
         fmt="ns", ylabel="p50 latency"),
    dict(id="lat-mt", suite="latency", title="MT scaling — per-delivery cost",
         cond="64 B · fan-out 1 — ns/delivery per worker count (lower = better scaling)",
         pat=r"^inproc-mt(\d+) 64B/fan1/\d+ep ns/delivery$",
         label=lambda m: f"mt{m.group(1)}", key=_num, log=False,
         fmt="ns", ylabel="ns per delivery",
         px=dict(label="worker threads", log=True, fmt="count")),
    dict(id="lat-eptype", suite="latency", title="Endpoint-type family — p50 latency",
         cond="eptype-* · 64 B · fan-out 1 · 1 topic",
         pat=r"^eptype-([\w-]+) 64B/fan1/1ep p50 latency$",
         label=lambda m: f"eptype-{m.group(1)}", key=lambda m: m.group(1), log=False,
         fmt="ns", ylabel="p50 latency"),
    dict(id="lat-fold", suite="latency", title="Fold family — per-delivery cost by width",
         cond="fold-n* · 512 B · fan-out 1 — ns/delivery per fold width",
         pat=r"^fold-n(\d+) 512B/fan1/1ep ns/delivery$",
         label=lambda m: f"fold n{m.group(1)}", key=_num, log=False,
         fmt="ns", ylabel="ns per delivery",
         px=dict(label="fold width n", log=True, fmt="count")),
    dict(id="lat-acl", suite="latency", title="ACL-inherit family — p50 latency",
         cond="acl-inherit depth 4 · 64 B — single-thread vs mt4",
         pat=r"^(acl-inherit-d4(?:-mt4)?) 64B/fan\d+/\d+ep p50 latency$",
         label=lambda m: m.group(1), key=lambda m: m.group(1), log=False,
         fmt="ns", ylabel="p50 latency"),
    dict(id="lat-heap", suite="latency", title="Heap & memory footprint",
         cond="allocator probe (allocs / bytes per hop) + whole-run max RSS — mixed units, log axis",
         names=[("heap allocs per forward (probe)", "allocs/forward"),
                ("heap allocs per terminus (probe)", "allocs/terminus"),
                ("heap bytes per forward (probe)", "bytes/forward"),
                ("heap bytes per terminus (probe)", "bytes/terminus"),
                ("bench_libtracer max RSS", "max RSS (KB)")],
         log=True, fmt="num", ylabel="value (per-series units)"),
    # -- throughput suite ---------------------------------------------------
    dict(id="tp-fan", suite="throughput", title="Throughput by fan-out",
         cond="inproc · 64 B · 1 topic — one line per fan-out",
         pat=r"^inproc 64B/fan(\d+)/1ep throughput$",
         label=lambda m: f"fan {m.group(1)}", key=_num, log=True,
         fmt="rate", ylabel="deliveries / second",
         px=dict(label="fan-out (subscribers)", log=True, fmt="count")),
    dict(id="tp-payload", suite="throughput", title="Throughput by payload size",
         cond="inproc · fan-out 1 · 1 topic — one line per payload",
         pat=r"^inproc (\d+)B/fan1/1ep throughput$",
         label=lambda m: f"{m.group(1)} B", key=_num, log=False,
         fmt="rate", ylabel="deliveries / second",
         px=dict(label="payload size", log=True, fmt="bytes")),
    dict(id="tp-dispatch", suite="throughput", title="Dispatch modes — throughput",
         cond="64 B · fan-out 1 · 1 topic — the four dispatch paths on one axes",
         pat=r"^(inproc|inproc-borrow|inproc-path|inproc-mt1) 64B/fan1/1ep throughput$",
         label=lambda m: m.group(1), key=lambda m: m.group(1), log=False,
         fmt="rate", ylabel="deliveries / second"),
    dict(id="tp-mt", suite="throughput", title="MT scaling — aggregate throughput",
         cond="64 B · fan-out 1 — one line per worker count",
         pat=r"^inproc-mt(\d+) 64B/fan1/\d+ep throughput$",
         label=lambda m: f"mt{m.group(1)}", key=_num, log=False,
         fmt="rate", ylabel="deliveries / second",
         px=dict(label="worker threads", log=True, fmt="count")),
    dict(id="tp-eptype", suite="throughput", title="Endpoint-type family — throughput",
         cond="eptype-* · 64 B · fan-out 1 · 1 topic",
         pat=r"^eptype-([\w-]+) 64B/fan1/1ep throughput$",
         label=lambda m: f"eptype-{m.group(1)}", key=lambda m: m.group(1), log=False,
         fmt="rate", ylabel="deliveries / second"),
    dict(id="tp-fold", suite="throughput", title="Fold family — throughput by width",
         cond="fold-n* · 512 B · fan-out 1 — one line per fold width",
         pat=r"^fold-n(\d+) 512B/fan1/1ep throughput$",
         label=lambda m: f"fold n{m.group(1)}", key=_num, log=False,
         fmt="rate", ylabel="deliveries / second",
         px=dict(label="fold width n", log=True, fmt="count")),
    dict(id="tp-topics", suite="throughput", title="Write-by-path throughput by topic count",
         cond="inproc-path · 64 B · fan-out 1 — one line per registry size",
         pat=r"^inproc-path 64B/fan1/(\d+)ep throughput$",
         label=lambda m: f"{m.group(1)} topics", key=_num, log=False,
         fmt="rate", ylabel="publishes / second",
         px=dict(label="topic count", log=True, fmt="count")),
]


# ---------------------------------------------------------------------------
# release-tag resolution (directive: mark releases on graph history)
# ---------------------------------------------------------------------------
def _release_tags() -> list[tuple[str, str]]:
    """@brief All ``v*`` tags as (name, peeled commit sha); [] when git/tags absent."""
    try:
        p = subprocess.run(
            ["git", "for-each-ref", "refs/tags/v*",
             "--format=%(refname:short) %(objectname) %(*objectname)"],
            capture_output=True, text=True, cwd=REPO, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return []
    tags = []
    for ln in p.stdout.splitlines():
        f = ln.split()
        if len(f) >= 2:
            tags.append((f[0], f[2] if len(f) >= 3 else f[1]))  # peeled sha for annotated tags
    return tags


def _is_ancestor(a: str, b: str) -> bool:
    try:
        return subprocess.run(["git", "merge-base", "--is-ancestor", a, b],
                              capture_output=True, cwd=REPO, timeout=30).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def release_annotations(entries: list[dict]) -> list[dict]:
    """@brief Map each ``v*`` tag onto this suite's recorded-commit axis.

    Exact when the tag's commit is itself a recorded point; else ``approx``
    (drawn with '≈') at the nearest FOLLOWING recorded commit — the first entry
    that has the tag commit as an ancestor. A tag reachable from no recorded
    commit (or unresolvable shas, e.g. a shallow clone) is silently skipped.
    """
    shas = [e.get("commit", {}).get("id", "") for e in entries]
    ann: list[dict] = []
    for name, sha in _release_tags():
        idx, approx = None, False
        for i, s in enumerate(shas):
            if s == sha:
                idx = i
                break
        if idx is None:
            for i, s in enumerate(shas):
                if s and _is_ancestor(sha, s):
                    idx, approx = i, True
                    break
        if idx is not None:
            ann.append({"i": idx, "label": name, "approx": approx})
    return ann


# ---------------------------------------------------------------------------
# family assembly
# ---------------------------------------------------------------------------
def _suite_key(suite_name: str) -> str:
    if "latency" in suite_name:
        return "latency"
    if "throughput" in suite_name:
        return "throughput"
    return suite_name


def _series_by_name(entries: list[dict]) -> dict[str, list[list[float]]]:
    """@brief name -> [[entry_idx, value], ...] (sparse; a series may start late)."""
    out: dict[str, list[list[float]]] = {}
    for i, e in enumerate(entries):
        for b in e.get("benches", []):
            try:
                out.setdefault(b["name"], []).append([i, float(b["value"])])
            except (KeyError, TypeError, ValueError):
                continue
    return out


def _first_line(msg: str, limit: int = 72) -> str:
    ln = (msg or "").splitlines()[0] if msg else ""
    ln = ln.replace("<", "&lt;").replace(">", "&gt;")
    return ln[: limit - 1] + "…" if len(ln) > limit else ln


def build(data: dict) -> dict:
    """@brief Assemble the chart payload perf_history.js draws.

    Returns {"suites": {key: {shas, msgs, releases}}, "charts": [...]} — each
    chart carries its family's series as sparse [entry_idx, value] point lists,
    a global per-label color index (same label == same color on every chart),
    and, for numeric-parameter families, the parameter axis meta + per-series
    numeric parameter value (pv) that unlocks the sweep/heatmap/3D views.
    """
    suites: dict[str, dict] = {}
    suite_series: dict[str, dict[str, list[list[float]]]] = {}
    for suite_name, entries in data.get("entries", {}).items():
        if not entries:
            continue
        k = _suite_key(suite_name)
        suites[k] = {
            "shas": [e.get("commit", {}).get("id", "")[:7] for e in entries],
            "msgs": [_first_line(e.get("commit", {}).get("message", "")) for e in entries],
            "releases": release_annotations(entries),
        }
        suite_series[k] = _series_by_name(entries)

    colors: dict[str, int] = {}
    charts: list[dict] = []
    for fam in FAMILIES:
        names = suite_series.get(fam["suite"], {})
        if not names:
            continue
        picked: list[tuple] = []  # (sort_key, label, pv, pts)
        if "names" in fam:  # explicit fixed list (heap/memory)
            for i, (name, label) in enumerate(fam["names"]):
                if name in names:
                    picked.append((i, label, None, names[name]))
        else:
            for name in names:
                m = re.match(fam["pat"], name)
                if not m:
                    continue
                key = fam["key"](m)
                pv = key if isinstance(key, float) else None
                picked.append((key, fam["label"](m), pv, names[name]))
        picked.sort(key=lambda t: (t[0],) if not isinstance(t[0], str) else (float("inf"), t[0]))
        if len(picked) < 2:
            continue  # a one-line "family" is not a comparison chart
        series = []
        for _, label, pv, pts in picked:
            ci = colors.setdefault(label, len(colors))
            s = {"label": label, "ci": ci, "pts": pts}
            if pv is not None:
                s["pv"] = pv
            series.append(s)
        chart = {"id": fam["id"], "suite": fam["suite"], "title": fam["title"],
                 "cond": fam["cond"], "fmt": fam["fmt"], "ylabel": fam["ylabel"],
                 "log": fam["log"], "series": series}
        if "px" in fam and all("pv" in s for s in series):
            chart["px"] = fam["px"]
        charts.append(chart)
    return {"suites": suites, "charts": charts}


# ---------------------------------------------------------------------------
# emission
# ---------------------------------------------------------------------------
def _assets() -> tuple[str, str]:
    """@brief The committed chart CSS + JS, read at generate time to be INLINED
    (same reasoning as render_compare._assets: performance.md renders one
    directory deep, so a relative _static href would 404; inlining is
    path-independent and keeps the page self-contained — no CDN)."""
    static = REPO / "docs" / "_static"
    return (static / "perf_history.css").read_text(), (static / "perf_history.js").read_text()


def html_block(data: dict) -> str:
    """@brief The MyST raw-html block embedding the family charts (or '' if the
    store yields no chartable family)."""
    payload = build(data)
    if not payload["charts"]:
        return ""
    css, js = _assets()
    nser = sum(len(c["series"]) for c in payload["charts"])
    blob = json.dumps(payload, separators=(",", ":"))
    return f"""\
:::{{raw}} html
<style>{css}</style>
<div class="ph-hist">
  <p class="ph-note">{len(payload["charts"])} family charts · {nser} series · x-axis = recorded
  <code>main</code> commits (oldest → newest) · 🏷 dashed verticals mark release tags
  (<b>≈</b> = tag commit itself is not a recorded point; marker sits at the nearest following
  recorded commit). Families with a numeric parameter axis offer <b>trend</b> /
  <b>sweep</b> / <b>heatmap</b> / <b>3D</b> views — same data, three axes
  (commit × parameter × value). Hover any chart for exact per-commit values.</p>
  <div class="ph-grid" id="ph-charts"></div>
  <script type="application/json" id="ph-data">{blob}</script>
</div>
<script>{js}</script>
:::"""


def release_note(entries: list[dict]) -> str:
    """@brief One-line release annotation for the table views ('' if none)."""
    ann = release_annotations(entries)
    if not ann:
        return ""
    shas = [e.get("commit", {}).get("id", "")[:7] for e in entries]
    parts = []
    for a in ann:
        where = f"`{shas[a['i']]}`" + (" (≈ nearest following recorded commit)" if a["approx"] else "")
        parts.append(f"🏷 **{a['label']}** at {where}, point {a['i'] + 1}/{len(entries)}")
    return "Releases: " + " · ".join(parts) + "."
