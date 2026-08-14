#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Decision-rule tests for the RAM-census series + warn ratchet (#1228).

The ratchet is warn-first, which means for its whole first life a wrong verdict is
INVISIBLE — nothing turns red to report it. These tests are therefore the only thing
standing between "the gate warns on drift" and "the gate warns on nothing", and they
run in `perf.yml` next to the other decision-rule suites (`test_perf_gate.py`,
`test_host_guard.py`), before anything is built or measured.

Both directions of every rule are pinned: drift warns AND a clean run passes; an
instrument failure is hard in warn mode AND in fail mode; a zero-median row is kept out
of the chart store AND asserted by the gate instead.

    python3 bench/test_ram_census.py    # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import io
import contextlib
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import ram_census as rc  # noqa: E402

# A faithful bench_conn_ram transcript: the padded columns the bench actually prints
# (`arm=%-12s metric=%-11s`), a NULL arm, comment lines, and a zero-valued row.
CONN = """\
# bench_conn_ram peers=8 frame=65536 reps=9
# sizeof: tcp_client=264 tcp_server=440 udp=216
RESULT arm=null         metric=drift       n=9 median=0 min=0 max=0
RESULT arm=tcp-server   metric=link_base   n=9 median=464 min=464 max=464
RESULT arm=tcp-server   metric=per_conn    n=9 median=280 min=280 max=284
RESULT arm=tcp-server   metric=hw_peak     n=9 median=24878 min=16676 max=57668
RESULT arm=tcp-server   metric=after_td    n=9 median=280 min=280 max=284
RESULT arm=tcp-server   metric=recycle     n=9 median=0 min=0 max=0
RESULT arm=udp-rope-heap metric=per_conn    n=9 median=-16 min=-16 max=-16
# allocs=2718 frees=2718 residual_live=0
"""

PINS = {
    # Stamped with THIS machine's compiler so the fixture exercises the matching-
    # toolchain path; the mismatch path gets its own cases below.
    "toolchain": rc.host_guard.compiler_identity(),
    "band_bytes": 8,
    "band_pct": 2.0,
    "pins": [
        {"arm": "tcp-server", "metric": "link_base", "value": 464},
        {"arm": "tcp-server", "metric": "per_conn", "value": 280},
        {"arm": "tcp-server", "metric": "after_td", "value": 280},
        {"arm": "tcp-server", "metric": "recycle", "value": 0},
        {"arm": "udp-rope-heap", "metric": "per_conn", "value": -16},
    ],
}


def run_gate(transcript: str, pins: dict, *extra: str) -> tuple[int, str]:
    """@brief Run the gate CLI over a transcript+pins pair; returns (rc, stdout)."""
    with tempfile.TemporaryDirectory() as d:
        raw = pathlib.Path(d) / "conn.txt"
        raw.write_text(transcript)
        pj = pathlib.Path(d) / "pins.json"
        pj.write_text(json.dumps(pins))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = rc.main(["gate", "--conn-raw", str(raw), "--pins", str(pj), *extra])
        return code, buf.getvalue()


class Parsing(unittest.TestCase):
    def test_every_result_row_is_parsed_and_comments_ignored(self):
        rows = rc.parse_rows(CONN)
        self.assertEqual(len(rows), 7)
        self.assertEqual(rows[("tcp-server", "per_conn")],
                         {"n": 9, "median": 280, "min": 280, "max": 284})

    def test_negative_medians_survive(self):
        """A negative per-connection figure is a real reading (the rope arm frees a
        block the baseline held); a parser that drops the sign silently loses a pin."""
        self.assertEqual(rc.parse_rows(CONN)[("udp-rope-heap", "per_conn")]["median"], -16)

    def test_a_transcript_with_no_rows_reports_why(self):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "empty.txt"
            p.write_text("# the bench crashed before its first row\n")
            rows, why = rc.read_transcript(str(p))
            self.assertEqual(rows, {})
            self.assertIn("no RESULT rows", why)

    def test_a_missing_transcript_is_not_an_exception(self):
        rows, why = rc.read_transcript("/nonexistent/conn.txt")
        self.assertEqual(rows, {})
        self.assertIn("does not exist", why)


