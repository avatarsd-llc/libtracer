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
import json
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
    """@brief The recorded false failures must all come back PASS."""

    def test_708_shared_depression_window(self):
        """The #708 shape: a machine depression on pair 3 that both arms fell into."""
        v = tput([252, 251, 161, 255], [258, 251, 236, 250])
        self.assertFalse(v["effect"])  # the median pair is flat
        self.assertFalse(v["fail"])

    def test_758_fold_b4_overlapping_distributions(self):
        """The #758 shape: medians x1.01, distributions fully overlapping."""
        v = tput([252, 251, 255, 256], [255, 250, 253, 251])
        self.assertFalse(v["fail"])

    def test_single_low_sample_never_fails_alone(self):
        """One arm draws one very bad sample; every other pair is flat."""
        v = tput([250, 251, 90, 252], [250, 250, 250, 250])
        self.assertFalse(v["fail"])
        self.assertEqual(v["pairs_breached"], 1)

    def test_a_contrary_pair_keeps_the_interval_open(self):
        """Medians breach, but one pair says the opposite: the interval still reaches 1.

        This is the case the disjoint-range rule used to decide. Deleting the interval
        condition turns it into a FAIL, which is how this test proves it does work."""
        v = tput([88, 200, 80, 86], [100, 104, 98, 102])
        self.assertTrue(v["effect"])
        self.assertFalse(v["significant"])
        self.assertFalse(v["fail"])

    def test_one_wild_pair_of_four_cannot_close_the_interval(self):
        """Three pairs breach hard; the fourth points the other way."""
        v = tput([80, 80, 80, 130], [100, 100, 100, 100])
        self.assertTrue(v["effect"])
        self.assertFalse(v["significant"])
        self.assertFalse(v["fail"])

    def test_effect_size_is_load_bearing(self):
        """Every pair is slower (significant), but only by 10% — under the -12% flat gate."""
        v = tput([90, 89, 90, 91], [100, 100, 100, 100])
        self.assertTrue(v["significant"])
        self.assertFalse(v["effect"])
        self.assertFalse(v["fail"])

    def test_identical_arms(self):
        self.assertFalse(tput([100, 100, 100, 100], [100, 100, 100, 100])["fail"])
        self.assertFalse(lat([100, 100, 100, 100], [100, 100, 100, 100])["fail"])

    def test_a_layout_sensitive_row_is_held_at_the_flat_threshold(self):
        """#1855's row: `fold-b4` throughput moves ~15-25% between builds of one source, so
        its null is wider than flat. The ruling on #1874 caps it: the null may tighten a
        row, never loosen it, so the row gates exactly as it did before the null."""
        null = {"fold-b4/512/1/1": {"deliv_s": 0.26, "p50_ns": 0.14}}
        self.assertEqual(pg.leg_factor("fold-b4/512/1/1", "deliv_s", null),
                         (pg.TPUT_REGRESS, False, "cap"))
        self.assertEqual(pg.leg_factor("fold-b4/512/1/1", "p50_ns", null),
                         (pg.LAT_REGRESS, True, "cap"))
        # At the cap the leg keeps main's whole rule: the #1855 samples (x0.85 with one
        # overlapping pair) fail on the flat threshold only if ranges are disjoint AND a
        # majority breach — and the CI rule alone is not what decides.
        c, b = [212, 214, 213, 230], [251, 250, 252, 249]
        v, _f, src = pg.leg_verdict("fold-b4/512/1/1", "deliv_s", c, b, null)
        self.assertEqual((src, v["rule"]), ("cap", "flat"))
        self.assertEqual(v["fail"], pg.legacy_verdict(c, b, pg.TPUT_REGRESS, True)["fail"])
        v, _f, _s = pg.leg_verdict("fold-b4/512/1/1", "deliv_s", [80, 80, 80, 130],
                                   [100, 100, 100, 100], null)
        self.assertFalse(v["disjoint"])
        self.assertFalse(v["fail"], "main's separation rule still holds on a capped leg")
        v, _f, _s = pg.leg_verdict("fold-b4/512/1/1", "deliv_s", [10, 80, 95, 99],
                                   [100, 100, 100, 100], null)
        self.assertFalse(v["majority"])
        self.assertFalse(v["fail"], "main's majority rule still holds on a capped leg")
        # A leg with no null at all is decided the same way.
        self.assertEqual(pg.leg_verdict("new/64/1/1", "p50_ns", [1.0], [1.0], {})[0]["rule"],
                         "flat")
        # Just under the cap is still the null's own (tighter) threshold, under the CI rule.
        f, _t, src = pg.leg_factor("k", "mean_ns", {"k": {"mean_ns": 0.039}})
        self.assertEqual(src, "null")
        self.assertLess(f, pg.MEAN_REGRESS)
        v, _f, _s = pg.leg_verdict("k", "mean_ns", [1.2] * 4, [1.0] * 4,
                                   {"k": {"mean_ns": 0.039}})
        self.assertEqual(v["rule"], "ci")

    def test_1871_two_percent_disjoint_passes_at_the_floor(self):
        """#1871's A/A: two copies of one binary, narrow-full x1.02 with DISJOINT ranges."""
        factor, _t, _s = pg.leg_factor("store-lat-narrow-full/32/1/1", "p50_ns",
                                       {"store-lat-narrow-full/32/1/1": {"p50_ns": 0.004}})
        self.assertAlmostEqual(factor, 1 + pg.NULL_FLOOR)
        v = pg.paired_verdict([750, 752, 758, 765, 751, 754, 760, 756],
                              [741, 742, 741, 742, 741, 742, 741, 742], factor, False)
        self.assertTrue(v["significant"])
        self.assertFalse(v["fail"])


class RealRegressionsStillFail(unittest.TestCase):
    """@brief Detection is not what was traded away. Each of these must FAIL."""

    def test_385_fanout_latency_step(self):
        """The +33% fan-out step CI measured on three runners at 25.6 -> 34.2 us."""
        v = lat([34214, 34044, 34234, 34100], [25718, 25518, 25588, 25600])
        self.assertTrue(v["fail"])
        self.assertEqual(v["pairs_breached"], 4)

    def test_synthetic_fold_b4_ablation(self):
        """The -20% fold-b4 throughput ablation #763's fix was demonstrated against."""
        v = tput([205, 203, 207, 201], [258, 251, 256, 253])
        self.assertTrue(v["fail"])

    def test_regression_survives_one_noisy_pair(self):
        """A real step that one pair fails to reproduce still fails."""
        v = tput([63, 65, 62, 64], [100, 104, 98, 66])
        self.assertTrue(v["fail"])
        self.assertEqual(v["pairs_breached"], 3)

    def test_an_injected_ten_percent_fails_a_row_at_the_floor(self):
        """#1807's acceptance shape: +10% on a row whose null is tight (3% threshold), in 8
        noisy ABBA pairs. The flat +15% gate could not fail this by construction."""
        null = {"inproc/64/1/1": {"p50_ns": 0.004, "deliv_s": 0.004}}
        base = [100.0, 101.0, 99.5, 100.4, 100.9, 99.8, 100.2, 100.6]
        slow = [b * 1.10 for b in reversed(base)]
        f, t, _ = pg.leg_factor("inproc/64/1/1", "p50_ns", null)
        self.assertTrue(pg.paired_verdict(slow, base, f, False, t)["fail"])
        f, t, _ = pg.leg_factor("inproc/64/1/1", "deliv_s", null)
        self.assertTrue(pg.paired_verdict([1e9 / x for x in slow], [1e9 / x for x in base],
                                          f, True, t)["fail"])
        f, t, _ = pg.leg_factor("inproc/64/1/1", "p50_ns", {})  # no null: the flat +15%
        self.assertFalse(pg.paired_verdict(slow, base, f, False, t)["fail"])


