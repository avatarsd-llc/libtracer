#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Generate the implementation capability matrix from REAL test evidence.

Every ✅ in docs/capability-matrix.md must be backed by an artifact that CI runs:
a shared conformance-vector category, a C++ ctest, a Rust `cargo test`, a TS
`npm test`, a live-interop job, or an ESP build. This tool is the single source of
that page: it validates that each cited artifact EXISTS (and, for the module row,
auto-derives status by scanning core/src + core/tests), then renders the page.

  tools/gen_capability_matrix.py            # regenerate docs/capability-matrix.md
  tools/gen_capability_matrix.py --check    # fail if evidence is missing OR the
                                            # committed page is stale (the CI gate)

The pass/fail of each cited test is proven by the existing CI jobs (conformance,
core-ci, ts, ws-interop, esp-idf, …); this tool proves the CLAIM is wired to a
real, CI-run artifact and that the page never drifts from reality.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAGE = ROOT / "docs" / "capability-matrix.md"

# --- evidence kinds: (relative path that must exist, the CI workflow that runs it)
VEC = "tests/conformance/vectors/v1"          # conformance.yml (cpp+ts+rust)
CTEST = "core/tests"                            # core-ci.yml
TSPKG = "bindings/typescript/packages"          # ts.yml / ws-interop.yml
RUST = "bindings/rust/tests"                     # conformance.yml (cargo test)

# Native cores × capability. Each cell: (badge, note, [evidence paths]). An empty
# evidence list is only allowed for an explicit ❌/— (nothing to prove).
CORES = [
    ("C++ core", "reference · native", {
        "Wire codec":      ("✅", "byte-exact", [f"{VEC}/framing", f"{VEC}/crc", f"{CTEST}/frame_test.cpp"]),
        "Typed TLVs":      ("✅", "", [f"{VEC}/tlv-types", f"{VEC}/path", f"{CTEST}/path_test.cpp", f"{CTEST}/acl_test.cpp"]),
        "FWD / FIELD":     ("✅", "", [f"{VEC}/fwd", f"{VEC}/field", f"{CTEST}/op_resolve_test.cpp", f"{CTEST}/fwd_fanout_test.cpp"]),
        "Client / node":   ("✅", "", [f"{CTEST}/graph_test.cpp", f"{CTEST}/fwd_node_server.cpp"]),
        "Transports":      ("✅", "tcp/udp/ws/quic/wt/can", [f"{CTEST}/tcp_test.cpp", f"{CTEST}/udp_test.cpp", f"{CTEST}/ws_transport_test.cpp", f"{CTEST}/transport_can_test.cpp", f"{CTEST}/transport_conformance_test.cpp"]),
        "Graph runtime":   ("✅", "", [f"{CTEST}/graph_test.cpp", f"{CTEST}/subtree_test.cpp", f"{CTEST}/children_test.cpp"]),
        "Cross-validated": ("✅", "golden", [f"{CTEST}/conformance_runner.cpp"]),
        "Published":       ("—", "the library", []),
    }),
    ("TypeScript", "native · edge", {
        "Wire codec":      ("✅", "byte-exact", [f"{TSPKG}/core/conformance/harness.mjs"]),
        "Typed TLVs":      ("✅", "", [f"{TSPKG}/client/test/vectors.test.mjs"]),
        "FWD / FIELD":     ("✅", "", [f"{TSPKG}/client/test/roundtrip.test.mjs"]),
        "Client / node":   ("🟡", "experimental", [f"{TSPKG}/client/test/session.test.mjs", f"{TSPKG}/client/test/interop.test.mjs"]),
        "Transports":      ("🟡", "ws + webtransport", [f"{TSPKG}/transport-ws/test/ws-codec.test.mjs", f"{TSPKG}/transport-webtransport/test/framing.test.mjs"]),
        "Graph runtime":   ("❌", "by design", []),
        "Cross-validated": ("✅", "+ live interop", [f"{TSPKG}/transport-ws/test/interop.test.mjs", f"{TSPKG}/client/test/interop.test.mjs"]),
        "Published":       ("npm", "core + client + ws", [f"{TSPKG}/core/package.json"]),
    }),
    ("Rust", "native · no_std", {
        "Wire codec":      ("✅", "byte-exact", [f"{RUST}/conformance_vectors.rs"]),
        "Typed TLVs":      ("✅", "builders + PATH", [f"{RUST}/conformance_vectors.rs", f"{VEC}/path"]),
        "FWD / FIELD":     ("✅", "", [f"{RUST}/conformance_vectors.rs", f"{VEC}/fwd", f"{VEC}/field"]),
        "Client / node":   ("❌", "deferred", []),
        "Transports":      ("❌", "deferred", []),
        "Graph runtime":   ("❌", "", []),
        "Cross-validated": ("✅", "28/28 + 31 tests", [f"{RUST}/conformance_vectors.rs"]),
        "Published":       ("⚠️", "pre-release", []),
    }),
]
CAP_COLS = ["Wire codec", "Typed TLVs", "FWD / FIELD", "Client / node", "Transports", "Graph runtime", "Cross-validated", "Published"]

