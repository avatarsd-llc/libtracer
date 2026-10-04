#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Decision-rule tests for the symbol-size ratchet's verdict (#1849).

Nothing here builds or measures: `judge` is a function of one pinned row and its measured
size, so fixture rows are a complete test bed. The properties under test —

  * an exact pin still fails on any move, in either direction;
  * a declared band passes every size inside it, inclusive, and says `in band`;
  * a size above the band fails as growth and one below it fails as a shrink to re-pin;
  * the shipped pin file's bands are well formed and contain their own pin.

    python3 bench/test_symbol_ratchet.py   # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import json
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import symbol_ratchet as sr  # noqa: E402


def row(measured, band=None, pinned=1115):
    """@brief One measured pin row, as `measure` returns it."""
    r = {"binary": "b", "symbol": "s", "bytes": pinned, "measured": measured, "why": ""}
    if band is not None:
        r["band"] = band
    return r


class ExactPin(unittest.TestCase):
    def test_equal_passes(self):
        self.assertEqual(sr.judge(row(1115)), ("ok", None))

    def test_growth_fails(self):
        mark, failure = sr.judge(row(1116))
        self.assertEqual(mark, "GREW")
        self.assertIn("grew 1 B", failure)

    def test_shrink_fails_with_repin(self):
        mark, failure = sr.judge(row(1099))
        self.assertEqual(mark, "SHRANK")
        self.assertIn("RE-PIN to 1099", failure)

    def test_missing_fails(self):
        r = row(None)
        r["why"] = "symbol not found"
        self.assertEqual(sr.judge(r)[0], "MISSING")
        self.assertIn("symbol not found", sr.judge(r)[1])


class DeclaredBand(unittest.TestCase):
    BAND = [1099, 1115]

    def test_pin_itself_is_ok(self):
        self.assertEqual(sr.judge(row(1115, self.BAND)), ("ok", None))

    def test_in_band_passes(self):
        for size in (1099, 1107, 1114):
            mark, failure = sr.judge(row(size, self.BAND))
            self.assertIsNone(failure, size)
            self.assertEqual(mark, "in band [1099, 1115]")

    def test_above_band_fails_as_growth(self):
        mark, failure = sr.judge(row(1116, self.BAND))
        self.assertEqual(mark, "GREW")
        self.assertIn("grew 1 B (1115 -> 1116)", failure)

    def test_below_band_fails_as_shrink(self):
        mark, failure = sr.judge(row(1098, self.BAND))
        self.assertEqual(mark, "SHRANK")
        self.assertIn("RE-PIN to 1098", failure)


class ShippedPins(unittest.TestCase):
    def test_bands_are_well_formed(self):
        pins = json.loads((pathlib.Path(__file__).parent / "symbol_ratchet.json").read_text())
        banded = [p for p in pins["symbols"] if "band" in p]
        self.assertTrue(banded)
        for p in banded:
            lo, hi = p["band"]
            self.assertLessEqual(lo, p["bytes"], p["symbol"])
            self.assertLessEqual(p["bytes"], hi, p["symbol"])
            self.assertTrue(p.get("band_note"), f"{p['symbol']}: a band needs a band_note")


if __name__ == "__main__":
    unittest.main()
