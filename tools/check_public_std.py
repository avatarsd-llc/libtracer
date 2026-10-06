#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""No owning std type in the installed core headers: a ratchet toward zero (#1781).

ADR-0083 Decision 11: no owning std type crosses core's public API. Inputs are views and
outputs go into caller buffers or core containers (``tr::mem::bytes_t``,
``tr::mem::block_array_t``, ``tr::mem::string_t``). Every header under
``core/include/libtracer`` is installed, so this scans all of their code: signatures,
public and private members, and inline bodies. A header's inline body is compiled into
the embedder's translation unit, so an owning std type there reaches the embedder's heap
exactly as a signature would.

Comments and string literals are stripped first, so prose that names a std type (for
history, or for the host-side spelling a caller may keep) is never counted.

* RATCHET. ``tools/public_std_baseline.json`` pins each header's count per std type,
  with the ticket that removes it. A count may only go down. A rise fails, and so does a
  header or a type the baseline does not list. A drop also fails until ``--repin``
  lowers the pin, so the baseline never sits above the truth. ``--repin`` only lowers
  or removes pins; it never adds one.
* ZERO is the acceptance of #1781: once the baseline is empty, the scan finds no owning
  std type in any installed header.

Usage::

    python3 tools/check_public_std.py            # gate (exit 1 on a failure)
    python3 tools/check_public_std.py --list     # print every hit with its line
    python3 tools/check_public_std.py --repin    # lower/remove pins that dropped
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "tools" / "public_std_baseline.json"
SCAN_ROOT = "core/include/libtracer"

# The owning std types. A view (`string_view`, `span`), a non-owning callable reference,
# `array`, `optional`, `expected` and `pmr::memory_resource` own no heap block of their own,
# so they are not listed. `make_shared` / `make_unique` mint an owning pointer, so they
# count even where the pointer type is spelled `auto`.
OWNING = (
    "string", "wstring", "u8string", "u16string", "u32string", "basic_string",
    "vector", "deque", "list", "forward_list", "queue", "stack", "priority_queue",
    "map", "multimap", "set", "multiset",
    "unordered_map", "unordered_multimap", "unordered_set", "unordered_multiset",
    "function", "move_only_function", "any",
    "unique_ptr", "shared_ptr", "make_unique", "make_shared",
    "stringstream", "ostringstream", "istringstream",
)
PATTERN = re.compile(r"\bstd::(?:pmr::)?(" + "|".join(OWNING) + r")\b(?![\w])")


def strip(text: str) -> str:
    """@brief Blank comments, string and character literals, keeping line numbers."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c == "R" and nxt == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            k = text.find("(", i + 2)
            delim = ")" + text[i + 2:k] + '"'
            j = text.find(delim, k)
            j = n if j < 0 else j + len(delim)
            out.append('""' + "\n" * text.count("\n", i, j))
            i = j
        elif c == "'" and i > 0 and text[i - 1].isalnum():
            out.append(c)  # a digit separator (`1'000`), not a character literal
            i += 1
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def scan_text(text: str) -> list[tuple[int, str, str]]:
    """@brief Every owning std type in @p text as (line, type, stripped line)."""
    hits = []
    for lineno, line in enumerate(strip(text).split("\n"), 1):
        for m in PATTERN.finditer(line):
            hits.append((lineno, m.group(1), line.strip()))
    return hits


def scan_tree(root: Path = ROOT) -> dict[str, list[tuple[int, str, str]]]:
    """@brief Every installed header's hits, keyed by its repo-relative path."""
    found = {}
    for path in sorted((root / SCAN_ROOT).rglob("*")):
        if path.suffix not in (".h", ".hpp") or not path.is_file():
            continue
        hits = scan_text(path.read_text(encoding="utf-8"))
        if hits:
            found[path.relative_to(root).as_posix()] = hits
    return found


def counts(found: dict[str, list[tuple[int, str, str]]]) -> dict[str, dict[str, int]]:
    """@brief The per-header, per-type counts the baseline pins."""
    out: dict[str, dict[str, int]] = {}
    for f, hits in found.items():
        per = out.setdefault(f, {})
        for _, t, _ in hits:
            per[t] = per.get(t, 0) + 1
    return out


def check(measured: dict[str, dict[str, int]], pins: dict) -> list[str]:
    """@brief The ratchet's failures: a rise, an unpinned hit, or a stale (dropped) pin."""
    errors = []
    for f, per in sorted(measured.items()):
        pinned = pins.get(f, {}).get("counts", {})
        for t, n in sorted(per.items()):
            p = pinned.get(t)
            if p is None:
                errors.append(f"{f}: std::{t} x{n} is new (not in the baseline)")
            elif n > p:
                errors.append(f"{f}: std::{t} rose {p} -> {n}")
            elif n < p:
                errors.append(f"{f}: std::{t} dropped {p} -> {n}; run --repin")
    for f, entry in sorted(pins.items()):
        for t, p in sorted(entry.get("counts", {}).items()):
            if measured.get(f, {}).get(t, 0) == 0:
                errors.append(f"{f}: std::{t} dropped {p} -> 0; run --repin")
    return errors


def repin(measured: dict[str, dict[str, int]], pins: dict) -> dict:
    """@brief Lower every pin to its measured count and drop the zeros; never add or raise."""
    out = {}
    for f, entry in pins.items():
        kept = {}
        for t, p in entry.get("counts", {}).items():
            n = measured.get(f, {}).get(t, 0)
            if n > 0:
                kept[t] = min(n, p)
        if kept:
            out[f] = dict(entry, counts=dict(sorted(kept.items())))
    return dict(sorted(out.items()))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--list", action="store_true", help="print every hit with its line")
    ap.add_argument("--repin", action="store_true", help="lower/remove pins that dropped")
    args = ap.parse_args(argv)

    found = scan_tree()
    measured = counts(found)
    pins = json.loads(BASELINE.read_text(encoding="utf-8")) if BASELINE.exists() else {}

    if args.list:
        for f, hits in found.items():
            for lineno, t, line in hits:
                print(f"{f}:{lineno}: std::{t}: {line}")
    if args.repin:
        BASELINE.write_text(json.dumps(repin(measured, pins), indent=2) + "\n", encoding="utf-8")
        pins = json.loads(BASELINE.read_text(encoding="utf-8"))

    total = sum(sum(per.values()) for per in measured.values())
    errors = check(measured, pins)
    for e in errors:
        print(f"FAIL {e}")
    print(f"public-std: {total} owning std type(s) in {len(measured)} installed header(s); "
          f"{len(errors)} failure(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
