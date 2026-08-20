#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The ADR-0079 store sweep's DETERMINISTIC columns, banked and warn-ratcheted (#1428).

`bench_store_sweep` landed diagnostic-only (#941 / PR #1427). ADR-0079 §Verification asks
for "a standing bench so a regression in any configuration is visible, and so a future
change to the composition default is argued from CI numbers, not memory"; a hand-run
sweep satisfies that partially, and this module is the rest of it.

WHAT IS BANKED, AND WHY ONLY THIS
=================================

Two columns, both deterministic by construction:

  * **Store `used()` high-water** (`RESULT_STORE_HWM`). The `hwm` mode runs
    SINGLE-THREADED over all T lanes precisely so the figure is a function of the
    workload and the topology and not of the scheduler. Measured byte-identical across
    five invocations taken at a 1-minute load average of 32.7 on a 31-CPU host — i.e.
    the column does not move even when the machine does. It is banked as a series AND
    carries a pinned warn-first ratchet (`bench/store_sweep_pins.json`).
  * **The T=1 latency cell** of each (arm, leg), best-of-rounds over the ABBA-interleaved
    tags. Banked as a series and watched by `store_guard.py drift` against a rolling
    best-of-window baseline. It is deliberately NOT pinned: an exact pin on a TIMED
    column would red on the host rather than on the code, and the repo already owns the
    right instrument for a timed series.

WHAT MUST NEVER BE BANKED HERE, stated rather than implied by omission
======================================================================

  * **The fan-out T-sweep** (`RESULT_STORE_TPUT`, T >= 2). Standing in-tree practice is
    that thread-contention benches are DIAGNOSTIC-not-gated —
    `bench_rx_source_topology`, `bench_route_handle_contention` and
    `bench_fanout_clone_storm` all decline the gate on that ground — and the sweep's own
    banked run put the unpinned throughput window's A/A null at 58.9 %. A gate whose
    null is 58.9 % is a flaky red, and a flaky red teaches everyone to ignore the gate.
  * **The process-heap escape high-water** (`RESULT_STORE_ESCAPE`).
    `bench/ram_census_pins.json` records a `tcp-server hw_peak` moving 66 % across runs
    and ~41 KB within one, which is why `ram_census.py` already excludes high-water
    columns from its pinned rows. The same caution applies here by construction.

Both are REFUSED rather than merely unused: a pin naming a non-gateable metric fails the
gate (see `GATEABLE`), and rows from either excluded family are counted and announced as
dropped, so a reader of CI output sees the exclusion happen instead of inferring it.

STAGING (read this before assuming a number here is enforced). The ratchet is
warn-first, exactly like the RAM census it is modelled on, and
`bench/store_sweep_pins.json` carries the activation criterion next to the pins it
governs, because prose beside a passing gate rots. It also PRE-DECLARES the step every
pinned `used()` figure takes when ADR-0079 Stage 2 (#843, gated on #1285) moves vertex
placement onto the injected store — a series started now would otherwise flag that as a
regression on the very PR that closes the gap.

A failure to MEASURE is not a budget verdict (#982's precedent, `ram_census.py`'s rule):
a missing transcript, a calibration the run did not pass, an overflowing or refusing
store, or a pinned row the run does not produce fails this gate in EITHER mode.

    ./store_sweep_gate.py emit --sweep-raw sweep.tsv --merge bench/benchmark_output.json
    ./store_sweep_gate.py gate --sweep-raw sweep.tsv --pins bench/store_sweep_pins.json
    ./store_sweep_gate.py gate --sweep-raw sweep.tsv --pins ... --emit-pins

Stdlib only, like every other bench tool here.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# The row shapes are DEFINED by the collator that already reads this transcript, and are
# imported rather than re-parsed here: two parsers for one instrument is how a tag gains a
# column in one consumer and silently matches zero rows in the other.
import collate_store_sweep as sweep  # noqa: E402

# The band arithmetic and the cross-toolchain downgrade are `ram_census.py`'s rules,
# imported for the same reason `store_guard.py` imports `host_guard`'s contamination
# predicate: "how wide is a band" and "when is a difference attributable" must be decided
# in ONE place, or two pinned gates end up with two policies nobody chose.
import ram_census  # noqa: E402

import host_guard  # noqa: E402

# The deterministic hwm metrics a pin may name. `used_total` is the occupancy a deployer
# sizes a slab against; `stores` is the topology that occupancy is spread over, pinned so
# a store that disappears cannot be hidden by a total that happens to hold.
GATEABLE = ("used_total", "stores")

# The families this instrument produces that are NEVER gated, and the measured reason for
# each. Printed on every run and enforced against the pin file, so the exclusion is a
# mechanism rather than a sentence in a comment.
EXCLUDED = {
    "throughput": "the fan-out T-sweep is thread-contention (its own A/A null read 58.9 %), "
                  "and every thread-contention bench in this tree is DIAGNOSTIC-not-gated",
    "escape": "a process-heap high-water moves 66 % across runs (bench/ram_census_pins.json), "
              "which is why ram_census.py already excludes high-water columns from its pins",
}

#: The arm that injects no store, so it has no occupancy to measure — it still carries
#: latency cells, and those ARE banked: a regression in the shipped default is a
#: regression.
kNoStoreArm = "H-baseline"

#: Series-name prefixes. These key a PERSISTED gh-pages store: renaming one orphans every
#: point recorded under it and restarts that history at zero. Treat them as fixed.
kHwmPrefix = "store-sweep"
kLatPrefix = "store-lat"

#: How many `CALIBRATE total_failures` rows a trustworthy transcript carries.
#: `run_store_sweep.sh` breaks the instrument's line BEFORE and AFTER the run, because an
#: instrument that was live at the start and inert by the end would otherwise sign off on
#: every cell in between. One row is a half-checked run and is refused.
kMinCalibrations = 2


def read_transcript(path: str | None) -> tuple[list[str], str]:
    """@brief Read a sweep transcript into lines; returns ([], reason) when it yields none."""
    if not path:
        return [], "not provided"
    p = pathlib.Path(path)
    if not p.exists():
        return [], f"{p} does not exist"
    lines = p.read_text().splitlines()
    return (lines, "") if lines else ([], f"{p} is empty")


def calibration_verdict(lines: list[str]) -> str | None:
    """@brief The instrument's own line-break check, read back off the transcript.

    `bench_store_sweep calibrate` asserts every ADR-0079 channel is actually drawn from
    and that the null arm reads exactly 0; it exits non-zero and prints its tally. A
    transcript with no tally at all is not a passing run, it is an unverified one, so it
    is refused here rather than trusted.
    """
    tallies = [f for line in lines
               if (f := line.split("\t"))[0] == "CALIBRATE" and len(f) >= 3
               and f[1] == "total_failures"]
    if len(tallies) < kMinCalibrations:
        return (f"only {len(tallies)} calibration tally/tallies in the transcript "
                f"(expected at least {kMinCalibrations}: the line is broken before AND "
                f"after the run) — this run is unverified, not passing")
    bad = [f[2] for f in tallies if f[2] != "0"]
    if bad:
        return (f"calibration reported {', '.join(bad)} failure(s) — a channel wired to a "
                f"store nothing draws from is an arm that silently measures nothing")
    return None


def hwm_totals(hwm: dict) -> dict[tuple[str, int], dict[str, int]]:
    """@brief Per-(arm, T) occupancy: the total, the store count, and the fault columns.

    The per-store rows are summed rather than pinned one by one because NARROW carries 49
    of them at T=24 — one per lane store — and 49 pins would re-pin as a block anyway. The
    `stores` count is kept beside the total so the topology cannot change underneath a
    total that happens to hold.
    """
    out: dict[tuple[str, int], dict[str, int]] = {}
    for (arm, T, _store, _idx), (used, _cap, classes, overflow) in hwm.items():
        slot = out.setdefault((arm, T), {"used_total": 0, "stores": 0, "classes": 0,
                                         "overflow": 0})
        slot["used_total"] += used
        slot["stores"] += 1
        slot["classes"] += classes
        slot["overflow"] += overflow
    return out


def refusals(chan: dict) -> int:
    """@brief Total refusals across every counted channel — a store that turned a draw away."""
    return sum(ref for (_arm, _T, _ch), (_b, _by, _pk, ref) in chan.items())


def lat_best(lat: dict) -> dict[tuple[str, str], float]:
    """@brief Best-of-rounds ns per op for every (arm, leg) — the T=1 latency cell.

    Best-of-rounds, never median: `docs/methodology.md` makes that normative because
    contamination is one-sided, and the collator this imports from applies the identical
    estimator to the same rows.
    """
    return {key: v for key, samples in lat.items()
            if (v := sweep.best(samples)) is not None}


def series(totals: dict[tuple[str, int], dict[str, int]],
           lat: dict[tuple[str, str], float]) -> list[dict]:
    """@brief The banked points: occupancy per (arm, T), and the T=1 latency per (arm, leg).

    `stores` is asserted by the gate and NOT trended, the same rule `ram_census.py` applies
    to its invariant rows: a series that never moves cannot alert, so publishing it would
    look like a measurement and be none.
    """
    out = [{"name": f"{kHwmPrefix} {arm} T{T} used_total", "unit": "bytes",
            "value": v["used_total"]}
           for (arm, T), v in sorted(totals.items()) if v["used_total"] > 0]
    out += [{"name": f"{kLatPrefix} {arm} {leg} p50", "unit": "ns", "value": round(ns, 2)}
            for (arm, leg), ns in sorted(lat.items())]
    return out


def dropped(lines: list[str]) -> dict[str, int]:
    """@brief Count the rows this module deliberately refuses to bank, by family.

    Announced on every run. A full local sweep is a perfectly good input to the gate — its
    excluded columns are simply never read — and saying how many rows were dropped is what
    keeps "we do not gate the T-sweep" from decaying into "nobody remembers whether we do".
    """
    tags = {"throughput": "RESULT_STORE_TPUT", "escape": "RESULT_STORE_ESCAPE"}
    return {family: sum(1 for line in lines if line.startswith(tag + "\t"))
            for family, tag in tags.items()}


def measured_pin_rows(totals: dict[tuple[str, int], dict[str, int]],
                      pins: dict) -> list[dict]:
    """@brief Resolve every pinned row against the transcript; missing stays None."""
    rows = []
    for p in pins["pins"]:
        cell = totals.get((p["arm"], int(p["threads"])))
        rows.append({**p, "measured": None if cell is None else cell.get(p["metric"])})
    return rows


#: Every field one pinned row must carry. A pin missing one of these is a wiring error
#: and is reported as such, rather than crashing the gate mid-table.
kPinFields = ("arm", "threads", "metric", "value")


def pin_key(pin: dict) -> str:
    """@brief The human-readable identity of one pinned row."""
    return f"{pin.get('arm', '?')}/T{pin.get('threads', '?')}/{pin.get('metric', '?')}"


def unpinnable(pins: dict) -> list[str]:
    """@brief Pins this gate refuses to read, and why — never silently ignored.

    Two kinds. A malformed pin (a missing field) is a wiring error. A pin naming a column
    outside `GATEABLE` is the one that matters: it is what makes the exclusion above a
    MECHANISM. A future edit that pins a throughput cell or an escape high-water does not
    quietly acquire a flaky gate; it fails here, naming the measured reason the column is
    out of scope.
    """
    out = []
    for p in pins["pins"]:
        missing = [f for f in kPinFields if f not in p]
        if missing:
            out.append(f"{pin_key(p)} is missing {', '.join(missing)}")
        elif p["metric"] not in GATEABLE:
            out.append(f"{pin_key(p)} names {p['metric']!r}, which is not a gateable "
                       f"column (gateable: {', '.join(GATEABLE)})")
    return out


def _cmd_emit(args: argparse.Namespace) -> int:
    lines, why = read_transcript(args.sweep_raw)
    if not lines:
        print(f"::notice::store_sweep_gate: no sweep transcript to record ({why})")
        return 0
    lat, _ls, _tp, _ts, hwm, _chan, _esc = sweep.parse(lines)
    points = series(hwm_totals(hwm), lat_best(lat))
    if not points:
        print("::notice::store_sweep_gate: the sweep transcript carries no gateable rows "
              "— its series is not recorded this commit")
        return 0

    merge = pathlib.Path(args.merge)
    existing = json.loads(merge.read_text()) if merge.exists() else []
    have = {m["name"] for m in existing}
    fresh = [m for m in points if m["name"] not in have]
    merge.write_text(json.dumps(existing + fresh, indent=2) + "\n")
    print(f"store_sweep_gate: merged {len(fresh)} deterministic metrics into {merge} "
          f"({len(existing)} already there)")
    for m in fresh:
        print(f"  {m['name']:<44} {m['value']:>12} {m['unit']}")
    for family, n in dropped(lines).items():
        if n:
            print(f"  dropped {n} {family} row(s) — NEVER banked: {EXCLUDED[family]}")
    return 0


def _emit_pins(totals: dict[tuple[str, int], dict[str, int]], pins: dict,
               toolchain: str) -> int:
    """@brief Print the measured pin file — the re-pin helper, and the Stage-2 path."""
    fresh = [{"arm": arm, "threads": T, "metric": metric, "value": cell[metric]}
             for (arm, T), cell in sorted(totals.items())
             for metric in GATEABLE if arm != kNoStoreArm]
    print(json.dumps({**pins, "toolchain": toolchain, "pins": fresh}, indent=2))
    return 0


def _cmd_gate(args: argparse.Namespace) -> int:
    pins = json.loads(pathlib.Path(args.pins).read_text())
    lines, why = read_transcript(args.sweep_raw)
    toolchain = host_guard.compiler_identity(args.cxx)
    lat, _ls, _tp, _ts, hwm, chan, _esc = sweep.parse(lines) if lines else ({}, {}, {}, {},
                                                                           {}, {}, {})
    totals = hwm_totals(hwm)

    if args.emit_pins:
        if not totals:
            print(f"store_sweep_gate: cannot re-pin — {why or 'no RESULT_STORE_HWM rows'}",
                  file=sys.stderr)
            return 2
        return _emit_pins(totals, pins, toolchain)

    mode, downgrade = ram_census.effective_mode(pins, args.mode, toolchain)
    print("STORE_SWEEP_GATE\tinstrument=bench_store_sweep hwm\t"
          f"mode={mode}\tband={pins['band_bytes']}B/{pins['band_pct']}%\t"
          f"toolchain={toolchain}")
    for family, n in dropped(lines).items():
        state = (f"{n} row(s) read and NOT gated" if n else
                 "not present in this transcript (the deterministic scope does not run it)")
        print(f"  excluded [{family}]: {state} — {EXCLUDED[family]}")
    if downgrade:
        print(f"::notice::store_sweep_gate: enforcing in warn mode — {downgrade}")

    # --- instrument failures: hard in EITHER mode -------------------------------
    if not totals:
        print(f"STORE_SWEEP_GATE\tFAIL\treason=no-measurement ({why or 'no hwm rows'})")
        print("  A failure to measure is not a budget verdict. Warn mode does not")
        print("  answer for a sweep that did not run.")
        return 1
    bad_cal = calibration_verdict(lines)
    if bad_cal:
        print(f"STORE_SWEEP_GATE\tFAIL\treason=calibration\n  {bad_cal}")
        return 1
    faults = [f"{arm}/T{T} overflowed {cell['overflow']} time(s)"
              for (arm, T), cell in sorted(totals.items()) if cell["overflow"]]
    if refusals(chan):
        faults.append(f"{refusals(chan)} channel refusal(s) — a store turned a draw away, "
                      f"so the occupancy below is a floor, not the workload's cost")
    if faults:
        print("STORE_SWEEP_GATE\tFAIL\treason=store-fault")
        for f in faults:
            print(f"  - {f}")
        return 1
    refused = unpinnable(pins)
    if refused:
        print("STORE_SWEEP_GATE\tFAIL\treason=ungateable-pin")
        for r in refused:
            print(f"  - {r}")
        print("  The T-sweep and the escape high-water are excluded by measurement, not")
        print("  by oversight — see EXCLUDED in bench/store_sweep_gate.py.")
        return 1

    rows = measured_pin_rows(totals, pins)
    missing = [r for r in rows if r["measured"] is None]
    if missing:
        print("STORE_SWEEP_GATE\tFAIL\treason=pin-not-measured")
        for r in missing:
            print(f"  - {pin_key(r)} is pinned but absent from the transcript")
        print("  A pin the run does not produce is a dead pin: either an arm or a thread")
        print("  count left the sweep, or the bench stopped reporting it. Fix the run or")
        print("  drop the pin in a PR.")
        return 1

    # --- the drift verdict ------------------------------------------------------
    drifted: list[str] = []
    table: list[str] = []
    for r in rows:
        got, want, band = r["measured"], r["value"], ram_census.band_for(r, pins)
        delta = got - want
        mark = "ok" if abs(delta) <= band else ("GREW" if delta > 0 else "SHRANK")
        table.append(f"  {pin_key(r)}\t{got}\tpinned {want}\t{delta:+d}\t"
                     f"(band ±{band})\t{mark}")
        if mark == "GREW":
            drifted.append(f"{pin_key(r)} grew {delta} ({want} -> {got}), past the "
                           f"±{band} band")
        elif mark == "SHRANK":
            drifted.append(f"{pin_key(r)} shrank {-delta} ({want} -> {got}) — an "
                           f"improvement: RE-PIN to {got}")
    print("\n".join(table))
    print(f"  (latency cells banked, never pinned: {len(lat_best(lat))} — a timed column "
          f"pinned to an exact value reds on the host; store_guard.py drift watches them)")

    if not drifted:
        print("STORE_SWEEP_GATE\tPASS")
        return 0

    for d in drifted:
        print(f"::warning::ADR-0079 store occupancy drift — {d}")
    print(f"STORE_SWEEP_GATE\t{'FAIL' if mode == 'fail' else 'WARN'}")
    for d in drifted:
        print(f"  - {d}")
    print("  Re-pin only with a deliberate PR edit of the pin file (--emit-pins prints the")
    print("  measured values). This gate is never auto-ratcheted: a pin that moves itself")
    print("  records drift instead of catching it. ADR-0079 Stage 2 (#843) is the one step")
    print("  already priced — see `stage2_step` in the pin file.")
    return 1 if mode == "fail" else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    e = sub.add_parser("emit", help="merge the deterministic columns into the chart-store JSON")
    e.add_argument("--sweep-raw", help="run_store_sweep.sh transcript")
    e.add_argument("--merge", default=str(HERE / "benchmark_output.json"),
                   help="customSmallerIsBetter JSON to append the sweep metrics to")
    e.set_defaults(func=_cmd_emit)

    g = sub.add_parser("gate", help="warn-first store-occupancy drift check")
    g.add_argument("--sweep-raw", help="run_store_sweep.sh transcript")
    g.add_argument("--pins", default=str(HERE / "store_sweep_pins.json"))
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
