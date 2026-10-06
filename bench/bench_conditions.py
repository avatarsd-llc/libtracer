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
  * CPU PRESSURE over the run itself — the growth of the PSI `some total=` stall
    counter (microseconds) between the two snapshots, as a percent of the wall time,
    from one of two sources:
      - PINNED (a CPU named by BENCH_CPU — the bench host): the bench job's OWN cgroup,
        `<cgroupfs>/<path from /proc/self/cgroup>/cpu.pressure`. The bench CPUs there are
        cgroup-isolated for the bench runner, so host-wide pressure (40-68 on the
        saturated studio host while the bench runner's cgroup read ~0) says nothing
        about the bench CPU and would flag every point. Host-wide pressure is still
        RECORDED, for information only.
      - UNPINNED (hosted runners — nothing to isolate): host-wide /proc/pressure/cpu.
    The window is the run's own, not `avg10`: avg10 is a 10 s moving average, so a
    0.0 s execution read the pressure the PREVIOUS (multi-threaded) execution left
    behind and every re-run of it read the same stale value (#1839). A run shorter than
    `PSI_MIN_WINDOW_S` is too short to measure a share on, and is judged on foreign
    time alone. Multi-threaded runs raise their own cgroup's pressure, so for them the
    pressure is recorded and not scored (#1803).

The rule (`classify`): a run is CONTENDED when foreign time exceeds `FOREIGN_MAX_PCT`
(2%) of the window, or the in-window pressure from the source above exceeds
`PRESSURE_MAX` (5). Otherwise CLEAN.
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
import re
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
# CPU pressure over the execution: `some total=` stall growth as a percent of the wall
# time, the same unit as avg10 (#1676's starting bar, measured in-window since #1839).
PRESSURE_MAX = 5.0
# Shortest execution whose in-window pressure is scored. Below it the 5% bar is a few
# milliseconds of stall, the order of one scheduler tick, so the share is noise and the
# run is judged on foreign time alone (#1839).
PSI_MIN_WINDOW_S = 0.5
# /proc/stat counts in USER_HZ ticks (10 ms) while rusage is microsecond-precise, so a
# short run can show a tick or two of "foreign" time that is only rounding between the
# two clocks. Foreign time at or under this many ticks is never contention on its own.
QUANTUM_TICKS = 2
# Default re-runs of a contended invocation before the ledger gives up on it.
DEFAULT_ATTEMPTS = 3

CLK_TCK = os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else 100
PROC_STAT = "/proc/stat"
PROC_PSI = "/proc/pressure/cpu"
PROC_SELF_CGROUP = "/proc/self/cgroup"
CGROUP_FS = "/sys/fs/cgroup"


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
    psi_us: int | None           # host-wide /proc/pressure/cpu `some total=` µs, None if absent
    cg_us: int | None = None     # this job's own cgroup `cpu.pressure` `some total=` µs


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


def parse_psi_total(text: str | None) -> int | None:
    """@brief The `some total=` stall counter (µs) of a PSI file, or None when absent or
    unparsable."""
    for line in (text or "").splitlines():
        if line.startswith("some "):
            for kv in line.split()[1:]:
                k, _, v = kv.partition("=")
                if k == "total":
                    try:
                        return int(v)
                    except ValueError:
                        return None
    return None


def cgroup_pressure_path(self_cgroup: str | None) -> str | None:
    """@brief `<cgroupfs>/<path>/cpu.pressure` for the cgroup-v2 line of /proc/self/cgroup.

    The bench is a child of this process and inherits its cgroup, so the wrapper's own
    cgroup IS the bench job's. None on a cgroup-v1-only host (no `0::` line).
    """
    for line in (self_cgroup or "").splitlines():
        if line.startswith("0::"):
            rel = line[3:].strip().lstrip("/")
            return f"{CGROUP_FS}/{rel}/cpu.pressure" if rel else f"{CGROUP_FS}/cpu.pressure"
    return None


def snapshot(cpus: Iterable[int], read: Callable[[str], str | None] = _read,
             now: Callable[[], int] = time.monotonic_ns) -> Sample:
    """@brief Sample the measured CPU set. @p read and @p now are injected for tests."""
    busy, total = parse_proc_stat(read(PROC_STAT) or "", cpus)
    cg = cgroup_pressure_path(read(PROC_SELF_CGROUP))
    return Sample(now(), busy, total, parse_psi_total(read(PROC_PSI)),
                  parse_psi_total(read(cg)) if cg else None)


@dataclasses.dataclass(frozen=True)
class Conditions:
    """@brief What the machine was doing during one invocation, and the verdict on it."""

    cpus: tuple[int, ...]
    pinned: bool
    wall_s: float
    foreign_pct: float
    own_cpu_s: float
    nivcsw: int
    pressure: float | None       # in-window `some` share, percent, from `pressure_source`;
                                 # None when absent or the run was too short to measure
    verdict: str
    reason: str
    pressure_source: str = "host"  # "cgroup" when pinned, "host" when unpinned
    host_pressure: float | None = None  # host-wide in-window share — information only
    pressure_scored: bool = True  # False: a multi-threaded run, judged on foreign time only

    @property
    def clean(self) -> bool:
        """@brief True when this invocation may carry a verdict."""
        return self.verdict == CLEAN

    def line(self) -> str:
        """@brief One human-auditable line: the numbers and the verdict path."""
        psi = "n/a" if self.pressure is None else f"{self.pressure:.1f}"
        if not self.pressure_scored:
            psi += " (not scored: multi-threaded)"
        elif self.wall_s < PSI_MIN_WINDOW_S:
            psi += " (not scored: window too short)"
        host = ("" if self.pressure_source == "host" or self.host_pressure is None
                else f" (host {self.host_pressure:.1f}, info)")
        return (f"{_cpuset(self.cpus)}{'' if self.pinned else ' (unpinned)'}: "
                f"foreign {self.foreign_pct:.1f}% of {self.wall_s:.1f}s, "
                f"{self.pressure_source} psi {psi}{host}, "
                f"nivcsw {self.nivcsw} -> {self.verdict}"
                + (f" ({self.reason})" if self.reason else ""))


def cpu_list(cpus: Iterable[int]) -> str:
    """@brief `3-6` or `0-3,6`: the kernel's cpu-list spelling, as `taskset -c` takes it."""
    c = sorted(set(cpus))
    runs, start = [], None
    for i, x in enumerate(c):
        if start is None:
            start = x
        if i + 1 == len(c) or c[i + 1] != x + 1:
            runs.append(f"{start}" if start == x else f"{start}-{x}")
            start = None
    return ",".join(runs)


def parse_cpu_list(spec: str) -> tuple[int, ...]:
    """@brief `2`, `2,3` or `2-6` (the kernel's cpu-list spelling) -> sorted CPU numbers."""
    out: set[int] = set()
    for part in spec.strip().split(","):
        if part.strip():
            lo, _, hi = part.strip().partition("-")
            out.update(range(int(lo), int(hi or lo) + 1))
    return tuple(sorted(out))


def _cpuset(cpus: Iterable[int]) -> str:
    """@brief `cpu2` or `cpus 0-3,6` — compact and unambiguous."""
    c = sorted(set(cpus))
    return f"cpu{c[0]}" if len(c) == 1 else "cpus " + cpu_list(c)


def off_cpus(allowed: Iterable[int], bench: Iterable[int]) -> tuple[int, ...]:
    """@brief Where the job's OWN processes go so they never queue on the bench CPU (#1890).

    @p allowed minus @p bench. When nothing would be left, @p allowed unchanged: a job
    with one CPU cannot step aside, and a move to an empty set would fail rather than
    leave the conditions check to say so.
    """
    rest = tuple(sorted(set(allowed) - set(bench)))
    return rest or tuple(sorted(set(allowed)))


def job_cpus(read: Callable[[str], str | None] = _read) -> tuple[int, ...]:
    """@brief The CPUs this job's cgroup may use (`cpuset.cpus.effective`).

    Read from the cgroup, not from this process's affinity, which is whatever the runner
    process that spawned it was left with. The nearest cgroup up the path that has the
    file decides (the cpuset controller may be enabled on the slice and not below it);
    falls back to that affinity where none has it.
    """
    cg = cgroup_pressure_path(read(PROC_SELF_CGROUP))
    d = cg.rsplit("/", 1)[0] if cg else ""
    while d.startswith(CGROUP_FS + "/"):  # the root constrains nothing
        text = read(d + "/cpuset.cpus.effective")
        if text and text.strip():
            return parse_cpu_list(text)
        d = d.rsplit("/", 1)[0]
    return tuple(sorted(os.sched_getaffinity(0)))


def _window_pct(us0: int | None, us1: int | None, wall_s: float) -> float | None:
    """@brief Stall microseconds gained over the window as a percent of its wall time;
    None when either reading is absent or the window has no length."""
    if us0 is None or us1 is None or wall_s <= 0:
        return None
    return max(0, us1 - us0) / (wall_s * 1e6) * 100.0


def classify(before: Sample, after: Sample, own_cpu_s: float, nivcsw: int,
             cpus: Iterable[int], pinned: bool = True,
             foreign_max_pct: float = FOREIGN_MAX_PCT,
             pressure_max: float = PRESSURE_MAX, clk_tck: int = CLK_TCK,
             score_pressure: bool = True) -> Conditions:
    """@brief The decision rule: CLEAN, or CONTENDED with the reason spoken.

    @p score_pressure False judges the run on foreign time alone (#1803). That is for a
    MULTI-threaded bench invocation, whose own threads queue behind each other on the
    pinned CPUs and so raise its own cgroup's pressure without anything foreign present.
    Foreign time still catches a real intruder; the pressure is recorded, not scored.

    Pressure is the run's own: the growth of the PSI `some total=` counter over the
    window as a share of its wall time. A run shorter than `PSI_MIN_WINDOW_S` has no
    measurable share and is judged on foreign time alone (#1839).

    Pinned runs gate on foreign time and the job's OWN cgroup pressure only; host-wide
    pressure is recorded beside them and never decides. Unpinned runs, which have no
    isolation to lean on, gate on foreign time and host-wide pressure.

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
    source = "cgroup" if pinned else "host"
    wall_s = max(0, after.wall_ns - before.wall_ns) / 1e9
    psi = _window_pct(before.cg_us, after.cg_us, wall_s) if pinned else \
        _window_pct(before.psi_us, after.psi_us, wall_s)
    if (score_pressure and wall_s >= PSI_MIN_WINDOW_S and psi is not None
            and psi > pressure_max):
        reasons.append(f"{source} psi {psi:.1f} > {pressure_max:g}")
    return Conditions(cpus=cpus, pinned=pinned, wall_s=wall_s,
                      foreign_pct=foreign_pct, own_cpu_s=own_cpu_s, nivcsw=nivcsw,
                      pressure=psi,
                      verdict=CONTENDED if reasons else CLEAN, reason="; ".join(reasons),
                      pressure_source=source,
                      host_pressure=_window_pct(before.psi_us, after.psi_us, wall_s),
                      pressure_scored=score_pressure)


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
    return parse_cpu_list(spec) if spec else None


def single_cpu_from_env(cpus: tuple[int, ...] | None) -> tuple[int, ...] | None:
    """@brief The ONE logical CPU a single-threaded bench step is pinned to (#1807).

    `BENCH_CPU_SINGLE` when set, else the first CPU of @p cpus (the `BENCH_CPU` set), else
    None (unpinned). One CPU, because a single-threaded measurement spread over several lets
    the scheduler migrate it mid-window; the set is for the multi-threaded families.
    """
    spec = os.environ.get("BENCH_CPU_SINGLE", "").strip()
    if spec:
        return (int(spec),)
    return (cpus[0],) if cpus else None


def _run_once(argv: list[str], cpus: tuple[int, ...] | None, timeout: float | None,
              env: dict | None, score_pressure: bool = True) -> tuple[str, str, int, Conditions]:
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
    cond = classify(before, after, own, ru1.ru_nivcsw - ru0.ru_nivcsw, measured, pinned,
                    score_pressure=score_pressure)
    return p.stdout, p.stderr, p.returncode, cond


def measure(argv: list[str], cpus: tuple[int, ...] | None = None,
            attempts: int = DEFAULT_ATTEMPTS, timeout: float | None = None,
            env: dict | None = None, log: Callable[[str], None] | None = None,
            run: Callable = _run_once, score_pressure: bool = True) -> Measurement:
    """@brief Run one timed bench invocation until it runs clean, at most @p attempts times.

    Returns the first clean attempt, or the last one with its CONTENDED conditions — the
    caller never has to decide what "clean" means. @p run is injected for tests (a
    scripted contended-then-clean sequence) and defaults to a real, sampled run.
    @p score_pressure False judges every attempt on foreign time only (see `classify`).
    """
    tried: list[Conditions] = []
    out = err = ""
    rc = 0
    for i in range(max(1, attempts)):
        out, err, rc, cond = (run(argv, cpus, timeout, env) if score_pressure
                              else run(argv, cpus, timeout, env, score_pressure=False))
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
        hosts = [c.host_pressure for c in kept if c.host_pressure is not None]
        host = (f" (host psi <={max(hosts):.1f}, info)"
                if kept[0].pressure_source != "host" and hosts else "")
        n_clean = sum(c.clean for c in kept)
        return (f"bench {_cpuset(kept[0].cpus)}{'' if kept[0].pinned else ' unpinned'}: "
                f"foreign <={worst_f:.1f}%, {kept[0].pressure_source} psi <={worst_p}{host}, "
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


# The runner's own processes, by executable name: a shell whose command line merely MENTIONS
# them (the workflow's own `pgrep` step) is not one.
RUNNER_PROCESS = re.compile(r"(^|/)Runner\.(Listener|Worker)$")


def runners_on(procs: dict[int, tuple[str, set[int]]], bench: Iterable[int]) -> list[int]:
    """@brief The runner processes (@p procs: pid -> (executable, CPUs any of its threads
    may run on)) still allowed on a @p bench CPU, sorted (#1890)."""
    b = set(bench)
    return sorted(pid for pid, (cmd, cpus) in procs.items()
                  if RUNNER_PROCESS.search(cmd) and cpus & b)


def _job_procs() -> dict[int, tuple[str, set[int]]]:
    """@brief This job's processes (its cgroup and the cgroups below it): pid -> (argv[0],
    the union of its threads' affinities). A process that exits mid-read is skipped."""
    cg = cgroup_pressure_path(_read(PROC_SELF_CGROUP))
    root = cg.rsplit("/", 1)[0] if cg else ""
    pids: set[int] = set()
    for d, _, files in os.walk(root) if root else ():
        if "cgroup.procs" in files:
            pids.update(int(x) for x in (_read(f"{d}/cgroup.procs") or "").split())
    out = {}
    for pid in pids:
        try:
            cmd = (_read(f"/proc/{pid}/cmdline") or "").split("\0")[0]
            cpus = set()
            for tid in os.listdir(f"/proc/{pid}/task"):
                cpus |= os.sched_getaffinity(int(tid))
        except OSError:
            continue
        out[pid] = (cmd, cpus)
    return out


def _cmd_runner_check(args: argparse.Namespace) -> int:
    """@brief Fail when a runner process of this job may still run on the bench CPU, and
    the job had another CPU to put it on (#1890)."""
    bench = parse_cpu_list(args.cpu)
    if not set(off_cpus(job_cpus(), bench)) - set(bench):
        print(f"bench_conditions: this job has no CPU besides {cpu_list(bench)}; nothing to check")
        return 0
    bad = runners_on(_job_procs(), bench)
    if bad:
        print(f"::error::runner process(es) {bad} may still run on bench CPU "
              f"{cpu_list(bench)}; the measurement would queue them behind the bench")
        return 1
    print(f"bench_conditions: no runner process of this job may run on {cpu_list(bench)}")
    return 0


def _cmd_off_cpus(args: argparse.Namespace) -> int:
    print(cpu_list(off_cpus(job_cpus(), parse_cpu_list(args.cpu))))
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
    o = sub.add_parser("off-cpus", help="this job's CPUs minus the bench CPU, as a cpu list")
    o.add_argument("--cpu", required=True, help="the bench CPU(s) to keep the job's own "
                   "processes off (#1890)")
    o.set_defaults(fn=_cmd_off_cpus)
    c = sub.add_parser("runner-check", help="fail if a runner process may run on the bench CPU")
    c.add_argument("--cpu", required=True, help="the bench CPU(s) (#1890)")
    c.set_defaults(fn=_cmd_runner_check)
    args = ap.parse_args(argv)
    if args.cmd == "run":
        if args.argv[:1] == ["--"]:
            args.argv = args.argv[1:]
        if not args.argv:
            ap.error("run: nothing to run (usage: run [opts] -- CMD ...)")
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
