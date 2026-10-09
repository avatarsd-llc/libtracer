#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the MCU no-heap link check (`tools/check_no_heap.py`, #1783).

The check reads ``objdump -r -t`` output, so these drive its parser and its ratchet with that
output's shape, built by a small helper, without a cross toolchain:

* every heap entry point is recognised, in every C++ variant, and nothing else is;
* a function reaches the heap by naming an entry point or by calling, within the archive, a
  function that does, to a fixpoint and across members; debug and unwind relocations and data
  sections that merely point at such a function are not calls;
* the ratchet fails on a new function that names the heap, a new function that only calls an
  already-pinned allocating helper (``push_back`` into ``_M_realloc_insert``), the same from
  another object, and a second call at a pinned site; a site that shrank is GONE, and
  ``lower`` never raises a pin.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import os
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_no_heap as cnh  # noqa: E402

REALLOC = "_ZNSt6vectorIN2tr5graph12field_step_tESaIS2_EE17_M_realloc_insertEv"
EMPLACE = "_ZNSt6vectorIN2tr5graph12field_step_tESaIS2_EE12emplace_backEv"


def member(name, functions, data=None):
    """@brief One archive member as ``objdump -r -t`` prints it.

    @p functions: {symbol: (binding, [referenced symbol, ...])}, each in its own
    ``.text.<symbol>`` section; binding ``l`` (local), ``g`` (global) or ``w`` (weak).
    @p data: {section: [referenced symbol, ...]} for non-code sections.
    """
    lines = [f"{name}:     file format elf32-littleriscv", "", "SYMBOL TABLE:"]
    for sym, (bind, _) in functions.items():
        sec = f".text.{sym}"
        lines.append(f"00000000 l    d  {sec}\t00000000 {sec}")
        flags = {"l": "l     F", "g": "g     F", "w": " w    F"}[bind]
        lines.append(f"00000000 {flags} {sec}\t00000010 {sym}")
    for sec in (data or {}):
        lines.append(f"00000000 l    d  {sec}\t00000000 {sec}")
    lines.append("00000000         *UND*\t00000000 _Znwj")
    lines.append("")
    blocks = [(f".text.{s}", refs) for s, (_, refs) in functions.items()]
    blocks += list((data or {}).items())
    for sec, refs in blocks:
        lines += ["", f"RELOCATION RECORDS FOR [{sec}]:", "OFFSET   TYPE              VALUE"]
        lines += [f"{4 * i:08x} R_RISCV_CALL_PLT  {r}" for i, r in enumerate(refs)]
        lines.append("00000000 R_RISCV_RELAX     *ABS*")
    return "\n".join(lines) + "\n"


def archive(*members):
    return "In archive libtracer.a:\n\n" + "\n".join(members)


GRAPH = {
    "_ZN2tr5graph7graph_t9subscribeEv": ("g", ["_Znwj", "memcpy", "_ZdlPvj"]),
    "_ZN2tr5graph8vertex_tD0Ev": ("g", ["_ZdlPvj+0x0"]),
    "_ZN2tr5graph6helperEv": ("g", ["memcpy"]),
}
PATH = {
    REALLOC: ("w", ["_Znwj", "_ZdlPvj"]),
    EMPLACE: ("w", [REALLOC]),
    "_ZN2tr5graph6path_t5parseEv": ("g", [EMPLACE, "memset"]),
    "_ZN2tr5graph6path_t4sizeEv": ("g", []),
}
TWAI = {"_ZN2tr3esp11twai_link_t5startEv": ("g", ["heap_caps_malloc", "free"])}
DEBUG = {".debug_info": ["_Znwj"], ".data.rel.ro._ZTVN2tr5graph8vertex_tE":
         ["_ZN2tr5graph8vertex_tD0Ev"]}

BASE = archive(member("graph.cpp.obj", GRAPH, DEBUG), member("path.cpp.obj", PATH),
               member("twai_link.cpp.obj", TWAI))


