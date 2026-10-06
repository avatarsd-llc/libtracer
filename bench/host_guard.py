#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Quiescence guard for the pinned bench host (#1236).

`perf-local.yml` banks one absolute-trend point per push onto a machine that is a
shared workstation, not a lab instrument: `taskset` pins the logical CPU but pins
nothing else a neighbour competes for — memory bandwidth, LLC, boost headroom. Until
this guard existed the sample was banked unconditionally, so a parallel build landed
in the series as a code regression. Sample 141 (`3b28d15b`) carries 104 of its 386
metrics more than 10% slow against its own neighbours; the machine was measured, not
the commit.

That was a bent trend chart while the pinned store was advisory. It stops being
merely cosmetic under the two-tier gate policy, where this host becomes the BLOCKING
tier at a ~5% bar: a contaminated point there would not mislead a reader, it would
fail a PR. The consumer side of that is built (`perf_gate.py --sample-note` refuses to
render a blocking verdict on a flagged sample); the pinned-host gate leg that would
call it is not, so today these verdicts still only guard the store. Hence three
instruments, in the order the workflow uses them:

  * `wait` — hold until the machine goes quiet, and say so if it never does. NOT a
    bare pre-flight check: the workflow's own `cmake --build -j31` immediately
    precedes the measurement and pushes the 1-minute load average far above any
    sane bar, so a check would refuse every run and a guard that always refuses is
    a guard that gets deleted. Waiting is what makes the bar enforceable.
  * `bracket` — the A/A null pair, run before and after the measured transcript. The
    two runs use the SAME binary, which is what makes it a host instrument rather
    than a code one: with the text layout held constant the only thing left to move
    the number is the machine. (A cross-BUILD comparison could not serve here.
    Layout sensitivity was measured on this host as metric-specific, not a general
    property — `compact-forward` 1.8%, `fwd-demux-fixed` ~0%, `fold-b4` ~15% across
    placements of identical source — so a pair drawn from two builds cannot separate
    a busy neighbour from a relinked function.)
  * `stamp` — write the host descriptor, the COMPILER IDENTITY, the CLOCK FLOOR (#1804)
    and any contamination verdict onto every emitted point, so the store records the
    conditions a number was taken under and not just the number.

Two rules run through all of it. A busy host SKIPS its sample and never fails the
job: refusing to measure is a correct outcome, and a red job trains the reader to
ignore red. And a suspect sample is FLAGGED, never deleted — the raw datum stays in
the store where it can be re-examined, while the charts and any gating consumer stop
believing it (`is_contaminated`, and `contaminated_samples.json` for the points that
predate the guard). Gating consumers keep that whole-sample verdict; the charts read
`untrusted_cells` instead, which hides only a flagged sample's untrusted rows (#1890).

Stdlib only, like every other bench tool here.

    python3 bench/host_guard.py wait     --max-load-per-cpu 0.25 --timeout 600
    python3 bench/host_guard.py bracket  --pre pre.txt --post post.txt --band 6 \
                                         --rows-out aa_rows.json
    python3 bench/host_guard.py stamp    --json a.json --json b.json --desc "..." \
                                         --compiler --clock-from bench_libtracer_raw.txt \
                                         --row-flags aa_rows.json
    python3 bench/host_guard.py check    --data data.js          # audit the store
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import statistics
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent

# The token a contaminated point carries in benchmark-action's free-form `extra`
# string. `extra` is the ONLY per-point slot that store shape offers, so the flag
# rides in the same field as the host descriptor rather than in a column of its own;
# it is a distinct ALL-CAPS word so the predicate below never has to guess.
CONTAM_TOKEN = "CONTAMINATED"

# Field separator inside `extra` — matches the descriptor perf-local.yml already builds.
SEP = " · "

# The token ONE point carries when its own row failed the A/A bracket (#1890). It rides
# only on the points `bracket_failed_names` names, never on the whole run, and it is
# spelled so `CONTAM_TOKEN` is not a substring of it: the run-level flag stays the only
# thing `is_contaminated` reads.
ROW_TOKEN = "AA-ROW-OVER-BAND"

# Default quiescence bar, as 1-minute load average per logical CPU. 0.25 on the
# 31-CPU bench host is a bar of ~7.75, which the workflow's own build blows through
# and a settled machine sits far below (idle is < 1). It is deliberately not tighter:
# the host serves other repos' CI, and a bar it can never reach costs every sample.
DEFAULT_LOAD_PER_CPU = 0.25

# Default A/A disagreement band, percent. The measured same-binary null floor on this
# host is ~±4% on deliveries/s when the machine is ALREADY loaded and near 0% on p50
# when it is quiet, so 6% sits above the noise and below the ~10% contamination this
# guard exists to catch.
DEFAULT_BAND = 6.0


def loadavg1() -> float:
    """@brief The 1-minute load average, or 0.0 where /proc is not present."""
    try:
        return float(pathlib.Path("/proc/loadavg").read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return 0.0


def load_bar(ncpu: int, per_cpu: float = DEFAULT_LOAD_PER_CPU) -> float:
    """@brief The quiescence bar for a host of @p ncpu logical CPUs.

    Scaled by CPU count because load average counts runnable tasks machine-wide: a
    load of 4 is half of a small host and background noise on a large one.
    """
    return max(1.0, per_cpu * max(1, ncpu))


def wait_for_quiet(bar: float, timeout: float, poll: float = 15.0,
                   now=time.monotonic, sleep=time.sleep,
                   read=loadavg1) -> tuple[bool, float, float]:
    """@brief Poll the load average until it falls to @p bar, or @p timeout expires.

    Returns (quiet, observed_load, waited_seconds). `quiet=False` is the SKIP signal,
    never a failure: the caller banks nothing and the job stays green.

    The clock and the reader are injected so the decision rule can be tested without
    a real machine or a real wall clock.
    """
    start = now()
    while True:
        load = read()
        waited = now() - start
        if load <= bar:
            return True, load, waited
        if waited >= timeout:
            return False, load, waited
        sleep(min(poll, max(0.0, timeout - waited)))


def parse_result_rows(text: str) -> dict[tuple, dict[str, list[float]]]:
    """@brief RESULT transcript -> {(sys, mode, size, fan, ep): {metric: [values]}}.

    The same tab-separated 12-column RESULT shape `perf_emit_benchmark.py` consumes
    (`RESULT sys mode size fan ep pub_s deliv_s mb_s p50ns p99ns meanns`), keyed the
    same way so a bracket row and a banked row mean the same point. Repeated rows are
    kept as a list and medianed by the caller — one bench run emits a row per repeat.
    """
    out: dict[tuple, dict[str, list[float]]] = {}
    for line in text.splitlines():
        f = line.split("\t")
        if len(f) != 12 or f[0] != "RESULT":
            continue
        try:
            key = (f[1], f[2], int(f[3]), int(f[4]), int(f[5]))
            deliv, p50 = float(f[7]), float(f[9])
        except ValueError:
            continue
        slot = out.setdefault(key, {"deliv_s": [], "p50_ns": []})
        slot["deliv_s"].append(deliv)
        slot["p50_ns"].append(p50)
    return out


def _medians(rows: dict[tuple, dict[str, list[float]]]) -> dict[tuple, dict[str, float]]:
    return {k: {m: statistics.median(v) for m, v in mv.items() if v}
            for k, mv in rows.items()}


def bracket_rows(pre_text: str, post_text: str) -> list[tuple[tuple, str, float, float, float]]:
    """@brief Every row the A/A null pair can compare, worst disagreement first.

    One tuple per (point, metric) present in BOTH transcripts: `(key, metric, pre, post,
    percent)`, where `key` is `parse_result_rows`' 5-tuple and `metric` is `deliv_s` or
    `p50_ns`. The verdict and the per-row flags (#1890) are both read off this one list,
    so "the worst row" and "the rows over the band" can never be computed two ways.
    """
    pre, post = _medians(parse_result_rows(pre_text)), _medians(parse_result_rows(post_text))
    out = []
    for key in set(pre) & set(post):
        for metric in ("deliv_s", "p50_ns"):
            a, b = pre[key].get(metric), post[key].get(metric)
            if a and b and a > 0:
                out.append((key, metric, a, b, abs(b - a) / a * 100.0))
    out.sort(key=lambda r: (-r[4], r[0], r[1]))
    return out


def bracket_verdict(pre_text: str, post_text: str,
                    band: float = DEFAULT_BAND) -> tuple[bool, str, float]:
    """@brief Compare the A/A null pair taken either side of the measured run.

    Returns (clean, worst_row_description, worst_percent). Every point present in
    BOTH transcripts is compared on throughput and p50; the worst absolute
    disagreement decides. Exceeding @p band means the machine moved under the
    measurement — the sample is still banked, but flagged, because a datum taken in
    known conditions is worth more than no datum at all. This WHOLE-RUN verdict is the
    one gating consumers read; the rows that exceeded the band are named separately by
    `bracket_failed_names`, for the charts (#1890).

    A pair with no comparable rows returns clean with a percent of 0.0 and a spoken
    reason: a missing transcript is the caller's problem to notice (the workflow
    warns on an empty one), and inventing a contamination verdict from no evidence
    would be its own kind of lie.
    """
    rows = bracket_rows(pre_text, post_text)
    if not rows:
        return True, "no comparable rows", 0.0
    (sysname, mode, size, fan, ep), metric, a, b, pct = rows[0]
    return pct <= band, (f"{sysname} {mode} {size}B/fan{fan}/{ep}ep {metric} "
                         f"{a:.1f} -> {b:.1f}"), pct


# The banked series each bracket metric feeds, as name suffixes (perf_emit_benchmark.py's
# spelling): throughput is banked twice, as deliveries/s and as its ns/delivery inversion.
_BANKED_SUFFIXES = {"deliv_s": (" throughput", " ns/delivery"), "p50_ns": (" p50 latency",)}


def bracket_failed_names(pre_text: str, post_text: str,
                         band: float = DEFAULT_BAND) -> list[str]:
    """@brief The banked series names of every bracket row that disagreed by more than
    @p band (#1890), sorted.

    The A/A probe is `bench_forward_demux`, which is also banked, so a row that failed
    its own same-binary check is a row of the store. `stamp --row-flags` marks exactly
    those points with `ROW_TOKEN`, and the charts hide them even where the rest of the
    run is drawn.
    """
    from perf_emit_benchmark import series_tag  # sibling; deferred, stdlib only
    out = set()
    for key, metric, _, _, pct in bracket_rows(pre_text, post_text):
        if pct > band:
            out.update(series_tag(*key) + sfx for sfx in _BANKED_SUFFIXES[metric])
    return sorted(out)


def compiler_identity(cxx: str | None = None) -> str:
    """@brief The compiler that built the bench binaries, as a stamped one-liner.

    Recorded per sample because the store previously carried the host string and
    nothing else: a toolchain bump and a code change moved the trend identically and
    a reader could not tell them apart. Falls back to a spoken "unknown" rather than
    raising — a missing compiler string must never cost the commit its point.
    """
    exe = cxx or os.environ.get("CXX") or "c++"
    try:
        out = subprocess.run([exe, "--version"], capture_output=True, text=True,
                             timeout=30).stdout.splitlines()
    except (OSError, subprocess.SubprocessError):
        return "compiler unknown"
    if not out:
        return "compiler unknown"
    head = out[0].strip()
    m = re.search(r"\b(\d+\.\d+\.\d+)\b", head)
    name = head.split("(")[0].strip() or exe
    return f"{name} {m.group(1)}" if m else head


def contamination_note(reason: str) -> str:
    """@brief The `extra` fragment appended to every point of a suspect sample."""
    return f"{CONTAM_TOKEN} ({reason})"


def is_contaminated(extra: str | None) -> bool:
    """@brief Does this point's `extra` string declare the sample suspect?

    The single predicate every consumer must use — the history renderer, and
    `perf_gate.py`'s `enforces`, which imports THIS function rather than re-deriving
    the rule (#1251) — so "what counts as contaminated" is decided in one place and
    cannot drift between the tool that writes it and the tool that reads it.
    """
    return bool(extra) and CONTAM_TOKEN in extra


def load_known_contaminated(path: pathlib.Path | None = None) -> dict[str, str]:
    """@brief sha -> reason, for samples banked BEFORE the guard existed.

    Kept as a reviewed, checked-in list rather than by rewriting the gh-pages store:
    the raw datum stays exactly as measured, and the judgement about it lands in git
    history where it can be argued with. A sha absent here and unflagged in its own
    `extra` is trusted.
    """
    p = path or (HERE / "contaminated_samples.json")
    try:
        doc = json.loads(p.read_text())
    except (OSError, ValueError):
        return {}
    return {str(s["commit"]): str(s.get("reason", "")) for s in doc.get("samples", [])
            if isinstance(s, dict) and s.get("commit")}


def entry_contaminated(entry: dict, known: dict[str, str] | None = None) -> str | None:
    """@brief The contamination reason for one stored entry, or None if it is trusted.

    Answers from either source — the flag the sample stamped on itself at measure
    time, or the retroactive list for the samples that predate the guard.
    """
    known = load_known_contaminated() if known is None else known
    sha = str((entry.get("commit") or {}).get("id", ""))
    for prefix, reason in known.items():
        if sha.startswith(prefix):
            return reason or "listed in bench/contaminated_samples.json"
    for b in entry.get("benches", []):
        if is_contaminated(b.get("extra")):
            return str(b.get("extra"))
    return None


# The per-row re-classification of a flagged run (#1890), for the CHARTS only. A run is
# flagged whole when one A/A row, or one invocation's conditions, failed; on 2026-10 that
# was 28 runs of 30, so whole-run hiding emptied the trend board. Within a flagged run a
# row stays drawn unless it is an outlier against its OWN series' neighbours: the median of
# up to `ROW_NEIGHBOURS` recorded values either side, by more than `DEFAULT_BAND` or by
# `ROW_SPREAD_K` x the neighbours' relative median absolute deviation, whichever is wider,
# so a row that is noisy by nature (the bimodal multi-threaded rows, `fold-b4`) is judged
# against its own spread and not against the quiet rows' band. A row with fewer than two
# neighbours cannot be vouched for and is hidden. A flagged run with more than
# `ROW_WHOLE_RUN_SHARE` of its rows hidden measured the machine, not the code, and is
# hidden whole, as before.
ROW_NEIGHBOURS = 4
ROW_SPREAD_K = 3.0
ROW_WHOLE_RUN_SHARE = 0.5


def _row_outlier(vals: list[float], j: int, band: float) -> bool:
    """@brief Is `vals[j]` an outlier against up to `ROW_NEIGHBOURS` values either side?"""
    nb = vals[max(0, j - ROW_NEIGHBOURS):j] + vals[j + 1:j + 1 + ROW_NEIGHBOURS]
    if len(nb) < 2:
        return True
    ref = statistics.median(nb)
    if ref <= 0:
        return False
    spread = statistics.median(abs(x - ref) for x in nb) / ref * 100.0
    return abs(vals[j] / ref - 1.0) * 100.0 > max(band, ROW_SPREAD_K * spread)


def untrusted_cells(entries: list[dict], known: dict[str, str] | None = None,
                    band: float = DEFAULT_BAND) -> dict[int, tuple[str, set[str] | None]]:
    """@brief entry index -> (reason, hidden row names, or None for the whole run).

    The CHARTS' trust rule (#1890); gating consumers keep `entry_contaminated`'s
    whole-run verdict. An unflagged run is absent (every row drawn). A run on the
    reviewed list is hidden whole: that list is a human verdict on the whole sample. A
    flagged run hides the rows that carry `ROW_TOKEN` (they failed their own A/A
    check) and the rows that are outliers against their own neighbours (see
    `ROW_NEIGHBOURS`), or the whole run when more than `ROW_WHOLE_RUN_SHARE` of its rows
    are hidden. Nothing in the store is changed: the rule is re-run on every render.
    """
    known = load_known_contaminated() if known is None else known
    flagged = {i: r for i, e in enumerate(entries) if (r := entry_contaminated(e, known))}
    series: dict[str, list[tuple[int, float]]] = {}
    for i, e in enumerate(entries):
        for b in e.get("benches", []):
            try:
                series.setdefault(b["name"], []).append((i, float(b["value"])))
            except (KeyError, TypeError, ValueError):
                continue
    hidden: dict[int, set[str]] = {i: set() for i in flagged}
    for name, pts in series.items():
        vals = [v for _, v in pts]
        for j, (i, _) in enumerate(pts):
            if i in hidden and _row_outlier(vals, j, band):
                hidden[i].add(name)
    out: dict[int, tuple[str, set[str] | None]] = {}
    for i, reason in flagged.items():
        e = entries[i]
        sha = str((e.get("commit") or {}).get("id", ""))
        rows = hidden[i] | {b.get("name") for b in e.get("benches", [])
                            if ROW_TOKEN in (b.get("extra") or "")}
        n = len(e.get("benches", [])) or 1
        whole = (any(sha.startswith(p) for p in known)
                 or len(rows) > ROW_WHOLE_RUN_SHARE * n)
        out[i] = (reason, None if whole else rows)
    return out


def stamp(paths: list[pathlib.Path], desc: str, row_flags: set[str] | None = None) -> int:
    """@brief Write @p desc into every point's `extra` in each emitted metrics JSON.

    A point named in @p row_flags (its row failed the A/A bracket, #1890) also gets
    `ROW_TOKEN`.
    """
    row_flags = row_flags or set()
    n = 0
    for p in paths:
        items = json.loads(p.read_text())
        for it in items:
            it["extra"] = desc + (SEP + ROW_TOKEN if it.get("name") in row_flags else "")
            n += 1
        p.write_text(json.dumps(items, indent=1) + "\n")
    return n


def _gh_output(**kv: object) -> None:
    """@brief Publish step outputs when running under Actions; a no-op elsewhere."""
    path = os.environ.get("GITHUB_OUTPUT")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as fh:
        for k, v in kv.items():
            fh.write(f"{k}={v}\n")


def _cmd_wait(args: argparse.Namespace) -> int:
    ncpu = args.ncpu or os.cpu_count() or 1
    bar = load_bar(ncpu, args.max_load_per_cpu)
    quiet, load, waited = wait_for_quiet(bar, args.timeout, args.poll)
    if quiet:
        print(f"host_guard: quiet after {waited:.0f}s — load {load:.2f} <= bar {bar:.2f} "
              f"({ncpu} cpus)")
    else:
        print(f"::notice::bench-local sample SKIPPED — host busy after {waited:.0f}s: "
              f"load {load:.2f} > bar {bar:.2f} ({ncpu} cpus). Nothing was banked; "
              f"the job is green because refusing to measure is the correct outcome.")
    _gh_output(bank="true" if quiet else "false", load=f"{load:.2f}", bar=f"{bar:.2f}")
    return 0  # a busy host is a SKIP, never a failure


def _cmd_bracket(args: argparse.Namespace) -> int:
    pre = pathlib.Path(args.pre).read_text() if pathlib.Path(args.pre).exists() else ""
    post = pathlib.Path(args.post).read_text() if pathlib.Path(args.post).exists() else ""
    clean, what, pct = bracket_verdict(pre, post, args.band)
    if clean:
        print(f"host_guard: A/A bracket clean — worst {pct:.1f}% <= band {args.band:.1f}% "
              f"({what})")
    else:
        print(f"::warning::bench-local sample FLAGGED contaminated — the A/A null pair "
              f"disagreed by {pct:.1f}% (band {args.band:.1f}%): {what}. The point is "
              f"still banked; the blocking tier ignores it, and the charts hide its rows "
              f"over the band and any row out of line with its neighbours (#1890).")
    _gh_output(clean="true" if clean else "false",
               note="" if clean else contamination_note(
                   f"A/A bracket {pct:.1f}% > {args.band:.1f}% band"))
    if args.rows_out:
        names = bracket_failed_names(pre, post, args.band)
        pathlib.Path(args.rows_out).write_text(json.dumps(names, indent=1) + "\n")
        if names:
            print(f"host_guard: {len(names)} banked row(s) over the band, flagged per row "
                  f"-> {args.rows_out}")
    return 0  # a flagged sample is recorded, not failed


def clock_floor(text: str) -> str | None:
    """@brief The clock floor a bench transcript recorded, as a stamped one-liner (#1804).

    Every gated bench prints `CLOCK <res_ns> <sample_ns>` ahead of its rows: the clock's
    resolution (`clock_getres`) and the measured cost of the pair of clock reads one timed
    sample pays. Returns e.g. `clock 1 ns res · 21.9 ns/sample`, or None when the transcript
    predates the line.
    """
    for line in text.splitlines():
        f = line.split("\t")
        if len(f) == 3 and f[0] == "CLOCK":
            try:
                res, sample = float(f[1]), float(f[2])
            except ValueError:
                continue
            return f"clock {res:g} ns res · {sample:.1f} ns/sample"
    return None


def _cmd_stamp(args: argparse.Namespace) -> int:
    desc = args.desc
    if args.compiler:
        desc += SEP + compiler_identity(args.cxx)
    if args.clock_from:
        path = pathlib.Path(args.clock_from)
        floor = clock_floor(path.read_text()) if path.exists() else None
        desc += SEP + (floor or "clock floor not recorded")
    # Several verdicts can flag one sample (the A/A bracket, and since #1676 the
    # measurement-conditions ledger); each non-empty one is stamped, in order.
    for note in args.note or []:
        if note:
            desc += SEP + note
    rows: set[str] = set()
    if args.row_flags and pathlib.Path(args.row_flags).exists():
        rows = set(json.loads(pathlib.Path(args.row_flags).read_text() or "[]"))
    n = stamp([pathlib.Path(p) for p in args.json], desc, rows)
    print(f"host_guard: stamped {n} points -> {desc}")
    return 0


def _cmd_check(args: argparse.Namespace) -> int:
    """@brief Audit a stored data.js: which samples are flagged, and by which source."""
    text = pathlib.Path(args.data).read_text()
    doc = json.loads(text[text.index("{"):].rstrip().rstrip(";"))
    known = load_known_contaminated()
    flagged = 0
    for suite, entries in doc.get("entries", {}).items():
        for i, (reason, rows) in sorted(untrusted_cells(entries, known).items()):
            flagged += 1
            sha = entries[i]["commit"]["id"][:8]
            shown = ("hidden whole" if rows is None
                     else f"{len(rows)}/{len(entries[i]['benches'])} rows hidden on the charts")
            print(f"  [{suite[:28]}...] sample {i} {sha}: {reason} ({shown})")
    print(f"host_guard: {flagged} flagged point-entries across the store")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    w = sub.add_parser("wait", help="hold until the host is quiet; skip the sample if not")
    w.add_argument("--max-load-per-cpu", type=float, default=DEFAULT_LOAD_PER_CPU)
    w.add_argument("--timeout", type=float, default=600.0)
    w.add_argument("--poll", type=float, default=15.0)
    w.add_argument("--ncpu", type=int, default=0)
    w.set_defaults(fn=_cmd_wait)

    b = sub.add_parser("bracket", help="verdict on the A/A null pair around the run")
    b.add_argument("--pre", required=True)
    b.add_argument("--post", required=True)
    b.add_argument("--band", type=float, default=DEFAULT_BAND)
    b.add_argument("--rows-out", default=None,
                   help="write the banked names of the rows over the band here (JSON, #1890)")
    b.set_defaults(fn=_cmd_bracket)

    s = sub.add_parser("stamp", help="write host/compiler/flag onto every emitted point")
    s.add_argument("--json", action="append", required=True)
    s.add_argument("--desc", required=True)
    s.add_argument("--note", action="append", default=[],
                   help="a verdict fragment to append (repeatable; empty ones are skipped)")
    s.add_argument("--row-flags", default=None,
                   help="a `bracket --rows-out` file: those points also get ROW_TOKEN (#1890)")
    s.add_argument("--cxx", default=None)
    s.add_argument("--compiler", action="store_true",
                   help="append the compiler identity to the descriptor")
    s.add_argument("--clock-from", default=None,
                   help="a bench transcript whose CLOCK line is appended (#1804)")
    s.set_defaults(fn=_cmd_stamp)

    c = sub.add_parser("check", help="audit a stored data.js for flagged samples")
    c.add_argument("--data", required=True)
    c.set_defaults(fn=_cmd_check)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
