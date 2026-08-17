#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Validity guard for the BANKED bench-local series (#1301).

`host_guard.py` guards the conditions one sample is taken under — quiescence, the
A/A null pair, the host and compiler identity stamped onto every point. It has
nothing to say about the *series* those points form, and that is where the #1173
investigation found the two remaining ways a regression hides:

  * **A gap.** `perf-local.yml` fires on push-to-`main` behind a `paths:` filter, and
    a run that never happens — filtered out, cancelled while queued because GitHub
    keeps only one queued run per concurrency group, failed, or skipped by the
    quiescence guard — leaves a hole nothing notices. Sixteen merges once passed with
    no banked point, which is how the compact-forward +41% step became a *range*
    rather than a commit.
  * **A staircase.** benchmark-action's `alert-threshold` compares each point with the
    one immediately before it. Five merges of +4% each never trip a 115% bar, and the
    series is 22% slower with no alert ever raised.

Three instruments, one per defect, in the order the workflow uses them:

  * `density` — how many bench-relevant `main` commits sit between HEAD and the last
    TRUSTED banked point, and whether that exceeds the guaranteed bound. Contaminated
    points are not coverage: a flagged sample answers "what was the machine doing",
    not "what did the code cost", so counting it as a sample would let the guarantee
    be satisfied by exactly the points it exists to distrust.
  * `drift` — the soft alert benchmark-action's comparator cannot express: each metric
    against a rolling baseline over the last N trusted points, so a staircase totals
    up instead of resetting at every step. A breach must additionally fall outside
    the window's whole `[min..max]` range, because a best-of-window baseline is a
    floor and a bar measured off a floor otherwise fires on ordinary spread.
  * `toolchain` — the read side of the compiler stamp. The write side landed with
    `host_guard.py stamp --compiler`; nothing yet *reads* it, so a toolchain bump and
    a code change still look identical to anyone browsing the chart. This prints the
    transitions, and `drift` marks any alert whose baseline window crosses one.

**The estimator rule is inherited, not restated.** `docs/methodology.md` makes
best-of-rounds normative because contamination is one-sided — a neighbour can only
make a round slower — and a median merely counts dirty rounds instead of rejecting
them. The same argument applies across *points* of a fixed-host series, which is why
the rolling baseline here is the arm's BEST (min for smaller-is-better, max for
bigger-is-better) over the window and never its median. A density guarantee that
banked more points but compared them with a median-of-window baseline would buy
nothing: it would raise the sample count and lower the trust per sample.

Everything here reads the store; nothing here measures. No wall clock is consulted,
so every rule below is testable off a fixture (`test_store_guard.py`). Stdlib only,
like every other bench tool.

    python3 bench/store_guard.py density   --data data.js --shas-file main.txt --max-gap 8
    python3 bench/store_guard.py drift     --data data.js --window 20 --threshold 15
    python3 bench/store_guard.py toolchain --data data.js
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# The contamination predicate lives with the guard that WRITES it (#1236), exactly as
# the renderer does it: one definition of "suspect", imported by every consumer.
import host_guard  # noqa: E402

# Default bound on the merge gap. Eight bench-relevant merges is the width a bisect
# closes in three builds; the observed hole that motivated this was sixteen.
DEFAULT_MAX_GAP = 8

# Default rolling-baseline window, in trusted points. Twenty points is a few days of
# merge traffic on this repo — long enough that a staircase accumulates inside it,
# short enough that a deliberate, accepted step-change ages out instead of alerting
# forever.
DEFAULT_WINDOW = 20

# Default soft-alert bar, percent worse than the rolling baseline. Matches the
# `alert-threshold: '115%'` this comparator sits beside, so the two differ in what
# they compare against and in nothing else.
DEFAULT_THRESHOLD = 15.0

# A metric needs this many baseline points before it is judged at all. Below it the
# "best of the window" is one or two samples, which is a point-to-point comparison
# wearing a different name.
DEFAULT_MIN_POINTS = 5

