#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Fail a change to a public API that adds no changelog fragment (#2600).

The rule is the one `CLAUDE.md` and `docs/agents/pr-review.md` state: a public API
change carries a CHANGELOG note, and the note is a fragment file
(`tools/changelog_assemble.py`). A public API change is, by path, a change to a
public header:

  core/include/**                               -> core/changelog.d/
  backends/*/include/**                         -> core/changelog.d/
  integrations/esp-idf/libtracer/include/**     -> integrations/esp-idf/libtracer/changelog.d/

A header added, modified, deleted or renamed counts. The rule is met when the same
diff adds or edits a fragment in the mapped directory. The bindings have no header
tree, so this check requires nothing of them; their CHANGELOG obligation stays with
review.

Usage::

  tools/check_changelog_fragment.py                        # origin/main...HEAD
  tools/check_changelog_fragment.py --base <ref> --head <ref>
"""
import argparse
import re
import subprocess
import sys

import changelog_assemble as ca

# (public-header pattern, fragment directory it requires a fragment in)
RULES = [
    (re.compile(r"^core/include/"), "core/changelog.d"),
    (re.compile(r"^backends/[^/]+/include/"), "core/changelog.d"),
    (re.compile(r"^integrations/esp-idf/libtracer/include/"), "integrations/esp-idf/libtracer/changelog.d"),
]


def changed_paths(base, head):
    """Return every path the diff ``base...head`` touches, both sides of a rename."""
    out = subprocess.run(
        ["git", "diff", "--name-status", "-M", f"{base}...{head}"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    rows = []
    for line in out.splitlines():
        status, *paths = line.split("\t")
        rows.append((status[0], paths))
    return rows


def missing(rows):
    """Return ``{fragment_dir: [header, ...]}`` for each rule the diff does not meet."""
    need, have = {}, set()
    for status, paths in rows:
        for path in paths:
            for pattern, directory in RULES:
                if pattern.match(path):
                    need.setdefault(directory, []).append(path)
        target = paths[-1]
        directory, _, name = target.rpartition("/")
        if status != "D" and directory.endswith(ca.FRAGMENT_DIR) and ca.FRAGMENT_RE.match(name):
            have.add(directory)
    return {d: sorted(set(h)) for d, h in need.items() if d not in have}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", maxsplit=1)[0])
    ap.add_argument("--base", default="origin/main")
    ap.add_argument("--head", default="HEAD")
    args = ap.parse_args()

    gaps = missing(changed_paths(args.base, args.head))
    for directory, headers in sorted(gaps.items()):
        print(
            f"::error::public headers changed with no fragment in {directory}/ "
            f"({', '.join(headers)}). Add {directory}/<issue>.<kind>.md holding the "
            "CHANGELOG bullet; see .github/CONTRIBUTING.md.",
            file=sys.stderr,
        )
    return 1 if gaps else 0


if __name__ == "__main__":
    sys.exit(main())
