#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Generate docs/performance.md from LIVE harness output — so the published page is
auto-generated test+perf results, not a hand-maintained snapshot (ADR-0032 §auto-published).

Runs the cross-core conformance driver (run-all.py), the in-process perf bench
(bench_libtracer), the cross-core codec benches (cpp-core bench_codec + ts-core
perf.mjs + rust-core `cargo run --example perf`), and the coverage audit, and
renders a MyST page: the cross-match matrix, the canonical latency/throughput
table, the cross-core codec comparison, and type×opt coverage — followed by the
zenoh methodology/figures narrative. CI regenerates this in-place before
sphinx-build (docs.yml); the committed copy is the last snapshot. Each codec
runner degrades gracefully if its toolchain is missing (a note, not a crash).

  python3 bench/gen_results_page.py            # write docs/performance.md
Env: LIBTRACER_CXX_HARNESS (conformance_runner path); falls back to build dirs.
Stdlib only.
"""
from __future__ import annotations

import datetime
import os
import pathlib
import re
import shutil
import statistics
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import render_compare  # noqa: E402  (sibling module in bench/)
import render_history  # noqa: E402  (sibling module in bench/)

REPO = pathlib.Path(__file__).resolve().parent.parent
OUT = REPO / "docs" / "performance.md"
BENCH = REPO / "bench" / "build" / "bench_libtracer"
BENCH_ZENOH = REPO / "bench" / "build" / "bench_zenoh"
CODEC = REPO / "bench" / "build" / "bench_codec"
VECTORS = REPO / "tests" / "conformance" / "vectors" / "v1"
CXX = os.environ.get("LIBTRACER_CXX_HARNESS") or str(REPO / "build" / "tests" / "conformance_runner")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, cwd=REPO, **kw).stdout


def cross_core_block() -> str:
    env = dict(os.environ, LIBTRACER_CXX_HARNESS=CXX)
    out = subprocess.run([sys.executable, "tests/conformance/run-all.py"],
                         capture_output=True, text=True, cwd=REPO, env=env).stdout
    # Pull the per-core summary + the gate verdict for a compact table.
    summary = [ln.strip() for ln in out.splitlines()
               if re.match(r"\s*(cpp|ts|rust):", ln) or ln.startswith(("Type codes:", "Opt bits:", "COVERAGE:", "CONFORMANCE:"))]
    body = "\n".join(f"- {s}" for s in summary) or "- (driver produced no summary)"
    return body, ("CONFORMANCE: PASS" in out)


def perf_block() -> str:
    if not BENCH.exists():
        return "_(bench not built — `cmake -S bench -B bench/build -DCMAKE_BUILD_TYPE=Release && cmake --build bench/build --target bench_libtracer`)_"
    out = run([str(BENCH)])
    rows = []
    want = {("inproc", 64, 1, 1): "in-process write (store+notify+deliver)",
            ("inproc-deliver", 64, 1, 1): "in-process deliver-only (`propagate`)",
            ("inproc-borrow", 64, 1, 1): "in-process, zero-alloc loaned path (borrowed view)",
            ("inproc-path", 64, 1, 1): "write-by-path (registry lookup)"}
    seen = {}
    for ln in out.splitlines():
        f = ln.split("\t")
        if len(f) == 12 and f[0] == "RESULT":
            key = (f[2], int(f[3]), int(f[4]), int(f[5]))
            if key in want and key not in seen:
                seen[key] = (float(f[7]), int(f[9]), int(f[11]))  # deliv_s, p50ns, meanns
    rows.append("| path | p50 latency | mean | throughput |")
    rows.append("| --- | --- | --- | --- |")
    for key, label in want.items():
        if key in seen:
            dv, p50, mean = seen[key]
            rows.append(f"| {label} | {p50} ns | {mean} ns | {dv/1e6:.1f} M/s |")
    return "\n".join(rows)


def _parse_codec(out: str) -> list[tuple[int, float, int, int]]:
    """Parse `RESULT\\t...\\tcodec\\t...` lines (12 fields) -> (size, pub_s, p50, mean)."""
    rows = []
    for ln in out.splitlines():
        f = ln.split("\t")
        if len(f) == 12 and f[0] == "RESULT" and f[2] == "codec":
            rows.append((int(f[3]), float(f[6]), int(f[9]), int(f[11])))
    return rows


def _codec_cpp() -> tuple[list | None, str | None]:
    if not CODEC.exists():
        return None, "bench not built (`cmake --build bench/build --target bench_codec`)"
    rows = _parse_codec(run([str(CODEC), str(VECTORS)]))
    return (rows, None) if rows else (None, "bench produced no codec results")


def _codec_ts() -> tuple[list | None, str | None]:
    if shutil.which("node") is None:
        return None, "node toolchain not available"
    perf = REPO / "bindings" / "typescript" / "packages" / "core" / "bench" / "perf.mjs"
    if not perf.exists():
        return None, "ts perf bench missing"
    proc = subprocess.run(["node", str(perf), str(VECTORS)],
                          capture_output=True, text=True, cwd=REPO)
    rows = _parse_codec(proc.stdout)
    return (rows, None) if rows else (None, "ts-core bench produced no results")


def _codec_rust() -> tuple[list | None, str | None]:
    if shutil.which("cargo") is None:
        return None, "cargo toolchain not available"
    proc = subprocess.run(["cargo", "run", "--release", "--quiet", "--manifest-path",
                           "bindings/rust/Cargo.toml", "--example", "perf", "--", str(VECTORS)],
                          capture_output=True, text=True, cwd=REPO)
    rows = _parse_codec(proc.stdout)
    return (rows, None) if rows else (None, "rust-core bench unavailable (cargo build/run failed)")


def codec_block() -> str:
    """Cross-core codec comparison: run all three codec benches over the v1 vectors
    and tabulate the median throughput + p50/mean latency per core. Each runner
    degrades gracefully (a note instead of a crash) if its toolchain is absent, so
    the docs build never hard-fails on a missing core."""
    cores = [("cpp-core", _codec_cpp), ("ts-core", _codec_ts), ("rust-core", _codec_rust)]
    rows = ["| core | throughput (median) | p50 latency (median) | mean (median) |",
            "| --- | --- | --- | --- |"]
    notes: list[str] = []
    any_ok = False
    for name, fn in cores:
        try:
            data, err = fn()
        except OSError as e:  # toolchain present but blew up — degrade, don't crash
            data, err = None, str(e)
        if not data:
            notes.append(f"- _{name}: unavailable — {err}_")
            continue
        any_ok = True
        pub = statistics.median(r[1] for r in data)
        p50 = statistics.median(r[2] for r in data)
        mean = statistics.median(r[3] for r in data)
        rows.append(f"| {name} | {pub / 1e6:.1f} M roundtrips/s | {int(p50)} ns | {int(mean)} ns |")
    body = "\n".join(rows)
    if not any_ok:
        body = "_(no core codec bench available in this environment — toolchains absent)_"
    if notes:
        body += "\n\n" + "\n".join(notes)
    return body


SURFACES_BLOCK = """\
## The measurement surfaces (how this page is organized)

