#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Decision-rule tests for the ADR-0079 store-sweep gated series (#1428).

The ratchet is warn-first, so for its whole first life a wrong verdict is INVISIBLE —
nothing turns red to report it. These tests are therefore the only thing standing between
"the gate warns on drift" and "the gate warns on nothing", and they run in `perf.yml`
beside the other decision-rule suites (`test_ram_census.py`, `test_store_guard.py`),
before anything is built or measured.

Two rules get more coverage than the rest, because they are the ones this instrument
could plausibly lose:

  * **The exclusions are enforced, not documented.** A pin naming the T-sweep or the
    escape high-water must FAIL the gate. Those two columns have measured nulls of 58.9 %
    and 66 %; a gate over either would be a flaky red, and a flaky red teaches everyone to
    ignore the gate.
  * **A failure to measure is not a budget verdict.** No transcript, a half-calibrated
    run, an overflowing store, a dead pin — every one of them is hard in warn mode too.

    python3 bench/test_store_sweep_gate.py  # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import store_sweep_gate as ssg  # noqa: E402

# A faithful `run_store_sweep.sh` transcript in miniature: the calibration tally before
# AND after, two arms of occupancy, the per-channel draw rows, two rounds x two tags of
# latency — plus one row from each of the two families that may never be gated, because a
# full local sweep is a legal input and the gate has to be seen ignoring them.
SWEEP = "\n".join([
    "# scope: deterministic",
    "CALIBRATE\twide_seam_graph_ctl\tOK\tblocks=2",
    "CALIBRATE\ttotal_failures\t0\t-",
    "# loadavg before: 4.86 14.05 17.47 2/4519 3424108",
    "RESULT_STORE_HWM\tWIDE\t1\twide\t0\t376\t40960\t3\t0",
    "RESULT_STORE_HWM\tNARROW\t1\tgraph\t0\t336\t32768\t2\t0",
    "RESULT_STORE_HWM\tNARROW\t1\tnet-rx\t0\t128\t8192\t1\t0",
    "RESULT_STORE_HWM\tNARROW\t1\tnet-egress\t0\t36\t8192\t1\t0",
    "RESULT_STORE_CHAN\tWIDE\t1\tgraph-ctl\t65\t2080\t376\t0",
    "RESULT_STORE_LAT\t0\tA\tWIDE\tnet-fwd\t250625\t435851\t263818\t256\t128",
    "RESULT_STORE_LAT\t0\tB\tWIDE\tnet-fwd\t252000\t441000\t265000\t256\t128",
    "RESULT_STORE_LAT\t1\tA\tWIDE\tnet-fwd\t251000\t436000\t264000\t256\t128",
    "RESULT_STORE_LAT\t1\tB\tWIDE\tnet-fwd\t249000\t430000\t262000\t256\t128",
    "RESULT_STORE_TPUT\t0\tA\tWIDE\tnet-fwd\t24\t43697\t1048728\t22883",
    "RESULT_STORE_ESCAPE\t0\tA\tWIDE\tbuild\t5096\t744\t121\t23",
    "# loadavg after: 4.45 13.51 17.23 4/4511 3424687",
    "CALIBRATE\ttotal_failures\t0\t-",
]) + "\n"

PINS = {
    # Stamped with THIS machine's compiler so the fixture exercises the matching-toolchain
    # path; the mismatch path gets its own cases below.
    "toolchain": ssg.host_guard.compiler_identity(),
    "band_bytes": 0,
    "band_pct": 0.0,
    "measured_spread_bytes": 0,
    "pins": [
        {"arm": "WIDE", "threads": 1, "metric": "used_total", "value": 376},
        {"arm": "WIDE", "threads": 1, "metric": "stores", "value": 1},
        {"arm": "NARROW", "threads": 1, "metric": "used_total", "value": 500},
        {"arm": "NARROW", "threads": 1, "metric": "stores", "value": 3},
    ],
}


def run_gate(transcript: str, pins: dict, *extra: str) -> tuple[int, str]:
    """@brief Run the gate CLI over a transcript+pins pair; returns (rc, stdout)."""
    with tempfile.TemporaryDirectory() as d:
        raw = pathlib.Path(d) / "sweep.tsv"
        raw.write_text(transcript)
        pj = pathlib.Path(d) / "pins.json"
        pj.write_text(json.dumps(pins))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = ssg.main(["gate", "--sweep-raw", str(raw), "--pins", str(pj), *extra])
        return code, buf.getvalue()


