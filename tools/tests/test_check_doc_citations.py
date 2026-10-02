#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the doc-citation gate (`tools/check_doc_citations.py`).

The gate checks SYMBOL citations by search (#1705) and refuses LINE-NUMBER citations in every
living doc (#1706). These pin both halves: the spellings a line citation can take are all
refused — a full path, the basename shorthand, a range, a comma list, a bare `:N` continuing
the file named before it — while the tokens that only LOOK like one (an address, a cited
markdown page, a file outside the tree) are left alone; and a symbol citation resolves to one
declaration, or is reported GONE or AMBIGUOUS, naming the citing page.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import contextlib
import io
import os
import pathlib
import shutil
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_doc_citations as cdc  # noqa: E402

# A stand-in tree: unambiguous basenames, one template, and one basename carried by two
# files — the shape the real repo has (`app_main.cpp` lives in several integrations).
# `config.hpp.in` is a FIXTURE name: what it pins is the `.hpp.in` SUFFIX support, in
# particular that the longest-first alternation never truncates it onto a `.hpp` file.
FILEMAP = {
    "graph.cpp": ["core/src/graph.cpp"],
    "graph.hpp": ["core/include/libtracer/graph.hpp"],
    "config.hpp.in": ["core/include/libtracer/config.hpp.in"],
    "app_main.cpp": ["examples/a/app_main.cpp", "examples/b/app_main.cpp"],
    "transport_ws.hpp": ["core/include/libtracer/transport_ws.hpp"],
}

GRAPH = "core/src/graph.cpp"
GRAPH_HPP = "core/include/libtracer/graph.hpp"


def refused(text):
    """The refusal messages the line-citation check gives `text` against the stand-in tree."""
    return cdc.line_citations(text, FILEMAP)


class LineCitationRefusalTest(unittest.TestCase):
    """Every spelling a line citation can take is refused, named by its resolved path."""

    def assert_refuses(self, text, *cited):
        out = refused(text)
        self.assertEqual(len(out), len(cited), out)
        for message, cite in zip(out, cited):
            self.assertIn(f"`{cite}`", message)
            self.assertIn("by symbol", message)

    def test_a_full_path_is_refused(self):
        self.assert_refuses("`core/src/graph.cpp:946`", f"{GRAPH}:946")

    def test_the_basename_shorthand_is_refused_under_its_full_path(self):
        self.assert_refuses("see `graph.hpp:1053` for the map", f"{GRAPH_HPP}:1053")

    def test_a_range_and_a_comma_list_are_refused(self):
        self.assert_refuses("`graph.cpp:12-20` and `transport_ws.hpp:181,339`",
                            f"{GRAPH}:12-20", "core/include/libtracer/transport_ws.hpp:181,339")

    def test_the_unbackticked_form_inside_a_code_excerpt_is_refused(self):
        self.assert_refuses("core/src/graph.cpp:1021   std::array<std::byte, 4096> stack;",
                            f"{GRAPH}:1021")

    def test_the_template_is_refused_as_itself_not_truncated(self):
        self.assert_refuses("`config.hpp.in:237`", "core/include/libtracer/config.hpp.in:237")

    def test_a_bare_continuation_is_refused_under_the_file_it_continues(self):
        self.assert_refuses("`graph.cpp:946` then `:304`", f"{GRAPH}:946", f"{GRAPH}:304")

    def test_a_non_source_file_in_the_tree_is_refused(self):
        out = cdc.line_citations("`.github/workflows/core-ci.yml:95-106`")
        self.assertEqual(len(out), 1, out)
        self.assertIn(".github/workflows/core-ci.yml:95-106", out[0])


class NotALineCitationTest(unittest.TestCase):
    """Tokens that only LOOK like a line citation are left alone."""

    def test_a_bare_line_with_nothing_before_it(self):
        self.assertEqual(refused("a stray `:304` opening a page, or `c` on :47311"), [])

    def test_a_cited_markdown_page_is_not_refused_and_ends_the_run(self):
        # `07.md:79` then `:285` means line 285 of that PAGE: a pointer into prose.
        self.assertEqual(refused("`docs/reference/07.md:79` ... `:285`"), [])
        self.assertEqual(len(refused("`graph.cpp:1` ... `docs/reference/07.md:79` ... `:285`")), 1)

    def test_an_address_names_no_file(self):
        self.assertEqual(cdc.line_citations("dial `127.0.0.1:47301` or `wss://robot.local:9000`"), [])

    def test_an_unknown_source_basename_is_not_a_citation(self):
        self.assertEqual(refused("`not_in_the_tree.cpp:42`"), [])

    def test_an_ambiguous_basename_is_reported_as_ambiguous(self):
        (message,) = refused("`app_main.cpp:12`")
        self.assertIn("examples/a/app_main.cpp", message)
        self.assertIn("examples/b/app_main.cpp", message)
        self.assertIn("full path", message)


class SourceMapTest(unittest.TestCase):
    """The basename map the shorthand resolves against."""

    def test_source_map_indexes_the_template_by_its_full_basename(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "inc"))
            for name in ("config.hpp.in", "graph.hpp"):
                open(os.path.join(root, "inc", name), "w").close()
            found = cdc.source_map(cdc.pathlib.Path(root))
        self.assertEqual(found["config.hpp.in"], ["inc/config.hpp.in"])
        self.assertNotIn("config.hpp", found)

    def test_source_map_skips_build_output(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "build"))
            open(os.path.join(root, "build", "graph.hpp"), "w").close()
            found = cdc.source_map(cdc.pathlib.Path(root))
        self.assertEqual(found, {})


