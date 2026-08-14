#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the doc-citation gate's spelling resolver and its re-pin.

`tools/check_doc_citations.py` is the only thing standing between the docs and the rot
class #802 found by hand: a `file:line` citation that still RESOLVES, still reads
authoritative, and points at unrelated code. #803 widened it to read the design pages'
basename shorthand and the `config.hpp.in` template, which put a resolver — a thing that
can be wrong quietly — between the docs and the gate. These pin its four decisions:
an ambiguous basename is an error rather than a guess, `.hpp.in` counts as a source, a
bare `:N` inherits the file most recently named, and an unknown name resolves to nothing
instead of to something.

The `Repin*` classes pin the other half (#836). A sweep is only as good as the spellings
it can see, and the ones it could NOT see are on the record: a range whose far endpoint
never moved (leaving `graph.cpp:1118-1114`), a comma continuation
(`transport_ws.hpp:181,339`), a bare `:2405` inheriting its file from the citation
before it, a basename with no directory (`transport_can.hpp:367`), and three
`grammar.hpp` anchors skipped because the anchor table is written in BOTH quote styles
and the sweep matched one. Each of those is a case below, and each of them fails if the
handling for that spelling is taken back out.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import contextlib
import io
import os
import pathlib
import re
import shutil
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_doc_citations as cdc  # noqa: E402

# A stand-in tree: two unambiguous basenames, one template, and one basename carried by
# two files — the shape the real repo has (`app_main.cpp` lives in four integrations).
#
# `config.hpp.in` is a FIXTURE name, not a tree path: #1244 deleted the real template when
# the ESP-IDF component moved to an override fragment. What these cases pin is the `.hpp.in`
# SUFFIX support in `SOURCE_SUFFIXES` — in particular that the longest-first alternation does
# not truncate a `.hpp.in` citation onto a `.hpp` file — which outlives any one template.
FILEMAP = {
    "graph.cpp": ["core/src/graph.cpp"],
    "graph.hpp": ["core/include/libtracer/graph.hpp"],
    "config.hpp.in": ["core/include/libtracer/config.hpp.in"],
    "app_main.cpp": ["examples/a/app_main.cpp", "examples/b/app_main.cpp"],
    # The two transport headers the 2026-08-07 sweep tripped over, by their real
    # spellings: `transport_ws.hpp:181,339` (comma continuation) and a bare
    # `transport_can.hpp:367` (no directory) — both in `docs/modules/transport.md`.
    "transport_ws.hpp": ["core/include/libtracer/transport_ws.hpp"],
    "transport_can.hpp": ["core/include/libtracer/transport_can.hpp"],
}

GRAPH = "core/src/graph.cpp"
WS = "core/include/libtracer/transport_ws.hpp"
CAN = "core/include/libtracer/transport_can.hpp"


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


def repinned(text, maps):
    """The re-pinned text for `text` under `maps`."""
    return cdc.repin_document(text, maps, FILEMAP)[0]


def moved(text, maps):
    """The (path, old, new) moves the re-pin makes in `text`."""
    return cdc.repin_document(text, maps, FILEMAP)[1]


def held(text, maps):
    """The (path, line) citations the re-pin refuses to move and hands to a human."""
    return cdc.repin_document(text, maps, FILEMAP)[2]


def write_tree(root, rel, text):
    """Create `rel` under `root` with `text`; returns nothing."""
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(text)


class LineMapTest(unittest.TestCase):
    """The exact old->new map two revisions of one file imply."""

    def test_an_insertion_shifts_every_line_below_it(self):
        got = cdc.line_map_from_texts("a\nb\nc", "a\nNEW\nb\nc")
        self.assertEqual(got, {1: 1, 2: 3, 3: 4})

    def test_a_deleted_line_maps_to_nothing(self):
        got = cdc.line_map_from_texts("a\nb\nc", "a\nc")
        self.assertEqual(got, {1: 1, 2: None, 3: 2})

    def test_an_in_place_reword_keeps_line_identity(self):
        # A comment reworded on the spot did not move; its citation must not either.
        got = cdc.line_map_from_texts("a\nold text\nc", "a\nnew text\nc")
        self.assertEqual(got, {1: 1, 2: 2, 3: 3})

    def test_a_replacement_that_changes_length_maps_to_nothing(self):
        # Two lines became three: no line inside the block has an identity to carry, so
        # the map says so instead of guessing.
        got = cdc.line_map_from_texts("a\nx\ny\nz", "a\np\nq\nr\ns")
        self.assertEqual(got[1], 1)
        self.assertEqual([got[2], got[3], got[4]], [None, None, None])


class ShiftLookupTest(unittest.TestCase):
    """How a SPARSE map speaks for the lines between its pins."""

    def test_an_exact_pin_wins(self):
        self.assertEqual(cdc.shift_lookup({100: 104})(100), 104)

    def test_a_line_between_two_agreeing_pins_inherits_their_shift(self):
        self.assertEqual(cdc.shift_lookup({100: 104, 200: 204})(150), 154)

    def test_a_line_between_two_disagreeing_pins_has_no_answer(self):
        # An edit lands between the pins, so the citation straddles two shifts. There is
        # no derivable answer and inventing one is how a re-pin corrupts a good citation.
        self.assertIsNone(cdc.shift_lookup({100: 104, 200: 209})(150))

    def test_the_tail_below_the_last_pin_carries_its_shift(self):
        self.assertEqual(cdc.shift_lookup({100: 104})(500), 504)

    def test_the_head_above_a_shifted_pin_has_no_answer(self):
        # The edit that moved line 100 may sit above line 50 or below it; unknowable.
        self.assertIsNone(cdc.shift_lookup({100: 104})(50))

    def test_the_head_above_an_unmoved_pin_is_unmoved(self):
        self.assertEqual(cdc.shift_lookup({100: 100})(50), 50)


class RepinRangeTest(unittest.TestCase):
    """`file:NNN-MMM` — the spelling that left `graph.cpp:1118-1114` behind."""

    MAP = {GRAPH: {1113: 1117, 1114: 1118, 1118: 1122}}

    def test_both_endpoints_of_a_range_move(self):
        self.assertEqual(repinned("`graph.cpp:1113-1118`", self.MAP), "`graph.cpp:1117-1122`")

    def test_the_far_endpoint_is_reported_as_a_move_of_its_own(self):
        self.assertIn((GRAPH, 1118, 1122), moved("`graph.cpp:1113-1118`", self.MAP))

    def test_a_range_is_never_left_inverted(self):
        # The head moves +4 and the tail is a fixed point (its anchor still resolves):
        # rewriting the head alone would emit `1118-1116`. Refuse the whole spec instead.
        partial = {GRAPH: {1114: 1118, 1116: 1116}}
        self.assertEqual(repinned("`graph.cpp:1114-1116`", partial), "`graph.cpp:1114-1116`")
        self.assertEqual(held("`graph.cpp:1114-1116`", partial), [(GRAPH, 1116)])

    def test_an_implausible_span_moves_neither_end(self):
        # `:12-900` may be a hyphenated word beside a citation rather than a 900-line
        # span, and the re-pin cannot tell. Moving the head alone is what produced
        # `graph.cpp:755-806` on the first live run — a correct head over a stale tail,
        # which reads as re-pinned. Hold both ends and name it.
        self.assertEqual(repinned("`graph.cpp:12-900`", {GRAPH: {12: 16}}), "`graph.cpp:12-900`")
        self.assertEqual(held("`graph.cpp:12-900`", {GRAPH: {12: 16}}), [(GRAPH, 12)])

    def test_a_trailing_hyphen_fragment_survives_the_rewrite(self):
        # `[\d,\-]+` is greedy, so unbackticked prose like `graph.cpp:12-20-style` is
        # captured with the spec `12-20-`. Rebuilding the range as f"{lo}-{hi}" alone
        # drops that trailing hyphen and silently edits the sentence to `16-24style`.
        # The single-line branch already preserves its tail; the range branch must too.
        m = {GRAPH: {12: 16, 20: 24}}
        self.assertEqual(repinned("graph.cpp:12-20-style", m), "graph.cpp:16-24-style")

    def test_a_bare_trailing_hyphen_survives_the_rewrite(self):
        m = {GRAPH: {12: 16, 20: 24}}
        self.assertEqual(repinned("graph.cpp:12-20-", m), "graph.cpp:16-24-")


class RepinCommaContinuationTest(unittest.TestCase):
    """`transport_ws.hpp:181,339` — a second line number after a comma."""

    MAP = {WS: {181: 185, 339: 343}}

    def test_every_element_of_a_comma_list_moves(self):
        self.assertEqual(repinned("WebSocket server and client (`transport_ws.hpp:181,339`), CAN", self.MAP),
                         "WebSocket server and client (`transport_ws.hpp:185,343`), CAN")

    def test_the_continuation_element_is_reported_as_a_move(self):
        self.assertIn((WS, 339, 343), moved("`transport_ws.hpp:181,339`", self.MAP))

    def test_an_unmappable_continuation_holds_the_whole_element(self):
        one_sided = {WS: {181: 185, 339: None}}
        self.assertEqual(repinned("`transport_ws.hpp:181,339`", one_sided), "`transport_ws.hpp:185,339`")
        self.assertEqual(held("`transport_ws.hpp:181,339`", one_sided), [(WS, 339)])


class RepinBareContinuationTest(unittest.TestCase):
    """A bare `` `:NNN` `` inheriting its file from the citation before it."""

    MAP = {GRAPH: {2305: 2309, 2405: 2409}}
    # CONTEXT.md, verbatim shape.
    TEXT = "both require its absence (`core/src/graph.cpp:2305`, `:2405`) and a `[*]` read"

    def test_a_bare_continuation_moves_with_its_inherited_file(self):
        self.assertEqual(repinned(self.TEXT, self.MAP),
                         "both require its absence (`core/src/graph.cpp:2309`, `:2409`) and a `[*]` read")

    def test_the_bare_form_is_reported_under_the_file_it_inherited(self):
        self.assertIn((GRAPH, 2405, 2409), moved(self.TEXT, self.MAP))

    def test_a_bare_continuation_after_a_cited_page_is_left_alone(self):
        # The page citation breaks inheritance, so `:2405` means line 2405 of the PAGE.
        text = "`graph.cpp:2305` ... `docs/reference/07.md:79` ... `:2405`"
        self.assertEqual(repinned(text, self.MAP),
                         "`graph.cpp:2309` ... `docs/reference/07.md:79` ... `:2405`")


class RepinBasenameTest(unittest.TestCase):
    """A citation with no directory — how the module pages write them."""

    def test_a_basename_only_citation_moves(self):
        self.assertEqual(repinned("(`transport_can.hpp:371`), QUIC", {CAN: {371: 375}}),
                         "(`transport_can.hpp:375`), QUIC")

    def test_the_basename_spelling_survives_the_rewrite(self):
        # Only the number is spliced. Expanding the shorthand to a full path would churn
        # every line of prose the re-pin touches.
        self.assertNotIn("core/include", repinned("`transport_can.hpp:371`", {CAN: {371: 375}}))

    def test_both_spellings_of_the_same_line_move_together(self):
        text = "`transport_can.hpp:371` and `core/include/libtracer/transport_can.hpp:371`"
        self.assertEqual(repinned(text, {CAN: {371: 375}}),
                         "`transport_can.hpp:375` and `core/include/libtracer/transport_can.hpp:375`")


class RepinOnePassTest(unittest.TestCase):
    """The rule that turned 51 stale citations into 60 when it was broken."""

    def test_a_chained_map_moves_each_citation_exactly_once(self):
        # 1114 -> 1118 and 1118 -> 1122 in the SAME map. A pass that re-read its own
        # output would take the first citation to 1122 via 1118.
        chained = {GRAPH: {1114: 1118, 1118: 1122}}
        self.assertEqual(repinned("`graph.cpp:1114` and `graph.cpp:1118`", chained),
                         "`graph.cpp:1118` and `graph.cpp:1122`")

    def test_a_second_run_over_already_repinned_text_is_reported_not_silent(self):
        # Re-running a map the docs already absorbed is the operator error the one-pass
        # rule cannot prevent; what it can do is keep the damage to one hop, not two.
        chained = {GRAPH: {1114: 1118, 1118: 1122}}
        once = repinned("`graph.cpp:1114`", chained)
        self.assertEqual(once, "`graph.cpp:1118`")
        self.assertEqual(repinned(once, chained), "`graph.cpp:1122`")


class RepinAnchorTableTest(unittest.TestCase):
    """The anchor table is a citation surface too — in BOTH quote styles."""

    GRAMMAR = "core/include/libtracer/grammar.hpp"
    MAP = {GRAMMAR: {290: 294, 388: 392}}
    TABLE = (
        'ANCHORS = [\n'
        '    ("core/include/libtracer/grammar.hpp:290", "receiver-resource depth bound"),\n'
        "    ('core/include/libtracer/grammar.hpp:388',\n"
        "     '* call stack: the walk keeps'),\n"
        ']\n'
    )

    def test_a_double_quoted_entry_moves(self):
        # The 2026-08-06 miss: three `grammar.hpp` anchors survived a sweep that only
        # matched `('...`, because the table carries 92 entries written `("...`.
        self.assertIn('("core/include/libtracer/grammar.hpp:294"',
                      cdc.repin_anchor_table(self.TABLE, self.MAP)[0])

    def test_a_single_quoted_entry_moves(self):
        self.assertIn("('core/include/libtracer/grammar.hpp:392'",
                      cdc.repin_anchor_table(self.TABLE, self.MAP)[0])

    def test_both_styles_are_reported_as_moves(self):
        self.assertEqual(sorted(cdc.repin_anchor_table(self.TABLE, self.MAP)[1]),
                         [(self.GRAMMAR, 290, 294), (self.GRAMMAR, 388, 392)])

    def test_a_citation_inside_an_anchor_TEXT_is_left_alone(self):
        # Element 1 is the substring the cited line must contain — quoting a `file:line`
        # there is quoting SOURCE, and rewriting it would break the anchor outright.
        entry = '    ("core/src/graph.cpp:163", "// see core/src/graph.cpp:163 for the key"),\n'
        out, moves, _ = cdc.repin_anchor_table(entry, {GRAPH: {163: 167}})
        self.assertIn('("core/src/graph.cpp:167"', out)
        self.assertIn("// see core/src/graph.cpp:163 for the key", out)
        self.assertEqual(moves, [(GRAPH, 163, 167)])

    def test_an_unmapped_file_is_untouched(self):
        self.assertEqual(cdc.repin_anchor_table(self.TABLE, {})[0], self.TABLE)


class AnchorLineMapsTest(unittest.TestCase):
    """Where the map comes from when no revision is named: what the gate can PROVE moved."""

    ANCHORS = [("src/a.cpp:2", "alpha_marker"), ("src/a.cpp:4", "beta_marker")]

    def maps_for(self, body):
        with tempfile.TemporaryDirectory() as root:
            write_tree(root, "src/a.cpp", body)
            return cdc.anchor_line_maps(self.ANCHORS, cdc.pathlib.Path(root))

    def test_an_anchor_that_still_resolves_is_a_fixed_point(self):
        maps, notes = self.maps_for("h\nalpha_marker\nmid\nbeta_marker\ntail\n")
        self.assertEqual(maps["src/a.cpp"], {2: 2, 4: 4})
        self.assertEqual(notes, [])

    def test_a_drifted_anchor_yields_its_move(self):
        maps, _ = self.maps_for("new\nh\nalpha_marker\nmid\nbeta_marker\ntail\n")
        self.assertEqual(maps["src/a.cpp"], {2: 3, 4: 5})

    def test_an_anchor_above_the_edit_is_not_dragged_by_one_below_it(self):
        # The file gains two lines in the MIDDLE. `:2` still resolves and must stay; `:4`
        # moved. This is why the check is "does it already resolve", not "does the new
        # line hold the old line's text" — the latter is vacuous under a uniform shift,
        # where it holds for every line whether or not that line needed moving.
        maps, _ = self.maps_for("h\nalpha_marker\nmid\nx\ny\nbeta_marker\ntail\n")
        self.assertEqual(maps["src/a.cpp"], {2: 2, 4: 6})

    def test_a_vanished_anchor_yields_a_note_and_no_mapping(self):
        maps, notes = self.maps_for("h\nalpha_marker\nmid\ntail\n")
        self.assertEqual(maps["src/a.cpp"], {2: 2})
        self.assertEqual(len(notes), 1)
        self.assertIn("anchor GONE", notes[0])

    def test_an_ambiguous_anchor_yields_a_note_and_no_mapping(self):
        maps, notes = self.maps_for("h\nalpha_marker\nbeta_marker\nx\nbeta_marker\n")
        self.assertNotIn(4, maps["src/a.cpp"])
        self.assertIn("ambiguous", notes[0])


class HistoricalGenreTest(unittest.TestCase):
    """A dated record cites the tree AS IT STOOD; a re-pin must not move it forward."""

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


class CitationIndexTest(unittest.TestCase):
    """A DRIFT has to name the page that cites it, in whatever spelling that page used."""

    def test_the_index_names_the_citing_page(self):
        index = cdc.citation_index([("docs/modules/transport.md", "`transport_ws.hpp:181,339`")], FILEMAP)
        self.assertEqual(index[f"{WS}:339"], ["docs/modules/transport.md"])

    def test_a_bare_continuation_is_indexed_under_the_file_it_inherited(self):
        # This is the case that used to read as an orphaned anchor: no page contains the
        # literal text `graph.cpp:2405`, so grepping for it found nothing.
        index = cdc.citation_index([("CONTEXT.md", "(`core/src/graph.cpp:2305`, `:2405`)")], FILEMAP)
        self.assertEqual(index[f"{GRAPH}:2405"], ["CONTEXT.md"])

    def test_two_pages_citing_one_line_are_both_named(self):
        index = cdc.citation_index([("a.md", "`graph.cpp:12`"), ("b.md", "`core/src/graph.cpp:12`")], FILEMAP)
        self.assertEqual(index[f"{GRAPH}:12"], ["a.md", "b.md"])
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


FOOTPRINT = "tools/cortexm0_footprint.py"
TESTS_CMAKE = "core/tests/CMakeLists.txt"
CORE_CI = ".github/workflows/core-ci.yml"


class CitableNonSourcePathTest(unittest.TestCase):
    """The non-source allowlist (#1052).

    A build file is not a source, so a citation into one used to match nothing here and
    could not be pinned. `docs/modules/segment.md` spelled `LIBTRACER_NO_ATOMIC` in three
    places and the gate checked only the header one, while the footprint-script and
    test-CMake ones had both drifted onto unrelated lines.
    """

    def test_an_enrolled_tooling_path_is_a_citation(self):
        self.assertEqual(locs(f"`{FOOTPRINT}:101`"), {f"{FOOTPRINT}:101"})

    def test_an_enrolled_build_path_reads_a_comma_list_and_a_range(self):
        self.assertEqual(locs(f"`{TESTS_CMAKE}:1055,1068-1069`"),
                         {f"{TESTS_CMAKE}:1055", f"{TESTS_CMAKE}:1068", f"{TESTS_CMAKE}:1069"})

    def test_an_enrolled_path_does_NOT_become_the_running_file(self):
        # The knob table walks `config.hpp.in` in bare refs with build-file citations in
        # between. Enrolment must not turn one of those asides into the running file.
        self.assertEqual(locs(f"`config.hpp.in:86` | `{TESTS_CMAKE}:1055` |\n| `:109` |"),
                         {"core/include/libtracer/config.hpp.in:86",
                          f"{TESTS_CMAKE}:1055",
                          "core/include/libtracer/config.hpp.in:109"})

    def test_an_unenrolled_tooling_path_is_still_not_a_citation(self):
        # Enrolment is an allowlist, not `.py`: covering build files wholesale is a
        # maintainer's call and is not what this table answers.
        self.assertEqual(locs("`tools/sync-version.py:42`"), set())

    def test_a_repin_leaves_an_enrolled_path_alone(self):
        # `revision_line_maps` derives maps from sources only, so there is no map to move
        # these by. Leaving doc and anchor in step is what makes the gate's DRIFT report
        # the whole fix rather than half of it.
        self.assertEqual(repinned(f"`{FOOTPRINT}:101`", {FOOTPRINT: {101: 120}}),
                         f"`{FOOTPRINT}:101`")

    def test_a_repin_REPORTS_the_enrolled_path_it_declined(self):
        # #1095 acceptance: `--repin` either handles what is newly enrolled or says
        # plainly that it will not. It declines (above); this is the saying-so.
        _, _, held = cdc.repin_document(f"`{FOOTPRINT}:101`", {FOOTPRINT: {101: 120}}, FILEMAP)
        self.assertEqual(held, [(FOOTPRINT, "101")])

    def test_a_partial_path_resolves_to_the_enrolled_file(self):
        # `integrations/esp-idf/README.md` cites its own component as
        # `libtracer/CMakeLists.txt:172`. An exact-path allowlist reads that as nothing.
        self.assertEqual(locs("`libtracer/CMakeLists.txt:172`"),
                         {"integrations/esp-idf/libtracer/CMakeLists.txt:172"})

    def test_a_bare_basename_resolves_to_the_enrolled_file(self):
        # `tests/testbed/README.md` cites the mesh driver by basename alone.
        self.assertEqual(locs("`mesh-testbed.test.mjs:24-25`"),
                         {"bindings/typescript/packages/client/test/mesh-testbed.test.mjs:24",
                          "bindings/typescript/packages/client/test/mesh-testbed.test.mjs:25"})

    def test_a_spelling_naming_two_enrolled_paths_resolves_to_neither(self):
        # `CMakeLists.txt` is the basename of four enrolled paths. Guessing one is the
        # failure mode the source resolver already refuses.
        self.assertEqual(locs("`CMakeLists.txt:172`"), set())


class UnverifiableCitationTest(unittest.TestCase):
    """A line number in a file the gate cannot read is an ERROR, not silence (#1095).

    The gate verified only `SOURCE_SUFFIXES`, so a line-numbered citation of anything else
    exited 0 whether it resolved or not. #1088 shifted `core-ci.yml` 18 lines under a
    design page citing `:95-106` for the ThreadSanitizer job; the range came to name a
    different job's matrix, the gate stayed green, and the PR read that silence as "no
    cited file shifted lines".
    """

    def test_an_ambiguous_non_source_basename_is_reported_not_silently_accepted(self):
        """The same false green, one step earlier — the case the first fix left open.

        `unverifiable_citations` reported only when the basename resolved to exactly ONE
        tree file. An AMBIGUOUS one resolves to none, so it fell through and was accepted
        in silence: `CMakeLists.txt:172` has 15 candidate carriers in this repo and kept
        the gate at exit 0, which is verbatim the thing #1095 exists to stop.
        """
        found = cdc.unverifiable_citations("built at `CMakeLists.txt:172`")
        self.assertEqual(len(found), 1, found)
        self.assertIn("ambiguous", found[0])
        self.assertIn("CMakeLists.txt:172", found[0])

    def test_an_unresolvable_basename_is_still_NOT_reported(self):
        """The one stated exemption has to survive the fix above.

        A token naming no file in the tree is an address or something outside the repo;
        reporting it would make every `127.0.0.1:8080` in the docs an error.
        """
        self.assertEqual(cdc.unverifiable_citations("reach it at `127.0.0.1:8080`"), [])

    def test_a_line_cited_in_an_unenrolled_real_file_is_reported(self):
        found = cdc.unverifiable_citations("see `.github/workflows/quic.yml:12`")
        self.assertEqual(len(found), 1, found)
        self.assertIn("cannot verify", found[0])

    def test_the_report_names_the_two_ways_out(self):
        found = cdc.unverifiable_citations("`.github/workflows/quic.yml:12`")
        self.assertIn("CITABLE_NON_SOURCE_PATHS", found[0])
        self.assertIn("drop the line number", found[0])

    def test_an_enrolled_path_is_not_reported(self):
        self.assertEqual(cdc.unverifiable_citations(f"`{CORE_CI}:113-124`"), [])

    def test_a_source_file_is_not_reported(self):
        self.assertEqual(cdc.unverifiable_citations("`core/src/graph.cpp:956`"), [])

    def test_a_cited_markdown_page_is_not_reported(self):
        # A doc-to-doc citation is out of scope: `docs/reference/07-host-embedding.md:79`
        # is a pointer into prose, not into code this gate can anchor.
        self.assertEqual(cdc.unverifiable_citations("`docs/reference/00-overview.md:12`"), [])

    def test_a_host_and_port_is_not_a_citation(self):
        # The discriminator is "does this name a real file", precisely so the testbed
        # README's `127.0.0.1:47301` and a `wss://robot.local:9000` URL stay quiet. A rule
        # that rejected the FORM alone would fire on both.
        self.assertEqual(cdc.unverifiable_citations("bind `127.0.0.1:47301` and "
                                                    "`wss://robot.local:9000`"), [])

    def test_a_file_outside_the_repo_is_not_reported(self):
        self.assertEqual(cdc.unverifiable_citations("`/etc/systemd/system/foo.service:12`"), [])

    def test_a_citation_with_no_line_number_is_not_reported(self):
        # A bare path is a pointer, not an anchor — it cannot rot onto another line.
        self.assertEqual(cdc.unverifiable_citations("`.github/workflows/quic.yml`"), [])


class UnverifiableCitationGateTest(unittest.TestCase):
    """That `main` actually RUNS the check — the same bug shape, one level up.

    `unverifiable_citations` can be perfect and the gate still exit 0 if nothing calls it.
    Reverting the call site alone left every other test in this file passing, which is
    precisely the "green while measuring nothing" failure #1095 exists to end. These
    assert on the gate's OUTPUT rather than its exit code: a stand-in doc set makes every
    anchor read as uncited, so the exit code is 1 either way and proves nothing.
    """

    @contextlib.contextmanager
    def _gate_over(self, relative_dir, body):
        """Run `main` over a single throwaway doc at `relative_dir`, yielding its output."""
        directory = os.path.join(REPO, relative_dir)
        os.makedirs(directory, exist_ok=True)
        doc = os.path.join(directory, "zz-1095-probe.md")
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

    def test_the_gate_reports_an_unverifiable_citation(self):
        with self._gate_over("docs/design", "see `.github/workflows/quic.yml:12`\n") as out:
            self.assertIn("cannot verify", out)
            self.assertIn("quic.yml:12", out)

    def test_the_gate_leaves_a_dated_record_alone(self):
        # An ADR cites the tree AS IT STOOD; demanding today's line be verifiable would
        # demand rewriting the record.
        with self._gate_over("docs/adr", "see `.github/workflows/quic.yml:12`\n") as out:
            self.assertNotIn("cannot verify", out)


class EnrolledPathsArePinnedTest(unittest.TestCase):
    """Enrolment is only worth having if the citations it exposes are actually pinned.

    Reads the real doc set, so it fails three separate ways: if a pin is dropped, if a
    citing page is reverted to a line no pin covers, or if a new citation into an enrolled
    file lands without one. That last case is the whole failure mode of #1052 — a citation
    sitting beside a verified one and reading as if it were verified too.
    """

    def test_every_citation_into_an_enrolled_path_has_a_pinned_head(self):
        pinned = {entry[0] for entry in cdc.ANCHORS}
        filemap = cdc.source_map()
        unpinned = []
        for doc in cdc.all_docs():
            try:
                text = doc.read_text()
            except (OSError, UnicodeDecodeError):
                continue
            rel = os.path.relpath(str(doc), REPO).replace(os.sep, "/")
            # The dated genres are exempt, for the reason the tool never anchors them: an
            # ADR cites the tree AS IT STOOD. `docs/adr/0071` cites
            # `integrations/esp-idf/libtracer/CMakeLists.txt:85-86` and that citation has
            # already drifted onto an unrelated comment — pinning it would mean editing
            # the record every time the component's source list moves. Before #1095 no
            # ADR cited either enrolled path, so this exemption had nothing to exclude.
            if cdc.is_historical(rel):
                continue
            spans, _ = cdc.citation_spans(text, filemap)
            unpinned += [f"{rel} cites {p}:{lo}" for p, lo, _ in spans
                         if p in cdc.CITABLE_NON_SOURCE_PATHS and f"{p}:{lo}" not in pinned]
        self.assertEqual(unpinned, [], "an enrolled non-source citation with no pin")

    def test_every_enrolled_path_carries_at_least_one_pin(self):
        pinned_paths = {entry[0].rsplit(":", 1)[0] for entry in cdc.ANCHORS}
        for path in cdc.CITABLE_NON_SOURCE_PATHS:
            self.assertIn(path, pinned_paths)

    def test_every_enrolled_path_exists(self):
        # Enrolment is hand-maintained, so a rename can leave a path behind. An anchor on
        # a missing file fails the gate loudly, but the ALLOWLIST entry alone would not.
        for path in cdc.CITABLE_NON_SOURCE_PATHS:
            self.assertTrue(os.path.isfile(os.path.join(REPO, path)), path)

    def test_no_live_doc_carries_an_unverifiable_citation(self):
        # The #1095 gate, over the real doc set. The dated genres are exempt for the same
        # reason they are never anchored: an ADR cites the tree AS IT STOOD.
        offenders = []
        for doc in cdc.all_docs():
            try:
                text = doc.read_text()
            except (OSError, UnicodeDecodeError):
                continue
            rel = os.path.relpath(str(doc), REPO).replace(os.sep, "/")
            if cdc.is_historical(rel):
                continue
            offenders += [f"{rel}: {e}" for e in cdc.unverifiable_citations(text)]
        self.assertEqual(offenders, [])

    def test_the_gate_workflow_runs_when_an_enrolled_path_changes(self):
        # A pin only gates if the job fires on the commit that moved the anchor. The
        # workflow's `paths:` is an allowlist, and `tools/**` is not in it: a script
        # outside it could shift a pinned line with the gate never running (the failure
        # shape `quic.yml`'s filter has already produced in this repo).
        wf = os.path.join(REPO, ".github", "workflows", "doc-citations.yml")
        with open(wf) as fh:
            entries = re.findall(r'^\s*-\s*"([^"]+)"\s*$', fh.read(), re.MULTILINE)
        for path in cdc.CITABLE_NON_SOURCE_PATHS:
            covered = any(e == path or (e.endswith("/**") and path.startswith(e[:-2]))
                          for e in entries)
            self.assertTrue(covered, f"{path} is pinned but the gate never runs on it")


class UnanchoredCitationTest(unittest.TestCase):
    """A cited span no anchor pins (#1243) — the hole a NEW citation fell through.

    The verify pass walks the pin list, so it can only ever check citations that were
    already pinned. The citation a PR *introduces* has no pin, was therefore never read,
    and pointed wherever the author typed — at a comment, at a blank line, at a transposed
    line number. Five did exactly that under a green `OK 382` on 2026-08-13.

    The rule is the mirror of the dead-pin check the gate already runs ("pinned here but no
    doc cites it any more"): a citation is pinned, or it is refused.
    """

    def unanchored(self, text, pinned):
        return cdc.unanchored_citations(text, set(pinned), FILEMAP)

    def test_a_cited_line_with_no_anchor_is_reported(self):
        out = self.unanchored("see `graph.cpp:1276`", set())
        self.assertEqual(len(out), 1)
        self.assertIn(f"{GRAPH}:1276", out[0])

    def test_the_report_asks_for_the_anchor(self):
        # The remedy has to be in the message: the author's next move is "add an anchor",
        # not "go read the gate's source to find out what it wants".
        out = self.unanchored("see `graph.cpp:1276`", set())
        self.assertIn("add an anchor", out[0])

    def test_a_pinned_line_is_not_reported(self):
        self.assertEqual(self.unanchored("see `graph.cpp:1276`", {f"{GRAPH}:1276"}), [])

    def test_an_anchor_anywhere_INSIDE_a_cited_range_covers_it(self):
        # The table's convention is to pin the most distinctive line within a cited range,
        # which is often neither endpoint (a doc that points at a `/**` means the block).
        self.assertEqual(self.unanchored("`graph.cpp:1270-1280`", {f"{GRAPH}:1275"}), [])

    def test_a_range_pinned_nowhere_is_reported_by_its_full_span(self):
        out = self.unanchored("`graph.cpp:1270-1280`", {f"{GRAPH}:1300"})
        self.assertIn(f"{GRAPH}:1270-1280", out[0])

    def test_a_bare_continuation_is_checked_under_the_file_it_inherited(self):
        # The spelling that hides a citation from a grep also used to hide it from any
        # coverage argument made by hand.
        out = self.unanchored("(`core/src/graph.cpp:12`, `:99`)", {f"{GRAPH}:12"})
        self.assertEqual(len(out), 1)
        self.assertIn(f"{GRAPH}:99", out[0])

    def test_an_unresolvable_name_is_not_reported(self):
        # It pins no location, so there is nothing to anchor. @ref unverifiable_citations
        # is what decides whether that silence is legitimate.
        self.assertEqual(self.unanchored("`nowhere.cpp:12`", set()), [])

    def test_a_cited_markdown_page_is_not_reported(self):
        self.assertEqual(self.unanchored("`docs/reference/07-host-embedding.md:79`", set()), [])


class UnanchoredCitationGateTest(unittest.TestCase):
    """That `main` RUNS the coverage check — the same shape as the #1095 gate test.

    `unanchored_citations` can be perfect and the gate still accept an unpinned citation if
    nothing calls it, which is exactly the failure #1243 reports. Asserts on OUTPUT, not on
    the exit code: over a stand-in doc set every real anchor reads as uncited, so the exit
    code is 1 either way and proves nothing.
    """

    @contextlib.contextmanager
    def _gate_over(self, relative_dir, body, anchors=None):
        """Run `main` over one throwaway doc (optionally with a stand-in ANCHORS table)."""
        directory = os.path.join(REPO, relative_dir)
        os.makedirs(directory, exist_ok=True)
        doc = os.path.join(directory, "zz-1243-probe.md")
        with open(doc, "w") as fh:
            fh.write(body)
        real_all_docs, real_anchors, buf = cdc.all_docs, cdc.ANCHORS, io.StringIO()
        try:
            cdc.all_docs = lambda: [pathlib.Path(doc)]
            if anchors is not None:
                cdc.ANCHORS = anchors
            with contextlib.redirect_stdout(buf):
                cdc.main([])
            yield buf.getvalue()
        finally:
            cdc.all_docs, cdc.ANCHORS = real_all_docs, real_anchors
            os.remove(doc)

    # The exact hygiene-run shape: a citation introduced by a PR, landing on a COMMENT line
    # inside `graph.cpp`, with no anchor. Before #1243 this printed `OK` and shipped.
    NEW_CITATION = "the fold is at `core/src/graph.cpp:1273`\n"
    RIGHT_ANCHOR = [("core/src/graph.cpp:1273",
                     "// Fold BEFORE dispatching: these deliveries were abandoned inside the")]

    def test_the_gate_reports_a_citation_no_anchor_pins(self):
        with self._gate_over("docs/design", self.NEW_CITATION) as out:
            self.assertIn("pinned by no ANCHORS entry", out)
            self.assertIn("core/src/graph.cpp:1273", out)

    def test_the_report_names_the_citing_doc(self):
        with self._gate_over("docs/design", self.NEW_CITATION) as out:
            self.assertIn("docs/design/zz-1243-probe.md", out)

    def test_the_same_citation_WITH_its_anchor_passes(self):
        # The contributor rule the fix creates: adding a citation means adding its anchor
        # in the same PR. With the anchor present the coverage complaint is gone.
        with self._gate_over("docs/design", self.NEW_CITATION, self.RIGHT_ANCHOR) as out:
            self.assertNotIn("pinned by no ANCHORS entry", out)

    def test_an_anchor_pointing_at_the_WRONG_line_still_fails(self):
        # Coverage is not a rubber stamp: the anchor is verified against the cited line's
        # text, so "add any anchor" is not a way past the gate.
        wrong = [("core/src/graph.cpp:1273", "this text is not on that line")]
        with self._gate_over("docs/design", self.NEW_CITATION, wrong) as out:
            self.assertIn("DRIFT", out)

    def test_the_gate_leaves_a_dated_record_alone(self):
        # An ADR cites the tree AS IT STOOD, and is never anchored; demanding coverage
        # there would demand rewriting the record on every refactor.
        with self._gate_over("docs/adr", self.NEW_CITATION) as out:
            self.assertNotIn("pinned by no ANCHORS entry", out)


class EveryLiveCitationIsPinnedTest(unittest.TestCase):
    """The coverage rule over the REAL doc set — the acceptance criterion, mechanized.

    Sibling of `EnrolledPathsArePinnedTest`, widened from the nine enrolled paths to every
    citation the living docs make. It fails if a citation lands without its anchor, which is
    the whole of #1243.
    """

    def test_no_live_doc_cites_a_span_no_anchor_pins(self):
        pinned = {entry[0] for entry in cdc.ANCHORS}
        filemap = cdc.source_map()
        offenders = []
        for doc in cdc.all_docs():
            try:
                text = doc.read_text()
            except (OSError, UnicodeDecodeError):
                continue
            rel = os.path.relpath(str(doc), REPO).replace(os.sep, "/")
            if cdc.is_historical(rel):
                continue
            offenders += [f"{rel}: {e}" for e in cdc.unanchored_citations(text, pinned, filemap)]
        self.assertEqual(offenders, [])


class RepinHoldVerdictTest(unittest.TestCase):
    """A HOLD is part of the verdict (#1243).

    `--repin` printed its holds and exited 0, so the rebase procedure that gated on the exit
    status read "held" as "done" and carried stale citations through a rebase — the same
    false green the verify pass had, one command over. It also must not cry wolf: a citation
    held in a file that did not move needs no attention, and a run that reddens on every
    clean tree is a run whose exit code stops being read.
    """

    @contextlib.contextmanager
    def _repin_over(self, maps, notes, body=""):
        """Run the `--repin` driver over one throwaway doc and a stand-in line map."""
        directory = os.path.join(REPO, "docs", "design")
        os.makedirs(directory, exist_ok=True)
        doc = os.path.join(directory, "zz-1243-repin-probe.md")
        with open(doc, "w") as fh:
            fh.write(body)
        real_all_docs, real_maps = cdc.all_docs, cdc.anchor_line_maps
        buf = io.StringIO()
        try:
            cdc.all_docs = lambda: [pathlib.Path(doc)]
            cdc.anchor_line_maps = lambda *a, **k: (maps, list(notes))
            with contextlib.redirect_stdout(buf):
                code = cdc.repin(None, False)
            yield code, buf.getvalue()
        finally:
            cdc.all_docs, cdc.anchor_line_maps = real_all_docs, real_maps
            os.remove(doc)

    def test_a_clean_run_exits_zero(self):
        with self._repin_over({GRAPH: {10: 10}}, []) as (code, out):
            self.assertEqual(code, 0)
            self.assertNotIn("HELD", out)

    def test_a_run_with_a_HOLD_exits_non_zero(self):
        # An anchor the map could not follow: the tool cannot re-pin what it cannot locate,
        # and the operator has to be told in a way a shell can see.
        with self._repin_over({}, ["src/a.cpp:2: anchor GONE — re-pin by hand"]) as (code, out):
            self.assertEqual(code, 1)
            self.assertIn("HELD", out)

    def test_the_summary_line_says_the_run_is_incomplete(self):
        with self._repin_over({}, ["src/a.cpp:2: anchor GONE — re-pin by hand"]) as (_, out):
            self.assertIn("INCOMPLETE", out)

    def test_an_unmappable_citation_in_a_MOVED_file_holds_the_run(self):
        # A span too wide for the re-pin's plausibility clamp, in a file whose anchors moved:
        # there IS a shift to apply and the tool refuses to guess it, so a human must.
        with self._repin_over({GRAPH: {10: 12}}, [], "`core/src/graph.cpp:33-77`\n") as (code, out):
            self.assertEqual(code, 1)
            self.assertIn(f"{GRAPH}:33", out)

    def test_the_same_citation_in_an_UNMOVED_file_is_silent(self):
        with self._repin_over({GRAPH: {10: 10}}, [], "`core/src/graph.cpp:33-77`\n") as (code, out):
            self.assertEqual(code, 0)
            self.assertNotIn("HOLD", out)


class AnchorHitsTest(unittest.TestCase):
    """The scope filter, including the `!` inversion #1271 added.

    `anchor_hits` is the one answer to "which lines does this anchor mean?", shared by the
    drift report, the re-pin line maps and the in-place ambiguity check. A positive scope
    selects candidates it appears ABOVE; a negated one selects the candidates it does not.
    The negated form is not a convenience: it is the ONLY way to select the earlier of two
    identical lines closer together than SCOPE_LINES, because a discriminator sitting
    between them is above the later one and above the earlier one's window too.
    """

    # The `transport_vertex.cpp:94` / `:98` shape, in miniature: one repeated call, one
    # discriminating line between the two occurrences.
    LINES = ["register(", "  DIAL", "register(", "  LISTEN"]

    def test_no_scope_means_every_match(self):
        self.assertEqual(cdc.anchor_hits(self.LINES, "register("), [1, 3])

    def test_a_positive_scope_selects_the_candidate_it_sits_above(self):
        self.assertEqual(cdc.anchor_hits(self.LINES, "register(", "DIAL"), [3])

    def test_a_negated_scope_selects_the_candidate_it_does_NOT_sit_above(self):
        self.assertEqual(cdc.anchor_hits(self.LINES, "register(", "!DIAL"), [1])

    def test_a_scope_matching_nothing_selects_nothing(self):
        self.assertEqual(cdc.anchor_hits(self.LINES, "register(", "absent"), [])

    def test_the_scope_window_ends_AT_the_candidate_line_inclusive(self):
        # Pre-existing semantics, pinned here because the `!` form inverts them and a
        # silent off-by-one would flip which of two adjacent candidates a negation selects:
        # the window is the SCOPE_LINES lines up to and INCLUDING the candidate, so a scope
        # that is the candidate's own text matches it.
        self.assertEqual(cdc.anchor_hits(self.LINES, "DIAL", "DIAL"), [2])
        self.assertEqual(cdc.anchor_hits(self.LINES, "DIAL", "!DIAL"), [])

    def test_the_real_table_has_no_ambiguous_anchor(self):
        # The acceptance criterion, over the shipped table: every anchor resolves to exactly
        # one line inside its scope. Fails the moment an entry is added whose text repeats.
        offenders = []
        for entry in cdc.ANCHORS:
            loc, anchor = entry[0], entry[1]
            scope = entry[2] if len(entry) > 2 else None
            path, lineno = loc.rsplit(":", 1)
            src = pathlib.Path(REPO) / path
            if not src.exists():
                continue
            lines = src.read_text().split("\n")
            hits = cdc.anchor_hits(lines, anchor, scope)
            if len(hits) > 1:
                offenders.append(f"{loc}: {anchor!r} matches {hits}")
        self.assertEqual(offenders, [])


class AmbiguousAnchorGateTest(unittest.TestCase):
    """That `main` REFUSES an in-place anchor whose text repeats in its scope (#1271).

    The blind spot this closes: multi-hit candidates were only ever computed on the DRIFT
    path, so an anchor that still resolved at its pinned line was accepted no matter how
    many other lines in its scope matched it too. That is how a mechanical re-pin aimed a
    paragraph about `deliver_remote`'s default full-route leg at its bound leg — faithful
    pin, wrong meaning, green gate.

    Asserts on OUTPUT, not on the exit code: over a stand-in doc set every real anchor reads
    as uncited, so the exit code is 1 either way and proves nothing.
    """

    # `if (!acl_allows(v, caller, acl_right_t::READ))` is the same statement in four ACL
    # gates of `graph.cpp` — a real repeated line, so this cannot pass on a fixture quirk.
    CITATION = "the read gate is at `core/src/graph.cpp:1163`\n"
    ANCHOR_TEXT = "if (!acl_allows(v, caller, acl_right_t::READ))"

    @contextlib.contextmanager
    def _gate_over(self, anchors):
        directory = os.path.join(REPO, "docs", "design")
        os.makedirs(directory, exist_ok=True)
        doc = os.path.join(directory, "zz-1271-probe.md")
        with open(doc, "w") as fh:
            fh.write(self.CITATION)
        real_all_docs, real_anchors, buf = cdc.all_docs, cdc.ANCHORS, io.StringIO()
        try:
            cdc.all_docs = lambda: [pathlib.Path(doc)]
            cdc.ANCHORS = anchors
            with contextlib.redirect_stdout(buf):
                cdc.main([])
            yield buf.getvalue()
        finally:
            cdc.all_docs, cdc.ANCHORS = real_all_docs, real_anchors
            os.remove(doc)

    def test_an_unscoped_repeated_anchor_is_reported(self):
        with self._gate_over([(f"{GRAPH}:1163", self.ANCHOR_TEXT)]) as out:
            self.assertIn("AMBIGUOUS", out)
            self.assertIn(f"{GRAPH}:1163", out)

    def test_the_report_says_how_to_fix_it(self):
        # The message has to name the remedy, because the anchor RESOLVES: nothing about the
        # cited line looks wrong, and "tighten the scope" is not guessable from a bare FAIL.
        with self._gate_over([(f"{GRAPH}:1163", self.ANCHOR_TEXT)]) as out:
            self.assertIn("Tighten the anchor text", out)

    def test_a_scope_that_disambiguates_passes(self):
        # `graph_t::read` owns the 1163 gate; the scope names it, and no other candidate
        # carries that signature above it.
        scoped = [(f"{GRAPH}:1163", self.ANCHOR_TEXT, "result_t<value_ref_t> graph_t::read(")]
        with self._gate_over(scoped) as out:
            self.assertNotIn("AMBIGUOUS", out)

    def test_an_anchor_whose_text_appears_once_is_never_flagged(self):
        # The no-false-positive half: an ordinary unique anchor needs no scope and must not
        # be asked for one, or the check costs more than the class it catches.
        unique = [(f"{GRAPH}:2489",
                   "result_t<void> graph_t::create_child(vertex_t* parent, const view_t& spec_value) {")]
        with self._gate_over(unique) as out:
            self.assertNotIn("AMBIGUOUS", out)


if __name__ == "__main__":
    unittest.main()
