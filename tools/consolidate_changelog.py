#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Consolidate a Keep-a-Changelog ``Unreleased`` section, mechanically.

A long-lived ``[Unreleased]`` accumulates one ``### Category`` heading per landing
PR, so the section ends up with the categories interleaved — `core/CHANGELOG.md`
carries eighteen of them against six distinct names. Consolidating that by hand at
tag time is the error-prone half of `.github/RELEASING.md`; this does it.

What it does, and *only* this:

* merges the repeated ``### Category`` subsections of the ``Unreleased`` section
  into one subsection per category, **preserving the order the entries appear in**
  (so the newest entry of a category stays newest);
* orders the merged categories by the Keep-a-Changelog sequence — Added, Changed,
  Deprecated, Removed, Fixed, Security — with any unrecognised category appended
  after, in first-seen order;
* optionally renames the section to ``## [X.Y.Z] — YYYY-MM-DD`` and opens a fresh
  empty ``## [Unreleased]`` above it (``--release``).

It never edits, reflows or re-word-wraps an entry: a subsection's body is moved
verbatim, so the operation is text-preserving by construction. Fenced code blocks
are tracked, so a ``### `` line inside a fence is body text, not a heading.

Per the 2026-08-01 release-mechanics ruling, consolidation happens **at tag time,
as the last pre-tag commit** — never speculatively. ``--check`` is what CI can run
to assert a *released* section is already consolidated.

Usage::

  tools/consolidate_changelog.py core/CHANGELOG.md            # print the result
  tools/consolidate_changelog.py core/CHANGELOG.md --write    # edit in place
  tools/consolidate_changelog.py core/CHANGELOG.md --check    # exit 1 on drift
  tools/consolidate_changelog.py core/CHANGELOG.md --release 0.7.0 --write
"""
import argparse
import datetime
import re
import sys

# The Keep-a-Changelog 1.1.0 category sequence. Anything else is kept, after these.
CATEGORY_ORDER = ["Added", "Changed", "Deprecated", "Removed", "Fixed", "Security"]

SECTION_RE = re.compile(r"^##\s+(?!#)")
SUBSECTION_RE = re.compile(r"^###\s+(?!#)(?P<name>.+?)\s*$")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
UNRELEASED_RE = re.compile(r"^##\s+\[?Unreleased\]?", re.IGNORECASE)


def _fence_mask(lines):
    """Return a list of bools: True where the line sits inside a fenced code block."""
    inside = False
    mask = []
    for line in lines:
        if FENCE_RE.match(line):
            mask.append(True)  # the fence line itself is never a heading
            inside = not inside
            continue
        mask.append(inside)
    return mask


def find_unreleased(lines):
    """Return ``(start, end)`` line indices of the Unreleased section, or ``None``.

    ``start`` is the index of the ``## [Unreleased]`` heading; ``end`` is the index
    of the next top-level ``## `` heading (or ``len(lines)``).
    """
    mask = _fence_mask(lines)
    start = None
    for i, line in enumerate(lines):
        if mask[i]:
            continue
        if start is None:
            if UNRELEASED_RE.match(line):
                start = i
        elif SECTION_RE.match(line):
            return start, i
    return (start, len(lines)) if start is not None else None


def split_subsections(body_lines):
    """Split a section body into ``(preamble, [(category, lines), ...])``.

    ``preamble`` is whatever precedes the first ``### `` heading (usually blank).
    Each entry's ``lines`` is the subsection body **excluding** its heading.
    """
    mask = _fence_mask(body_lines)
    preamble = []
    subsections = []
    current = None
    for i, line in enumerate(body_lines):
        m = None if mask[i] else SUBSECTION_RE.match(line)
        if m:
            current = (m.group("name"), [])
            subsections.append(current)
            continue
        (preamble if current is None else current[1]).append(line)
    return preamble, subsections


def _trim(lines):
    """Drop leading and trailing blank lines."""
    out = list(lines)
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    return out


def consolidate_body(body_lines):
    """Merge the repeated ``### Category`` subsections of one section body."""
    preamble, subsections = split_subsections(body_lines)
    if not subsections:
        return list(body_lines)

    merged = {}
    seen_order = []
    for name, lines in subsections:
        body = _trim(lines)
        if not body:
            continue
        if name not in merged:
            merged[name] = []
            seen_order.append(name)
        merged[name].append(body)

    ordered = [c for c in CATEGORY_ORDER if c in merged]
    ordered += [c for c in seen_order if c not in CATEGORY_ORDER]

    out = _trim(preamble)
    for name in ordered:
        if out:
            out.append("")
        out.append(f"### {name}")
        for block in merged[name]:
            out.append("")
            out.extend(block)
    return ["", *out, ""] if out else [""]


def consolidate(text, release=None, date=None):
    """Return ``text`` with its Unreleased section consolidated (and maybe released)."""
    lines = text.split("\n")
    span = find_unreleased(lines)
    if span is None:
        return text
    start, end = span
    heading = lines[start]
    body = consolidate_body(lines[start + 1 : end])

    head_lines = [heading]
    if release:
        stamp = date or datetime.date.today().isoformat()
        bracketed = heading.lstrip("#").strip().startswith("[")
        title = f"[{release}]" if bracketed else release
        head_lines = ["## [Unreleased]", "", f"## {title} — {stamp}"]

    return "\n".join(lines[:start] + head_lines + body + lines[end:])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", maxsplit=1)[0])
    ap.add_argument("changelog", nargs="+", help="CHANGELOG.md path(s) to consolidate")
    ap.add_argument("--write", action="store_true", help="edit the file(s) in place")
    ap.add_argument(
        "--check",
        action="store_true",
        help="exit 1 if consolidation would change anything; write nothing",
    )
    ap.add_argument("--release", help="also rename Unreleased to this version, e.g. 0.7.0")
    ap.add_argument("--date", help="release date (default: today, ISO)")
    args = ap.parse_args()

    if args.check and (args.write or args.release):
        ap.error("--check is read-only; it cannot be combined with --write/--release")

    drifted = []
    for path in args.changelog:
        with open(path, encoding="utf-8") as fh:
            original = fh.read()
        result = consolidate(original, release=args.release, date=args.date)
        if args.check:
            if result != original:
                drifted.append(path)
            continue
        if args.write:
            if result != original:
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(result)
                print(f"consolidate_changelog: rewrote {path}", file=sys.stderr)
        else:
            sys.stdout.write(result)

    if drifted:
        for path in drifted:
            print(
                f"::error file={path}::{path} has interleaved '###' headings in its "
                "Unreleased section — run tools/consolidate_changelog.py --write",
                file=sys.stderr,
            )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