class ThresholdBoundary(unittest.TestCase):
    """@brief Where the threshold comes from, and the boundary it draws."""

    def test_just_under_the_threshold_passes(self):
        v = tput([89, 89, 89, 89], [100, 100, 100, 100])  # -11%, under the -12% gate
        self.assertFalse(v["effect"])
        self.assertFalse(v["fail"])

    def test_just_over_the_threshold_fails(self):
        v = tput([87, 87, 87, 87], [100, 100, 100, 100])  # -13%
        self.assertTrue(v["fail"])

    def test_sub_tick_latency_step_cannot_fail_on_grain_alone(self):
        """A one-grain p50 step on a single-digit-ns point is still tick-guarded (flat)."""
        v = lat([4, 4, 4, 4], [3, 3, 3, 3])
        self.assertFalse(v["fail"])

    def test_two_pairs_require_unanimity(self):
        """Two pairs that disagree leave the interval reaching 1; two that agree close it."""
        self.assertFalse(tput([63, 101], [100, 100])["fail"])
        self.assertTrue(tput([63, 62], [100, 100])["fail"])

    def test_null_threshold_is_three_spreads_with_a_floor(self):
        null = {"k": {"p50_ns": 0.03, "mean_ns": 0.001, "deliv_s": 0.03}}
        self.assertAlmostEqual(pg.leg_factor("k", "p50_ns", null)[0], 1.09)
        self.assertAlmostEqual(pg.leg_factor("k", "mean_ns", null)[0], 1.03)  # the floor
        self.assertAlmostEqual(pg.leg_factor("k", "deliv_s", null)[0], 1 / 1.09)
        self.assertFalse(pg.leg_factor("k", "p50_ns", null)[1], "the null measured the grain")

    def test_a_row_the_null_lacks_falls_back_to_flat_and_says_so(self):
        self.assertEqual(pg.leg_factor("new/64/1/1", "p50_ns", {}),
                         (pg.LAT_REGRESS, True, "flat"))
        self.assertEqual(pg.leg_factor("new/64/1/1", "deliv_s", {}),
                         (pg.TPUT_REGRESS, False, "flat"))

    def test_the_bootstrap_is_deterministic(self):
        r = [1.01, 0.98, 1.05, 1.02, 0.99, 1.03, 1.00, 1.04]
        self.assertEqual(pg.bootstrap_ci(r), pg.bootstrap_ci(list(r)))
        lo, hi = pg.bootstrap_ci(r)
        self.assertLessEqual(lo, 1.015)
        self.assertGreaterEqual(hi, 1.015)

    def test_the_banked_null_file_parses(self):
        """The committed null is what the gate reads; a malformed one must not ship."""
        if not pg.NULL_FILE.exists():
            self.skipTest("no banked null in this tree")
        doc = json.loads(pg.NULL_FILE.read_text())
        self.assertIn("meta", doc)
        for k, legs in doc["rows"].items():
            self.assertRegex(k, r"^[\w-]+/\d+/\d+/\d+$")
            for leg, s in legs.items():
                self.assertIn(leg, pg.LEGS)
                self.assertGreaterEqual(s, 0.0)


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
             8: "eight", 9: "nine", 10: "ten", 11: "eleven", 12: "twelve",
             13: "thirteen", 14: "fourteen", 15: "fifteen", 16: "sixteen",
             17: "seventeen", 18: "eighteen", 19: "nineteen", 20: "twenty",
             **{20 + i: f"twenty-{w}" for i, w in enumerate(
                 ("one", "two", "three", "four", "five", "six", "seven", "eight", "nine"),
                 start=1)},
             30: "thirty",
             **{30 + i: f"thirty-{w}" for i, w in enumerate(
                 ("one", "two", "three", "four", "five", "six", "seven", "eight", "nine"),
                 start=1)}}

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


class HeapLkvGatedAt1KiB(unittest.TestCase):
    """@brief The heap LKV rows are gated at 1024 B too, and the #1768 step fails them.

    v0.17.0 shipped a 2x slowdown on `lkv-store-heap` / `lkv-alloc-heap` at 1024 B with every
    gate green: the only heap point was 64 B, where the same change was ~30% FASTER. These pin
    that the 1024 B rows are POINTS, and that the recorded shapes reach the verdict they must."""

    def test_both_heap_rows_are_points_at_1024(self):
        keys = {(mode, size) for (_b, mode, size, _f, _e) in pg.POINTS}
        self.assertIn(("lkv-store-heap", 1024), keys)
        self.assertIn(("lkv-alloc-heap", 1024), keys)
        self.assertIn(("lkv-store-heap", 64), keys)  # the 64 B point stays

    def test_1768_alloc_step_fails_on_throughput(self):
        """`lkv-alloc-heap 1024B` 17.6 -> 47 ns, as ops/s (M). Throughput is its live leg."""
        self.assertTrue(tput([21.3, 21.1, 21.4, 21.2], [56.8, 56.5, 57.0, 56.6])["fail"])

    def test_1768_store_step_fails_on_latency_despite_the_tick_guard(self):
        """`lkv-store-heap 1024B` 27 -> 54 ns: sub-100 ns, but 27 ns is over LAT_TICK_NS."""
        self.assertTrue(lat([54, 54, 55, 54], [27, 27, 27, 28])["fail"])

    def test_the_64b_gain_direction_never_fails(self):
        """The other half of slice 10: 64 B got faster. A speed-up is never a pullback."""
        self.assertFalse(tput([34.0, 34.2, 33.9, 34.1], [26.0, 26.1, 25.9, 26.0])["fail"])
        self.assertFalse(lat([29, 29, 30, 29], [41, 41, 42, 41])["fail"])

    def test_a_one_tick_wobble_at_1024_passes(self):
        """A 1-2 tick move on a ~27 ns row is clock grain, not a regression."""
        self.assertFalse(lat([29, 28, 29, 28], [27, 27, 27, 27])["fail"])


class PicosecondBatchRows(unittest.TestCase):
    """@brief Batch rows reach the gate to the picosecond, and a 0 is "not measured" (#1804).

    A batch row prints p50 and mean with three decimals; reading them with int() would
    re-quantize the very figures the change exists to keep. A bulk-only row (`lkv-*`) now
    publishes 0 latency, which must skip that leg, never divide by it or fail on it."""

    TRANSCRIPT = ("CLOCK\t1.000\t21.874\n"
                  "ALLOC\tpinned\tglibc.malloc.arena_max=8\n"
                  "RESULT\tlibtracer\tfold-b4\t512\t1\t1\t250000000\t250000000\t0.0"
                  "\t3.912\t0\t3.950\n"
                  "RESULT\tlibtracer\tlkv-store-heap\t64\t1\t1\t40000000\t40000000"
                  "\t2560.0\t0\t0\t0\n")

    def rows(self):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "bench_libtracer"
            p.write_text("")
            with unittest.mock.patch.object(pg, "timed_run",
                                            lambda a, *_x, **_k: _measurement(a, self.TRANSCRIPT)), \
                    unittest.mock.patch.object(pg, "CLOCK_FLOORS", []), \
                    unittest.mock.patch.object(pg, "ALLOC_STATES", []):
                rows = pg.run_bench_once(p)
                floors = list(pg.CLOCK_FLOORS)
                self.allocs = list(pg.ALLOC_STATES)
        return rows, floors

    def test_fractional_ns_survive_the_parse(self):
        rows, _ = self.rows()
        v = pg.metric(rows, "fold-b4", 512, 1, 1)
        self.assertEqual((v["p50_ns"], v["mean_ns"]), (3.912, 3.950))

    def test_clock_line_is_recorded_not_a_row(self):
        rows, floors = self.rows()
        self.assertEqual(floors, [(1.0, 21.874)])
        self.assertEqual(len(rows), 2)

    def test_alloc_line_is_recorded_not_a_row(self):
        """#1903: the allocator state each process printed reaches the verdict's block."""
        rows, _ = self.rows()
        self.assertEqual(self.allocs, [("pinned", "glibc.malloc.arena_max=8")])
        self.assertEqual(len(rows), 2)
        self.assertIn("pinned glibc.malloc.arena_max=8 (2 process(es))",
                      pg.alloc_line(self.allocs * 2))

    def test_alloc_line_names_an_unpinned_process(self):
        line = pg.alloc_line([("pinned", "a"), ("unpinned", "-")])
        self.assertIn("WARNING 1 of 2 process(es) unpinned", line)
        self.assertIn("not recorded", pg.alloc_line([]))

    def test_a_zero_latency_leg_is_skipped_not_judged(self):
        key = "lkv-store-heap/64/1/1"
        sample = {"p50_ns": 0.0, "mean_ns": 0.0, "deliv_s": 4.0e7}
        slow = {"p50_ns": 0.0, "mean_ns": 0.0, "deliv_s": 2.0e7}  # -50% throughput
        fake = {"cand": {key: [dict(slow) for _ in range(4)]},
                "base": {key: [dict(sample) for _ in range(4)]}}
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            fails, _ = pg.gate_paired({}, {}, 4, {}, fake)
        fails = [x for x in fails if x.startswith(key)]  # the other points are missing (#1847)
        self.assertEqual(len(fails), 1)
        self.assertIn("deliv/s", fails[0])  # the one leg the row has still gates
        self.assertNotIn("p50 ", out.getvalue().split(key)[1].split("run drift")[0])
        self.assertNotIn("inf", out.getvalue())


