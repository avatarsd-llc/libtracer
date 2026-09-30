#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Measurement-condition classifier: was the bench CPU ours while we timed it? (#1676)

A perf verdict is a claim about the code, and it is only that when the machine was not
the thing that moved. On 2026-09-30 the shared bench host ran at load ~115 on 31 CPUs,
nothing kept other work off the pinned bench CPU, and gates reported 400 ns against a
100 ns baseline on paths the PRs under test never touched. The gate measured the host
and billed the code for it.

This module is the ONE place that answers "were the conditions clean?". Every timed
bench invocation goes through `measure()` (from Python: `perf_gate.py`) or through the
`run` subcommand (from a workflow shell step), and every verdict about a set of
invocations comes from `Ledger`. Nothing else in the tree reads /proc to decide it.

What is sampled, around each invocation:

  * FOREIGN TIME on the measured CPU set — busy jiffies from /proc/stat (user, nice,
    system, irq, softirq, steal) minus the bench process's own CPU time, as a share of
    the window. Steal counts as foreign: on a VM it is the hypervisor running someone
    else while our vCPU wanted to. The bench's own time comes from the kernel's
    child-rusage for the reaped process — the same utime/stime counters
    /proc/<pid>/stat exposes, read after exit so the process cannot vanish between
    the last sample and the reap.
  * INVOLUNTARY CONTEXT SWITCHES of the bench — `ru_nivcsw`, the counter
    /proc/<pid>/status prints as `nonvoluntary_ctxt_switches`. Recorded, not gated: a
    multi-threaded bench pinned to one CPU preempts itself, so the count alone cannot
    tell a neighbour from the bench's own threads.
  * CPU PRESSURE — /proc/pressure/cpu `some avg10`, sampled as the run starts. The
    reading is taken at launch, not at exit, on purpose: several bench modes run N
    threads on one pinned CPU, and PSI cannot tell our own runnable threads from a
    neighbour's, so a post-run reading would charge the bench for its own threads.
    The pre-launch reading is the host's state as the measurement began; foreign time
    is what covers the window itself. The exit reading is recorded beside it.

The rule (`classify`): a run is CONTENDED when foreign time exceeds `FOREIGN_MAX_PCT`
(2%) of the window, or launch pressure exceeds `PRESSURE_MAX` (5). Otherwise CLEAN.
`measure()` re-runs a contended invocation up to `attempts` times and keeps the first
clean one; if none is clean it keeps the last and says so. A `Ledger` over a job's
invocations is CLEAN only if every one of them is — and a ledger that is not clean is
the only input a gate needs to print INCONCLUSIVE instead of PASS or FAIL.

The contamination token is host_guard.py's (`contamination_note`), not a second one: an
unclean ledger stamps the same CONTAMINATED flag the A/A bracket does, so the history
renderer, `store_guard.trusted()` (the rolling-drift baseline and the density count)
and `perf_gate.enforces` all honour it without learning a new word.

Unpinned runs (GitHub-hosted runners) are classified by the same rule over the process's
whole affinity set. They cannot be isolated, so the record says `unpinned` and the
foreign share is diluted across the set; the interleaved A/B stays their primary defence.

Stdlib only, like every other bench tool here.

    # from a workflow step: pin, measure, re-run while contended, record
    python3 bench/bench_conditions.py run --cpu 2 --out raw.txt --record cond.jsonl -- ./bench_x
    # the verdict over every recorded invocation (+ GITHUB_OUTPUT line= / note= / clean=)
    python3 bench/bench_conditions.py summary --record cond.jsonl
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
import pathlib
import resource
import subprocess
import sys
import time
from typing import Callable, Iterable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from host_guard import contamination_note  # noqa: E402

CLEAN = "CLEAN"
CONTENDED = "CONTENDED"

