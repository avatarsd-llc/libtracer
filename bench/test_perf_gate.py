#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Decision-rule tests for the interleaved perf gate (`perf_gate.paired_verdict`).

These are the gate's OWN unit tests, and they exist because #763 was a defect in the
gate rather than in anything it measured: the shapes below are the recorded sample sets
from the three false failures (#708, #758 twice) and from the real regressions the gate
must keep catching (#385's +33% fan-out step, and the measured -20% synthetic fold-b4
ablation used to demonstrate this fix). A change to the rules that loses either
direction fails here, in a second, instead of on a release train.

    python3 bench/test_perf_gate.py            # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import pathlib
import sys
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
