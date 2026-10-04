#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""@brief Source-selection tests for the performance page's trend charts (#1769).

bench-local is the trend instrument: one pinned CPU, both arms of each paired point from
the same pass. These tests pin the three things that make it the page's default: the
selector opens on it (with hosted one click away, labelled as the portability envelope),
the zenoh / libtracer ratio is built from same-pass arms only, and rows above 1 KiB reach
the default payload.

    python3 bench/test_render_history.py    # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import json
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import render_history as rh  # noqa: E402

JS = pathlib.Path(__file__).resolve().parent.parent / "docs" / "_static" / "perf_history.js"

SIZES = (64, 1024, 4096, 16384)


def _store() -> dict:
    """@brief A two-commit store carrying the paired zenoh/libtracer payload family.

    Every size above 1 KiB is present on both arms so a test can tell "dropped by the
    renderer" from "never recorded".
    """
    benches = []
    for size in SIZES:
        for eng in ("", "zenoh "):
            benches.append({"name": f"{eng}inproc {size}B/fan1/1ep p50 latency",
                            "value": 100.0 + size / 64, "unit": "ns", "extra": "h"})
    return {"entries": {"libtracer latency (ns, smaller is better)": [
        {"commit": {"id": c * 40, "message": f"commit {c}"}, "benches": benches}
        for c in ("a", "b")]}}


class _NoGit(unittest.TestCase):
    """@brief Stubs the git-backed annotations so the tests depend on the store alone."""

    def setUp(self):
        self._saved = (rh.instrument_annotations, rh.release_annotations)
        rh.instrument_annotations = lambda *a, **k: []
        rh.release_annotations = lambda *a, **k: []

    def tearDown(self):
        rh.instrument_annotations, rh.release_annotations = self._saved


class RatioIsSamePassOnly(_NoGit):
    """@brief The quotient is offered on a same-pass store and never on the hosted one."""

    def _chart(self, payload: dict, cid: str) -> dict:
        return next(c for c in payload["charts"] if c["id"] == cid)

    def test_same_pass_store_carries_the_ratio(self):
        out = rh.build(_store(), {}, same_pass=True)
        self.assertIn("ratio", self._chart(out, "vs-zenoh-payload"))

    def test_hosted_store_carries_no_ratio(self):
        out = rh.build(_store(), {}, same_pass=False)
        chart = self._chart(out, "vs-zenoh-payload")
        self.assertNotIn("ratio", chart)
        # The absolute lines themselves are still drawn; only the quotient is withheld.
        self.assertEqual(len(chart["series"]), 2 * len(SIZES))

    def test_html_blocks_build_hosted_without_ratio(self):
        blocks = rh.html_blocks(_store(), _store())
        block = blocks["dispatch"]
        hosted = json.loads(re.search(r'class="ph-data">(.*?)</script>', block).group(1))
        local = json.loads(re.search(r'class="ph-data-local">(.*?)</script>', block).group(1))
        self.assertFalse(any("ratio" in c for c in hosted["charts"]))
        self.assertTrue(any("ratio" in c for c in local["charts"]))


class BenchLocalIsTheDefault(_NoGit):
    """@brief The selector opens on bench-local; hosted is one click away and labelled."""

    def _buttons(self, block: str) -> list[tuple[str, str, str]]:
        return re.findall(r'<button type="button" class="ph-srcbtn( on)?" data-src="(\w+)"'
                          r'[^>]*>([^<]*)</button>', block)

    def test_local_button_is_first_and_selected(self):
        block = rh.html_blocks(_store(), _store())["dispatch"]
        btns = self._buttons(block)
        self.assertEqual([(b[0], b[1]) for b in btns], [(" on", "local"), ("", "hosted")])
        self.assertIn("portability envelope", btns[1][2])

    def test_hosted_is_selected_when_local_is_unreachable(self):
        block = rh.html_blocks(_store(), None)["dispatch"]
        self.assertIn('data-src="local" disabled', block)
        self.assertEqual([(b[0], b[1]) for b in self._buttons(block)],
                         [("", "local"), (" on", "hosted")])

    def test_client_default_is_local(self):
        js = JS.read_text()
        body = re.search(r"function readSource\(\) \{(.*?)\n  \}", js, re.S).group(1)
        self.assertIn('=== "hosted" ? "hosted" : "local"', body)
        self.assertIn('catch (e) { return "local"; }', body)


class RowsAboveOneKibAreVisible(_NoGit):
    """@brief Payload sizes above 1 KiB reach the default (bench-local) payload."""

    def test_every_size_survives(self):
        out = rh.build(_store(), {}, same_pass=True)
        chart = next(c for c in out["charts"] if c["id"] == "vs-zenoh-payload")
        pvs = sorted({s["rpv"] for s in chart["series"]})
        self.assertEqual(pvs, [float(s) for s in SIZES])


if __name__ == "__main__":
    unittest.main()
