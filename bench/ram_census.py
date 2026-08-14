#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The RAM-census instruments, published as a series and warn-ratcheted (#1228).

Two benches price steady-state RAM — the axis that decides whether a node fits the
16 KB target — and until now nothing ran either of them in CI:

  * `bench_conn_ram`         — what ONE CONNECTION costs (per-transport).
  * `bench_ram_census_tcp`   — what the NODE around it costs (100 vertices, a
                               SPEC-created TCP listener, a real remote peer process).

Latency, allocations, code size and flash all have gates or recorded trends; RAM was
the one cost axis measured exclusively by hand, so it could drift silently.

This module does two jobs and keeps them apart on purpose:

  `emit`  MERGES both censuses into the pinned host's existing customSmallerIsBetter
          JSON (the one `perf_emit_benchmark.py` just wrote), so the census points land
          in the SAME store, on the SAME commit axis, and get the SAME host/compiler
          stamp as the latency trend. There is deliberately no second publish pipeline.

  `gate`  WARN-FIRST drift check of the PER-CONNECTION figures against pinned,
          MEASURED values — the same shape as the flash-footprint drift check: a pin, a
          band, a warning annotation, and a re-pin that only ever happens in a PR. It is
          never auto-ratcheted, and it starts in `--mode warn` because a threshold
          picked before the noise is known is the unreachable-budget failure mode this
          repo already hit once.

STAGING (read this before assuming a number here is enforced). The per-connection
ratchet is warn-first; the whole-node census is TREND-ONLY and has no pins at all.
`bench/ram_census_pins.json` carries the activation criterion — how many pinned-host
samples must accumulate before `--mode fail` is defensible — next to the pins it
governs, because prose beside a passing gate rots.

A failure to MEASURE is not a budget verdict (the precedent is #982's footprint
sentinel): a missing or unparsable transcript, a non-zero NULL arm, or a pinned row the
transcript does not contain fails this gate in EITHER mode. Those say the instrument is
broken, not that the budget moved.

    ./ram_census.py emit --conn-raw conn.txt --node-raw node.txt \\
        --merge bench/benchmark_output.json
    ./ram_census.py gate --conn-raw conn.txt --pins bench/ram_census_pins.json
    ./ram_census.py gate --conn-raw conn.txt --pins bench/ram_census_pins.json --emit-pins

Stdlib only. Both benches print the same row shape:
    RESULT arm=<arm> metric=<metric> n=N median=V min=V max=V
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import host_guard  # noqa: E402  (the pinned host's own instrument identity helpers)

ROW = re.compile(
    r"^RESULT\s+arm=(\S+)\s+metric=(\S+)\s+n=(\d+)\s+"
    r"median=(-?\d+)\s+min=(-?\d+)\s+max=(-?\d+)\s*$",
    re.MULTILINE,
)

# The per-connection family the ratchet may cover. Every one of these was measured
# repetition-to-repetition and run-to-run STABLE; the high-water columns (`hw_peak`,
# `hw_retained`) are deliberately absent because they carry a transient buffer that
# swings by tens of kilobytes between runs and would make a band meaningless.
GATEABLE = ("link_base", "per_conn", "after_td", "recycle")

# `null`'s drift is a validity check, not a series: it MUST read 0, and a series that
# is constant zero has no ratio for the chart store to trend.
NULL_ARM = "null"


def parse_rows(text: str) -> dict[tuple[str, str], dict[str, int]]:
    """@brief `RESULT arm=… metric=…` rows -> {(arm, metric): {median,min,max,n}}."""
    out: dict[tuple[str, str], dict[str, int]] = {}
    for m in ROW.finditer(text):
        arm, metric, n, med, lo, hi = m.groups()
        out[(arm, metric)] = {"n": int(n), "median": int(med), "min": int(lo), "max": int(hi)}
    return out


def read_transcript(path: str | None) -> tuple[dict[tuple[str, str], dict[str, int]], str]:
    """@brief Parse a transcript path; returns ({}, reason) when it yields nothing."""
    if not path:
        return {}, "not provided"
    p = pathlib.Path(path)
    if not p.exists():
        return {}, f"{p} does not exist"
    text = p.read_text()
    rows = parse_rows(text)
    return (rows, "") if rows else ({}, f"{p} contains no RESULT rows")


def unit_for(metric: str) -> str:
    """@brief The recorded unit — allocator block COUNTS are not bytes."""
    return "blocks" if metric.endswith("_blocks") else "bytes"


def series_name(prefix: str, arm: str, metric: str) -> str:
    """@brief The recorded series name for one census row.

    These names key a PERSISTED gh-pages store: renaming one orphans every point
    recorded under it and restarts that history at zero. Treat them as fixed.
    """
    return f"{prefix} {arm} {metric}"