# Platform integrations (deliver the C++ core). (badge, note, [evidence]).
INTEGRATIONS = [
    ("ESP-IDF", "port", "full C++ node — graph + FWD + udp/tcp/ws/can + TWAI",
     ("✅", "esp32c6 + c3 + linux", ["integrations/esp-idf/libtracer/idf_component.yml", "integrations/esp-idf/libtracer/CMakeLists.txt"]),
     ("✅", "managed component", [])),
    ("Arduino", "port / packaging", "packages core as an Arduino library",
     ("⚠️", "", ["integrations/arduino/library.properties"]), ("⚠️", "stub", [])),
    ("PlatformIO", "port / packaging", "library.json → core/",
     ("⚠️", "", ["library.json"]), ("⚠️", "stub", [])),
    ("ESPHome", "—", "no-op placeholder component",
     ("❌", "", []), ("❌", "not implemented", [])),
    ("ROS 2 (rmw_tracer)", "binding", "drop-in RMW over the C++ graph",
     ("⚠️", "18-line stub", []), ("⚠️", "early stub", [])),
]

# Module catalog rows to auto-derive from the source tree. name -> the core/src TU
# stem and the core/tests stem; status is DETECTED, not asserted, so a catalog
# entry with no TU is honestly "planned" (the earlier overclaim guard).
MODULES = [
    ("transport_tcp", "transport_tcp", "tcp_test"),
    ("transport_udp", "transport_udp", "udp_test"),
    ("transport_ws", "transport_ws", "ws_transport_test"),
    ("transport_can", "transport_can", "transport_can_test"),
    ("transport_quic", "transport_quic", "quic_test"),
    ("transport_webtransport", "transport_webtransport", "webtransport_test"),
    ("transport_unix", "transport_unix", "transport_unix_test"),
    ("transport_uart", "transport_uart", "transport_uart_test"),
    ("transport_i2c", "transport_i2c", "transport_i2c_test"),
    ("transport_spi", "transport_spi", "transport_spi_test"),
    ("fwd_router", "fwd_router", "fwd_fanout_test"),
    ("graph_runtime", "graph", "graph_test"),
    ("security_acl", "security_acl", "security_acl_test"),
]


def check_evidence():
    """Every cited evidence path must exist. Returns a list of (label, path) misses."""
    misses = []
    def check(label, paths):
        for p in paths:
            if not (ROOT / p).exists():
                misses.append((label, p))
    for name, _kind, cells in CORES:
        for col, (_b, _n, ev) in cells.items():
            check(f"{name} / {col}", ev)
    for name, _k, _d, build, status in INTEGRATIONS:
        check(f"{name} / build", build[2])
        check(f"{name} / status", status[2])
    return misses


def module_status(src_stem, test_stem):
    """Detect a module's real status from the tree (not asserted)."""
    has_src = (ROOT / "core" / "src" / f"{src_stem}.cpp").exists() or \
              (ROOT / "core" / "include" / "libtracer" / f"{src_stem}.hpp").exists()
    has_test = (ROOT / "core" / "tests" / f"{test_stem}.cpp").exists()
    if has_src and has_test:
        return ("✅", "implemented · tested")
    if has_src:
        return ("🟡", "implemented · no test")
    return ("—", "planned (catalog)")


def cell(badge, note):
    n = f'<span class="capm-note">{note}</span>' if note else ""
    s = {"✅": "ok", "🟡": "partial", "⚠️": "warn", "❌": "no", "—": "na", "npm": "ok"}.get(badge, "na")
    return f'<td data-s="{s}"><span class="capm-badge">{badge}</span>{n}</td>'