def lkv_out(heap64, pool64, heap1k=None, pool1k=None):
    """@brief A doctored `bench_libtracer lkv` stdout: the two lkv-alloc rows per size."""
    def row(mode, size, ops):
        return "\t".join(["RESULT", "libtracer", mode, str(size), "1", "1", str(ops),
                           str(ops), "0", "0", "0", "0"])
    lines = [row("lkv-alloc-heap", 64, heap64), row("lkv-alloc-pool", 64, pool64)]
    if heap1k is not None:
        lines += [row("lkv-alloc-heap", 1024, heap1k), row("lkv-alloc-pool", 1024, pool1k)]
    return "\n".join(lines + ["noise line", "RESULT\ttoo\tshort"]) + "\n"


class LkvRatioReport(unittest.TestCase):
    """@brief The ADR-0060 pool/heap ratio (#1745, #1695) is reported and NEVER fails.

    The pool's acceptance is not a speed ratio: its only goal is never to take an
    allocation from the system heap, which `bench_forward_heap`'s LKV-ROUTE count gates
    structurally. So no ratio, not even a pool that costs what the heap costs, may reach
    the fail list. The doctored shapes are #1739's and #1695's (1.5-1.6x on runners where
    identical code read 2.1-3.2x) and a ~1.0x fallback."""

    SHAPES = [(102, 150), (98, 320), (155, 160), (160, 210), (140, 300), (300, 310)]

    def run_paired(self, cand_out, base_out, pairs=3):
        """@brief The paired report over doctored outputs, keyed by which binary ran."""
        def fake_timed(argv, timeout, **_k):
            return cand_out(argv) if argv[0] == "cand" else base_out(argv)
        with unittest.mock.patch.object(pg, "timed", fake_timed), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            ret = pg.lkv_ratio_report_paired(pathlib.Path("cand"), pathlib.Path("base"), pairs)
        return ret, out.getvalue()

    def test_ratio_never_fails_paired(self):
        """Every shape, a heap-cost pool included, returns no fail and says so."""
        for cand, base in self.SHAPES:
            ret, out = self.run_paired(lambda a: lkv_out(100, cand), lambda a: lkv_out(100, base))
            self.assertIsNone(ret)
            self.assertIn("reported, not gated", out)
            self.assertIn(f"main {base / 100:.1f}x, same session", out)

    def test_ratio_never_fails_legacy(self):
        for pool in (98, 102, 150, 300):
            with unittest.mock.patch.object(pg, "timed", lambda argv, timeout, **_k: lkv_out(100, pool)), \
                    contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertIsNone(pg.lkv_ratio_report(pathlib.Path("cand")))
            self.assertIn("reported, not gated", out.getvalue())

    def test_no_ratio_threshold_survives(self):
        """The thresholds were deleted, not parked: nothing for a later edit to re-arm."""
        for name in ("LKV_MIN_RATIO", "LKV_FALLBACK_RATIO", "lkv_verdict", "lkv_ratio_gate",
                     "lkv_ratio_gate_paired"):
            self.assertFalse(hasattr(pg, name), name)

    def test_both_sizes_reported(self):
        _, out = self.run_paired(lambda a: lkv_out(100, 300, 100, 470),
                                 lambda a: lkv_out(100, 310, 100, 480))
        self.assertIn("S=64", out)
        self.assertIn("S=1024", out)

    def test_best_of_pairs_not_one_round(self):
        """One contaminated pool round must not decide the report: the best observation wins."""
        rounds = iter([lkv_out(100, 140), lkv_out(100, 300), lkv_out(100, 290)])
        _, out = self.run_paired(lambda a: next(rounds), lambda a: lkv_out(100, 300))
        self.assertIn("3.0x", out)

    def test_interleaved_alternating_start(self):
        """Pairs alternate which arm runs first (A B / B A / A B)."""
        seen = []
        def rec(a):
            seen.append(a[0])
            return lkv_out(100, 300)
        self.run_paired(rec, rec, pairs=3)
        self.assertEqual(seen, ["base", "cand", "cand", "base", "base", "cand"])

    def test_main_without_rows_reports_the_candidate_alone(self):
        _, out = self.run_paired(lambda a: lkv_out(100, 105), lambda a: "no rows\n")
        self.assertIn("1.1x", out)
        self.assertNotIn("same session", out)


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


def _measurement(argv, stdout: str = "", clean: bool = True, rc: int = 0) -> "pg.bc.Measurement":
    """@brief A kept attempt with the given output and verdict, for a patched `timed_run`."""
    cond = pg.bc.Conditions(cpus=(3,), pinned=True, wall_s=1.0, foreign_pct=0.0 if clean else 9.0,
                            own_cpu_s=1.0, nivcsw=0, pressure=0.0,
                            verdict=pg.bc.CLEAN if clean else pg.bc.CONTENDED,
                            reason="" if clean else "foreign 9.0% > 2%")
    return pg.bc.Measurement(list(argv), stdout, "", rc, [cond])


