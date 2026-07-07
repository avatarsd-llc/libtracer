# Performance & Conformance

```{note}
This page is **auto-generated** from the live test + benchmark harnesses on each docs
build (`bench/gen_results_page.py`, ADR-0032). It is the published response surface,
not a hand-edited snapshot. All rates and latencies are **absolute measured values**,
representative of the CI runner (shared-runner variance is real — read trends, not the
third digit); the libtracer-vs-Zenoh charts below plot both engines on the same axes.

_Generated from a local build on 2026-07-07 10:46 UTC (not a CI deploy)._
```

## Cross-core conformance (every native core must agree byte-for-byte)

The shared conformance vectors are decoded+re-encoded by every enabled core; a DISAGREE
fails CI (ADR-0028). Live driver summary:

- cpp: 28/28 vectors ok
- ts: 28/28 vectors ok
- rust: 28/28 vectors ok
- CONFORMANCE: PASS

## In-process latency & throughput

Canonical points from `bench_libtracer` (the µs-latency / zero-copy thesis, ADR-0031):

| path | p50 latency | mean | throughput |
| --- | --- | --- | --- |
| in-process (zero-copy dispatch) | 170 ns | 177 ns | 6.3 M/s |
| in-process, zero-alloc loaned path | 121 ns | 126 ns | 9.6 M/s |
| write-by-path (registry lookup) | 150 ns | 157 ns | 6.9 M/s |

## Cross-core codec performance (decode→encode roundtrip, same v1 vectors)