# Foreign busy time on the measured CPU set, percent of the window. The maintainer's
# starting bar (#1676). The reserved bench CPU measured 2-3% busy with the runner idle
# right after the host fix, so this is tight on purpose and meant to be re-read against
# the recorded values rather than loosened by guesswork.
FOREIGN_MAX_PCT = 2.0
# /proc/pressure/cpu `some avg10` at launch, percent (#1676's starting bar).
PRESSURE_MAX = 5.0
# /proc/stat counts in USER_HZ ticks (10 ms) while rusage is microsecond-precise, so a
# short run can show a tick or two of "foreign" time that is only rounding between the
# two clocks. Foreign time at or under this many ticks is never contention on its own.
QUANTUM_TICKS = 2
# Default re-runs of a contended invocation before the ledger gives up on it.
DEFAULT_ATTEMPTS = 3

CLK_TCK = os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else 100
PROC_STAT = "/proc/stat"
PROC_PSI = "/proc/pressure/cpu"


def _read(path: str) -> str | None:
    """@brief A /proc file's text, or None where it does not exist (no PSI, no /proc)."""
    try:
        return pathlib.Path(path).read_text()
    except OSError:
        return None


@dataclasses.dataclass(frozen=True)
class Sample:
    """@brief One /proc snapshot of the measured CPU set, taken either side of a run."""

    wall_ns: int                 # monotonic clock, ns
    busy: int                    # busy ticks summed over the set (incl. irq + steal)
    total: int                   # all ticks summed over the set
    psi_avg10: float | None      # /proc/pressure/cpu `some avg10`, None when absent


def parse_proc_stat(text: str, cpus: Iterable[int]) -> tuple[int, int]:
    """@brief (busy, total) ticks summed over @p cpus from /proc/stat text.

    Fields: user nice system idle iowait irq softirq steal [guest guest_nice]. Guest
    time is already inside user/nice, so only the first eight are summed. iowait is
    idle time (the CPU was free to run anyone), so it is not busy.
    """
    want = {f"cpu{c}" for c in cpus}
    busy = total = 0
    for line in text.splitlines():
        f = line.split()
        if not f or f[0] not in want:
            continue
        v = [int(x) for x in f[1:9]] + [0] * max(0, 8 - len(f[1:9]))
        user, nice, system, idle, iowait, irq, softirq, steal = v[:8]
        busy += user + nice + system + irq + softirq + steal
        total += user + nice + system + idle + iowait + irq + softirq + steal
    return busy, total


def parse_psi_avg10(text: str | None) -> float | None:
    """@brief The `some avg10` value of a PSI file, or None when absent or unparsable."""
    for line in (text or "").splitlines():
        if line.startswith("some "):
            for kv in line.split()[1:]:
                k, _, v = kv.partition("=")
                if k == "avg10":
                    try:
                        return float(v)
                    except ValueError:
                        return None
    return None


def snapshot(cpus: Iterable[int], read: Callable[[str], str | None] = _read,
             now: Callable[[], int] = time.monotonic_ns) -> Sample:
    """@brief Sample the measured CPU set. @p read and @p now are injected for tests."""
    busy, total = parse_proc_stat(read(PROC_STAT) or "", cpus)
    return Sample(now(), busy, total, parse_psi_avg10(read(PROC_PSI)))


@dataclasses.dataclass(frozen=True)
class Conditions:
    """@brief What the machine was doing during one invocation, and the verdict on it."""

    cpus: tuple[int, ...]
    pinned: bool
    wall_s: float
    foreign_pct: float
    own_cpu_s: float
    nivcsw: int
    pressure: float | None       # `some avg10` at launch — the gated reading
    pressure_exit: float | None  # `some avg10` at exit — recorded only (self-polluted)
    verdict: str
    reason: str

    @property
    def clean(self) -> bool:
        """@brief True when this invocation may carry a verdict."""
        return self.verdict == CLEAN

    def line(self) -> str:
        """@brief One human-auditable line: the numbers and the verdict path."""
        psi = "n/a" if self.pressure is None else f"{self.pressure:.1f}"
        return (f"{_cpuset(self.cpus)}{'' if self.pinned else ' (unpinned)'}: "
                f"foreign {self.foreign_pct:.1f}% of {self.wall_s:.1f}s, psi {psi}, "
                f"nivcsw {self.nivcsw} -> {self.verdict}"
                + (f" ({self.reason})" if self.reason else ""))


