#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the two release-mechanics tools.

`tools/consolidate_changelog.py` rewrites a file that a human release then tags,
and `tools/gen_release_notes.py` writes the only text most consumers will read
about a release. Neither had a test; both run exactly once per release, where a
mistake is expensive and unrepeatable.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import io
import os
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURES = os.path.join(REPO, "tools", "tests", "fixtures")
sys.path.insert(0, os.path.join(REPO, "tools"))

import consolidate_changelog as cc  # noqa: E402
import gen_release_notes as grn  # noqa: E402


def read(path):
    """Read a UTF-8 text file whole."""
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def bullets(text):
    """Every top-level `- ` bullet in `text`, as a set — the content-preservation probe."""
    return {line for line in text.split("\n") if line.startswith("- ")}


def unfenced_headings(lines, prefix="### "):
    """Heading lines outside code fences — an independent re-derivation of the rule.

    Deliberately not `consolidate_changelog._fence_mask`: a test that counts headings
    with the same helper the tool splits on cannot notice the two disagreeing.
    """
    out, inside = [], False
    for line in lines:
        if line.lstrip().startswith(("```", "~~~")):
            inside = not inside
            continue
        if not inside and line.startswith(prefix):
            out.append(line)
    return out


class ConsolidateTest(unittest.TestCase):
    """The interleaved → consolidated rewrite, and what it must not disturb."""

    def setUp(self):
        self.interleaved = read(os.path.join(FIXTURES, "interleaved-CHANGELOG.md"))
        self.consolidated = read(os.path.join(FIXTURES, "consolidated-CHANGELOG.md"))

    def test_matches_the_golden_file(self):
        """The fixture round-trips to the committed consolidated form, byte for byte."""
        self.assertEqual(cc.consolidate(self.interleaved), self.consolidated)

    def test_is_idempotent(self):
        """Consolidating an already-consolidated file changes nothing."""
        self.assertEqual(cc.consolidate(self.consolidated), self.consolidated)

    def test_preserves_every_entry(self):
        """No bullet is dropped, duplicated or reworded — only moved."""
        self.assertEqual(bullets(self.interleaved), bullets(self.consolidated))
        self.assertEqual(self.interleaved.count("(#10"), self.consolidated.count("(#10"))

    def test_one_heading_per_category(self):
        """The nine interleaved `###` headings collapse to six distinct ones."""
        before = unfenced_headings(self.interleaved.split("\n"))
        start, end = cc.find_unreleased(self.consolidated.split("\n"))
        section_heads = unfenced_headings(self.consolidated.split("\n")[start:end])
        self.assertEqual(len(section_heads), len(set(section_heads)))
        self.assertEqual(len(section_heads), 6)
        # The released `## [0.1.0]` section keeps its own `### Added`, so the whole
        # file still has more headings than the Unreleased section does.
        self.assertGreater(len(before), len(section_heads))

    def test_keep_a_changelog_order(self):
        """Categories sort Added, Changed, Removed, Fixed, Security, then the rest."""
        start, end = cc.find_unreleased(self.consolidated.split("\n"))
        heads = [l[4:] for l in unfenced_headings(self.consolidated.split("\n")[start:end])]
        self.assertEqual(
            heads,
            ["Added", "Changed", "Removed", "Fixed", "Security", "Notes for packagers"],
        )

    def test_entry_order_within_a_category_is_preserved(self):
        """First-landed stays first: #101 before #104 before #108."""
        body = self.consolidated
        self.assertLess(body.index("(#101)"), body.index("(#104)"))
        self.assertLess(body.index("(#104)"), body.index("(#108)"))

    def test_a_fenced_heading_is_not_a_heading(self):
        """A column-zero `###`/`##` line inside a code fence stays inside its entry.

        The indented variant is not enough on its own — an indented line never matched
        the `^###` heading pattern anyway, so a fixture with only that one keeps passing
        when the fence tracking is ablated away. This asserts on the column-zero fence.
        """
        self.assertIn("\n### Not A Heading At Column Zero\n", self.consolidated)
        self.assertIn("\n## Also Not A Section At Column Zero\n", self.consolidated)
        # It must stay inside the Added entry it belongs to: after `### Added`, and
        # before the next real heading, `### Changed`.
        self.assertLess(
            self.consolidated.index("### Added"),
            self.consolidated.index("### Not A Heading At Column Zero"),
        )
        self.assertLess(
            self.consolidated.index("### Not A Heading At Column Zero"),
            self.consolidated.index("### Changed"),
        )
        # And the indented one stays under Changed.
        self.assertLess(
            self.consolidated.index("  ### Not A Heading"),
            self.consolidated.index("### Removed"),
        )

    def test_fence_mask_tracks_open_and_close(self):
        """`_fence_mask` marks the fenced body, not the fence lines' neighbours."""
        lines = ["a", "```", "### x", "```", "### y"]
        self.assertEqual(cc._fence_mask(lines), [False, True, True, True, False])

    def test_released_sections_are_untouched(self):
        """Only the Unreleased section is rewritten."""
        tail = "## [0.1.0] — 2026-01-01\n\n### Added\n\n- The first cut.\n"
        self.assertTrue(self.consolidated.endswith(tail))

    def test_release_renames_and_reopens(self):
        """`--release` stamps the version and opens a fresh empty Unreleased."""
        out = cc.consolidate(self.interleaved, release="0.7.0", date="2026-08-02")
        self.assertIn("## [Unreleased]\n\n## [0.7.0] — 2026-08-02\n", out)
        self.assertEqual(bullets(out), bullets(self.interleaved))

    def test_release_keeps_the_heading_style(self):
        """An unbracketed `## Unreleased` (the TS changelog's style) stays unbracketed."""
        src = "# t\n\n## Unreleased\n\n### Added\n\n- one\n"
        out = cc.consolidate(src, release="0.7.0", date="2026-08-02")
        self.assertIn("## 0.7.0 — 2026-08-02", out)
        self.assertNotIn("## [0.7.0]", out)

    def test_no_unreleased_section_is_a_no_op(self):
        """A changelog with nothing unreleased is returned unchanged."""
        src = "# t\n\n## [0.1.0] — 2026-01-01\n\n### Added\n\n- one\n"
        self.assertEqual(cc.consolidate(src), src)

    def test_check_mode_exit_codes(self):
        """`--check` fails on the interleaved fixture and passes on the golden one."""
        script = os.path.join(REPO, "tools", "consolidate_changelog.py")
        bad = subprocess.run(
            [sys.executable, script, "--check", os.path.join(FIXTURES, "interleaved-CHANGELOG.md")],
            capture_output=True,
            check=False,
        )
        self.assertEqual(bad.returncode, 1)
        good = subprocess.run(
            [
                sys.executable,
                script,
                "--check",
                os.path.join(FIXTURES, "consolidated-CHANGELOG.md"),
            ],
            capture_output=True,
            check=False,
        )
        self.assertEqual(good.returncode, 0)

    def test_write_mode_edits_in_place(self):
        """`--write` rewrites the file to the consolidated form."""
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "CHANGELOG.md")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(self.interleaved)
            subprocess.run(
                [sys.executable, os.path.join(REPO, "tools", "consolidate_changelog.py"),
                 path, "--write"],
                capture_output=True,
                check=True,
            )
            self.assertEqual(read(path), self.consolidated)


