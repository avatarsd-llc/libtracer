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

    def test_a_cost_section_without_a_latency_heading_is_a_hit(self):
        self.assert_only("concept-no-latency.md", "docs/concepts/x.md", "concept-sections")

    def test_the_concepts_index_is_navigation_not_a_concept(self):
        self.assertEqual(hits_of("concept-missing.md", "docs/concepts/index.md"), [])

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
            for fixture in ("line-length.md", "restatement.md"):  # the banner rule aside
                self.assertEqual([h for h in hits_of(fixture, rel) if h[0] != "annex-banner"], [], rel)

    def test_a_glossary_avoid_phrase_is_a_hit(self):
        self.assert_only("avoid-phrase.md", "docs/reference/x.md", "avoid-phrase")

    def test_the_hit_names_its_line(self):
        self.assertEqual(hits_of("avoid-phrase.md", "docs/reference/x.md"), [("avoid-phrase", 5)])


class SidebarOrderTest(unittest.TestCase):
    """The sidebar runs by reader intent, set by index.md's toctree order (#2381)."""

    @staticmethod
    def index(*captions):
        return "# libtracer\n\n" + "".join(
            f"```{{toctree}}\n:caption: {c}\n:hidden:\n\npage-{i}\n```\n\n"
            for i, c in enumerate(captions))

    def hits(self, text):
        return [(h.rule, h.line) for h in cds.lint_page("index.md", text, CTX)]

    def test_the_intent_order_draws_no_hit(self):
        self.assertEqual(self.hits(self.index(*cds.SIDEBAR)), [])

    def test_start_here_last_is_a_hit(self):
        order = list(cds.SIDEBAR[1:]) + [cds.SIDEBAR[0]]
        self.assertEqual([r for r, _ in self.hits(self.index(*order))], ["sidebar-order"])

    def test_a_section_outside_the_intent_list_is_a_hit(self):
        hits = self.hits(self.index(*cds.SIDEBAR, "Glossary"))
        self.assertEqual([r for r, _ in hits], ["sidebar-order"])

    def test_the_real_index_follows_the_intent_order(self):
        text = (pathlib.Path(REPO) / "index.md").read_text()
        self.assertEqual([h for h in cds.lint_page("index.md", text, CTX) if h.rule == "sidebar-order"], [])


class AnnexBannerTest(unittest.TestCase):
    """Exactly the annexes v1 §3 lists carry the one-line banner (#2385, ruling 5)."""

    BANNER = "> **Normative annex of [Tracer v1](../spec/v1.md#3-wire-format), per v1 §3.**\n"

    def hits(self, rel, text):
        return [(h.rule, h.line) for h in cds.lint_page(rel, text, CTX)]

    def test_an_annex_with_the_banner_under_its_title_draws_no_hit(self):
        page = "# Reference 01\n\n" + self.BANNER + "\nA receiver MUST, and a line long enough " + "x" * 90 + "\n"
        self.assertEqual(self.hits("docs/reference/01-data-format.md", page), [])

    def test_an_annex_without_the_banner_is_a_hit(self):
        self.assertEqual(self.hits("docs/reference/01-data-format.md", "# Reference 01\n\nText.\n"),
                         [("annex-banner", 1)])

    def test_the_banner_must_sit_under_the_title_not_further_down(self):
        page = "# Reference 01\n\nText.\n\n" + self.BANNER
        self.assertEqual(self.hits("docs/reference/01-data-format.md", page), [("annex-banner", 1)])

    def test_a_page_that_is_not_an_annex_must_not_carry_the_banner(self):
        page = "# Reference 02\n\n" + self.BANNER
        self.assertEqual(self.hits("docs/reference/02-graph-model.md", page), [("annex-banner", 3)])

    def test_every_real_annex_carries_the_banner_and_no_other_page_does(self):
        ctx, pages = cds.real_context()
        hits = [h for p in sorted(set(pages) | ctx.annexes)
                for h in cds.lint_page(p, (pathlib.Path(REPO) / p).read_text(), ctx)
                if h.rule == "annex-banner"]
        self.assertEqual(hits, [])


class DiagramStyleTest(unittest.TestCase):
    """No diagram styles a node on the page; colour comes from the vocabulary classes (#2373)."""

    def hits(self, text):
        return [(h.rule, h.line) for h in cds.lint_page("docs/reference/x.md", text, CTX)
                if h.rule == "diagram-style"]

    def diagram(self, fence, *body):
        alt = ["accTitle: A", "accDescr: B"] if fence == "{mermaid}" else []
        return "\n".join(["# P", "", "```" + fence, "flowchart LR", *alt, *body, "```", ""])

    def test_vocabulary_classes_draw_no_hit(self):
        page = self.diagram("{mermaid}", "A[a]:::vertex --> B[b]", "class B ring")
        self.assertEqual(self.hits(page), [])

    def test_style_classdef_and_linkstyle_in_a_directive_are_hits_on_their_lines(self):
        page = self.diagram("{mermaid}", "A --> B", "style A fill:#f00",
                            "  classDef hot fill:#f00", "linkStyle 0 stroke:#f00")
        self.assertEqual(self.hits(page), [("diagram-style", 8), ("diagram-style", 9),
                                           ("diagram-style", 10)])

    def test_a_bare_mermaid_fence_is_held_to_the_vocabulary_too(self):
        self.assertEqual(self.hits(self.diagram("mermaid", "A --> B", "style A fill:#f00")),
                         [("diagram-style", 6)])

    def test_the_words_in_another_code_block_or_in_prose_are_not_hits(self):
        page = "# P\n\nstyle A fill:#f00\n\n```text\nclassDef hot fill:#f00\n```\n"
        self.assertEqual(self.hits(page), [])


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
    """End to end on the real tree. Nothing here pins today's hit count: the committed baseline
    is the pin, and the gate below holds the tree to it (#2382)."""

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

    def test_the_real_tree_passes_the_gate_against_its_baseline(self):
        code, out = self.run_main("--gate")
        self.assertEqual(code, 0, out[-2000:])
        self.assertIn("gating", out)

    def test_the_docs_workflow_runs_the_lint_as_a_gate(self):
        wf = (pathlib.Path(REPO) / ".github" / "workflows" / "docs.yml").read_text()
        runs = [ln.strip() for ln in wf.splitlines() if "check_docs_style.py" in ln
                and ln.strip().startswith("run:")]
        self.assertEqual(runs, ["run: python3 tools/check_docs_style.py --gate"])


if __name__ == "__main__":
    unittest.main()
