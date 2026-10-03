#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""@brief Decision-rule tests for the sustained-step detector (#1770).

Two halves. The synthetic half pins the rule itself: the hold length, the per-row
threshold (spread, clock tick, A/A floor) and where a step is placed. The store half runs
the detector over real bench-local values (the last 40 recorded commits of the store as of
cff17dee) and pins the two cases the ticket names: the 2026-10-01 1 KiB step is found, and
rows that only carry noise (including cff17dee's one-point spike) are not.

    python3 bench/test_step_detect.py    # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import host_guard  # noqa: E402
import render_history as rh  # noqa: E402
import step_detect as sd  # noqa: E402

JS = pathlib.Path(__file__).resolve().parent.parent / "docs" / "_static" / "perf_history.js"


def _pts(values: list[float]) -> list[list[float]]:
    """@brief A row as the detector reads it: [[entry_idx, value], ...]."""
    return [[i, float(v)] for i, v in enumerate(values)]


def _noisy(level: float, n: int, amp: float) -> list[float]:
    """@brief A deterministic noisy level: a repeating +amp, -amp, 0 pattern (relative)."""
    pat = (1.0 + amp, 1.0 - amp, 1.0)
    return [level * pat[i % 3] for i in range(n)]


class Constants(unittest.TestCase):
    """@brief The rule's knobs are named, and the two the ticket fixes keep their values."""

    def test_hold_and_floor(self):
        self.assertEqual(sd.HOLD_POINTS, 3)
        self.assertAlmostEqual(sd.STEP_FLOOR, host_guard.DEFAULT_BAND / 100.0)
        for name in ("BASELINE_POINTS", "MIN_BASELINE_POINTS", "STEP_SIGMAS", "STEP_TICKS",
                     "RECENT_POINTS", "PAGE_ROWS"):
            self.assertTrue(hasattr(sd, name), name)


class HoldRule(unittest.TestCase):
    """@brief A move is a step only when HOLD_POINTS consecutive points agree."""

    def test_step_that_holds_is_found_at_its_first_point(self):
        row = [100.0] * 10 + [150.0] * sd.HOLD_POINTS
        found = sd.find_steps(_pts(row))
        self.assertEqual([s["i"] for s in found], [10])
        self.assertEqual((found[0]["before"], found[0]["after"]), (100.0, 150.0))

    def test_one_point_short_of_the_hold_is_not_a_step(self):
        row = [100.0] * 10 + [150.0] * (sd.HOLD_POINTS - 1) + [100.0] * 5
        self.assertEqual(sd.find_steps(_pts(row)), [])

    def test_single_and_double_spikes_are_not_steps(self):
        row = [100.0] * 10 + [180.0] + [100.0] * 5 + [180.0, 180.0] + [100.0] * 5
        self.assertEqual(sd.find_steps(_pts(row)), [])

    def test_a_spike_inside_the_baseline_does_not_move_it(self):
        row = [100.0] * 5 + [300.0] + [100.0] * 4 + [150.0] * 5
        found = sd.find_steps(_pts(row))
        self.assertEqual([(s["i"], s["before"]) for s in found], [(10, 100.0)])

    def test_step_down_and_back_up_are_two_steps(self):
        row = [100.0] * 8 + [60.0] * 8 + [100.0] * 8
        self.assertEqual([s["i"] for s in sd.find_steps(_pts(row))], [8, 16])

    def test_transitional_point_does_not_take_the_step(self):
        # The first point past the bar sits nearer the old level than the new one: it is
        # noise before the step, and the step belongs to the point after it.
        row = [100.0] * 10 + [115.0, 200.0, 200.0, 200.0, 200.0]
        self.assertEqual([s["i"] for s in sd.find_steps(_pts(row))], [11])

    def test_short_row_is_not_scanned(self):
        self.assertEqual(sd.find_steps(_pts([1.0, 2.0, 2.0, 2.0])), [])


class PerRowThreshold(unittest.TestCase):
    """@brief The bar is the row's own: spread, clock tick, and never below the A/A band."""

    def test_same_move_is_a_step_on_a_quiet_row_and_noise_on_a_noisy_one(self):
        quiet = _noisy(100.0, 15, 0.01) + [120.0] * 4
        noisy = _noisy(100.0, 15, 0.09) + [120.0] * 4
        self.assertEqual([s["i"] for s in sd.find_steps(_pts(quiet))], [15])
        self.assertEqual(sd.find_steps(_pts(noisy)), [])
        self.assertLess(sd.row_threshold(quiet), sd.row_threshold(noisy))

    def test_noisy_row_still_sees_a_large_step(self):
        noisy = _noisy(100.0, 15, 0.09) + _noisy(200.0, 6, 0.09)
        self.assertEqual([s["i"] for s in sd.find_steps(_pts(noisy))], [15])

    def test_one_clock_tick_is_never_a_step(self):
        # A p50 row in 10 ns ticks: mostly 120 with an occasional 130, so its median
        # point-to-point move is zero and only the tick term stands between a one-tick
        # settle and a "step".
        row = [120.0, 120.0, 120.0, 120.0, 130.0] * 6 + [130.0] * 6
        self.assertEqual(sd.row_spread(row), 0.0)
        self.assertEqual(sd.find_steps(_pts(row)), [])

    def test_two_clock_ticks_can_be(self):
        row = [120.0, 120.0, 120.0, 120.0, 130.0] * 6 + [140.0] * 6
        self.assertEqual([s["i"] for s in sd.find_steps(_pts(row))], [30])

    def test_floor_is_the_host_guard_band(self):
        below = [100.0] * 12 + [100.0 * (1 + sd.STEP_FLOOR * 0.8)] * 5
        above = [100.0] * 12 + [100.0 * (1 + sd.STEP_FLOOR * 1.5)] * 5
        self.assertEqual(sd.find_steps(_pts(below)), [])
        self.assertEqual([s["i"] for s in sd.find_steps(_pts(above))], [12])


# The last 40 recorded commits of the bench-local latency suite as of cff17dee, and the
# p50 values those entries carry for each row (contaminated samples included, as the
# detector reads them). Entry 33 is 45df56f3, the first recorded commit after #1660
# (RFC-0028 S10, 0009af56) merged; entry 39 is cff17dee.
SHAS = ["ce5f48a1", "0b081d95", "e7a205a0", "979af9da", "23b25430", "bc813f33", "87f619e5",
        "e08c0504", "a6283613", "a02181ae", "cae7e276", "05a7b942", "d34d612d", "3935d15f",
        "e89351ec", "537f8194", "a8f808a3", "aa218094", "09e6959b", "e6578e9f", "c722f806",
        "5fe4370e", "6f135e8f", "d237342a", "a2dc4a03", "41e9cb2b", "60c47146", "57af3dbc",
        "3fc129f2", "bf3f33e7", "2999f8af", "4b427ae4", "1db1cd93", "45df56f3", "8ae2dccd",
        "93b6d521", "09b82320", "cfa1e1df", "8eb31537", "cff17dee"]
STEPPED = {
    "lkv-store-heap 1024B/fan1/1ep p50 latency":
        [28, 57, 27, 28, 32, 36, 29, 27, 27, 28, 30, 30, 34, 37, 34, 57, 28, 28, 34, 30, 28, 29,
         31, 31, 31, 27, 28, 53, 32, 26, 28, 36, 28, 56, 49, 63, 56, 54, 60, 65],
    "lkv-alloc-heap 1024B/fan1/1ep p50 latency":
        [19, 41, 19, 21, 19, 19, 19, 19, 20, 19, 20, 19, 19, 30, 19, 39, 19, 20, 19, 19, 18, 20,
         17, 17, 17, 17, 22, 36, 18, 18, 18, 17, 23, 44, 39, 50, 59, 48, 47, 54],
    # Above 1 KiB: the in-process payload sweep moved in the same window.
    "inproc 8192B/fan1/1ep p50 latency":
        [170, 170, 170, 170, 170, 170, 170, 180, 180, 170, 180, 170, 170, 180, 170, 190, 170,
         190, 170, 170, 170, 170, 170, 170, 170, 170, 170, 180, 280, 180, 190, 180, 190, 310,
         260, 270, 220, 230, 230, 220],
}
NOISE_ONLY = {
    # cff17dee (the last point) read 18 ns between neighbours at 11-12 ns: one point.
    "lkv-store-pool 64B/fan1/1ep p50 latency":
        [11, 18, 11, 12, 12, 11, 11, 11, 12, 11, 11, 12, 11, 11, 11, 18, 12, 12, 11, 11, 12, 11,
         11, 11, 11, 11, 11, 19, 12, 11, 11, 11, 12, 13, 14, 15, 12, 12, 11, 18],
    "lkv-store-pool 1024B/fan1/1ep p50 latency":
        [14, 21, 14, 16, 21, 16, 14, 16, 16, 14, 16, 16, 15, 14, 15, 26, 16, 16, 16, 15, 16, 15,
         15, 16, 15, 16, 15, 22, 14, 16, 21, 15, 21, 21, 17, 23, 22, 15, 14, 21],
    "lkv-alloc-pool 64B/fan1/1ep p50 latency":
        [7, 9, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 9, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 11, 7,
         7, 7, 7, 7, 8, 7, 8, 7, 7, 7, 8],
}
LATENCY = "libtracer bench-local latency (ns, smaller is better, fixed pinned host)"
THROUGHPUT = "libtracer bench-local throughput (deliveries/s, bigger is better, fixed pinned host)"


def _store() -> dict:
    """@brief The fixture rows as a two-suite benchmark-action store."""
    rows = {**STEPPED, **NOISE_ONLY}
    lat = [{"commit": {"id": sha + "0" * 32, "message": f"commit {n}"},
            "benches": [{"name": name, "value": vals[n], "unit": "ns"}
                        for name, vals in rows.items()]}
           for n, sha in enumerate(SHAS)]
    thr_name = "lkv-store-heap 1024B/fan1/1ep throughput"
    thr = [{"commit": {"id": sha + "0" * 32, "message": f"commit {n}"},
            "benches": [{"name": thr_name, "unit": "deliveries/s",
                         "value": 1e9 / STEPPED["lkv-store-heap 1024B/fan1/1ep p50 latency"][n]}]}
           for n, sha in enumerate(SHAS)]
    return {"entries": {LATENCY: lat, THROUGHPUT: thr}}


class CurrentStore(unittest.TestCase):
    """@brief Real bench-local values: the 1 KiB step is found, noise-only rows are not."""

    def test_1kib_step_is_flagged_at_45df56f3(self):
        for name in ("lkv-store-heap 1024B/fan1/1ep p50 latency",
                     "lkv-alloc-heap 1024B/fan1/1ep p50 latency"):
            found = sd.find_steps(_pts(STEPPED[name]))
            self.assertEqual([SHAS[s["i"]] for s in found], ["45df56f3"], name)
            self.assertGreater(found[0]["after"] / found[0]["before"], 1.9, name)

    def test_rows_above_1kib_are_scanned(self):
        found = sd.find_steps(_pts(STEPPED["inproc 8192B/fan1/1ep p50 latency"]))
        self.assertIn("45df56f3", [SHAS[s["i"]] for s in found])

    def test_noise_only_rows_are_not_flagged(self):
        for name, vals in NOISE_ONLY.items():
            self.assertEqual(sd.find_steps(_pts(vals)), [], name)

    def test_store_steps_direction_and_window(self):
        steps = sd.store_steps(_store(), recent=sd.RECENT_POINTS)
        heap = [s for s in steps if s["row"].startswith("lkv-store-heap 1024B")]
        self.assertEqual({s["suite"] for s in heap}, {LATENCY, THROUGHPUT})
        for s in heap:
            self.assertTrue(s["sha"].startswith("45df56f3"))
            self.assertTrue(s["prev_sha"].startswith("1db1cd93"))
            # Slower on both suites: latency up, throughput down.
            self.assertTrue(s["regression"], s["suite"])
        self.assertFalse([s for s in steps if s["row"] in NOISE_ONLY])

    def test_recent_window_excludes_older_steps(self):
        self.assertEqual(sd.store_steps(_store(), recent=5), [])


class PageAndCharts(unittest.TestCase):
    """@brief The page lists steps, the chart payload carries them, the trend view draws them."""

    def setUp(self):
        self._saved = (rh.instrument_annotations, rh.release_annotations, sd.merge_window)
        rh.instrument_annotations = lambda *a, **k: []
        rh.release_annotations = lambda *a, **k: []
        sd.merge_window = lambda *a, **k: [("0" * 40, "1660")]

    def tearDown(self):
        rh.instrument_annotations, rh.release_annotations, sd.merge_window = self._saved

    def test_page_table_lists_the_step(self):
        block = sd.page_block(_store())
        self.assertIn("| Row | Before → after |", block)
        self.assertIn("`lkv-store-heap 1024B/fan1/1ep p50 latency`", block)
        self.assertIn("[`45df56f`]", block)
        self.assertIn("[#1660]", block)
        self.assertNotIn("lkv-store-pool 64B", block)

    def test_page_without_store_says_so(self):
        self.assertIn("not reachable", sd.page_block(None))

    def test_bench_local_payload_carries_steps_and_hosted_does_not(self):
        local = rh.build(_store(), {}, same_pass=True, steps=True)
        hosted = rh.build(_store(), {}, same_pass=False)
        lkv = next(c for c in local["charts"] if c["id"] == "lkv")
        marked = {s["label"]: s["steps"] for s in lkv["series"] if "steps" in s}
        self.assertEqual([t[0] for t in marked.get("store heap 1024 B", [])], [33])
        self.assertNotIn("store pool 64 B", marked)
        for c in hosted["charts"]:
            for v in c["metrics"]:
                self.assertFalse([s for s in v["series"] if "steps" in s], c["id"])

    def test_trend_view_draws_and_slices_steps(self):
        js = JS.read_text()
        self.assertIn("function stepMark(", js)
        self.assertIn("se.steps", js)
        self.assertIn("s2.steps", js)  # the commit-range slice re-bases step indices


if __name__ == "__main__":
    unittest.main()
