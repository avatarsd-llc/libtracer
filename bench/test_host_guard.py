#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Decision-rule tests for the pinned-host quiescence guard (#1236).

The guard's whole value is that it says NO on a busy machine and YES on a quiet one,
so the two directions are what these tests pin — the same shape `test_perf_gate.py`
uses for the gate's verdicts. The load and clock readers are injected, so "the host
was busy for nine minutes and then settled" is a test case rather than a nine-minute
test.

The store-side cases pin the other half of the acceptance: a flagged sample must be
provably ignored by the consumer that reads the series, and a clean one must be
indistinguishable from today.

    python3 bench/test_host_guard.py    # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import host_guard as hg  # noqa: E402
import render_history as rh  # noqa: E402


def transcript(rows: list[tuple[str, float, float]]) -> str:
    """@brief A RESULT transcript from (mode, deliv_s, p50_ns) triples.

    Mirrors bench_libtracer's 12-column tab-separated shape exactly; anything less
    faithful would test the fixture rather than the parser.
    """
    out = ["# a comment line the parser must ignore"]
    for mode, deliv, p50 in rows:
        out.append("\t".join(["RESULT", "libtracer", mode, "64", "1", "1",
                              "0", f"{deliv}", "0", f"{int(p50)}", "0", "0"]))
    return "\n".join(out) + "\n"


class LoadBar(unittest.TestCase):
    """@brief The bar scales with the machine and never drops below 1.0."""

    def test_scales_with_cpu_count(self):
        self.assertAlmostEqual(hg.load_bar(31, 0.25), 7.75)
        self.assertAlmostEqual(hg.load_bar(8, 0.25), 2.0)

    def test_floor_keeps_a_tiny_host_measurable(self):
        # 0.25 * 2 = 0.5 would refuse a machine doing nothing but running the bench.
        self.assertEqual(hg.load_bar(2, 0.25), 1.0)


class WaitForQuiet(unittest.TestCase):
    """@brief The skip decision: busy hosts skip, settling hosts measure."""

    def _run(self, loads, timeout=600.0):
        seq = list(loads)
        clock = {"t": 0.0}

        def read():
            return seq.pop(0) if seq else seq_last[0]

        seq_last = [loads[-1]]

        def now():
            return clock["t"]

        def sleep(dt):
            clock["t"] += dt

        return hg.wait_for_quiet(4.0, timeout, poll=15.0, now=now, sleep=sleep, read=read)

    def test_quiet_immediately_measures(self):
        quiet, load, waited = self._run([0.4])
        self.assertTrue(quiet)
        self.assertEqual(waited, 0.0)

    def test_settles_after_its_own_build_and_then_measures(self):
        # The workflow's `cmake --build -j31` precedes the measurement, so the first
        # reads are ALWAYS high. Refusing here would refuse every run — the reason
        # this is a wait and not a check.
        quiet, load, waited = self._run([28.0, 19.0, 9.0, 3.1])
        self.assertTrue(quiet)
        self.assertAlmostEqual(load, 3.1)
        self.assertEqual(waited, 45.0)

    def test_never_settles_skips_and_does_not_fail(self):
        quiet, load, _ = self._run([12.0] * 100, timeout=60.0)
        self.assertFalse(quiet)
        self.assertAlmostEqual(load, 12.0)

    def test_skip_is_exit_zero(self):
        """A busy host must leave the job GREEN — a red job trains the reader to ignore red."""
        import argparse
        args = argparse.Namespace(max_load_per_cpu=0.0001, timeout=0.0, poll=1.0, ncpu=31)
        self.assertEqual(hg._cmd_wait(args), 0)

    def test_busy_host_flags_the_sample_instead_of_skipping_it(self):
        """A host that never settles banks a FLAGGED sample: the page shows it marked."""
        import argparse
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            out = pathlib.Path(d) / "out"
            out.write_text("")
            old = os.environ.get("GITHUB_OUTPUT")
            os.environ["GITHUB_OUTPUT"] = str(out)
            try:
                args = argparse.Namespace(max_load_per_cpu=0.0001, timeout=0.0, poll=1.0,
                                          ncpu=31)
                hg._cmd_wait(args)
            finally:
                if old is None:
                    del os.environ["GITHUB_OUTPUT"]
                else:
                    os.environ["GITHUB_OUTPUT"] = old
            kv = dict(ln.split("=", 1) for ln in out.read_text().splitlines() if "=" in ln)
        self.assertNotIn("bank", kv)  # nothing downstream may gate banking on it again
        if kv["quiet"] == "false":  # the bar is ~0, so only an idle-at-zero host is quiet
            self.assertTrue(hg.is_contaminated(kv["note"]))
            self.assertIn("host load", kv["note"])

    def test_busy_note_is_a_contamination_flag(self):
        self.assertTrue(hg.is_contaminated(hg.busy_note(57.1, 7.75)))


