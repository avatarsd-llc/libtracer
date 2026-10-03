#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Sustained-step detector over the bench-local store (#1770).

A row can step and stay there without anyone noticing: `lkv-store-heap 1024B` doubled on
2026-10-01 (27 -> 54 ns on bench-local) and shipped in v0.17.0. `store_guard.py drift`
compares only the NEWEST point with a rolling baseline, so a step that landed a few points
ago and held is no longer news to it. This module walks every row's whole history and
names each place the row moved to a new level and STAYED there.

**The decision rule.** A row has a step at point *i* when each of the `HOLD_POINTS` points
starting at *i* sits beyond the row's threshold, on the same side of the baseline. The
baseline is the median of up to `BASELINE_POINTS` points before *i* (never reaching back
past the previous step). The threshold is per row:

    threshold = max(STEP_FLOOR, STEP_SIGMAS * spread, STEP_TICKS * resolution)

where `spread` is the row's own robust point-to-point spread and `resolution` its clock
tick (both below, both relative to the row's level). One global
percentage cannot serve every row: bench-local's point-to-point noise runs from about 1%
to about 9% depending on the row family, so a bar low enough to catch a 15% step on a
quiet row fires on every third point of a noisy one.

- **Why every hold point must agree.** A single contaminated sample (the pinned host is a
  shared workstation, #1236) moves one point, not three. `cff17dee` read `lkv-store-pool
  64B` at 18 ns between neighbours at 11 ns; that is one point and is not a step.
- **Why contaminated points are scanned, not skipped.** The charts omit them, but the
  2026-10-01 1 KiB step landed during a contended stretch: with contaminated points
  removed only two clean points remain after it, and a step that cannot be seen until the
  host is quiet for three runs is a step seen too late. The hold rule is what keeps one
  bad sample from posing as a step, so the detector needs no second filter.
- **Why the spread is robust.** It is the median absolute log-ratio of consecutive
  points, scaled to a standard deviation (`/ (0.6745 * sqrt 2)`). Steps are rare, so the
  median ignores them, and it ignores isolated spikes for the same reason.
- **Why a resolution term.** Many rows are clock-quantized (p50 rows in 10 ns ticks,
  sub-10 ns rows in 1 ns ticks). On such a row most consecutive points are equal, the
  median difference is zero, and a row flapping between 120 and 130 ns would read as a
  string of 4-sigma steps. The row's median non-zero move is its tick, and a step must
  span more than `STEP_TICKS` of them.
- **Why the floor.** `STEP_FLOOR` is the host guard's A/A band: two back-to-back runs of
  the SAME binary may disagree by that much before a sample counts as contaminated, so a
  move inside it is a move the instrument cannot attribute to the code.

Point indices are store-entry indices (the charts' x-axis), so a step's index is the first
recorded commit that measured the new level. The merge that caused it lies between that
commit and the recorded point before it; `merge_window` lists the first-parent merges in
that range.

    python3 bench/step_detect.py --data data.js               # every step, oldest first
    python3 bench/step_detect.py --data data.js --recent 20   # the last 20 recorded commits
    python3 bench/step_detect.py --recent 20 --markdown       # fetches gh-pages; job summary

Stdlib only, like every other bench tool. `test_step_detect.py` pins the decision rule.
"""
from __future__ import annotations

import argparse
import math
import os
import pathlib
import re
import statistics
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import host_guard  # noqa: E402

# ---------------------------------------------------------------------------
# the decision rule's named constants
# ---------------------------------------------------------------------------
HOLD_POINTS = 3
"""@brief Consecutive points that must all sit beyond the threshold for a move to be a step."""

BASELINE_POINTS = 7
"""@brief Points before a candidate step whose median is the "before" level."""

MIN_BASELINE_POINTS = 3
"""@brief Fewer points than this before a candidate (since the row began, or since its
previous step) is not a level, so no step is judged there."""

STEP_SIGMAS = 4.0
"""@brief The per-row threshold, in units of the row's own robust point-to-point spread."""

STEP_TICKS = 1.5
"""@brief A step must span more than this many of the row's clock ticks: one tick is never
a step, two can be."""

STEP_FLOOR = host_guard.DEFAULT_BAND / 100.0
"""@brief The smallest relative move ever called a step: the host guard's A/A band."""

MIN_SPREAD_POINTS = 5
"""@brief A row with fewer points than this has no spread to measure and is not scanned."""

RECENT_POINTS = 20
"""@brief The page's "Recent steps" window, in recorded commits."""

PAGE_ROWS = 40
"""@brief At most this many steps are tabled on the page, largest first."""

_BIGGER_IS_BETTER = "bigger is better"


# ---------------------------------------------------------------------------
# the rule
# ---------------------------------------------------------------------------
def row_spread(values: list[float]) -> float | None:
    """@brief A row's robust point-to-point spread, as a relative (log) standard deviation.

    The median absolute log-ratio of consecutive points, scaled to a standard deviation.
    None when the row is too short or carries a non-positive value, which a log-ratio
    cannot express.
    """
    if len(values) < MIN_SPREAD_POINTS or any(v <= 0 for v in values):
        return None
    ds = [abs(math.log(b / a)) for a, b in zip(values, values[1:])]
    return statistics.median(ds) / (0.6745 * math.sqrt(2.0))


def row_resolution(values: list[float]) -> float:
    """@brief A row's clock tick, relative to its median: the median NON-ZERO move.

    On a quantized row this is the tick (10 ns on a p50 row of 130 ns: 7.7%). On a
    continuous row it is about the noise, which `STEP_SIGMAS` already dominates.
    """
    moves = [abs(b - a) for a, b in zip(values, values[1:]) if b != a]
    if len(moves) < MIN_SPREAD_POINTS:
        return 0.0  # a row that almost never moves has no tick to read, only its steps
    return statistics.median(moves) / statistics.median(values)


def row_threshold(values: list[float]) -> float | None:
    """@brief The relative (log) move a step must exceed on this row, or None if unscannable."""
    s = row_spread(values)
    if s is None:
        return None
    return max(STEP_FLOOR, STEP_SIGMAS * s, STEP_TICKS * row_resolution(values))


def find_steps(pts: list[list[float]]) -> list[dict]:
    """@brief Every sustained step in one row.

    @p pts is the row as `[[entry_idx, value], ...]` in store order, contaminated points
    included. Returns `[{"i", "before", "after", "threshold"}]` in order, where `i` is the
    entry index of the first point at the new level, `before` is the baseline median,
    `after` is the median of the new level (up to `BASELINE_POINTS` points, stopping at the
    next step) and `threshold` is the row's relative bar.

    Where the rule holds at adjacent candidates, a first point that is already past the
    bar but still NEARER the old level than the new one is transitional (noise before the
    step, not the step), and the step moves on to the next candidate. Otherwise the
    earliest candidate stands, so a row that keeps drifting after a step does not pull the
    step later.
    """
    vals = [float(v) for _, v in pts]
    thr = row_threshold(vals)
    if thr is None:
        return []

    def judge(j: int, last: int) -> float | None:
        """@brief The baseline at @p j when the rule holds there, else None."""
        lo = max(last, j - BASELINE_POINTS)
        if j - lo < MIN_BASELINE_POINTS or j + HOLD_POINTS > len(vals):
            return None
        base = statistics.median(vals[lo:j])
        moves = [math.log(v / base) for v in vals[j:j + HOLD_POINTS]]
        if all(m > thr for m in moves) or all(m < -thr for m in moves):
            return base
        return None

    found: list[tuple[int, float]] = []  # (position in vals, baseline)
    last, i = 0, 1
    while i + HOLD_POINTS <= len(vals):
        base = judge(i, last)
        if base is None:
            i += 1
            continue
        j = i
        while j + 1 < i + HOLD_POINTS:
            nxt = judge(j + 1, last)
            if nxt is None:
                break
            new_level = statistics.median(vals[j + 1:j + 1 + HOLD_POINTS])
            if abs(math.log(vals[j] / base)) >= abs(math.log(new_level / vals[j])):
                break
            j, base = j + 1, nxt
        found.append((j, base))
        last, i = j, j + HOLD_POINTS

    out = []
    for n, (j, base) in enumerate(found):
        end = found[n + 1][0] if n + 1 < len(found) else len(vals)
        after = statistics.median(vals[j:min(end, j + BASELINE_POINTS)])
        out.append({"i": int(pts[j][0]), "before": base, "after": after, "threshold": thr})
    return out


# ---------------------------------------------------------------------------
# the store
# ---------------------------------------------------------------------------
def suite_is_bigger_better(suite: str) -> bool:
    """@brief Does a larger value mean a faster result in this suite?"""
    return _BIGGER_IS_BETTER in suite.lower()


def rows_of(entries: list[dict]) -> dict[str, dict]:
    """@brief name -> {"pts": [[entry_idx, value], ...], "unit": str} over EVERY entry.

    Contaminated entries are kept on purpose; see the module docstring.
    """
    out: dict[str, dict] = {}
    for i, e in enumerate(entries):
        for b in e.get("benches", []):
            try:
                v = float(b["value"])
                row = out.setdefault(b["name"], {"pts": [], "unit": b.get("unit", "")})
            except (KeyError, TypeError, ValueError):
                continue
            row["pts"].append([i, v])
    return out


def store_steps(data: dict, recent: int | None = None) -> list[dict]:
    """@brief Every step in every row of every suite of a benchmark-action store.

    With @p recent, only steps whose first point is among the last @p recent recorded
    commits of its suite. Each step carries its row, suite, unit, the recorded commit that
    first measured the new level (`sha`), the recorded commit before it (`prev_sha`), and
    whether it made the row slower (`regression`).
    """
    steps: list[dict] = []
    for suite, entries in (data.get("entries") or {}).items():
        if not entries:
            continue
        bigger = suite_is_bigger_better(suite)
        first_recent = 0 if recent is None else max(0, len(entries) - recent)
        shas = [str((e.get("commit") or {}).get("id", "")) for e in entries]
        for name, row in rows_of(entries).items():
            for st in find_steps(row["pts"]):
                if st["i"] < first_recent:
                    continue
                up = st["after"] > st["before"]
                steps.append(dict(st, row=name, suite=suite, unit=row["unit"],
                                  sha=shas[st["i"]],
                                  prev_sha=shas[st["i"] - 1] if st["i"] > 0 else "",
                                  regression=(up != bigger)))
    steps.sort(key=lambda s: (s["i"], s["row"]))
    return steps


def magnitude(step: dict) -> float:
    """@brief How far a step moved, as |log(after / before)| — symmetric in direction."""
    return abs(math.log(step["after"] / step["before"]))


def merge_window(prev_sha: str, sha: str) -> list[tuple[str, str]]:
    """@brief First-parent merges in (prev_sha, sha], newest first, as (sha, PR number or "").

    One of these is the merge that made the step. Returns [] when git or either commit is
    unavailable (a shallow clone, a fork), which the callers render as a plain range.
    """
    if not prev_sha or not sha:
        return []
    try:
        p = subprocess.run(["git", "log", "--first-parent", "--format=%H %s",
                            f"{prev_sha}..{sha}"],
                           capture_output=True, text=True, cwd=REPO, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return []
    if p.returncode != 0:
        return []
    out = []
    for ln in p.stdout.splitlines():
        h, _, subj = ln.partition(" ")
        m = re.match(r"Merge pull request #(\d+)", subj) or re.search(r"\(#(\d+)\)\s*$", subj)
        out.append((h, m.group(1) if m else ""))
    return out


# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------
_GH = "https://github.com/avatarsd-llc/libtracer"


def fmt_value(v: float, unit: str) -> str:
    """@brief A level in the row's own unit, compact (`56 ns`, `1.2 µs`, `17.6 M/s`)."""
    def num(x: float) -> str:
        return f"{x:.0f}" if x >= 100 or x == int(x) else f"{x:.1f}"
    if unit == "ns" and v >= 1e3:
        return f"{v / 1e6:.2f} ms" if v >= 1e6 else f"{v / 1e3:.1f} µs"
    if unit == "bytes" and v >= 1024:
        return f"{v / 1024:.1f} KiB"
    if unit == "deliveries/s":
        return (f"{v / 1e6:.1f} M/s" if v >= 1e6 else f"{v / 1e3:.0f} k/s" if v >= 1e3
                else f"{num(v)}/s")
    if v >= 1e6:
        return f"{v / 1e6:.1f} M {unit}".strip()
    return f"{num(v)} {unit}".strip()


def fmt_move(step: dict, verdict: bool = False) -> str:
    """@brief `28 ns → 56 ns (+100%)`, or `(+100%, worse)` with @p verdict."""
    pct = (step["after"] / step["before"] - 1.0) * 100.0
    tail = (", worse" if step["regression"] else ", better") if verdict else ""
    return (f"{fmt_value(step['before'], step['unit'])} → {fmt_value(step['after'], step['unit'])}"
            f" ({pct:+.0f}%{tail})")


def _merges_md(step: dict) -> str:
    """@brief The window's merges as PR links, then the commit range as a compare link."""
    prev, sha = step["prev_sha"], step["sha"]
    merges = merge_window(prev, sha)
    rng = f"[`{prev[:7]}..{sha[:7]}`]({_GH}/compare/{prev}...{sha})"
    if not merges:
        return rng if prev else "—"
    prs = [f"[#{n}]({_GH}/pull/{n})" for _, n in merges if n]
    shown = ", ".join(prs[:4]) + (f" +{len(prs) - 4}" if len(prs) > 4 else "")
    return f"{shown or str(len(merges)) + ' commits'} · {rng}"


def markdown_table(steps: list[dict], limit: int | None = None) -> str:
    """@brief The steps as a Markdown table: row, before → after, first commit, merges."""
    rows = steps if limit is None else steps[:limit]
    lines = ["| Row | Before → after | First commit | Merges in the window |",
             "|---|---|---|---|"]
    for s in rows:
        lines.append(f"| `{s['row']}` | {fmt_move(s, verdict=True)} "
                     f"| [`{s['sha'][:7]}`]({_GH}/commit/{s['sha']}) | {_merges_md(s)} |")
    return "\n".join(lines)


def page_block(local: dict | None) -> str:
    """@brief The Performance page's "Recent steps" body, from the bench-local store.

    The largest `PAGE_ROWS` steps over the last `RECENT_POINTS` recorded commits, largest
    first: one code change often moves dozens of rows at once (every metric of every size
    of one bench), and ordering by size keeps the one that doubled a row at the top
    instead of behind forty rows that moved by a fifth.
    """
    rule = (f"A **step** is a row that moves beyond its own threshold and holds there for "
            f"{HOLD_POINTS} consecutive recorded points. The threshold is per row: "
            f"{STEP_SIGMAS:g}× the row's robust point-to-point spread, more than one tick of "
            f"its clock, and never below {STEP_FLOOR:.0%}, the host guard's A/A band. "
            f"bench-local's noise runs from about 1% to about 9% by row family, so one global "
            f"percentage would either miss steps on quiet rows or fire on noisy ones. A single "
            f"contaminated sample moves one point and is never a step. Each step is marked on "
            f"its chart with ▲ (up) or ▼ (down) at the first commit that measured the new "
            f"level; the merge that caused it is in the window between that commit and the "
            f"recorded point before it. Rule and constants: `bench/step_detect.py`, which "
            f"also prints the full list (`python3 bench/step_detect.py --recent "
            f"{RECENT_POINTS}`).")
    if not local:
        return rule + "\n\n_(the bench-local store was not reachable in this build)_"
    steps = store_steps(local, recent=RECENT_POINTS)
    if not steps:
        return rule + f"\n\n_No step in the last {RECENT_POINTS} recorded commits._"
    steps.sort(key=lambda s: (-magnitude(s), -s["i"], s["row"]))
    shown = min(len(steps), PAGE_ROWS)
    head = (f"{len(steps)} steps in the last {RECENT_POINTS} recorded commits on "
            f"bench-local; the {shown} largest, largest first:" if shown < len(steps) else
            f"{len(steps)} steps in the last {RECENT_POINTS} recorded commits on "
            f"bench-local, largest first:")
    return f"{rule}\n\n{head}\n\n{markdown_table(steps, PAGE_ROWS)}"


def text_report(steps: list[dict]) -> str:
    """@brief The steps as plain text, one per line, oldest first (the CLI's default)."""
    if not steps:
        return "no sustained steps"
    out = []
    for s in steps:
        out.append(f"{s['sha'][:7]} (after {s['prev_sha'][:7] or '-'})  "
                   f"{'SLOWER' if s['regression'] else 'faster'}  {fmt_move(s)}  "
                   f"[bar {s['threshold']:.0%}]  {s['row']}")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def load_store(path: str | None) -> dict | None:
    """@brief A benchmark-action store from @p path, else bench-local from `origin/gh-pages`."""
    import json
    raw = None
    if path:
        raw = pathlib.Path(path).read_text()
    else:
        subprocess.run(["git", "fetch", "--no-tags", "--depth=1", "origin", "gh-pages"],
                       capture_output=True, cwd=REPO)
        p = subprocess.run(["git", "show", "FETCH_HEAD:dev/bench-local/data.js"],
                           capture_output=True, text=True, cwd=REPO)
        raw = p.stdout if p.returncode == 0 else None
    if not raw:
        return None
    start = raw.find("{")
    return json.loads(raw[start:].rstrip().rstrip(";").rstrip()) if start >= 0 else None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--data",
                    help="a benchmark-action data.js (default: bench-local from gh-pages)")
    ap.add_argument("--recent", type=int, default=None,
                    help="only steps among the last N recorded commits")
    ap.add_argument("--markdown", action="store_true",
                    help="print a Markdown table (also appended to the Actions job summary)")
    args = ap.parse_args(argv)
    data = load_store(args.data)
    if data is None:
        print("step_detect: no store to read", file=sys.stderr)
        return 2
    steps = store_steps(data, recent=args.recent)
    if not args.markdown:
        print(text_report(steps))
        return 0
    steps.sort(key=lambda s: (-s["i"], -magnitude(s), s["row"]))
    window = f"the last {args.recent} recorded commits" if args.recent else "the whole store"
    md = (f"### Sustained steps — {window}\n\n" +
          (markdown_table(steps) if steps else "No sustained step."))
    print(md)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(md + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
