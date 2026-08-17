#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Decision-rule tests for the banked-series validity guard (#1301).

Nothing here measures anything, which is the point: every rule the guard applies is a
function of a stored series, so a fixture store is a complete test bed and no host
condition can change an answer. Three properties carry the car —

  * a gap is counted in TRUSTED points, so a contaminated sample never satisfies the
    density guarantee it exists to be distrusted by;
  * the rolling baseline is the window's BEST and never its median, per
    `docs/methodology.md`'s one-sided-contamination rule, and a staircase that the
    point-to-point comparator sleeps through must fire;
  * the compiler identity written by `host_guard.py stamp --compiler` must be
    readable back out of the descriptor it was appended to.

    python3 bench/test_store_guard.py   # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import host_guard as hg  # noqa: E402
import store_guard as sg  # noqa: E402

LATENCY = "libtracer bench-local latency (ns, smaller is better, fixed pinned host)"
THROUGHPUT = "libtracer bench-local throughput (deliveries/s, bigger is better)"
HOST = "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus"
METRIC = "inproc 64B/fan1/1ep p50 latency"


def sha(n: int) -> str:
    """@brief A distinct 40-hex commit id for fixture entry @p n."""
    return f"{n:040x}"


def entry(n: int, value: float, extra: str = HOST, name: str = METRIC) -> dict:
    """@brief One stored point, in benchmark-action's exact entry shape."""
    return {"commit": {"id": sha(n), "message": f"commit {n}"}, "date": 1700000000 + n,
            "benches": [{"name": name, "value": value, "unit": "ns", "extra": extra}]}


def store(entries: list[dict], suite: str = LATENCY) -> dict:
    """@brief A one-suite `data.js` document."""
    return {"entries": {suite: entries}}


class ParseStore(unittest.TestCase):
    """@brief data.js is JavaScript; a half-fetched one must not read as empty."""

    def test_assignment_is_unwrapped(self):
        doc = sg.parse_data_js('window.BENCHMARK_DATA = {"entries": {"a": []}};\n')
        self.assertEqual(doc["entries"], {"a": []})

    def test_garbage_raises_rather_than_returning_an_empty_series(self):
        with self.assertRaises(ValueError):
            sg.parse_data_js("not a store at all")

    def test_suite_direction_is_read_off_the_name(self):
        self.assertFalse(sg.suite_is_bigger_better(LATENCY))
        self.assertTrue(sg.suite_is_bigger_better(THROUGHPUT))


class Density(unittest.TestCase):
    """@brief How far the store lags `main`, counted in commits that could have moved it."""

    def test_gap_is_zero_when_head_itself_is_banked(self):
        doc = store([entry(3, 100)])
        rep = sg.density_report(doc, [sha(3), sha(2), sha(1)], known={})
        self.assertEqual(rep["gap"], 0)

    def test_gap_counts_the_commits_ahead_of_the_last_point(self):
        doc = store([entry(1, 100)])
        rep = sg.density_report(doc, [sha(4), sha(3), sha(2), sha(1)], known={})
        self.assertEqual(rep["gap"], 3)

    def test_a_store_with_no_matching_point_is_the_full_history(self):
        rep = sg.density_report(store([entry(9, 100)]), [sha(2), sha(1)], known={})
        self.assertEqual(rep["gap"], 2)

    def test_a_contaminated_point_is_not_coverage(self):
        """The defect this guarantee exists for: a banked point nobody may believe."""
        flagged = f"{HOST} · {hg.contamination_note('A/A bracket 12.0% > 6.0% band')}"
        doc = store([entry(1, 100), entry(3, 100, extra=flagged)])
        rep = sg.density_report(doc, [sha(3), sha(2), sha(1)], known={})
        self.assertEqual(rep["gap"], 2, "a flagged sample must not close the gap")

    def test_the_worst_suite_decides(self):
        """A half-recorded point (one store written, one not) must not read as covered."""
        doc = {"entries": {LATENCY: [entry(3, 100)], THROUGHPUT: [entry(1, 100)]}}
        rep = sg.density_report(doc, [sha(3), sha(2), sha(1)], known={})
        self.assertEqual(rep["gap"], 2)

    def test_empty_store_reports_the_whole_range(self):
        rep = sg.density_report({"entries": {}}, [sha(2), sha(1)], known={})
        self.assertEqual(rep["gap"], 2)


