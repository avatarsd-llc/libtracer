#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the docs style lint (`tools/check_docs_style.py`, #2371).

The seam is the one #2374 names: small fixture pages. One page is conformant and must
draw no hit at all; every other fixture breaks exactly one rule and must draw a hit of
that rule and no other. The fixtures live in `tools/tests/fixtures/docs_style/` and are
linted against a stand-in repository (annex set, published-page set, files that exist,
avoid list), so a change to the real tree never moves these tests.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import contextlib
import io
import json
import os
import pathlib
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_docs_style as cds  # noqa: E402

FIXTURES = pathlib.Path(REPO) / "tools" / "tests" / "fixtures" / "docs_style"

# The stand-in repository every fixture is linted against.
CTX = cds.Context(
    annexes=frozenset({"docs/reference/01-data-format.md", "docs/reference/05-protocol-tlvs.md"}),
    published=lambda rel: rel.startswith("docs/reference/") or rel.startswith("docs/concepts/"),
    exists=lambda name: name in {
        "tests/conformance/vectors/v1/value/batch-time-base",  # a vector, by path
        "batch-time-base",                                     # the same vector, by name
        "batch_compose_test",                                  # a test, by stem
    },
    avoid=(("route handle", "path element"),),
)


def hits_of(fixture, rel):
    """The (rule, line) pairs the lint gives one fixture page, linted as `rel`."""
    text = (FIXTURES / fixture).read_text()
    return [(h.rule, h.line) for h in cds.lint_page(rel, text, CTX)]


class FixturePageTest(unittest.TestCase):
    """One conformant page; one page per rule, breaking exactly that rule."""

    def assert_only(self, fixture, rel, rule, count=1):
        got = hits_of(fixture, rel)
        self.assertEqual([r for r, _ in got], [rule] * count, got)

    def test_the_conformant_concept_page_draws_no_hit(self):
        self.assertEqual(hits_of("conformant.md", "docs/concepts/batching.md"), [])

    def test_a_diagram_without_title_and_description_is_a_hit(self):
        self.assert_only("alt-text.md", "docs/reference/x.md", "alt-text", count=2)

    def test_a_concept_page_missing_a_required_section_is_a_hit(self):
        self.assert_only("concept-missing.md", "docs/concepts/x.md", "concept-sections")

    def test_concept_sections_out_of_order_are_a_hit(self):
        self.assert_only("concept-order.md", "docs/concepts/x.md", "concept-sections")

    def test_the_concept_template_binds_only_concept_pages(self):
        self.assertEqual(hits_of("concept-missing.md", "docs/reference/x.md"), [])

    def test_a_normative_source_naming_an_rfc_is_a_hit(self):
        self.assert_only("normative-source.md", "docs/reference/x.md", "normative-source")

    def test_exercised_by_naming_a_missing_vector_is_a_hit(self):
        self.assert_only("exercised-by.md", "docs/reference/x.md", "exercised-by")

    def test_an_overlong_prose_line_is_a_hit(self):
        self.assert_only("line-length.md", "docs/reference/x.md", "line-length")

    def test_a_link_to_our_own_published_page_off_site_is_a_hit(self):
        self.assert_only("own-site-link.md", "docs/reference/x.md", "own-site-link", count=2)

    def test_a_must_outside_the_spec_is_a_restatement_hit(self):
        self.assert_only("restatement.md", "docs/reference/x.md", "restatement")

    def test_the_spec_is_left_alone(self):
        for rel in ("docs/reference/01-data-format.md", "docs/spec/v1.md", "docs/spec/rfcs/0001-x.md"):
            self.assertEqual(hits_of("line-length.md", rel), [], rel)
            self.assertEqual(hits_of("restatement.md", rel), [], rel)

    def test_a_glossary_avoid_phrase_is_a_hit(self):
        self.assert_only("avoid-phrase.md", "docs/reference/x.md", "avoid-phrase")

    def test_the_hit_names_its_line(self):
        self.assertEqual(hits_of("avoid-phrase.md", "docs/reference/x.md"), [("avoid-phrase", 5)])


