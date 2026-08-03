#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the doc-citation gate's spelling resolver.

`tools/check_doc_citations.py` is the only thing standing between the docs and the rot
class #802 found by hand: a `file:line` citation that still RESOLVES, still reads
authoritative, and points at unrelated code. #803 widened it to read the design pages'
basename shorthand and the `config.hpp.in` template, which put a resolver — a thing that
can be wrong quietly — between the docs and the gate. These pin its four decisions:
an ambiguous basename is an error rather than a guess, `.hpp.in` counts as a source, a
bare `:N` inherits the file most recently named, and an unknown name resolves to nothing
instead of to something.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import os
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_doc_citations as cdc  # noqa: E402

# A stand-in tree: two unambiguous basenames, one template, and one basename carried by
# two files — the shape the real repo has (`app_main.cpp` lives in four integrations).
FILEMAP = {
    "graph.cpp": ["core/src/graph.cpp"],
    "graph.hpp": ["core/include/libtracer/graph.hpp"],
    "config.hpp.in": ["core/include/libtracer/config.hpp.in"],
    "app_main.cpp": ["examples/a/app_main.cpp", "examples/b/app_main.cpp"],
}


def locs(text):
    """The resolved `path:line` set for `text` against the stand-in tree."""
    return cdc.cited_locations(text, FILEMAP)[0]


def errs(text):
    """The ambiguity errors `text` produces against the stand-in tree."""
    return cdc.cited_locations(text, FILEMAP)[1]


class ResolveShorthandTest(unittest.TestCase):
    """The basename shorthand the design pages have always used."""

    def test_basename_resolves_to_the_full_path(self):
        self.assertEqual(locs("see `graph.hpp:1053` for the map"),
                         {"core/include/libtracer/graph.hpp:1053"})

    def test_full_path_still_resolves_unchanged(self):
        self.assertEqual(locs("`core/src/graph.cpp:946`"), {"core/src/graph.cpp:946"})

    def test_the_two_spellings_normalise_to_one_location(self):
        both = locs("`graph.cpp:946` and `core/src/graph.cpp:946`")
        self.assertEqual(both, {"core/src/graph.cpp:946"})

    def test_unbackticked_form_inside_a_code_excerpt_resolves(self):
        self.assertEqual(locs("core/src/graph.cpp:1021   std::array<std::byte, 4096> stack;"),
                         {"core/src/graph.cpp:1021"})


class AmbiguousBasenameTest(unittest.TestCase):
    """A basename two files carry is an error, never a guess."""

    def test_ambiguous_basename_reports_an_error(self):
        self.assertEqual(len(errs("`app_main.cpp:12`")), 1)

    def test_the_error_names_every_candidate_and_asks_for_the_full_path(self):
        (message,) = errs("`app_main.cpp:12`")
        self.assertIn("examples/a/app_main.cpp", message)
        self.assertIn("examples/b/app_main.cpp", message)
        self.assertIn("full path", message)

    def test_an_ambiguous_basename_pins_no_location(self):
        self.assertEqual(locs("`app_main.cpp:12`"), set())

    def test_spelling_the_full_path_resolves_an_otherwise_ambiguous_basename(self):
        self.assertEqual(locs("`examples/b/app_main.cpp:12`"), {"examples/b/app_main.cpp:12"})
        self.assertEqual(errs("`examples/b/app_main.cpp:12`"), [])


class TemplateExtensionTest(unittest.TestCase):
    """`config.hpp.in` — the knob declarations the configuration pages cite."""

    def test_dot_in_template_resolves(self):
        self.assertEqual(locs("`config.hpp.in:237`"),
                         {"core/include/libtracer/config.hpp.in:237"})

    def test_the_template_does_not_truncate_to_the_generated_header(self):
        # The extension alternation is ordered longest-first; a `.hpp` match on
        # `config.hpp.in` would silently pin the wrong (generated, untracked) file.
        self.assertNotIn("core/include/libtracer/config.hpp:237", locs("`config.hpp.in:237`"))

    def test_source_map_indexes_the_template_by_its_full_basename(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "inc"))
            for name in ("config.hpp.in", "graph.hpp"):
                open(os.path.join(root, "inc", name), "w").close()
            found = cdc.source_map(cdc.pathlib.Path(root))
        self.assertEqual(found["config.hpp.in"], ["inc/config.hpp.in"])
        self.assertNotIn("config.hpp", found)

    def test_source_map_records_both_paths_for_a_repeated_basename(self):
        with tempfile.TemporaryDirectory() as root:
            for sub in ("a", "b"):
                os.makedirs(os.path.join(root, sub))
                open(os.path.join(root, sub, "app_main.cpp"), "w").close()
            found = cdc.source_map(cdc.pathlib.Path(root))
        self.assertEqual(found["app_main.cpp"], ["a/app_main.cpp", "b/app_main.cpp"])

    def test_source_map_skips_build_output(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "build"))
            open(os.path.join(root, "build", "graph.hpp"), "w").close()
            found = cdc.source_map(cdc.pathlib.Path(root))
        self.assertEqual(found, {})