class RollingBaseline(unittest.TestCase):
    """@brief The estimator rule, inherited from docs/methodology.md and not restated."""

    def test_baseline_is_the_best_of_the_window_not_the_median(self):
        vals = [100.0, 101.0, 140.0, 145.0, 150.0]  # two dirty points at the end
        self.assertEqual(sg._window_best(vals, bigger_is_better=False), 100.0)
        self.assertEqual(sg._window_best(vals, bigger_is_better=True), 150.0)

    def test_a_staircase_fires_though_every_step_is_small(self):
        """Five +4% merges: point-to-point never trips 115%, the baseline does."""
        vals, v = [], 100.0
        for i in range(12):
            vals.append(entry(i, round(v, 3)))
            v *= 1.04
        rep = sg.drift_alerts(vals, bigger_is_better=False, window=20, threshold=15.0,
                              min_points=5, known={})
        self.assertEqual(rep["status"], "ok")
        self.assertEqual(len(rep["alerts"]), 1)
        a = rep["alerts"][0]
        self.assertGreater(a["pct"], 15.0)
        self.assertLess(a["pct_vs_prev"], 5.0,
                        "the point-to-point comparator must be shown staying silent")

    def test_spread_inside_the_windows_own_range_does_not_alert(self):
        """A best-of-window baseline is a FLOOR; without this the bar fires on noise.

        Measured on the real bench-local store: 59 metrics breach 15% against the
        window's best, and 2 survive the range guard.
        """
        entries = [entry(i, 100.0 + 20.0 * (i % 2)) for i in range(11)]
        entries.append(entry(11, 119.0))  # +19% on the best, inside the 100..120 range
        rep = sg.drift_alerts(entries, bigger_is_better=False, known={})
        self.assertEqual(rep["alerts"], [])
        loud = sg.drift_alerts(entries, bigger_is_better=False, known={},
                               range_guard=False)
        self.assertEqual(len(loud["alerts"]), 1, "the guard, not the bar, is what "
                                                 "rejects it")

    def test_a_move_outside_the_range_still_alerts(self):
        entries = [entry(i, 100.0 + 20.0 * (i % 2)) for i in range(11)]
        entries.append(entry(11, 160.0))
        rep = sg.drift_alerts(entries, bigger_is_better=False, known={})
        self.assertEqual(len(rep["alerts"]), 1)
        self.assertEqual(rep["alerts"][0]["window_worst"], 120.0)

    def test_the_range_guard_reads_the_right_end_when_bigger_is_better(self):
        entries = [entry(i, 1000.0 - 200.0 * (i % 2)) for i in range(11)]
        entries.append(entry(11, 820.0))  # -18% on the best, inside 800..1000
        self.assertEqual(sg.drift_alerts(entries, bigger_is_better=True,
                                         known={})["alerts"], [])
        entries[-1] = entry(11, 700.0)
        self.assertEqual(len(sg.drift_alerts(entries, bigger_is_better=True,
                                             known={})["alerts"]), 1)

    def test_a_flat_series_is_clean(self):
        rep = sg.drift_alerts([entry(i, 100 + (i % 2)) for i in range(12)],
                              bigger_is_better=False, known={})
        self.assertEqual(rep["alerts"], [])

    def test_a_contaminated_point_neither_alerts_nor_sets_the_baseline(self):
        flagged = f"{HOST} · {hg.contamination_note('A/A bracket 40.0% > 6.0% band')}"
        entries = [entry(i, 100) for i in range(11)] + [entry(11, 400, extra=flagged)]
        rep = sg.drift_alerts(entries, bigger_is_better=False, known={})
        self.assertEqual(rep["alerts"], [], "a flagged sample cannot raise an alert")

    def test_bigger_is_better_alerts_on_a_fall(self):
        entries = [entry(i, 1000.0) for i in range(11)] + [entry(11, 700.0)]
        rep = sg.drift_alerts(entries, bigger_is_better=True, known={})
        self.assertEqual(len(rep["alerts"]), 1)
        self.assertAlmostEqual(rep["alerts"][0]["pct"], 30.0, places=3)

    def test_bigger_is_better_does_not_alert_on_a_rise(self):
        entries = [entry(i, 1000.0) for i in range(11)] + [entry(11, 1400.0)]
        self.assertEqual(sg.drift_alerts(entries, bigger_is_better=True)["alerts"], [])

    def test_a_new_series_is_not_judged_against_a_history_it_lacks(self):
        entries = [entry(i, 100) for i in range(11)]
        entries.append(entry(11, 900, name="brand new metric"))
        rep = sg.drift_alerts(entries, bigger_is_better=False, known={})
        self.assertEqual(rep["alerts"], [])

    def test_too_few_trusted_points_declines_to_judge(self):
        rep = sg.drift_alerts([entry(i, 100) for i in range(3)],
                              bigger_is_better=False, known={})
        self.assertEqual(rep["status"], "insufficient")

    def test_a_step_change_alerts_once_and_then_stops(self):
        """An accepted step must not alert forever, or the alert stops being read.

        The first point after the step is outside everything behind it and fires; the
        next one is inside the range the step itself established and does not. The
        `--window` bound is the second, slower expiry — the step ages out of the
        baseline entirely.
        """
        flat = [entry(i, 100.0) for i in range(10)]
        first = sg.drift_alerts(flat + [entry(10, 140.0)], bigger_is_better=False,
                                window=20, known={})
        second = sg.drift_alerts(flat + [entry(10, 140.0), entry(11, 140.0)],
                                 bigger_is_better=False, window=20, known={})
        self.assertEqual(len(first["alerts"]), 1)
        self.assertEqual(second["alerts"], [])

    def test_the_window_bounds_how_far_back_the_baseline_reaches(self):
        aged = [entry(i, 100.0) for i in range(3)] + \
               [entry(3 + i, 140.0 + i) for i in range(9)]
        wide = sg.drift_alerts(aged, bigger_is_better=False, window=20, known={},
                               range_guard=False)
        narrow = sg.drift_alerts(aged, bigger_is_better=False, window=6, known={},
                                 range_guard=False)
        self.assertEqual(len(wide["alerts"]), 1, "the old 100s are still the best")
        self.assertEqual(narrow["alerts"], [], "they have aged out of a short window")


