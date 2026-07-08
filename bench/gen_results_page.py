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
    want = {("inproc", 64, 1, 1): "in-process (zero-copy dispatch)",
            ("inproc-borrow", 64, 1, 1): "in-process, zero-alloc loaned path",
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


COMPARE_INTRO = """\
## libtracer vs Zenoh — measured, absolute

A side-by-side comparison against [Eclipse Zenoh](https://zenoh.io) (zenoh-c 1.9.0, peer
mode). Two surfaces: three **in-process** axes — subscriber **fan-out**, **payload** size,
and **topic count** — and a **network** comparison over the real loopback kernel path. Both engines are built
`-O3` and measured in the **same pass on the same runner**, so the numbers are directly
comparable on identical hardware. The charts plot **absolute** throughput / latency /
bandwidth — libtracer and Zenoh as two series on shared axes — so you read the real
numbers off the graph; there are no speed-up ratios.

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
[`bench/`](https://github.com/avatarsd-llc/libtracer/tree/main/bench)."""


HISTORY_BLOCK = """\
## History & trends (per-commit, `main`)

The table above is one run; the trend is the durable signal. Every push to `main`
runs the full bench once, archives the raw transcript as a per-commit CI artifact
(`bench-results-<sha>`, on the `perf` workflow run), and records **every**
`(mode, size, fanout, endpoints)` point — latency (p50/p99 ns), throughput
(deliveries/s), and memory footprint (heap-probe bytes per hop, whole-run max RSS)
as **separate series** — to a persisted build-to-build history
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
## Reading these numbers

- **Sign conventions.** The history store charts one direction per suite: the
  *latency* suite is smaller-is-better nanoseconds — throughput also appears there
  inverted as `ns/delivery` (`1e9 / deliveries-per-second`) so a slowdown always
  charts as a rise — and memory-footprint metrics (bytes, KB) live in the same
  smaller-is-better suite. The *throughput* suite is bigger-is-better, natural
  `deliveries/s`. On this page, throughput is shown in natural units.
- **Two thresholds, two jobs.** The **hard gate** (`bench/perf_gate.py`, per-PR)
  fails a PR whose p50 exceeds **150 %** of its own same-runner `main` baseline (or
  whose throughput drops below 66 %). The **soft alert** (trend tracker, per `main`
  commit) is tighter — **125 %** commit-to-commit — but only comments; it exists to
  catch slow creep that stays under the hard gate across many commits. An alert is
  a prompt to look at the trend, not a failure.
- **Noise floor.** Each recorded point is the **median of the repeated RESULT
  rows** one run emits, so single-iteration jitter does not move the series — but
  shared CI runners still vary ~2× in absolute speed. Read trends across several
  commits, not the third digit of one point. Reproducing locally: build Release
  (`-O3`), pin the bench to a core (`taskset -c N`), take the best of several runs,
  and compare only numbers measured on the same machine in the same session.
- **`inproc-path` is a resolver canary, not a hot pattern.** The write-by-path
  rows exercise the registry lookup on every write *on purpose*, so a resolver-cost
  regression is visible as its own series. Hot paths resolve the path once and
  write through the held vertex handle (the `inproc` / `inproc-borrow` rows) —
  compare against those, not `inproc-path`, when judging dispatch cost."""


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

## Cross-core conformance (every native core must agree byte-for-byte)

The shared conformance vectors are decoded+re-encoded by every enabled core; a DISAGREE
fails CI (ADR-0028). Live driver summary:

{summary}

## In-process latency & throughput

Canonical points from `bench_libtracer` (the µs-latency / zero-copy thesis, ADR-0031):

{perf_block()}

{HISTORY_BLOCK}

{READING_BLOCK}

## Cross-core codec performance (decode→encode roundtrip, same v1 vectors)

Every native core (cpp-core / ts-core / rust-core) runs the SAME per-vector
decode→encode roundtrip over the shared v1 conformance vectors (ADR-0032 `lang`
axis, #96), so this is a like-for-like codec surface across implementations.
Figures are the **median across all v1 vectors** (one decode + one encode == one
roundtrip); a core whose toolchain is absent in this build degrades to a note.

{codec_block()}

{COMPARE_INTRO}

{zenoh_compare_block()}
"""
    OUT.write_text(page)
    print(f"wrote {OUT.relative_to(REPO)} ({'conformance PASS' if passed else 'conformance check ran'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
