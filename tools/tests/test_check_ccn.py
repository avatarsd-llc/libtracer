#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the cyclomatic-complexity ratchet (`tools/check_ccn.py`, #1790).

The arms run over synthetic measurements, so they need no lizard install:

* everything at or under its pin and the ceiling passes;
* a new function over the ceiling fails;
* a pinned function rising fails;
* a pinned function dropping is stale until ``repin`` lowers it, then passes;
* the agent-doc cap passes at 100 lines and fails at 101.

Run with ``python3 -m unittest discover -s tools/tests``.
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_ccn as gate  # noqa: E402

PINS = {"tr::big": {"file": "core/src/a.cpp", "ccn": [40]}}


def _fns(big: int, extra: int = 3) -> list:
    """@brief A measured tree: the pinned function at `big`, another at `extra`."""
    return [("core/src/a.cpp", "tr::big", big, 1), ("core/src/b.cpp", "tr::small", extra, 1)]


class CcnRatchetTest(unittest.TestCase):
    """@brief Ceiling, rise, drop and re-pin."""

    def test_pass(self):
        self.assertEqual(gate.check(_fns(40), PINS), ([], []))

    def test_new_function_over_ceiling_fails(self):
        failures, _ = gate.check(_fns(40, gate.CEILING + 1), PINS)
        self.assertEqual(len(failures), 1)
        self.assertIn("tr::small", failures[0])

    def test_at_ceiling_passes(self):
        self.assertEqual(gate.check(_fns(40, gate.CEILING), PINS), ([], []))

    def test_pinned_rise_fails(self):
        failures, _ = gate.check(_fns(41), PINS)
        self.assertEqual(failures, ["tr::big: CCN 41, pinned at 40"])

    def test_pinned_drop_is_stale_then_repins(self):
        failures, stale = gate.check(_fns(30), PINS)
        self.assertEqual(failures, [])
        self.assertEqual(len(stale), 1)
        pins = gate.repin(_fns(30), PINS)
        self.assertEqual(pins["tr::big"]["ccn"], [30])
        self.assertEqual(gate.check(_fns(30), pins), ([], []))

    def test_drop_under_ceiling_removes_pin(self):
        self.assertEqual(gate.repin(_fns(12), PINS), {})

    def test_repin_never_raises_or_adds(self):
        pins = gate.repin(_fns(45, 20), PINS)
        self.assertEqual(pins, PINS)

    def test_new_overload_over_ceiling_fails(self):
        fns = _fns(40) + [("core/src/c.cpp", "tr::big", 20, 9)]
        failures, _ = gate.check(fns, PINS)
        self.assertEqual(failures, [f"tr::big: CCN 20, ceiling is {gate.CEILING}"])

    def test_split_moves_no_file_excess(self):
        whole = [("f.cpp", "g", 30, 1)]
        split = [("f.cpp", "g", 15, 1), ("f.cpp", "h", 15, 9)]
        merged = [("f.cpp", "g", 20, 1)]
        self.assertEqual(gate.file_excess(whole), {"f.cpp": 20})
        self.assertEqual(gate.file_excess(split), {"f.cpp": 10})
        self.assertEqual(gate.file_excess(merged), {"f.cpp": 10})

    def test_report_has_distribution(self):
        text = gate.report(_fns(40), _fns(41))
        self.assertIn("| 2 | 3 | 21.5 | 40 | 40 | 40 | 1 | 1 |", text)
        self.assertIn("| `core/src/a.cpp` | 31 | 30 | -1 |", text)


class AgentDocCapTest(unittest.TestCase):
    """@brief CLAUDE.md and CONTEXT.md stay at AGENT_DOC_MAX_LINES or fewer."""

    def _root(self, lines: int) -> Path:
        root = Path(tempfile.mkdtemp())
        for doc in gate.AGENT_DOCS:
            (root / doc).write_text("x\n" * lines)
        return root

    def test_at_limit_passes(self):
        self.assertEqual(gate.check_agent_docs(self._root(gate.AGENT_DOC_MAX_LINES)), [])

    def test_over_limit_fails(self):
        failures = gate.check_agent_docs(self._root(gate.AGENT_DOC_MAX_LINES + 1))
        self.assertEqual(len(failures), len(gate.AGENT_DOCS))

    def test_limit_is_100(self):
        self.assertEqual(gate.AGENT_DOC_MAX_LINES, 100)


if __name__ == "__main__":
    unittest.main()
