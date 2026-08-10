#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Symbol-size ratchet on the hot dispatch symbols — a TRIPWIRE, not a proof (#1155).

`dispatch_edge_target` is a standing tripwire in this repo: #1065/#1078 landed on it, then
#1086 one day later, then #1129 had to undo the cause. Every time it was found by a bench run
*after the fact*, and #1086's own history shows the failure mode — the implementer reported
`bench_libtracer` unusable and rested on disassembly taken at `-O2`, which was false at `-O3`.

What this measures is **code shape, not time**. A header edit that re-partitions a TU's inline
budget shows up here at review time; a regression with no size signature does not show up here
at all and still needs the bench. Read a failure as "something moved, go price it", never as
"this is slower".

Why it cannot flake: compilation is deterministic, so two builds of the same source at the same
flags produce byte-identical sizes. The A/A null on this instrument is exactly **0**, which is
what licenses a zero-tolerance comparison.

TOOLCHAIN-BOUND. Symbol sizes are a property of the compiler, not of the source: this repo has
already measured 955 B of spread across toolchains on a footprint number. The pin therefore
records the toolchain it was taken on and the gate REFUSES to compare against a different one
rather than reporting a difference it cannot attribute.

RATCHET, NOT CEILING (the standing rule): pins are the MEASURED values. Growth fails. A shrink
also fails, with a re-pin instruction — a pin left above the truth is how 24 B and 8 B of drift
went unnoticed here before, and prose beside a passing gate rots.

    ./symbol_ratchet.py --build bench/build --pins bench/symbol_ratchet.json
    ./symbol_ratchet.py --build bench/build --pins bench/symbol_ratchet.json --emit
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

# A clone suffix is a hot/cold or partial split of the SAME function: `[clone .cold]`,
# `[clone .isra.0]`, `[clone .part.0]`. Sizes are summed per function so that a split moves
# bytes between clones without the gate reading it as a change it cannot explain.
CLONE = re.compile(r"\s*\[clone\s+\.[^\]]*\]")


def toolchain_id(cxx="g++"):
    """@brief The compiler identity a pin is only comparable within."""
    out = subprocess.run([cxx, "--version"], capture_output=True, text=True, check=False)
    return out.stdout.splitlines()[0].strip() if out.stdout else "unknown"


def symbol_sizes(binary):
    """@brief Map "demangled name with clone suffixes stripped" -> summed size in bytes."""
    out = subprocess.run(
        ["nm", "-S", "-C", str(binary)], capture_output=True, text=True, check=False
    )
    if out.returncode != 0:
        raise SystemExit(f"nm failed on {binary}: {out.stderr.strip()}")
    sizes = {}
    for line in out.stdout.splitlines():
        parts = line.split(" ", 3)
        # "<addr> <size> <type> <name>" — entries without a size have three fields.
        if len(parts) < 4 or not re.fullmatch(r"[0-9a-fA-F]+", parts[1] or ""):
            continue
        name = CLONE.sub("", parts[3]).strip()
        sizes[name] = sizes.get(name, 0) + int(parts[1], 16)
    return sizes


def measure(build, pins):
    """@brief Resolve every pinned symbol to its measured size; unresolved stays None."""
    cache, measured = {}, []
    for p in pins["symbols"]:
        binary = Path(build) / p["binary"]
        if not binary.exists():
            measured.append({**p, "measured": None, "why": f"{binary} not built"})
            continue
        if p["binary"] not in cache:
            cache[p["binary"]] = symbol_sizes(binary)
        table = cache[p["binary"]]
        # Match on the demangled prefix: the pin names the function, not its full signature,
        # so a parameter-type spelling change does not silently orphan a pin.
        hits = [n for n in table if n.startswith(p["symbol"])]
        if not hits:
            measured.append({**p, "measured": None, "why": "symbol not found"})
            continue
        measured.append({**p, "measured": sum(table[h] for h in hits), "why": ""})
    return measured


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", required=True, help="the bench build directory")
    ap.add_argument("--pins", required=True)
    ap.add_argument("--emit", action="store_true", help="print the measured pin file and exit 0")
    ap.add_argument("--cxx", default="g++")
    args = ap.parse_args()

    pins = json.loads(Path(args.pins).read_text())
    rows = measure(args.build, pins)
    tc = toolchain_id(args.cxx)

    if args.emit:
        print(json.dumps({**pins, "toolchain": tc,
                          "symbols": [{k: v for k, v in r.items()
                                       if k in ("binary", "symbol", "bytes")}
                                      | ({"bytes": r["measured"]} if r["measured"] else {})
                                      for r in rows]}, indent=2))
        for r in rows:
            print(f"# {r['symbol']}\t{r['measured']}\t{r['why']}", file=sys.stderr)
        return 0

    print(f"SYMBOL_RATCHET\ttoolchain={tc}")
    if tc != pins["toolchain"]:
        # Not a pass and not a size failure: an unattributable comparison, refused.
        print("SYMBOL_RATCHET\tFAIL\treason=toolchain-mismatch")
        print(f"  pinned on: {pins['toolchain']}")
        print(f"  measured on: {tc}")
        print("  Symbol sizes are a property of the compiler. Re-pin on the gate's toolchain")
        print("  (--emit) or run this gate where the pin was taken.")
        return 1

    bad, table = [], []
    for r in rows:
        got, want = r["measured"], r["bytes"]
        if got is None:
            bad.append(f"{r['symbol']} — {r['why']}")
            table.append(f"  {r['symbol']}\tMISSING\t(pinned {want})")
            continue
        delta = got - want
        mark = "ok" if delta == 0 else ("GREW" if delta > 0 else "SHRANK")
        table.append(f"  {r['symbol']}\t{got} B\tpinned {want} B\t{delta:+d}\t{mark}")
        if delta > 0:
            bad.append(f"{r['symbol']} grew {delta} B ({want} -> {got})")
        elif delta < 0:
            bad.append(f"{r['symbol']} shrank {-delta} B ({want} -> {got}) — RE-PIN to {got}")
    print("\n".join(table))

    if bad:
        print("SYMBOL_RATCHET\tFAIL")
        for b in bad:
            print(f"  - {b}")
        print("")
        print("  This gate measures CODE SHAPE, not time. A growth here is not a proof of a")
        print("  latency regression and a flat gate is not a proof of its absence — it means")
        print("  something re-partitioned the inline budget, and it is worth pricing on the")
        print("  bench before it lands. A shrink is an improvement: re-pin it (--emit) so the")
        print("  ratchet keeps holding the new floor.")
        return 1
    print("SYMBOL_RATCHET\tPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
