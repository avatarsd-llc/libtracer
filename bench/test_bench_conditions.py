#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Decision-rule tests for the measurement-condition classifier (#1676).

The classifier's whole value is that it says CONTENDED when the bench CPU was not ours
and CLEAN when it was, and that a gate built on it prints INCONCLUSIVE rather than PASS
or FAIL on a contended run. Those directions are what these tests pin. The /proc text is
synthetic and the runner is scripted, so "contended, then clean on the re-run" is a test
case rather than a hope about the machine CI happens to draw.

One live case closes the loop against a real kernel: a busy-loop neighbour pinned to the
same CPU as a measured process must be reported CONTENDED. It picks the highest CPU in
this process's own affinity set, never a fixed number, so it cannot land on the bench
host's reserved CPUs from a developer's shell.

    python3 bench/test_bench_conditions.py    # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import contextlib
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import bench_conditions as bc  # noqa: E402
import host_guard as hg  # noqa: E402
import perf_gate as pg  # noqa: E402


def proc_stat(cpu: int, user: int, system: int, idle: int, steal: int = 0,
              irq: int = 0) -> str:
    """@brief A /proc/stat excerpt with an aggregate line and two per-CPU lines.

    The aggregate `cpu` line and a neighbour CPU carry junk on purpose: the parser must
    read only the measured CPU's line.
    """
    return (f"cpu  999999 0 999999 999999 0 0 0 0 0 0\n"
            f"cpu{cpu} {user} 0 {system} {idle} 5 {irq} 0 {steal} 0 0\n"
            f"cpu{cpu + 1} 777777 0 777777 1 0 0 0 0 0 0\n"
            f"intr 12345\n")


def psi(total_us: int, avg10: float = 0.0) -> str:
    """@brief A /proc/pressure/cpu body with `some total=` @p total_us.

    @p avg10 is decoration the rule must ignore: a stale moving average is what #1839
    removed from the decision.
    """
    return (f"some avg10={avg10:.2f} avg60=0.00 avg300=0.00 total={total_us}\n"
            f"full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n")


def us(pct: float, wall_s: float = 10.0) -> int:
    """@brief The stall microseconds that are @p pct percent of a @p wall_s window."""
    return round(pct / 100.0 * wall_s * 1e6)


CG = "/runners.slice/actions.runner.studio-bench.service"


def sample(stat: str, pressure: int | None, wall_ns: int, cpu: int = 7,
           cgroup: int | None = None, avg10: float = 0.0) -> bc.Sample:
    """@brief One snapshot built from synthetic /proc text through the real reader.

    @p pressure is the host-wide PSI `some total=` (µs); @p cgroup, when given, is this
    job's own cgroup counter, reached the way the real reader reaches it — through
    /proc/self/cgroup. @p avg10 decorates both files and must never decide.
    """
    files = {bc.PROC_STAT: stat,
             bc.PROC_PSI: None if pressure is None else psi(pressure, avg10)}
    if cgroup is not None:
        files[bc.PROC_SELF_CGROUP] = f"0::{CG}\n"
        files[f"{bc.CGROUP_FS}{CG}/cpu.pressure"] = psi(cgroup, avg10)
    return bc.snapshot((cpu,), read=files.get, now=lambda: wall_ns)


# A 10 s window on one CPU at USER_HZ=100 is 1000 ticks.
T0 = sample(proc_stat(7, user=1000, system=500, idle=8000), 0, 0)


def after(extra_busy: int, extra_idle: int, pressure: int | None = 0,
          steal: int = 0) -> bc.Sample:
    """@brief The snapshot 10 s after T0 with @p extra_busy busy ticks in the window."""
    return sample(proc_stat(7, user=1000 + extra_busy, system=500, idle=8000 + extra_idle,
                            steal=steal), pressure, 10_000_000_000)