Every number below belongs to exactly ONE of these measurement approaches. They use
different harnesses, different processes, and different units — **a value is only
comparable to values from the same surface**, never across surfaces.

| § | surface | what it measures | harness | discipline |
| --- | --- | --- | --- | --- |
| 1 | Cross-core conformance | byte-exactness across cores (not speed) | `run-all.py` | any DISAGREE fails CI |
| 2 | In-process latency & throughput | single-process dispatch cost (the µs thesis) | `bench_libtracer` | gated per PR **and** per `main` push, same-runner |
| 3 | Memory footprint | heap allocations counted, not timed | `bench_forward_heap` probes + max RSS | forward hop hard-gated at ZERO allocs |
| 4 | libtracer vs Zenoh | absolute side-by-side, both engines same pass | `bench_libtracer` + `bench_zenoh` (+ loopback net) | same runner, same pass — no ratios |
| 5 | Cross-core codec | decode→encode roundtrip per implementation | cpp / ts / rust codec benches | same v1 vectors for all cores |

**Enforcement (what actually stops a pullback).** Absolute nanoseconds vary ~2× with
the CI runner drawn, so raw chart height is a *trend* signal, not a gate. The gates are
all **same-runner relative** comparisons, where machine speed cancels:

- **per PR** — `bench/perf_gate.py` builds `main`'s bench *and* the PR's bench on one
  runner and fails the PR if any of six canonical points regresses (p50 **+15 %** /
  deliveries/s **−12 %**, best-of-3 runs);
