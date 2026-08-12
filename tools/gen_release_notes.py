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

The body is capped at ``--max-chars`` (see ``GITHUB_BODY_LIMIT``). A release body over
GitHub's limit is rejected with ``HTTP 422: body is too long``, which is how the v0.9.0
release object failed **after all four package registries had already published** — an
un-retryable half-published state. The cap is therefore the script's job, not the
workflow's: only the script knows where an entry ends.
"""
import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request

API_URL = "https://api.anthropic.com/v1/messages"

# GitHub rejects a release body over 125 000 characters (HTTP 422). We stop short of
# it: the AI summary is generated after the sections are measured, and a few hundred
# characters of drift between what we count and what the API counts must not cost a
# release. The margin is the only reason this is not simply 125000.
GITHUB_BODY_LIMIT = 120_000
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


def render_sections(sections, budget=None):
    """Render the per-package changelog sections as one markdown block.

    With a @p budget (characters), every package is trimmed to a max-min FAIR share
    rather than first-come-first-served: core is an order of magnitude larger than the
    bindings, so a running cut would spend the whole budget on core and publish the
    npm/crates/ESP sections as nothing at all — which is #676's failure exactly, the
    one this tool exists to prevent. Returns ``(text, truncated)``.
    """
    if budget is not None:
        overhead = sum(len(f"#### {label}\n\n\n") for label, _ in sections)
        sections = share_budget(sections, max(budget - overhead, 0))
    truncated = any(t for _, _, t in sections) if budget is not None else False
    out = []
    for label, section, *_ in sections:
        out.append(f"#### {label}")
        out.append("")
        out.append(section)
        out.append("")
    text = "\n".join(out).strip()
    return (text, truncated) if budget is not None else text


def share_budget(sections, budget):
    """Max-min fair share of @p budget across @p sections.

    Smallest first: each package may take an equal share of what is left, and a
    package that needs less than its share hands the surplus to the ones still
    hungry. The result is that the small binding changelogs always survive whole and
    only the largest package (core) is cut.

    Returns ``[(label, text, truncated), ...]`` in the input order.
    """
    order = sorted(range(len(sections)), key=lambda i: len(sections[i][1]))
    out = [None] * len(sections)
    left = budget
    for n, i in enumerate(order):
        label, text = sections[i]
        share = left // (len(order) - n)
        kept, cut = trim_to_entry_boundary(text, share)
        out[i] = (label, kept, cut)
        left -= len(kept)
    return out


def changelog_links(changelog_paths, version):
    """Return a markdown link per package changelog, pinned to this release's tag."""
    slug = os.environ.get("GITHUB_REPOSITORY", "avatarsd-llc/libtracer")
    base = f"https://github.com/{slug}/blob/v{version}/"
    return [f"[{package_label(p)}]({base}{p.replace(os.sep, '/')})" for p in changelog_paths]


def trim_to_entry_boundary(section, budget):
    """Trim @p section to at most @p budget characters, ending on a whole entry.

    Cutting mid-entry would publish half a sentence as if it were the whole change, so
    the cut walks back to the last line that OPENS something — a heading or a
    top-level bullet — and drops that line too, since everything from it on is the
    partial entry. Returns ``(text, truncated)``; ``truncated`` is False when the
    section already fits.
    """
    if len(section) <= budget:
        return section, False
    # Split into blocks first and ACCUMULATE, rather than slicing to the budget and
    # walking back: only the block list knows whether the entry straddling the cut
    # ended just after it or ran on for another paragraph.
    blocks = []
    for line in section.split("\n"):
        if not blocks or line.startswith("#") or line.startswith("- "):
            blocks.append([line])
        else:
            blocks[-1].append(line)
    kept, used = [], 0
    for block in blocks:
        text = "\n".join(block)
        cost = len(text) + (1 if kept else 0)
        if used + cost > budget:
            break
        kept.append(text)
        used += cost
    while kept and kept[-1].lstrip().startswith("#"):  # no heading over an empty list
        kept.pop()
    return "\n".join(kept).rstrip(), True


def ai_summary(version, changelog_section, commits):
    key = os.environ.get("ANTHROPIC_API_KEY")
    if not key:
        return ""
    prompt = (
        f"Write a concise, friendly release announcement for libtracer {version}.\n"
        "2-4 short paragraphs of prose, no markdown headings, no bullet lists. "
        "Lead with what's most meaningful to users. Answer, in simple words, the "
        "question a returning user asks first: what are the actual improvements — "
        "the precise logic/behavior changes — over the previous release? Name what "
        "the code now does differently, not just that files changed. Ground every "
        "claim in the changelog and commits below — do NOT invent features or "
        "numbers. "
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
    ap.add_argument(
        "--max-chars",
        type=int,
        default=GITHUB_BODY_LIMIT,
        help=f"cap the emitted body at this many characters (default {GITHUB_BODY_LIMIT}).",
    )
    args = ap.parse_args()

    paths = args.changelog or DEFAULT_CHANGELOGS
    sections = collect_sections(paths, args.version)
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
        heading = f"### Changelog — {args.version}"
        notice = (
            "> **Truncated** — the full release body exceeded GitHub's 125 000-character "
            "limit. Nothing is missing from the repository; the complete changelogs for "
            "this tag: " + ", ".join(changelog_links(paths, args.version)) + "."
        )
        # The summary is never trimmed: it is small, it is the part written FOR this
        # page, and a reader who loses it loses the only thing the changelogs do not say.
        budget = args.max_chars - len("\n".join(out + [heading, "", "", notice, ""]))
        section, truncated = render_sections(sections, max(budget, 0))
        out.append(heading)
        out.append("")
        out.append(section)
        if truncated:
            out.append("")
            out.append(notice)
            print(
                f"gen_release_notes: body over {args.max_chars} chars — changelog truncated",
                file=sys.stderr,
            )
    if not out:
        out.append(f"libtracer {args.version}.")
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
