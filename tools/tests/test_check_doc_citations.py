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
import os
import re
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


FOOTPRINT = "tools/cortexm0_footprint.py"
TESTS_CMAKE = "core/tests/CMakeLists.txt"


class CitableBuildPathTest(unittest.TestCase):
    """The build/tooling allowlist (#1052).

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
            spans, _ = cdc.citation_spans(text, filemap)
            unpinned += [f"{rel} cites {p}:{lo}" for p, lo, _ in spans
                         if p in cdc.CITABLE_BUILD_PATHS and f"{p}:{lo}" not in pinned]
        self.assertEqual(unpinned, [], "an enrolled build/tooling citation with no pin")

    def test_every_enrolled_path_carries_at_least_one_pin(self):
        pinned_paths = {entry[0].rsplit(":", 1)[0] for entry in cdc.ANCHORS}
        for path in cdc.CITABLE_BUILD_PATHS:
            self.assertIn(path, pinned_paths)

    def test_the_gate_workflow_runs_when_an_enrolled_path_changes(self):
        # A pin only gates if the job fires on the commit that moved the anchor. The
        # workflow's `paths:` is an allowlist, and `tools/**` is not in it: a script
        # outside it could shift a pinned line with the gate never running (the failure
        # shape `quic.yml`'s filter has already produced in this repo).
        wf = os.path.join(REPO, ".github", "workflows", "doc-citations.yml")
        with open(wf) as fh:
            entries = re.findall(r'^\s*-\s*"([^"]+)"\s*$', fh.read(), re.MULTILINE)
        for path in cdc.CITABLE_BUILD_PATHS:
            covered = any(e == path or (e.endswith("/**") and path.startswith(e[:-2]))
                          for e in entries)
            self.assertTrue(covered, f"{path} is pinned but the gate never runs on it")


if __name__ == "__main__":
    unittest.main()