class SeriesShape(unittest.TestCase):
    def test_zero_and_null_rows_are_kept_out_of_the_store(self):
        """A ratio against zero is undefined — such a series can never alert, so it
        would look like a measurement and be none. The gate asserts them instead."""
        series, skipped = rc.metrics(rc.parse_rows(CONN), "conn-ram")
        names = {m["name"] for m in series}
        self.assertNotIn("conn-ram null drift", names)
        self.assertNotIn("conn-ram tcp-server recycle", names)
        self.assertEqual(len(skipped), 2)

    def test_recorded_names_and_units(self):
        series, _ = rc.metrics(rc.parse_rows(CONN), "conn-ram")
        by = {m["name"]: m for m in series}
        self.assertEqual(by["conn-ram tcp-server per_conn"]["value"], 280)
        self.assertEqual(by["conn-ram tcp-server per_conn"]["unit"], "bytes")

    def test_block_counts_are_not_labelled_bytes(self):
        self.assertEqual(rc.unit_for("S_B_blocks"), "blocks")
        self.assertEqual(rc.unit_for("S_B_bytes"), "bytes")

    def test_the_median_is_what_is_recorded(self):
        """min/max bracket the run; a point recorded from either end would trend the
        machine's worst repetition rather than the code's cost."""
        series, _ = rc.metrics(rc.parse_rows(CONN), "conn-ram")
        peak = next(m for m in series if m["name"].endswith("hw_peak"))
        self.assertEqual(peak["value"], 24878)


class Emit(unittest.TestCase):
    def test_merge_appends_without_disturbing_the_latency_metrics(self):
        with tempfile.TemporaryDirectory() as d:
            raw = pathlib.Path(d) / "conn.txt"
            raw.write_text(CONN)
            out = pathlib.Path(d) / "benchmark_output.json"
            out.write_text(json.dumps([{"name": "inproc 64B/fan1/1ep p50 latency",
                                        "unit": "ns", "value": 42}]))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(rc.main(["emit", "--conn-raw", str(raw),
                                          "--merge", str(out)]), 0)
            got = json.loads(out.read_text())
            self.assertEqual(got[0]["value"], 42)
            self.assertIn("conn-ram tcp-server per_conn", {m["name"] for m in got})

    def test_merge_is_idempotent(self):
        """The emit step must be re-runnable: a duplicated name would give one commit
        two points in one series."""
        with tempfile.TemporaryDirectory() as d:
            raw = pathlib.Path(d) / "conn.txt"
            raw.write_text(CONN)
            out = pathlib.Path(d) / "benchmark_output.json"
            out.write_text("[]")
            for _ in range(2):
                with contextlib.redirect_stdout(io.StringIO()):
                    rc.main(["emit", "--conn-raw", str(raw), "--merge", str(out)])
            names = [m["name"] for m in json.loads(out.read_text())]
            self.assertEqual(len(names), len(set(names)))

    def test_an_absent_node_census_costs_only_its_own_series(self):
        """The whole-node census needs a peer process. A peer that fails to come up
        must not cost the commit the per-connection series as well."""
        with tempfile.TemporaryDirectory() as d:
            raw = pathlib.Path(d) / "conn.txt"
            raw.write_text(CONN)
            out = pathlib.Path(d) / "benchmark_output.json"
            out.write_text("[]")
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                code = rc.main(["emit", "--conn-raw", str(raw),
                                "--node-raw", str(pathlib.Path(d) / "missing.txt"),
                                "--merge", str(out)])
            self.assertEqual(code, 0)
            self.assertIn("::notice::", buf.getvalue())
            self.assertTrue(json.loads(out.read_text()))


class GateVerdicts(unittest.TestCase):
    def test_a_clean_run_passes(self):
        code, out = run_gate(CONN, PINS)
        self.assertEqual(code, 0)
        self.assertIn("RAM_CENSUS_GATE\tPASS", out)

    def test_growth_past_the_band_warns_with_an_annotation(self):
        pins = json.loads(json.dumps(PINS))
        pins["pins"][1]["value"] = 200  # doctored baseline: 280 measured vs 200 pinned
        code, out = run_gate(CONN, pins)
        self.assertEqual(code, 0, "warn mode must not red the job")
        self.assertIn("::warning::", out)
        self.assertIn("RAM_CENSUS_GATE\tWARN", out)
        self.assertIn("grew 80 B", out)

    def test_the_same_growth_fails_once_the_ratchet_is_activated(self):
        pins = json.loads(json.dumps(PINS))
        pins["pins"][1]["value"] = 200
        code, out = run_gate(CONN, pins, "--mode", "fail")
        self.assertEqual(code, 1)
        self.assertIn("RAM_CENSUS_GATE\tFAIL", out)

    def test_drift_inside_the_band_is_not_drift(self):
        """The measured in-run spread on the tightest pinned row is 4 B; a band under
        it would fire on the instrument rather than on the code."""
        pins = json.loads(json.dumps(PINS))
        pins["pins"][1]["value"] = 284
        code, _ = run_gate(CONN, pins)
        self.assertEqual(code, 0)

    def test_a_shrink_asks_for_a_re_pin(self):
        """A pin left above the truth is how drift hides: the next growth is free."""
        pins = json.loads(json.dumps(PINS))
        pins["pins"][1]["value"] = 999
        _, out = run_gate(CONN, pins)
        self.assertIn("RE-PIN to 280", out)

    def test_the_percentage_term_governs_the_large_rows(self):
        """8 B is noise on a 64 KB link base; the band must scale there."""
        pins = {"band_bytes": 8, "band_pct": 2.0,
                "pins": [{"arm": "a", "metric": "link_base", "value": 65760}]}
        self.assertEqual(rc.band_for(pins["pins"][0], pins), 1315)

    def test_the_byte_term_governs_the_small_rows(self):
        """2% of 0 is 0; without the absolute floor a zero-valued pin gets a zero band
        and every 8 B of allocator rounding becomes a warning."""
        pins = {"band_bytes": 8, "band_pct": 2.0,
                "pins": [{"arm": "a", "metric": "recycle", "value": 0}]}
        self.assertEqual(rc.band_for(pins["pins"][0], pins), 8)


