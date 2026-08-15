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

The TIER cases cover a third: a rule that existed only as prose. The two-tier verdict
policy lived in a `perf.yml` comment while `perf_gate.py` had no tier concept at all,
so the policy could be neither obeyed nor violated (#1251). These pin the disposition
of a fail under each tier, the default when no tier is declared, and the rule that a
sample `host_guard.py` FLAGGED cannot fail a PR through the blocking tier.

    python3 bench/test_perf_gate.py            # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import contextlib
import io
import pathlib
import re
import sys
import tempfile
import unittest
import unittest.mock

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


class MemChargedSteps(unittest.TestCase):
    """@brief An RFC-priced per-vertex step passes; one byte more does not.

    The charge is the one thing that lets a strict ratchet say yes to a cost a ratified
    clause already bought. It has to be exactly as narrow as it claims, so these pin both
    edges — a charge that absorbed more than its bytes would be a tolerance wearing a
    citation, and the whole point of declaring it is that it cannot become one."""

    def gate(self, cur_bytes, base_bytes, charged):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), \
                unittest.mock.patch.object(pg, "MEM_CHARGED", charged):
            fails = pg.mem_gate({"mem:vertex": {"bytes": cur_bytes, "allocs": 3}},
                                {"mem:vertex": {"bytes": base_bytes, "allocs": 3}})
        return fails, buf.getvalue()

    CHARGE = {"mem:vertex": (8, "RFC-0024 §6.4 vertex-index slot")}

    def test_exactly_the_charged_step_passes(self):
        fails, out = self.gate(144, 136, self.CHARGE)
        self.assertEqual(fails, [])
        self.assertIn("charged 8/8B", out)
        self.assertIn("RFC-0024", out)
        self.assertIn("144", out)  # the PRINTED figure is the real one, not the excused one

    def test_one_byte_over_the_charge_still_fails(self):
        fails, _ = self.gate(153, 136, self.CHARGE)
        self.assertEqual(len(fails), 1)
        self.assertIn("memory pullback", fails[0])
        self.assertIn("the other 9B is not", fails[0])

    def test_without_the_charge_the_same_step_fails(self):
        """The ablation: the charge, not the threshold, is what admits 136 -> 144."""
        fails, _ = self.gate(144, 136, {})
        self.assertEqual(len(fails), 1)
        self.assertIn("memory pullback", fails[0])

    def test_a_landed_charge_says_so(self):
        """Once the step is on main the delta is zero and the entry is dead weight."""
        fails, out = self.gate(144, 144, self.CHARGE)
        self.assertEqual(fails, [])
        self.assertIn("UNSPENT", out)
        self.assertIn("delete the entry", out)

    def test_every_charge_names_a_point_the_gate_probes(self):
        for key in pg.MEM_CHARGED:
            self.assertIn(key.removeprefix("mem:"), pg.MEM_POINTS,
                          f"{key} is charged but never probed — a charge against nothing")

    def test_every_charge_cites_a_clause(self):
        for key, (nbytes, why) in pg.MEM_CHARGED.items():
            self.assertGreater(nbytes, 0, f"{key}: a zero-byte charge is not a charge")
            self.assertIn("RFC-", why, f"{key}: a charge must name the clause that prices it")


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


class PointsAreDocumented(unittest.TestCase):
    """@brief The same pin for the gated latency/throughput points (#1041).

    `MEM_POINTS` had this guard and `POINTS` did not, though both lists are edited by the
    same kind of change. docs/methodology.md states the point COUNT in prose and is
    spliced into the published performance page by `gen_results_page.py`, so a stale count
    is the PUBLIC description of what the per-PR gate covers — and the doc is not
    generated, so it can only rot silently, which is how the memory-probe count reached
    #792 stale at three.

    The count is published from TWO hand-written places, so both are pinned: the prose in
    docs/methodology.md, and the `gate` column of `gen_results_page.py`'s instrument
    registry, which renders the same claim into the instrument table at the top of the
    same page. Pinning one of the two would leave the page able to contradict itself."""

    DOC = pathlib.Path(__file__).resolve().parents[1] / "docs" / "methodology.md"
    GEN = pathlib.Path(__file__).resolve().parent / "gen_results_page.py"
    WORDS = {2: "two", 3: "three", 4: "four", 5: "five", 6: "six", 7: "seven",
             8: "eight", 9: "nine", 10: "ten", 11: "eleven", 12: "twelve"}

    def test_methodology_names_every_point(self):
        if not self.DOC.exists():          # bench/ checked out alone
            self.skipTest(f"{self.DOC} not present")
        text = self.DOC.read_text()
        words = self.WORDS
        n = len(pg.POINTS)
        # `assertTrue` rather than `assertIn`: the doc is 300 lines and a failing
        # `assertIn` dumps the whole of it over the real message.
        self.assertTrue(f"**{words[n]} canonical points**" in text,
                        f"{self.DOC} does not say '{words[n]} canonical points' — "
                        f"POINTS now has {n} entries and the doc has rotted (#1041)")
        # The key form is the one the gate itself prints (`paired_samples`), so the doc
        # names each point exactly as a failure line will name it.
        for (_binary, mode, size, fan, ep) in pg.POINTS:
            key = f"{mode}/{size}/{fan}/{ep}"
            self.assertTrue(f"`{key}`" in text,
                            f"{key} is gated but not named in {self.DOC}")

    def test_instrument_registry_states_the_count(self):
        """The published instrument table repeats the count in its own words."""
        if not self.GEN.exists():          # perf_gate.py vendored alone
            self.skipTest(f"{self.GEN} not present")
        n = len(pg.POINTS)
        self.assertTrue(f"{self.WORDS[n]} canonical points" in self.GEN.read_text(),
                        f"{self.GEN}'s instrument registry does not say "
                        f"'{self.WORDS[n]} canonical points' — POINTS now has {n} entries "
                        f"and the published instrument table has rotted (#1041)")