# A field of the `extra` descriptor that reads as "<name> <major.minor.patch>" is the
# compiler identity `host_guard.compiler_identity` wrote. The host's other fields do
# not match: the kernel field carries no space, "31 cpus" and "governor not-exposed"
# carry no dotted version, and the CPU model carries neither.
_COMPILER_FIELD = re.compile(r"^(?P<name>\S.*\S)\s+(?P<ver>\d+\.\d+\.\d+)$")

# The suites benchmark-action writes into one store, told apart by the direction that
# counts as WORSE. The store names them in prose, so the direction is read out of the
# name rather than configured twice.
_BIGGER_IS_BETTER = "bigger is better"


def parse_data_js(text: str) -> dict:
    """@brief Parse a benchmark-action `data.js` into its JSON document.

    The file is a JavaScript assignment (`window.BENCHMARK_DATA = {...}`), not JSON,
    so the object is sliced out of it. Raises `ValueError` on anything that is not
    that shape — a truncated or half-fetched store must fail loudly here rather than
    read as an empty series, which would silently answer "no drift, no gap".
    """
    start = text.find("{")
    if start < 0:
        raise ValueError("no JSON object in data.js")
    return json.loads(text[start:].rstrip().rstrip(";").rstrip())


def suite_is_bigger_better(suite: str) -> bool:
    """@brief Does a larger value mean a FASTER result in this suite?"""
    return _BIGGER_IS_BETTER in suite.lower()


def trusted(entries: list[dict], known: dict[str, str] | None = None) -> list[dict]:
    """@brief The entries a verdict may be built on, in store order.

    Drops every sample flagged at measure time or listed retroactively. This is the
    single place the two guards meet: `host_guard` decides WHAT is suspect, and this
    decides that suspect points are neither coverage nor baseline.
    """
    known = host_guard.load_known_contaminated() if known is None else known
    return [e for e in entries if not host_guard.entry_contaminated(e, known)]


def entry_sha(entry: dict) -> str:
    """@brief The commit a stored entry was measured at."""
    return str((entry.get("commit") or {}).get("id", ""))


def merge_gap(banked: set[str], candidates: list[str]) -> tuple[int, str | None]:
    """@brief How far the newest trusted point lags a NEWEST-FIRST commit list.

    @p candidates is the bench-relevant history of `main`, newest first; @p banked is
    the set of shas carrying a trusted point. Returns (gap, first_covered_sha) where
    the gap counts the commits ahead of that point. A history with no trusted point at
    all reports the full length and `None`, which is the correct "measure now".
    """
    for i, sha in enumerate(candidates):
        if sha in banked:
            return i, sha
    return len(candidates), None


def density_report(doc: dict, candidates: list[str], suite_substr: str = "",
                   known: dict[str, str] | None = None) -> dict:
    """@brief The gap between HEAD and the last trusted point, per suite and overall.

    The overall gap is the WORST suite's: the latency and throughput stores are banked
    by the same step, so a suite that is behind on its own is an instrument fault, and
    hiding it behind a healthier sibling is how a half-recorded point reads as
    coverage.
    """
    known = host_guard.load_known_contaminated() if known is None else known
    per_suite: dict[str, dict] = {}
    for suite, entries in (doc.get("entries") or {}).items():
        if suite_substr and suite_substr.lower() not in suite.lower():
            continue
        ok = trusted(entries, known)
        gap, at = merge_gap({entry_sha(e) for e in ok}, candidates)
        per_suite[suite] = {"gap": gap, "at": at, "trusted": len(ok),
                            "banked": len(entries)}
    worst = max((s["gap"] for s in per_suite.values()), default=len(candidates))
    return {"gap": worst, "suites": per_suite, "candidates": len(candidates)}


def compiler_of(extra: str | None) -> str | None:
    """@brief The compiler identity stamped into a point's `extra`, if any.

    Returns None for the points banked before `--compiler` shipped, which is a real
    answer and not a defect: those samples genuinely do not record their toolchain,
    and pretending otherwise would let a bump hide inside them.
    """
    if not extra:
        return None
    for field in extra.split(host_guard.SEP):
        field = field.strip()
        if not field or host_guard.CONTAM_TOKEN in field:
            continue
        m = _COMPILER_FIELD.match(field)
        if m:
            return f"{m.group('name')} {m.group('ver')}"
    return None


