# Performance & Conformance

```{note}
This page is **auto-generated** from the live test + benchmark harnesses on each docs
build (`bench/gen_results_page.py`, ADR-0032). It is the published response surface,
not a hand-edited snapshot. All rates and latencies are **absolute measured values**,
representative of the CI runner (shared-runner variance is real — read trends, not the
third digit); the libtracer-vs-Zenoh charts below plot both engines on the same axes.

_Generated from a local build on 2026-07-07 10:05 UTC (not a CI deploy)._
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
| in-process (zero-copy dispatch) | 171 ns | 175 ns | 6.3 M/s |
| in-process, zero-alloc loaned path | 121 ns | 127 ns | 9.6 M/s |
| write-by-path (registry lookup) | 160 ns | 163 ns | 6.9 M/s |

## Cross-core codec performance (decode→encode roundtrip, same v1 vectors)

Every native core (cpp-core / ts-core / rust-core) runs the SAME per-vector
decode→encode roundtrip over the shared v1 conformance vectors (ADR-0032 `lang`
axis, #96), so this is a like-for-like codec surface across implementations.
Figures are the **median across all v1 vectors** (one decode + one encode == one
roundtrip); a core whose toolchain is absent in this build degrades to a note.

| core | throughput (median) | p50 latency (median) | mean (median) |
| --- | --- | --- | --- |
| cpp-core | 5.1 M roundtrips/s | 205 ns | 211 ns |
| ts-core | 1.1 M roundtrips/s | 931 ns | 992 ns |
| rust-core | 5.0 M roundtrips/s | 220 ns | 221 ns |

## libtracer vs Zenoh — measured, absolute

A side-by-side comparison against [Eclipse Zenoh](https://zenoh.io) (zenoh-c 1.9.0, peer
mode), swept across three in-process axes — subscriber **fan-out**, **payload** size, and
**topic count**. Both engines are built `-O3` and measured in the **same pass on the same
runner**, so the numbers are directly comparable on identical hardware. The charts plot
**absolute** throughput / latency / bandwidth — libtracer and Zenoh as two series on
shared axes — so you read the real numbers off the graph; there are no speed-up ratios.
Full harness in [`bench/`](https://github.com/avatarsd-llc/libtracer/tree/main/bench).

:::{raw} html
<link rel="stylesheet" href="_static/ltz_compare.css">
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
  <p class="ltz-prov">_Generated from a local build on 2026-07-07 10:05 UTC (not a CI deploy)._</p>
  <script type="application/json" id="ltz-data">{"charts":[{"id":"tp-fan","title":"Throughput vs fan-out","cond":"64 B payload \u00b7 1 topic \u00b7 in-process","x":{"log":true,"fmt":"count","label":"subscribers per topic (fan-out)"},"y":{"log":true,"fmt":"rate","label":"deliveries / second"},"series":{"libtracer":[[1,9032953.0],[4,26094477.0],[16,49492592.0],[64,66615920.0],[256,71209925.0],[1024,74602503.0],[4096,73602211.0]],"zenoh":[[1,6911107.0],[4,11398336.0],[16,14940384.0],[64,14655582.0],[256,12663792.0],[1024,13218415.0],[4096,16902514.0]]},"reading":"Across the sweep (1 \u2192 4k): <b>libtracer 9.0 M\u2009\u2192\u200974 M</b>; <b>Zenoh 6.9 M\u2009\u2192\u200917 M</b>."},{"id":"lat-fan","title":"p50 latency vs fan-out","cond":"64 B payload \u00b7 1 topic \u00b7 in-process","x":{"log":true,"fmt":"count","label":"subscribers per topic (fan-out)"},"y":{"log":true,"fmt":"ns","label":"p50 latency"},"series":{"libtracer":[[1,121],[4,170],[16,341],[64,962],[256,3447],[1024,13405],[4096,53270]],"zenoh":[[1,180],[4,391],[16,1092],[64,3988],[256,18725],[1024,59112],[4096,234080]]},"reading":"Across the sweep (1 \u2192 4k): <b>libtracer 121 ns\u2009\u2192\u200953 \u00b5s</b>; <b>Zenoh 180 ns\u2009\u2192\u2009234 \u00b5s</b>."},{"id":"tp-size","title":"Throughput vs payload","cond":"1 subscriber \u00b7 1 topic \u00b7 in-process","x":{"log":true,"fmt":"bytes","label":"payload size"},"y":{"log":true,"fmt":"rate","label":"deliveries / second"},"series":{"libtracer":[[1,8911162.0],[16,8945003.0],[64,9032953.0],[256,8494170.0],[1024,8550531.0],[4096,6983364.0],[8192,6177428.0]],"zenoh":[[1,4192458.0],[16,6493265.0],[64,6911107.0],[256,6769270.0],[1024,6568746.0],[4096,5337905.0],[8192,4753649.0]]},"reading":"Across the sweep (1 B \u2192 8 KB): <b>libtracer 8.9 M\u2009\u2192\u20096.2 M</b>; <b>Zenoh 4.2 M\u2009\u2192\u20094.8 M</b>."},{"id":"mb-size","title":"Bandwidth vs payload","cond":"1 subscriber \u00b7 1 topic \u00b7 in-process","x":{"log":true,"fmt":"bytes","label":"payload size"},"y":{"log":true,"fmt":"mb","label":"application bandwidth"},"series":{"libtracer":[[1,8.9],[16,143.1],[64,578.1],[256,2174.5],[1024,8755.7],[4096,28603.9],[8192,50605.5]],"zenoh":[[1,4.2],[16,103.9],[64,442.3],[256,1732.9],[1024,6726.4],[4096,21864.1],[8192,38941.9]]},"reading":"Across the sweep (1 B \u2192 8 KB): <b>libtracer 9 MB/s\u2009\u2192\u200950.6 GB/s</b>; <b>Zenoh 4 MB/s\u2009\u2192\u200938.9 GB/s</b>."},{"id":"tp-ep","title":"Throughput vs topic count","cond":"64 B \u00b7 1 subscriber \u00b7 write-by-path","x":{"log":true,"fmt":"count","label":"number of topics"},"y":{"log":false,"fmt":"rate","label":"publishes / second"},"series":{"libtracer":[[1,6864120.0],[4,6778273.0],[16,6616108.0],[64,6659430.0],[256,6268671.0],[1024,5688502.0],[4096,5544724.0]],"zenoh":[[1,6764831.0],[4,6646634.0],[16,6715647.0],[64,6589438.0],[256,6672661.0],[1024,6057800.0],[4096,5336260.0]]},"reading":"Across the sweep (1 \u2192 4k): <b>libtracer 6.9 M\u2009\u2192\u20095.5 M</b>; <b>Zenoh 6.8 M\u2009\u2192\u20095.3 M</b>."},{"id":"lat-ep","title":"p50 latency vs topic count","cond":"64 B \u00b7 1 subscriber \u00b7 write-by-path","x":{"log":true,"fmt":"count","label":"number of topics"},"y":{"log":false,"fmt":"ns","label":"p50 latency"},"series":{"libtracer":[[1,160],[4,151],[16,160],[64,160],[256,170],[1024,180],[4096,180]],"zenoh":[[1,190],[4,180],[16,181],[64,191],[256,190],[1024,200],[4096,191]]},"reading":"Across the sweep (1 \u2192 4k): <b>libtracer 160 ns\u2009\u2192\u2009180 ns</b>; <b>Zenoh 190 ns\u2009\u2192\u2009191 ns</b>."}],"table":[{"sweep":"fan-out","grp":false,"system":"libtracer","x":"1","throughput":"9.0 M/s","bandwidth":"\u2014","p50":"121 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"4","throughput":"26 M/s","bandwidth":"\u2014","p50":"170 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"16","throughput":"49 M/s","bandwidth":"\u2014","p50":"341 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"64","throughput":"67 M/s","bandwidth":"\u2014","p50":"962 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"256","throughput":"71 M/s","bandwidth":"\u2014","p50":"3.4 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"1k","throughput":"75 M/s","bandwidth":"\u2014","p50":"13 \u00b5s"},{"sweep":"","grp":false,"system":"libtracer","x":"4k","throughput":"74 M/s","bandwidth":"\u2014","p50":"53 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"1","throughput":"6.9 M/s","bandwidth":"\u2014","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"4","throughput":"11 M/s","bandwidth":"\u2014","p50":"391 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"16","throughput":"15 M/s","bandwidth":"\u2014","p50":"1.1 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"64","throughput":"15 M/s","bandwidth":"\u2014","p50":"4.0 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"256","throughput":"13 M/s","bandwidth":"\u2014","p50":"19 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"1k","throughput":"13 M/s","bandwidth":"\u2014","p50":"59 \u00b5s"},{"sweep":"","grp":false,"system":"zenoh","x":"4k","throughput":"17 M/s","bandwidth":"\u2014","p50":"234 \u00b5s"},{"sweep":"payload","grp":true,"system":"libtracer","x":"1 B","throughput":"8.9 M/s","bandwidth":"9 MB/s","p50":"121 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"16 B","throughput":"8.9 M/s","bandwidth":"143 MB/s","p50":"130 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"64 B","throughput":"9.0 M/s","bandwidth":"578 MB/s","p50":"121 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"256 B","throughput":"8.5 M/s","bandwidth":"2.2 GB/s","p50":"130 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"1 KB","throughput":"8.6 M/s","bandwidth":"8.8 GB/s","p50":"130 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"4 KB","throughput":"7.0 M/s","bandwidth":"28.6 GB/s","p50":"151 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"8 KB","throughput":"6.2 M/s","bandwidth":"50.6 GB/s","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"1 B","throughput":"4.2 M/s","bandwidth":"4 MB/s","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"16 B","throughput":"6.5 M/s","bandwidth":"104 MB/s","p50":"190 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"64 B","throughput":"6.9 M/s","bandwidth":"442 MB/s","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"256 B","throughput":"6.8 M/s","bandwidth":"1.7 GB/s","p50":"181 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"1 KB","throughput":"6.6 M/s","bandwidth":"6.7 GB/s","p50":"190 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"4 KB","throughput":"5.3 M/s","bandwidth":"21.9 GB/s","p50":"221 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"8 KB","throughput":"4.8 M/s","bandwidth":"38.9 GB/s","p50":"250 ns"},{"sweep":"topics","grp":true,"system":"libtracer","x":"1","throughput":"6.9 M/s","bandwidth":"\u2014","p50":"160 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"4","throughput":"6.8 M/s","bandwidth":"\u2014","p50":"151 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"16","throughput":"6.6 M/s","bandwidth":"\u2014","p50":"160 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"64","throughput":"6.7 M/s","bandwidth":"\u2014","p50":"160 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"256","throughput":"6.3 M/s","bandwidth":"\u2014","p50":"170 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"1k","throughput":"5.7 M/s","bandwidth":"\u2014","p50":"180 ns"},{"sweep":"","grp":false,"system":"libtracer","x":"4k","throughput":"5.5 M/s","bandwidth":"\u2014","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"1","throughput":"6.8 M/s","bandwidth":"\u2014","p50":"190 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"4","throughput":"6.6 M/s","bandwidth":"\u2014","p50":"180 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"16","throughput":"6.7 M/s","bandwidth":"\u2014","p50":"181 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"64","throughput":"6.6 M/s","bandwidth":"\u2014","p50":"191 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"256","throughput":"6.7 M/s","bandwidth":"\u2014","p50":"190 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"1k","throughput":"6.1 M/s","bandwidth":"\u2014","p50":"200 ns"},{"sweep":"","grp":false,"system":"zenoh","x":"4k","throughput":"5.3 M/s","bandwidth":"\u2014","p50":"191 ns"}]}</script>
</div>
<script src="_static/ltz_compare.js"></script>
:::