class ProcParsing(unittest.TestCase):
    """@brief The /proc readers take exactly the fields the rule is defined on."""

    def test_only_the_measured_cpu_is_summed(self):
        busy, total = bc.parse_proc_stat(proc_stat(7, 100, 50, 850, steal=3, irq=2), [7])
        self.assertEqual(busy, 100 + 50 + 2 + 3)       # user+system+irq+steal
        self.assertEqual(total, 100 + 50 + 850 + 5 + 2 + 3)  # + idle + iowait

    def test_iowait_is_not_busy(self):
        busy, _ = bc.parse_proc_stat("cpu3 0 0 0 100 900 0 0 0 0 0\n", [3])
        self.assertEqual(busy, 0)

    def test_psi_total(self):
        self.assertEqual(bc.parse_psi_total(psi(1_250_000, avg10=99.0)), 1_250_000)
        self.assertIsNone(bc.parse_psi_total(None))    # a kernel without PSI

    def test_cpuset_spelling(self):
        self.assertEqual(bc._cpuset([2]), "cpu2")
        self.assertEqual(bc._cpuset([0, 1, 2, 3, 6]), "cpus 0-3,6")


class Classify(unittest.TestCase):
    """@brief The rule: foreign time over the bar, or in-window pressure over the bar."""

    def test_a_clean_run(self):
        # 1000 ticks, all of them the bench's own 10 s of CPU.
        c = bc.classify(T0, after(1000, 0), own_cpu_s=10.0, nivcsw=3, cpus=[7])
        self.assertEqual(c.verdict, bc.CLEAN, c.line())
        self.assertEqual(c.foreign_pct, 0.0)
        self.assertEqual(c.wall_s, 10.0)

    def test_a_contended_run(self):
        # A neighbour took 30% of the CPU: 1000 busy ticks, only 7 s of them ours.
        c = bc.classify(T0, after(1000, 0), own_cpu_s=7.0, nivcsw=900, cpus=[7])
        self.assertEqual(c.verdict, bc.CONTENDED)
        self.assertAlmostEqual(c.foreign_pct, 30.0)
        self.assertIn("foreign 30.0% > 2%", c.reason)

    def test_the_bar_is_two_percent(self):
        just_under = bc.classify(T0, after(1000, 0), own_cpu_s=9.81, nivcsw=0, cpus=[7])
        just_over = bc.classify(T0, after(1000, 0), own_cpu_s=9.79, nivcsw=0, cpus=[7])
        self.assertTrue(just_under.clean, just_under.line())
        self.assertFalse(just_over.clean, just_over.line())

    def test_steal_is_foreign(self):
        # A VM whose hypervisor ran someone else for 5% of the window.
        c = bc.classify(T0, after(950, 0, steal=50), own_cpu_s=9.5, nivcsw=0, cpus=[7])
        self.assertEqual(c.verdict, bc.CONTENDED)

    def test_clock_rounding_is_not_contention(self):
        # A 0.1 s run: 10 ticks, 2 of them "foreign" only because /proc/stat counts in
        # 10 ms ticks and rusage in microseconds. 20% of a tiny window, still clean.
        t1 = sample(proc_stat(7, user=1010, system=500, idle=8000), 0, 100_000_000)
        c = bc.classify(T0, t1, own_cpu_s=0.08, nivcsw=0, cpus=[7])
        self.assertTrue(c.clean, c.line())

    def test_unpinned_host_pressure_in_window_contends(self):
        """Hosted runners have nothing to isolate: host-wide pressure decides there."""
        c = bc.classify(T0, after(1000, 0, pressure=us(41.4)), own_cpu_s=10.0, nivcsw=0,
                        cpus=[7], pinned=False)
        self.assertEqual(c.verdict, bc.CONTENDED)
        self.assertIn("host psi 41.4 > 5", c.reason)

    def test_pinned_host_pressure_high_cgroup_quiet_cpu_clean_is_clean(self):
        """The pinned host's bench CPUs are cgroup-isolated: host-wide pressure of 40-68
        says nothing about them. Own cgroup ~0 and a clean CPU must read CLEAN, with the
        host figure recorded for information."""
        t0 = sample(proc_stat(7, 1000, 500, 8000), 0, 0, cgroup=0)
        t1 = sample(proc_stat(7, 2000, 500, 8000), us(55.2), 10_000_000_000, cgroup=us(0.1))
        c = bc.classify(t0, t1, own_cpu_s=10.0, nivcsw=0, cpus=[7], pinned=True)
        self.assertEqual(c.verdict, bc.CLEAN, c.line())
        self.assertEqual(c.pressure_source, "cgroup")
        self.assertAlmostEqual(c.pressure, 0.1)
        self.assertAlmostEqual(c.host_pressure, 55.2)
        self.assertIn("host 55.2, info", c.line())

    def test_pinned_own_cgroup_pressure_contends(self):
        t0 = sample(proc_stat(7, 1000, 500, 8000), 0, 0, cgroup=0)
        t1 = sample(proc_stat(7, 2000, 500, 8000), 0, 10_000_000_000, cgroup=us(12.5))
        c = bc.classify(t0, t1, own_cpu_s=10.0, nivcsw=0, cpus=[7], pinned=True)
        self.assertEqual(c.verdict, bc.CONTENDED)
        self.assertIn("cgroup psi 12.5 > 5", c.reason)

    def test_cgroup_pressure_path(self):
        self.assertEqual(bc.cgroup_pressure_path(f"0::{CG}\n"),
                         f"{bc.CGROUP_FS}{CG}/cpu.pressure")
        self.assertIsNone(bc.cgroup_pressure_path("12:cpu,cpuacct:/foo\n"))  # v1-only

    def test_stale_avg10_from_a_previous_run_is_not_contention(self):
        """#1839: a short execution right after a multi-threaded one reads avg10 ~50
        left over from the previous run. Only the stall gained in its own window counts:
        none was gained, so a quiet window is CLEAN however high avg10 reads."""
        t0 = sample(proc_stat(7, 1000, 500, 8000), 0, 0, cgroup=9_000_000, avg10=50.8)
        t1 = sample(proc_stat(7, 2000, 500, 8000), 0, 10_000_000_000, cgroup=9_000_000,
                    avg10=50.8)
        c = bc.classify(t0, t1, own_cpu_s=10.0, nivcsw=0, cpus=[7], pinned=True)
        self.assertTrue(c.clean, c.line())
        self.assertEqual(c.pressure, 0.0)

    def test_the_pressure_bar_is_five_percent_of_the_window(self):
        def run(pct):
            t0 = sample(proc_stat(7, 1000, 500, 8000), 0, 0, cgroup=1_000_000)
            t1 = sample(proc_stat(7, 2000, 500, 8000), 0, 10_000_000_000,
                        cgroup=1_000_000 + us(pct))
            return bc.classify(t0, t1, own_cpu_s=10.0, nivcsw=0, cpus=[7], pinned=True)
        self.assertTrue(run(4.9).clean)
        self.assertFalse(run(5.1).clean)

    def test_too_short_to_measure_is_judged_on_foreign_time_alone(self):
        """A 10 ms execution: the stall share of so short a window is noise, so even a
        large one is recorded and not scored. A foreign intruder still contends."""
        t0 = sample(proc_stat(7, 1000, 500, 8000), 0, 0, cgroup=0)
        t1 = sample(proc_stat(7, 1001, 500, 8000), 0, 10_000_000, cgroup=5_000)  # 50%
        quiet = bc.classify(t0, t1, own_cpu_s=0.01, nivcsw=0, cpus=[7], pinned=True)
        self.assertTrue(quiet.clean, quiet.line())
        self.assertAlmostEqual(quiet.pressure, 50.0)
        self.assertIn("not scored: window too short", quiet.line())
        t2 = sample(proc_stat(7, 2000, 500, 8000), 0, 10_000_000_000 + 10_000_000,
                    cgroup=5_000)
        busy = bc.classify(T0, t2, own_cpu_s=7.0, nivcsw=0, cpus=[7], pinned=True)
        self.assertFalse(busy.clean)
        self.assertIn("foreign", busy.reason)
        self.assertNotIn("psi", busy.reason)

    def test_multi_threaded_own_pressure_is_recorded_not_scored(self):
        """#1803: a multi-threaded run queues behind its own threads; its in-window
        pressure is recorded and never decides."""
        t0 = sample(proc_stat(7, 1000, 500, 8000), 0, 0, cgroup=0)
        t1 = sample(proc_stat(7, 2000, 500, 8000), 0, 10_000_000_000, cgroup=us(80.0))
        c = bc.classify(t0, t1, own_cpu_s=10.0, nivcsw=0, cpus=[7], pinned=True,
                        score_pressure=False)
        self.assertTrue(c.clean, c.line())
        self.assertAlmostEqual(c.pressure, 80.0)
        self.assertIn("not scored: multi-threaded", c.line())

    def test_no_psi_is_not_contention(self):
        t0 = sample(proc_stat(7, 1000, 500, 8000), None, 0)
        c = bc.classify(t0, after(1000, 0, pressure=None), own_cpu_s=10.0, nivcsw=0,
                        cpus=[7])
        self.assertTrue(c.clean)
        self.assertIn("psi n/a", c.line())


