#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Fold per-PR changelog fragments into each CHANGELOG's ``Unreleased`` section.

Why (#2600). Two queued PRs that each added a bullet to the same ``### Added`` list
conflicted on rebase, every time. A PR now adds its own file instead, and two new files
never conflict:

    <package>/changelog.d/<issue>.<kind>.md

beside the package's ``CHANGELOG.md`` (``core/``, ``integrations/esp-idf/libtracer/``,
each binding). ``<issue>`` is the issue number; ``<kind>`` names the ``### `` heading the
bullet goes under, spelled as a slug of the heading: lower case, every run of other
characters one ``-``. So ``added``, ``changed``, ``deprecated``, ``removed``, ``fixed``,
``security`` (Keep a Changelog), and every heading the CHANGELOG already uses:
``changed-breaking`` is ``### Changed (breaking)``, ``removed-breaking`` is
``### Removed (breaking)``, ``performance`` is ``### Performance``. A slug that matches
none of those is refused, so a typo cannot open a new heading.

The body is the bullet text exactly as it would appear in the CHANGELOG, starting with
``- ``. One fragment may hold several bullets.

What assembly does, and only this, per CHANGELOG, in issue order:

* appends the fragment's text after the last line of the first ``### <Heading>``
  subsection of ``Unreleased``, with no blank line between bullets, as a hand edit
  does; a blank line separates them only when the fragment itself has one, or when
  the subsection ends in a table or a fence;
* if ``Unreleased`` has no such subsection yet, opens one at the end of the section
  (``tools/consolidate_changelog.py`` puts the headings in Keep a Changelog order at
  the cut);
* deletes the fragment (``--write`` only).

It never rewraps or edits the text. Consolidation (merging repeated headings) and the
version stamp stay with ``tools/consolidate_changelog.py``, which the release runs
right after this; see ``.github/RELEASING.md``.

Usage::

  tools/changelog_assemble.py                    # print each assembled Unreleased section
  tools/changelog_assemble.py --check            # the same dry run; exit 1 on a bad fragment
  tools/changelog_assemble.py --check --fail-on-pending   # also exit 1 if any fragment is left
  tools/changelog_assemble.py --write            # fold in and delete the fragments
  tools/changelog_assemble.py core/CHANGELOG.md  # only these CHANGELOGs
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import consolidate_changelog as cc  # noqa: E402
from gen_release_notes import DEFAULT_CHANGELOGS  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRAGMENT_DIR = "changelog.d"
FRAGMENT_RE = re.compile(r"^(?P<issue>[0-9]+)\.(?P<kind>[a-z0-9]+(?:-[a-z0-9]+)*)\.md$")


def slug(heading):
    """Return the fragment kind that names ``### heading``."""
    return re.sub(r"[^a-z0-9]+", "-", heading.lower()).strip("-")


def known_headings(lines):
    """Return ``{kind: heading}`` for the Keep a Changelog set plus every heading in use."""
    kinds = {slug(name): name for name in cc.CATEGORY_ORDER}
    mask = cc._fence_mask(lines)
    for i, line in enumerate(lines):
        m = None if mask[i] else cc.SUBSECTION_RE.match(line)
        if m:
            kinds.setdefault(slug(m.group("name")), m.group("name"))
    return kinds


def fragment_dir(changelog):
    """Return the fragment directory that belongs to ``changelog``."""
    return os.path.join(os.path.dirname(changelog), FRAGMENT_DIR)


def read_fragments(changelog, kinds):
    """Return ``(fragments, errors)``: ``fragments`` is ``[(issue, kind, path, lines)]``."""
    directory = fragment_dir(changelog)
    fragments, errors = [], []
    if not os.path.isdir(directory):
        return fragments, errors
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        m = FRAGMENT_RE.match(name)
        if not m:
            errors.append(f"{path}: not named <issue>.<kind>.md")
            continue
        if m.group("kind") not in kinds:
            known = ", ".join(sorted(kinds))
            errors.append(f"{path}: kind '{m.group('kind')}' is no heading of {changelog} ({known})")
            continue
        with open(path, encoding="utf-8") as fh:
            body = cc._trim(fh.read().split("\n"))
        if not body or not body[0].startswith("- "):
            errors.append(f"{path}: the body must be the bullet text, starting with '- '")
            continue
        fragments.append((int(m.group("issue")), m.group("kind"), path, body))
    fragments.sort(key=lambda f: (f[0], f[1]))
    return fragments, errors