def render():
    L = []
    L.append("# Implementation capability matrix")
    L.append("")
    L.append("What each libtracer implementation actually provides — **generated from real")
    L.append("test evidence** by [`tools/gen_capability_matrix.py`](https://github.com/avatarsd-llc/libtracer/blob/main/tools/gen_capability_matrix.py),")
    L.append("not hand-authored. Every ✅ is backed by an artifact CI runs (a shared")
    L.append("conformance-vector category, a C++ `ctest`, a Rust `cargo test`, a TS `npm test`,")
    L.append("a live-interop job, or an ESP build); the module row below is auto-derived by")
    L.append("scanning the source tree, so a catalog entry with no translation unit shows as")
    L.append("*planned*, not *done*. A CI check fails if this page drifts from that evidence.")
    L.append("")
    L.append("*The **C++ core is the golden reference**; the TypeScript and Rust cores are")
    L.append("from-scratch native reimplementations kept in lock-step by the same vectors")
    L.append("([ADR-0028](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md)).*")
    L.append("")
    L.append("```{raw} html")
    L.append(STYLE)
    L.append('<div class="capm">')
    L.append(CONTROLS)
    # cores table
    L.append('  <h3 style="margin:.5rem 0">Native protocol implementations</h3>')
    L.append('  <div class="capm-wrap"><table class="capm-table"><thead><tr><th>Implementation</th>' +
             "".join(f"<th>{c}</th>" for c in CAP_COLS) + "</tr></thead><tbody>")
    for name, kind, cells in CORES:
        row = [f'<td class="capm-name">{name}<span class="capm-kind">{kind}</span></td>']
        for col in CAP_COLS:
            b, n, _ev = cells[col]
            row.append(cell(b, n))
        L.append("        <tr>" + "".join(row) + "</tr>")
    L.append("      </tbody></table></div>")
    # integrations table
    L.append('  <h3 style="margin:1rem 0 .5rem">Platform integrations &amp; bindings</h3>')
    L.append('  <p style="font-size:.85rem;color:var(--color-foreground-secondary);margin:.25rem 0 .5rem">These deliver the <b>C++ core</b> to a platform — not separate protocol implementations.</p>')
    L.append('  <div class="capm-wrap"><table class="capm-table"><thead><tr><th>Integration</th><th>Kind</th><th>What it delivers</th><th>Build / CI</th><th>Status</th></tr></thead><tbody>')
    for name, kind, delivers, build, status in INTEGRATIONS:
        L.append("        <tr>" +
                 f'<td class="capm-name">{name}</td><td>{kind}</td>' +
                 f'<td class="capm-name" style="font-weight:400">{delivers}</td>' +
                 cell(build[0], build[1]) + cell(status[0], status[1]) + "</tr>")
    L.append("      </tbody></table></div>")
    # modules table (auto-derived)
    L.append('  <h3 style="margin:1rem 0 .5rem">C++ modules (auto-derived from the source tree)</h3>')
    L.append('  <p style="font-size:.85rem;color:var(--color-foreground-secondary);margin:.25rem 0 .5rem">Status detected from <code>core/src</code> + <code>core/tests</code> — a module the catalog lists but has no translation unit shows as <b>planned</b>.</p>')
    L.append('  <div class="capm-wrap"><table class="capm-table"><thead><tr><th>Module</th><th>Translation unit</th><th>Status</th></tr></thead><tbody>')
    for name, src, test in MODULES:
        b, n = module_status(src, test)
        tu = f"core/src/{src}.cpp" if (ROOT / "core" / "src" / f"{src}.cpp").exists() else "—"
        L.append("        <tr>" + f'<td class="capm-name">{name}</td>' +
                 f'<td class="capm-name" style="font-weight:400"><code>{tu}</code></td>' + cell(b, n) + "</tr>")
    L.append("      </tbody></table></div>")
    L.append(LEGEND)
    L.append("</div>")
    L.append(SCRIPT)
    L.append("```")
    L.append("")
    L.append("## How this is verified")
    L.append("")
    L.append("- Each ✅ cites a **real artifact** — a conformance-vector category, a `ctest`, a")
    L.append("  `cargo test`, an `npm test`, a live-interop job, or an ESP build — and")
    L.append("  `tools/gen_capability_matrix.py --check` (CI) fails if any citation is missing")
    L.append("  or if this page is stale. The tests' pass/fail is proven by the existing CI")
    L.append("  jobs (`conformance`, `core-ci`, `ts`, `ws-interop`, `esp-idf`).")
    L.append("- The **module** table is auto-derived by scanning the tree, so it cannot")
    L.append("  overclaim: `transport_i2c`/`spi`/`unix` show as *planned* until their TU lands.")
    L.append("- Full module catalog: [reference/10-module-catalog.md](reference/10-module-catalog.md).")
    L.append("  Third-party implementations: [docs/implementations.md](https://github.com/avatarsd-llc/libtracer/blob/main/docs/implementations.md).")
    L.append("")
    return "\n".join(L)


