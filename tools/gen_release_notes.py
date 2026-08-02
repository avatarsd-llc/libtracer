#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Build the GitHub Release body for a libtracer tag.

Emits (to stdout): an AI-written prose summary of the release, followed by the
verbatim release section of **every package changelog in the tree** — core, the
Rust binding, the TypeScript packages and the ESP-IDF component — each under its
own heading. One tag publishes all of them in lockstep (`release.yml`), so one
release body has to speak for all of them: before the 2026-08-01 ruling only
``core/CHANGELOG.md`` was read, which is why a TS-only BREAKING entry (#676) was
invisible to the npm consumers it was written for.

Per package the tool takes the ``## [X.Y.Z]`` section if the file already carries
one, and otherwise falls back to its ``## [Unreleased]`` section — consolidation
is the last pre-tag commit and only core is expected to have had it, so a binding
whose entries are still under ``Unreleased`` at tag time is the normal case, not a
mistake.

The AI summary uses the Anthropic Messages API (key from ``ANTHROPIC_API_KEY``); if
the key is absent or the call fails for any reason, it degrades gracefully to just
the CHANGELOG sections — this script must NEVER fail a release.

Usage::

  tools/gen_release_notes.py --version X.Y.Z \\
      [--changelog core/CHANGELOG.md --changelog bindings/rust/CHANGELOG.md ...] \\
      [--commits-file commits.txt] > body.md

``--changelog`` is repeatable and also accepts several paths at once; it defaults to
the four package changelogs. ``--commits-file`` is an optional plain list of commit
subjects since the previous tag (the workflow produces it with ``git log``); it gives
the model extra grounding.
"""
import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request

API_URL = "https://api.anthropic.com/v1/messages"
MODEL = os.environ.get("RELEASE_NOTES_MODEL", "claude-haiku-4-5-20251001")

# Every publishable package's changelog. Keep in step with `release.yml`'s publish
# jobs: a package that ships under the tag must speak in the release body.
DEFAULT_CHANGELOGS = [
    "core/CHANGELOG.md",
    "bindings/rust/CHANGELOG.md",
    "bindings/typescript/CHANGELOG.md",
    "integrations/esp-idf/libtracer/CHANGELOG.md",
]

# Display name per package directory. A path not listed here falls back to its
# directory, so adding a package changelog needs no edit here to work.
PACKAGE_LABELS = {
    "core": "core — C++ reference implementation",
    "bindings/rust": "bindings/rust — the `libtracer` crate",
    "bindings/typescript": "bindings/typescript — the `@avatarsd-llc/libtracer-*` npm packages",
    "integrations/esp-idf/libtracer": "integrations/esp-idf — the ESP component",
}


def package_label(changelog_path):
    """Return the release-body heading for one package changelog."""
    directory = os.path.dirname(changelog_path.replace(os.sep, "/")).strip("/")
    return PACKAGE_LABELS.get(directory, directory or changelog_path)


def _section(text, version):
    """Return the body of the `## [version]` section (excludes the heading)."""
    # Match from the version heading to the next top-level `## ` heading (or EOF).
    pat = re.compile(
        r"^##\s*\[?" + re.escape(version) + r"\]?.*?$\n(?P<body>.*?)(?=^##\s|\Z)",
        re.MULTILINE | re.DOTALL,
    )
    m = pat.search(text)
    return m.group("body").strip() if m else ""


def extract_changelog(changelog_path, version):
    """Return the release section for `version`, falling back to `Unreleased`.

    The fallback is deliberate: only `core/CHANGELOG.md` is consolidated and renamed
    in the last pre-tag commit, so at tag time a binding's entries legitimately still
    live under `## [Unreleased]`. Without the fallback those packages contribute an
    empty section and their users learn nothing from the release.
    """
    try:
        with open(changelog_path, encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return ""
    return _section(text, version) or _section(text, "Unreleased")


def collect_sections(changelog_paths, version):
    """Return `[(label, section), ...]` for every changelog that has content."""
    out = []
    for path in changelog_paths:
        section = extract_changelog(path, version)
        if section:
            out.append((package_label(path), section))
        else:
            print(f"gen_release_notes: no section in {path}", file=sys.stderr)
    return out


def render_sections(sections):
    """Render the per-package changelog sections as one markdown block."""
    out = []
    for label, section in sections:
        out.append(f"#### {label}")
        out.append("")
        out.append(section)
        out.append("")
    return "\n".join(out).strip()


def ai_summary(version, changelog_section, commits):
    key = os.environ.get("ANTHROPIC_API_KEY")
    if not key:
        return ""
    prompt = (
        f"Write a concise, friendly release announcement for libtracer {version}.\n"
        "2-4 short paragraphs of prose, no markdown headings, no bullet lists. "
        "Lead with what's most meaningful to users. Ground every claim in the "
        "changelog and commits below — do NOT invent features or numbers. "
        "libtracer is a decentralized, zero-copy, graph-based pub/sub protocol "
        "(C++ reference core plus native Rust and TypeScript codecs).\n"
        "The changelog below is grouped per published package; when a change only "
        "affects one package, say which.\n\n"
        f"## Changelog for {version}, by package\n{changelog_section or '(none provided)'}\n\n"
        f"## Commits since the previous release\n{commits or '(none provided)'}\n"
    )
    payload = json.dumps(
        {
            "model": MODEL,
            "max_tokens": 1024,
            "messages": [{"role": "user", "content": prompt}],
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        API_URL,
        data=payload,
        headers={
            "x-api-key": key,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            data = json.load(resp)
        parts = [b.get("text", "") for b in data.get("content", []) if b.get("type") == "text"]
        return "".join(parts).strip()
    except (urllib.error.URLError, TimeoutError, ValueError, KeyError) as exc:
        print(f"gen_release_notes: AI summary skipped ({exc})", file=sys.stderr)
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument(
        "--changelog",
        action="extend",
        nargs="+",
        default=None,
        help="package changelog path(s); repeatable. Default: every package changelog.",
    )
    ap.add_argument("--commits-file")
    args = ap.parse_args()

    sections = collect_sections(args.changelog or DEFAULT_CHANGELOGS, args.version)
    section = render_sections(sections)
    commits = ""
    if args.commits_file and os.path.exists(args.commits_file):
        with open(args.commits_file, encoding="utf-8") as fh:
            commits = fh.read().strip()

    summary = ai_summary(args.version, section, commits)

    out = []
    if summary:
        out.append(summary)
        out.append("")
        out.append("---")
        out.append("")
    if section:
        out.append(f"### Changelog — {args.version}")
        out.append("")
        out.append(section)
    if not out:
        out.append(f"libtracer {args.version}.")
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