def _loose(lines):
    """True when a fragment joined after ``lines`` needs a blank line before it."""
    last = lines[-1].strip() if lines else ""
    return last.startswith("|") or cc.FENCE_RE.match(last) is not None


def assemble_body(body_lines, fragments, kinds):
    """Return the Unreleased body with every fragment folded in."""
    preamble, subsections = cc.split_subsections(body_lines)
    subsections = [(name, list(lines)) for name, lines in subsections]
    for _issue, kind, _path, text in fragments:
        heading = kinds[kind]
        target = next((lines for name, lines in subsections if name == heading), None)
        if target is not None:
            filled = [i for i, line in enumerate(target) if line.strip()]
            if not filled:
                target[:] = ["", *text, ""]
                continue
            end = filled[-1] + 1
            gap = [""] if "" in text or _loose(target[:end]) else []
            target[end:end] = gap + text
            continue
        prev = subsections[-1][1] if subsections else preamble
        if not prev or prev[-1].strip():
            prev.append("")
        subsections.append((heading, ["", *text, ""]))
    out = list(preamble)
    for name, lines in subsections:
        out.append(f"### {name}")
        out.extend(lines)
    return out


def assemble(text, fragments, kinds):
    """Return ``text`` with ``fragments`` folded into its Unreleased section."""
    lines = text.split("\n")
    span = cc.find_unreleased(lines)
    if span is None or not fragments:
        return text
    start, end = span
    body = assemble_body(lines[start + 1 : end], fragments, kinds)
    return "\n".join(lines[: start + 1] + body + lines[end:])


def unreleased_section(text):
    """Return the Unreleased section of ``text``, heading included."""
    lines = text.split("\n")
    span = cc.find_unreleased(lines)
    return "" if span is None else "\n".join(lines[span[0] : span[1]]).rstrip("\n") + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", maxsplit=1)[0])
    ap.add_argument("changelog", nargs="*", help="CHANGELOG.md path(s); default: every package's")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="dry run (the default); exit 1 on a bad fragment")
    mode.add_argument("--write", action="store_true", help="fold the fragments in and delete them")
    ap.add_argument("--fail-on-pending", action="store_true", help="with --check: exit 1 if any fragment is left")
    args = ap.parse_args()
    if args.fail_on_pending and args.write:
        ap.error("--fail-on-pending is a check; it cannot be combined with --write")

    paths = args.changelog or [os.path.join(REPO, p) for p in DEFAULT_CHANGELOGS]
    errors, pending = [], []
    for path in paths:
        with open(path, encoding="utf-8") as fh:
            original = fh.read()
        kinds = known_headings(original.split("\n"))
        fragments, bad = read_fragments(path, kinds)
        errors += bad
        if not fragments:
            continue
        pending += [f[2] for f in fragments]
        if cc.find_unreleased(original.split("\n")) is None:
            errors.append(f"{path}: no Unreleased section to fold {len(fragments)} fragment(s) into")
            continue
        result = assemble(original, fragments, kinds)
        if args.write:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(result)
            for f in fragments:
                os.remove(f[2])
            print(f"changelog_assemble: folded {len(fragments)} fragment(s) into {path}", file=sys.stderr)
        else:
            sys.stdout.write(f"===== {os.path.relpath(path, REPO)} =====\n{unreleased_section(result)}\n")

    for e in errors:
        print(f"::error::{e}", file=sys.stderr)
    if args.fail_on_pending and pending:
        for p in pending:
            print(f"::error file={p}::fragment not assembled; run tools/changelog_assemble.py --write", file=sys.stderr)
        return 1
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