class Bracket(unittest.TestCase):
    """@brief The A/A null pair: same binary either side of the measured run."""

    def test_identical_pair_is_clean(self):
        t = transcript([("inproc", 1.0e7, 100), ("chain-forward", 2.0e7, 43)])
        clean, _, p75, pct = hg.bracket_verdict(t, t)
        self.assertTrue(clean)
        self.assertEqual((p75, pct), (0.0, 0.0))

    def test_noise_under_the_band_is_clean(self):
        pre = transcript([("inproc", 1.0e7, 100)])
        post = transcript([("inproc", 1.04e7, 102)])  # 4% — the measured null floor
        clean, _, _, pct = hg.bracket_verdict(pre, post)
        self.assertTrue(clean)
        self.assertLess(pct, hg.AA_RUN_P75_BAND)

    def test_a_neighbour_arriving_mid_run_is_caught(self):
        pre = transcript([("inproc", 1.0e7, 100)])
        post = transcript([("inproc", 0.85e7, 118)])  # the shape sample 141 has
        clean, what, _, pct = hg.bracket_verdict(pre, post)
        self.assertFalse(clean)
        self.assertGreater(pct, 10.0)
        self.assertIn("inproc", what)

    def test_one_wrecked_row_still_flags_through_the_cap(self):
        # One contaminated row among many clean ones must still flag when it is past the
        # cap: averaging is how a real disturbance gets diluted into silence.
        pre = transcript([("a", 1e7, 100), ("b", 1e7, 100), ("c", 1e7, 100)])
        post = transcript([("a", 1e7, 100), ("b", 1e7, 100), ("c", 0.7e7, 100)])
        clean, what, p75, pct = hg.bracket_verdict(pre, post)
        self.assertFalse(clean)
        self.assertEqual(p75, 0.0)
        self.assertIn(" c ", what)

    def test_no_comparable_rows_does_not_invent_a_verdict(self):
        clean, what, p75, pct = hg.bracket_verdict("", "")
        self.assertTrue(clean)
        self.assertEqual((p75, pct), (0.0, 0.0))
        self.assertIn("no comparable rows", what)

    def test_p50_moves_alone_are_caught(self):
        # Throughput can sit still while latency steps; the guard reads both legs.
        pre = transcript([("inproc", 1e7, 100)])
        post = transcript([("inproc", 1e7, 130)])
        clean, what, _, _ = hg.bracket_verdict(pre, post)
        self.assertFalse(clean)
        self.assertIn("p50_ns", what)


def _spread(pcts: list[float]) -> tuple[str, str]:
    """@brief An A/A pair with one row per entry of @p pcts, each that many percent slow.

    Throughput and p50 both move by exactly that percent, so every point is two rows of
    the same disagreement and the row distribution is @p pcts, each value twice.
    """
    pre = transcript([(f"m{i}", 1e7, 100.0) for i in range(len(pcts))])
    post = transcript([(f"m{i}", 1e7 * (1 - p / 100.0), 100.0 * (1 + p / 100.0))
                       for i, p in enumerate(pcts)])
    return pre, post