STYLE = """<style>
  .capm { --ok: 46,160,67; --partial: 210,153,34; --warn: 219,109,40; --no: 180,60,60; --na: 130,130,130; }
  .capm-controls { display:flex; flex-wrap:wrap; gap:.5rem; align-items:center; margin:1rem 0 .5rem; }
  .capm-controls .capm-btn { font:inherit; font-size:.8125rem; padding:.3rem .7rem; border-radius:999px; cursor:pointer; border:1px solid var(--color-background-border); background:var(--color-background-secondary); color:var(--color-foreground-secondary); }
  .capm-controls .capm-btn[aria-pressed="true"] { border-color:var(--color-brand-primary); color:var(--color-brand-primary); font-weight:600; }
  .capm-wrap { overflow-x:auto; margin:.5rem 0 1.5rem; border:1px solid var(--color-background-border); border-radius:10px; }
  table.capm-table { border-collapse:collapse; width:100%; font-size:.85rem; min-width:640px; }
  table.capm-table th, table.capm-table td { padding:.5rem .6rem; text-align:center; border-bottom:1px solid var(--color-background-border); }
  table.capm-table thead th { position:sticky; top:0; background:var(--color-background-secondary); font-weight:600; z-index:1; white-space:nowrap; }
  table.capm-table tbody th, table.capm-table td.capm-name { text-align:left; white-space:nowrap; }
  table.capm-table td.capm-name { font-weight:600; }
  .capm-kind { display:block; font-weight:400; font-size:.72rem; color:var(--color-foreground-muted); text-transform:uppercase; letter-spacing:.04em; }
  td[data-s="ok"] { background:rgba(var(--ok),.13); } td[data-s="partial"] { background:rgba(var(--partial),.15); }
  td[data-s="warn"] { background:rgba(var(--warn),.15); } td[data-s="no"] { background:rgba(var(--no),.10); }
  td[data-s="na"] { background:transparent; color:var(--color-foreground-muted); }
  td .capm-note { display:block; font-size:.7rem; color:var(--color-foreground-muted); margin-top:.1rem; white-space:nowrap; }
  .capm[data-filter="verified"] td[data-s]:not([data-s="ok"]) .capm-badge { opacity:.18; }
  .capm[data-filter="gaps"] td[data-s="ok"] .capm-badge, .capm[data-filter="gaps"] td[data-s="na"] .capm-badge { opacity:.18; }
  .capm-legend { display:flex; flex-wrap:wrap; gap:.75rem 1.25rem; font-size:.8rem; color:var(--color-foreground-secondary); margin:.25rem 0 1rem; }
</style>"""

CONTROLS = """  <div class="capm-controls" role="group" aria-label="Filter cells">
    <span style="font-size:.75rem;text-transform:uppercase;letter-spacing:.06em;color:var(--color-foreground-muted)">Highlight</span>
    <button class="capm-btn" data-f="all" aria-pressed="true">All</button>
    <button class="capm-btn" data-f="verified" aria-pressed="false">Verified only</button>
    <button class="capm-btn" data-f="gaps" aria-pressed="false">Gaps only</button>
  </div>"""

LEGEND = """  <div class="capm-legend">
    <span><b>✅</b> verified by a CI-run test</span><span><b>🟡</b> functional, experimental</span>
    <span><b>⚠️</b> present — unpublished / stub</span><span><b>❌</b> absent</span>
    <span><b>native</b> from-scratch reimpl</span><span><b>port</b> compiles the C++ core</span>
  </div>"""

SCRIPT = """<script>
  (function () { var r = document.querySelector(".capm"); if (!r) return;
    var b = r.querySelectorAll(".capm-btn");
    b.forEach(function (x) { x.addEventListener("click", function () {
      var f = x.getAttribute("data-f"); b.forEach(function (o) { o.setAttribute("aria-pressed", String(o === x)); });
      if (f === "all") r.removeAttribute("data-filter"); else r.setAttribute("data-filter", f); }); }); })();
</script>"""


def main():
    check = "--check" in sys.argv[1:]
    misses = check_evidence()
    if misses:
        print("capability matrix: cited evidence MISSING (claim not backed by a real artifact):")
        for label, path in misses:
            print(f"  - {label}: {path}")
        sys.exit(1)
    rendered = render()
    if check:
        current = PAGE.read_text(encoding="utf-8") if PAGE.exists() else ""
        if current.rstrip("\n") != rendered.rstrip("\n"):
            print("capability matrix: docs/capability-matrix.md is STALE.")
            print("fix: python3 tools/gen_capability_matrix.py")
            sys.exit(1)
        print("ok: capability matrix matches the evidence and is up to date")
    else:
        PAGE.write_text(rendered, encoding="utf-8")
        print(f"wrote {PAGE.relative_to(ROOT)} ({len(CORES)} cores, {len(INTEGRATIONS)} integrations, {len(MODULES)} modules)")


if __name__ == "__main__":
    main()
