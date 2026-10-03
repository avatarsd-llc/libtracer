#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Cyclomatic-complexity ratchet for ``core/``, plus the agent-doc length cap (#1790).

Every function in ``core/include`` and ``core/src`` is measured with ``lizard`` at the
pinned version below (CCN counting changes between lizard releases, so a different
version is refused rather than compared).

* CEILING. A function above ``CEILING`` that is not pinned fails. Every function that
  was already above it is pinned, so an unpinned offender is new or newly grown.
* RATCHET. A pinned function may only go down. A rise fails. A drop also fails until
  it is re-pinned (``--repin``), so a pin never sits above the truth. ``--repin`` only
  lowers or removes pins; it never adds one. Adding a pin is a hand edit of
  ``tools/ccn_pins.json`` that a reviewer sees.
* PER-FILE EXCESS (report only). Each file's sum of ``max(0, ccn - FILE_EXCESS_FLOOR)``
  is shown against a base revision. Splitting a function into helpers lowers that
  function's score but not this total; deleting or normalizing branches lowers both.
  That is the standing rule: lower complexity by deleting, never by splitting.
* AGENT DOCS. ``CLAUDE.md`` and ``CONTEXT.md`` must stay at ``AGENT_DOC_MAX_LINES``
  lines or fewer. The limit is a constant here, so changing it is a reviewed edit.

Pins are keyed by the qualified function name, not by file, so moving a function
between files keeps its pin. Overloads share a name; their CCNs above the ceiling are
pinned as a list, compared largest to largest.

Only git-tracked files are scanned, so build trees and generated sources never are;
``EXCLUDE`` lists the generated, vendored and third-party patterns on top of that.

Usage::

    python3 tools/check_ccn.py                    # gate + report (exit 1 on a failure)
    python3 tools/check_ccn.py --base origin/main # also report per-file excess vs main
    python3 tools/check_ccn.py --repin            # lower/remove pins that dropped
    python3 tools/check_ccn.py --emit-all         # bootstrap: pin every current offender
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PINS = ROOT / "tools" / "ccn_pins.json"
LIZARD_VERSION = "1.24.0"
CEILING = 15
FILE_EXCESS_FLOOR = 10
AGENT_DOC_MAX_LINES = 100
AGENT_DOCS = ("CLAUDE.md", "CONTEXT.md")
SCAN_ROOTS = ("core/include", "core/src")
SUFFIXES = (".h", ".hpp", ".c", ".cc", ".cpp")
# Generated, vendored and third-party code. None lives under SCAN_ROOTS today; the
# patterns keep it out if some ever does.
EXCLUDE = (
    "*/third_party/*",
    "*/vendor/*",
    "*/external/*",
    "*/generated/*",
    "*_generated.*",
    "*.gen.*",
)


def tracked_sources(root: Path, rev: str | None = None) -> list[str]:
    """@brief Repo-relative C/C++ sources under SCAN_ROOTS, minus EXCLUDE."""
    cmd = ["git", "-C", str(root)]
    cmd += ["ls-tree", "-r", "--name-only", rev, "--"] if rev else ["ls-files", "--"]
    out = subprocess.run(cmd + list(SCAN_ROOTS), capture_output=True, text=True, check=True)
    return sorted(
        p
        for p in out.stdout.splitlines()
        if p.endswith(SUFFIXES) and not any(fnmatch.fnmatch(p, x) for x in EXCLUDE)
    )


def measure(root: Path, rev: str | None = None) -> list[tuple[str, str, int, int]]:
    """@brief (file, qualified name, ccn, line) per function, at the worktree or `rev`."""
    import lizard  # Imported here so the unit tests need no lizard install.
    from lizard_ext import version

    if version != LIZARD_VERSION:
        raise SystemExit(f"lizard {version} installed; the pins need {LIZARD_VERSION}")
    files = tracked_sources(root, rev)
    with tempfile.TemporaryDirectory() as tmp:
        base = root
        if rev:
            base = Path(tmp)
            archive = subprocess.run(
                ["git", "-C", str(root), "archive", rev, "--", *files],
                capture_output=True,
                check=True,
            )
            subprocess.run(["tar", "-x", "-C", tmp], input=archive.stdout, check=True)
        return [
            (rel, f.name, f.cyclomatic_complexity, f.start_line)
            for rel in files
            for f in lizard.analyze_file(str(base / rel)).function_list
        ]


def over_ceiling(functions) -> dict[str, list[int]]:
    """@brief name -> CCNs above CEILING, largest first."""
    out: dict[str, list[int]] = {}
    for _, name, ccn, _ in functions:
        if ccn > CEILING:
            out.setdefault(name, []).append(ccn)
    return {name: sorted(v, reverse=True) for name, v in out.items()}