- **per `main` push** — the same gate re-runs HEAD against its **parent commit** on
  **three independently-drawn runners** (the redundant no-pullback ratchet; each
  replica is a complete same-runner experiment): a regression that lands anyway
  turns `main` red, and a single noisy runner cannot manufacture the verdict;
- **trend soft-alert** — the history tracker comments on a commit whose series drifts
  past 125 % of the previous point (cross-runner, so an alert is a prompt to look,
  not a verdict).
"""


COMPARE_INTRO = """\
## 4 · libtracer vs Zenoh — measured, absolute

A side-by-side comparison against [Eclipse Zenoh](https://zenoh.io) (zenoh-c 1.9.0, peer
mode). Two surfaces: three **in-process** axes — subscriber **fan-out**, **payload** size,
and **topic count** — and a **network** comparison over the real loopback kernel path. Both engines are built
`-O3` and measured in the **same pass on the same runner**, so the numbers are directly
comparable on identical hardware. The charts plot **absolute** throughput / latency /
bandwidth — libtracer and Zenoh as series on shared axes — so you read the real
numbers off the graph; there are no speed-up ratios.

**Semantic fairness.** libtracer's `write` row also **persists** the value (it becomes
the vertex's last-known-value) and bumps the `await`/readiness sequence on every op;
Zenoh's `put` is transient delivery only — so the libtracer write row does **strictly
more semantic work** per op than the Zenoh row it is charted against. The
**deliver-only (`propagate`)** series is the apples-to-apples counterpart: the value is
stored once and each op only delivers, matching Zenoh's put semantics. Note also that
**ACL enforcement is disabled** in the comparison rows (no subject resolver installed,
so the gate is a single null check); the gated cost is measured separately by the
`acl-inherit-d4` rows on this page.

Network **throughput** is charted against **composition size K**, because throughput here
comes from *batching*, and the two engines batch differently. libtracer batches by
**composition**: a composite endpoint's value is a K-link rope already in memory, shipped
as **one datagram** (`send(iov)` — one syscall for K values), so effective values/s scale
with K *at flat latency*. Zenoh has no composite send; its throughput is the transport's
**timer-batched** put rate, independent of K — so it plots as a flat reference. (A single
one-value-per-`send` rate would be the unbatched worst case for libtracer and is not the
throughput path.) Network **latency** is the separate per-transport (**UDP** / **TCP**),
single-value, two-process measurement — the same topology for both engines, so it is fair.

