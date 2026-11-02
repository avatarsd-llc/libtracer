#!/usr/bin/env python3
"""@brief The ratchet's baseline rule (#2466): last benched ancestor, never a superseded one.

A batch of merges benches once, so `HEAD^` may be a commit nobody measured. These pin the
rule that keeps a regression in a non-final batch member inside the comparison, and the
wiring that makes perf.yml use it.
"""
from __future__ import annotations

import pathlib
import re
import unittest

import ratchet_baseline as rb

WF = pathlib.Path(__file__).resolve().parents[1] / ".github" / "workflows"


class Pick(unittest.TestCase):
    """@brief Which successful run's commit becomes the baseline."""

    # main: B (benched) <- m1 <- m2 (planted regression, run superseded) <- m3 = HEAD
    ANCESTORS = {"B", "m1", "m2"}

    def anc(self, sha):
        return sha in self.ANCESTORS

    def test_a_superseded_batch_compares_against_the_batch_base(self):
        # m1 and m2 have no successful run (superseded while pending): HEAD^ is m2, but
        # the baseline is B, so m2's regression is inside HEAD's comparison.
        self.assertEqual(rb.pick("m3", ["B", "A"], self.anc), "B")

    def test_the_newest_benched_ancestor_wins(self):
        self.assertEqual(rb.pick("m3", ["m1", "B"], self.anc), "m1")

    def test_head_itself_is_never_its_own_baseline(self):
        # The nightly re-benches a commit whose push run already succeeded.
        self.assertEqual(rb.pick("m3", ["m3", "m1", "B"], self.anc), "m1")

    def test_a_rerun_of_a_later_commit_is_not_an_ancestor(self):
        # Re-running an older commit's run after newer merges: the newest success is
        # a descendant, not an ancestor, and must be skipped.
        self.assertEqual(rb.pick("m1", ["m3", "B"], lambda s: s == "B"), "B")

    def test_nothing_benched_means_fall_back(self):
        self.assertEqual(rb.pick("m3", [], self.anc), "")
        self.assertEqual(rb.pick("m3", ["X", "Y"], self.anc), "")

    def test_the_candidate_walk_is_bounded(self):
        calls = []

        def never(sha):
            calls.append(sha)
            return False

        rb.pick("h", [f"c{i}" for i in range(200)], never)
        self.assertEqual(len(calls), rb.MAX_CANDIDATES)


class Wiring(unittest.TestCase):
    """@brief perf.yml benches once per batch and compares against the resolver's commit."""

    def setUp(self):
        if not (WF / "perf.yml").is_file():
            self.skipTest("workflows not present")  # bench/ checked out alone
        self.perf = (WF / "perf.yml").read_text()
        self.local = (WF / "perf-local.yml").read_text()

    def test_main_pushes_share_one_group_per_workflow_and_never_cancel(self):
        # One group per branch (not per sha): GitHub keeps the newest pending run only.
        for text in (self.perf, self.local):
            block = re.search(r"\nconcurrency:\n(.*?)\n\n", text, re.S).group(1)
            self.assertNotIn("github.sha", block)
            self.assertIn("github.ref_name", block)
        self.assertIn("cancel-in-progress: false", self.local)
        self.assertIn("cancel-in-progress: ${{ github.event_name == 'pull_request' }}",
                      self.perf)

    def test_the_ratchet_baseline_is_the_resolver_and_not_head_parent(self):
        self.assertIn("bench/ratchet_baseline.py", self.perf)
        self.assertNotIn("worktree add --detach ../ratchet-baseline HEAD^", self.perf)

    def test_no_full_history_checkout_on_the_bench_host(self):
        # perf-local's density job keeps its history: it counts commits, on a hosted runner.
        self.assertNotIn("fetch-depth: 0", self.perf)

    def test_zenoh_is_nightly_only(self):
        for text in (self.perf, self.local):
            step = text[text.index("name: Run the Zenoh comparison bench"):]
            step = step[:step.index("\n      - ")]
            self.assertIn("github.event_name == 'schedule'", step)


if __name__ == "__main__":
    unittest.main(verbosity=2)