def scripted(verdicts: list[str]):
    """@brief A `measure` runner that replays a verdict per attempt and counts calls."""
    calls = []

    def run(argv, cpus, timeout, env):
        v = verdicts[len(calls)]
        calls.append(v)
        foreign = 0.0 if v == bc.CLEAN else 30.0
        cond = bc.Conditions(cpus=(7,), pinned=True, wall_s=1.0, foreign_pct=foreign,
                             own_cpu_s=1.0, nivcsw=0, pressure=0.0,
                             verdict=v, reason="" if v == bc.CLEAN else "foreign 30.0% > 2%")
        return f"attempt {len(calls)}\n", "", 0, cond
    return run, calls


class Retry(unittest.TestCase):
    """@brief A contended run is re-run; only a clean one is kept as clean."""

    def test_clean_first_time_runs_once(self):
        run, calls = scripted([bc.CLEAN])
        m = bc.measure(["./bench_x"], run=run)
        self.assertEqual(len(calls), 1)
        self.assertTrue(m.clean)

    def test_contended_then_clean_keeps_the_clean_retry(self):
        run, calls = scripted([bc.CONTENDED, bc.CLEAN])
        logs: list[str] = []
        m = bc.measure(["./bench_x"], attempts=3, run=run, log=logs.append)
        self.assertEqual(len(calls), 2)
        self.assertTrue(m.clean)
        self.assertEqual(m.stdout, "attempt 2\n")        # the contended output is dropped
        self.assertEqual([c.verdict for c in m.attempts], [bc.CONTENDED, bc.CLEAN])
        self.assertIn("re-running (2/3)", logs[0])

    def test_contended_throughout_keeps_the_last_and_says_so(self):
        run, calls = scripted([bc.CONTENDED] * 3)
        m = bc.measure(["./bench_x"], attempts=3, run=run)
        self.assertEqual(len(calls), 3)
        self.assertFalse(m.clean)
        self.assertEqual(m.stdout, "attempt 3\n")