class RunLevelRule(unittest.TestCase):
    """@brief The run-level A/A rule (ruling 2026-10-07): p75 row over the band, or any
    row over the cap. The worst row alone no longer decides."""

    def test_thresholds(self):
        self.assertEqual((hg.AA_RUN_P75_BAND, hg.AA_RUN_ROW_CAP), (6.0, 15.0))

    def test_d678bf17_like_run_stays_clean(self):
        # 45 rows, median ~0.9%, two rows over 6% and none over the cap: what the old
        # worst-row rule flagged on nearly every clean run.
        pcts = [0.9] * 43 + [7.5, 13.5]
        clean, _, p75, worst = hg.bracket_verdict(*_spread(pcts))
        self.assertTrue(clean)
        self.assertLess(p75, hg.AA_RUN_P75_BAND)
        self.assertAlmostEqual(worst, 13.5)

    def test_p75_just_over_the_band_flags(self):
        # 8 rows; the nearest-rank p75 is the 6th smallest. Put it at 6.1%, max under cap.
        pcts = [0.0] * 2 + [1.0] * 3 + [6.1, 7.0, 8.0]
        clean, _, p75, worst = hg.bracket_verdict(*_spread(pcts))
        self.assertFalse(clean)
        self.assertAlmostEqual(p75, 6.1)
        self.assertLess(worst, hg.AA_RUN_ROW_CAP)

    def test_p75_at_the_band_is_clean(self):
        pcts = [0.0] * 2 + [1.0] * 3 + [5.9, 7.0, 8.0]
        clean, _, p75, _ = hg.bracket_verdict(*_spread(pcts))
        self.assertTrue(clean)
        self.assertAlmostEqual(p75, 5.9)

    def test_one_row_over_the_cap_flags_a_quiet_run(self):
        pcts = [0.5] * 44 + [15.5]
        clean, what, p75, worst = hg.bracket_verdict(*_spread(pcts))
        self.assertFalse(clean)
        self.assertLess(p75, 1.0)
        self.assertAlmostEqual(worst, 15.5)
        self.assertIn("m44", what)

    def test_one_row_at_the_cap_is_clean(self):
        clean, _, _, _ = hg.bracket_verdict(*_spread([0.5] * 44 + [14.9]))
        self.assertTrue(clean)

    def test_note_carries_the_measured_p75_and_max(self):
        note = hg.bracket_note(6.4, 9.2)
        self.assertTrue(hg.is_contaminated(note))
        self.assertIn("p75 6.4%", note)
        self.assertIn("max 9.2%", note)
        self.assertNotIn(hg.SEP, note)  # one field of `extra`, not two

    def test_rows_over_the_row_band_are_still_named_on_a_clean_run(self):
        # The per-row marking for the charts is unchanged: a clean run's two rows over
        # `DEFAULT_BAND` still carry ROW_TOKEN.
        pre, post = _spread([0.9] * 43 + [7.5, 13.5])
        self.assertTrue(hg.bracket_verdict(pre, post)[0])
        names = hg.bracket_failed_names(pre, post)
        self.assertEqual(len(names), 6)  # 2 points x (throughput, ns/delivery, p50)
        self.assertTrue(any("m43" in n for n in names) and any("m44" in n for n in names))


class ContaminationPredicate(unittest.TestCase):
    """@brief One predicate, both sources — the self-flag and the retroactive list."""

    def test_extra_flag_is_recognised(self):
        note = hg.contamination_note("A/A bracket 9.1% > 6.0% band")
        self.assertTrue(hg.is_contaminated(f"studio · pinned cpu2 · g++ 13.3.0 · {note}"))

    def test_clean_extra_is_not(self):
        self.assertFalse(hg.is_contaminated("studio · pinned cpu2 · g++ 13.3.0"))
        self.assertFalse(hg.is_contaminated(None))
        self.assertFalse(hg.is_contaminated(""))

    def test_known_list_flags_by_sha_prefix(self):
        known = {"3b28d15b": "sample 141"}
        e = {"commit": {"id": "3b28d15bb4b992c9c9682360370ad9b50a000218"}, "benches": []}
        self.assertEqual(hg.entry_contaminated(e, known), "sample 141")

    def test_unlisted_clean_sample_is_trusted(self):
        e = {"commit": {"id": "deadbeef" * 5},
             "benches": [{"name": "x", "value": 1, "extra": "studio · g++ 13.3.0"}]}
        self.assertIsNone(hg.entry_contaminated(e, {}))

    def test_shipped_list_contains_sample_141(self):
        """The acceptance criterion, asserted against the file that ships."""
        known = hg.load_known_contaminated()
        self.assertTrue(any(s.startswith("3b28d15b") for s in known),
                        "sample 141 (3b28d15b) must be listed as contaminated")


