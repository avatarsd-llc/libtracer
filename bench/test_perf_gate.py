#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Decision-rule tests for the perf gate (`paired_verdict` and the memory ratchet).

These are the gate's OWN unit tests, and they exist because #763 was a defect in the
gate rather than in anything it measured: the shapes below are the recorded sample sets
from the three false failures (#708, #758 twice) and from the real regressions the gate
must keep catching (#385's +33% fan-out step, and the measured -20% synthetic fold-b4
ablation used to demonstrate this fix). A change to the rules that loses either
direction fails here, in a second, instead of on a release train.

The memory-ratchet cases cover the other defect class the gate has now shown: not a
wrong verdict but an ABSENT one — a probe that could not run, printed nothing, and
passed (#792).

    python3 bench/test_perf_gate.py            # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import perf_gate as pg  # noqa: E402


def tput(cand, base):
    """@brief Verdict for the throughput leg (lower is worse)."""
    return pg.paired_verdict(cand, base, pg.TPUT_REGRESS, True)


def lat(cand, base):
    """@brief Verdict for a latency leg (higher is worse, tick-guarded)."""
    return pg.paired_verdict(cand, base, pg.LAT_REGRESS, False, True)


class NoFalseFails(unittest.TestCase):
    """@brief The three recorded false failures must all come back PASS."""

    def test_708_shared_depression_window(self):
        """The #708 shape: a machine depression on pair 3 that both arms fell into."""
        v = tput([252, 251, 161, 255], [258, 251, 236, 250])
        self.assertFalse(v["fail"])
        self.assertFalse(v["disjoint"])  # the populations overlap: indistinguishable

    def test_758_fold_b4_overlapping_distributions(self):
        """The #758 shape: medians x1.01, distributions fully overlapping."""
        v = tput([252, 251, 255, 256], [255, 250, 253, 251])
        self.assertFalse(v["fail"])

    def test_single_low_sample_never_fails_alone(self):
        """One arm draws one very bad sample; every other pair is flat."""
        v = tput([250, 251, 90, 252], [250, 250, 250, 250])
        self.assertFalse(v["fail"])
        self.assertEqual(v["pairs_breached"], 1)

    def test_sign_flip_inside_the_ranges_is_never_a_fail(self):
        """Medians breach, but the candidate's best beats the baseline's worst."""
        v = tput([88, 200, 80, 86], [100, 104, 98, 102])
        self.assertTrue(v["effect"])
        self.assertFalse(v["disjoint"])
        self.assertFalse(v["fail"])

    def test_separation_is_load_bearing(self):
        """effect + majority hold; only DISJOINTNESS stands between this and a fail.

        Three pairs breach hard and the medians breach, but one candidate sample beats
        every baseline sample. Deleting the range rule turns this into a FAIL, which is
        how this test proves the rule is doing work rather than riding along.
        """
        v = tput([80, 80, 80, 130], [100, 100, 100, 100])
        self.assertTrue(v["effect"])
        self.assertTrue(v["majority"])
        self.assertFalse(v["disjoint"])
        self.assertFalse(v["fail"])

    def test_reproducibility_is_load_bearing(self):
        """effect + disjointness hold; only the MAJORITY rule stands in the way.

        A bimodal candidate: two pairs breach, two are within a hair of the baseline.
        Deleting the majority rule turns this into a FAIL.
        """
        v = tput([10, 80, 95, 99], [100, 100, 100, 100])
        self.assertTrue(v["effect"])
        self.assertTrue(v["disjoint"])
        self.assertFalse(v["majority"])
        self.assertFalse(v["fail"])

    def test_effect_size_is_load_bearing(self):
        """disjointness + majority hold; only the MEDIAN threshold stands in the way.

        Every candidate sample is worse and two of three pairs breach, but the
        aggregate move is -11% — under the -12% gate. Deleting the median leg turns
        this into a FAIL on a difference the gate does not claim to resolve.
        """
        v = tput([64, 60, 64], [68, 72, 76])
        self.assertTrue(v["disjoint"])
        self.assertTrue(v["majority"])
        self.assertFalse(v["effect"])
        self.assertFalse(v["fail"])

    def test_identical_arms(self):
        self.assertFalse(tput([100, 100, 100, 100], [100, 100, 100, 100])["fail"])
        self.assertFalse(lat([100, 100, 100, 100], [100, 100, 100, 100])["fail"])


class RealRegressionsStillFail(unittest.TestCase):
    """@brief Detection is not what was traded away. Each of these must FAIL."""

    def test_385_fanout_latency_step(self):
        """The +33% fan-out step CI measured on three runners at 25.6 -> 34.2 us."""
        v = lat([34214, 34044, 34234, 34100], [25718, 25518, 25588, 25600])
        self.assertTrue(v["fail"])
        self.assertEqual(v["pairs_breached"], 4)

    def test_synthetic_fold_b4_ablation(self):
        """The -20% fold-b4 throughput ablation this fix was demonstrated against.

        This is the load-bearing one for #763's defect 1: `fold-b4` runs at ~3 ns, so
        LAT_TICK_NS leaves throughput as its ONLY live gate leg. The rule must fail it
        on throughput alone when the effect reproduces.
        """
        v = tput([205, 203, 207, 201], [258, 251, 256, 253])
        self.assertTrue(v["fail"])

    def test_regression_survives_one_noisy_pair(self):
        """A real step that one pair fails to reproduce still fails on the majority."""
        v = tput([63, 65, 62, 64], [100, 104, 98, 66])
        self.assertTrue(v["fail"])
        self.assertEqual(v["pairs_breached"], 3)


class ThresholdBoundary(unittest.TestCase):
    """@brief The thresholds themselves are unchanged — only the decision around them."""

    def test_just_under_the_threshold_passes(self):
        v = tput([89, 89, 89, 89], [100, 100, 100, 100])  # -11%, under the -12% gate
        self.assertFalse(v["effect"])
        self.assertFalse(v["fail"])

    def test_just_over_the_threshold_fails(self):
        v = tput([87, 87, 87, 87], [100, 100, 100, 100])  # -13%
        self.assertTrue(v["fail"])

    def test_sub_tick_latency_step_cannot_fail_on_grain_alone(self):
        """A one-grain p50 step on a single-digit-ns point is still tick-guarded."""
        v = lat([4, 4, 4, 4], [3, 3, 3, 3])
        self.assertFalse(v["fail"])

    def test_two_pairs_require_unanimity(self):
        """Below three pairs 'a majority' is meaningless, so every pair must breach."""
        self.assertFalse(tput([63, 101], [100, 100])["fail"])
        self.assertTrue(tput([63, 62], [100, 100])["fail"])


class MemoryRatchetIsNeverSilent(unittest.TestCase):
    """@brief #792: an absent `bench_forward_heap` must never read as a passing gate.

    `mem_probe` returns `{}` for a missing binary and `mem_gate` iterates the CANDIDATE
    dict, so before this fix a candidate that was never built produced no output and no
    fail — the gate vanished. These pin the three supply shapes.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        self._probes: dict[pathlib.Path, dict] = {}
        real_probe = pg.mem_probe
        # The ratchet's decision is about SUPPLY, not about running a compiled binary:
        # stub the probe so these tests need no build, and restore it after.
        # `{}` for an unknown path is the real `mem_probe`'s contract for a binary that
        # is absent — the exact input that used to make the gate disappear.
        pg.mem_probe = lambda p: self._probes.get(p, {})  # noqa: E731
        self.addCleanup(setattr, pg, "mem_probe", real_probe)

    def binary(self, name: str, points: dict) -> pathlib.Path:
        """@brief A present bench_forward_heap whose probe yields `points`."""
        p = self.dir / name
        p.write_text("#!/bin/sh\n")
        self._probes[p] = points
        return p

    def run_ratchet(self, cand, base) -> tuple[list[str], str]:
        """@brief (fails, stdout) for one paired-mode ratchet decision."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            fails = pg.mem_ratchet(cand, base)
        return fails, buf.getvalue()

    def test_symmetric_absence_prints_skip_and_passes(self):
        """Neither arm built: nothing to compare, but the gate must say so out loud."""
        missing = self.dir / "bench_forward_heap"
        for cand, base in ((missing, None), (missing, self.dir / "base_fwd"), (None, None)):
            with self.subTest(cand=cand, base=base):
                fails, out = self.run_ratchet(cand, base)
                self.assertEqual(fails, [])          # a genuine skip does not fail the gate
                self.assertIn("SKIP", out)           # ... and is never silent
                self.assertIn("memory ratchet", out)

    def test_asymmetric_absence_fails_and_names_the_missing_binary(self):
        """Baseline supplied, candidate missing — the shape #792 was reported for."""
        base = self.binary("base_fwd", {"mem:vertex": {"bytes": 100, "allocs": 3}})
        cand = self.dir / "bench_forward_heap"  # never built
        fails, out = self.run_ratchet(cand, base)
        self.assertEqual(len(fails), 1)
        self.assertIn("wiring error", fails[0])
        self.assertIn(str(cand), fails[0])       # the missing binary is named
        self.assertIn("MISSING", fails[0])
        self.assertNotIn("SKIP", out)            # a wiring error is not a skip

    def test_asymmetric_absence_mirror_case_also_fails(self):
        """Candidate built, baseline arm never supplied: same wiring error, same fail."""
        cand = self.binary("bench_forward_heap", {"mem:vertex": {"bytes": 100, "allocs": 3}})
        for base in (None, self.dir / "base_fwd"):
            with self.subTest(base=base):
                fails, out = self.run_ratchet(cand, base)
                self.assertEqual(len(fails), 1)
                self.assertIn("wiring error", fails[0])
                self.assertIn("baseline", fails[0])
                self.assertNotIn("SKIP", out)

    def test_both_present_runs_the_ratchet(self):
        """The existing behaviour is what the fix must not cost: probe both, compare."""
        base = self.binary("base_fwd", {"mem:vertex": {"bytes": 100, "allocs": 3}})
        cand = self.binary("bench_forward_heap", {"mem:vertex": {"bytes": 100, "allocs": 3}})
        fails, out = self.run_ratchet(cand, base)
        self.assertEqual(fails, [])
        self.assertIn("mem:vertex", out)         # the point is printed, not skipped
        self.assertIn("base 100B", out)

    def test_both_present_still_catches_a_regression(self):
        """Detection is not what was traded away: a real pullback fails through it."""
        base = self.binary("base_fwd", {"mem:vertex": {"bytes": 100, "allocs": 3}})
        cand = self.binary("bench_forward_heap", {"mem:vertex": {"bytes": 130, "allocs": 4}})
        fails, _ = self.run_ratchet(cand, base)
        self.assertEqual(len(fails), 2)          # bytes pullback + allocation pullback
        self.assertTrue(any("memory pullback" in f for f in fails))
        self.assertTrue(any("allocation pullback" in f for f in fails))


class LegacyMemoryRatchetIsNeverSilent(unittest.TestCase):
    """@brief The legacy (recorded-baseline) arm carried the identical hole: no `mem:`
    keys in the run simply meant `mem_gate` printed nothing."""

    def ratchet(self, cur, base, fwd=pathlib.Path("/nonexistent/bench_forward_heap")):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            fails = pg.mem_ratchet_legacy(cur, base, fwd)
        return fails, buf.getvalue()

    def test_no_points_anywhere_prints_skip_and_passes(self):
        fails, out = self.ratchet({"inproc": {}}, None)
        self.assertEqual(fails, [])
        self.assertIn("SKIP", out)

    def test_baseline_has_points_but_run_produced_none_fails(self):
        """Asymmetric: the recorded baseline gates memory, this run silently would not."""
        fails, out = self.ratchet({"inproc": {}}, {"mem:vertex": {"bytes": 100, "allocs": 3}})
        self.assertEqual(len(fails), 1)
        self.assertIn("wiring error", fails[0])
        self.assertIn("bench_forward_heap", fails[0])
        self.assertNotIn("SKIP", out)

    def test_points_present_ratchets_as_before(self):
        cur = {"mem:vertex": {"bytes": 130, "allocs": 3}}
        base = {"mem:vertex": {"bytes": 100, "allocs": 3}}
        fails, out = self.ratchet(cur, base)
        self.assertEqual(len(fails), 1)
        self.assertIn("memory pullback", fails[0])
        self.assertIn("mem:vertex", out)


class MemPointsAreDocumented(unittest.TestCase):
    """@brief docs/methodology.md states the memory-probe COUNT in prose and is not
    generated, so it can only rot silently (#792 found it stale at three). This pins the
    doc to the list — an editor who changes MEM_POINTS fails here until the doc follows."""

    DOC = pathlib.Path(__file__).resolve().parents[1] / "docs" / "methodology.md"

    def test_methodology_names_every_mem_point(self):
        if not self.DOC.exists():          # bench/ checked out alone
            self.skipTest(f"{self.DOC} not present")
        text = self.DOC.read_text()
        words = {2: "two", 3: "three", 4: "four", 5: "five", 6: "six", 7: "seven"}
        n = len(pg.MEM_POINTS)
        # `assertTrue` rather than `assertIn`: the doc is 300 lines and a failing
        # `assertIn` dumps the whole of it over the real message.
        self.assertTrue(f"**{words[n]} memory probes**" in text,
                        f"{self.DOC} does not say '{words[n]} memory probes' — "
                        f"MEM_POINTS now has {n} entries and the doc has rotted (#792)")
        for point in pg.MEM_POINTS:
            self.assertTrue(f"`{point}`" in text,
                            f"{point} is gated but not named in {self.DOC}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