def entry_compiler(entry: dict) -> str | None:
    """@brief The compiler identity of a stored entry, read off its first point."""
    for b in entry.get("benches") or []:
        c = compiler_of(b.get("extra"))
        if c:
            return c
    return None


def toolchain_timeline(entries: list[dict]) -> list[dict]:
    """@brief Every point at which the recorded compiler identity changed.

    A transition from None is a *first* recording, not a bump, and is reported as
    such: the store predates the stamp there, so nothing changed that anyone can see.
    """
    out: list[dict] = []
    prev: str | None = None
    seen = False
    for e in entries:
        cur = entry_compiler(e)
        if cur is None:
            continue
        if seen and cur != prev:
            out.append({"sha": entry_sha(e), "from": prev, "to": cur})
        prev, seen = cur, True
    return out


def _window_best(values: list[float], bigger_is_better: bool) -> float:
    """@brief The window's BEST value — the estimator `docs/methodology.md` mandates.

    Never a median. Contamination is one-sided across points of a fixed-host series
    for the same reason it is across rounds of one measurement: a neighbour can only
    make a point slower. The best of the window therefore rejects the dirty points,
    while a median counts them and drifts upward with the noise the guard exists to
    reject.
    """
    return max(values) if bigger_is_better else min(values)


def _window_worst(values: list[float], bigger_is_better: bool) -> float:
    """@brief The window's WORST value — the range guard's other end.

    Paired with `_window_best`: together they are the `[min..max]` range
    `docs/methodology.md` reads as the contamination diagnostic, used here to demand
    that a breach fall OUTSIDE everything the baseline window already contains.
    """
    return min(values) if bigger_is_better else max(values)


def _worse_pct(current: float, baseline: float, bigger_is_better: bool) -> float:
    """@brief How much WORSE @p current is than @p baseline, in percent (may be < 0)."""
    if baseline <= 0:
        return 0.0
    return ((baseline - current) if bigger_is_better else (current - baseline)) \
        / baseline * 100.0


def drift_alerts(entries: list[dict], bigger_is_better: bool,
                 window: int = DEFAULT_WINDOW, threshold: float = DEFAULT_THRESHOLD,
                 min_points: int = DEFAULT_MIN_POINTS,
                 known: dict[str, str] | None = None,
                 range_guard: bool = True) -> dict:
    """@brief Compare the newest trusted point with a rolling best-of-window baseline.

    Returns a report carrying, per breaching metric, BOTH the move against the
    baseline and the move against the immediately previous point. The second number
    is what benchmark-action's comparator sees, and printing them side by side is the
    whole argument for this tool: a staircase shows as a large baseline delta beside a
    small point-to-point one.

    A metric absent from the newest point, or with fewer than @p min_points in the
    window, is not judged — a new series has no history to drift from.

    **The range guard is what makes the best-of-window baseline reportable.** A best
    is a *floor*: measured against it, a metric with any run-to-run spread breaches a
    percentage bar routinely, and an alert that fires on nothing trains the reader to
    ignore it. Against the real store this rule is the difference between 59 alerts
    and 2. So a breach must also land outside the window's ENTIRE `[min..max]` range —
    the same disjoint-ranges criterion `docs/methodology.md` already requires before
    an A/B may be believed. A staircase still fires (each step is worse than every
    point behind it); spread inside the historical range does not.
    """
    ok = trusted(entries, known)
    if len(ok) < min_points + 1:
        return {"status": "insufficient", "trusted": len(ok), "alerts": [],
                "toolchain": []}
    current, history = ok[-1], ok[-(window + 1):-1]
    cur_vals = {b.get("name"): b.get("value") for b in current.get("benches") or []}
    prev_vals = {b.get("name"): b.get("value") for b in history[-1].get("benches") or []}
    crossings = toolchain_timeline(history + [current])

    alerts = []
    for name, value in cur_vals.items():
        if not isinstance(value, (int, float)):
            continue
        past = [b["value"] for e in history for b in e.get("benches") or []
                if b.get("name") == name and isinstance(b.get("value"), (int, float))]
        if len(past) < min_points:
            continue
        baseline = _window_best(past, bigger_is_better)
        pct = _worse_pct(value, baseline, bigger_is_better)
        if pct <= threshold:
            continue
        worst = _window_worst(past, bigger_is_better)
        if range_guard and _worse_pct(value, worst, bigger_is_better) <= 0.0:
            continue
        prev = prev_vals.get(name)
        alerts.append({
            "name": name,
            "value": value,
            "baseline": baseline,
            "window_worst": worst,
            "pct": pct,
            "pct_vs_prev": _worse_pct(value, prev, bigger_is_better)
            if isinstance(prev, (int, float)) else None,
            "points": len(past),
        })
    alerts.sort(key=lambda a: -a["pct"])
    return {"status": "ok", "trusted": len(ok), "sha": entry_sha(current),
            "window": len(history), "alerts": alerts, "toolchain": crossings}