def _cpuset(cpus: Iterable[int]) -> str:
    """@brief `cpu2` or `cpus 0-3,6` — compact and unambiguous."""
    c = sorted(set(cpus))
    if len(c) == 1:
        return f"cpu{c[0]}"
    runs, start = [], None
    for i, x in enumerate(c):
        if start is None:
            start = x
        if i + 1 == len(c) or c[i + 1] != x + 1:
            runs.append(f"{start}" if start == x else f"{start}-{x}")
            start = None
    return "cpus " + ",".join(runs)


def classify(before: Sample, after: Sample, own_cpu_s: float, nivcsw: int,
             cpus: Iterable[int], pinned: bool = True,
             foreign_max_pct: float = FOREIGN_MAX_PCT,
             pressure_max: float = PRESSURE_MAX, clk_tck: int = CLK_TCK) -> Conditions:
    """@brief The decision rule: CLEAN, or CONTENDED with the reason spoken.

    Pure — two snapshots and the bench's own usage in, a verdict out — so the rule is
    tested on synthetic /proc text rather than on a machine that happens to be busy.
    """
    cpus = tuple(sorted(set(cpus)))
    d_total = max(0, after.total - before.total)
    d_busy = max(0, after.busy - before.busy)
    foreign_ticks = max(0.0, d_busy - own_cpu_s * clk_tck)
    foreign_pct = (foreign_ticks / d_total * 100.0) if d_total else 0.0
    reasons = []
    if foreign_ticks > QUANTUM_TICKS and foreign_pct > foreign_max_pct:
        reasons.append(f"foreign {foreign_pct:.1f}% > {foreign_max_pct:g}%")
    if before.psi_avg10 is not None and before.psi_avg10 > pressure_max:
        reasons.append(f"psi {before.psi_avg10:.1f} > {pressure_max:g}")
    return Conditions(cpus=cpus, pinned=pinned,
                      wall_s=max(0, after.wall_ns - before.wall_ns) / 1e9,
                      foreign_pct=foreign_pct, own_cpu_s=own_cpu_s, nivcsw=nivcsw,
                      pressure=before.psi_avg10, pressure_exit=after.psi_avg10,
                      verdict=CONTENDED if reasons else CLEAN, reason="; ".join(reasons))


@dataclasses.dataclass
class Measurement:
    """@brief One invocation's kept attempt: its output, exit code and every attempt's
    conditions (the kept one last)."""

    argv: list[str]
    stdout: str
    stderr: str
    returncode: int
    attempts: list[Conditions]

    @property
    def conditions(self) -> Conditions:
        """@brief The conditions of the attempt whose output was kept."""
        return self.attempts[-1]

    @property
    def clean(self) -> bool:
        """@brief True when the kept attempt ran clean."""
        return self.conditions.clean


def cpus_from_env() -> tuple[int, ...] | None:
    """@brief The pin requested by `BENCH_CPU` (e.g. `2` or `2,3`), or None (unpinned)."""
    spec = os.environ.get("BENCH_CPU", "").strip()
    if not spec:
        return None
    out: set[int] = set()
    for part in spec.split(","):
        lo, _, hi = part.partition("-")
        out.update(range(int(lo), int(hi or lo) + 1))
    return tuple(sorted(out))


def _run_once(argv: list[str], cpus: tuple[int, ...] | None, timeout: float | None,
              env: dict | None) -> tuple[str, str, int, Conditions]:
    """@brief Run @p argv once, pinned to @p cpus when given, and classify the window."""
    pinned = cpus is not None
    measured = cpus if pinned else tuple(sorted(os.sched_getaffinity(0)))
    preexec = (lambda: os.sched_setaffinity(0, cpus)) if pinned else None
    ru0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    before = snapshot(measured)
    p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, env=env,
                       preexec_fn=preexec)
    after = snapshot(measured)
    ru1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    own = (ru1.ru_utime - ru0.ru_utime) + (ru1.ru_stime - ru0.ru_stime)
    cond = classify(before, after, own, ru1.ru_nivcsw - ru0.ru_nivcsw, measured, pinned)
    return p.stdout, p.stderr, p.returncode, cond