def with_fn(obj, sym, refs, bind="g"):
    """@brief BASE with one more function, @p sym referencing @p refs, in member @p obj."""
    tables = {"graph.cpp.obj": dict(GRAPH), "path.cpp.obj": dict(PATH),
              "twai_link.cpp.obj": dict(TWAI)}
    tables[obj][sym] = (bind, refs)
    return archive(member("graph.cpp.obj", tables["graph.cpp.obj"], DEBUG),
                   member("path.cpp.obj", tables["path.cpp.obj"]),
                   member("twai_link.cpp.obj", tables["twai_link.cpp.obj"]))


class FamilyTest(unittest.TestCase):
    def test_heap_entry_points(self):
        for sym, fam in [("_Znwj", "operator new"), ("_Znam", "operator new[]"),
                         ("_ZdlPvm", "operator delete"), ("_ZdaPv", "operator delete[]"),
                         ("malloc", "malloc"), ("realloc", "realloc"), ("strdup", "strdup"),
                         ("_malloc_r", "_malloc_r"), ("_free_r", "_free_r"),
                         ("pvPortMalloc", "pvPortMalloc"), ("heap_caps_free", "heap_caps_free"),
                         ("heap_caps_aligned_alloc", "heap_caps_aligned_alloc"),
                         ("xTaskCreate", "xTaskCreate"), ("pthread_create", "pthread_create")]:
            self.assertEqual(cnh.family(sym), fam, sym)

    def test_not_heap(self):
        for sym in ("memcpy", "_ZN2tr3mem13heap_source_t9try_allocEjj", "freeaddrinfo",
                    "heap_caps_get_free_size", "xTaskCreateStatic", "xQueueGenericCreateStatic",
                    "*ABS*"):
            self.assertIsNone(cnh.family(sym), sym)


class ClosureTest(unittest.TestCase):
    def test_sites(self):
        self.assertEqual(cnh.parse_relocs(BASE), {
            "graph.cpp.obj": {
                "_ZN2tr5graph7graph_t9subscribeEv": {"operator new": 1, "operator delete": 1},
                "_ZN2tr5graph8vertex_tD0Ev": {"operator delete": 1},
            },
            "path.cpp.obj": {
                REALLOC: {"operator new": 1, "operator delete": 1},
                EMPLACE: {"via " + REALLOC: 1},
                "_ZN2tr5graph6path_t5parseEv": {"via " + EMPLACE: 1},
            },
            "twai_link.cpp.obj": {
                "_ZN2tr3esp11twai_link_t5startEv": {"heap_caps_malloc": 1, "free": 1},
            },
        })

    def test_section_symbol_reference_resolves(self):
        text = with_fn("graph.cpp.obj", "_ZN2tr5graph4tickEv",
                       [".text._ZN2tr5graph7graph_t9subscribeEv"])
        sites = cnh.parse_relocs(text)["graph.cpp.obj"]
        self.assertEqual(sites["_ZN2tr5graph4tickEv"],
                         {"via _ZN2tr5graph7graph_t9subscribeEv": 1})