def metrics(rows: dict[tuple[str, str], dict[str, int]], prefix: str) -> tuple[list[dict], list[str]]:
    """@brief Census rows -> chart-store metrics, plus the rows deliberately skipped.

    A row whose median is 0 is skipped, and that is not squeamishness: the chart store
    trends a RATIO against the previous point, which is undefined at zero — it would
    render a flat line that can never alert, i.e. something that looks like a
    measurement and is not (the same rule that removed the constant-zero p99 series).
    The zero-valued rows are exactly the invariants (`null` drift, `recycle`), and the
    gate asserts them EXACTLY rather than trending them, which is a stronger check.
    """
    out: list[dict] = []
    skipped: list[str] = []
    for (arm, metric), v in sorted(rows.items()):
        if arm == NULL_ARM:
            skipped.append(f"{prefix} {arm} {metric} (validity check, asserted not trended)")
            continue
        if v["median"] == 0:
            skipped.append(f"{prefix} {arm} {metric} (median 0 — no ratio to trend)")
            continue
        out.append({"name": series_name(prefix, arm, metric), "unit": unit_for(metric),
                    "value": v["median"]})
    return out, skipped


def null_arm_verdict(rows: dict[tuple[str, str], dict[str, int]], what: str) -> str | None:
    """@brief The instrument's own null: a non-zero drift invalidates every column."""
    v = rows.get((NULL_ARM, "drift"))
    if v is None:
        return f"{what}: no NULL arm in the transcript — the instrument is unverified"
    if v["median"] != 0 or v["min"] != 0 or v["max"] != 0:
        return (f"{what}: NULL arm drifted (median={v['median']} min={v['min']} "
                f"max={v['max']}) — every column of this run is suspect")
    return None


def _cmd_emit(args: argparse.Namespace) -> int:
    conn, conn_why = read_transcript(args.conn_raw)
    node, node_why = read_transcript(args.node_raw)
    if not conn and not node:
        print(f"::notice::ram_census: no census transcript to record "
              f"(conn: {conn_why}; node: {node_why})")
        return 0

    series: list[dict] = []
    skipped: list[str] = []
    for rows, prefix, why in ((conn, args.conn_prefix, conn_why),
                              (node, args.node_prefix, node_why)):
        if not rows:
            # Absent is a NOTICE, never a failure: the whole-node census needs a peer
            # process, and a peer that fails to come up must cost this commit its
            # census point, not the whole pinned-host job.
            print(f"::notice::ram_census: {prefix} series not recorded — {why}")
            continue
        s, sk = metrics(rows, prefix)
        series += s
        skipped += sk

    merge = pathlib.Path(args.merge)
    existing = json.loads(merge.read_text()) if merge.exists() else []
    have = {m["name"] for m in existing}
    fresh = [m for m in series if m["name"] not in have]
    merge.write_text(json.dumps(existing + fresh, indent=2) + "\n")
    print(f"ram_census: merged {len(fresh)} census metrics into {merge} "
          f"({len(existing)} already there)")
    for m in fresh:
        print(f"  {m['name']:<44} {m['value']:>12} {m['unit']}")
    for s in skipped:
        print(f"  skipped: {s}")
    return 0


def measured_pin_rows(conn: dict[tuple[str, str], dict[str, int]],
                      pins: dict) -> list[dict]:
    """@brief Resolve every pinned row against the transcript; missing stays None."""
    rows = []
    for p in pins["pins"]:
        v = conn.get((p["arm"], p["metric"]))
        rows.append({**p, "measured": None if v is None else v["median"],
                     "spread": None if v is None else v["max"] - v["min"]})
    return rows


def band_for(pin: dict, pins: dict) -> int:
    """@brief The allowed absolute drift for one pin, in its own unit.

    Both terms exist because the pinned figures span three orders of magnitude: a pure
    percentage is unusable on a 0-to-tens-of-bytes row, and a pure byte count is
    unusable on a 64 KB link base.
    """
    return max(int(pins["band_bytes"]),
               int(abs(pin["value"]) * float(pins["band_pct"]) / 100.0))


def effective_mode(pins: dict, mode: str, toolchain: str) -> tuple[str, str | None]:
    """@brief The mode this run may actually enforce in, and why if it was downgraded.

    A per-connection figure is a property of the COMPILER and the allocator as much as
    of the source (`sizeof` of every transport is in these numbers), so a difference
    measured across toolchains is not attributable to the code. `symbol_ratchet.py`
    refuses such a comparison outright; here it is downgraded to a warning instead,
    because a RAM census that stops reporting is worse than one reporting an
    unattributed number — but it may never turn main red.
    """
    if toolchain == pins.get("toolchain"):
        return mode, None
    return "warn", (f"pinned on {pins.get('toolchain')!r}, measured on {toolchain!r} — "
                    f"a cross-toolchain difference is not attributable to the code")