def measure(argv: list[str], cpus: tuple[int, ...] | None = None,
            attempts: int = DEFAULT_ATTEMPTS, timeout: float | None = None,
            env: dict | None = None, log: Callable[[str], None] | None = None,
            run: Callable = _run_once) -> Measurement:
    """@brief Run one timed bench invocation until it runs clean, at most @p attempts times.

    Returns the first clean attempt, or the last one with its CONTENDED conditions — the
    caller never has to decide what "clean" means. @p run is injected for tests (a
    scripted contended-then-clean sequence) and defaults to a real, sampled run.
    """
    tried: list[Conditions] = []
    out = err = ""
    rc = 0
    for i in range(max(1, attempts)):
        out, err, rc, cond = run(argv, cpus, timeout, env)
        tried.append(cond)
        if cond.clean:
            break
        if log and i + 1 < attempts:
            log(f"bench_conditions: {pathlib.Path(argv[0]).name} ran CONTENDED "
                f"({cond.reason}) — re-running ({i + 2}/{attempts})")
    return Measurement(list(argv), out, err, rc, tried)


class Ledger:
    """@brief The conditions of every timed invocation in one job, and the verdict on all.

    A ledger is CLEAN only when every invocation's kept attempt is — a verdict built on
    one contended transcript and nine clean ones is still built on a contended one.
    """

    def __init__(self, entries: list[dict] | None = None) -> None:
        self.entries: list[dict] = list(entries or [])

    def add(self, m: Measurement) -> Measurement:
        """@brief Record one invocation; returns @p m so a call site stays one expression."""
        self.entries.append({"cmd": pathlib.Path(m.argv[0]).name
                             + "".join(f" {a}" for a in m.argv[1:]),
                             "attempts": [dataclasses.asdict(c) for c in m.attempts]})
        return m

    def kept(self) -> list[Conditions]:
        """@brief The kept attempt of every invocation, in record order."""
        return [_cond(e["attempts"][-1]) for e in self.entries if e.get("attempts")]

    @property
    def clean(self) -> bool:
        """@brief True when every recorded invocation ran clean (vacuously so if none)."""
        return all(c.clean for c in self.kept())

    def reruns(self) -> int:
        """@brief How many extra attempts contention cost this job."""
        return sum(len(e.get("attempts", [])) - 1 for e in self.entries)

    def line(self) -> str:
        """@brief The one-line stamp for a result file's host descriptor."""
        kept = self.kept()
        if not kept:
            return "conditions not recorded"
        worst_f = max(c.foreign_pct for c in kept)
        psis = [c.pressure for c in kept if c.pressure is not None]
        worst_p = f"{max(psis):.1f}" if psis else "n/a"
        n_clean = sum(c.clean for c in kept)
        return (f"bench {_cpuset(kept[0].cpus)}{'' if kept[0].pinned else ' unpinned'}: "
                f"foreign <={worst_f:.1f}%, psi <={worst_p}, "
                f"nivcsw {sum(c.nivcsw for c in kept)}, "
                f"{n_clean}/{len(kept)} clean ({self.reruns()} re-run)")

    def note(self) -> str:
        """@brief host_guard's CONTAMINATED fragment when unclean, else empty."""
        if self.clean:
            return ""
        bad = [(e["cmd"], _cond(e["attempts"][-1])) for e in self.entries
               if e.get("attempts") and not _cond(e["attempts"][-1]).clean]
        cmd, c = bad[0]
        more = f" +{len(bad) - 1} more" if len(bad) > 1 else ""
        return contamination_note(f"bench CPU contended: {cmd.split()[0]} {c.reason}{more}")

    def report(self, notable_only: bool = False) -> list[str]:
        """@brief Every invocation's kept conditions, one line each, for the log.

        @p notable_only keeps just the invocations that were re-run or stayed contended —
        what a long gate log needs, since the ledger's `line()` already covers the rest.
        """
        out = []
        for e in self.entries:
            if not e.get("attempts"):
                continue
            n = len(e["attempts"])
            if notable_only and n == 1 and _cond(e["attempts"][-1]).clean:
                continue
            out.append(f"    {e['cmd'][:48]:<48} {_cond(e['attempts'][-1]).line()}"
                       + (f"  [attempt {n}]" if n > 1 else ""))
        return out

    def append_to(self, path: pathlib.Path) -> None:
        """@brief Persist the entries as JSON lines (the record a workflow accumulates)."""
        with open(path, "a", encoding="utf-8") as fh:
            for e in self.entries:
                fh.write(json.dumps(e) + "\n")

    @classmethod
    def load(cls, path: pathlib.Path) -> "Ledger":
        """@brief Read a JSON-lines record; a missing file is an empty ledger."""
        try:
            text = path.read_text()
        except OSError:
            return cls()
        return cls([json.loads(x) for x in text.splitlines() if x.strip()])


