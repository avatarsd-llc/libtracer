#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the diagram palette check (`tools/check_diagram_contrast.py`, #2378).

The palette is the visual vocabulary of #2374: one fill, stroke and text colour per class,
light and dark. These pin the WCAG arithmetic against known pairs, the parse of the CSS
blocks, and the verdicts: text on its fill meets AA (4.5:1), a stroke stands off its page
at 3:1, both themes name the same classes, and the two dark blocks agree.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import json
import os
import pathlib
import re
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_diagram_contrast as cdc  # noqa: E402


def css(light, dark, auto=None):
    """A palette stylesheet with the three marked blocks."""
    def block(marker, sel, vars_):
        body = "".join(f"  --lt-{k}: {v};\n" for k, v in vars_.items())
        return f"/* lt-palette: {marker} */\n{sel} {{\n{body}}}\n"
    return (block("light", "body", light) + block("dark", 'body[data-theme="dark"]', dark)
            + "@media (prefers-color-scheme: dark) {\n"
            + block("dark-auto", 'body:not([data-theme="light"])', dark if auto is None else auto)
            + "}\n")


GOOD_LIGHT = {"frame-fill": "#ffffff", "frame-stroke": "#0072b2", "frame-text": "#000000"}
GOOD_DARK = {"frame-fill": "#000000", "frame-stroke": "#56b4e9", "frame-text": "#ffffff"}


class ContrastArithmeticTest(unittest.TestCase):
    """The WCAG 2.x contrast ratio, against values anyone can look up."""

    def test_black_on_white_is_21(self):
        self.assertAlmostEqual(cdc.contrast("#000000", "#ffffff"), 21.0, places=2)

    def test_the_ratio_is_symmetric(self):
        self.assertAlmostEqual(cdc.contrast("#0072b2", "#ffffff"), cdc.contrast("#ffffff", "#0072b2"))

    def test_777_on_white_just_misses_aa(self):
        self.assertAlmostEqual(cdc.contrast("#777777", "#ffffff"), 4.48, places=2)

    def test_short_hex_is_accepted(self):
        self.assertAlmostEqual(cdc.contrast("#000", "#fff"), 21.0, places=2)


class PaletteVerdictTest(unittest.TestCase):
    """One problem, one finding, naming the class and the theme."""

    def test_a_good_palette_has_no_finding(self):
        self.assertEqual(cdc.findings(css(GOOD_LIGHT, GOOD_DARK)), [])

    def test_text_below_aa_on_its_fill_is_a_finding(self):
        light = dict(GOOD_LIGHT, **{"frame-text": "#777777"})
        out = cdc.findings(css(light, GOOD_DARK))
        self.assertEqual(len(out), 1, out)
        self.assertIn("frame", out[0])
        self.assertIn("light", out[0])

    def test_a_stroke_that_vanishes_into_the_page_is_a_finding(self):
        dark = dict(GOOD_DARK, **{"frame-stroke": "#222222"})
        out = cdc.findings(css(GOOD_LIGHT, dark))
        self.assertEqual(len(out), 2, out)  # the dark block and its auto twin
        self.assertTrue(all("stroke" in f for f in out), out)

    def test_a_class_missing_from_one_theme_is_a_finding(self):
        light = dict(GOOD_LIGHT, **{"ring-fill": "#fff", "ring-stroke": "#e69f00", "ring-text": "#000"})
        out = cdc.findings(css(light, GOOD_DARK))
        self.assertTrue(any("ring" in f and "dark" in f for f in out), out)

    def test_the_two_dark_blocks_must_agree(self):
        auto = dict(GOOD_DARK, **{"frame-fill": "#111111"})
        out = cdc.findings(css(GOOD_LIGHT, GOOD_DARK, auto))
        self.assertTrue(any("dark-auto" in f for f in out), out)


class RealPaletteTest(unittest.TestCase):
    """The site's own palette passes, and carries every vocabulary class."""

    def test_the_site_palette_passes(self):
        text = (pathlib.Path(REPO) / "docs" / "_static" / "diagram-vocabulary.css").read_text()
        self.assertEqual(cdc.findings(text), [])

    def test_the_site_palette_names_the_vocabulary(self):
        text = (pathlib.Path(REPO) / "docs" / "_static" / "diagram-vocabulary.css").read_text()
        self.assertEqual(set(cdc.palette(text)["light"]), set(cdc.VOCABULARY))


class VendoredMermaidTest(unittest.TestCase):
    """The site loads the pinned Mermaid from its own static assets (#2374 ruling 1)."""

    def setUp(self):
        self.conf = (pathlib.Path(REPO) / "docs" / "conf.py").read_text()

    def test_the_pinned_version_is_vendored(self):
        version = re.search(r'^mermaid_version = "([0-9.]+)"$', self.conf, re.M).group(1)
        self.assertIn('mermaid_use_local = f"vendor/mermaid-{mermaid_version}/', self.conf)
        vendored = pathlib.Path(REPO) / "docs" / "_static" / "vendor" / f"mermaid-{version}"
        self.assertTrue((vendored / "mermaid.esm.min.mjs").is_file(), vendored)
        self.assertTrue((vendored / "LICENSE").is_file(), vendored)

    def test_every_chunk_the_entry_imports_is_vendored(self):
        version = re.search(r'^mermaid_version = "([0-9.]+)"$', self.conf, re.M).group(1)
        vendored = pathlib.Path(REPO) / "docs" / "_static" / "vendor" / f"mermaid-{version}"
        imports = set()
        for mjs in vendored.rglob("*.mjs"):
            for rel in re.findall(r'from\s*"(\./[^"]+\.mjs)"|import\(\s*"(\./[^"]+\.mjs)"\s*\)',
                                  mjs.read_text(encoding="utf-8")):
                imports.add((mjs.parent / (rel[0] or rel[1])).resolve())
        self.assertTrue(imports, "no relative imports found; the ESM layout changed")
        missing = sorted(str(p) for p in imports if not p.is_file())
        self.assertEqual(missing, [])

    def test_the_syntax_gate_parses_with_the_same_version(self):
        version = re.search(r'^mermaid_version = "([0-9.]+)"$', self.conf, re.M).group(1)
        lock = json.loads((pathlib.Path(REPO) / "tools" / "mermaid-lint" / "package-lock.json").read_text())
        self.assertEqual(lock["packages"]["node_modules/mermaid"]["version"], version)

    def test_the_extension_is_pinned_to_match(self):
        reqs = (pathlib.Path(REPO) / "docs" / "requirements.txt").read_text()
        self.assertRegex(reqs, r"(?m)^sphinxcontrib-mermaid==\d")


if __name__ == "__main__":
    unittest.main()
