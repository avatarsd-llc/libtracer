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
tier at a ~5% bar: a contaminated point there does not mislead a reader, it fails a
PR. Hence three instruments, in the order the workflow uses them:

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
  * `stamp` — write the host descriptor, the COMPILER IDENTITY and any contamination
    verdict onto every emitted point, so the store records the conditions a number
    was taken under and not just the number.

Two rules run through all of it. A busy host SKIPS its sample and never fails the
job: refusing to measure is a correct outcome, and a red job trains the reader to
ignore red. And a suspect sample is FLAGGED, never deleted — the raw datum stays in
the store where it can be re-examined, while the charts and any gating consumer stop
believing it (`is_contaminated`, and `contaminated_samples.json` for the points that
predate the guard).

Stdlib only, like every other bench tool here.

    python3 bench/host_guard.py wait     --max-load-per-cpu 0.25 --timeout 600
    python3 bench/host_guard.py bracket  --pre pre.txt --post post.txt --band 6
    python3 bench/host_guard.py stamp    --json a.json --json b.json --desc "..."
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


def bracket_verdict(pre_text: str, post_text: str,
                    band: float = DEFAULT_BAND) -> tuple[bool, str, float]:
    """@brief Compare the A/A null pair taken either side of the measured run.

    Returns (clean, worst_row_description, worst_percent). Every point present in
    BOTH transcripts is compared on throughput and p50; the worst absolute
    disagreement decides. Exceeding @p band means the machine moved under the
    measurement — the sample is still banked, but flagged, because a datum taken in
    known conditions is worth more than no datum at all.

    A pair with no comparable rows returns clean with a percent of 0.0 and a spoken
    reason: a missing transcript is the caller's problem to notice (the workflow
    warns on an empty one), and inventing a contamination verdict from no evidence
    would be its own kind of lie.
    """
    pre, post = _medians(parse_result_rows(pre_text)), _medians(parse_result_rows(post_text))
    worst_pct, worst_what = 0.0, "no comparable rows"
    for key in sorted(set(pre) & set(post)):
        for metric in ("deliv_s", "p50_ns"):
            a, b = pre[key].get(metric), post[key].get(metric)
            if not a or not b or a <= 0:
                continue
            pct = abs(b - a) / a * 100.0
            if pct > worst_pct:
                sysname, mode, size, fan, ep = key
                worst_pct = pct
                worst_what = (f"{sysname} {mode} {size}B/fan{fan}/{ep}ep {metric} "
                              f"{a:.1f} -> {b:.1f}")
    return worst_pct <= band, worst_what, worst_pct


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

    The single predicate every consumer must use — the history renderer today, the
    blocking-tier gate when #1251 lands — so "what counts as contaminated" is decided
    in one place and cannot drift between the tool that writes it and the tool that
    reads it.
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


def stamp(paths: list[pathlib.Path], desc: str) -> int:
    """@brief Write @p desc into every point's `extra` in each emitted metrics JSON."""
    n = 0
    for p in paths:
        items = json.loads(p.read_text())
        for it in items:
            it["extra"] = desc
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
              f"still banked; charts and the blocking tier will ignore it.")
    _gh_output(clean="true" if clean else "false",
               note="" if clean else contamination_note(
                   f"A/A bracket {pct:.1f}% > {args.band:.1f}% band"))
    return 0  # a flagged sample is recorded, not failed


def _cmd_stamp(args: argparse.Namespace) -> int:
    desc = args.desc
    if args.compiler:
        desc += SEP + compiler_identity(args.cxx)
    if args.note:
        desc += SEP + args.note
    n = stamp([pathlib.Path(p) for p in args.json], desc)
    print(f"host_guard: stamped {n} points -> {desc}")
    return 0


def _cmd_check(args: argparse.Namespace) -> int:
    """@brief Audit a stored data.js: which samples are flagged, and by which source."""
    text = pathlib.Path(args.data).read_text()
    doc = json.loads(text[text.index("{"):].rstrip().rstrip(";"))
    known = load_known_contaminated()
    flagged = 0
    for suite, entries in doc.get("entries", {}).items():
        for i, e in enumerate(entries):
            reason = entry_contaminated(e, known)
            if reason:
                flagged += 1
                sha = e["commit"]["id"][:8]
                print(f"  [{suite[:28]}...] sample {i} {sha}: {reason}")
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
    b.set_defaults(fn=_cmd_bracket)

    s = sub.add_parser("stamp", help="write host/compiler/flag onto every emitted point")
    s.add_argument("--json", action="append", required=True)
    s.add_argument("--desc", required=True)
    s.add_argument("--note", default="")
    s.add_argument("--cxx", default=None)
    s.add_argument("--compiler", action="store_true",
                   help="append the compiler identity to the descriptor")
    s.set_defaults(fn=_cmd_stamp)

    c = sub.add_parser("check", help="audit a stored data.js for flagged samples")
    c.add_argument("--data", required=True)
    c.set_defaults(fn=_cmd_check)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