class GateTimesFamilyByFamily(unittest.TestCase):
    """@brief The gate times each family on its own, ABBA, pinned by its set (#1807).

    A SINGLE family runs on ONE logical CPU, pressure scored; a MULTI family on every bench
    CPU, judged on foreign time only (#1803), after every SINGLE step. A contended pair is
    dropped for its family alone, and only a gated family that lost more than
    MAX_DROPPED_PAIRS makes the verdict INCONCLUSIVE.
    """

    MULTI_POINTS = {"inproc-mt4/64/1/4", "acl-inherit-d4-mt4/64/1/4", "poolalloc-mt4/64/1/1"}
    FAMILIES = {"inproc-fan": "single", "inproc-mt": "multi", "fold": "single"}

    def bins(self, root: pathlib.Path, tag: str) -> dict:
        out = {}
        for key, name in pg.BENCH_BY_KEY.items():
            p = root / tag / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("")
            out[key] = p
        return out

    def test_the_multi_threaded_rows_are_gated_points(self):
        keys = {f"{m}/{s}/{f}/{e}" for (_b, m, s, f, e) in pg.POINTS}
        self.assertLessEqual(self.MULTI_POINTS, keys)

    def plan(self, probe) -> list:
        with tempfile.TemporaryDirectory() as d, \
                unittest.mock.patch.object(pg, "CPUS", (3, 4, 5, 6)), \
                unittest.mock.patch.object(pg, "CPU_SINGLE", (3,)):
            return pg.gate_plan(self.bins(pathlib.Path(d), "c"), self.bins(pathlib.Path(d), "b"),
                                probe=probe)

    def test_single_families_then_siblings_then_multi(self):
        plan = self.plan(lambda p: self.FAMILIES)
        labels = [s.label for s in plan]
        siblings = [n for k, n in pg.BENCH_BY_KEY.items() if k != "main"]
        self.assertEqual(labels, ["inproc-fan", "fold", *siblings, "inproc-mt"])
        for s in plan:
            if s.label == "inproc-mt":
                self.assertEqual((s.cpus, s.score_pressure), ((3, 4, 5, 6), False))
            else:
                self.assertEqual((s.cpus, s.score_pressure), ((3,), True), s.label)
        self.assertEqual(plan[0].args, {0: ("--family", "inproc-fan"),
                                        1: ("--family", "inproc-fan")})

    def test_an_arm_without_families_makes_both_sweep_everything(self):
        plan = self.plan(lambda p: self.FAMILIES if "/c/" in str(p) else None)
        main = [s for s in plan if s.key == "main"]
        self.assertEqual(len(main), 1)
        self.assertEqual((main[0].args, main[0].cpus, main[0].score_pressure),
                         ({0: (), 1: ()}, (3, 4, 5, 6), True))

    def test_a_family_only_the_candidate_lists_runs_on_the_candidate_alone(self):
        plan = self.plan(lambda p: {**self.FAMILIES, "new": "single"} if "/c/" in str(p)
                         else self.FAMILIES)
        new = next(s for s in plan if s.label == "new")
        self.assertEqual(new.args, {0: ("--family", "new")})

    def run_paired(self, contended: set[tuple[str, int]] = frozenset(), pairs: int = 4,
                   families: dict | None = None):
        """@brief paired_samples over doctored runs; @p contended = {(family, pair)}."""
        seen: list[tuple[str, str]] = []
        count: dict[str, int] = {}

        def fake(argv, timeout, score_pressure=True, cpus=None):
            fam = argv[2] if len(argv) > 2 else pathlib.Path(argv[0]).name
            arm = "cand" if "/c/" in argv[0] else "base"
            seen.append((fam, arm))
            n = count[fam] = count.get(fam, 0) + 1
            row = _row("inproc", 64, 1, 1) if fam == "inproc-fan" else ""
            return _measurement(argv, row + "\n", ((fam, (n - 1) // 2) not in contended))
        with tempfile.TemporaryDirectory() as d, \
                unittest.mock.patch.object(pg, "list_families",
                                           lambda p: families or self.FAMILIES), \
                unittest.mock.patch.object(pg, "timed_run", fake), \
                contextlib.redirect_stdout(io.StringIO()):
            s = pg.paired_samples(self.bins(pathlib.Path(d), "c"),
                                  self.bins(pathlib.Path(d), "b"), pairs)
        return s, seen

    def test_each_family_runs_its_pairs_back_to_back_abba(self):
        _, seen = self.run_paired(pairs=4)
        fan = [arm for fam, arm in seen if fam == "inproc-fan"]
        self.assertEqual(fan, ["base", "cand", "cand", "base"] * 2)
        first = [fam for fam, _ in seen]
        self.assertEqual(first[:8], ["inproc-fan"] * 8, "one family's pairs are contiguous")
        last_single = max(i for i, (f, _) in enumerate(seen) if f != "inproc-mt")
        first_multi = min(i for i, (f, _) in enumerate(seen) if f == "inproc-mt")
        self.assertLess(last_single, first_multi)

    def test_a_contended_pair_is_dropped_for_its_family_only(self):
        s, _ = self.run_paired({("inproc-fan", 1)}, pairs=4)
        self.assertEqual(len(s["cand"]["inproc/64/1/1"]), 3)
        self.assertEqual(len(s["base"]["inproc/64/1/1"]), 3)
        self.assertEqual(s["dropped"], {"inproc-fan": 1})
        self.assertEqual(s["inconclusive"], [])

    def test_a_gated_family_that_lost_too_many_pairs_is_inconclusive(self):
        lost = {("inproc-fan", i) for i in range(pg.MAX_DROPPED_PAIRS + 1)}
        s, _ = self.run_paired(lost, pairs=8)
        self.assertEqual(len(s["inconclusive"]), 1)
        self.assertIn("inproc-fan", s["inconclusive"][0])

    def test_an_ungated_family_never_makes_the_run_inconclusive(self):
        s, _ = self.run_paired({("fold", i) for i in range(8)}, pairs=8)
        self.assertEqual(s["dropped"]["fold"], 8)
        self.assertEqual(s["inconclusive"], [])

    def test_the_verdict_names_the_inconclusive_family(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = pg.render_verdict([], [], "blocking", None, None, [],
                                   ["inproc-fan: only 5/8 pairs ran clean"])
        self.assertEqual(rc, pg.EXIT_INCONCLUSIVE)
        self.assertIn("? inproc-fan: only 5/8", out.getvalue())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(pg.render_verdict([], [], "advisory", None, None, [],
                                               ["inproc-fan: x"]), 0)

    def test_single_cpu_defaults_to_the_first_bench_cpu(self):
        with unittest.mock.patch.dict(pg.bc.os.environ, {"BENCH_CPU_SINGLE": ""}):
            self.assertEqual(pg.bc.single_cpu_from_env((3, 4, 5, 6)), (3,))
            self.assertIsNone(pg.bc.single_cpu_from_env(None))
        with unittest.mock.patch.dict(pg.bc.os.environ, {"BENCH_CPU_SINGLE": "5"}):
            self.assertEqual(pg.bc.single_cpu_from_env((3, 4, 5, 6)), (5,))

    def test_best_of_runs_every_single_step_before_any_multi_step(self):
        seen: list[str] = []

        def fake(argv, timeout, score_pressure=True, cpus=None):
            seen.append(argv[2] if len(argv) > 2 else pathlib.Path(argv[0]).name)
            return _measurement(argv)
        with tempfile.TemporaryDirectory() as d, \
                unittest.mock.patch.object(pg, "list_families", lambda p: self.FAMILIES), \
                unittest.mock.patch.object(pg, "timed_run", fake), \
                contextlib.redirect_stdout(io.StringIO()):
            pg.best_of(self.bins(pathlib.Path(d), "c"), 3)
        self.assertEqual(seen[:6], ["inproc-fan"] * 3 + ["fold"] * 3)
        self.assertEqual(seen[-3:], ["inproc-mt"] * 3)

    def verdict_for(self, cond: "pg.bc.Conditions") -> tuple[int, str]:
        led = pg.bc.Ledger()
        led.add(pg.bc.measure(["./bench_libtracer"], run=lambda *a, **k: ("", "", 0, cond)))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = pg.render_verdict([], [], "blocking", None, led)
        return rc, buf.getvalue()

    def run_cond(self, own_cpu_s: float, cg_psi: float, multi: bool) -> "pg.bc.Conditions":
        # cg_psi percent of the 10 s window as `some total=` stall microseconds.
        before = pg.bc.Sample(0, 0, 0, None, 0)
        after = pg.bc.Sample(10_000_000_000, 1000, 1000, None, round(cg_psi * 100_000))
        return pg.bc.classify(before, after, own_cpu_s=own_cpu_s, nivcsw=0, cpus=[3, 4, 5, 6],
                              pinned=True, clk_tck=100, score_pressure=not multi)

    def test_a_foreign_intruder_is_contended_on_either_set(self):
        # 12% of the window was someone else's CPU time; our own cgroup psi reads 0.
        for multi in (False, True):
            with self.subTest(multi=multi):
                rc, out = self.verdict_for(self.run_cond(8.8, 0.0, multi))
                self.assertEqual(rc, pg.EXIT_INCONCLUSIVE)
                self.assertIn("foreign", out)

    def test_own_pressure_is_scored_on_single_and_not_on_multi(self):
        rc, out = self.verdict_for(self.run_cond(10.0, 84.4, multi=False))
        self.assertEqual(rc, pg.EXIT_INCONCLUSIVE)
        self.assertIn("psi", out)
        rc, out = self.verdict_for(self.run_cond(10.0, 84.4, multi=True))
        self.assertEqual(rc, 0, out)
        self.assertIn("PERF: PASS", out)


def _row(mode: str, size: int, fan: int, ep: int) -> str:
    """@brief One 12-column RESULT line for @p mode at the given key."""
    return "\t".join(["RESULT", "libtracer", mode, str(size), str(fan), str(ep),
                      "1000000", "1000000", "0", "200", "300", "210"])


class MissingGatedKeysFail(unittest.TestCase):
    """@brief #1847: a gated key nobody emits must FAIL, never read as "not gated".

    The two demux points were keyed `/79/` after RFC-0018 shrank the frame to 61 B, so for
    seven weeks neither arm emitted them and every run printed "absent from one arm — not
    gated" over a PASS. These pin the re-key against the bench source and make an absent
    key fail in both the paired and the legacy path."""

    def sample(self):
        return {"p50_ns": 200.0, "mean_ns": 210.0, "deliv_s": 1.0e6}

    def gate(self, cand_keys, base_keys):
        fake = {"cand": {k: [self.sample() for _ in range(4)] for k in cand_keys},
                "base": {k: [self.sample() for _ in range(4)] for k in base_keys}}
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            fails, _ = pg.gate_paired({}, {}, 4, {}, fake)
        return fails, out.getvalue()

    def keys(self):
        return [f"{m}/{s}/{f}/{e}" for (_b, m, s, f, e) in pg.POINTS]

    def test_demux_points_match_the_frame_the_bench_emits(self):
        """The bench reports `frame.size()`, 61 B for the packed-PATH FWD frame since
        1fe92124 (main's history records `fwd-demux-fixed 61B/...`); pin both the size and
        the mode names the bench source emits."""
        demux = [(m, s) for (b, m, s, _f, _e) in pg.POINTS if b == "demux"]
        # The frame-keyed rows carry the frame size; the `fwd-demux-value` ladder row
        # (#1806) is keyed by its payload, so it is pinned separately.
        frame_keyed = [(m, s) for (m, s) in demux if m != "fwd-demux-value"]
        self.assertEqual(frame_keyed, [("fwd-demux-fixed", 61), ("fwd-demux-scan", 61)])
        self.assertEqual([x for x in demux if x[0] == "fwd-demux-value"],
                         [("fwd-demux-value", 16384)])
        src = (pg.HERE / "bench_forward_demux.cpp").read_text()
        self.assertIn('run_point(n, 1, "fwd-demux-fixed")', src)
        self.assertIn('run_point(n, n, "fwd-demux-scan")', src)
        self.assertIn('run_point(1, 1, "fwd-demux-value", p,', src)

    def test_every_point_present_passes(self):
        fails, _ = self.gate(self.keys(), self.keys())
        self.assertEqual(fails, [])

    def test_a_key_absent_from_both_arms_fails_loudly(self):
        k = "fwd-demux-fixed/61/1/1"
        present = [x for x in self.keys() if x != k]
        fails, out = self.gate(present, present)
        self.assertEqual(len(fails), 1)
        self.assertTrue(fails[0].startswith(k))
        self.assertIn("::error::", out)
        # The cliff family's own "not emitted ... not gated" line is about a different
        # family; the absent gated key must never be the one read as not gated.
        self.assertNotIn("absent from one arm", out)

    def test_a_key_absent_from_the_candidate_only_fails(self):
        k = "fwd-demux-scan/61/64/64"
        fails, _ = self.gate([x for x in self.keys() if x != k], self.keys())
        self.assertEqual([f.split()[0] for f in fails], [k])

    def test_a_key_absent_from_the_baseline_only_is_not_gated(self):
        k = "eptype-stream/64/1/1"  # a point the baseline build predates
        fails, out = self.gate(self.keys(), [x for x in self.keys() if x != k])
        self.assertEqual(fails, [])
        self.assertIn("not gated", out)

    def test_multi_rows_on_a_small_host_are_not_failed(self):
        present = [x for x in self.keys() if x not in pg.MAY_BE_ABSENT]
        fails, _ = self.gate(present, present)
        self.assertEqual(fails, [])

    def test_a_multi_row_the_baseline_has_but_the_candidate_dropped_fails(self):
        k = sorted(pg.MAY_BE_ABSENT)[0]
        fails, _ = self.gate([x for x in self.keys() if x != k], self.keys())
        self.assertEqual([f.split()[0] for f in fails], [k])

    def test_the_old_79_byte_rows_do_not_satisfy_the_gate(self):
        """The exact input of the seven-week gap: the bench emits 61 B rows only."""
        transcript = "\n".join([_row("fwd-demux-fixed", 61, 1, 1),
                                 _row("fwd-demux-scan", 61, 64, 64)]) + "\n"
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "bench_forward_demux"
            p.write_text("")
            with unittest.mock.patch.object(pg, "timed_run",
                                            lambda a, *_x, **_k: _measurement(a, transcript)):
                rows = pg.run_bench_once(p)
        for (b, m, s, f, e) in pg.POINTS:
            if b == "demux" and m != "fwd-demux-value":  # payload-keyed ladder row (#1806)
                self.assertIsNotNone(pg.metric(rows, m, s, f, e), m)
        self.assertIsNone(pg.metric(rows, "fwd-demux-fixed", 79, 1, 1))
        self.assertIsNone(pg.metric(rows, "fwd-demux-scan", 79, 64, 64))

    def test_the_legacy_path_fails_a_missing_key(self):
        """No baseline binary: `best_of` is the only arm, and a key it did not emit fails."""
        with tempfile.TemporaryDirectory() as d:
            bench = pathlib.Path(d) / "bench_libtracer"
            bench.write_text("")
            out = io.StringIO()
            argv = ["perf_gate.py", "--tier", "blocking", "--bench", str(bench),
                    "--bench-fwd", str(pathlib.Path(d) / "absent_fwd")]
            cur = {k: self.sample() for k in self.keys() if k != "inproc/64/1/1"}
            with unittest.mock.patch.object(sys, "argv", argv), \
                    unittest.mock.patch.object(pg, "best_of", lambda *a: dict(cur)), \
                    unittest.mock.patch.object(pg, "lkv_ratio_report", lambda *a: None), \
                    unittest.mock.patch.object(pg, "mem_probe", lambda *a: {}), \
                    unittest.mock.patch.object(pg, "BASELINE", pathlib.Path(d) / "b.json"), \
                    unittest.mock.patch.object(pg, "LEDGER", pg.bc.Ledger()), \
                    unittest.mock.patch.object(pg, "BENCH_ERRORS", []), \
                    contextlib.redirect_stdout(out):
                rc = pg.main()
        self.assertEqual(rc, 1, out.getvalue())
        self.assertIn("inproc/64/1/1 not measured", out.getvalue())


class StoreLatencyRowsAreGated(unittest.TestCase):
    """@brief #1869: bench_store_sweep's `RESULT_STORE_LAT` rows reach the gate as POINTS.

    They were banked and charted, never gated, so a store read or write that got slower
    merged with the gate silent. These pin the parser, the key shape, the profiles and legs
    covered, the missing-key rule and the absence of a clock-tick guard on batch rows."""

    TRANSCRIPT = ("# RESULT_STORE_LAT round tag arm leg p50ps p99ps meanps n batch\n"
                  "RESULT_STORE_LAT\t0\tA\tNARROW\tgraph-read\t300500\t490000\t310000"
                  "\t256\t128\n"
                  "RESULT_STORE_LAT\t0\tA\tWIDE\tgraph-write\t77125\t110000\t80000"
                  "\t256\t256\n")

    def rows(self) -> list[tuple]:
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "bench_store_sweep"
            p.write_text("")
            with unittest.mock.patch.object(pg, "timed_run",
                                            lambda a, *_x, **_k: _measurement(a, self.TRANSCRIPT)):
                return pg.run_bench_once(p)

    def test_rows_parse_to_point_keys_with_p50_only(self):
        rows = self.rows()
        v = pg.metric(rows, "store-lat-narrow-graph-read", 32, 1, 1)
        self.assertEqual(v, {"p50_ns": 300.5, "deliv_s": 0.0, "mean_ns": 0.0})
        self.assertEqual(pg.metric(rows, "store-lat-wide-graph-write", 32, 1, 1)["p50_ns"],
                         77.125)

    def test_every_leg_is_gated_on_a_narrow_and_a_wide_profile(self):
        store = {m for (b, m, _s, _f, _e) in pg.POINTS if b == "store"}
        for profile in ("narrow", "wide"):
            for leg in ("net-fwd", "graph-write", "graph-read", "full"):
                self.assertIn(f"store-lat-{profile}-{leg}", store)
        src = (pg.HERE / "bench_store_sweep.cpp").read_text()
        for leg in ("net-fwd", "graph-write", "graph-read", "full"):
            self.assertIn(f'"{leg}"', src, f"bench_store_sweep no longer names leg {leg}")
        self.assertEqual(pg.BENCH_BY_KEY["store"], "bench_store_sweep")

    def test_every_gate_build_step_builds_every_gated_binary(self):
        """A binary the workflow does not build is a missing key on every run (#1847): each
        `cmake --build` step that builds `bench_libtracer` for the gate builds them all."""
        wf = pg.HERE.parent / ".github" / "workflows" / "perf.yml"
        if not wf.exists():
            self.skipTest(f"{wf} not present")
        steps = re.findall(r"cmake --build [^\n]*--target bench_libtracer(?:[^\n]*\\\n)*[^\n]*",
                           wf.read_text())
        self.assertGreaterEqual(len(steps), 4)
        for step in steps:
            for name in pg.BENCH_BY_KEY.values():
                self.assertIn(name, step, f"perf.yml builds without {name}: {step}")

    def test_value_bytes_match_the_workload(self):
        """The row does not print its value size; the key's size is the header's constant."""
        hdr = (pg.HERE / "store_sweep_node.hpp").read_text()
        m = re.search(r"kValueBytes\s*=\s*(\d+)", hdr)
        self.assertIsNotNone(m)
        self.assertEqual(int(m.group(1)), pg.STORE_LAT_VALUE_BYTES)
        for (b, _m, s, _f, _e) in pg.POINTS:
            if b == "store":
                self.assertEqual(s, pg.STORE_LAT_VALUE_BYTES)

    def test_a_missing_store_key_fails(self):
        keys = [f"{m}/{s}/{f}/{e}" for (_b, m, s, f, e) in pg.POINTS]
        k = "store-lat-wide-graph-read/32/1/1"
        sample = {"p50_ns": 300.0, "mean_ns": 0.0, "deliv_s": 0.0}
        fake = {"cand": {x: [dict(sample) for _ in range(4)] for x in keys if x != k},
                "base": {x: [dict(sample) for _ in range(4)] for x in keys}}
        with contextlib.redirect_stdout(io.StringIO()):
            fails, _ = pg.gate_paired({}, {}, 4, {}, fake)
        self.assertEqual([f.split()[0] for f in fails], [k])

    def test_a_ten_ns_regression_at_76_ns_is_not_tick_guarded(self):
        """graph-write runs ~76 ns: a real +16% must fail, where the tick guard would hold
        it until +25 ns."""
        self.assertFalse(pg.tick_guarded("store-lat-wide-graph-write/32/1/1"))
        self.assertTrue(pg.tick_guarded("inproc/64/1/1"))
        keys = [f"{m}/{s}/{f}/{e}" for (_b, m, s, f, e) in pg.POINTS]
        k = "store-lat-wide-graph-write/32/1/1"

        def arm(p50: float) -> list[dict]:
            return [{"p50_ns": p50 + i * 0.1, "mean_ns": 0.0, "deliv_s": 0.0} for i in range(4)]

        fake = {"cand": {x: arm(88.5 if x == k else 300.0) for x in keys},
                "base": {x: arm(76.0 if x == k else 300.0) for x in keys}}
        with contextlib.redirect_stdout(io.StringIO()):
            fails, _ = pg.gate_paired({}, {}, 4, {}, fake)
        self.assertEqual(len(fails), 1, fails)
        self.assertTrue(fails[0].startswith(f"{k} p50 pullback"), fails)


class NonZeroBenchExitIsInconclusive(unittest.TestCase):
    """@brief #1847: a bench that exited non-zero makes the verdict INCONCLUSIVE.

    Its partial transcript used to drop rows into "not gated" and the gate printed PASS
    over a crash. A non-zero exit is not a verdict on the code, so it is neither PASS nor
    FAIL: the blocking tier exits `EXIT_INCONCLUSIVE`, the advisory tier 0."""

    def timed_with_rc(self, rc: int) -> list[str]:
        errors: list[str] = []
        fake = pg.bc.Measurement(["./bench_forward_demux"], "", "boom", rc, [])
        with unittest.mock.patch.object(pg.bc, "measure", lambda *a, **k: fake), \
                unittest.mock.patch.object(pg, "LEDGER", unittest.mock.Mock(add=lambda m: m)), \
                unittest.mock.patch.object(pg, "BENCH_ERRORS", errors), \
                contextlib.redirect_stdout(io.StringIO()):
            pg.timed(["./bench_forward_demux"], timeout=1)
        return errors

    def test_timed_records_a_non_zero_exit(self):
        self.assertEqual(self.timed_with_rc(134), ["bench_forward_demux exited 134"])

    def test_timed_records_nothing_on_success(self):
        self.assertEqual(self.timed_with_rc(0), [])

    def verdict(self, tier: str, fails: list[str]) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = pg.render_verdict(fails, [], tier, None, pg.bc.Ledger(),
                                   ["bench_forward_demux exited 134"])
        return rc, out.getvalue()

    def test_blocking_tier_is_inconclusive_not_pass(self):
        rc, out = self.verdict("blocking", [])
        self.assertEqual(rc, pg.EXIT_INCONCLUSIVE)
        self.assertIn("PERF: INCONCLUSIVE", out)
        self.assertNotIn("PERF: PASS", out)
        self.assertIn("exited 134", out)

    def test_inconclusive_overrides_the_missing_key_fails(self):
        rc, out = self.verdict("blocking", ["fwd-demux-fixed/61/1/1 not measured: ..."])
        self.assertEqual(rc, pg.EXIT_INCONCLUSIVE)
        self.assertIn("  ? fwd-demux-fixed/61/1/1", out)
        self.assertNotIn("  ! ", out)

    def test_advisory_tier_reports_and_exits_zero(self):
        rc, out = self.verdict("advisory", [])
        self.assertEqual(rc, 0)
        self.assertIn("PERF: INCONCLUSIVE", out)
        self.assertIn("::warning::", out)


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


class AllocatorCliffFamily(unittest.TestCase):
    """@brief The allocator-cliff checks (#1806): neighbour steps, the comparison against main,
    and the segment-draw ratchet that a reverted #1768 split must fail at 985 B."""

    @staticmethod
    def _samples(arm_rows: dict[str, dict[int, list[float]]],
                 mode: str = "cliff-alloc-heap") -> dict[str, dict[str, list[dict]]]:
        """{arm: {size: [p50 per pair]}} -> the paired-samples shape gate_cliff reads."""
        return {arm: {f"{mode}/{size}/1/1": [{"p50_ns": v, "mean_ns": v, "deliv_s": 1e9 / v}
                                             for v in vs]
                      for size, vs in rows.items()}
                for arm, rows in arm_rows.items()}

    # The healthy heap shape measured on the reference host: a ~1.5x step at 985 B (two draws)
    # and a ~1.35x step at 1040 B (the payload alone past the cache ceiling).
    HEALTHY = {960: [12.1] * 4, 984: [12.2] * 4, 985: [18.3] * 4, 1032: [18.5] * 4,
               1040: [25.0] * 4, 4096: [25.4] * 4}

    def test_healthy_steps_are_not_cliffs(self):
        with contextlib.redirect_stdout(io.StringIO()):
            fails, warns = pg.gate_cliff(self._samples({"cand": self.HEALTHY,
                                                        "base": self.HEALTHY}))
        self.assertEqual((fails, warns), ([], []))

    def test_a_new_step_fails_at_its_size(self):
        cand = {**self.HEALTHY, 985: [40.0] * 4}
        with contextlib.redirect_stdout(io.StringIO()):
            fails, _ = pg.gate_cliff(self._samples({"cand": cand, "base": self.HEALTHY}))
        self.assertTrue(any("cliff at 985 B" in f for f in fails), fails)
        self.assertTrue(any("cliff-alloc-heap/985/1/1 p50 pullback" in f for f in fails), fails)

    def test_a_cliff_main_already_has_only_warns(self):
        both = {**self.HEALTHY, 985: [40.0] * 4}
        with contextlib.redirect_stdout(io.StringIO()):
            fails, warns = pg.gate_cliff(self._samples({"cand": both, "base": both}))
        self.assertEqual(fails, [])
        self.assertTrue(any("cliff at 985 B" in w for w in warns), warns)

    def test_one_noisy_pair_is_not_a_cliff(self):
        cand = {**self.HEALTHY, 985: [18.3, 40.0, 18.4, 18.2]}
        with contextlib.redirect_stdout(io.StringIO()):
            fails, _ = pg.gate_cliff(self._samples({"cand": cand, "base": self.HEALTHY}))
        self.assertEqual(fails, [])

    def test_no_cliff_rows_is_said_not_passed_silently(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            fails, _ = pg.gate_cliff({"cand": {}, "base": {}})
        self.assertEqual(fails, [])
        self.assertIn("not gated", out.getvalue())

    def test_cliff_rows_are_collected_from_a_run(self):
        rows = [("cliff-alloc-heap", 985, 1, 1, 5e7, 18.3, 18.9),
                ("cliff-alloc-pool", 985, 1, 1, 9e7, 9.1, 9.2),
                ("inproc", 64, 1, 1, 6e6, 170.0, 169.0)]
        self.assertEqual(sorted(pg.cliff_rows(rows)),
                         ["cliff-alloc-heap/985/1/1", "cliff-alloc-pool/985/1/1"])

    # `bench_forward_heap`'s segdraw rows around the boundary, on main and with #1768's
    # layout split reverted (recorded from both builds).
    SEGDRAW_MAIN = """RESULT segdraw S=984 draws=1 bytes=1032 max_block=1032
RESULT segdraw S=985 draws=2 bytes=1025 max_block=985
RESULT segdraw S=1024 draws=2 bytes=1064 max_block=1024
RESULT segdraw S=4096 draws=2 bytes=4136 max_block=4096
"""
    SEGDRAW_REVERTED = """RESULT segdraw S=984 draws=1 bytes=1032 max_block=1032
RESULT segdraw S=985 draws=1 bytes=1033 max_block=1033
RESULT segdraw S=1024 draws=1 bytes=1072 max_block=1072
RESULT segdraw S=4096 draws=1 bytes=4144 max_block=4144
"""

    def test_main_passes_the_segdraw_ratchet(self):
        main = pg.segdraw_parse(self.SEGDRAW_MAIN)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pg.segdraw_gate(main, main), [])

    def test_reverting_the_split_fails_at_985(self):
        """Acceptance (#1806): the reverted split fails the cliff family at 985 B, with or
        without main to compare against, and 984 B stays clean."""
        rev = pg.segdraw_parse(self.SEGDRAW_REVERTED)
        main = pg.segdraw_parse(self.SEGDRAW_MAIN)
        for base in (main, None):
            with contextlib.redirect_stdout(io.StringIO()):
                fails = pg.segdraw_gate(rev, base)
            self.assertTrue(any(f.startswith("segdraw S=985:") and "ceiling" in f
                                for f in fails), fails)
            self.assertFalse(any(f.startswith("segdraw S=984:") for f in fails), fails)
        with contextlib.redirect_stdout(io.StringIO()):
            fails = pg.segdraw_gate(rev, main)
        self.assertTrue(any("segdraw S=985: bytes 1025 -> 1033, max_block 985 -> 1033" in f
                            for f in fails), fails)

    def test_an_extra_draw_fails_exactly(self):
        main = pg.segdraw_parse(self.SEGDRAW_MAIN)
        more = pg.segdraw_parse(self.SEGDRAW_MAIN.replace("S=4096 draws=2", "S=4096 draws=3"))
        with contextlib.redirect_stdout(io.StringIO()):
            fails = pg.segdraw_gate(more, main)
        self.assertEqual(fails, ["segdraw S=4096: draws 2 -> 3 (exact ratchet vs main)"])

    def test_no_segdraw_rows_is_said(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(pg.segdraw_gate({}, None), [])
        self.assertIn("not gated", out.getvalue())


class ExactRows1808(unittest.TestCase):
    """@brief #1808's exact rows: RAM per edge / link / 1 KiB value, blocks per write and the
    STREAM write's stripe-lock sections. Counts, so every rule is exact."""

    # Recorded from `bench_forward_heap` at the commit that added the rows.
    MAIN = """RESULT ramprobe edge_callback blocks_x1000=1035 bytes_x1000=215687 n=256
RESULT ramprobe edge_wire blocks_x1000=8035 bytes_x1000=489187 n=256
RESULT ramprobe link blocks_x1000=13769 bytes_x1000=412187 n=256
RESULT ramprobe vertex_value_1k blocks_x1000=3000 bytes_x1000=1112812 n=256
RESULT writeblocks owned S=1024 seam_x1000=1000 seam_bytes_x1000=40000 heap_x1000=0 n=64
RESULT writeblocks rope2 S=16384 seam_x1000=1000 seam_bytes_x1000=64000 heap_x1000=0 n=64
RESULT streamlock w1 sections_x1000=1000 heap_x1000=0 delivered_x1000=1000 n=64
RESULT streamlock spill sections_x1000=1000 heap_x1000=2000 delivered_x1000=7000 n=64
RESULT streamlock defer sections_x1000=1000 heap_x1000=0 delivered_x1000=0 n=64
RESULT streamlock w4 sections_x1000=1000 delivered_x1000=1000 n=8000
"""

    def gate(self, cur: str, base: str | None) -> list[str]:
        with contextlib.redirect_stdout(io.StringIO()):
            return pg.exact_gate(pg.exact_parse(cur),
                                 pg.exact_parse(base) if base is not None else None)

    def test_every_row_kind_is_parsed(self):
        got = pg.exact_parse(self.MAIN)
        self.assertEqual(len(got), 10)
        self.assertEqual(got["ramprobe:edge_callback"]["blocks_x1000"], 1035)
        self.assertEqual(got["writeblocks:rope2 S=16384"]["seam_bytes_x1000"], 64000)
        self.assertNotIn("heap_x1000", got["streamlock:w4"])

    def test_main_against_itself_passes(self):
        self.assertEqual(self.gate(self.MAIN, self.MAIN), [])
        self.assertEqual(self.gate(self.MAIN, None), [])

    def test_a_fraction_of_a_block_more_fails(self):
        cur = self.MAIN.replace("edge_callback blocks_x1000=1035", "edge_callback blocks_x1000=1036")
        fails = self.gate(cur, self.MAIN)
        self.assertEqual(len(fails), 1)
        self.assertIn("ramprobe:edge_callback: blocks_x1000 1.035 -> 1.036", fails[0])

    def test_a_second_seam_block_per_write_fails(self):
        cur = self.MAIN.replace("owned S=1024 seam_x1000=1000", "owned S=1024 seam_x1000=2000")
        self.assertTrue(any("writeblocks:owned S=1024" in f for f in self.gate(cur, self.MAIN)))

    def test_live_bytes_have_the_per_vertex_tolerance(self):
        one_byte = self.MAIN.replace("bytes_x1000=412187", "bytes_x1000=413187")
        self.assertEqual(self.gate(one_byte, self.MAIN), [])
        many = self.MAIN.replace("bytes_x1000=412187", "bytes_x1000=432187")
        self.assertTrue(any("ramprobe:link: live bytes" in f for f in self.gate(many, self.MAIN)))

    def test_a_second_stripe_section_fails_without_a_baseline(self):
        """#1713's one-section claim needs no main to compare against."""
        for case in ("w1", "spill", "w4"):
            cur = self.MAIN.replace(f"streamlock {case} sections_x1000=1000",
                                    f"streamlock {case} sections_x1000=2000")
            fails = self.gate(cur, None)
            self.assertTrue(any(f.startswith(f"streamlock {case}: 2 stripe-lock") for f in fails),
                            fails)

    def test_heap_on_the_steady_stream_write_fails(self):
        cur = self.MAIN.replace("w1 sections_x1000=1000 heap_x1000=0",
                                "w1 sections_x1000=1000 heap_x1000=1000")
        self.assertTrue(any("streamlock w1: 1 global-heap" in f for f in self.gate(cur, None)))

    def test_a_refused_spill_that_delivers_fails(self):
        cur = self.MAIN.replace("defer sections_x1000=1000 heap_x1000=0 delivered_x1000=0",
                                "defer sections_x1000=1000 heap_x1000=0 delivered_x1000=4000")
        self.assertTrue(any("streamlock defer" in f for f in self.gate(cur, None)))

    def test_a_row_main_has_and_the_candidate_dropped_fails(self):
        cur = "\n".join(ln for ln in self.MAIN.splitlines() if "edge_wire" not in ln)
        fails = self.gate(cur, self.MAIN)
        self.assertTrue(any(f.startswith("ramprobe:edge_wire: main emits") for f in fails))
        self.assertTrue(any("ramprobe:edge_wire is a gated RAM probe" in f for f in fails))

    def test_a_main_without_the_rows_gates_only_the_invariants(self):
        self.assertEqual(self.gate(self.MAIN, ""), [])

    def test_ram_points_are_documented(self):
        doc = pathlib.Path(__file__).resolve().parents[1] / "docs" / "methodology.md"
        if not doc.exists():
            self.skipTest(f"{doc} not present")
        text = doc.read_text()
        for point in pg.RAM_POINTS:
            self.assertTrue(f"`{point}`" in text, f"{point} is gated but not named in {doc}")



class AaNullBankAndReplay(unittest.TestCase):
    """@brief `aa_null.py` (#1807): the spread it banks, and the replay that reports the
    false-fail rate and the injected-10% detection rate the acceptance criteria name."""

    @staticmethod
    def raw(offsets: list[float], noise: float, rounds: int = 12, gone=()) -> dict:
        """@brief A synthetic measurement: one row per build at 100 ns x (1 + offset), with
        a deterministic +-noise wobble per round; (build, round) in @p gone is dropped."""
        import random as _r
        rng = _r.Random(7)
        per_build = []
        for b, off in enumerate(offsets):
            col = []
            for r in range(rounds):
                v = 100.0 * (1 + off) * (1 + rng.uniform(-noise, noise))
                col.append(None if (b, r) in gone else
                           {"p50_ns": v, "mean_ns": v, "deliv_s": 1e9 / v})
            per_build.append(col)
        return {"builds": [f"b{i}" for i in range(len(offsets))], "rounds": rounds,
                "samples": {"inproc/64/1/1": per_build}}

    def test_a_tight_row_banks_a_tight_spread(self):
        import aa_null
        out = aa_null.bank(self.raw([0, 0, 0], 0.005))
        s = out["rows"]["inproc/64/1/1"]["p50_ns"]
        self.assertLess(s, 0.01)
        self.assertAlmostEqual(pg.leg_factor("inproc/64/1/1", "p50_ns", out["rows"])[0],
                               1 + pg.NULL_FLOOR)

    def test_a_layout_offset_between_builds_widens_the_threshold(self):
        """A +8% build is the #1761/#1767 shape: the null must price it, not ignore it."""
        import aa_null
        rows = aa_null.bank(self.raw([0, 0.08, -0.02], 0.005))["rows"]
        self.assertGreater(pg.leg_factor("inproc/64/1/1", "p50_ns", rows)[0], 1.08)

    def test_replay_counts_no_false_fail_and_catches_ten_percent(self):
        import aa_null
        ev = aa_null.evaluate(self.raw([0, 0, 0], 0.01),
                              aa_null.bank(self.raw([0, 0, 0], 0.01, rounds=10))["rows"])
        self.assertGreater(ev["sessions"], 0)
        self.assertEqual(ev["false_fail_sessions"], 0)
        caught, total = ev["detect"]["inproc/64/1/1"]
        self.assertEqual(caught, total)

    def test_a_dropped_round_breaks_only_the_windows_through_it(self):
        import aa_null
        ses = aa_null.session_ratios(self.raw([0, 0], 0.0, rounds=10, gone={(0, 9)})
                                     ["samples"]["inproc/64/1/1"], "p50_ns", 8)
        self.assertEqual(len(ses), 2 * 2)  # windows 0-7 and 1-8, both directions


class HistoryKeepsOneRunnersTuple(unittest.TestCase):
    """@brief The history emitter records one runner's whole tuple per point (#1807)."""

    def test_the_best_p50_runner_brings_its_own_p99_and_throughput(self):
        import perf_emit_benchmark as pe
        a = {"p50_ns": 100.0, "p99_ns": 900.0, "deliv_s": 9.0e6}
        b = {"p50_ns": 105.0, "p99_ns": 300.0, "deliv_s": 9.9e6}
        self.assertEqual(pe.best_tuple([b, a]), a)  # never p99 300 beside p50 100

    def test_a_bulk_only_row_picks_by_throughput(self):
        import perf_emit_benchmark as pe
        a = {"p50_ns": 0.0, "p99_ns": 0.0, "deliv_s": 4.0e7}
        b = {"p50_ns": 0.0, "p99_ns": 0.0, "deliv_s": 4.4e7}
        self.assertEqual(pe.best_tuple([a, b]), b)

if __name__ == "__main__":
    unittest.main(verbosity=2)
