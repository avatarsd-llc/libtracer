#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The diagram palette check: every vocabulary class stays legible in both themes (#2378).

`docs/_static/diagram-vocabulary.css` gives each diagram class (frame, record, vertex,
ring, app, peer, notlib) a fill, a stroke and a text colour, once for the light theme and
again for the dark one. The dark values appear twice, for an explicit dark choice and for
the browser's own preference, so each block carries a marker comment:
`/* lt-palette: light */`, `/* lt-palette: dark */` and `/* lt-palette: dark-auto */`.

A finding is printed when:

- a class's text on its own fill is under WCAG AA for normal text (4.5:1);
- a class's stroke against the page background is under 3:1 (WCAG 1.4.11, non-text);
- a class is named in one theme and missing from another;
- the dark and dark-auto blocks disagree on any value.

Exit 1 on any finding. No third-party dependencies.
"""
import pathlib
import re
import sys

VOCABULARY = ("frame", "record", "vertex", "ring", "app", "peer", "notlib")
ROLES = ("fill", "stroke", "text")
# The page each theme draws on: furo's --color-background-primary.
PAGE = {"light": "#ffffff", "dark": "#131416", "dark-auto": "#131416"}
TEXT_MIN = 4.5
STROKE_MIN = 3.0
CSS = pathlib.Path(__file__).resolve().parent.parent / "docs" / "_static" / "diagram-vocabulary.css"

_BLOCK = re.compile(r"/\*\s*lt-palette:\s*([\w-]+)\s*\*/\s*[^{]*\{([^}]*)\}")
_VAR = re.compile(r"--lt-([a-z]+)-(fill|stroke|text)\s*:\s*(#[0-9a-fA-F]{3,6})\s*;")


def _rgb(hex_):
    h = hex_.lstrip("#")
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    return [int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)]


def _luminance(hex_):
    lin = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in _rgb(hex_)]
    return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]


def contrast(a, b):
    """The WCAG 2.x contrast ratio of two hex colours."""
    la, lb = sorted((_luminance(a), _luminance(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


def palette(text):
    """{theme: {class: {role: hex}}} from the marked blocks of the stylesheet."""
    out = {}
    for theme, body in _BLOCK.findall(text):
        classes = out.setdefault(theme, {})
        for cls, role, value in _VAR.findall(body):
            classes.setdefault(cls, {})[role] = value.lower()
    return out


def findings(text):
    pal = palette(text)
    out = [f"{t}: palette block missing" for t in PAGE if t not in pal]
    names = set().union(*(set(c) for c in pal.values())) if pal else set()
    for theme, classes in pal.items():
        for cls in sorted(names - set(classes)):
            out.append(f"{theme}: class {cls} missing")
        for cls, roles in sorted(classes.items()):
            missing = [r for r in ROLES if r not in roles]
            if missing:
                out.append(f"{theme}: {cls} lacks {', '.join(missing)}")
                continue
            ratio = contrast(roles["text"], roles["fill"])
            if ratio < TEXT_MIN:
                out.append(f"{theme}: {cls} text on fill is {ratio:.2f}:1, under {TEXT_MIN}:1")
            ratio = contrast(roles["stroke"], PAGE.get(theme, PAGE["light"]))
            if ratio < STROKE_MIN:
                out.append(f"{theme}: {cls} stroke on page is {ratio:.2f}:1, under {STROKE_MIN}:1")
    if "dark" in pal and "dark-auto" in pal and pal["dark"] != pal["dark-auto"]:
        out.append("dark-auto: values differ from the dark block")
    return out


def main(argv):
    path = pathlib.Path(argv[1]) if len(argv) > 1 else CSS
    found = findings(path.read_text(encoding="utf-8"))
    for f in found:
        print(f"{path}: {f}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