class AnnexSetTest(unittest.TestCase):
    """The annex set is read from v1 §3, the only list of what is normative."""

    def test_the_annexes_are_the_bold_reference_bullets_of_section_3(self):
        v1 = (
            "## 2. Terms\n- **[docs/reference/99-not.md](../reference/99-not.md)**\n"
            "## 3. Wire format\n"
            "- **[docs/reference/01-data-format.md](../reference/01-data-format.md)** — frames\n"
            "  see [docs/reference/02-graph.md](../reference/02-graph.md) informatively\n"
            "- **[docs/reference/23-noise.md](../reference/23-noise.md)** — Noise\n"
            "## 4. Vectors\n"
        )
        self.assertEqual(cds.annexes_from_v1(v1),
                         {"docs/reference/01-data-format.md", "docs/reference/23-noise.md"})

    def test_the_real_v1_names_four_annexes(self):
        v1 = (pathlib.Path(REPO) / "docs" / "spec" / "v1.md").read_text()
        self.assertEqual(len(cds.annexes_from_v1(v1)), 4)


class AvoidListTest(unittest.TestCase):
    """The avoid list beside CONTEXT.md parses into (phrase, use-instead) pairs."""

    def test_each_bullet_is_one_phrase_and_its_replacement(self):
        text = "# Avoid\n\nprose\n\n- \"route handle\" → path element\n- \"COMPACT plane\" → chain delivery\n"
        self.assertEqual(cds.parse_avoid(text),
                         (("route handle", "path element"), ("COMPACT plane", "chain delivery")))

    def test_the_real_list_is_short_and_nonempty(self):
        pairs = cds.parse_avoid((pathlib.Path(REPO) / "CONTEXT-AVOID.md").read_text())
        self.assertTrue(1 <= len(pairs) <= 30, pairs)


class BaselineTest(unittest.TestCase):
    """Hits are counted per page and rule; only a count above the baseline is new."""

    def hit(self, path, rule, line):
        return cds.Hit(path, line, rule, "m")

    def test_a_hit_within_the_baseline_is_not_new(self):
        hits = [self.hit("a.md", "line-length", 3), self.hit("a.md", "line-length", 9)]
        self.assertEqual(cds.new_hits(hits, {"a.md": {"line-length": 2}}), [])

    def test_a_count_above_the_baseline_reports_the_page_and_rule(self):
        hits = [self.hit("a.md", "line-length", 3), self.hit("a.md", "line-length", 9)]
        self.assertEqual(cds.new_hits(hits, {"a.md": {"line-length": 1}}),
                         [("a.md", "line-length", 2, 1)])

    def test_a_rule_new_to_a_page_is_new(self):
        hits = [self.hit("a.md", "restatement", 3)]
        self.assertEqual(cds.new_hits(hits, {"a.md": {"line-length": 4}}),
                         [("a.md", "restatement", 1, 0)])

    def test_the_baseline_round_trips(self):
        hits = [self.hit("b.md", "alt-text", 1), self.hit("a.md", "alt-text", 2)]
        self.assertEqual(json.loads(cds.baseline_json(hits)), {"a.md": {"alt-text": 1},
                                                               "b.md": {"alt-text": 1}})


class RealTreeTest(unittest.TestCase):
    """End to end on the real tree. Nothing here pins today's hit count: a docs PR that adds a
    hit must not fail a unit test while the lint is report-only."""

    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cds.main(list(argv))
        return code, out.getvalue()

    def test_report_only_exits_zero(self):
        code, out = self.run_main()
        self.assertEqual(code, 0)
        self.assertIn("report-only", out)

    def test_gate_mode_fails_on_a_hit_outside_the_baseline(self):
        with tempfile.TemporaryDirectory() as d:
            empty = pathlib.Path(d) / "baseline.json"
            empty.write_text("{}\n")
            code, out = self.run_main("--gate", "--baseline", str(empty))
        self.assertEqual(code, 1)
        self.assertIn("NEW", out)


if __name__ == "__main__":
    unittest.main()