class ReleaseNotesTest(unittest.TestCase):
    """The multi-package release body (#607 ruling a)."""

    def _write(self, tmp, rel, text):
        path = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        return path

    def test_version_section_wins_over_unreleased(self):
        """A consolidated package contributes its `[X.Y.Z]` section."""
        with tempfile.TemporaryDirectory() as tmp:
            p = self._write(
                tmp,
                "core/CHANGELOG.md",
                "# c\n\n## [Unreleased]\n\n- later\n\n## [0.7.0] — 2026-08-02\n\n- shipped\n",
            )
            self.assertEqual(grn.extract_changelog(p, "0.7.0"), "- shipped")

    def test_unreleased_fallback(self):
        """An unconsolidated package still contributes — this is the #676 residue."""
        with tempfile.TemporaryDirectory() as tmp:
            p = self._write(
                tmp,
                "bindings/typescript/CHANGELOG.md",
                "# ts\n\n## Unreleased\n\n### Changed\n\n- **BREAKING — routes**\n\n## 0.6.0 — x\n",
            )
            self.assertIn("BREAKING — routes", grn.extract_changelog(p, "0.7.0"))

    def test_missing_file_is_empty_not_fatal(self):
        """A changelog path that does not exist must never fail a release."""
        self.assertEqual(grn.extract_changelog("/nonexistent/CHANGELOG.md", "0.7.0"), "")

    def test_collect_and_render_every_package(self):
        """All four packages appear, each under its own heading, in argument order."""
        with tempfile.TemporaryDirectory() as tmp:
            paths = [
                self._write(tmp, "core/CHANGELOG.md", "# c\n\n## [0.7.0] — d\n\n- core entry\n"),
                self._write(tmp, "bindings/rust/CHANGELOG.md", "# r\n\n## [Unreleased]\n\n- rust entry\n"),
                self._write(tmp, "bindings/typescript/CHANGELOG.md", "# t\n\n## Unreleased\n\n- ts entry\n"),
                self._write(
                    tmp,
                    "integrations/esp-idf/libtracer/CHANGELOG.md",
                    "# e\n\n## [Unreleased]\n\n- esp entry\n",
                ),
            ]
            # Labels are keyed on the repo-relative directory, so make them relative.
            rel = [os.path.relpath(p, tmp) for p in paths]
            cwd = os.getcwd()
            os.chdir(tmp)
            try:
                sections = grn.collect_sections(rel, "0.7.0")
            finally:
                os.chdir(cwd)
            self.assertEqual(len(sections), 4)
            body = grn.render_sections(sections)
            for entry in ("- core entry", "- rust entry", "- ts entry", "- esp entry"):
                self.assertIn(entry, body)
            self.assertEqual(body.count("#### "), 4)
            self.assertLess(body.index("core entry"), body.index("rust entry"))
            self.assertIn("#### core — C++ reference implementation", body)
            self.assertIn("bindings/rust", body)

    def test_empty_packages_are_skipped_not_headed(self):
        """A package with no section contributes no empty heading."""
        with tempfile.TemporaryDirectory() as tmp:
            p = self._write(tmp, "core/CHANGELOG.md", "# c\n\n## [0.1.0] — d\n\n- old\n")
            self.assertEqual(grn.collect_sections([p], "0.7.0"), [])

    def test_a_fitting_body_is_not_trimmed_at_all(self):
        """The budgeted path must be a no-op when nothing is over — the control arm."""
        sections = [("core", "- a\n- b"), ("rust", "- c")]
        plain = grn.render_sections(sections)
        text, truncated = grn.render_sections(sections, budget=10_000)
        self.assertFalse(truncated)
        self.assertEqual(text, plain)

    def test_the_cut_lands_on_an_entry_boundary(self):
        """Half an entry published as a whole one is worse than a missing entry."""
        text, truncated = grn.trim_to_entry_boundary("- one\n  cont\n- two\n  cont", 14)
        self.assertTrue(truncated)
        self.assertEqual(text, "- one\n  cont")

    def test_a_huge_core_does_not_starve_the_bindings(self):
        """#676's failure, restated as a budget: every package must still speak.

        First-come-first-served truncation spends the whole budget on core — the npm
        and crates consumers this tool exists to reach would read nothing.
        """
        sections = [("core", "\n".join(f"- core {i}" for i in range(2000))), ("rust", "- rust one")]
        text, truncated = grn.render_sections(sections, budget=4_000)
        self.assertTrue(truncated)
        self.assertIn("- rust one", text)
        self.assertIn("- core 0", text)
        self.assertNotIn("- core 1999", text)

    def test_the_real_090_body_fits_under_githubs_limit(self):
        """The regression itself: v0.9.0's body was 191 358 characters and 422'd."""
        cwd = os.getcwd()
        os.chdir(REPO)
        try:
            sections = grn.collect_sections(grn.DEFAULT_CHANGELOGS, "0.9.0")
            full = grn.render_sections(sections)
            text, truncated = grn.render_sections(sections, budget=grn.GITHUB_BODY_LIMIT)
        finally:
            os.chdir(cwd)
        self.assertGreater(len(full), 125_000, "fixture is stale — this release no longer overflows")
        self.assertTrue(truncated)
        self.assertLessEqual(len(text), grn.GITHUB_BODY_LIMIT)
        self.assertEqual(text.count("#### "), len(grn.DEFAULT_CHANGELOGS))

    def test_unknown_package_falls_back_to_its_directory(self):
        """A new package changelog needs no table edit to get a heading."""
        self.assertEqual(grn.package_label("bindings/python/CHANGELOG.md"), "bindings/python")

    def test_defaults_cover_every_changelog_in_the_tree(self):
        """DEFAULT_CHANGELOGS must not go stale as packages are added."""
        # TRACKED files only. An `os.walk` also finds untracked copies of the repo —
        # `.claude/` agent worktrees and ablation trees, `bench/vendor/`'s vendored
        # changelog — and reddens on local scratch state that no release can publish.
        # It was green in CI only because CI has none of them.
        listing = subprocess.run(
            ["git", "-C", REPO, "ls-files", "*CHANGELOG.md"],
            capture_output=True,
            text=True,
            check=True,
        )
        found = {
            line
            for line in listing.stdout.split("\n")
            if line and "tools/tests/fixtures/" not in line  # this file's own inputs
        }
        self.assertEqual(found, set(grn.DEFAULT_CHANGELOGS))

    def test_main_emits_every_package_without_an_api_key(self):
        """End to end, key absent: the body is the concatenated sections."""
        env_key = os.environ.pop("ANTHROPIC_API_KEY", None)
        argv, stdout = sys.argv, sys.stdout
        try:
            with tempfile.TemporaryDirectory() as tmp:
                self._write(tmp, "core/CHANGELOG.md", "# c\n\n## [Unreleased]\n\n- core entry\n")
                self._write(tmp, "bindings/rust/CHANGELOG.md", "# r\n\n## [Unreleased]\n\n- rust entry\n")
                cwd = os.getcwd()
                os.chdir(tmp)
                try:
                    sys.argv = [
                        "gen_release_notes.py",
                        "--version",
                        "0.7.0",
                        "--changelog",
                        "core/CHANGELOG.md",
                        "bindings/rust/CHANGELOG.md",
                    ]
                    sys.stdout = io.StringIO()
                    grn.main()
                    out = sys.stdout.getvalue()
                finally:
                    os.chdir(cwd)
            self.assertIn("### Changelog — 0.7.0", out)
            self.assertIn("- core entry", out)
            self.assertIn("- rust entry", out)
        finally:
            sys.argv, sys.stdout = argv, stdout
            if env_key is not None:
                os.environ["ANTHROPIC_API_KEY"] = env_key