def _cond(d: dict) -> Conditions:
    """@brief A Conditions back from its recorded dict."""
    d = dict(d)
    d["cpus"] = tuple(d["cpus"])
    return Conditions(**d)


def _gh(name: str, text: str) -> None:
    """@brief Append to a GitHub Actions file (GITHUB_OUTPUT / GITHUB_STEP_SUMMARY)."""
    path = os.environ.get(name)
    if path:
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(text)


def _cmd_run(args: argparse.Namespace) -> int:
    cpus = (tuple(int(c) for c in args.cpu.split(",")) if args.cpu
            else cpus_from_env())
    m = measure(args.argv, cpus=cpus, attempts=args.attempts,
                log=lambda s: print(s, file=sys.stderr))
    if args.out:
        pathlib.Path(args.out).write_text(m.stdout)
    if not args.out or args.tee:
        sys.stdout.write(m.stdout)
    if args.err:
        pathlib.Path(args.err).write_text(m.stderr)
    else:
        sys.stderr.write(m.stderr)
    print(f"bench_conditions: {m.conditions.line()}", file=sys.stderr)
    record = args.record or os.environ.get("BENCH_CONDITIONS_RECORD")
    if record:
        led = Ledger()
        led.add(m)
        led.append_to(pathlib.Path(record))
    # The bench's own exit code, always: a contended run is a condition to record, never
    # a crash to report. The verdict over the whole job is `summary`'s, in one place.
    return m.returncode


def _cmd_summary(args: argparse.Namespace) -> int:
    led = Ledger.load(pathlib.Path(args.record))
    verdict = "CLEAN" if led.clean else "CONTENDED"
    print(f"bench_conditions: {len(led.entries)} timed invocation(s) — {verdict}")
    for x in led.report():
        print(x)
    print(f"  {led.line()}")
    if not led.clean:
        print(f"::warning::bench CPU contended — {led.note()}. The sample is flagged; "
              f"no verdict and no rolling baseline will be built on it.")
    _gh("GITHUB_OUTPUT", f"clean={'true' if led.clean else 'false'}\n"
                         f"line={led.line()}\nnote={led.note()}\n")
    _gh("GITHUB_STEP_SUMMARY", f"### measurement conditions — {verdict}\n```\n"
                               + "\n".join([led.line()] + led.report()) + "\n```\n")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run one timed bench invocation under the classifier")
    r.add_argument("--cpu", default="", help="pin to these CPUs (default: $BENCH_CPU)")
    r.add_argument("--attempts", type=int, default=DEFAULT_ATTEMPTS)
    r.add_argument("--out", default="", help="write the kept attempt's stdout here")
    r.add_argument("--tee", action="store_true", help="with --out, also echo it")
    r.add_argument("--err", default="", help="write the kept attempt's stderr here")
    r.add_argument("--record", default="",
                   help="append the conditions here (default: $BENCH_CONDITIONS_RECORD)")
    r.add_argument("argv", nargs=argparse.REMAINDER)
    r.set_defaults(fn=_cmd_run)
    s = sub.add_parser("summary", help="the verdict over every recorded invocation")
    s.add_argument("--record", required=True)
    s.set_defaults(fn=_cmd_summary)
    args = ap.parse_args(argv)
    if args.cmd == "run":
        if args.argv[:1] == ["--"]:
            args.argv = args.argv[1:]
        if not args.argv:
            ap.error("run: nothing to run (usage: run [opts] -- CMD ...)")
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