def check(functions, pins: dict[str, dict]) -> tuple[list[str], list[str]]:
    """@brief (failures, stale pins). A rise or an unpinned offender fails; a drop is stale."""
    measured = over_ceiling(functions)
    failures, stale = [], []
    for name in sorted(set(measured) | set(pins)):
        got = measured.get(name, [])
        pinned = pins.get(name, {}).get("ccn", [])
        for i in range(max(len(got), len(pinned))):
            m = got[i] if i < len(got) else CEILING
            p = pinned[i] if i < len(pinned) else CEILING
            if m > p:
                why = f"pinned at {p}" if i < len(pinned) else f"ceiling is {CEILING}"
                failures.append(f"{name}: CCN {m}, {why}")
            elif m < p:
                stale.append(f"{name}: pinned {p}, now {m} -- re-pin with --repin")
    return failures, stale


def repin(functions, pins: dict[str, dict]) -> dict[str, dict]:
    """@brief Lower each pin to its measured value and drop the ones back under CEILING."""
    measured = over_ceiling(functions)
    files = {name: f for f, name, _, _ in functions}
    out = {}
    for name, pin in pins.items():
        got = measured.get(name, [])
        ccn = [min(p, got[i]) for i, p in enumerate(pin["ccn"]) if i < len(got)]
        if ccn:
            out[name] = {"file": files.get(name, pin["file"]), "ccn": ccn}
    return out


def emit_all(functions) -> dict[str, dict]:
    """@brief Pins for every current offender (bootstrap only)."""
    files = {name: f for f, name, ccn, _ in functions if ccn > CEILING}
    return {name: {"file": files[name], "ccn": v} for name, v in over_ceiling(functions).items()}


def check_agent_docs(root: Path) -> list[str]:
    """@brief Failures for each agent doc longer than AGENT_DOC_MAX_LINES."""
    out = []
    for doc in AGENT_DOCS:
        n = len((root / doc).read_text(encoding="utf-8").splitlines())
        if n > AGENT_DOC_MAX_LINES:
            out.append(f"{doc}: {n} lines, limit {AGENT_DOC_MAX_LINES}")
    return out


def percentile(sorted_values: list[int], q: float) -> int:
    """@brief Nearest-rank percentile of an ascending list."""
    return sorted_values[max(0, math.ceil(q * len(sorted_values)) - 1)]


def file_excess(functions) -> dict[str, int]:
    """@brief file -> sum of CCN above FILE_EXCESS_FLOOR."""
    out: dict[str, int] = {}
    for f, _, ccn, _ in functions:
        out[f] = out.get(f, 0) + max(0, ccn - FILE_EXCESS_FLOOR)
    return out


def report(functions, base_functions=None) -> str:
    """@brief The Markdown job summary: distribution, then per-file excess vs base."""
    v = sorted(c for _, _, c, _ in functions)
    lines = [
        "## Cyclomatic complexity (core/)",
        "",
        "| functions | median | mean | p90 | p99 | max | above 15 | above 30 |",
        "|---|---|---|---|---|---|---|---|",
        f"| {len(v)} | {percentile(v, 0.5)} | {sum(v) / len(v):.1f} | {percentile(v, 0.9)}"
        f" | {percentile(v, 0.99)} | {v[-1]} | {sum(c > 15 for c in v)}"
        f" | {sum(c > 30 for c in v)} |",
    ]
    if base_functions is not None:
        now, was = file_excess(functions), file_excess(base_functions)
        moved = sorted(f for f in set(now) | set(was) if now.get(f, 0) != was.get(f, 0))
        lines += [
            "",
            f"### Per-file CCN above {FILE_EXCESS_FLOOR}, against the base (report only)",
            "",
            f"Total {sum(was.values())} -> {sum(now.values())}.",
        ]
        if moved:
            lines += ["", "| file | base | now | delta |", "|---|---|---|---|"]
            for f in moved:
                a, b = was.get(f, 0), now.get(f, 0)
                lines.append(f"| `{f}` | {a} | {b} | {b - a:+d} |")
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    """@brief CLI entry point."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", help="git revision to report per-file excess against")
    ap.add_argument("--repin", action="store_true", help="lower/remove dropped pins")
    ap.add_argument("--emit-all", action="store_true", help="pin every current offender")
    args = ap.parse_args(argv)

    functions = measure(ROOT)
    doc = json.loads(PINS.read_text()) if PINS.exists() else {"functions": {}}
    if args.emit_all or args.repin:
        doc["functions"] = emit_all(functions) if args.emit_all else repin(functions, doc["functions"])
        doc["lizard"] = LIZARD_VERSION
        PINS.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")
    if doc.get("lizard") != LIZARD_VERSION:
        raise SystemExit(f"{PINS.name} was measured with lizard {doc.get('lizard')}")

    base = measure(ROOT, args.base) if args.base else None
    text = report(functions, base)
    failures, stale = check(functions, doc["functions"])
    failures += check_agent_docs(ROOT)
    if failures or stale:
        text += "\n### Failures\n\n" + "".join(f"- {x}\n" for x in failures + stale)
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(text)
    return 1 if failures or stale else 0


if __name__ == "__main__":
    sys.exit(main())
