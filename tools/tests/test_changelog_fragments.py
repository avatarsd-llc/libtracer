#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the changelog fragments (#2600).

`tools/changelog_assemble.py` must produce exactly the text a hand edit would, and
`tools/check_changelog_fragment.py` must be red on a public-header change with no
fragment and green with one. Run with ``python3 -m unittest discover -s tools/tests``.
"""
import os
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import changelog_assemble as ca  # noqa: E402
import check_changelog_fragment as ccf  # noqa: E402

CHANGELOG = """# Changelog

## [Unreleased]

### Changed (breaking)

- **A break (#1).** Wrapped
  over two lines.

### Removed

- **A removal (#2).**

  | Old | New |
  | --- | --- |
  | `a` | `b` |

## [0.1.0] — 2026-01-01

### Performance

- old
"""


def write(path, text):
    """Write ``text`` to ``path``, creating its directory."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def run(*args, cwd=None):
    """Run a command, returning the completed process."""
    return subprocess.run(list(args), cwd=cwd, capture_output=True, text=True)


class AssembleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.tmp.name, "CHANGELOG.md")
        write(self.path, CHANGELOG)

    def tearDown(self):
        self.tmp.cleanup()

    def fragment(self, name, body):
        write(os.path.join(self.tmp.name, "changelog.d", name), body)

    def assemble(self, *flags):
        script = os.path.join(REPO, "tools", "changelog_assemble.py")
        return run(sys.executable, script, *flags, self.path)

    def test_kinds_are_the_files_own_headings(self):
        kinds = ca.known_headings(CHANGELOG.split("\n"))
        self.assertEqual(kinds["changed-breaking"], "Changed (breaking)")
        self.assertEqual(kinds["performance"], "Performance")  # from a released section
        self.assertEqual(kinds["security"], "Security")  # Keep a Changelog, unused here

    def test_write_matches_a_hand_edit_and_deletes_the_fragments(self):
        self.fragment("20.changed-breaking.md", "- **Later break (#20).**\n")
        self.fragment("10.changed-breaking.md", "- **Earlier break (#10).**\n  More.\n")
        self.fragment("11.removed.md", "- **After a table (#11).**\n")
        self.fragment("12.fixed.md", "- **A fix (#12).**\n")
        self.assertEqual(self.assemble("--write").returncode, 0)
        hand = CHANGELOG.replace(
            "  over two lines.\n",
            "  over two lines.\n- **Earlier break (#10).**\n  More.\n- **Later break (#20).**\n",
        ).replace(
            "  | `a` | `b` |\n",
            "  | `a` | `b` |\n\n- **After a table (#11).**\n\n### Fixed\n\n- **A fix (#12).**\n",
        )
        with open(self.path, encoding="utf-8") as fh:
            self.assertEqual(fh.read(), hand)
        self.assertEqual(os.listdir(os.path.join(self.tmp.name, "changelog.d")), [])

    def test_check_writes_nothing(self):
        self.fragment("12.fixed.md", "- **A fix (#12).**\n")
        res = self.assemble("--check")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("### Fixed\n\n- **A fix (#12).**", res.stdout)
        self.assertNotIn("## [0.1.0]", res.stdout)
        with open(self.path, encoding="utf-8") as fh:
            self.assertEqual(fh.read(), CHANGELOG)
        self.assertEqual(self.assemble("--check", "--fail-on-pending").returncode, 1)

    def test_bad_fragments_are_refused(self):
        for name, body in (
            ("12.fixes.md", "- typo kind\n"),
            ("fix.fixed.md", "- no issue\n"),
            ("13.fixed.md", "not a bullet\n"),
        ):
            with self.subTest(name=name):
                self.fragment(name, body)
                res = self.assemble("--check")
                self.assertEqual(res.returncode, 1)
                self.assertIn(name, res.stderr)
                os.remove(os.path.join(self.tmp.name, "changelog.d", name))

    def test_no_fragments_is_a_no_op(self):
        self.assertEqual(ca.assemble(CHANGELOG, [], {}), CHANGELOG)
        self.assertEqual(self.assemble("--check", "--fail-on-pending").returncode, 0)

    def test_empty_unreleased_gets_its_first_heading(self):
        text = "# c\n\n## [Unreleased]\n\n## [0.1.0] — d\n\n- old\n"
        kinds = ca.known_headings(text.split("\n"))
        out = ca.assemble(text, [(1, "added", "x", ["- new (#1)."])], kinds)
        self.assertEqual(out, "# c\n\n## [Unreleased]\n\n### Added\n\n- new (#1).\n\n## [0.1.0] — d\n\n- old\n")

    def test_repo_fragments_are_valid(self):
        res = run(sys.executable, os.path.join(REPO, "tools", "changelog_assemble.py"), "--check")
        self.assertEqual(res.returncode, 0, res.stderr)


class RequireFragmentTest(unittest.TestCase):
    """Drive a throwaway repository through both arms of the rule."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = self.tmp.name
        for cmd in (
            ["git", "init", "-q", "-b", "main"],
            ["git", "config", "user.email", "t@example.invalid"],
            ["git", "config", "user.name", "t"],
            ["git", "config", "commit.gpgsign", "false"],
        ):
            subprocess.run(cmd, cwd=self.repo, check=True)
        write(os.path.join(self.repo, "core/include/libtracer/a.hpp"), "// a\n")
        write(os.path.join(self.repo, "core/src/a.cpp"), "// a\n")
        self.commit()
        self.base = self.head()

    def tearDown(self):
        self.tmp.cleanup()

    def commit(self):
        subprocess.run(["git", "add", "-A"], cwd=self.repo, check=True)
        subprocess.run(["git", "commit", "-q", "-m", "c"], cwd=self.repo, check=True)

    def head(self):
        return run("git", "rev-parse", "HEAD", cwd=self.repo).stdout.strip()

    def gaps(self):
        cwd = os.getcwd()
        os.chdir(self.repo)
        try:
            return ccf.missing(ccf.changed_paths(self.base, self.head()))
        finally:
            os.chdir(cwd)

    def test_header_change_without_fragment_is_red(self):
        write(os.path.join(self.repo, "core/include/libtracer/a.hpp"), "// b\n")
        self.commit()
        self.assertEqual(self.gaps(), {"core/changelog.d": ["core/include/libtracer/a.hpp"]})

    def test_header_change_with_fragment_is_green(self):
        write(os.path.join(self.repo, "core/include/libtracer/a.hpp"), "// b\n")
        write(os.path.join(self.repo, "core/changelog.d/7.changed.md"), "- x (#7).\n")
        self.commit()
        self.assertEqual(self.gaps(), {})

    def test_fragment_in_the_wrong_package_does_not_count(self):
        write(os.path.join(self.repo, "integrations/esp-idf/libtracer/include/libtracer_esp/b.hpp"), "// b\n")
        write(os.path.join(self.repo, "core/changelog.d/7.added.md"), "- x (#7).\n")
        self.commit()
        self.assertIn("integrations/esp-idf/libtracer/changelog.d", self.gaps())

    def test_backend_header_maps_to_core(self):
        write(os.path.join(self.repo, "backends/cuda/include/libtracer/m.hpp"), "// m\n")
        self.commit()
        self.assertIn("core/changelog.d", self.gaps())

    def test_source_only_change_needs_nothing(self):
        write(os.path.join(self.repo, "core/src/a.cpp"), "// b\n")
        self.commit()
        self.assertEqual(self.gaps(), {})


if __name__ == "__main__":
    unittest.main()
