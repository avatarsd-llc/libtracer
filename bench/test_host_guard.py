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


class Bracket(unittest.TestCase):
    """@brief The A/A null pair: same binary either side of the measured run."""

    def test_identical_pair_is_clean(self):
        t = transcript([("inproc", 1.0e7, 100), ("compact-forward", 2.0e7, 43)])
        clean, _, pct = hg.bracket_verdict(t, t)
        self.assertTrue(clean)
        self.assertEqual(pct, 0.0)

    def test_noise_under_the_band_is_clean(self):
        pre = transcript([("inproc", 1.0e7, 100)])
        post = transcript([("inproc", 1.04e7, 102)])  # 4% — the measured null floor
        clean, _, pct = hg.bracket_verdict(pre, post)
        self.assertTrue(clean)
        self.assertLess(pct, hg.DEFAULT_BAND)

    def test_a_neighbour_arriving_mid_run_is_caught(self):
        pre = transcript([("inproc", 1.0e7, 100)])
        post = transcript([("inproc", 0.85e7, 118)])  # the shape sample 141 has
        clean, what, pct = hg.bracket_verdict(pre, post)
        self.assertFalse(clean)
        self.assertGreater(pct, 10.0)
        self.assertIn("inproc", what)

    def test_worst_row_decides_not_the_average(self):
        # One contaminated row among many clean ones must still flag: averaging is
        # how a real disturbance gets diluted into silence.
        pre = transcript([("a", 1e7, 100), ("b", 1e7, 100), ("c", 1e7, 100)])
        post = transcript([("a", 1e7, 100), ("b", 1e7, 100), ("c", 0.7e7, 100)])
        clean, what, pct = hg.bracket_verdict(pre, post)
        self.assertFalse(clean)
        self.assertIn(" c ", what)

    def test_no_comparable_rows_does_not_invent_a_verdict(self):
        clean, what, pct = hg.bracket_verdict("", "")
        self.assertTrue(clean)
        self.assertEqual(pct, 0.0)
        self.assertIn("no comparable rows", what)

    def test_p50_moves_alone_are_caught(self):
        # Throughput can sit still while latency steps; the guard reads both legs.
        pre = transcript([("inproc", 1e7, 100)])
        post = transcript([("inproc", 1e7, 130)])
        clean, what, _ = hg.bracket_verdict(pre, post)
        self.assertFalse(clean)
        self.assertIn("p50_ns", what)


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

    def _store(self, extras: list[str], shas: list[str]) -> dict:
        return {"entries": {"libtracer bench-local latency (ns, smaller is better)": [
            {"commit": {"id": sha, "message": f"commit {i}"},
             "benches": [{"name": "inproc 64B/fan1/1ep p50 latency", "value": 100 + i,
                          "unit": "ns", "extra": extra}]}
            for i, (sha, extra) in enumerate(zip(shas, extras))]}}

    def test_flagged_point_is_omitted_from_the_series(self):
        note = hg.contamination_note("A/A bracket 12.0% > 6.0% band")
        data = self._store(["clean host", f"clean host · {note}", "clean host"],
                           ["a" * 40, "b" * 40, "c" * 40])
        entries = list(data["entries"].values())[0]
        skip = rh._contaminated_idx(entries)
        self.assertEqual(set(skip), {1})
        series = rh._series_by_name(entries, skip)
        idxs = [p[0] for p in series["inproc 64B/fan1/1ep p50 latency"]]
        self.assertEqual(idxs, [0, 2], "the flagged sample must leave a gap, not a point")

    def test_clean_store_is_unchanged(self):
        """A quiet host must produce exactly what it produces today."""
        data = self._store(["clean host"] * 3, ["a" * 40, "b" * 40, "c" * 40])
        entries = list(data["entries"].values())[0]
        self.assertEqual(rh._contaminated_idx(entries), {})
        self.assertEqual(len(rh._series_by_name(entries, {})
                             ["inproc 64B/fan1/1ep p50 latency"]), 3)

    def test_payload_explains_the_gap(self):
        note = hg.contamination_note("A/A bracket 12.0% > 6.0% band")
        data = self._store(["h", f"h · {note}", "h"], ["a" * 40, "b" * 40, "c" * 40])
        out = rh.build(data, colors={})
        self.assertIn("1", out["suites"]["latency"]["contaminated"])


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

    def test_missing_compiler_does_not_raise(self):
        """A toolchain the guard cannot interrogate must not cost the commit its point."""
        self.assertEqual(hg.compiler_identity("definitely-not-a-compiler-xyz"),
                         "compiler unknown")


if __name__ == "__main__":
    unittest.main(verbosity=2)