class VerdictTier(unittest.TestCase):
    """@brief #1251: the two-tier policy, as a rule the gate can actually apply.

    Before this the policy was a `perf.yml` comment and `perf_gate.py` had no tier
    concept, so "the pinned host blocks, the runners warn" described a mechanism that
    did not exist. What the tier decides is the DISPOSITION of a fail — never how the
    comparison is made — so every case below feeds the two tiers the SAME fail list.
    """

    FAILS = ["inproc/64/1/1 p50 pullback: 130ns vs base 100ns (+30%), reproduced in "
             "4/4 interleaved pairs with disjoint ranges"]
    # Exactly the shape `host_guard.py stamp` writes into a point's `extra`.
    FLAGGED = ("box · pinned cpu2 · gcc 15.1.0 · "
               "CONTAMINATED (A/A bracket 9.4% > 6.0% band)")

    def verdict(self, fails, tier, note=None, warns=()):
        """@brief (exit_code, stdout) for one rendered verdict."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = pg.render_verdict(list(fails), list(warns), tier, note)
        return rc, buf.getvalue()

    def test_blocking_fails_on_a_breached_ratchet(self):
        rc, out = self.verdict(self.FAILS, "blocking")
        self.assertEqual(rc, 1)
        self.assertIn("PERF: FAIL", out)
        self.assertIn("tier=blocking", out)
        self.assertNotIn("ADVISORY", out)

    def test_advisory_does_not_fail_on_the_same_input(self):
        """The load-bearing pair: identical input, identical numbers, exit 0."""
        rc, out = self.verdict(self.FAILS, "advisory")
        self.assertEqual(rc, 0)
        self.assertIn("PERF: FAIL", out)          # the comparison is still reported ...
        self.assertIn("ADVISORY", out)            # ... and says why it is not enforced
        self.assertIn("::warning::", out)         # ... loudly enough to be seen

    def test_the_tier_never_hides_the_numbers(self):
        """An unenforced breach prints exactly what an enforced one prints."""
        _, blocking = self.verdict(self.FAILS, "blocking")
        _, advisory = self.verdict(self.FAILS, "advisory")
        for line in self.FAILS:
            self.assertIn("  ! " + line, blocking)
            self.assertIn("  ! " + line, advisory)

    def test_a_flagged_sample_cannot_fail_the_blocking_tier(self):
        """host_guard.py FLAGS a suspect sample, never deletes it — and the whole value
        of flagging is that a gating consumer stops believing it. A contaminated sample
        that could still red a PR would bill the code for the machine."""
        rc, out = self.verdict(self.FAILS, "blocking", self.FLAGGED)
        self.assertEqual(rc, 0)
        self.assertIn("FLAGGED contaminated", out)
        self.assertIn("::warning::", out)         # downgraded, never silent
        self.assertIn("  ! " + self.FAILS[0], out)

    def test_a_clean_sample_note_still_enforces(self):
        """The ablation for the case above: it is the FLAG that disarms the tier, not
        the mere presence of a note. A host descriptor with no verdict on it enforces."""
        rc, _ = self.verdict(self.FAILS, "blocking", "box · pinned cpu2 · gcc 15.1.0")
        self.assertEqual(rc, 1)

    def test_contamination_is_decided_by_host_guards_own_predicate(self):
        """One rule, one place. `enforces` imports `is_contaminated` rather than
        re-deriving the token, so the writer and the reader cannot drift apart."""
        import host_guard
        self.assertIs(pg.is_contaminated, host_guard.is_contaminated)
        self.assertTrue(host_guard.is_contaminated(self.FLAGGED))

    def test_a_passing_run_is_a_pass_in_either_tier(self):
        for tier in pg.TIERS:
            with self.subTest(tier=tier):
                rc, out = self.verdict([], tier)
                self.assertEqual(rc, 0)
                self.assertIn("PERF: PASS", out)
                self.assertIn(f"tier={tier}", out)

    def test_the_soft_warn_list_is_a_different_mechanism_and_survives(self):
        """`warns` (#792/#464) is a comparison the gate declines to call a failure in
        ANY tier — measured, reported, never fatal. It is not the tier and the tier
        must not have absorbed it: it keeps its own `~` marker and never reads as a
        fail, including in the blocking tier."""
        warn = ("fold-b4/512/1/1 throughput pullback — NOT FAILED: the same point's "
                "latency legs contradict it")
        for tier in pg.TIERS:
            with self.subTest(tier=tier):
                rc, out = self.verdict([], tier, warns=[warn])
                self.assertEqual(rc, 0)
                self.assertIn("PERF: PASS", out)
                self.assertIn("  ~ " + warn, out)
                self.assertNotIn("  ! ", out)

    def test_default_tier_is_advisory(self):
        """A forgotten flag must under-enforce LOUDLY rather than red a machine the bar
        was never calibrated against. The workflow lint below is what keeps that safe."""
        self.assertEqual(pg.DEFAULT_TIER, "advisory")
        self.assertEqual(pg._tier([]), "advisory")
        self.assertEqual(pg._tier(["--pairs", "4"]), "advisory")

    def test_a_declared_tier_wins_over_the_default(self):
        for tier in pg.TIERS:
            with self.subTest(tier=tier):
                self.assertEqual(pg._tier(["--tier", tier, "--pairs", "4"]), tier)

    def test_an_unknown_tier_refuses_to_run(self):
        """Neither fall-back is honest: to blocking would red a job on a typo, to
        advisory would turn a gate into an instrument that never enforces."""
        with self.assertRaises(SystemExit) as cm, \
                contextlib.redirect_stderr(io.StringIO()):
            pg._tier(["--tier", "warn"])
        self.assertEqual(cm.exception.code, 2)


class WorkflowsDeclareTheirTier(unittest.TestCase):
    """@brief Every CI invocation of the gate must name its tier on the command line.

    This is what makes an advisory DEFAULT safe. The default exists for a maintainer's
    laptop; if a workflow ever inherits it, the repo would silently hold a gate that
    measures and never enforces — the exact failure mode #1251 was filed about, one
    layer down. So the YAML has to say the word.
    """

    WORKFLOWS = pathlib.Path(__file__).resolve().parents[1] / ".github" / "workflows"
    # `python3 …/perf_gate.py`, and never `test_perf_gate.py` — the unit-test step is
    # not a gate invocation and has no tier to declare.
    INVOKE = re.compile(r"python3\s+\S*(?<!test_)perf_gate\.py")

    def invocations(self) -> list[tuple[str, str]]:
        """@brief (workflow name, whole shell command) for each gate invocation.

        Continuation lines are joined, because the tier flag may sit on any of them.
        """
        found = []
        for wf in sorted(self.WORKFLOWS.glob("*.yml")):
            lines = wf.read_text().splitlines()
            i = 0
            while i < len(lines):
                text = lines[i].strip()
                if not text.startswith("#") and self.INVOKE.search(text):
                    j = i
                    while text.endswith("\\") and j + 1 < len(lines):
                        j += 1
                        text = text[:-1] + " " + lines[j].strip()
                    found.append((wf.name, text))
                    i = j
                i += 1
        return found

    def test_there_is_at_least_one_invocation_to_check(self):
        """Without this the lint below passes loudest when it has nothing to read —
        a renamed workflow directory would make it vacuous rather than red."""
        if not self.WORKFLOWS.is_dir():        # bench/ checked out alone
            self.skipTest(f"{self.WORKFLOWS} not present")
        self.assertTrue(self.invocations(), f"no perf_gate.py invocation found under "
                                            f"{self.WORKFLOWS} — has the lint gone blind?")

    def test_every_invocation_declares_a_valid_tier(self):
        if not self.WORKFLOWS.is_dir():
            self.skipTest(f"{self.WORKFLOWS} not present")
        for name, cmd in self.invocations():
            with self.subTest(workflow=name):
                self.assertIn("--tier", cmd,
                              f"{name} invokes perf_gate.py without --tier, so its "
                              f"verdict policy is whatever the default happens to be: "
                              f"{cmd}")
                tier = cmd.split("--tier", 1)[1].split()[0]
                self.assertIn(tier, pg.TIERS, f"{name} declares --tier {tier}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
