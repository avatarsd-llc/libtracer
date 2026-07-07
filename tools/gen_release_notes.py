#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Build the GitHub Release body for a libtracer tag.

Emits (to stdout): an AI-written prose summary of the release, followed by the
verbatim ``## [X.Y.Z]`` section extracted from ``core/CHANGELOG.md``. The AI
summary uses the Anthropic Messages API (key from ``ANTHROPIC_API_KEY``); if the
key is absent or the call fails for any reason, it degrades gracefully to just the
CHANGELOG section — this script must NEVER fail a release.

Usage::

  tools/gen_release_notes.py --version X.Y.Z [--changelog core/CHANGELOG.md] \\
      [--commits-file commits.txt] > body.md

``--commits-file`` is an optional plain list of commit subjects since the previous
tag (the workflow produces it with ``git log``); it gives the model extra grounding.
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


def extract_changelog(changelog_path, version):
    """Return the body of the `## [version]` section (excludes the heading)."""
    try:
        text = open(changelog_path, encoding="utf-8").read()
    except OSError:
        return ""
    # Match from the version heading to the next top-level `## ` heading (or EOF).
    pat = re.compile(
        r"^##\s*\[?" + re.escape(version) + r"\]?.*?$\n(?P<body>.*?)(?=^##\s|\Z)",
        re.MULTILINE | re.DOTALL,
    )
    m = pat.search(text)
    return m.group("body").strip() if m else ""


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
        "(C++ reference core plus native Rust and TypeScript codecs).\n\n"
        f"## Changelog for {version}\n{changelog_section or '(none provided)'}\n\n"
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
    ap.add_argument("--changelog", default="core/CHANGELOG.md")
    ap.add_argument("--commits-file")
    args = ap.parse_args()

    section = extract_changelog(args.changelog, args.version)
    commits = ""
    if args.commits_file and os.path.exists(args.commits_file):
        commits = open(args.commits_file, encoding="utf-8").read().strip()

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
