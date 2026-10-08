#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the MCU no-heap link check (`tools/check_no_heap.py`, #1783).

The check reads ``nm -A -u`` output, so these drive its parser and its ratchet with that
output verbatim, without a cross toolchain:

* every heap entry point is recognised, in every C++ variant, and nothing else is;
* a reference the baseline does not pin is NEW (the planted-``new`` case), and a pin that
  is no longer referenced is GONE, so the baseline can only shrink.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import os
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_no_heap as cnh  # noqa: E402

NM = """\
libtracer.a:graph.cpp.obj:         U _Znwj
libtracer.a:graph.cpp.obj:         U _ZdlPvj
libtracer.a:graph.cpp.obj:         U memcpy
libtracer.a:mem_source.cpp.obj:         U _ZnwjSt11align_val_tRKSt9nothrow_t
libtracer.a:mem_source.cpp.obj:         U _ZdlPvjSt11align_val_t
libtracer.a:twai_link.cpp.obj:         U heap_caps_malloc
libtracer.a:twai_link.cpp.obj:         U free
libtracer.a:rope.cpp.obj:         U _ZN2tr4view6rope_t6appendENS0_6view_tE
"""


class FamilyTest(unittest.TestCase):
    def test_heap_entry_points(self):
        for sym, fam in [("_Znwj", "operator new"), ("_Znam", "operator new[]"),
                         ("_ZdlPvm", "operator delete"), ("_ZdaPv", "operator delete[]"),
                         ("malloc", "malloc"), ("realloc", "realloc"), ("strdup", "strdup"),
                         ("pvPortMalloc", "pvPortMalloc"), ("heap_caps_free", "heap_caps_free"),
                         ("heap_caps_aligned_alloc", "heap_caps_aligned_alloc")]:
            self.assertEqual(cnh.family(sym), fam, sym)

    def test_not_heap(self):
        for sym in ("memcpy", "_ZN2tr3mem13heap_source_t9try_allocEjj", "freeaddrinfo",
                    "heap_caps_get_free_size", "_ZNSt6thread15_M_start_threadE"):
            self.assertIsNone(cnh.family(sym), sym)


class RatchetTest(unittest.TestCase):
    def setUp(self):
        self.found = cnh.parse_nm(NM)

    def test_parse(self):
        self.assertEqual(self.found, {
            "graph.cpp.obj": {"operator new", "operator delete"},
            "mem_source.cpp.obj": {"operator new", "operator delete"},
            "twai_link.cpp.obj": {"heap_caps_malloc", "free"},
        })

    def test_pinned_passes(self):
        pins = {o: sorted(f) for o, f in self.found.items()}
        self.assertEqual(cnh.compare(self.found, pins), ({}, {}))

    def test_planted_new_fails(self):
        pins = {o: sorted(f) for o, f in self.found.items()}
        planted = cnh.parse_nm(NM + "libtracer.a:rope.cpp.obj:         U _Znwj\n")
        new, gone = cnh.compare(planted, pins)
        self.assertEqual(new, {"rope.cpp.obj": ["operator new"]})
        self.assertEqual(gone, {})

    def test_removed_reference_must_be_unpinned(self):
        pins = {o: sorted(f) for o, f in self.found.items()}
        pins["rope.cpp.obj"] = ["operator delete"]
        new, gone = cnh.compare(self.found, pins)
        self.assertEqual(new, {})
        self.assertEqual(gone, {"rope.cpp.obj": ["operator delete"]})

    def test_strict_has_no_pins(self):
        new, _ = cnh.compare(self.found, {})
        self.assertEqual(set(new), set(self.found))


if __name__ == "__main__":
    unittest.main()
