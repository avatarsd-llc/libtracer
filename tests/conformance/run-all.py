#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Polyglot conformance driver — run every registered core's harness over the shared
vectors and gate on any divergence.

Each harness emits TAP (see HARNESS.md); this builds an `impl x vector` matrix, prints
it, and exits non-zero if any *enabled* core fails a vector, is missing a vector that
exists on disk, or two enabled cores disagree. Disabled (pending) cores are reported
but do not gate. Stdlib only.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent  # tests/conformance -> repo root
REGISTRY = HERE / "harnesses.json"

# Common build locations to find the C++ harness if $LIBTRACER_CXX_HARNESS is unset.
CXX_FALLBACKS = [
    REPO / "build" / "tests" / "conformance_runner",
    REPO / "core" / "build" / "tests" / "conformance_runner",
    Path("/tmp/lt-build/tests/conformance_runner"),
]

TAP_LINE = re.compile(r"^(ok|not ok)\s+\d+\s*-\s*(.+?)\s*$")


def vectors_on_disk(vectors_dir: Path) -> list[str]:
    """Every vector = a directory containing input.bin, keyed by relative path."""
    out = []
    for p in sorted(vectors_dir.rglob("input.bin")):
        out.append(p.parent.relative_to(vectors_dir).as_posix())
    return out


def expand(cmd: list[str]) -> list[str] | None:
    """Expand ${ENV} tokens; resolve the C++ harness from fallbacks if needed.

    Returns None if a required token cannot be resolved (harness unavailable).
    """
    resolved = []
    for tok in cmd:
        m = re.fullmatch(r"\$\{([A-Z_][A-Z0-9_]*)\}", tok)
        if m:
            val = os.environ.get(m.group(1))
            if not val and m.group(1) == "LIBTRACER_CXX_HARNESS":
                val = next((str(p) for p in CXX_FALLBACKS if p.exists()), None)
            if not val:
                return None
            resolved.append(val)
        else:
            resolved.append(tok)
    return resolved


def run_harness(name: str, cmd: list[str], vectors_dir: Path) -> dict[str, bool]:
    """Run one harness, parse TAP -> {vector_relpath: passed}."""
    full = cmd + [str(vectors_dir)]
    proc = subprocess.run(full, cwd=REPO, capture_output=True, text=True)
    results: dict[str, bool] = {}
    for line in proc.stdout.splitlines():
        m = TAP_LINE.match(line)
        if m:
            results[m.group(2)] = m.group(1) == "ok"
    if not results and proc.returncode != 0:
        print(f"  [{name}] produced no TAP (exit {proc.returncode}):", file=sys.stderr)
        print("    " + (proc.stderr.strip() or proc.stdout.strip())[:500], file=sys.stderr)
    return results


def main() -> int:
    reg = json.loads(REGISTRY.read_text())
    vectors_dir = (REPO / reg["vectors_dir"]).resolve()
    vectors = vectors_on_disk(vectors_dir)
    if not vectors:
        print(f"no vectors under {vectors_dir}", file=sys.stderr)
        return 2

    enabled, pending, unavailable = {}, [], []
    for h in reg["harnesses"]:
        name = h["name"]
        if not h.get("enabled", False):
            pending.append((name, h.get("note", "")))
            continue
        cmd = expand(h["cmd"])
        if cmd is None:
            unavailable.append((name, h.get("note", "")))
            continue
        enabled[name] = run_harness(name, cmd, vectors_dir)

    # --- matrix ---
    names = list(enabled)
    width = max([len(v) for v in vectors] + [len("vector")])
    header = "vector".ljust(width) + "".join(f"  {n:>6}" for n in names)
    print(header)
    print("-" * len(header))

    def cell(passed):  # noqa: ANN001
        return {True: "  ok", False: "  FAIL", None: "  -"}[passed]

    gate_ok = True
    for v in vectors:
        row = v.ljust(width)
        seen = set()
        for n in names:
            r = enabled[n].get(v, None)
            seen.add(r)
            row += f"  {cell(r):>6}"
            if r is not True:  # FAIL or missing in an enabled core gates
                gate_ok = False
        if len([s for s in seen if s is not None]) > 1:  # cores disagree
            gate_ok = False
            row += "  <- DISAGREE"
        print(row)

    # --- summary ---
    print()
    for n in names:
        passed = sum(1 for v in vectors if enabled[n].get(v) is True)
        print(f"  {n}: {passed}/{len(vectors)} vectors ok")
    for n, note in unavailable:
        print(f"  {n}: UNAVAILABLE (enabled but harness not built) — {note}")
        gate_ok = False
    for n, note in pending:
        print(f"  {n}: pending (disabled) — {note}")

    print()
    print("CONFORMANCE: PASS" if gate_ok else "CONFORMANCE: FAIL")
    return 0 if gate_ok else 1


if __name__ == "__main__":
    sys.exit(main())