def _cmd_gate(args: argparse.Namespace) -> int:
    pins = json.loads(pathlib.Path(args.pins).read_text())
    conn, why = read_transcript(args.conn_raw)
    toolchain = host_guard.compiler_identity(args.cxx)

    if args.emit_pins:
        if not conn:
            print(f"ram_census: cannot re-pin — {why}", file=sys.stderr)
            return 2
        fresh = [{"arm": a, "metric": m, "value": v["median"]}
                 for (a, m), v in sorted(conn.items())
                 if a != NULL_ARM and m in GATEABLE]
        print(json.dumps({**pins, "toolchain": toolchain, "pins": fresh}, indent=2))
        return 0

    mode, downgrade = effective_mode(pins, args.mode, toolchain)
    print("RAM_CENSUS_GATE\tinstrument=bench_conn_ram\t"
          f"mode={mode}\tband={pins['band_bytes']}B/{pins['band_pct']}%\t"
          f"toolchain={toolchain}")
    if downgrade:
        print(f"::notice::ram_census: enforcing in warn mode — {downgrade}")

    # --- instrument failures: hard in EITHER mode -------------------------------
    if not conn:
        print(f"RAM_CENSUS_GATE\tFAIL\treason=no-measurement ({why})")
        print("  A failure to measure is not a budget verdict. Warn mode does not")
        print("  answer for a census that did not run.")
        return 1
    bad_null = null_arm_verdict(conn, "bench_conn_ram")
    if bad_null:
        print(f"RAM_CENSUS_GATE\tFAIL\treason=null-arm\n  {bad_null}")
        return 1

    rows = measured_pin_rows(conn, pins)
    missing = [r for r in rows if r["measured"] is None]
    if missing:
        print("RAM_CENSUS_GATE\tFAIL\treason=pin-not-measured")
        for r in missing:
            print(f"  - {r['arm']}/{r['metric']} is pinned but absent from the transcript")
        print("  A pin the run does not produce is a dead pin: either the arm was")
        print("  disabled (--no-can drops the CAN arms) or the bench stopped")
        print("  reporting it. Fix the run or drop the pin in a PR.")
        return 1

    # --- the drift verdict ------------------------------------------------------
    drifted: list[str] = []
    table: list[str] = []
    for r in rows:
        got, want, band = r["measured"], r["value"], band_for(r, pins)
        delta = got - want
        mark = "ok" if abs(delta) <= band else ("GREW" if delta > 0 else "SHRANK")
        table.append(f"  {r['arm']}/{r['metric']}\t{got} B\tpinned {want} B\t"
                     f"{delta:+d}\t(band ±{band} B, in-run spread {r['spread']} B)\t{mark}")
        if mark == "GREW":
            drifted.append(f"{r['arm']}/{r['metric']} grew {delta} B ({want} -> {got}), "
                           f"past the ±{band} B band")
        elif mark == "SHRANK":
            drifted.append(f"{r['arm']}/{r['metric']} shrank {-delta} B ({want} -> {got}) "
                           f"— an improvement: RE-PIN to {got}")
    print("\n".join(table))

    if not drifted:
        print("RAM_CENSUS_GATE\tPASS")
        return 0

    for d in drifted:
        print(f"::warning::per-connection RAM drift — {d}")
    print(f"RAM_CENSUS_GATE\t{'FAIL' if mode == 'fail' else 'WARN'}")
    for d in drifted:
        print(f"  - {d}")
    print("  Re-pin only with a deliberate PR edit of the pin file (--emit-pins prints")
    print("  the measured values). This gate is never auto-ratcheted: a pin that moves")
    print("  itself records drift instead of catching it.")
    return 1 if mode == "fail" else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    e = sub.add_parser("emit", help="merge both censuses into the chart-store JSON")
    e.add_argument("--conn-raw", help="bench_conn_ram transcript")
    e.add_argument("--node-raw", help="bench_ram_census_tcp transcript")
    e.add_argument("--merge", default=str(HERE / "benchmark_output.json"),
                   help="customSmallerIsBetter JSON to append the census metrics to")
    e.add_argument("--conn-prefix", default="conn-ram")
    e.add_argument("--node-prefix", default="node-census")
    e.set_defaults(func=_cmd_emit)

    g = sub.add_parser("gate", help="warn-first per-connection drift check")
    g.add_argument("--conn-raw", help="bench_conn_ram transcript")
    g.add_argument("--pins", default=str(HERE / "ram_census_pins.json"))
    g.add_argument("--mode", choices=("warn", "fail"), default="warn")
    g.add_argument("--cxx", default=None,
                   help="the compiler whose identity attributes the pins (default $CXX)")
    g.add_argument("--emit-pins", action="store_true",
                   help="print the measured pin file and exit (re-pin helper)")
    g.set_defaults(func=_cmd_gate)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