class LedgerVerdict(unittest.TestCase):
    """@brief A job is clean only if every timed invocation is, and says so in the
    token the rest of the store already honours."""

    def ledger(self, *runs: list[str]) -> bc.Ledger:
        led = bc.Ledger()
        for i, verdicts in enumerate(runs):
            run, _ = scripted(verdicts)
            led.add(bc.measure([f"./bench_{i}", "--mode"], attempts=len(verdicts), run=run))
        return led

    def test_all_clean(self):
        led = self.ledger([bc.CLEAN], [bc.CONTENDED, bc.CLEAN])
        self.assertTrue(led.clean)
        self.assertEqual(led.note(), "")
        self.assertIn("2/2 clean (1 re-run)", led.line())

    def test_one_contended_invocation_contaminates_the_job(self):
        led = self.ledger([bc.CLEAN], [bc.CONTENDED, bc.CONTENDED])
        self.assertFalse(led.clean)
        self.assertTrue(hg.is_contaminated(led.note()), led.note())
        self.assertIn("bench_1", led.note())

    def test_record_round_trips_through_jsonl(self):
        led = self.ledger([bc.CLEAN], [bc.CONTENDED, bc.CONTENDED])
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "cond.jsonl"
            led.append_to(p)
            back = bc.Ledger.load(p)
        self.assertEqual(back.line(), led.line())
        self.assertEqual(back.note(), led.note())
        self.assertEqual(bc.Ledger.load(pathlib.Path("/nonexistent")).entries, [])

    def test_a_contended_point_never_enters_the_rolling_baseline(self):
        """The note stamps into a point's `extra`; store_guard.trusted() — the rolling
        drift window and the density count — must drop it without learning a new word."""
        import store_guard as sg
        led = self.ledger([bc.CONTENDED, bc.CONTENDED])
        entries = [
            {"commit": {"id": "a" * 40}, "benches": [{"extra": "box · clean"}]},
            {"commit": {"id": "b" * 40}, "benches": [{"extra": "box · " + led.note()}]},
        ]
        kept = sg.trusted(entries, known={})
        self.assertEqual([e["commit"]["id"][0] for e in kept], ["a"])


