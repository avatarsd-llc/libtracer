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
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

REPO = pathlib.Path(__file__).resolve().parent.parent
OUT = REPO / "docs" / "test-report.md"
BUILD = pathlib.Path(os.environ.get("LIBTRACER_CORE_BUILD") or (REPO / "core" / "build"))
VECTORS = REPO / "tests" / "conformance" / "vectors" / "v1"

# Suite name -> (category, legacy one-liner). The description half is now DEAD for any
# suite whose source resolves: `_brief()` reads the test file's own @brief instead, so
# these strings survive only as the fallback for an unresolvable suite. The category
# half is a deliberate OVERRIDE of CATEGORY_RULES below, for the handful whose name
# does not imply their layer.
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


# --- source resolution ------------------------------------------------------
# ctest name -> the .cpp that defines it, and that file's own @brief.
#
# Both are DERIVED, never listed. A hand-maintained table of descriptions had
# already drifted: it named 25 suites while ctest ran 64, so 43 suites reached the
# published report with no description at all and four entries described tests that
# no longer existed. A table that must be updated by hand to stay true will not stay
# true; the build files and the test headers cannot go stale, because they are what
# actually runs.
CMAKELISTS = [REPO / "core" / "tests" / "CMakeLists.txt",
              REPO / "core" / "examples" / "CMakeLists.txt"]


def _test_sources() -> dict[str, pathlib.Path]:
    """@brief ctest suite name -> its defining source file, read off the build graph.

    Resolves `add_test(NAME x COMMAND tgt)` through `add_executable(tgt src...)`, so
    the four naming exceptions need no special case: the conformance runner, the
    NO_ATOMIC rebuild of an existing source, a suite whose target is spelled
    differently from its test name, and every `core/examples/` binary all fall out of
    the same two rules.
    """
    exe: dict[str, str] = {}
    tests: dict[str, str] = {}
    for cm in CMAKELISTS:
        if not cm.exists():
            continue
        text = " ".join(cm.read_text().split())  # flatten: these calls wrap lines
        for m in re.finditer(r"add_executable\(\s*(\S+)\s+([^)]*)\)", text):
            src = next((w for w in m.group(2).split() if w.endswith(".cpp")), None)
            if src:
                exe[m.group(1)] = src
        for m in re.finditer(r"add_test\(\s*NAME\s+([\w.-]+)\s+COMMAND\s+([\w.-]+)", text):
            tests[m.group(1)] = m.group(2)
    out: dict[str, pathlib.Path] = {}
    for name, tgt in tests.items():
        src = exe.get(tgt)
        if not src:
            continue
        for base in (REPO / "core" / "tests", REPO / "core" / "examples"):
            cand = (base / src).resolve()
            if cand.exists():
                out[name] = cand
                break
    return out


def _brief(path: pathlib.Path) -> str:
    """@brief The file header's `@brief`, flattened to one line.

    This is the test's description on the published page. Taking it from the source
    means the report says what the test says about itself — a description cannot
    drift from the test it describes, because there is only one copy.
    """
    try:
        head = path.read_text(errors="ignore")[:4000]
    except OSError:
        return ""
    m = re.search(r"@brief\s+(.+?)(?:\n\s*\*\s*\n|\*/)", head, re.S)
    if not m:
        return ""
    return re.sub(r"\s+", " ", re.sub(r"\n\s*\*\s?", " ", m.group(1))).strip()


def _src_link(path: pathlib.Path | None) -> str:
    """@brief A permalink to the file on the default branch, or an em dash."""
    if path is None:
        return "—"
    rel = path.relative_to(REPO).as_posix()
    return f"[`{rel.split('/')[-1]}`](https://github.com/avatarsd-llc/libtracer/blob/main/{rel})"


# Ordered name rules -> category. FIRST match wins, so the specific rules come before
# the general ones (`ws_transport` is a Transport suite; bare `ws` is the frame codec).
#
# Derived rather than listed for the same reason the descriptions are: the hand table
# had grown to cover 21 of 64 suites, so 43 landed in "other" and the "By subsystem"
# rollup — the one number a reader skims — was 67 % noise. A rule that mis-files a new
# suite is visible and fixable here; a table that silently omits it is not.
CATEGORY_RULES: list[tuple[tuple[str, ...], str]] = [
    (("example_",), "Examples"),
    (("transport_can", "transport_conformance", "can_tx_pool", "udp", "tcp",
      "ws_transport", "write_all_eintr"), "Transport"),
    (("fwd_", "op_resolve", "mount_routing", "route_handle", "compact_cache",
      "net_control_plane_race", "transport_vertex", "bridge"), "Net (FWD plane)"),
    (("graph", "acl", "effective_acl", "security_acl", "app_fields", "identity",
      "retire", "edge_eviction", "registry_teardown", "delivery_drops", "vertex",
      "children", "folded_children", "subtree"), "Graph (L4)"),
    (("byteorder", "frame", "ws", "can_frames", "key_view", "length_prefix_framer",
      "tlv_", "rope_decode"), "Codec (L2/L3)"),
    (("path", "substrate", "rope", "mem_"), "Substrate (L0/L1)"),
]


def category_of(name: str) -> str:
    """@brief The suite's subsystem: an explicit SUITES entry, else the first rule that
    matches its name, else "other"."""
    if name in SUITES:
        return SUITES[name][0]
    for prefixes, cat in CATEGORY_RULES:
        if any(name == p or name.startswith(p) for p in prefixes):
            return cat
    return "other"


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

    # Per-suite detail, grouped by category. "covers" is the SOURCE FILE's own
    # @brief and "source" links to it, so the report cannot describe a test as
    # something other than what the test says it is.
    sources = _test_sources()
    lines.append("## Suites")
    lines.append("")
    lines.append("Each row's description is the test file's own `@brief`, read from the source "
                 "at generation time, and `source` links to that file. Neither is transcribed "
                 "into this generator, so neither can drift from the test.")
    lines.append("")
    for cat in CATEGORY_ORDER:
        rows = by_cat.get(cat)
        if not rows:
            continue
        lines.append(f"### {cat}")
        lines.append("")
        lines.append("| suite | result | time | covers | source |")
        lines.append("| --- | --- | --- | --- | --- |")
        for name, status, t in sorted(rows):
            mark = "✅ pass" if status == "pass" else "❌ **FAIL**"
            src = sources.get(name)
            covers = (_brief(src) if src else "") or SUITES.get(name, ("", "—"))[1]
            covers = covers.replace("|", "\\|")
            lines.append(f"| `{name}` | {mark} | {t:.2f}s | {covers} | {_src_link(src)} |")
        lines.append("")
    missing = sorted(n for n, _, _ in results if n not in sources)
    if missing:
        lines.append("```{warning}")
        lines.append("No source could be resolved from the CMake build graph for: "
                     + ", ".join(f"`{m}`" for m in missing)
                     + ". Those rows fall back to a hand-written description, which is exactly "
                       "the drift this resolution exists to remove.")
        lines.append("```")
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
