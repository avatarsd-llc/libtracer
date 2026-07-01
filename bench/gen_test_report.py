#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Generate docs/test-report.md from a LIVE ctest run — a rich, auto-generated,
published test report across every C++ suite in the repo, categorized by layer,
with per-suite pass/fail + timing, plus the conformance-vector count, the sanitizer
matrix, and the 16KB zero-heap forward gate.

Runs `ctest --output-junit` against the Release build, parses the JUnit XML, buckets
the suites by subsystem (codec / substrate / graph / net / transport / examples /
conformance), and renders a MyST page: a top-line rollup, a per-category table, and a
"how it's verified" matrix (Release + ASan/UBSan + TSan + the zero-heap gate). CI
regenerates this in-place before sphinx-build (docs.yml); the committed copy is the
last snapshot. Degrades gracefully if a build dir is missing (a note, not a crash).

  python3 bench/gen_test_report.py            # write docs/test-report.md
Env: LIBTRACER_CORE_BUILD (Release build dir; defaults to core/build). Stdlib only.
"""
from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import xml.etree.ElementTree as ET

REPO = pathlib.Path(__file__).resolve().parent.parent
OUT = REPO / "docs" / "test-report.md"
BUILD = pathlib.Path(os.environ.get("LIBTRACER_CORE_BUILD") or (REPO / "core" / "build"))
VECTORS = REPO / "tests" / "conformance" / "vectors" / "v1"

# Suite name -> (category, one-line what-it-covers). Names match core/tests/CMakeLists
# add_test(NAME ...). A suite not listed here still shows under "other".
SUITES = {
    "byteorder":            ("Codec (L2/L3)", "little-endian load/store + string-view helpers"),
    "frame":                ("Codec (L2/L3)", "TLV encode/decode, CRC, trailer round-trip"),
    "ws":                   ("Codec (L2/L3)", "WebSocket RFC 6455 frame codec (mask/unmask, fragments)"),
    "conformance":          ("Codec (L2/L3)", "the shared cross-core vector suite (input.bin → expected)"),
    "can_frames":           ("Codec (L2/L3)", "CAN 29-bit ID + view_can_frames split/reassemble"),
    "path":                 ("Substrate (L0/L1)", "path parse/canonicalize, PathKey, field-path"),
    "substrate":            ("Substrate (L0/L1)", "segment/view/rope, refcount, backends"),
    "substrate_no_atomic":  ("Substrate (L0/L1)", "the NO_ATOMIC single-core refcount build"),
    "cuda":                 ("Substrate (L0/L1)", "device-memory views + heterogeneous rope (opt-in)"),
    "graph":                ("Graph (L4)", "roles, lock-free LKV, read/write/await, fan-out, field-write"),
    "children":             ("Graph (L4)", ":children[] SPEC vertex creation (ADR-0017/#82)"),
    "acl":                  ("Graph (L4)", ":acl structural storage (ADR-0018/0020)"),
    "op_resolve":           ("Net (FWD plane)", "terminus op resolution + zero-copy FWD{REPLY} (RFC-0004)"),
    "fwd_multihop":         ("Net (FWD plane)", "multi-hop forward: dst-shrink / src-grow byte-exact"),
    "fwd_compact":          ("Net (FWD plane)", "route-handle label compaction + self-heal (RFC-0004 §E.1)"),
    "fwd_fanout":           ("Net (FWD plane)", "producer remote fan-out + delivery_compact (#136)"),
    "transport_vertex":     ("Net (FWD plane)", "transport/connection as a / vertex (ADR-0027/#83)"),
    "bridge":               ("Net (ROUTER plane)", "ROUTER wrap/unwrap, dedup, hop_count, status (M4/#77)"),
    "transport_can":        ("Transport", "CAN classic + CAN-FD framing"),
    "transport_can_vcan":   ("Transport", "SocketCAN over a vcan loopback (E2E)"),
    "udp":                  ("Transport", "UDP socket transport, two-node E2E"),
    "ws_transport":         ("Transport", "WebSocket RFC 6455 codec + transport"),
    "example_in_process_pubsub": ("Examples", "the in-process pub/sub example"),
    "example_two_node_loopback": ("Examples", "the two-node loopback example"),
    "example_udp_two_node":      ("Examples", "the two-node UDP example"),
}
CATEGORY_ORDER = ["Codec (L2/L3)", "Substrate (L0/L1)", "Graph (L4)", "Net (FWD plane)",
                  "Net (ROUTER plane)", "Transport", "Examples", "other"]


def run_ctest_junit(build: pathlib.Path) -> list[tuple[str, str, float]]:
    """Return [(name, status, time_s)] from a ctest JUnit run, or [] if unavailable."""
    if not build.exists():
        return []
    junit = build / "ctest-junit.xml"
    subprocess.run(["ctest", "--test-dir", str(build), "--output-junit", str(junit)],
                   capture_output=True, text=True)
    if not junit.exists():
        return []
    out = []
    for tc in ET.parse(junit).getroot().iter("testcase"):
        name = tc.get("name", "?")
        t = float(tc.get("time", "0") or 0)
        status = tc.get("status") or ("fail" if tc.find("failure") is not None else "run")
        # ctest JUnit uses status="run"/"fail"; a <failure> child also marks failure.
        ok = tc.find("failure") is None and status not in ("fail", "failed")
        out.append((name, "pass" if ok else "fail", t))
    return out


def category_of(name: str) -> str:
    return SUITES.get(name, ("other", ""))[0]


def main() -> int:
    results = run_ctest_junit(BUILD)
    vectors = len(list(VECTORS.rglob("input.bin"))) if VECTORS.exists() else 0

    lines: list[str] = []
    lines.append("# Test report")
    lines.append("")
    lines.append("```{note}")
    lines.append("Auto-generated from a live `ctest` run by `bench/gen_test_report.py` "
                 "(regenerated in CI before every Pages deploy). Not hand-maintained.")
    lines.append("```")
    lines.append("")

    if not results:
        lines.append("_(the Release build was not available — run "
                     "`cmake -S core -B core/build -DBUILD_TESTING=ON && cmake --build core/build` "
                     "then re-run this generator.)_")
        OUT.write_text("\n".join(lines) + "\n")
        print(f"wrote {OUT} (no build)")
        return 0

    total = len(results)
    passed = sum(1 for _, s, _ in results if s == "pass")
    wall = sum(t for _, _, t in results)

    # Top-line rollup.
    verdict = "✅ all green" if passed == total else f"❌ {total - passed} failing"
    lines.append("## Summary")
    lines.append("")
    lines.append(f"| suites | passing | conformance vectors | wall time | verdict |")
    lines.append(f"| --- | --- | --- | --- | --- |")
    lines.append(f"| {total} | {passed}/{total} | {vectors} | {wall:.2f}s | {verdict} |")
    lines.append("")

    # Per-category rollup.
    by_cat: dict[str, list[tuple[str, str, float]]] = {}
    for name, status, t in results:
        by_cat.setdefault(category_of(name), []).append((name, status, t))

    lines.append("## By subsystem")
    lines.append("")
    lines.append("| category | suites | passing |")
    lines.append("| --- | --- | --- |")
    for cat in CATEGORY_ORDER:
        rows = by_cat.get(cat)
        if not rows:
            continue
        p = sum(1 for _, s, _ in rows if s == "pass")
        mark = "✅" if p == len(rows) else "❌"
        lines.append(f"| {cat} | {len(rows)} | {mark} {p}/{len(rows)} |")
    lines.append("")

    # Per-suite detail, grouped by category.
    lines.append("## Suites")
    lines.append("")
    for cat in CATEGORY_ORDER:
        rows = by_cat.get(cat)
        if not rows:
            continue
        lines.append(f"### {cat}")
        lines.append("")
        lines.append("| suite | result | time | covers |")
        lines.append("| --- | --- | --- | --- |")
        for name, status, t in sorted(rows):
            mark = "✅ pass" if status == "pass" else "❌ **FAIL**"
            covers = SUITES.get(name, ("", "—"))[1]
            lines.append(f"| `{name}` | {mark} | {t:.2f}s | {covers} |")
        lines.append("")

    # How it's verified — the config matrix (sanitizers are separate CI jobs; this
    # documents the discipline, and the zero-heap gate is a live absolute gate).
    lines.append("## How every suite is verified")
    lines.append("")
    lines.append("Beyond this Release pass, the same suites run under three more configurations "
                 "in CI (`core-ci.yml`), and the net forward path carries an absolute allocation gate:")
    lines.append("")
    lines.append("| configuration | what it proves |")
    lines.append("| --- | --- |")
    lines.append("| **Release** (this page) | functional correctness, byte-exact wire behavior |")
    lines.append("| **ASan + UBSan** | no leaks, no undefined behavior, no buffer overruns |")
    lines.append("| **TSan** | the lock-free LKV + concurrent forward paths are race-free |")
    lines.append("| **GCC-13 + GCC-15** | the toolchain floor + the ESP on-silicon compiler |")
    lines.append("| **16KB zero-heap gate** | the FWD forward hop allocates **0 bytes** "
                 "(`bench_forward_heap`, `ZEROHEAP_MAX=0`; ADR-0038/0039) |")
    lines.append("")
    lines.append("Cross-implementation conformance (C++ / TypeScript / Rust agree on every "
                 "vector) and the live latency/throughput numbers are on the "
                 "[Performance](performance.md) page.")
    lines.append("")

    OUT.write_text("\n".join(lines) + "\n")
    print(f"wrote {OUT}: {passed}/{total} suites passing, {vectors} vectors")
    return 0 if passed == total else 0  # report generation never fails the build itself


if __name__ == "__main__":
    sys.exit(main())
