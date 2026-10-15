#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Decision-rule tests for the counted-metrics ledger (#2137).

Both directions of every rule are pinned: a raise fails AND a clean run passes; a fall
fails with a re-pin AND a fall inside the band passes; a missing row fails in warn mode
too; a pin that rises in a PR fails AND one with a new waiver passes; a moved pin with an
unchanged `moved_by` fails. They run in `counted-ledger.yml` before anything is built.

    python3 bench/test_counted_ledger.py
"""
from __future__ import annotations

import contextlib
import copy
import io
import json
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import counted_ledger as cl  # noqa: E402

TOOLCHAIN = "c++ 13.3.0"

COPIES = {
    "family": "copies", "title": "copies", "unit": "bytes", "per_payload": True,
    "instrument": "bench_peer_counts", "toolchain": TOOLCHAIN, "band_abs": 0, "band_pct": 0,
    "rows": [
        {"id": "ws @1024", "target": "WIDE", "source": "measured", "arm": "ws-relay-srv",
         "metric": "copy_bytes@1024", "payload": 1024, "value": 3150, "goal": 1024, "moved_by": "#1"},
        {"id": "httpd rx @1024", "target": "MID", "source": "static", "payload": 1024, "value": 2048,
         "goal": 1024, "moved_by": "#1", "basis": "read from the code"},
    ],
}
WAKE = {
    "family": "idle_wakeups", "title": "wakeups", "unit": "wakeups/s", "per_payload": False,
    "instrument": "bench_peer_counts", "toolchain": TOOLCHAIN, "band_abs": 5, "band_pct": 10,
    "rows": [{"id": "tcp-idle-32", "target": "WIDE", "source": "measured", "arm": "tcp-idle-32",
              "metric": "wakeups_per_s", "value": 330, "goal": 0, "moved_by": "#1"}],
}
RAM_FAM = {
    "family": "ram_per_peer", "title": "ram", "unit": "bytes", "per_payload": False,
    "instrument": "bench_conn_ram", "band_abs": 0, "band_pct": 0,
    "rows": [{"id": "tcp per peer", "target": "WIDE", "source": "ram_census", "arm": "tcp-server",
              "metric": "per_conn", "goal": 160, "moved_by": "#2134"}],
}
RAM = {"band_bytes": 8, "band_pct": 0.5, "activation": {"mode": "warn"},
       "pins": [{"arm": "tcp-server", "metric": "per_conn", "value": 280}]}


def ledger(*fams: dict, ram: dict = RAM) -> list[dict]:
    """@brief A loaded ledger built from in-memory family dicts."""
    return cl.load_texts({f["family"]: json.dumps(f) for f in fams}, json.dumps(ram))


def peer(**values: float) -> dict:
    """@brief A parsed transcript: arm__metric=value (dots and @ spelled _ and AT)."""
    out = {}
    for k, v in values.items():
        arm, metric = k.split("__")
        out[(arm.replace("_", "-"), metric.replace("AT", "@"))] = {"n": 3, "median": v, "min": v, "max": v}
    return out


def run_gate(led, peer_rows, conn_rows=None, toolchain=TOOLCHAIN, mode="fail"):
    with contextlib.redirect_stdout(io.StringIO()):
        return cl.gate_rows(led, peer_rows, conn_rows or {}, RAM, toolchain, mode)


class transcript_tests(unittest.TestCase):
    def test_parses_floats_and_padded_columns(self):
        rows = cl.parse_rows("RESULT arm=ws-relay-srv metric=vcsw@64 n=3 median=0.97 min=0.94 max=1.00\n"
                             "RESULT arm=tcp-server   metric=per_conn    n=3 median=280 min=280 max=280\n"
                             "# comment\n")
        self.assertEqual(rows[("ws-relay-srv", "vcsw@64")]["median"], 0.97)
        self.assertEqual(rows[("tcp-server", "per_conn")]["max"], 280.0)


class gate_tests(unittest.TestCase):
    def test_exact_count_passes(self):
        _, fails, warns = run_gate(ledger(COPIES), peer(ws_relay_srv__copy_bytesAT1024=3150))
        self.assertEqual((fails, warns), ([], []))

    def test_one_byte_more_fails(self):
        _, fails, _ = run_gate(ledger(COPIES), peer(ws_relay_srv__copy_bytesAT1024=3151))
        self.assertEqual(len(fails), 1)
        self.assertIn("rose", fails[0])

    def test_a_fall_fails_with_a_repin(self):
        _, fails, _ = run_gate(ledger(COPIES), peer(ws_relay_srv__copy_bytesAT1024=1024))
        self.assertEqual(len(fails), 1)
        self.assertIn("RE-PIN", fails[0])

    def test_inside_the_band_passes(self):
        _, fails, _ = run_gate(ledger(WAKE), peer(tcp_idle_32__wakeups_per_s=360))
        self.assertEqual(fails, [])
        _, fails, _ = run_gate(ledger(WAKE), peer(tcp_idle_32__wakeups_per_s=364))
        self.assertEqual(len(fails), 1)

    def test_static_rows_are_not_measured(self):
        _, fails, _ = run_gate(ledger(COPIES), peer(ws_relay_srv__copy_bytesAT1024=3150))
        self.assertFalse(any("httpd" in f for f in fails))

    def test_missing_row_fails_even_in_warn_mode(self):
        _, fails, _ = run_gate(ledger(COPIES), peer(other__x=1), mode="warn")
        self.assertEqual(len(fails), 1)
        self.assertIn("absent", fails[0])

    def test_warn_mode_downgrades_drift(self):
        _, fails, warns = run_gate(ledger(COPIES), peer(ws_relay_srv__copy_bytesAT1024=4000), mode="warn")
        self.assertEqual(fails, [])
        self.assertEqual(len(warns), 1)

    def test_other_toolchain_only_warns(self):
        _, fails, warns = run_gate(ledger(COPIES), peer(ws_relay_srv__copy_bytesAT1024=4000),
                                   toolchain="c++ 15.1.0")
        self.assertEqual(fails, [])
        self.assertEqual(len(warns), 1)

    def test_ram_rows_read_the_ram_pins_and_their_mode(self):
        led = ledger(RAM_FAM)
        self.assertEqual(led[0]["rows"][0]["value"], 280)
        _, fails, warns = run_gate(led, peer(x__y=1), peer(tcp_server__per_conn=300))
        self.assertEqual(fails, [])  # ram_census_pins.json is still in warn mode
        self.assertEqual(len(warns), 1)
        _, fails, warns = run_gate(led, peer(x__y=1), peer(tcp_server__per_conn=286))
        self.assertEqual((fails, warns), ([], []))  # inside ram_census's own 8 B band

    def test_ram_rows_without_a_transcript_are_skipped(self):
        table, fails, _ = run_gate(ledger(RAM_FAM), peer(x__y=1))
        self.assertEqual(fails, [])
        self.assertTrue(any("not measured" in t for t in table))


class diff_tests(unittest.TestCase):
    def moved(self, value, moved_by="#2", waiver=None):
        head = copy.deepcopy(COPIES)
        row = head["rows"][1]
        row["value"], row["moved_by"] = value, moved_by
        if waiver:
            row["waiver"] = waiver
        return cl.diff_rows(ledger(COPIES), ledger(head))

    def test_unchanged_passes(self):
        self.assertEqual(cl.diff_rows(ledger(COPIES), ledger(COPIES)), [])

    def test_a_lowered_pin_naming_its_change_passes(self):
        self.assertEqual(self.moved(1024), [])

    def test_a_moved_pin_must_name_its_change(self):
        problems = self.moved(1024, moved_by="#1")
        self.assertEqual(len(problems), 1)
        self.assertIn("moved_by", problems[0])

    def test_a_raised_static_count_fails(self):
        problems = self.moved(4096)
        self.assertEqual(len(problems), 1)
        self.assertIn("rose", problems[0])

    def test_a_raise_with_a_new_waiver_passes(self):
        self.assertEqual(self.moved(4096, waiver="#9999 accepted"), [])

    def test_a_removed_row_fails(self):
        head = copy.deepcopy(COPIES)
        head["rows"].pop()
        self.assertEqual(len(cl.diff_rows(ledger(COPIES), ledger(head))), 1)

    def test_a_raised_ram_pin_fails_without_a_moved_by(self):
        raised = copy.deepcopy(RAM)
        raised["pins"][0]["value"] = 300
        problems = cl.diff_rows(ledger(RAM_FAM), ledger(RAM_FAM, ram=raised))
        self.assertEqual(len(problems), 1)
        lowered = copy.deepcopy(RAM)
        lowered["pins"][0]["value"] = 200
        self.assertEqual(cl.diff_rows(ledger(RAM_FAM), ledger(RAM_FAM, ram=lowered)), [])


class schema_tests(unittest.TestCase):
    def test_static_row_needs_its_basis(self):
        bad = copy.deepcopy(COPIES)
        del bad["rows"][1]["basis"]
        with self.assertRaises(SystemExit):
            ledger(bad)

    def test_ram_row_may_not_carry_its_own_value(self):
        bad = copy.deepcopy(RAM_FAM)
        bad["rows"][0]["value"] = 1
        with self.assertRaises(SystemExit):
            ledger(bad)

    def test_unknown_family_is_refused(self):
        with self.assertRaises(SystemExit):
            cl.load_texts({"latency": json.dumps(COPIES)}, None)

    def test_the_checked_in_ledger_loads(self):
        led = cl.load_ledger()
        self.assertEqual([f["family"] for f in led], list(cl.FAMILIES))
        for fam in led:
            targets = {r["target"] for r in fam["rows"]}
            self.assertEqual(targets, {"WIDE", "MID"}, fam["family"])


class render_tests(unittest.TestCase):
    def test_copies_render_as_a_multiple_and_history_is_read(self):
        history = {"entries": {"s": [
            {"commit": {"id": "aaaaaaaa1111"}, "benches": [
                {"name": cl.series_name("copies", "ws-relay-srv", "copy_bytes@1024"), "value": 3266}]},
            {"commit": {"id": "bbbbbbbb2222"}, "benches": [
                {"name": cl.series_name("copies", "ws-relay-srv", "copy_bytes@1024"), "value": 3150}]},
        ]}}
        page = cl.render_markdown(ledger(COPIES, RAM_FAM), history)
        self.assertIn("3.08× (3150 B)", page)
        self.assertIn("`aaaaaaaa` → 3.08× (3150 B), 2 point(s)", page)
        self.assertIn("| tcp per peer | WIDE | measured (`ram_census_pins.json`) | 280 | 160 |", page)


if __name__ == "__main__":
    unittest.main(verbosity=2)