class HistoricalGenreTest(unittest.TestCase):
    """A dated record cites the tree AS IT STOOD; so the gate never checks it."""

    def test_the_three_dated_genres_are_historical(self):
        for rel in ("docs/adr/0078-acl-cache-coherence.md",
                    "docs/spec/rfcs/0024-bound-paths.md",
                    "docs/research/2026-07-04-architecture-deepening-review.md"):
            self.assertTrue(cdc.is_historical(rel), rel)

    def test_the_living_doc_surfaces_are_not(self):
        for rel in ("CONTEXT.md", "docs/modules/transport.md", "docs/reference/00-overview.md",
                    "docs/design/allocation-and-backpressure.md", "docs/getting-started.md",
                    "tools/check_doc_citations.py"):
            self.assertFalse(cdc.is_historical(rel), rel)

    def test_a_docs_prefix_that_merely_starts_the_same_is_not_historical(self):
        self.assertFalse(cdc.is_historical("docs/adrs-explained.md"))
        self.assertFalse(cdc.is_historical("docs/specification-notes.md"))


class NonSourceDirTest(unittest.TestCase):
    """Directories that hold a SECOND copy of the tree must not make basenames ambiguous.

    The gate resolves a bare `graph.cpp:42` by basename, so any directory carrying a copy
    of the sources turns every such citation into an "ambiguous" error. Which copies exist
    depends on what the developer happened to build or unpack, so without these exclusions
    the gate is green or red by accident. `.pio` is PlatformIO's per-project cache: a
    `pio run` in the packaging fixture unpacks the library under test into
    `.pio/libdeps/<env>/libtracer/` (#965).
    """

    def _tree_with(self, *rel_paths):
        """@brief Build a temp tree containing each path, and return its source map."""
        root = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root, True)
        for rel in rel_paths:
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("// x\n")
        return cdc.source_map(root)

    def test_a_pio_libdeps_copy_does_not_make_a_basename_ambiguous(self):
        m = self._tree_with(
            "core/src/graph.cpp",
            "tests/packaging/pio_esp32_can/.pio/libdeps/esp32c6/libtracer/core/src/graph.cpp",
        )
        self.assertEqual(m["graph.cpp"], ["core/src/graph.cpp"])

    def test_a_build_agent_copy_does_not_make_a_basename_ambiguous(self):
        m = self._tree_with(
            "core/include/libtracer/config.hpp",
            "build-agent/generated/include/libtracer/config.hpp",
        )
        self.assertEqual(m["config.hpp"], ["core/include/libtracer/config.hpp"])

    def test_a_bench_agent_copy_does_not_make_a_basename_ambiguous(self):
        """The `bench-` half of NON_SOURCE_DIR_PREFIXES (#1050).

        Sibling of the `build-agent` case above. A `cmake -S bench -B bench-agent` renders
        the same generated `config.hpp` one level deeper than a core build does, so the
        nesting differs from the `build-` case and is worth its own case rather than a
        parametrisation. Without this, reverting `NON_SOURCE_DIR_PREFIXES` to
        `("build-",)` leaves the whole suite green — the prefix was added with no
        mechanized guard, and a real `bench-*` tree never exists in CI.
        """
        m = self._tree_with(
            "core/include/libtracer/config.hpp",
            "bench-agent/core/generated/include/libtracer/config.hpp",
        )
        self.assertEqual(m["config.hpp"], ["core/include/libtracer/config.hpp"])

    def test_a_genuine_second_copy_IS_still_ambiguous(self):
        """The exclusions must not be so broad that real ambiguity stops being reported."""
        m = self._tree_with("examples/a/app_main.cpp", "examples/b/app_main.cpp")
        self.assertEqual(len(m["app_main.cpp"]), 2)