def emit(transcript: str, existing: list | None = None) -> tuple[int, list, str]:
    """@brief Run the emit CLI; returns (rc, merged metrics, stdout)."""
    with tempfile.TemporaryDirectory() as d:
        raw = pathlib.Path(d) / "sweep.tsv"
        raw.write_text(transcript)
        out = pathlib.Path(d) / "benchmark_output.json"
        out.write_text(json.dumps(existing if existing is not None else []))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = ssg.main(["emit", "--sweep-raw", str(raw), "--merge", str(out)])
        return code, json.loads(out.read_text()), buf.getvalue()


class Parsing(unittest.TestCase):
    def test_per_store_rows_sum_into_one_cell_per_arm_and_thread_count(self):
        """NARROW carries 49 store rows at T=24; the pin is the total and the count."""
        _l, _ls, _t, _ts, hwm, _c, _e = ssg.sweep.parse(SWEEP.splitlines())
        totals = ssg.hwm_totals(hwm)
        self.assertEqual(totals[("NARROW", 1)]["used_total"], 336 + 128 + 36)
        self.assertEqual(totals[("NARROW", 1)]["stores"], 3)
        self.assertEqual(totals[("WIDE", 1)]["stores"], 1)

    def test_the_latency_cell_is_best_of_rounds_over_both_tags(self):
        """Best-of-rounds, never median: contamination is one-sided, so the minimum is
        the estimate least disturbed by it (docs/methodology.md)."""
        lat, *_ = ssg.sweep.parse(SWEEP.splitlines())
        self.assertAlmostEqual(ssg.lat_best(lat)[("WIDE", "net-fwd")], 249.0)

    def test_a_missing_transcript_is_not_an_exception(self):
        lines, why = ssg.read_transcript("/nonexistent/sweep.tsv")
        self.assertEqual(lines, [])
        self.assertIn("does not exist", why)


class TheExcludedColumnsAreRefused(unittest.TestCase):
    """The T-sweep and the escape high-water may never be gated. Both have measured
    reasons (a 58.9 % and a 66 % null), and both are enforced rather than documented."""

    def test_a_pin_naming_the_throughput_column_fails_the_gate(self):
        pins = json.loads(json.dumps(PINS))
        pins["pins"].append({"arm": "WIDE", "threads": 24, "metric": "per_thread_ops_s",
                             "value": 43697})
        code, out = run_gate(SWEEP, pins)
        self.assertEqual(code, 1)
        self.assertIn("ungateable-pin", out)

    def test_a_pin_naming_the_escape_high_water_fails_the_gate(self):
        pins = json.loads(json.dumps(PINS))
        pins["pins"].append({"arm": "WIDE", "threads": 1, "metric": "escape_peak",
                             "value": 744})
        code, out = run_gate(SWEEP, pins)
        self.assertEqual(code, 1)
        self.assertIn("ungateable-pin", out)

    def test_a_malformed_pin_is_a_wiring_error_not_a_crash(self):
        pins = json.loads(json.dumps(PINS))
        pins["pins"].append({"arm": "WIDE", "metric": "used_total", "value": 376})
        code, out = run_gate(SWEEP, pins)
        self.assertEqual(code, 1)
        self.assertIn("missing threads", out)

    def test_excluded_rows_are_counted_and_announced_rather_than_silently_ignored(self):
        _code, out = run_gate(SWEEP, PINS)
        self.assertIn("excluded [throughput]: 1 row(s) read and NOT gated", out)
        self.assertIn("excluded [escape]: 1 row(s) read and NOT gated", out)

    def test_excluded_rows_never_reach_the_store(self):
        _code, got, _out = emit(SWEEP)
        for m in got:
            self.assertFalse(m["name"].startswith("store-tput"))
            self.assertNotIn("escape", m["name"])