class RendererIgnoresFlaggedSamples(unittest.TestCase):
    """@brief The consumer half: a flagged sample must not shape the trend."""

    def _store(self, extras: list[str], shas: list[str],
               values: list[float] | None = None) -> dict:
        values = values or [100 + i for i in range(len(shas))]
        return {"entries": {"libtracer bench-local latency (ns, smaller is better)": [
            {"commit": {"id": sha, "message": f"commit {i}"},
             "benches": [{"name": "inproc 64B/fan1/1ep p50 latency", "value": v,
                          "unit": "ns", "extra": extra}]}
            for i, (sha, extra, v) in enumerate(zip(shas, extras, values))]}}

    def test_flagged_outlier_is_omitted_from_the_series(self):
        note = hg.contamination_note("A/A bracket 12.0% > 6.0% band")
        data = self._store(["clean host", f"clean host · {note}", "clean host"],
                           ["a" * 40, "b" * 40, "c" * 40], [100, 150, 102])
        entries = list(data["entries"].values())[0]
        skip = rh._untrusted_cells(entries)
        self.assertEqual(set(skip), {1})
        series, sus = rh._series_by_name(entries, skip)
        idxs = [p[0] for p in series["inproc 64B/fan1/1ep p50 latency"]]
        self.assertEqual(idxs, [0, 2], "the flagged sample must stay off the line")
        self.assertEqual(sus["inproc 64B/fan1/1ep p50 latency"], [[1, 150.0]],
                         "the flagged sample is kept, marked suspect, not dropped")

    def test_flagged_row_inside_its_neighbours_is_drawn(self):
        """#1890: a flagged run no longer hides a row that agrees with its own neighbours."""
        note = hg.contamination_note("A/A bracket 12.0% > 6.0% band")
        data = self._store(["h", f"h · {note}", "h"], ["a" * 40, "b" * 40, "c" * 40])
        entries = list(data["entries"].values())[0]
        series, sus = rh._series_by_name(entries, rh._untrusted_cells(entries))
        self.assertEqual([p[0] for p in series["inproc 64B/fan1/1ep p50 latency"]], [0, 1, 2])
        self.assertEqual(sus, {})

    def test_clean_store_is_unchanged(self):
        """A quiet host must produce exactly what it produces today."""
        data = self._store(["clean host"] * 3, ["a" * 40, "b" * 40, "c" * 40])
        entries = list(data["entries"].values())[0]
        self.assertEqual(rh._untrusted_cells(entries), {})
        self.assertEqual(len(rh._series_by_name(entries, {})[0]
                             ["inproc 64B/fan1/1ep p50 latency"]), 3)

    def test_payload_explains_the_gap(self):
        note = hg.contamination_note("A/A bracket 12.0% > 6.0% band")
        data = self._store(["h", f"h · {note}", "h"], ["a" * 40, "b" * 40, "c" * 40],
                           [100, 150, 102])
        out = rh.build(data, colors={})
        self.assertIn("1", out["suites"]["latency"]["contaminated"])

    def test_gating_keeps_the_whole_run_verdict(self):
        """The per-row rule is the charts'; `entry_contaminated` still flags the whole run."""
        note = hg.contamination_note("A/A bracket 12.0% > 6.0% band")
        data = self._store(["h", f"h · {note}", "h"], ["a" * 40, "b" * 40, "c" * 40])
        entries = list(data["entries"].values())[0]
        self.assertTrue(hg.entry_contaminated(entries[1], {}))