class RatchetTest(unittest.TestCase):
    def setUp(self):
        self.pins = cnh.parse_relocs(BASE)

    def test_pinned_passes(self):
        self.assertEqual(cnh.compare(cnh.parse_relocs(BASE), self.pins), ({}, {}))

    def test_planted_new_in_a_pinned_object_fails(self):
        new, gone = cnh.compare(cnh.parse_relocs(
            with_fn("graph.cpp.obj", "_Z14tr_planted_newv", ["_Znwj"])), self.pins)
        self.assertEqual(new, {("graph.cpp.obj", "_Z14tr_planted_newv", "operator new"): (0, 1)})
        self.assertEqual(gone, {})

    def test_new_call_into_a_pinned_helper_fails(self):
        # The re-review's attack: a new function whose only call is push_back, i.e. the
        # already-pinned emplace_back -> _M_realloc_insert. It names no heap entry point.
        new, gone = cnh.compare(cnh.parse_relocs(
            with_fn("path.cpp.obj", "_Z12planted_pushv", [EMPLACE])), self.pins)
        self.assertEqual(new, {("path.cpp.obj", "_Z12planted_pushv", "via " + EMPLACE): (0, 1)})
        self.assertEqual(gone, {})

    def test_call_into_a_pinned_helper_from_another_object_fails(self):
        new, _ = cnh.compare(cnh.parse_relocs(
            with_fn("twai_link.cpp.obj", "_Z12planted_pushv", [EMPLACE])), self.pins)
        self.assertEqual(new, {("twai_link.cpp.obj", "_Z12planted_pushv", "via " + EMPLACE):
                               (0, 1)})

    def test_call_to_a_clean_function_passes(self):
        new, gone = cnh.compare(cnh.parse_relocs(
            with_fn("graph.cpp.obj", "_Z5cleanv", ["_ZN2tr5graph6helperEv", "memcpy"])),
            self.pins)
        self.assertEqual((new, gone), ({}, {}))

    def test_second_call_in_a_pinned_function_fails(self):
        text = BASE.replace(f"00000000 R_RISCV_CALL_PLT  {EMPLACE}\n",
                            f"00000000 R_RISCV_CALL_PLT  {EMPLACE}\n"
                            f"00000040 R_RISCV_CALL_PLT  {EMPLACE}\n", 1)
        new, gone = cnh.compare(cnh.parse_relocs(text), self.pins)
        self.assertEqual(new, {("path.cpp.obj", "_ZN2tr5graph6path_t5parseEv",
                                "via " + EMPLACE): (1, 2)})
        self.assertEqual(gone, {})

    def test_removed_reference_must_be_unpinned(self):
        pins = cnh.parse_relocs(with_fn("path.cpp.obj", "_Z12planted_pushv", [EMPLACE]))
        new, gone = cnh.compare(cnh.parse_relocs(BASE), pins)
        self.assertEqual(new, {})
        self.assertEqual(gone, {("path.cpp.obj", "_Z12planted_pushv", "via " + EMPLACE): (1, 0)})

    def test_repin_only_lowers(self):
        pins = cnh.parse_relocs(with_fn("path.cpp.obj", "_Z12planted_pushv", [EMPLACE]))
        found = cnh.parse_relocs(with_fn("graph.cpp.obj", "_Z14tr_planted_newv", ["_Znwj"]))
        self.assertEqual(cnh.lower(pins, found), self.pins,
                         "the dropped site goes, the new one is not added")

    def test_strict_has_no_pins(self):
        new, _ = cnh.compare(cnh.parse_relocs(BASE), {})
        self.assertEqual(len(new), 9)


class RebaselineTest(unittest.TestCase):
    def setUp(self):
        self.pins = cnh.parse_relocs(BASE)

    def renamed(self, extra_refs=None):
        """@brief BASE with subscribe renamed, optionally also gaining a second heap call."""
        old = "_ZN2tr5graph7graph_t9subscribeEv"
        refs = ["_Znwj", "memcpy", "_ZdlPvj"] + (extra_refs or [])
        graph = {("_ZN2tr5graph7graph_t10subscribe2Ev" if k == old else k): v
                 for k, v in GRAPH.items()}
        graph["_ZN2tr5graph7graph_t10subscribe2Ev"] = ("g", refs)
        return archive(member("graph.cpp.obj", graph, DEBUG), member("path.cpp.obj", PATH),
                       member("twai_link.cpp.obj", TWAI))

    def test_pure_rename_reseeds_cleanly(self):
        found = cnh.parse_relocs(self.renamed())
        self.assertNotEqual(cnh.compare(found, self.pins), ({}, {}), "the plain ratchet fails")
        self.assertIsNone(cnh.rebaseline(self.pins, found))

    def test_rename_that_adds_a_heap_call_is_refused(self):
        found = cnh.parse_relocs(self.renamed(["_Znwj"]))
        self.assertIsNotNone(cnh.rebaseline(self.pins, found))

    def test_new_site_without_a_vanished_twin_is_refused(self):
        found = cnh.parse_relocs(with_fn("graph.cpp.obj", "_Z14tr_planted_newv", ["_Znwj"]))
        self.assertIsNotNone(cnh.rebaseline(self.pins, found))


if __name__ == "__main__":
    unittest.main()