class SeriesShape(unittest.TestCase):
    def test_both_deterministic_columns_are_banked(self):
        _code, got, _out = emit(SWEEP)
        names = {m["name"] for m in got}
        self.assertIn("store-sweep WIDE T1 used_total", names)
        self.assertIn("store-lat WIDE net-fwd p50", names)

    def test_the_store_count_is_asserted_and_not_trended(self):
        """A series that never moves cannot alert — publishing it would look like a
        measurement and be none. The gate pins it exactly instead (ram_census.py's rule
        for its own invariant rows)."""
        _code, got, _out = emit(SWEEP)
        self.assertFalse([m for m in got if m["name"].endswith("stores")])
        self.assertIn("stores", [p["metric"] for p in PINS["pins"]])

    def test_units_are_recorded(self):
        _code, got, _out = emit(SWEEP)
        by = {m["name"]: m for m in got}
        self.assertEqual(by["store-sweep WIDE T1 used_total"]["unit"], "bytes")
        self.assertEqual(by["store-lat WIDE net-fwd p50"]["unit"], "ns")

    def test_merge_appends_without_disturbing_the_latency_metrics(self):
        _code, got, _out = emit(SWEEP, [{"name": "inproc 64B/fan1/1ep p50 latency",
                                         "unit": "ns", "value": 42}])
        self.assertEqual(got[0]["value"], 42)
        self.assertIn("store-sweep WIDE T1 used_total", {m["name"] for m in got})

    def test_merge_is_idempotent(self):
        """The emit step must be re-runnable: a duplicated name would give one commit two
        points in one series."""
        with tempfile.TemporaryDirectory() as d:
            raw = pathlib.Path(d) / "sweep.tsv"
            raw.write_text(SWEEP)
            out = pathlib.Path(d) / "benchmark_output.json"
            out.write_text("[]")
            for _ in range(2):
                with contextlib.redirect_stdout(io.StringIO()):
                    ssg.main(["emit", "--sweep-raw", str(raw), "--merge", str(out)])
            names = [m["name"] for m in json.loads(out.read_text())]
            self.assertEqual(len(names), len(set(names)))

    def test_an_absent_transcript_costs_only_this_series(self):
        """The sweep is one instrument among several on the pinned host. A transcript
        that failed to appear must not cost the commit its whole point."""
        with tempfile.TemporaryDirectory() as d:
            out = pathlib.Path(d) / "benchmark_output.json"
            out.write_text("[]")
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                code = ssg.main(["emit", "--sweep-raw", str(pathlib.Path(d) / "nope.tsv"),
                                 "--merge", str(out)])
            self.assertEqual(code, 0)
            self.assertIn("::notice::", buf.getvalue())


class GateVerdicts(unittest.TestCase):
    def test_a_clean_run_passes(self):
        code, out = run_gate(SWEEP, PINS)
        self.assertEqual(code, 0)
        self.assertIn("STORE_SWEEP_GATE\tPASS", out)

    def test_growth_warns_with_an_annotation_and_does_not_red_the_job(self):
        pins = json.loads(json.dumps(PINS))
        pins["pins"][0]["value"] = 296  # doctored baseline: 376 measured vs 296 pinned
        code, out = run_gate(SWEEP, pins)
        self.assertEqual(code, 0, "warn mode must not red the job")
        self.assertIn("::warning::ADR-0079 store occupancy drift", out)
        self.assertIn("STORE_SWEEP_GATE\tWARN", out)
        self.assertIn("grew 80", out)

    def test_the_same_growth_fails_once_the_ratchet_is_activated(self):
        pins = json.loads(json.dumps(PINS))
        pins["pins"][0]["value"] = 296
        code, out = run_gate(SWEEP, pins, "--mode", "fail")
        self.assertEqual(code, 1)
        self.assertIn("STORE_SWEEP_GATE\tFAIL", out)

    def test_one_byte_of_growth_is_growth(self):
        """The band is zero on purpose: the column was measured byte-identical across
        eight invocations spanning an 8x range of host load, so a tolerance would only
        make the first N bytes of the next growth free."""
        pins = json.loads(json.dumps(PINS))
        pins["pins"][0]["value"] = 375
        code, _out = run_gate(SWEEP, pins, "--mode", "fail")
        self.assertEqual(code, 1)

    def test_a_changed_store_count_is_caught_even_at_an_unchanged_total(self):
        """Topology is what this sweep varies. A store that disappears while the total
        holds is exactly the change a bytes-only pin would wave through."""
        pins = json.loads(json.dumps(PINS))
        pins["pins"][3]["value"] = 4
        code, out = run_gate(SWEEP, pins, "--mode", "fail")
        self.assertEqual(code, 1)
        self.assertIn("NARROW/T1/stores", out)

    def test_a_shrink_asks_for_a_re_pin(self):
        """A pin left above the truth is how drift hides: the next growth is free."""
        pins = json.loads(json.dumps(PINS))
        pins["pins"][0]["value"] = 999
        _code, out = run_gate(SWEEP, pins)
        self.assertIn("RE-PIN to 376", out)


class ToolchainAttribution(unittest.TestCase):
    """The pinned figures contain the `sizeof` of the std::pmr control blocks, so they are
    compiler-bound; a difference measured across toolchains is not attributable to code."""

    def test_a_mismatched_toolchain_downgrades_to_warn_but_still_reports(self):
        pins = json.loads(json.dumps(PINS))
        pins["toolchain"] = "some-compiler-from-another-decade"
        pins["pins"][0]["value"] = 296
        code, out = run_gate(SWEEP, pins, "--mode", "fail")
        self.assertEqual(code, 0)
        self.assertIn("::warning::", out)
        self.assertIn("::notice::", out)