class GateIsInconclusive(unittest.TestCase):
    """@brief perf_gate renders INCONCLUSIVE, never PASS or FAIL, on a contended ledger."""

    FAILS = ["inproc/64/1/1 p50 pullback: 400ns vs base 100ns (+300%), reproduced in "
             "4/4 interleaved pairs with disjoint ranges"]

    def verdict(self, fails, tier, ledger):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = pg.render_verdict(list(fails), [], tier, None, ledger)
        return rc, buf.getvalue()

    def contended(self) -> bc.Ledger:
        run, _ = scripted([bc.CONTENDED] * 3)
        led = bc.Ledger()
        led.add(bc.measure(["./bench_libtracer"], run=run))
        return led

    def clean(self) -> bc.Ledger:
        run, _ = scripted([bc.CONTENDED, bc.CLEAN])
        led = bc.Ledger()
        led.add(bc.measure(["./bench_libtracer"], run=run))
        return led

    def test_a_contended_fail_is_inconclusive_not_fail(self):
        rc, out = self.verdict(self.FAILS, "blocking", self.contended())
        self.assertEqual(rc, pg.EXIT_INCONCLUSIVE)
        self.assertIn("PERF: INCONCLUSIVE", out)
        self.assertNotIn("PERF: FAIL", out)
        self.assertIn("  ? " + self.FAILS[0], out)      # the numbers still print
        self.assertNotIn("  ! ", out)
        self.assertIn("Re-run", out)

    def test_a_contended_pass_is_inconclusive_not_pass(self):
        rc, out = self.verdict([], "blocking", self.contended())
        self.assertEqual(rc, pg.EXIT_INCONCLUSIVE)
        self.assertNotIn("PERF: PASS", out)

    def test_advisory_reports_inconclusive_and_exits_zero(self):
        rc, out = self.verdict(self.FAILS, "advisory", self.contended())
        self.assertEqual(rc, 0)
        self.assertIn("PERF: INCONCLUSIVE", out)
        self.assertIn("::warning::", out)

    def test_contended_then_clean_retry_renders_a_real_verdict(self):
        rc, out = self.verdict(self.FAILS, "blocking", self.clean())
        self.assertEqual(rc, 1)
        self.assertIn("PERF: FAIL", out)
        rc, out = self.verdict([], "blocking", self.clean())
        self.assertEqual(rc, 0)
        self.assertIn("PERF: PASS", out)


class LiveContention(unittest.TestCase):
    """@brief Against the real kernel: a neighbour pinned onto the measured CPU is seen."""

    def test_a_busy_neighbour_on_the_bench_cpu_is_contended(self):
        if not hasattr(os, "sched_getaffinity") or not os.path.exists(bc.PROC_STAT):
            self.skipTest("needs Linux /proc and CPU affinity")
        cpu = max(os.sched_getaffinity(0))
        spin = "import time\nt=time.monotonic()+{s}\nwhile time.monotonic()<t: pass\n"
        hog = subprocess.Popen([sys.executable, "-c", spin.format(s=30)],
                               preexec_fn=lambda: os.sched_setaffinity(0, {cpu}))
        try:
            m = bc.measure([sys.executable, "-c", spin.format(s=2)], cpus=(cpu,),
                           attempts=1)
        finally:
            hog.kill()
            hog.wait()
        c = m.conditions
        self.assertEqual(c.verdict, bc.CONTENDED, c.line())
        self.assertGreater(c.foreign_pct, bc.FOREIGN_MAX_PCT, c.line())
        self.assertEqual(c.cpus, (cpu,))