# A stand-in header for the symbol resolver (#1705): a declared-once member that is USED
# many times, two overloads, a constructor sharing its class's name, a parameter sharing a
# member's name, a stale comment naming a renamed symbol, and an out-of-line definition.
SYMBOL_HEADER = """\
#pragma once
namespace tr::graph {
// legacy_name used to live here; the comment outlived the rename.
class graph_t {
   public:
    explicit graph_t(int slots);
    graph_t(int slots, bool peer_named);
    [[nodiscard]] int unsubscribe(const sub_t& sub);
    [[nodiscard]] int unsubscribe(const sub_t& sub,
                                  release_fn_t release);
    int size() const { return slots_; }
    int twice() const { return slots_ * 2; }

   private:
    int slots_ = 0;
};
struct alignas(8) edge_pub_t {
    edge_pub_t* next = nullptr;
};
inline int graph_t::helper(int x) { return x; }
}  // namespace tr::graph
""".split("\n")


class SymbolHitsTest(unittest.TestCase):
    """`symbol_hits` — the search that replaces a line number with a name (#1705).

    One hit resolves, none is a GONE symbol, more than one is ambiguous. Each case below is a
    shape the design and module pages actually cite.
    """

    def hits(self, needle):
        return cdc.symbol_hits(SYMBOL_HEADER, needle)

    def line_of(self, text):
        return [i + 1 for i, ln in enumerate(SYMBOL_HEADER) if text in ln]

    def test_a_member_used_many_times_resolves_to_its_declaration(self):
        self.assertEqual(self.hits("graph_t::slots_"), self.line_of("int slots_ = 0;"))
        self.assertEqual(self.hits("slots_"), self.line_of("int slots_ = 0;"))

    def test_a_type_name_means_the_type_not_its_constructors(self):
        self.assertEqual(self.hits("graph_t"), self.line_of("class graph_t {"))
        self.assertEqual(self.hits("edge_pub_t"), self.line_of("struct alignas(8) edge_pub_t {"))

    def test_a_constructor_is_named_by_qualifying_the_type_with_itself(self):
        self.assertEqual(len(self.hits("graph_t::graph_t")), 2)
        self.assertEqual(self.hits("graph_t::graph_t(int slots)"),
                         self.line_of("explicit graph_t(int slots);"))

    def test_overloads_are_ambiguous_until_the_parameters_single_one_out(self):
        self.assertEqual(len(self.hits("graph_t::unsubscribe")), 2)
        one = self.line_of("unsubscribe(const sub_t& sub);")
        self.assertEqual(self.hits("graph_t::unsubscribe(const sub_t& sub)"), one)
        # A parameter list that wraps onto the next line still counts.
        two = self.line_of("unsubscribe(const sub_t& sub,")
        self.assertEqual(self.hits("graph_t::unsubscribe(const sub_t& sub, release_fn_t"), two)

    def test_a_parameter_is_not_a_declaration_of_the_member_it_shadows(self):
        self.assertEqual(self.hits("peer_named"), self.line_of("bool peer_named);"))
        self.assertEqual(self.hits("graph_t::peer_named"), [])

    def test_a_renamed_symbol_is_gone_even_while_a_comment_still_names_it(self):
        self.assertEqual(self.hits("legacy_name"), [])

    def test_an_out_of_line_definition_resolves_by_its_qualified_name(self):
        self.assertEqual(self.hits("graph_t::helper"), self.line_of("graph_t::helper(int x)"))

    def test_a_substring_must_sit_on_exactly_one_line(self):
        self.assertEqual(self.hits("return slots_ * 2"), self.line_of("slots_ * 2"))
        self.assertEqual(len(self.hits("return slots_")), 2)
        # A substring that looks like `name(args` but declares nothing is matched literally.
        self.assertEqual(self.hits("size() const { return"), self.line_of("int size() const"))


