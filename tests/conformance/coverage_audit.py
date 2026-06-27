#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Conformance vector coverage audit (ADR-0028 / #60, the non-fuzzing half).

Walks every vector's input.bin as a TLV tree and credits every *type code* and
every *opt bit* it exercises (including nested children). Then reports coverage
against the full v1 wire surface so that "all vectors green" can be backed by a
statement of *what* the green actually covers.

This is the coverage half of conformance Phase 2. The differential-fuzzing half
(encode in core X -> decode in core Y) needs >=2 native cores and is gated on the
TypeScript (#56) and Rust (#57) cores landing.

Report-only by default (exit 0). Pass --strict to exit non-zero on any gap, so a
future CI step can enforce full coverage once the missing vectors are authored.
Stdlib only; no codec dependency — it parses structure (type/opt/length), not CRC.
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
VECTORS = HERE / "vectors" / "v1"

# The v1 core type-code registry (docs/reference/05-protocol-tlvs.md). 0x05 is
# retired (LIST, ADR-0003) and intentionally absent.
TYPE_NAMES = {
    0x01: "VALUE",
    0x02: "NAME",
    0x03: "DESCRIPTION",
    0x04: "SUBSCRIBER",
    0x06: "PATH",
    0x07: "POINT",
    0x08: "ERROR",
    0x09: "STATUS",
    0x0A: "ACL",
    0x0B: "SETTINGS",
    0x0C: "TIME",
    0x0D: "ROUTER",
    0x0E: "SPEC",
}

# opt bit -> (mask, name). Bits 7 and 0 are reserved-must-be-zero (not coverage
# targets — there is no valid vector that sets them).
OPT_BITS = [
    (0x40, "PL"),
    (0x20, "TS"),
    (0x10, "CR"),
    (0x08, "LL"),
    (0x04, "CW"),
    (0x02, "TF"),
]


class ParseError(Exception):
    pass


def walk(buf: bytes, types: set[int], opts: set[int], depth: int = 0) -> None:
    """Credit every type code and opt bit in this TLV and its children."""
    if depth > 32:
        raise ParseError("nesting > 32")
    if len(buf) < 4:
        raise ParseError("truncated header")
    type_b = buf[0]
    opt_b = buf[1]
    types.add(type_b)
    for mask, _name in OPT_BITS:
        if opt_b & mask:
            opts.add(mask)
    ll = bool(opt_b & 0x08)
    pl = bool(opt_b & 0x40)
    header = 6 if ll else 4
    if len(buf) < header:
        raise ParseError("truncated extended header")
    length = int.from_bytes(buf[2:header], "little")
    payload = buf[header : header + length]
    if len(payload) != length:
        raise ParseError("truncated payload")
    if pl:
        pos = 0
        while pos < len(payload):
            pos += walk_one(payload[pos:], types, opts, depth + 1)


def walk_one(buf: bytes, types: set[int], opts: set[int], depth: int) -> int:
    """Walk one child, returning the bytes it consumed (header+length, no trailer)."""
    if len(buf) < 4:
        raise ParseError("truncated child header")
    opt_b = buf[1]
    ll = bool(opt_b & 0x08)
    ts = bool(opt_b & 0x20)
    cr = bool(opt_b & 0x10)
    cw = bool(opt_b & 0x04)
    tf = bool(opt_b & 0x02)
    header = 6 if ll else 4
    length = int.from_bytes(buf[2:header], "little")
    ts_size = (4 if tf else 8) if ts else 0
    crc_size = (2 if cw else 4) if cr else 0
    total = header + length + ts_size + crc_size
    walk(buf[: header + length + ts_size + crc_size], types, opts, depth)
    return total


def main() -> int:
    strict = "--strict" in sys.argv[1:]
    seen_types: set[int] = set()
    seen_opts: set[int] = set()
    n_vectors = 0
    errors = []
    for input_bin in sorted(VECTORS.rglob("input.bin")):
        n_vectors += 1
        try:
            walk(input_bin.read_bytes(), seen_types, seen_opts)
        except ParseError as e:
            errors.append(f"{input_bin.parent.relative_to(VECTORS).as_posix()}: {e}")

    missing_types = [t for t in TYPE_NAMES if t not in seen_types]
    missing_opts = [(m, n) for (m, n) in OPT_BITS if m not in seen_opts]

    print(f"Conformance coverage audit — {n_vectors} vectors\n")
    print(f"Type codes: {len(seen_types & set(TYPE_NAMES))}/{len(TYPE_NAMES)} covered")
    for t, name in TYPE_NAMES.items():
        mark = "ok  " if t in seen_types else "GAP "
        print(f"  [{mark}] 0x{t:02X} {name}")
    print(f"\nOpt bits: {len(seen_opts)}/{len(OPT_BITS)} covered")
    for mask, name in OPT_BITS:
        mark = "ok  " if mask in seen_opts else "GAP "
        print(f"  [{mark}] {name} (0x{mask:02X})")

    if errors:
        print("\nParse errors:")
        for e in errors:
            print(f"  {e}")

    gaps = len(missing_types) + len(missing_opts) + len(errors)
    print()
    if gaps == 0:
        print("COVERAGE: COMPLETE")
        return 0
    miss_t = ", ".join(f"0x{t:02X} {TYPE_NAMES[t]}" for t in missing_types) or "none"
    miss_o = ", ".join(n for _m, n in missing_opts) or "none"
    print(f"COVERAGE: PARTIAL — missing type codes: {miss_t}; missing opt bits: {miss_o}")
    print("(Authoring vectors for the gaps is tracked in #60; the differential-fuzzing")
    print(" half needs a 2nd native core — #56/#57.)")
    return 1 if strict else 0


if __name__ == "__main__":
    sys.exit(main())