Every native core (cpp-core / ts-core / rust-core) runs the SAME per-vector
decode→encode roundtrip over the shared v1 conformance vectors (ADR-0032 `lang`
axis, #96), so this is a like-for-like codec surface across implementations.
Figures are the **median across all v1 vectors** (one decode + one encode == one
roundtrip); a core whose toolchain is absent in this build degrades to a note.

| core | throughput (median) | p50 latency (median) | mean (median) |
| --- | --- | --- | --- |
| cpp-core | 4.6 M roundtrips/s | 205 ns | 205 ns |
| ts-core | 1.1 M roundtrips/s | 942 ns | 1009 ns |
| rust-core | 4.8 M roundtrips/s | 266 ns | 272 ns |

## libtracer vs Zenoh — measured, absolute

A side-by-side comparison against [Eclipse Zenoh](https://zenoh.io) (zenoh-c 1.9.0, peer
mode). Two surfaces: three **in-process** axes — subscriber **fan-out**, **payload** size,
and **topic count** — and a **network** comparison over the real loopback kernel path
across three transports (**UDP**, **TCP**, **WebSocket**). The network charts render
wherever both engines establish the link — reliably via `bench/grid.sh` locally; a
sandboxed CI runner may omit the Zenoh side. Both engines are built `-O3`
and measured in the **same pass on the same runner**, so the numbers are directly
comparable on identical hardware. The charts plot **absolute** throughput / latency /
bandwidth — libtracer and Zenoh as two series on shared axes — so you read the real
numbers off the graph; there are no speed-up ratios. Full harness in
[`bench/`](https://github.com/avatarsd-llc/libtracer/tree/main/bench).

:::{raw} html
<style>/* SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Styles for the libtracer-vs-Zenoh absolute-value comparison charts embedded in
 * the Performance page (bench/render_compare.py emits the markup; ltz_compare.js
 * draws the SVGs). Theme-aware: honours prefers-color-scheme AND Furo's data-theme
 * toggle (set on <body>), so it tracks the reader's light/dark choice either way.
 * All tokens are scoped under .ltz-compare so nothing leaks into the rest of the page. */
.ltz-compare {
  --ltz-ink: #1a212b; --ltz-muted: #5a6473;
  --ltz-line: #d3d9e2; --ltz-grid: #dbe1ea; --ltz-gridminor: #eef1f6; --ltz-hair: #c3cbd8;
  --ltz-panel: rgba(127, 140, 160, .05);
  --ltz-lt: #2563c9; --ltz-zn: #d9662e;
  --ltz-warn-bg: #fdf3e7; --ltz-warn-bd: #e7c9a2; --ltz-warn-ink: #7a4d18;
  --ltz-mono: ui-monospace, "SF Mono", "SFMono-Regular", Menlo, Consolas, monospace;
  margin: 8px 0 4px;
}
@media (prefers-color-scheme: dark) {
  .ltz-compare {
    --ltz-ink: #e6e9ef; --ltz-muted: #98a2b3;
    --ltz-line: #2c3644; --ltz-grid: #26303c; --ltz-gridminor: #1a222c; --ltz-hair: #3a4655;
    --ltz-panel: rgba(127, 140, 160, .07);
    --ltz-lt: #6ea8ff; --ltz-zn: #f0955f;
    --ltz-warn-bg: #2a2013; --ltz-warn-bd: #5a4423; --ltz-warn-ink: #e6c69a;
  }
}
/* Furo toggles data-theme on <body>; let it win in both directions. */
body[data-theme="light"] .ltz-compare, :root[data-theme="light"] .ltz-compare {
  --ltz-ink: #1a212b; --ltz-muted: #5a6473;
  --ltz-line: #d3d9e2; --ltz-grid: #dbe1ea; --ltz-gridminor: #eef1f6; --ltz-hair: #c3cbd8;
  --ltz-panel: rgba(127, 140, 160, .05);
  --ltz-lt: #2563c9; --ltz-zn: #d9662e;
  --ltz-warn-bg: #fdf3e7; --ltz-warn-bd: #e7c9a2; --ltz-warn-ink: #7a4d18;
}
body[data-theme="dark"] .ltz-compare, :root[data-theme="dark"] .ltz-compare {
  --ltz-ink: #e6e9ef; --ltz-muted: #98a2b3;
  --ltz-line: #2c3644; --ltz-grid: #26303c; --ltz-gridminor: #1a222c; --ltz-hair: #3a4655;
  --ltz-panel: rgba(127, 140, 160, .07);
  --ltz-lt: #6ea8ff; --ltz-zn: #f0955f;
  --ltz-warn-bg: #2a2013; --ltz-warn-bd: #5a4423; --ltz-warn-ink: #e6c69a;
}

.ltz-compare .ltz-banner {
  display: flex; gap: 11px; align-items: flex-start;
  background: var(--ltz-warn-bg); border: 1px solid var(--ltz-warn-bd);
  color: var(--ltz-warn-ink); border-radius: 8px; padding: 12px 15px;
  margin: 4px 0 18px; font-size: 13.5px; line-height: 1.5;
}
.ltz-compare .ltz-banner .ic { font-family: var(--ltz-mono); font-weight: 700; flex: none; }
.ltz-compare .ltz-legend { display: flex; gap: 20px; align-items: center; flex-wrap: wrap; margin: 0 0 14px; }
.ltz-compare .ltz-legend .item { display: flex; gap: 8px; align-items: center; font-size: 13.5px; font-weight: 600; color: var(--ltz-ink); }
.ltz-compare .ltz-legend .sw { width: 22px; height: 3px; border-radius: 2px; }
.ltz-compare .ltz-legend .sub { color: var(--ltz-muted); font-weight: 400; font-size: 12.5px; }

.ltz-compare .ltz-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 16px; }
@media (max-width: 720px) { .ltz-compare .ltz-grid { grid-template-columns: 1fr; } }
.ltz-compare .ltz-card { background: var(--ltz-panel); border: 1px solid var(--ltz-line); border-radius: 10px; padding: 15px 15px 12px; }
.ltz-compare .ltz-card h4 { margin: 0 0 2px; font-size: 14.5px; font-weight: 650; color: var(--ltz-ink); }
.ltz-compare .ltz-card .cond { font-family: var(--ltz-mono); font-size: 11px; color: var(--ltz-muted); margin: 0 0 8px; }
.ltz-compare .ltz-card svg { display: block; width: 100%; height: auto; }
.ltz-compare .ltz-reading { font-size: 12.5px; color: var(--ltz-muted); margin: 7px 2px 0; line-height: 1.45; }
.ltz-compare .ltz-reading b { color: var(--ltz-ink); font-weight: 600; }

.ltz-compare .ltz-ax, .ltz-compare .ltz-tick { fill: var(--ltz-muted); font-family: var(--ltz-mono); font-size: 10px; }
.ltz-compare .ltz-axtitle { fill: var(--ltz-muted); font-size: 11px; }
.ltz-compare .ltz-gl { stroke: var(--ltz-grid); stroke-width: 1; }
.ltz-compare .ltz-glm { stroke: var(--ltz-gridminor); stroke-width: 1; }
.ltz-compare .ltz-frame { stroke: var(--ltz-hair); stroke-width: 1; fill: none; }

.ltz-compare .ltz-tablewrap { overflow-x: auto; border: 1px solid var(--ltz-line); border-radius: 9px; margin-top: 18px; }
.ltz-compare table.ltz-raw { border-collapse: collapse; width: 100%; font-size: 12.5px; margin: 0; }
.ltz-compare table.ltz-raw thead th { text-align: right; font-family: var(--ltz-mono); font-weight: 600; font-size: 10.5px; text-transform: uppercase; letter-spacing: .04em; color: var(--ltz-muted); padding: 9px 13px; }
.ltz-compare table.ltz-raw thead th:first-child, .ltz-compare table.ltz-raw tbody td:first-child { text-align: left; }
.ltz-compare table.ltz-raw tbody td { padding: 7px 13px; text-align: right; border-top: 1px solid var(--ltz-line); font-variant-numeric: tabular-nums; font-family: var(--ltz-mono); color: var(--ltz-ink); }
.ltz-compare table.ltz-raw tbody td.sys { font-weight: 600; }
.ltz-compare table.ltz-raw tbody td.sys .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 7px; vertical-align: middle; }
.ltz-compare table.ltz-raw tbody tr.grp td { border-top: 2px solid var(--ltz-hair); }
.ltz-compare .ltz-prov { font-family: var(--ltz-mono); font-size: 11.5px; color: var(--ltz-muted); margin: 14px 0 0; }
</style>
<div class="ltz-compare">
  <div class="ltz-banner"><span class="ic">i</span><span>Both engines are built <code>-O3</code> and measured in the same pass on the same runner, so this is like-for-like on identical hardware. Values are absolute and representative of the CI runner — read trends and orders of magnitude, not the third digit (shared-runner variance is real). No ratios: every point is a measured number on an absolute axis.</span></div>
  <div class="ltz-legend">
    <span class="item"><span class="sw" style="background:var(--ltz-lt)"></span>libtracer <span class="sub">— inproc zero-copy dispatch</span></span>
    <span class="item"><span class="sw" style="background:var(--ltz-zn)"></span>Zenoh <span class="sub">— zenoh-c 1.9.0, peer mode</span></span>
  </div>
  <div class="ltz-grid" id="ltz-charts"></div>
  <div class="ltz-tablewrap"><table class="ltz-raw" id="ltz-raw"><thead><tr>
    <th>sweep</th><th>system</th><th>point</th><th>throughput</th><th>bandwidth</th><th>p50 latency</th>
  </tr></thead><tbody></tbody></table></div>
  <p class="ltz-prov">_Generated from a local build on 2026-07-07 10:46 UTC (not a CI deploy)._</p>
  <script type="application/json" id="ltz-data">{"charts":[{"id":"tp-fan","title":"Throughput vs fan-out","cond":"64 B payload \u00b7 1 topic \u00b7 in-process","x":{"log":true,"fmt":"count","label":"subscribers per topic (fan-out)"},"y":{"log":true,"fmt":"rate","label":"deliveries / second"},"series":{"libtracer":[[1,5203006.0],[4,26668378.0],[16,34177618.0],[64,30129630.0],[256,44388142.0],[1024,77183394.0],[4096,75980076.0]],"zenoh":[[1,6976038.0],[4,11794745.0],[16,14999929.0],[64,16958275.0],[256,17210770.0],[1024,17717325.0],[4096,17632771.0]]},"reading":"Across the sweep (1 \u2192 4k): <b>libtracer 5.2 M\u2009\u2192\u200976 M</b>; <b>Zenoh 7.0 M\u2009\u2192\u200918 M</b>."},{"id":"lat-fan","title":"p50 latency vs fan-out","cond":"64 B payload \u00b7 1 topic \u00b7 in-process","x":{"log":true,"fmt":"count","label":"subscribers per topic (fan-out)"},"y":{"log":true,"fmt":"ns","label":"p50 latency"},"series":{"libtracer":[[1,120],[4,170],[16,731],[64,2024],[256,3296],[1024,13164],[4096,52769]],"zenoh":[[1,180],[4,390],[16,1082],[64,3757],[256,14537],[1024,57648],[4096,232988]]},"reading":"Across the sweep (1 \u2192 4k): <b>libtracer 120 ns\u2009\u2192\u200953 \u00b5s</b>; <b>Zenoh 180 ns\u2009\u2192\u2009233 \u00b5s</b>."},{"id":"tp-size","title":"Throughput vs payload","cond":"1 subscriber \u00b7 1 topic \u00b7 in-process","x":{"log":true,"fmt":"bytes","label":"payload size"},"y":{"log":true,"fmt":"rate","label":"deliveries / second"},"series":{"libtracer":[[1,4650531.0],[16,8786799.0],[64,5203006.0],[256,8671898.0],[1024,8521925.0],[4096,5174082.0],[8192,5253411.0]],"zenoh":[[1,5565783.0],[16,7014716.0],[64,6976038.0],[256,6743205.0],[1024,6360145.0],[4096,5259974.0],[8192,4711931.0]]},"reading":"Across the sweep (1 B \u2192 8 KB): <b>libtracer 4.7 M\u2009\u2192\u20095.3 M</b>; <b>Zenoh 5.6 M\u2009\u2192\u20094.7 M</b>."},{"id":"mb-size","title":"Bandwidth vs payload","cond":"1 subscriber \u00b7 1 topic \u00b7 in-process","x":{"log":true,"fmt":"bytes","label":"payload size"},"y":{"log":true,"fmt":"mb","label":"application bandwidth"},"series":{"libtracer":[[1,4.7],[16,140.6],[64,333.0],[256,2220.0],[1024,8726.5],[4096,21193.0],[8192,43035.9]],"zenoh":[[1,5.6],[16,112.2],[64,446.5],[256,1726.3],[1024,6512.8],[4096,21544.9],[8192,38600.1]]},"reading":"Across the sweep (1 B \u2192 8 KB): <b>libtracer 5 MB/s\u2009\u2192\u200943.0 GB/s</b>; <b>Zenoh 6 MB/s\u2009\u2192\u200938.6 GB/s</b>."},{"id":"tp-ep","title":"Throughput vs topic count","cond":"64 B \u00b7 1 subscriber \u00b7 write-by-path","x":{"log":true,"fmt":"count","label":"number of topics"},"y":{"log":false,"fmt":"rate","label":"publishes / second"},"series":{"libtracer":[[1,6930800.0],[4,6936059.0],[16,6446541.0],[64,6244365.0],[256,6254648.0],[1024,5826575.0],[4096,5663539.0]],"zenoh":[[1,6892573.0],[4,6833025.0],[16,6770122.0],[64,6654535.0],[256,6684596.0],[1024,6175621.0],[4096,5212058.0]]},"reading":"Across the sweep (1 \u2192 4k): <b>libtracer 6.9 M\u2009\u2192\u20095.7 M</b>; <b>Zenoh 6.9 M\u2009\u2192\u20095.2 M</b>."},{"id":"lat-ep","title":"p50 latency vs topic count","cond":"64 B \u00b7 1 subscriber \u00b7 write-by-path","x":{"log":true,"fmt":"count","label":"number of topics"},"y":{"log":false,"fmt":"ns","label":"p50 latency"},"series":{"libtracer":[[1,160],[4,160],[16,161],[64,161],[256,161],[1024,180],[4096,180]],"zenoh":[[1,180],[4,180],[16,181],[64,190],[256,190],[1024,190],[4096,191]]},"reading":"Across the sweep (1 \u2192 4k): <b>libtracer 160 ns\u2009\u2192\u2009180 ns</b>; <b>Zenoh 180 ns\u2009\u2192\u2009191 ns</b>."},{"id":"net-tp-udp","title":"UDP \u2014 throughput vs payload","cond":"one publisher \u00b7 one subscriber \u00b7 loopback kernel path","x":{"log":true,"fmt":"bytes","label":"payload size"},"y":{"log":true,"fmt":"rate","label":"messages / second"},"series":{"libtracer":[[16,856298.0],[256,1171610.0],[1024,1134698.0],[8192,771055.0]],"zenoh":[[16,2962209.0],[256,3035303.0],[1024,3792191.0],[8192,982897.0]]},"reading":"Across the sweep (16 B \u2192 8 KB): <b>libtracer 856 k\u2009\u2192\u2009771 k</b>; <b>Zenoh 3.0 M\u2009\u2192\u2009983 k</b>."},{"id":"net-lat-udp","title":"UDP \u2014 p50 latency vs payload","cond":"one-way, same-clock \u00b7 loopback kernel path","x":{"log":true,"fmt":"bytes","label":"payload size"},"y":{"log":true,"fmt":"ns","label":"p50 latency"},"series":{"libtracer":[[16,1833],[256,1733],[1024,1843],[8192,2204]],"zenoh":[[16,22322],[256,12393],[1024,9678],[8192,10099]]},"reading":"Across the sweep (16 B \u2192 8 KB): <b>libtracer 1.8 \u00b5s\u2009\u2192\u20092.2 \u00b5s</b>; <b>Zenoh 22 \u00b5s\u2009\u2192\u200910 \u00b5s</b>."},{"id":"net-tp-tcp","title":"TCP \u2014 throughput vs payload","cond":"one publisher \u00b7 one subscriber \u00b7 loopback kernel path","x":{"log":true,"fmt":"bytes","label":"payload size"},"y":{"log":true,"fmt":"rate","label":"messages / second"},"series":{"libtracer":[[16,3784639.0],[256,1074359.0],[1024,754976.0],[8192,662425.0]],"zenoh":[[16,2497927.0],[256,3388196.0],[1024,3421525.0],[8192,702655.0]]},"reading":"Across the sweep (16 B \u2192 8 KB): <b>libtracer 3.8 M\u2009\u2192\u2009662 k</b>; <b>Zenoh 2.5 M\u2009\u2192\u2009703 k</b>."},{"id":"net-lat-tcp","title":"TCP \u2014 p50 latency vs payload","cond":"one-way, same-clock \u00b7 loopback kernel path","x":{"log":true,"fmt":"bytes","label":"payload size"},"y":{"log":true,"fmt":"ns","label":"p50 latency"},"series":{"libtracer":[[16,2785],[256,3257],[1024,3306],[8192,3006]],"zenoh":[[16,18465],[256,16210],[1024,8105],[8192,21020]]},"reading":"Across the sweep (16 B \u2192 8 KB): <b>libtracer 2.8 \u00b5s\u2009\u2192\u20093.0 \u00b5s</b>; <b>Zenoh 18 \u00b5s\u2009\u2192\u200921 \u00b5s</b>."},{"id":"net-tp-ws","title":"WS \u2014 throughput vs payload","cond":"one publisher \u00b7 one subscriber \u00b7 loopback kernel path","x":{"log":true,"fmt":"bytes","label":"payload size"},"y":{"log":true,"fmt":"rate","label":"messages / second"},"series":{"libtracer":[[16,3210474.0],[256,1320694.0],[1024,842813.0],[8192,161678.0]],"zenoh":[[16,3666767.0],[256,3566911.0],[1024,2422196.0],[8192,828383.0]]},"reading":"Across the sweep (16 B \u2192 8 KB): <b>libtracer 3.2 M\u2009\u2192\u2009162 k</b>; <b>Zenoh 3.7 M\u2009\u2192\u2009828 k</b>."},{"id":"net-lat-ws","title":"WS \u2014 p50 latency vs payload","cond":"one-way, same-clock \u00b7 loopback kernel path","x":{"log":true,"fmt":"bytes","label":"payload size"},"y":{"log":true,"fmt":"ns","label":"p50 latency"},"series":{"libtracer":[[16,2826],[256,3086],[1024,4138],[8192,9779]],"zenoh":[[16,11221],[256,12133],[1024,10550],[8192,10790]]},"reading":"Across the sweep (16 B \u2192 8 KB): <b>libtracer 2.8 \u00b5s\u2009\u2192\u20099.8 \u00b5s</b>; <b>Zenoh 11 \u00b5s\u2009\u2192\u200911 \u00b5s</b>."}],"table":[{"sweep":"fan-out","grp":false,"system":"libtracer","x":"1","throughput":"5.2 M/s","bandwidth":"\u2014","p50":"120 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"4","throughput":"27 M/s","bandwidth":"\u2014","p50":"170 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"16","throughput":"34 M/s","bandwidth":"\u2014","p50":"731 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"64","throughput":"30 M/s","bandwidth":"\u2014","p50":"2.0 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"256","throughput":"44 M/s","bandwidth":"\u2014","p50":"3.3 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"1k","throughput":"77 M/s","bandwidth":"\u2014","p50":"13 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"4k","throughput":"76 M/s","bandwidth":"\u2014","p50":"53 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"1","throughput":"7.0 M/s","bandwidth":"\u2014","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"4","throughput":"12 M/s","bandwidth":"\u2014","p50":"390 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"16","throughput":"15 M/s","bandwidth":"\u2014","p50":"1.1 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"64","throughput":"17 M/s","bandwidth":"\u2014","p50":"3.8 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"256","throughput":"17 M/s","bandwidth":"\u2014","p50":"15 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"1k","throughput":"18 M/s","bandwidth":"\u2014","p50":"58 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"4k","throughput":"18 M/s","bandwidth":"\u2014","p50":"233 \u00b5s"},{"sweep":"payload","grp":true,"system":"libtracer","x":"1 B","throughput":"4.7 M/s","bandwidth":"5 MB/s","p50":"270 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"16 B","throughput":"8.8 M/s","bandwidth":"141 MB/s","p50":"130 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"64 B","throughput":"5.2 M/s","bandwidth":"333 MB/s","p50":"120 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"256 B","throughput":"8.7 M/s","bandwidth":"2.2 GB/s","p50":"130 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"1 KB","throughput":"8.5 M/s","bandwidth":"8.7 GB/s","p50":"130 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"4 KB","throughput":"5.2 M/s","bandwidth":"21.2 GB/s","p50":"160 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"8 KB","throughput":"5.3 M/s","bandwidth":"43.0 GB/s","p50":"170 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"1 B","throughput":"5.6 M/s","bandwidth":"6 MB/s","p50":"171 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"16 B","throughput":"7.0 M/s","bandwidth":"112 MB/s","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"64 B","throughput":"7.0 M/s","bandwidth":"446 MB/s","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"256 B","throughput":"6.7 M/s","bandwidth":"1.7 GB/s","p50":"181 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"1 KB","throughput":"6.4 M/s","bandwidth":"6.5 GB/s","p50":"181 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"4 KB","throughput":"5.3 M/s","bandwidth":"21.5 GB/s","p50":"230 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"8 KB","throughput":"4.7 M/s","bandwidth":"38.6 GB/s","p50":"250 ns"},{"sweep":"topics","grp":true,"system":"libtracer","x":"1","throughput":"6.9 M/s","bandwidth":"\u2014","p50":"160 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"4","throughput":"6.9 M/s","bandwidth":"\u2014","p50":"160 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"16","throughput":"6.4 M/s","bandwidth":"\u2014","p50":"161 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"64","throughput":"6.2 M/s","bandwidth":"\u2014","p50":"161 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"256","throughput":"6.3 M/s","bandwidth":"\u2014","p50":"161 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"1k","throughput":"5.8 M/s","bandwidth":"\u2014","p50":"180 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"4k","throughput":"5.7 M/s","bandwidth":"\u2014","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"1","throughput":"6.9 M/s","bandwidth":"\u2014","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"4","throughput":"6.8 M/s","bandwidth":"\u2014","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"16","throughput":"6.8 M/s","bandwidth":"\u2014","p50":"181 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"64","throughput":"6.7 M/s","bandwidth":"\u2014","p50":"190 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"256","throughput":"6.7 M/s","bandwidth":"\u2014","p50":"190 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"1k","throughput":"6.2 M/s","bandwidth":"\u2014","p50":"190 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"4k","throughput":"5.2 M/s","bandwidth":"\u2014","p50":"191 ns"},{"sweep":"net-udp","grp":true,"system":"libtracer","x":"16 B","throughput":"856 k/s","bandwidth":"14 MB/s","p50":"1.8 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"256 B","throughput":"1.2 M/s","bandwidth":"300 MB/s","p50":"1.7 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"1 KB","throughput":"1.1 M/s","bandwidth":"1.2 GB/s","p50":"1.8 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"8 KB","throughput":"771 k/s","bandwidth":"6.3 GB/s","p50":"2.2 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"16 B","throughput":"3.0 M/s","bandwidth":"47 MB/s","p50":"22 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"256 B","throughput":"3.0 M/s","bandwidth":"777 MB/s","p50":"12 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"1 KB","throughput":"3.8 M/s","bandwidth":"3.9 GB/s","p50":"9.7 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"8 KB","throughput":"983 k/s","bandwidth":"8.1 GB/s","p50":"10 \u00b5s"},{"sweep":"net-tcp","grp":true,"system":"libtracer","x":"16 B","throughput":"3.8 M/s","bandwidth":"61 MB/s","p50":"2.8 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"256 B","throughput":"1.1 M/s","bandwidth":"275 MB/s","p50":"3.3 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"1 KB","throughput":"755 k/s","bandwidth":"773 MB/s","p50":"3.3 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"8 KB","throughput":"662 k/s","bandwidth":"5.4 GB/s","p50":"3.0 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"16 B","throughput":"2.5 M/s","bandwidth":"40 MB/s","p50":"18 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"256 B","throughput":"3.4 M/s","bandwidth":"867 MB/s","p50":"16 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"1 KB","throughput":"3.4 M/s","bandwidth":"3.5 GB/s","p50":"8.1 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"8 KB","throughput":"703 k/s","bandwidth":"5.8 GB/s","p50":"21 \u00b5s"},{"sweep":"net-ws","grp":true,"system":"libtracer","x":"16 B","throughput":"3.2 M/s","bandwidth":"51 MB/s","p50":"2.8 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"256 B","throughput":"1.3 M/s","bandwidth":"338 MB/s","p50":"3.1 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"1 KB","throughput":"843 k/s","bandwidth":"863 MB/s","p50":"4.1 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"8 KB","throughput":"162 k/s","bandwidth":"1.3 GB/s","p50":"9.8 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"16 B","throughput":"3.7 M/s","bandwidth":"59 MB/s","p50":"11 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"256 B","throughput":"3.6 M/s","bandwidth":"913 MB/s","p50":"12 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"1 KB","throughput":"2.4 M/s","bandwidth":"2.5 GB/s","p50":"11 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"8 KB","throughput":"828 k/s","bandwidth":"6.8 GB/s","p50":"11 \u00b5s"}]}</script>