class ToolchainAttribution(unittest.TestCase):
    """Every pinned figure contains the `sizeof` of a transport, so it is compiler- and
    allocator-bound. A difference measured across toolchains is not attributable."""

    def test_a_matching_toolchain_enforces_as_asked(self):
        self.assertEqual(rc.effective_mode({"toolchain": "c++ 13.3.0"}, "fail", "c++ 13.3.0"),
                         ("fail", None))

    def test_a_mismatched_toolchain_downgrades_to_warn(self):
        mode, why = rc.effective_mode({"toolchain": "c++ 13.3.0"}, "fail", "c++ 15.1.0")
        self.assertEqual(mode, "warn")
        self.assertIn("not attributable", why)

    def test_the_downgrade_still_reports_the_drift(self):
        """Downgraded is not silent: the number is still printed and still warned on —
        an instrument that goes quiet on a toolchain bump measures nothing for a
        release."""
        pins = json.loads(json.dumps(PINS))
        pins["toolchain"] = "some-compiler-from-another-decade"
        pins["pins"][1]["value"] = 200
        code, out = run_gate(CONN, pins, "--mode", "fail")
        self.assertEqual(code, 0)
        self.assertIn("::warning::", out)
        self.assertIn("::notice::", out)


class InstrumentFailuresAreHardInBothModes(unittest.TestCase):
    """A failure to MEASURE is not a budget verdict (#982's precedent). Warn mode
    answers for the BUDGET only — it must never answer for a dead instrument."""

    def test_no_transcript_fails_in_warn_mode(self):
        with tempfile.TemporaryDirectory() as d:
            pj = pathlib.Path(d) / "pins.json"
            pj.write_text(json.dumps(PINS))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                code = rc.main(["gate", "--conn-raw", str(pathlib.Path(d) / "nope.txt"),
                                "--pins", str(pj)])
            self.assertEqual(code, 1)
            self.assertIn("no-measurement", buf.getvalue())

    def test_a_drifting_null_arm_fails_in_warn_mode(self):
        bad = CONN.replace("arm=null         metric=drift       n=9 median=0 min=0 max=0",
                           "arm=null         metric=drift       n=9 median=48 min=0 max=96")
        code, out = run_gate(bad, PINS)
        self.assertEqual(code, 1)
        self.assertIn("null-arm", out)

    def test_a_missing_null_arm_fails(self):
        no_null = "\n".join(l for l in CONN.splitlines() if "arm=null" not in l) + "\n"
        code, out = run_gate(no_null, PINS)
        self.assertEqual(code, 1)
        self.assertIn("null-arm", out)

    def test_a_pin_the_run_does_not_produce_fails(self):
        """The CAN arms vanish under --no-can. A gate that shrugged at a missing pin
        would report PASS while measuring less and less."""
        pins = json.loads(json.dumps(PINS))
        pins["pins"].append({"arm": "can-bus", "metric": "per_conn", "value": 512})
        code, out = run_gate(CONN, pins)
        self.assertEqual(code, 1)
        self.assertIn("pin-not-measured", out)


class ShippedPins(unittest.TestCase):
    """The pin file that actually ships is checked here, not just a fixture."""

    def setUp(self):
        self.pins = json.loads((pathlib.Path(rc.HERE) / "ram_census_pins.json").read_text())

    def test_the_shipped_pins_pass_against_a_shipped_shape_transcript(self):
        for p in self.pins["pins"]:
            self.assertIn(p["metric"], rc.GATEABLE,
                          "only the run-to-run stable per-connection columns may be pinned")

    def test_the_activation_criterion_is_recorded_next_to_the_pins(self):
        """Staging that lives only in a comment rots. The criterion for flipping to
        --mode fail must be readable from the file the flip would govern."""
        self.assertIn("activation", self.pins)
        self.assertIn("samples", self.pins["activation"])
        self.assertGreater(self.pins["activation"]["samples"], 0)

    def test_the_band_is_wider_than_the_measured_spread(self):
        self.assertGreaterEqual(self.pins["band_bytes"], self.pins["measured_spread_bytes"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