class InstrumentFailuresAreHardInBothModes(unittest.TestCase):
    """A failure to MEASURE is not a budget verdict (#982's precedent). Warn mode answers
    for the BUDGET only — it must never answer for a dead instrument."""

    def test_no_transcript_fails_in_warn_mode(self):
        with tempfile.TemporaryDirectory() as d:
            pj = pathlib.Path(d) / "pins.json"
            pj.write_text(json.dumps(PINS))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                code = ssg.main(["gate", "--sweep-raw", str(pathlib.Path(d) / "nope.tsv"),
                                 "--pins", str(pj)])
            self.assertEqual(code, 1)
            self.assertIn("no-measurement", buf.getvalue())

    def test_a_failed_calibration_fails_the_gate(self):
        bad = SWEEP.replace("CALIBRATE\ttotal_failures\t0\t-",
                            "CALIBRATE\ttotal_failures\t2\t-", 1)
        code, out = run_gate(bad, PINS)
        self.assertEqual(code, 1)
        self.assertIn("calibration", out)

    def test_a_half_calibrated_run_fails(self):
        """`run_store_sweep.sh` breaks the instrument's line before AND after the
        transcript: an instrument that was live at the start and inert by the end would
        otherwise sign off on every cell in between."""
        once = SWEEP.replace("# loadavg after: 4.45 13.51 17.23 4/4511 3424687\n"
                             "CALIBRATE\ttotal_failures\t0\t-\n",
                             "# loadavg after: 4.45 13.51 17.23 4/4511 3424687\n")
        code, out = run_gate(once, PINS)
        self.assertEqual(code, 1)
        self.assertIn("unverified", out)

    def test_an_overflowing_store_fails(self):
        """An overflow means a draw fell back off the slab, so the occupancy below is a
        floor rather than the workload's cost."""
        bad = SWEEP.replace("RESULT_STORE_HWM\tWIDE\t1\twide\t0\t376\t40960\t3\t0",
                            "RESULT_STORE_HWM\tWIDE\t1\twide\t0\t376\t40960\t3\t7")
        code, out = run_gate(bad, PINS)
        self.assertEqual(code, 1)
        self.assertIn("store-fault", out)

    def test_a_refusing_channel_fails(self):
        bad = SWEEP.replace("RESULT_STORE_CHAN\tWIDE\t1\tgraph-ctl\t65\t2080\t376\t0",
                            "RESULT_STORE_CHAN\tWIDE\t1\tgraph-ctl\t65\t2080\t376\t3")
        code, out = run_gate(bad, PINS)
        self.assertEqual(code, 1)
        self.assertIn("store-fault", out)

    def test_a_pin_the_run_does_not_produce_fails(self):
        """A gate that shrugged at a missing pin would report PASS while measuring less
        and less — the sweep's arms and thread counts are both constants that an edit
        could quietly drop."""
        pins = json.loads(json.dumps(PINS))
        pins["pins"].append({"arm": "MID", "threads": 24, "metric": "used_total",
                             "value": 2933})
        code, out = run_gate(SWEEP, pins)
        self.assertEqual(code, 1)
        self.assertIn("pin-not-measured", out)


class ShippedPins(unittest.TestCase):
    """The pin file that actually ships is checked here, not just a fixture."""

    def setUp(self):
        self.pins = json.loads(
            (pathlib.Path(ssg.HERE) / "store_sweep_pins.json").read_text())

    def test_only_the_deterministic_columns_are_pinned(self):
        for p in self.pins["pins"]:
            self.assertIn(p["metric"], ssg.GATEABLE,
                          "only the deterministic hwm columns may be pinned — the T-sweep "
                          "and the escape high-water are excluded by measurement")

    def test_the_store_less_control_arm_is_not_pinned(self):
        """H-baseline injects no store, so it has no occupancy. A pin on it would be a
        dead pin the gate fails on every run."""
        self.assertNotIn(ssg.kNoStoreArm, {p["arm"] for p in self.pins["pins"]})

    def test_the_activation_criterion_is_recorded_next_to_the_pins(self):
        """Staging that lives only in a comment rots. The criterion for flipping to
        --mode fail must be readable from the file the flip would govern."""
        self.assertIn("activation", self.pins)
        self.assertGreater(self.pins["activation"]["samples"], 0)

    def test_the_stage_2_step_is_pre_declared(self):
        """#843 moves vertex placement onto the injected store, so every used_total pin
        will step. Pre-declaring it is what stops the series flagging the fix as the
        regression."""
        self.assertIn("843", self.pins["activation"]["stage2_step"])

    def test_the_exclusions_are_recorded_beside_the_pins(self):
        for family in ssg.EXCLUDED:
            self.assertIn(family, self.pins["_never_gated"])

    def test_the_band_is_at_least_the_measured_spread(self):
        self.assertGreaterEqual(self.pins["band_bytes"], self.pins["measured_spread_bytes"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
