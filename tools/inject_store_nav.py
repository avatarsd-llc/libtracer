#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Overlay a store-navigation banner onto the mirrored benchmark-action pages.

The two raw trend browsers served at `<site>/dev/bench/` and `<site>/dev/bench-local/`
are stock `github-action-benchmark` output. Each renders ONE store and says nothing
about the other, so a reader who lands on either has no way to discover that a second
store exists, or that the unified family charts on the Performance page read both —
which is exactly the conclusion a maintainer drew from that page: "there is no selector".

The stores live on the machine-maintained `gh-pages` branch, which is never modified.
This runs on the COPY already extracted into the Pages artifact by docs.yml's mirror
step, so the branch stays byte-identical and the served pages gain the nav.

Fail-soft by contract. The stock page is upstream's and may change shape at any release;
a docs deploy must never fail because an anchor moved. A file that is missing, already
carries the banner, or has no recognizable `<body>` is left exactly as it is and reported
as a GitHub Actions warning. Stdlib only, same doctrine as the rest of bench/tools.

Usage: python3 tools/inject_store_nav.py <site-root>
"""
from __future__ import annotations

import pathlib
import re
import sys

# Idempotence marker. Checked before insertion, so a re-run over an already-overlaid
# tree is a no-op rather than a stack of banners.
MARKER = "<!-- lt-store-nav -->"

# The insertion point: immediately after the opening <body>, before the stock header.
# Deliberately the loosest anchor that can carry the banner — any attribute set, any
# whitespace — because the narrower the pattern, the more upstream edits break it.
ANCHOR = re.compile(r"<body\b[^>]*>", re.I)

# Where each mirrored page sits and how it names itself in the nav.
PAGES = [
    ("dev/bench/index.html", "hosted"),
    ("dev/bench-local/index.html", "local"),
]

# Self-contained: inline styles, no asset of its own, nothing fetched. The stock page
# loads its chart library from a CDN; this banner adds no third party of its own and
# must keep working if that one is blocked.
STYLE = (
    "font:13px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
    "background:#f4f6fa;border:1px solid #dbe1ea;border-radius:8px;"
    "padding:8px 12px;margin:0 0 12px;color:#5a6473;"
    "display:flex;gap:10px;align-items:center;flex-wrap:wrap"
)
LINKSTYLE = "color:#3273dc;text-decoration:none;font-weight:600"
HERESTYLE = "color:#1a212b;font-weight:700"


def _link(href: str, text: str, here: bool) -> str:
    """@brief One nav entry — a link, or plain bold text when it names this page."""
    if here:
        return f'<span style="{HERESTYLE}">{text}</span>'
    return f'<a href="{href}" style="{LINKSTYLE}">{text}</a>'


def banner(which: str) -> str:
    """@brief The nav fragment for one mirrored store page.

    Names all three views of the same benchmarks — the two raw stores and the unified
    family charts — and marks which one the reader is on. The Performance-page link is
    absolute-from-root rather than relative: both mirrored pages sit two directories
    deep, and one `../..` typo would ship a dead link on the busiest page of the site.
    """
    return (
        f'{MARKER}<div style="{STYLE}">'
        '<span style="text-transform:uppercase;letter-spacing:.06em;font-size:11px">source</span>'
        + _link("/dev/bench/", "GitHub-hosted", which == "hosted")
        + '<span>|</span>'
        + _link("/dev/bench-local/", "bench-local", which == "local")
        + '<span>|</span>'
        # The Performance page has no chapter called "history"; the section that
        # introduces the unified family charts is "Every chart on this page is the same
        # chart", and its MyST heading anchor is what this points at. A link to a section
        # that exists beats a link to the name someone expected it to have.
        + _link("/docs/performance.html#every-chart-on-this-page-is-the-same-chart",
                "unified family views on performance.html", False)
        + '<span style="font-size:11.5px">— this page is the raw per-series archive of '
          'ONE store; the Performance page charts both, grouped into families.</span>'
        "</div>"
    )


def overlay(root: pathlib.Path) -> int:
    """@brief Insert the banner into every mirrored store page under @p root.

    Returns the number of pages modified. Never raises on a page it cannot handle: each
    skip is reported as an Actions warning and the page ships unmodified, because a
    cosmetic banner is not worth a failed docs deploy.
    """
    done = 0
    for rel, which in PAGES:
        p = root / rel
        if not p.is_file():
            print(f"::warning::store nav: {rel} not present in the mirror; skipped")
            continue
        try:
            html = p.read_text(encoding="utf-8")
        except OSError as e:
            print(f"::warning::store nav: {rel} unreadable ({e}); shipped unmodified")
            continue
        if MARKER in html:
            done += 1  # already overlaid — idempotent re-run, not a failure
            continue
        m = ANCHOR.search(html)
        if not m:
            print(f"::warning::store nav: no <body> anchor in {rel} "
                  "(upstream page changed shape); shipped unmodified")
            continue
        out = html[: m.end()] + "\n    " + banner(which) + html[m.end():]
        try:
            p.write_text(out, encoding="utf-8")
        except OSError as e:
            print(f"::warning::store nav: {rel} not writable ({e}); shipped unmodified")
            continue
        done += 1
    return done


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__)
        return 2
    n = overlay(pathlib.Path(argv[1]))
    print(f"store nav: {n} mirrored page(s) carry the selector banner")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