class RepoChangelogTest(unittest.TestCase):
    """The tools, applied to the real tree — the release path they will actually take."""

    def test_every_package_contributes_to_a_release_body(self):
        """Right now, each of the four packages has unreleased content to publish."""
        cwd = os.getcwd()
        os.chdir(REPO)
        try:
            sections = grn.collect_sections(grn.DEFAULT_CHANGELOGS, "0.7.0")
        finally:
            os.chdir(cwd)
        self.assertEqual(len(sections), len(grn.DEFAULT_CHANGELOGS))

    def test_the_676_ts_breaking_entries_reach_the_body(self):
        """#676's residue: the TS BREAKING text must be in the release body."""
        cwd = os.getcwd()
        os.chdir(REPO)
        try:
            body = grn.render_sections(grn.collect_sections(grn.DEFAULT_CHANGELOGS, "0.7.0"))
        finally:
            os.chdir(cwd)
        self.assertIn("`walkTopology` composes routes from mount paths", body)
        self.assertIn("TopologyGraph` gains `complete`", body)

    def test_rust_semver_visible_changes_are_recorded(self):
        """The backfilled Rust entries name every semver-visible change since v0.6.0."""
        text = read(os.path.join(REPO, "bindings", "rust", "CHANGELOG.md"))
        head = text.split("## [0.6.0]")[0]
        for token in ("MAX_SEGMENTS", "admit_path_tlv", "DeliveryPolicy", "subscriber_policy"):
            self.assertIn(token, head)

    def test_every_package_changelog_consolidates_cleanly(self):
        """The consolidator runs on every real changelog without losing a bullet."""
        for rel in grn.DEFAULT_CHANGELOGS:
            with self.subTest(rel):
                original = read(os.path.join(REPO, rel))
                out = cc.consolidate(original)
                self.assertEqual(bullets(original), bullets(out))
                self.assertEqual(cc.consolidate(out), out)


if __name__ == "__main__":
    unittest.main()
