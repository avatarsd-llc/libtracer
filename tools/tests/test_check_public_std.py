#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the public-std ratchet (`tools/check_public_std.py`, #1781).

* code is counted, prose is not: comments, string literals, raw strings and digit
  separators never produce or hide a hit;
* views and non-owning std types are not counted;
* the ratchet fails a rise, a new header or type, and a stale (dropped) pin;
* ``repin`` only lowers and removes pins, never adds or raises one.

Run with ``python3 -m unittest discover -s tools/tests -p "test_check_public_std.py"``.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_public_std as gate  # noqa: E402


def _types(text: str) -> list:
    return [t for _, t, _ in gate.scan_text(text)]


class ScanTest(unittest.TestCase):
    def test_code_is_counted(self):
        self.assertEqual(_types("std::vector<std::byte> f(std::string s);"), ["vector", "string"])
        self.assertEqual(_types("auto p = std::make_shared<int>(1);"), ["make_shared"])
        self.assertEqual(_types("std::pmr::vector<int> v;"), ["vector"])

    def test_prose_is_not(self):
        text = (
            "// std::vector in a line comment\n"
            "/* std::string in a block\n   comment */ int x;\n"
            'const char* s = "std::function";\n'
            'auto r = R"x(std::unique_ptr)x";\n'
            "char c = 'v';\n"
        )
        self.assertEqual(_types(text), [])

    def test_views_and_non_owning_are_not(self):
        text = ("std::string_view a; std::span<const std::byte> b; std::array<int, 2> c;\n"
                "std::optional<int> d; std::pmr::memory_resource* e; std::function_ref f;\n")
        self.assertEqual(_types(text), [])

    def test_digit_separator_does_not_swallow_the_line(self):
        self.assertEqual(_types("auto n = 1'000; std::vector<int> v;"), ["vector"])

    def test_line_numbers_survive_stripping(self):
        hits = gate.scan_text("/* a\n b\n */\nstd::deque<int> q;\n")
        self.assertEqual([(n, t) for n, t, _ in hits], [(4, "deque")])


PINS = {"core/include/libtracer/a.hpp": {"owner": "#1", "counts": {"vector": 2}}}


class RatchetTest(unittest.TestCase):
    def test_at_pin_passes(self):
        self.assertEqual(gate.check({"core/include/libtracer/a.hpp": {"vector": 2}}, PINS), [])

    def test_rise_fails(self):
        errors = gate.check({"core/include/libtracer/a.hpp": {"vector": 3}}, PINS)
        self.assertTrue(any("rose" in e for e in errors))

    def test_new_type_or_header_fails(self):
        errors = gate.check({"core/include/libtracer/a.hpp": {"vector": 2, "string": 1},
                             "core/include/libtracer/b.hpp": {"map": 1}}, PINS)
        self.assertEqual(sum("is new" in e for e in errors), 2)

    def test_drop_is_stale_until_repinned(self):
        measured = {"core/include/libtracer/a.hpp": {"vector": 1}}
        self.assertTrue(any("dropped" in e for e in gate.check(measured, PINS)))
        lowered = gate.repin(measured, PINS)
        self.assertEqual(lowered["core/include/libtracer/a.hpp"]["counts"], {"vector": 1})
        self.assertEqual(lowered["core/include/libtracer/a.hpp"]["owner"], "#1")
        self.assertEqual(gate.check(measured, lowered), [])

    def test_drop_to_zero_removes_the_pin(self):
        self.assertTrue(gate.check({}, PINS))
        self.assertEqual(gate.repin({}, PINS), {})

    def test_repin_never_adds_or_raises(self):
        measured = {"core/include/libtracer/a.hpp": {"vector": 5},
                    "core/include/libtracer/b.hpp": {"map": 1}}
        self.assertEqual(gate.repin(measured, PINS), PINS)


if __name__ == "__main__":
    unittest.main()
