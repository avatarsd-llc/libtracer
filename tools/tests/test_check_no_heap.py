#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the MCU no-heap link check (`tools/check_no_heap.py`, #1783).

The check reads ``objdump -r -t`` output, so these drive its parser and its gate with that
output's shape, built by a small helper, without a cross toolchain:

* every heap entry point is recognised, in every C++ variant, and nothing else is;
* a function reaches the heap by naming an entry point or by calling, within the archive, a
  function that does, to a fixpoint and across members; debug and unwind relocations and data
  sections that merely point at such a function are not calls;
* the gate is zero (#2288): an archive with no heap reference passes, any reference fails and
  is named by object, function and entry point, and there is no baseline file or flag that
  could absorb one.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import contextlib
import io
import os
import pathlib
import sys
import tempfile
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


CLEAN = archive(member("graph.cpp.obj", {"_ZN2tr5graph6helperEv": ("g", ["memcpy"])}, DEBUG),
                member("path.cpp.obj", {"_ZN2tr5graph6path_t4sizeEv": ("g", [])}))


class GateTest(unittest.TestCase):
    """The gate is zero: any reference fails, and there is no baseline to absorb one (#2288)."""

    def run_main(self, text):
        with tempfile.TemporaryDirectory() as d:
            dump = pathlib.Path(d) / "objdump.txt"
            dump.write_text(text)
            tool = pathlib.Path(d) / "objdump"
            tool.write_text(f"#!/bin/sh\ncat '{dump}'\n")
            tool.chmod(0o755)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = cnh.main(["--target", "esp32c6", "--archive", "libtracer.a",
                                 "--objdump", str(tool)])
        return code, out.getvalue()

    def test_an_archive_with_no_heap_reference_passes(self):
        code, out = self.run_main(CLEAN)
        self.assertEqual(code, 0, out)
        self.assertIn("0 heap reference(s)", out)

    def test_any_reference_fails_and_names_each_function_and_entry_point(self):
        code, out = self.run_main(BASE)
        self.assertEqual(code, 1)
        errors = [ln for ln in out.splitlines() if ln.startswith("::error::")]
        self.assertEqual(len(errors), 9, out)  # one per (function, entry point)
        self.assertTrue(any("twai_link.cpp.obj" in e and "heap_caps_malloc" in e for e in errors))
        self.assertTrue(any(("path_t5parse" in e or "path_t::parse" in e) and "reaches the heap" in e for e in errors))

    def test_one_planted_new_fails_a_clean_archive(self):
        planted = CLEAN.replace("RELOCATION RECORDS FOR [.text._ZN2tr5graph6helperEv]:\n"
                                "OFFSET   TYPE              VALUE\n",
                                "RELOCATION RECORDS FOR [.text._ZN2tr5graph6helperEv]:\n"
                                "OFFSET   TYPE              VALUE\n"
                                "00000040 R_RISCV_CALL_PLT  _Znwj\n", 1)
        code, out = self.run_main(planted)
        self.assertEqual(code, 1)
        self.assertIn("operator new", out)

    def test_there_is_no_baseline_to_absorb_a_reference(self):
        self.assertFalse((pathlib.Path(REPO) / "tools" / "no_heap_baseline.json").exists())
        for flag in ("--baseline", "--repin", "--rebaseline", "--seed", "--strict"):
            with self.assertRaises(SystemExit, msg=flag), \
                    contextlib.redirect_stderr(io.StringIO()):
                cnh.main(["--target", "x", "--archive", "a", flag, "y"])

    def test_the_ci_gates_run_without_a_baseline(self):
        for wf in ("esp-idf.yml", "footprint-cortexm0.yml"):
            text = (pathlib.Path(REPO) / ".github" / "workflows" / wf).read_text()
            self.assertIn("check_no_heap.py", text, wf)
            self.assertNotIn("no_heap_baseline", text, wf)
            self.assertNotIn("--repin", text, wf)


if __name__ == "__main__":
    unittest.main()
