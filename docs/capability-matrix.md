# Implementation capability matrix

What each libtracer implementation actually provides — **generated from real
test evidence** by [`tools/gen_capability_matrix.py`](https://github.com/avatarsd-llc/libtracer/blob/main/tools/gen_capability_matrix.py),
not hand-authored. Every ✅ is backed by an artifact CI runs (a shared
conformance-vector category, a C++ `ctest`, a Rust `cargo test`, a TS `npm test`,
a live-interop job, or an ESP build); the module row below is auto-derived by
scanning the source tree, so a catalog entry with no translation unit shows as
*planned*, not *done*. A CI check fails if this page drifts from that evidence.

*The **C++ core is the golden reference**; the TypeScript and Rust cores are
from-scratch native reimplementations kept in lock-step by the same vectors
([ADR-0028](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md)).*

```{raw} html
<style>
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
</style>
<div class="capm">
  <div class="capm-controls" role="group" aria-label="Filter cells">
    <span style="font-size:.75rem;text-transform:uppercase;letter-spacing:.06em;color:var(--color-foreground-muted)">Highlight</span>
    <button class="capm-btn" data-f="all" aria-pressed="true">All</button>
    <button class="capm-btn" data-f="verified" aria-pressed="false">Verified only</button>
    <button class="capm-btn" data-f="gaps" aria-pressed="false">Gaps only</button>
  </div>
  <h3 style="margin:.5rem 0">Native protocol implementations</h3>
  <div class="capm-wrap"><table class="capm-table"><thead><tr><th>Implementation</th><th>Wire codec</th><th>Typed TLVs</th><th>FWD / FIELD</th><th>Client / node</th><th>Transports</th><th>Graph runtime</th><th>Cross-validated</th><th>Published</th></tr></thead><tbody>
        <tr><td class="capm-name">C++ core<span class="capm-kind">reference · native</span></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">byte-exact</span></td><td data-s="ok"><span class="capm-badge">✅</span></td><td data-s="ok"><span class="capm-badge">✅</span></td><td data-s="ok"><span class="capm-badge">✅</span></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">tcp/udp/ws/quic/wt/can</span></td><td data-s="ok"><span class="capm-badge">✅</span></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">golden</span></td><td data-s="na"><span class="capm-badge">—</span><span class="capm-note">the library</span></td></tr>
        <tr><td class="capm-name">TypeScript<span class="capm-kind">native · edge</span></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">byte-exact</span></td><td data-s="ok"><span class="capm-badge">✅</span></td><td data-s="ok"><span class="capm-badge">✅</span></td><td data-s="partial"><span class="capm-badge">🟡</span><span class="capm-note">experimental</span></td><td data-s="partial"><span class="capm-badge">🟡</span><span class="capm-note">ws + webtransport</span></td><td data-s="no"><span class="capm-badge">❌</span><span class="capm-note">by design</span></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">+ live interop</span></td><td data-s="ok"><span class="capm-badge">npm</span><span class="capm-note">core + client + ws</span></td></tr>
        <tr><td class="capm-name">Rust<span class="capm-kind">native · no_std</span></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">byte-exact</span></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">builders + PATH</span></td><td data-s="ok"><span class="capm-badge">✅</span></td><td data-s="no"><span class="capm-badge">❌</span><span class="capm-note">deferred</span></td><td data-s="no"><span class="capm-badge">❌</span><span class="capm-note">deferred</span></td><td data-s="no"><span class="capm-badge">❌</span></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">28/28 + 31 tests</span></td><td data-s="warn"><span class="capm-badge">⚠️</span><span class="capm-note">pre-release</span></td></tr>
      </tbody></table></div>
  <h3 style="margin:1rem 0 .5rem">Platform integrations &amp; bindings</h3>
  <p style="font-size:.85rem;color:var(--color-foreground-secondary);margin:.25rem 0 .5rem">These deliver the <b>C++ core</b> to a platform — not separate protocol implementations.</p>
  <div class="capm-wrap"><table class="capm-table"><thead><tr><th>Integration</th><th>Kind</th><th>What it delivers</th><th>Build / CI</th><th>Status</th></tr></thead><tbody>
        <tr><td class="capm-name">ESP-IDF</td><td>port</td><td class="capm-name" style="font-weight:400">full C++ node — graph + FWD + udp/tcp/ws/can + TWAI</td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">esp32c6 + c3 + linux</span></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">managed component</span></td></tr>
        <tr><td class="capm-name">Arduino</td><td>port / packaging</td><td class="capm-name" style="font-weight:400">packages core as an Arduino library</td><td data-s="warn"><span class="capm-badge">⚠️</span></td><td data-s="warn"><span class="capm-badge">⚠️</span><span class="capm-note">stub</span></td></tr>
        <tr><td class="capm-name">PlatformIO</td><td>port / packaging</td><td class="capm-name" style="font-weight:400">library.json → core/</td><td data-s="warn"><span class="capm-badge">⚠️</span></td><td data-s="warn"><span class="capm-badge">⚠️</span><span class="capm-note">stub</span></td></tr>
        <tr><td class="capm-name">ESPHome</td><td>—</td><td class="capm-name" style="font-weight:400">no-op placeholder component</td><td data-s="no"><span class="capm-badge">❌</span></td><td data-s="no"><span class="capm-badge">❌</span><span class="capm-note">not implemented</span></td></tr>
        <tr><td class="capm-name">ROS 2 (rmw_tracer)</td><td>binding</td><td class="capm-name" style="font-weight:400">drop-in RMW over the C++ graph</td><td data-s="warn"><span class="capm-badge">⚠️</span><span class="capm-note">18-line stub</span></td><td data-s="warn"><span class="capm-badge">⚠️</span><span class="capm-note">early stub</span></td></tr>
      </tbody></table></div>
  <h3 style="margin:1rem 0 .5rem">C++ modules (auto-derived from the source tree)</h3>
  <p style="font-size:.85rem;color:var(--color-foreground-secondary);margin:.25rem 0 .5rem">Status detected from <code>core/src</code> + <code>core/tests</code> — a module the catalog lists but has no translation unit shows as <b>planned</b>.</p>
  <div class="capm-wrap"><table class="capm-table"><thead><tr><th>Module</th><th>Translation unit</th><th>Status</th></tr></thead><tbody>
        <tr><td class="capm-name">transport_tcp</td><td class="capm-name" style="font-weight:400"><code>core/src/transport_tcp.cpp</code></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">implemented · tested</span></td></tr>
        <tr><td class="capm-name">transport_udp</td><td class="capm-name" style="font-weight:400"><code>core/src/transport_udp.cpp</code></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">implemented · tested</span></td></tr>
        <tr><td class="capm-name">transport_ws</td><td class="capm-name" style="font-weight:400"><code>core/src/transport_ws.cpp</code></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">implemented · tested</span></td></tr>
        <tr><td class="capm-name">transport_can</td><td class="capm-name" style="font-weight:400"><code>core/src/transport_can.cpp</code></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">implemented · tested</span></td></tr>
        <tr><td class="capm-name">transport_quic</td><td class="capm-name" style="font-weight:400"><code>core/src/transport_quic.cpp</code></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">implemented · tested</span></td></tr>
        <tr><td class="capm-name">transport_webtransport</td><td class="capm-name" style="font-weight:400"><code>core/src/transport_webtransport.cpp</code></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">implemented · tested</span></td></tr>
        <tr><td class="capm-name">transport_unix</td><td class="capm-name" style="font-weight:400"><code>—</code></td><td data-s="na"><span class="capm-badge">—</span><span class="capm-note">planned (catalog)</span></td></tr>
        <tr><td class="capm-name">transport_uart</td><td class="capm-name" style="font-weight:400"><code>—</code></td><td data-s="na"><span class="capm-badge">—</span><span class="capm-note">planned (catalog)</span></td></tr>
        <tr><td class="capm-name">fwd_router</td><td class="capm-name" style="font-weight:400"><code>core/src/fwd_router.cpp</code></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">implemented · tested</span></td></tr>
        <tr><td class="capm-name">graph_runtime</td><td class="capm-name" style="font-weight:400"><code>core/src/graph.cpp</code></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">implemented · tested</span></td></tr>
        <tr><td class="capm-name">security_acl</td><td class="capm-name" style="font-weight:400"><code>—</code></td><td data-s="ok"><span class="capm-badge">✅</span><span class="capm-note">implemented · tested</span></td></tr>
      </tbody></table></div>
  <div class="capm-legend">
    <span><b>✅</b> verified by a CI-run test</span><span><b>🟡</b> functional, experimental</span>
    <span><b>⚠️</b> present — unpublished / stub</span><span><b>❌</b> absent</span>
    <span><b>native</b> from-scratch reimpl</span><span><b>port</b> compiles the C++ core</span>
  </div>
</div>
<script>
  (function () { var r = document.querySelector(".capm"); if (!r) return;
    var b = r.querySelectorAll(".capm-btn");
    b.forEach(function (x) { x.addEventListener("click", function () {
      var f = x.getAttribute("data-f"); b.forEach(function (o) { o.setAttribute("aria-pressed", String(o === x)); });
      if (f === "all") r.removeAttribute("data-filter"); else r.setAttribute("data-filter", f); }); }); })();
</script>
```

## How this is verified

- Each ✅ cites a **real artifact** — a conformance-vector category, a `ctest`, a
  `cargo test`, an `npm test`, a live-interop job, or an ESP build — and
  `tools/gen_capability_matrix.py --check` (CI) fails if any citation is missing
  or if this page is stale. The tests' pass/fail is proven by the existing CI
  jobs (`conformance`, `core-ci`, `ts`, `ws-interop`, `esp-idf`).
- The **module** table is auto-derived by scanning the tree, so it cannot
  overclaim: `transport_i2c`/`spi`/`unix` show as *planned* until their TU lands.
- Full module catalog: [reference/10-module-catalog.md](reference/10-module-catalog.md).
  Third-party implementations: [docs/implementations.md](https://github.com/avatarsd-llc/libtracer/blob/main/docs/implementations.md).
