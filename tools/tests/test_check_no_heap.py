#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the MCU no-heap link check (`tools/check_no_heap.py`, #1783).

The check reads ``objdump -r`` output, so these drive its parser and its ratchet with that
output verbatim, without a cross toolchain:

* every heap entry point is recognised, in every C++ variant, and nothing else is;
* references are counted per call site (object, section, entry point), and debug or unwind
  relocations are not call sites;
* a planted ``new`` fails even inside an object that already pins ``operator new``, whether
  it sits in a new function or is a second call in a pinned one; a pin that is no longer
  referenced, or referenced fewer times, is GONE, so the baseline can only shrink.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import os
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_no_heap as cnh  # noqa: E402

RELOCS = """\
In archive libtracer.a:

graph.cpp.obj:     file format elf32-littleriscv

RELOCATION RECORDS FOR [.text._ZN2tr5graph7graph_t9subscribeEv]:
OFFSET   TYPE              VALUE
00000010 R_RISCV_CALL_PLT  _Znwj
00000010 R_RISCV_RELAX     *ABS*
00000020 R_RISCV_CALL_PLT  memcpy
00000030 R_RISCV_CALL_PLT  _ZdlPvj


RELOCATION RECORDS FOR [.text._ZN2tr5graph8vertex_tD0Ev]:
OFFSET   TYPE              VALUE
00000004 R_RISCV_CALL_PLT  _ZdlPvj+0x0


RELOCATION RECORDS FOR [.debug_info]:
OFFSET   TYPE              VALUE
00000040 R_RISCV_32        _Znwj

twai_link.cpp.obj:     file format elf32-littleriscv

RELOCATION RECORDS FOR [.text._ZN2tr3esp11twai_link_t5startEv]:
OFFSET   TYPE              VALUE
00000008 R_RISCV_CALL_PLT  heap_caps_malloc
00000018 R_RISCV_CALL_PLT  free
"""

# A second, distinct operator new in an already-pinned function of graph.cpp.obj.
SECOND_CALL = RELOCS.replace(
    "00000020 R_RISCV_CALL_PLT  memcpy",
    "00000020 R_RISCV_CALL_PLT  _ZnwjRKSt9nothrow_t")

# A new function in graph.cpp.obj, an object that already pins operator new.
NEW_FUNCTION = RELOCS.replace(
    "RELOCATION RECORDS FOR [.debug_info]:",
    "RELOCATION RECORDS FOR [.text._Z14tr_planted_newv]:\n"
    "OFFSET   TYPE              VALUE\n"
    "00000002 R_RISCV_CALL_PLT  _Znwj\n\n\n"
    "RELOCATION RECORDS FOR [.debug_info]:")


class FamilyTest(unittest.TestCase):
    def test_heap_entry_points(self):
        for sym, fam in [("_Znwj", "operator new"), ("_Znam", "operator new[]"),
                         ("_ZdlPvm", "operator delete"), ("_ZdaPv", "operator delete[]"),
                         ("malloc", "malloc"), ("realloc", "realloc"), ("strdup", "strdup"),
                         ("_malloc_r", "_malloc_r"), ("_free_r", "_free_r"),
                         ("pvPortMalloc", "pvPortMalloc"), ("heap_caps_free", "heap_caps_free"),
                         ("heap_caps_aligned_alloc", "heap_caps_aligned_alloc")]:
            self.assertEqual(cnh.family(sym), fam, sym)

    def test_not_heap(self):
        for sym in ("memcpy", "_ZN2tr3mem13heap_source_t9try_allocEjj", "freeaddrinfo",
                    "heap_caps_get_free_size", "_ZNSt6thread15_M_start_threadE", "*ABS*"):
            self.assertIsNone(cnh.family(sym), sym)


class ParseTest(unittest.TestCase):
    def test_sites(self):
        self.assertEqual(cnh.parse_relocs(RELOCS), {
            "graph.cpp.obj": {
                "_ZN2tr5graph7graph_t9subscribeEv": {"operator new": 1, "operator delete": 1},
                "_ZN2tr5graph8vertex_tD0Ev": {"operator delete": 1},
            },
            "twai_link.cpp.obj": {
                "_ZN2tr3esp11twai_link_t5startEv": {"heap_caps_malloc": 1, "free": 1},
            },
        })


class RatchetTest(unittest.TestCase):
    def setUp(self):
        self.pins = cnh.parse_relocs(RELOCS)

    def test_pinned_passes(self):
        self.assertEqual(cnh.compare(cnh.parse_relocs(RELOCS), self.pins), ({}, {}))

    def test_second_call_in_a_pinned_function_fails(self):
        new, gone = cnh.compare(cnh.parse_relocs(SECOND_CALL), self.pins)
        self.assertEqual(new, {("graph.cpp.obj", "_ZN2tr5graph7graph_t9subscribeEv",
                                "operator new"): (1, 2)})
        self.assertEqual(gone, {})

    def test_planted_new_in_a_pinned_object_fails(self):
        new, gone = cnh.compare(cnh.parse_relocs(NEW_FUNCTION), self.pins)
        self.assertEqual(new, {("graph.cpp.obj", "_Z14tr_planted_newv", "operator new"): (0, 1)})
        self.assertEqual(gone, {})

    def test_removed_reference_must_be_unpinned(self):
        pins = cnh.parse_relocs(SECOND_CALL)
        new, gone = cnh.compare(cnh.parse_relocs(RELOCS), pins)
        self.assertEqual(new, {})
        self.assertEqual(gone, {("graph.cpp.obj", "_ZN2tr5graph7graph_t9subscribeEv",
                                 "operator new"): (2, 1)})

    def test_repin_only_lowers(self):
        pins = cnh.parse_relocs(SECOND_CALL)
        lowered = cnh.lower(pins, cnh.parse_relocs(NEW_FUNCTION))
        self.assertEqual(lowered, self.pins, "the planted site is not added, the extra call drops")

    def test_strict_has_no_pins(self):
        new, _ = cnh.compare(cnh.parse_relocs(RELOCS), {})
        self.assertEqual(len(new), 5)


if __name__ == "__main__":
    unittest.main()