class PerRowBracket(unittest.TestCase):
    """@brief The bracket names the banked rows that failed their own A/A check (#1890)."""

    def test_only_rows_over_the_band_are_named(self):
        pre = transcript([("fwd-demux-fixed", 1000.0, 100.0), ("fwd-demux-scan", 1000.0, 100.0)])
        post = transcript([("fwd-demux-fixed", 1001.0, 100.0), ("fwd-demux-scan", 800.0, 100.0)])
        self.assertEqual(hg.bracket_failed_names(pre, post, 6.0),
                         ["fwd-demux-scan 64B/fan1/1ep ns/delivery",
                          "fwd-demux-scan 64B/fan1/1ep throughput"])
        # The whole-run verdict over the same pair is unchanged: one row fails it.
        clean, what, _, pct = hg.bracket_verdict(pre, post)
        self.assertFalse(clean)
        self.assertIn("fwd-demux-scan", what)
        self.assertAlmostEqual(pct, 20.0)

    def test_stamp_flags_exactly_the_named_points(self):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "m.json"
            p.write_text(json.dumps([{"name": "a", "value": 1}, {"name": "b", "value": 2}]))
            hg.stamp([p], "studio", {"b"})
            got = {it["name"]: it["extra"] for it in json.loads(p.read_text())}
            self.assertEqual(got, {"a": "studio", "b": f"studio · {hg.ROW_TOKEN}"})
            self.assertNotIn(hg.CONTAM_TOKEN, hg.ROW_TOKEN)


class Stamping(unittest.TestCase):
    """@brief Every banked point records the conditions it was taken under."""

    def test_stamp_writes_extra_on_every_point(self):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "m.json"
            p.write_text(json.dumps([{"name": "a", "value": 1, "unit": "ns"},
                                     {"name": "b", "value": 2, "unit": "ns"}]))
            n = hg.stamp([p], "studio · g++ 13.3.0")
            self.assertEqual(n, 2)
            self.assertTrue(all(it["extra"] == "studio · g++ 13.3.0"
                                for it in json.loads(p.read_text())))

    def test_compiler_identity_is_a_version_string(self):
        ident = hg.compiler_identity()
        self.assertTrue(ident, "compiler identity must never be empty")

    def test_clock_floor_is_read_off_the_transcript(self):
        """The CLOCK line a bench prints ahead of its rows becomes a stamped fragment (#1804)."""
        text = "# banner\nCLOCK\t1.000\t21.874\nRESULT\tlibtracer\tfold-b4\t512\t1\t1\t1\t1\t0.0\t3.912\t0\t3.950\n"
        self.assertEqual(hg.clock_floor(text), "clock 1 ns res · 21.9 ns/sample")

    def test_clock_floor_absent_is_none(self):
        """A transcript that predates the CLOCK line stamps nothing rather than a guess."""
        self.assertIsNone(hg.clock_floor("RESULT\tlibtracer\tinproc\t64\t1\t1\t1\t1\t0\t9\t9\t9\n"))

    def test_alloc_state_is_read_off_the_transcript(self):
        """The ALLOC lines every family process prints become one stamped fragment (#1903)."""
        tun = ("glibc.malloc.mmap_threshold=131072:glibc.malloc.trim_threshold=33554432:"
               "glibc.malloc.arena_max=8")
        text = f"CLOCK\t1.000\t21.874\nALLOC\tpinned\t{tun}\nALLOC\tpinned\t{tun}\n"
        self.assertEqual(hg.alloc_state(text), "alloc pinned mmap_threshold=131072:"
                                               "trim_threshold=33554432:arena_max=8")

    def test_alloc_state_disagreeing_processes_say_mixed(self):
        text = "ALLOC\tpinned\tglibc.malloc.arena_max=8\nALLOC\tunpinned\t-\n"
        self.assertEqual(hg.alloc_state(text), "alloc MIXED pinned arena_max=8 | unpinned -")

    def test_alloc_state_absent_is_none(self):
        self.assertIsNone(hg.alloc_state("CLOCK\t1.000\t21.874\n"))

    def test_missing_compiler_does_not_raise(self):
        """A toolchain the guard cannot interrogate must not cost the commit its point."""
        self.assertEqual(hg.compiler_identity("definitely-not-a-compiler-xyz"),
                         "compiler unknown")


if __name__ == "__main__":
    unittest.main(verbosity=2)