class BareContinuationTest(unittest.TestCase):
    """A bare `:N` inherits the file most recently named."""

    def test_bare_line_inherits_the_preceding_file(self):
        self.assertEqual(locs("`graph.cpp:946` then `:304`"),
                         {"core/src/graph.cpp:946", "core/src/graph.cpp:304"})

    def test_bare_line_inherits_across_a_basename_spelling_too(self):
        self.assertEqual(locs("`graph.hpp:1053`\n\nand `:1060`"),
                         {"core/include/libtracer/graph.hpp:1053",
                          "core/include/libtracer/graph.hpp:1060"})

    def test_bare_line_with_nothing_before_it_resolves_to_nothing(self):
        self.assertEqual(locs("a stray `:304` opening a page"), set())

    def test_a_cited_markdown_page_breaks_the_inheritance_run(self):
        # `reference/07.md:79` then `:285` means line 285 of that PAGE. Attaching it to
        # the header named three paragraphs earlier pinned a line nothing was discussing.
        self.assertEqual(locs("`graph.hpp:1053` ... `docs/reference/07.md:79` ... `:285`"),
                         {"core/include/libtracer/graph.hpp:1053"})

    def test_a_cited_build_file_does_NOT_break_the_run(self):
        # The knob table names `config.hpp.in` once, then walks it in bare refs while each
        # row's CMake column cites a `CMakeLists.txt` line in between.
        self.assertEqual(locs("`config.hpp.in:86` | `CMakeLists.txt:188` |\n| `:109` |"),
                         {"core/include/libtracer/config.hpp.in:86",
                          "core/include/libtracer/config.hpp.in:109"})

    def test_an_unresolvable_name_breaks_the_run_rather_than_stealing_it(self):
        self.assertEqual(locs("`graph.cpp:946` ... `elsewhere.cpp:11` ... `:304`"),
                         {"core/src/graph.cpp:946"})


class NoMatchTest(unittest.TestCase):
    """A name the tree does not carry resolves to nothing, silently."""

    def test_unknown_basename_pins_nothing(self):
        self.assertEqual(locs("`not_in_the_tree.cpp:42`"), set())

    def test_unknown_basename_is_not_an_error(self):
        self.assertEqual(errs("`not_in_the_tree.cpp:42`"), [])

    def test_a_non_source_extension_is_not_a_citation(self):
        self.assertEqual(locs("`tools/sync-version.py:42`"), set())


class SpanTest(unittest.TestCase):
    """Ranges and comma lists — a citation is a pointer, not a listing."""

    def test_a_range_registers_every_line_in_it(self):
        self.assertEqual(locs("`graph.cpp:996-998`"),
                         {"core/src/graph.cpp:996", "core/src/graph.cpp:997",
                          "core/src/graph.cpp:998"})

    def test_a_comma_list_registers_each_line(self):
        self.assertEqual(locs("`graph.hpp:145,153`"),
                         {"core/include/libtracer/graph.hpp:145",
                          "core/include/libtracer/graph.hpp:153"})

    def test_an_implausible_span_collapses_to_its_first_line(self):
        # A hyphenated word next to a citation is a parse artifact, not a 900-line span.
        self.assertEqual(locs("`graph.cpp:12-900`"), {"core/src/graph.cpp:12"})

    def test_citation_spans_reports_the_span_not_its_interior(self):
        spans, _ = cdc.citation_spans("`graph.cpp:996-998`", FILEMAP)
        self.assertEqual(spans, [("core/src/graph.cpp", 996, 998)])


if __name__ == "__main__":
    unittest.main()