class ToolchainIdentity(unittest.TestCase):
    """@brief The read side of `host_guard.py stamp --compiler`."""

    def test_the_stamped_identity_reads_back_out_of_the_descriptor(self):
        desc = HOST + hg.SEP + hg.compiler_identity("definitely-not-a-compiler-xyz")
        self.assertIsNone(sg.compiler_of(desc), "'compiler unknown' carries no version")
        self.assertEqual(sg.compiler_of(HOST + hg.SEP + "g++ 13.3.0"), "g++ 13.3.0")

    def test_host_fields_are_never_mistaken_for_a_compiler(self):
        kernel = HOST + hg.SEP + "governor not-exposed" + hg.SEP + "7.0.11-76070011-generic"
        self.assertIsNone(sg.compiler_of(kernel))

    def test_the_contamination_note_is_not_a_compiler(self):
        extra = HOST + hg.SEP + hg.contamination_note("A/A bracket 12.0% > 6.0% band")
        self.assertIsNone(sg.compiler_of(extra))

    def test_a_bump_is_reported_and_a_first_recording_is_not(self):
        gcc, clang = HOST + hg.SEP + "g++ 13.3.0", HOST + hg.SEP + "clang 18.1.3"
        entries = [entry(0, 100), entry(1, 100, extra=gcc), entry(2, 100, extra=gcc),
                   entry(3, 100, extra=clang)]
        tl = sg.toolchain_timeline(entries)
        self.assertEqual([(c["from"], c["to"]) for c in tl],
                         [("g++ 13.3.0", "clang 18.1.3")])
        self.assertEqual(tl[0]["sha"], sha(3))

    def test_drift_reports_a_crossing_inside_its_own_window(self):
        gcc, clang = HOST + hg.SEP + "g++ 13.3.0", HOST + hg.SEP + "clang 18.1.3"
        entries = [entry(i, 100.0, extra=gcc) for i in range(10)] + \
                  [entry(10, 130.0, extra=clang)]
        rep = sg.drift_alerts(entries, bigger_is_better=False, known={})
        self.assertEqual(len(rep["alerts"]), 1)
        self.assertTrue(rep["toolchain"],
                        "an alert whose window crosses a bump must say so")