class TheJobStaysOffTheBenchCpu(unittest.TestCase):
    """@brief The bench's own job must not count as contention on the bench CPU (#1890).

    From 2026-10-03 every bench-local point was flagged "bench CPU contended": the job's
    own Runner.Worker had inherited the bench CPU (perf.yml moves the runner there) and
    queued behind the bench, so own-cgroup pressure read 80-100 with under 1% foreign
    time. The fix moves the job's own processes onto the rest of its CPUs before anything
    is timed; these tests pin the rule and that the workflow applies it.
    """

    WORKFLOW = pathlib.Path(__file__).resolve().parent.parent / ".github/workflows/perf-local.yml"

    def test_off_cpus_is_the_job_set_minus_the_bench_cpu(self):
        self.assertEqual(bc.off_cpus((2, 3, 4, 5, 6), (2,)), (3, 4, 5, 6))
        self.assertEqual(bc.cpu_list(bc.off_cpus((2, 3, 4, 5, 6), (2,))), "3-6")

    def test_a_single_cpu_job_is_left_where_it_is(self):
        """Nothing to step aside to: the move is a no-op, never an empty affinity."""
        self.assertEqual(bc.off_cpus((2,), (2,)), (2,))

    def test_job_cpus_come_from_the_nearest_cgroup_cpuset(self):
        """The slice's cpuset decides, not the affinity the shell inherited from the runner."""
        files = {bc.PROC_SELF_CGROUP: "0::/bench.slice/runner.service\n",
                 f"{bc.CGROUP_FS}/bench.slice/cpuset.cpus.effective": "2-6\n"}
        self.assertEqual(bc.job_cpus(files.get), (2, 3, 4, 5, 6))

    def test_cpu_list_round_trips(self):
        for spec in ("2", "3-6", "0-1,7-30"):
            self.assertEqual(bc.cpu_list(bc.parse_cpu_list(spec)), spec)

    def test_the_workflow_moves_the_runner_off_before_anything_is_timed(self):
        # Comments name the commands too; only the steps' own lines count.
        text = "\n".join(ln for ln in self.WORKFLOW.read_text().splitlines()
                         if not ln.lstrip().startswith("#"))
        move = text.find("bench_conditions.py off-cpus --cpu \"$BENCH_CPU\"")
        self.assertGreater(move, 0, "perf-local must move the runner's processes off BENCH_CPU")
        self.assertIn("Runner\\.(Listener|Worker)", text[move - 400:move + 400])
        for timed in ("bench_conditions.py run", "host_guard.py wait"):
            self.assertLess(move, text.find(timed), f"the move must come before `{timed}`")


class CLI(unittest.TestCase):
    """@brief The workflow seam: `run` passes the bench's exit code through and records;
    `summary` reads the record back into one verdict."""

    def test_run_then_summary(self):
        tool = pathlib.Path(bc.__file__)
        with tempfile.TemporaryDirectory() as d:
            rec, out = pathlib.Path(d) / "c.jsonl", pathlib.Path(d) / "raw.txt"
            env = {k: v for k, v in os.environ.items()
                   if k not in ("BENCH_CPU", "GITHUB_OUTPUT", "GITHUB_STEP_SUMMARY")}
            p = subprocess.run([sys.executable, str(tool), "run", "--attempts", "1",
                                "--record", str(rec), "--out", str(out), "--",
                                sys.executable, "-c", "print('RESULT x'); raise SystemExit(7)"],
                               capture_output=True, text=True, env=env)
            self.assertEqual(p.returncode, 7)             # the bench's own exit code
            self.assertEqual(out.read_text(), "RESULT x\n")
            self.assertIn("bench_conditions:", p.stderr)
            self.assertEqual(len(bc.Ledger.load(rec).entries), 1)
            s = subprocess.run([sys.executable, str(tool), "summary", "--record", str(rec)],
                               capture_output=True, text=True, env=env)
            self.assertEqual(s.returncode, 0)             # the verdict is data, not a crash
            self.assertIn("1 timed invocation(s)", s.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