def _fmt_prev(alert: dict) -> str:
    """@brief The point-to-point figure a breach would have shown, spoken."""
    p = alert.get("pct_vs_prev")
    return "n/a" if p is None else f"{p:+.1f}%"


def _load_doc(path: str) -> dict:
    return parse_data_js(pathlib.Path(path).read_text())


def _gh_output(**kv: object) -> None:
    """@brief Publish step outputs when running under Actions; a no-op elsewhere."""
    out = os.environ.get("GITHUB_OUTPUT")
    if not out:
        return
    with open(out, "a", encoding="utf-8") as fh:
        for k, v in kv.items():
            fh.write(f"{k}={v}\n")


def _summary(lines: list[str]) -> None:
    """@brief Append to the job summary when running under Actions."""
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


def _cmd_density(args: argparse.Namespace) -> int:
    candidates = [ln.strip() for ln in
                  pathlib.Path(args.shas_file).read_text().splitlines() if ln.strip()]
    try:
        doc = _load_doc(args.data)
    except (OSError, ValueError) as exc:
        # No readable store is the widest possible gap, not an error: the first run
        # after the store is created must measure, not fail.
        print(f"::notice::store unreadable ({exc}) — treating the gap as unbounded")
        doc = {"entries": {}}
    rep = density_report(doc, candidates, args.suite_substr)
    due = rep["gap"] > args.max_gap
    for suite, s in sorted(rep["suites"].items()):
        print(f"  [{suite[:44]}] gap {s['gap']} · last trusted "
              f"{(s['at'] or 'none')[:8]} · {s['trusted']}/{s['banked']} trusted")
    verdict = ("catch-up DUE" if due else "within the guarantee")
    print(f"store_guard: worst gap {rep['gap']} bench-relevant commits "
          f"(bound {args.max_gap}) — {verdict}")
    _summary(["### bench-local sample density",
              f"Worst gap **{rep['gap']}** bench-relevant `main` commits behind HEAD "
              f"(bound {args.max_gap}) — {verdict}."])
    _gh_output(due="true" if due else "false", gap=rep["gap"])
    return 1 if (due and args.mode == "fail") else 0