WebSocket and QUIC are not yet charted: libtracer's WebSocket transport shows large
single-run latency spikes under this bench (order-of-magnitude p50 jitter) that would make
a published latency chart misleading, and QUIC needs the `-DLIBTRACER_WITH_QUIC` module
(msquic + TLS). Full harness in
[`bench/`](https://github.com/avatarsd-llc/libtracer/tree/main/bench).

### Why the throughput charts show different numbers

Several charts below are titled "throughput" and their absolute values differ by orders
of magnitude. That is expected: **each chart sweeps a different variable while pinning a
different scenario**, so each has a different denominator. Compare series *within* one
chart (same scenario, both engines); never compare heights *across* charts.

| chart | swept variable | pinned scenario | unit | expected shape |
| --- | --- | --- | --- | --- |
| Throughput vs fan-out | subscribers per write | 64 B payload · 1 topic · in-process | deliveries/s | rises with fan-out (per-write dispatch amortizes) |
| Throughput vs payload | payload size | fan-out 1 · 1 topic · in-process | deliveries/s | falls as payload grows (copy-bound) |
| Bandwidth vs payload | payload size | **same run** as "vs payload" | MB/s | same data re-expressed in bytes — rises with payload |
| Throughput vs topic count | number of vertices | 64 B · fan-out 1 · in-process | deliveries/s | ~flat; probes registry pressure |
| Network throughput vs composition size | composite size K | 64 B values · loopback UDP · two processes | effective values/s | rises with K for libtracer (one datagram per composite); flat for Zenoh (timer batching) |

The per-commit history store adds one more deliberate duplication: throughput is
recorded **twice** — natural `deliveries/s` in the bigger-is-better suite *and* inverted
`ns/delivery` in the smaller-is-better latency suite (so a slowdown always charts as a
rise there). Same measurement, two units."""


HISTORY_BLOCK = """\
### History & trends (per-commit, `main`)

The table above is one run; the trend is the durable signal. Every push to `main`
runs the full bench on **three independently-drawn runners**, archives all raw
transcripts as a per-commit CI artifact (`bench-results-<sha>`, on the `perf`
workflow run), and records **every** `(mode, size, fanout, endpoints)` point —
latency (p50/p99 ns), throughput (deliveries/s), and memory footprint (heap-probe
bytes per hop, whole-run max RSS) as **separate series** — to a persisted
build-to-build history. Per metric the recorded value is the **best across the
three runners** (min latency / max throughput): machine speed varies ~2× between
runner draws, so best-of-3 approximates the code's capability rather than the
machine lottery
([benchmark-action/github-action-benchmark](https://github.com/benchmark-action/github-action-benchmark),
stored on the machine-maintained `gh-pages` branch). A commit that drifts a series
past **125 %** of the previous point gets an automatic soft-alert comment; the hard
per-PR gate stays in `bench/perf_gate.py`.

**[Open the interactive trend charts ↗](https://libtracer.avatarsd.com/dev/bench/)**
— every series across all `main` commits, zoomable, with per-point commit links.

:::{raw} html
<iframe src="/dev/bench/index.html" title="libtracer benchmark history (per-commit trends)"
        loading="lazy"
        style="width:100%;height:70vh;border:1px solid var(--color-background-border,#d0d0d0);border-radius:6px;background:#fff">
</iframe>
:::"""


READING_BLOCK = """\
### Reading these numbers

- **Sign conventions.** The history store charts one direction per suite: the
  *latency* suite is smaller-is-better nanoseconds — throughput also appears there
  inverted as `ns/delivery` (`1e9 / deliveries-per-second`) so a slowdown always
  charts as a rise — and memory-footprint metrics (bytes, KB) live in the same
  smaller-is-better suite. The *throughput* suite is bigger-is-better, natural
  `deliveries/s`. On this page, throughput is shown in natural units.
- **Three thresholds, three jobs.** The **hard PR gate** (`bench/perf_gate.py`)
  fails a PR whose p50 exceeds **115 %** of its own same-runner `main` baseline or
  whose throughput drops below **88 %**, over six canonical points, best-of-3 runs.
  The **push ratchet** re-runs the same gate on every `main` push — HEAD against its
  parent commit on one runner — so a pullback that lands anyway turns `main` red.
  The **soft alert** (trend tracker, per `main` commit, cross-runner) comments at
  **125 %** commit-to-commit; because it compares across runners it is a prompt to
  look at the trend, not a verdict.
- **Noise floor.** Each recorded point is the **median of the repeated RESULT
  rows** one run emits, so single-iteration jitter does not move the series — but
  shared CI runners still vary ~2× in absolute speed — which is why each point is
  recorded as the best across three runner draws — and sub-µs points sit on a
  ~10 ns timer grain. The tell: **a move that hits every series at once —
  including unrelated ones like the pure-codec `fold-n*` rows — is the runner; a
  move confined to one family is the code.** Read trends across several commits,
  not the third digit of one point. Reproducing locally: build Release
  (`-O3`), pin the bench to a core (`taskset -c N`), take the best of several runs,
  and compare only numbers measured on the same machine in the same session.
- **`inproc-path` is a resolver canary, not a hot pattern.** The write-by-path
  rows exercise the registry lookup on every write *on purpose*, so a resolver-cost
  regression is visible as its own series. Hot paths resolve the path once and
  write through the held vertex handle (the `inproc` / `inproc-borrow` rows) —
  compare against those, not `inproc-path`, when judging dispatch cost."""


def _load_history() -> dict | None:
    """@brief Load the benchmark-action store (gh-pages `dev/bench/data.js`) as JSON.

    Source order: a local `dev/bench/data.js` (present when a caller pre-mirrored the
    branch), else a best-effort shallow fetch of `origin/gh-pages` + `git show`.
    Returns None (a note, not a crash) when the store is unreachable — e.g. a fork
    without the branch or an offline build.
    """
    import json
    raw = None
    local = REPO / "dev" / "bench" / "data.js"
    if local.exists():
        raw = local.read_text()
    else:
        subprocess.run(["git", "fetch", "--depth=1", "origin", "gh-pages"],
                       capture_output=True, cwd=REPO)
        for ref in ("FETCH_HEAD", "origin/gh-pages"):
            p = subprocess.run(["git", "show", f"{ref}:dev/bench/data.js"],
                               capture_output=True, text=True, cwd=REPO)
            if p.returncode == 0 and p.stdout:
                raw = p.stdout
                break
    if not raw:
        return None
    raw = raw.strip()
    raw = raw.removeprefix("window.BENCHMARK_DATA = ").rstrip(";")
    try:
        return json.loads(raw)
    except ValueError:
        return None


def _spark(vals: list[float]) -> str:
    """@brief Render a min-max-normalized unicode sparkline for a value series."""
    bars = "▁▂▃▄▅▆▇█"
    lo, hi = min(vals), max(vals)
    if hi <= lo:
        return bars[0] * len(vals)
    return "".join(bars[min(7, int((v - lo) / (hi - lo) * 7.999))] for v in vals)


def _fmt_val(v: float) -> str:
    """@brief Human-compact number: 12.3M / 4.5k / 929 / 23.8."""
    a = abs(v)
    if a >= 1e6:
        return f"{v / 1e6:.3g}M"
    if a >= 1e4:
        return f"{v / 1e3:.3g}k"
    return f"{v:.4g}"


def unified_history_block(data: dict | None) -> str:
    """@brief The UNIFIED family trend charts: one chart per series-family with
    every series of the family as a line on shared axes (render_history.py),
    release tags as labeled vertical markers, and — for numeric-parameter
    families — the sweep / heatmap / 3D three-axis views. Self-contained inline
    SVG+JS, no CDN; degrades to a note when the store is unreachable."""
    if not data:
        return ("_(per-commit history store unreachable in this build — the interactive"
                " chart link above still serves it once published)_")
    try:
        block = render_history.html_block(data)
    except Exception as e:  # a malformed store must never break the docs build
        return f"_(unified history charts unavailable in this build — {e})_"
    return block or "_(no chartable series family in the history store yet)_"


def history_tables_block(data: dict | None) -> str:
    """@brief The IN-PAGE per-commit history: every tracked series across every
    `main` commit in the store, as compact tables (first→last, extremes, sparkline).

    This is the same data the interactive chart plots, embedded as text so the
    history survives wherever the iframe cannot (PDF export, RSS scrapers, a
    momentarily unpublished `/dev/bench/`), and so one page carries current
    numbers, their full history, and the Zenoh comparison side by side. Release
    tags are noted per suite (render_history resolves them via git).
    """
    if not data:
        return ("_(per-commit history store unreachable in this build — the interactive"
                " chart link above still serves it once published)_")
    out: list[str] = []
    for suite, entries in data.get("entries", {}).items():
        if not entries:
            continue
        first_c = entries[0]["commit"]["id"][:7]
        last_c = entries[-1]["commit"]["id"][:7]
        series: dict[str, list[float]] = {}
        for e in entries:
            for b in e.get("benches", []):
                series.setdefault(b["name"], []).append(float(b["value"]))
        unit = entries[-1]["benches"][0].get("unit", "") if entries[-1].get("benches") else ""
        out.append(f"### {suite}")
        out.append("")
        out.append(f"{len(entries)} tracked commit(s), `{first_c}` → `{last_c}`; unit: {unit}.")
        rel = render_history.release_note(entries)
        if rel:
            out.append("")
            out.append(rel)
        out.append("")
        out.append("| series | pts | first → last | Δ | min … max | trend |")
        out.append("| --- | --- | --- | --- | --- | --- |")
        for name in sorted(series):
            v = series[name]
            delta = f"{(v[-1] - v[0]) / v[0] * 100:+.1f}%" if len(v) > 1 and v[0] else "—"
            out.append(f"| {name} | {len(v)} | {_fmt_val(v[0])} → {_fmt_val(v[-1])} | {delta} "
                       f"| {_fmt_val(min(v))} … {_fmt_val(max(v))} | `{_spark(v)}` |")
        out.append("")
    return "\n".join(out).rstrip()


def tests_block() -> str:
    """@brief The unified test rollup: live ctest summary inline on this page,
    with the per-suite detail remaining on the Test report page."""
    try:
        import gen_test_report as tr
        results = tr.run_ctest_junit(tr.BUILD)
    except Exception:
        results = []
    if not results:
        return ("_(the Release test build was not available in this pass — see the"
                " [full test report](test-report.md))_")
    total = len(results)
    passed = sum(1 for _, s, _ in results if s == "pass")
    wall = sum(t for _, _, t in results)
    cats: dict[str, list[int]] = {}
    for name, s, _ in results:
        c = cats.setdefault(tr.category_of(name), [0, 0])
        c[0] += 1
        c[1] += 1 if s == "pass" else 0
    verdict = "✅ all green" if passed == total else f"❌ {total - passed} failing"
    rows = " · ".join(f"{k} {v[1]}/{v[0]}" for k, v in sorted(cats.items()))
    return (f"| suites | passing | wall time | verdict |\n| --- | --- | --- | --- |\n"
            f"| {total} | {passed}/{total} | {wall:.2f}s | {verdict} |\n\n"
            f"By area: {rows}. Full per-suite detail: [Test report](test-report.md).")


def zenoh_compare_block() -> str:
    """Run both grids and render the absolute-value comparison charts. Degrades to a
    note (never a crash) if the bench isn't built or Zenoh isn't vendored."""
    tsv = os.environ.get("LIBTRACER_COMPARE_TSV")
    if tsv and pathlib.Path(tsv).exists():
        combined = pathlib.Path(tsv).read_text()  # CI runs the sweep once, shared with the PR comment
    elif BENCH.exists():
        combined = run([str(BENCH), "grid"])
        if BENCH_ZENOH.exists():
            combined += "\n" + run([str(BENCH_ZENOH), "grid"])
    else:
        return "_(bench not built — `cmake -S bench -B bench/build -DCMAKE_BUILD_TYPE=Release && cmake --build bench/build`)_"
    rows = render_compare.parse(combined)
    if not render_compare.has_zenoh(rows):
        return ("_(Zenoh not vendored in this build, so the comparison charts are omitted."
                " Run [`bench/fetch_zenoh.sh`](https://github.com/avatarsd-llc/libtracer/tree/main/bench)"
                " before the bench build to generate them; the libtracer numbers above still apply.)_")
    return render_compare.html_block(rows, provenance())


def provenance() -> str:
    """A one-line CI-generated stamp: date, commit, run, runner.

    In GitHub Actions the GITHUB_* / RUNNER_OS env vars pin exactly which deploy
    produced these numbers (so the figures are auditable per deploy, not a stale
    hand-edit). Off CI it degrades to a plain local-build note.
    """
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    if os.environ.get("GITHUB_ACTIONS") != "true":
        return f"_Generated from a local build on {date} (not a CI deploy)._"
    sha = os.environ.get("GITHUB_SHA", "")
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    run_id = os.environ.get("GITHUB_RUN_ID", "")
    runner = os.environ.get("RUNNER_OS", "")
    commit = f"[`{sha[:7]}`]({server}/{repo}/commit/{sha})" if sha and repo else "unknown commit"
    run = f"[run {run_id}]({server}/{repo}/actions/runs/{run_id})" if run_id and repo else "CI run"
    runner_note = f" · runner `{runner}`" if runner else ""
    return f"**🤖 CI-generated** on {date} · commit {commit} · {run}{runner_note}."


def main() -> int:
    summary, passed = cross_core_block()
    history = _load_history()
    page = f"""\
# Performance & Conformance

```{{note}}
This page is **auto-generated** from the live test + benchmark harnesses on each docs
build (`bench/gen_results_page.py`, ADR-0032). It is the published response surface,
not a hand-edited snapshot. All rates and latencies are **absolute measured values**,
representative of the CI runner (shared-runner variance is real — read trends, not the
third digit); the libtracer-vs-Zenoh charts below plot both engines on the same axes.

{provenance()}
```

{SURFACES_BLOCK}

## 1 · Cross-core conformance (every native core must agree byte-for-byte)

The shared conformance vectors are decoded+re-encoded by every enabled core; a DISAGREE
fails CI (ADR-0028). Live driver summary:

{summary}

## 2 · In-process latency & throughput

Canonical points from `bench_libtracer` (the µs-latency / zero-copy thesis, ADR-0031):

{perf_block()}

{HISTORY_BLOCK}

#### Unified family trends (all related series on one axes; releases marked)

Instead of one tiny chart per series, related series are grouped into **families** —
fan-out sweep, payload sweep, MT scaling, dispatch modes, endpoint types, fold widths,
ACL, heap/memory — and each family is ONE chart with its series as lines on shared axes
(log axes where a family spans decades). Release tags are drawn as labeled vertical
markers on every chart (**≈** = the tag's commit is not itself a recorded point; the
marker sits at the nearest following recorded commit). Families swept over a numeric
parameter carry the full three-axis view set — **trend** (value vs commit), **sweep**
(value vs parameter, one line per commit), **heatmap** (commit × parameter, color =
value), and an isometric **3D** surface — switchable per chart; hover for exact values.

{unified_history_block(history)}

#### Full history, in-page (every tracked series, all recorded `main` commits)

{history_tables_block(history)}

{READING_BLOCK}

## 3 · Memory footprint (allocations counted, not timed)

A different instrument entirely: `bench_forward_heap` replaces the global allocator
with a counting wrapper and arms it around exactly one operation — so these are exact
allocation counts and bytes, not statistics. Four probes feed the history store above:

- **forward hop** — hard-gated at **zero** allocations every CI run (ADR-0038 §16KB-RAM);
- **terminus resolve** — report-only; a terminus may allocate (ADR-0039), the probe
  keeps the cost visible;
- **per-vertex steady heap** — LIVE usable-size bytes a default leaf holds, and the
  increment one small LKV write adds (the vertex-diet trend, #361);
- **whole-run max RSS** — the coarse process-level footprint.

Their series live in the smaller-is-better history suite (§2) under
`heap bytes per … (probe)` and `max RSS`.

{COMPARE_INTRO}

{zenoh_compare_block()}

## Test rollup (live ctest, unified with the perf surface)

{tests_block()}

## 5 · Cross-core codec performance (decode→encode roundtrip, same v1 vectors)

Every native core (cpp-core / ts-core / rust-core) runs the SAME per-vector
decode→encode roundtrip over the shared v1 conformance vectors (ADR-0032 `lang`
axis, #96), so this is a like-for-like codec surface across implementations.
Figures are the **median across all v1 vectors** (one decode + one encode == one
roundtrip); a core whose toolchain is absent in this build degrades to a note.

{codec_block()}
"""
    OUT.write_text(page)
    print(f"wrote {OUT.relative_to(REPO)} ({'conformance PASS' if passed else 'conformance check ran'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