class SymbolCitationScanTest(unittest.TestCase):
    """The scanner reads a symbol citation, and it starts a bare-continuation run."""

    def test_a_symbol_citation_is_not_a_line_citation(self):
        self.assertEqual(refused("see `graph.cpp:graph_t::propagate`"), [])

    def test_a_bare_line_after_a_symbol_citation_is_refused_under_that_file(self):
        out = refused("`graph.hpp:graph_t` then `graph.cpp:graph_t::propagate` and `:99`")
        self.assertEqual(len(out), 1, out)
        self.assertIn(f"`{GRAPH}:99`", out[0])

    def test_prose_after_a_colon_is_not_a_citation(self):
        # A compiler message in a CHANGELOG: a space follows the colon, so no symbol branch.
        m = [x for x in cdc.CITATION_RE.finditer("`esp_http_server.h: No such file`")
             if x.group("sympath")]
        self.assertEqual(m, [])


class SymbolCitationGateTest(unittest.TestCase):
    """That `main` checks symbol citations against the REAL tree, and names the citing page."""

    @contextlib.contextmanager
    def _gate_over(self, body):
        directory = os.path.join(REPO, "docs/design")
        doc = os.path.join(directory, "zz-1705-probe.md")
        with open(doc, "w") as fh:
            fh.write(body)
        real_all_docs, buf = cdc.all_docs, io.StringIO()
        try:
            cdc.all_docs = lambda: [pathlib.Path(doc)]
            with contextlib.redirect_stdout(buf):
                cdc.main([])
            yield buf.getvalue()
        finally:
            cdc.all_docs = real_all_docs
            os.remove(doc)

    def test_a_live_symbol_passes(self):
        with self._gate_over(f"the write path, `{GRAPH}:graph_t::write_impl`\n") as out:
            self.assertNotIn("zz-1705-probe.md", out)

    def test_a_renamed_symbol_fails_and_names_the_citation(self):
        with self._gate_over(f"`{GRAPH}:graph_t::write_impl_renamed_away`\n") as out:
            self.assertIn("zz-1705-probe.md", out)
            self.assertIn("GONE", out)
            self.assertIn("graph_t::write_impl_renamed_away", out)

    def test_an_ambiguous_substring_fails(self):
        with self._gate_over(f"`{GRAPH}:return std::unexpected(status_t::SCHEMA_NOT_FOUND);`\n") as out:
            self.assertIn("AMBIGUOUS", out)

    def test_a_source_spelling_naming_no_file_fails(self):
        with self._gate_over("`no_such_file_1705.cpp:graph_t::write_impl`\n") as out:
            self.assertIn("names no source file", out)

    def test_a_dated_record_is_not_checked(self):
        directory = os.path.join(REPO, "docs/adr")
        doc = os.path.join(directory, "zz-1705-probe.md")
        with open(doc, "w") as fh:
            fh.write(f"`{GRAPH}:graph_t::write_impl_renamed_away`\n")
        real_all_docs, buf = cdc.all_docs, io.StringIO()
        try:
            cdc.all_docs = lambda: [pathlib.Path(doc)]
            with contextlib.redirect_stdout(buf):
                cdc.main([])
        finally:
            cdc.all_docs = real_all_docs
            os.remove(doc)
        self.assertNotIn("zz-1705-probe.md", buf.getvalue())

    def test_a_line_citation_fails_the_gate_and_names_the_page(self):
        with self._gate_over(f"the write path, `{GRAPH}:42`\n") as out:
            self.assertIn("zz-1705-probe.md", out)
            self.assertIn("line-number citation", out)


class ContractTest(unittest.TestCase):
    """The re-pin tool and the anchor table are gone, and the real tree is clean (#1706)."""

    def test_the_line_citation_machinery_is_deleted(self):
        for name in ("ANCHORS", "repin", "anchor_hits", "CITABLE_NON_SOURCE_PATHS",
                     "unanchored_citations", "revision_line_maps"):
            self.assertFalse(hasattr(cdc, name), name)

    def test_the_gate_takes_no_arguments(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(cdc.main(["--repin"]), 2)

    def test_the_real_tree_passes(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            status = cdc.main([])
        self.assertEqual(status, 0, buf.getvalue())


if __name__ == "__main__":
    unittest.main()