class CommandLine(unittest.TestCase):
    """@brief The exit-code contract the workflow depends on."""

    def _data(self, tmp: pathlib.Path, doc_entries: list[dict]) -> str:
        p = tmp / "data.js"
        import json
        p.write_text("window.BENCHMARK_DATA = " + json.dumps(store(doc_entries)) + ";\n")
        return str(p)

    def test_drift_in_warn_mode_never_fails_the_job(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            data = self._data(pathlib.Path(d),
                              [entry(i, 100) for i in range(11)] + [entry(11, 400)])
            self.assertEqual(sg.main(["drift", "--data", data]), 0)
            self.assertEqual(sg.main(["drift", "--data", data, "--mode", "fail"]), 1)

    def test_density_reports_without_failing_by_default(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            data = self._data(pathlib.Path(d), [entry(1, 100)])
            shas = pathlib.Path(d) / "shas.txt"
            shas.write_text("\n".join(sha(i) for i in (9, 8, 7, 6, 5, 4, 3, 2, 1)) + "\n")
            self.assertEqual(sg.main(["density", "--data", data, "--shas-file",
                                      str(shas), "--max-gap", "4"]), 0)
            self.assertEqual(sg.main(["density", "--data", data, "--shas-file",
                                      str(shas), "--max-gap", "4", "--mode", "fail"]), 1)

    def test_a_missing_store_is_an_unbounded_gap_not_a_crash(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            shas = pathlib.Path(d) / "shas.txt"
            shas.write_text(sha(1) + "\n")
            self.assertEqual(sg.main(["density", "--data", str(pathlib.Path(d) / "nope"),
                                      "--shas-file", str(shas)]), 0)


class WorkflowWiring(unittest.TestCase):
    """@brief The guarantee is the WORKFLOW's, not the tool's.

    Every rule above is worth nothing if nothing calls it, and a comparator that runs
    nowhere reads exactly like one that never fires. These assert the three legs are
    wired, in the one file that can honour them.
    """

    WF = pathlib.Path(__file__).resolve().parents[1] / ".github" / "workflows" \
        / "perf-local.yml"

    def setUp(self):
        if not self.WF.is_file():
            self.skipTest(f"{self.WF} not present")  # bench/ checked out alone
        self.text = self.WF.read_text()

    def test_a_catch_up_trigger_exists(self):
        """Push-to-main behind a paths: filter cannot bound the gap on its own."""
        self.assertIn("schedule:", self.text)
        self.assertRegex(self.text, r"cron:\s*'")

    def test_the_density_check_runs_and_gates_the_measurement(self):
        self.assertRegex(self.text, r"store_guard\.py density")
        self.assertIn("needs.density.outputs.measure", self.text)

    def test_the_rolling_comparator_and_the_toolchain_read_are_wired(self):
        self.assertRegex(self.text, r"store_guard\.py drift")
        self.assertRegex(self.text, r"store_guard\.py toolchain")

    def test_the_rolling_comparator_stays_soft(self):
        """This workflow renders no pass/fail on a commit; the drift leg must not
        become the first one to, silently, via a flag."""
        for line in self.text.splitlines():
            if "store_guard.py drift" in line and not line.strip().startswith("#"):
                self.assertNotIn("--mode fail", line)


if __name__ == "__main__":
    unittest.main(verbosity=2)