</div>
<script>// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Draws the libtracer-vs-Zenoh absolute-value comparison charts on the Performance
// page. Reads the measured data (emitted by bench/render_compare.py during the docs
// build) from the <script type="application/json" id="ltz-data"> block, and renders
// one inline SVG per chart plus a raw-number table. Absolute axes only — no ratios.
// Log axes get a real log grid (minor lines at 2..9 x 10^n, and at each power of two
// on the base-2 x axis). Vanilla JS, no dependencies.
(function () {
  var el = document.getElementById("ltz-data");
  if (!el) return;
  var DATA;
  try { DATA = JSON.parse(el.textContent); } catch (e) { return; }

  function fmtRate(v) { return v >= 1e6 ? (v / 1e6).toFixed(v < 1e7 ? 1 : 0) + " M" : v >= 1e3 ? (v / 1e3).toFixed(0) + " k" : "" + Math.round(v); }
  function fmtNs(v) { return v >= 1e6 ? (v / 1e6).toFixed(v < 1e7 ? 1 : 0) + " ms" : v >= 1e3 ? (v / 1e3).toFixed(v < 1e4 ? 1 : 0) + " µs" : Math.round(v) + " ns"; }
  function fmtBytes(v) { return v >= 1024 ? (v / 1024) + " KB" : v + " B"; }
  function fmtMB(v) { return v >= 1000 ? (v / 1000).toFixed(1) + " GB/s" : Math.round(v) + " MB/s"; }
  function fmtCount(v) { return v >= 1000 ? (v / 1000) + "k" : "" + v; }
  var FMT = { rate: fmtRate, ns: fmtNs, bytes: fmtBytes, mb: fmtMB, count: fmtCount };

  function logTicks(min, max) {
    var lo = Math.floor(Math.log10(min)), hi = Math.ceil(Math.log10(max)), t = [];
    for (var e = lo; e <= hi; e++) t.push(Math.pow(10, e));
    return t;
  }
  function linTicks(min, max) {
    var span = max - min || max, step = Math.pow(10, Math.floor(Math.log10(span))), n = span / step;
    var s = n < 2 ? step / 2 : n < 5 ? step : step * 2;
    var lo = Math.floor(min / s) * s, hi = Math.ceil(max / s) * s, t = [];
    for (var v = lo; v <= hi + 1e-9; v += s) t.push(v);
    return t;
  }

  var COL = { libtracer: "var(--ltz-lt)", zenoh: "var(--ltz-zn)" };

  function chart(cfg) {
    var W = 560, H = 350, m = { l: 66, r: 18, t: 14, b: 46 };
    var pw = W - m.l - m.r, ph = H - m.t - m.b;
    var xs = cfg.x.vals, xmin = Math.min.apply(null, xs), xmax = Math.max.apply(null, xs);
    var X = function (v) {
      return cfg.x.log
        ? m.l + (Math.log2(v) - Math.log2(xmin)) / (Math.log2(xmax) - Math.log2(xmin)) * pw
        : m.l + (v - xmin) / (xmax - xmin) * pw;
    };
    var all = [];
    cfg.series.forEach(function (s) { s.pts.forEach(function (p) { all.push(p[1]); }); });
    var ymin = Math.min.apply(null, all), ymax = Math.max.apply(null, all), yt;
    if (cfg.y.log) { yt = logTicks(ymin, ymax); ymin = yt[0]; ymax = yt[yt.length - 1]; }
    else { yt = linTicks(ymin, ymax); ymin = yt[0]; ymax = yt[yt.length - 1]; }
    var Y = function (v) {
      return cfg.y.log
        ? m.t + (1 - (Math.log10(v) - Math.log10(ymin)) / (Math.log10(ymax) - Math.log10(ymin))) * ph
        : m.t + (1 - (v - ymin) / (ymax - ymin)) * ph;
    };
    var yf = FMT[cfg.y.fmt], xf = FMT[cfg.x.fmt];
    var s = '<svg viewBox="0 0 ' + W + " " + H + '" role="img" aria-label="' + cfg.title + '">';
    // minor log grid first
    if (cfg.y.log) {
      var e0 = Math.round(Math.log10(ymin)), e1 = Math.round(Math.log10(ymax));
      for (var e = e0; e < e1; e++) for (var k = 2; k <= 9; k++) {
        var v = k * Math.pow(10, e); if (v <= ymin || v >= ymax) continue; var y = Y(v);
        s += '<line class="ltz-glm" x1="' + m.l + '" y1="' + y.toFixed(1) + '" x2="' + (W - m.r) + '" y2="' + y.toFixed(1) + '"/>';
      }
    }
    if (cfg.x.log) {
      for (var p = 1; p <= xmax; p *= 2) {
        if (p <= xmin || p >= xmax) continue; var x = X(p);
        s += '<line class="ltz-glm" x1="' + x.toFixed(1) + '" y1="' + m.t + '" x2="' + x.toFixed(1) + '" y2="' + (m.t + ph) + '"/>';
      }
    }
    yt.forEach(function (v) {
      var y = Y(v);
      s += '<line class="ltz-gl" x1="' + m.l + '" y1="' + y.toFixed(1) + '" x2="' + (W - m.r) + '" y2="' + y.toFixed(1) + '"/>';
      s += '<text class="ltz-tick" x="' + (m.l - 8) + '" y="' + (y + 3).toFixed(1) + '" text-anchor="end">' + yf(v) + "</text>";
    });
    xs.forEach(function (v) {
      var x = X(v);
      s += '<line class="ltz-gl" x1="' + x.toFixed(1) + '" y1="' + m.t + '" x2="' + x.toFixed(1) + '" y2="' + (m.t + ph) + '"/>';
      s += '<text class="ltz-tick" x="' + x.toFixed(1) + '" y="' + (m.t + ph + 16) + '" text-anchor="middle">' + xf(v) + "</text>";
    });
    s += '<rect class="ltz-frame" x="' + m.l + '" y="' + m.t + '" width="' + pw + '" height="' + ph + '"/>';
    s += '<text class="ltz-axtitle" x="' + (m.l + pw / 2) + '" y="' + (H - 6) + '" text-anchor="middle">' + cfg.x.label + "</text>";
    s += '<text class="ltz-axtitle" transform="translate(15 ' + (m.t + ph / 2) + ') rotate(-90)" text-anchor="middle">' + cfg.y.label + "</text>";
    cfg.series.forEach(function (se) {
      var pts = se.pts.map(function (p) { return X(p[0]).toFixed(1) + "," + Y(p[1]).toFixed(1); }).join(" ");
      s += '<polyline fill="none" stroke="' + se.color + '" stroke-width="2.2" stroke-linejoin="round" stroke-linecap="round" points="' + pts + '"/>';
      se.pts.forEach(function (p, i) {
        var last = i === se.pts.length - 1;
        s += '<circle cx="' + X(p[0]).toFixed(1) + '" cy="' + Y(p[1]).toFixed(1) + '" r="' + (last ? 4 : 2.6) + '" fill="' + se.color + '"/>';
      });
      var lp = se.pts[se.pts.length - 1];
      s += '<text x="' + (X(lp[0]) - 6).toFixed(1) + '" y="' + (Y(lp[1]) - 8).toFixed(1) + '" text-anchor="end" class="ltz-tick" style="fill:' + se.color + ';font-weight:700">' + yf(lp[1]) + "</text>";
    });
    s += "</svg>";
    return s;
  }

  var host = document.getElementById("ltz-charts");
  if (host) DATA.charts.forEach(function (c) {
    var series = ["libtracer", "zenoh"].map(function (sys) {
      return { name: sys, color: COL[sys], pts: (c.series[sys] || []) };
    }).filter(function (s) { return s.pts.length; });
    var xvals = (c.series.libtracer || c.series.zenoh || []).map(function (p) { return p[0]; });
    var card = document.createElement("div");
    card.className = "ltz-card";
    card.innerHTML = "<h4>" + c.title + '</h4><p class="cond">' + c.cond + "</p>"
      + chart({ title: c.title, x: { vals: xvals, log: c.x.log, fmt: c.x.fmt, label: c.x.label }, y: c.y, series: series })
      + '<p class="ltz-reading">' + c.reading + "</p>";
    host.appendChild(card);
  });

  var tb = document.querySelector("#ltz-raw tbody");
  if (tb && DATA.table) DATA.table.forEach(function (r, i) {
    var tr = document.createElement("tr");
    if (r.grp) tr.className = "grp";
    var dot = r.system === "libtracer" ? "var(--ltz-lt)" : "var(--ltz-zn)";
    tr.innerHTML = "<td>" + (r.sweep || "") + "</td>"
      + '<td class="sys"><span class="dot" style="background:' + dot + '"></span>' + r.system + "</td>"
      + "<td>" + r.x + "</td><td>" + r.throughput + "</td><td>" + r.bandwidth + "</td><td>" + r.p50 + "</td>";
    tb.appendChild(tr);
  });
})();
</script>
:::