def _cmd_drift(args: argparse.Namespace) -> int:
    doc = _load_doc(args.data)
    breaches, lines = 0, ["### bench-local rolling-baseline drift"]
    for suite, entries in (doc.get("entries") or {}).items():
        if args.suite_substr and args.suite_substr.lower() not in suite.lower():
            continue
        rep = drift_alerts(entries, suite_is_bigger_better(suite), args.window,
                           args.threshold, args.min_points,
                           range_guard=not args.no_range_guard)
        if rep["status"] != "ok":
            print(f"store_guard: [{suite[:44]}] only {rep['trusted']} trusted points "
                  f"— no baseline yet")
            continue
        crossed = bool(rep["toolchain"])
        head = (f"[{suite[:44]}] {len(rep['alerts'])} metric(s) worse than the "
                f"best of the last {rep['window']} trusted points by >"
                f"{args.threshold:.0f}%")
        print(f"store_guard: {head}")
        for c in rep["toolchain"]:
            print(f"  toolchain change inside the window at {c['sha'][:8]}: "
                  f"{c['from']} -> {c['to']}")
        for a in rep["alerts"][: args.top]:
            print(f"  {a['name']}: {a['value']:g} vs baseline {a['baseline']:g} "
                  f"(window {a['baseline']:g}..{a['window_worst']:g}, "
                  f"{a['pct']:+.1f}% over {a['points']} pts; "
                  f"point-to-point {_fmt_prev(a)})")
        if rep["alerts"]:
            breaches += len(rep["alerts"])
            worst = rep["alerts"][0]
            note = (" The baseline window crosses a toolchain change, so this is not "
                    "attributable to code." if crossed else "")
            print(f"::warning::bench-local rolling drift — {len(rep['alerts'])} "
                  f"metric(s) in '{suite}' are >{args.threshold:.0f}% worse than the "
                  f"best of the last {rep['window']} trusted points; worst "
                  f"{worst['name']} {worst['pct']:+.1f}% (point-to-point "
                  f"{_fmt_prev(worst)}).{note}")
            lines.append(f"- `{suite}`: **{len(rep['alerts'])}** metric(s) drifted; "
                         f"worst `{worst['name']}` {worst['pct']:+.1f}% vs baseline, "
                         f"{_fmt_prev(worst)} vs the previous point.{note}")
        else:
            lines.append(f"- `{suite}`: clean against the best of "
                         f"{rep['window']} trusted points.")
    _summary(lines)
    _gh_output(breaches=breaches)
    if breaches and args.mode == "fail":
        print("::error::rolling-baseline drift, and this leg is in fail mode")
        return 1
    return 0


def _cmd_toolchain(args: argparse.Namespace) -> int:
    doc = _load_doc(args.data)
    lines, total = ["### bench-local toolchain identity"], 0
    for suite, entries in sorted((doc.get("entries") or {}).items()):
        ok = trusted(entries)
        latest = next((entry_compiler(e) for e in reversed(ok)
                       if entry_compiler(e)), None)
        unstamped = sum(1 for e in ok if entry_compiler(e) is None)
        changes = toolchain_timeline(ok)
        total += len(changes)
        print(f"  [{suite[:44]}] compiler now {latest or 'unrecorded'} · "
              f"{unstamped}/{len(ok)} trusted points unstamped · "
              f"{len(changes)} change(s)")
        for c in changes:
            print(f"    {c['sha'][:8]}: {c['from']} -> {c['to']}")
        lines.append(f"- `{suite}`: **{latest or 'unrecorded'}**, "
                     f"{len(changes)} recorded toolchain change(s), "
                     f"{unstamped} unstamped point(s).")
    print(f"store_guard: {total} toolchain transition(s) across the store")
    _summary(lines)
    _gh_output(changes=total)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("density", help="how far the store lags main, in commits")
    d.add_argument("--data", required=True)
    d.add_argument("--shas-file", required=True,
                   help="bench-relevant main commits, NEWEST first, one sha per line")
    d.add_argument("--max-gap", type=int, default=DEFAULT_MAX_GAP)
    d.add_argument("--suite-substr", default="")
    d.add_argument("--mode", choices=("report", "fail"), default="report")
    d.set_defaults(fn=_cmd_density)

    r = sub.add_parser("drift", help="newest point vs a rolling best-of-window baseline")
    r.add_argument("--data", required=True)
    r.add_argument("--window", type=int, default=DEFAULT_WINDOW)
    r.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)
    r.add_argument("--min-points", type=int, default=DEFAULT_MIN_POINTS)
    r.add_argument("--suite-substr", default="")
    r.add_argument("--top", type=int, default=10)
    r.add_argument("--no-range-guard", action="store_true",
                   help="report every percentage breach, including those inside the "
                        "baseline window's own range (diagnostic only — on the real "
                        "store this is ~30x noisier)")
    r.add_argument("--mode", choices=("warn", "fail"), default="warn")
    r.set_defaults(fn=_cmd_drift)

    t = sub.add_parser("toolchain", help="surface the recorded compiler identity")
    t.add_argument("--data", required=True)
    t.set_defaults(fn=_cmd_toolchain)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
