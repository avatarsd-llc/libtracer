#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Generate docs/performance.md from LIVE harness output — so the published page is
auto-generated test+perf results, not a hand-maintained snapshot (ADR-0032 §auto-published).

Runs the cross-core conformance driver (run-all.py), the in-process perf bench
(bench_libtracer), the cross-core codec benches (cpp-core bench_codec + ts-core
perf.mjs + rust-core `cargo run --example perf`), and the coverage audit, and
renders a MyST page: the cross-match matrix, the per-family trend charts (one chart
idiom throughout, distributed into the chapter each family belongs to), the
cross-core codec comparison, and the zenoh comparison. CI regenerates this in-place before
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



def _missing(binary: str, hint: str) -> str:
    """A bench binary is absent: degrade locally, FAIL the published build.

    A placeholder is fine in a local preview. On the live site it is not: the prose around
    these blocks asserts measured figures ("87 ns", "-36%"), so a reader meets an assertion
    with a "not built" note where its evidence should be and reasonably reads the assertion
    as measured. That is exactly what §2b did — `bench_forward_demux` was never in the docs
    workflow's build target list, so the published page carried a placeholder under live
    numbers. `LIBTRACER_DOCS_STRICT=1` (set only in docs.yml) turns that into a build
    failure, so the page can never again publish prose whose evidence is missing.
    """
    if os.environ.get("LIBTRACER_DOCS_STRICT") == "1":
        sys.exit(f"gen_results_page: {binary} not built, and LIBTRACER_DOCS_STRICT=1. "
                 f"Build it ({hint}) or drop the section that cites it.")
    return f"_({binary} not built — `{hint}`)_"


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


DEMUX_BLOCK = """\
What one **FWD forward hop** costs in the router — before any payload is touched, measured
by `bench/bench_forward_demux.cpp`.

A hop performs **two** linear scans of the connection registry, and the bench is arranged so
both are visible:

- `by_segments` resolves the `dst` mount (`net/<module>/<name>`, ADR-0061);
- `entry_by_name` fetches the **inbound** child's precomputed `src` prefix.

`fixed` places the target child first, so its lookup hits immediately and that chart isolates the
size-independent part of a hop. `scan` places it last, so the lookup walks the whole table — the
rise of the `scan` chart over the `fixed` one, at the same registry size, is the scan's marginal
cost. Read them as a pair; neither is meaningful alone."""

DEMUX_NOTES_BLOCK = """\
### Two corrections this bench exists to prevent

**Timing must be batch-amortized.** A hop costs the same order as `clock_gettime`, so timing
each hop individually measures the clock. An early draft did exactly that and reported the
whole-table scan *beating* the first-hit lookup — the signature of a clock-dominated window.
The bench calibrates its batch size against the host's own clock and prints what it chose,
rather than hardcoding a number tuned on one machine.

**Both scans must be visible.** An earlier revision registered the inbound child *first*, so
`entry_by_name` always hit at position 1 and was invisible: the bench reported one scan's cost
and called it the hop. Registering it *last* was supposed to fix that — but the bench then
registered the inbound link **twice**, once at each end, so the lookup matched the first slot
at position 1 regardless and the fix never took effect. (That duplicate was also a real
registry bug: a shadow slot that survived `erase`.) Both are gone; the numbers above are the
first that actually include the second scan.

**The timed path must be the production path.** The bench used to call `on_frame` by name — a
ctx-less entry only tests and SDK hosts take. A real transport delivers through the receiver
`add_child` installs, which is bound to a stable per-child ctx. The hop is now driven through
the inbound link's receiver, so what is timed is what ships.

The two scans are not interchangeable: `entry_by_name` compares whole strings (a length check
rejects most candidates), while `by_segments` compares a qualified key piecewise — which is
why the `dst` scan dominates, at roughly 7:1 measured.

### Where the hop stops, and why

After the duplicate header parse was removed the hop sits at ~86 ns, and **~85% of that is still
TLV header parsing** — but it is now nine *distinct* header reads, none of them a duplicate: the
FWD header, the op VALUE, the dst PATH and four `dst` segments in the peek, plus the selector peek
and the src PATH in the rebuild. Each header is read exactly once.

Two source-level levers were implemented and measured, and **both regress**, so neither was taken:

| variant | p50 |
| --- | ---: |
| as shipped | **86–87 ns** |
| `read_fwd_header` forced `always_inline` | 95–96 ns (+10%) |
| narrowed header struct (48 → 40 bytes) | 95–96 ns (+10%) |

The reasoning that motivated them was sound and the measurement still refuted it. `read_fwd_header`
parses into a seven-field grammar struct through a `std::expected`, then repacks into a six-field
one through a `std::optional`, for a path that reads four fields — which looks like obvious waste.
Disassembly says otherwise: at `-O3` the parse is fully inlined with values flowing in registers
straight into the narrow struct's stores. There is no intermediate object and no copy, so the
repack costs **zero instructions** and removing it at source level can only make things worse.
Forcing the inline is worse still — nine copies of a ~450-byte parser cost more in instruction
cache than the single well-predicted out-of-line call it replaces.

The gather is not a lever either: it measures ~1.6 ns, not the ~11 ns an earlier profile
attributed to it. It fills a stack `std::array` of spans and allocates nothing, which the
`allocs=0` gate pins.

So the forward hop is at a local optimum for its current structure. Recording that here is
deliberate — "the compiler already did it" is not visible from the source, and the two obvious
edits both look like clear wins right up until they are measured.
### What the shape means

The size-independent term is flat across N, as the design predicted. The term that grows is
the **scan**.

**Per-module scoping does not narrow it.** ADR-0061 and an earlier revision of this page both
claimed that keying the registry per module turns a whole-table walk into a walk of one
module's members. That was an inference, and measurement refuted it: scoping changed the
*key*, not the *container*. Building the implied module-bucketed index moved N=64 from 363 ns
to 390 ns — no win. A node's links overwhelmingly sit in **one** module (a device has many
`ws-server` peers, not many modules), so bucketing relocates the scan instead of shortening
it. The per-module key earns its place by keeping two modules' same-named connections
distinct — a **correctness** property. It buys no lookup time.

**One of the two scans is gone.** A hop no longer looks the inbound child up at all: its mount
run is carried on the link's own receiver ctx, created once in `add_child`. Measured against
the corrected bench, that is **-36 ns at N=64 (-9%)**, and the saving grows linearly with the
link count — 128 -> 117 at N=8, 258 -> 229 at N=32 — exactly the shape of deleting one of two
linear scans. A copy on the ctx rather than a pointer into the registry slot is deliberate:
slot addresses are not stable across a connection-create (#521), whereas the ctx deque never
invalidates a reference.

**Is the remaining scan a per-frame or a first-frame cost?** It depends on the path, and
ADR-0062 changed the answer for one of them:

| Path | Registry work per frame | |
| --- | --- | --- |
| Plain `FWD` write | mount descent + inbound `entry_by_name` | **per-frame** — the frame carries the full path, so there is nothing to cache against |
| `COMPACT` on a bound label | one dereference of the cached registry slot | **first-frame** — resolved once, then memoized |
| Remote delivery to a subscriber | `by_name(sub.link)` | **per-frame** — the subscriber record still holds a name |

Compaction (RFC-0004 §E.1) shrinks the *wire*; ADR-0062 shrinks the *resolution*. A binding
now holds the resolved target rather than a name, and both cached forms self-invalidate
without a callback: the terminus compares a **retirement generation**, and the forwarding hop
reads the registry **slot**, whose `link` teardown nulls in place — so a departed link reads
`nullptr`, the same clean miss an unresolved lookup gives. The tombstone *is* the
invalidation, which is why this needed neither a second generation concept nor a teardown
sweep. (It also required slot addresses to be stable, which is what ADR-0063's chunked list
provides; the container decision had to land first.)

Two changes landed on this path, and the second was the larger. ADR-0062 removed the
*resolution*; then the span control arm stopped building an owning `tlv_t` at all, reading
`COMPACT` by offset through `peek_control` exactly as the rope arm already did. Measured on the
warm path (`bench_compact_delivery`, host p50):

| | before ADR-0062 | after ADR-0062 | after the offset read | after exact-reserve egress | after gathered egress |
| --- | ---: | ---: | ---: | ---: | ---: |
| terminus delivery | 298 ns | 202 ns | **~110 ns** | — | — |
| terminus allocations | 15 | 11 | **3** | — | — |
| forwarding hop | 202 ns | 180 ns | ~90 ns | ~70 ns | **~45 ns** |
| forwarding allocations | 18 | 17 | 9 | 4 | **0** |

End to end that is **−63% on the terminus** and **−78% on the forwarding hop**, with per-frame
allocations down 15 → 3 and 18 → **0**.

The last column is the one that ends the sequence: the forwarding hop no longer *builds* a frame
at all. A `COMPACT` is a 6-byte frame header, a 6-byte label child, and a payload that is already
contiguous in the inbound frame — so the head is written to a 12-byte stack buffer and the payload
is handed to the transport **by reference**, as a two-element scatter-gather list. The two
allocations and two payload copies that produced bytes the transport was about to gather anyway
are simply gone.

That shows up as more than the average suggests, because the cost it removes **scaled with the
payload** while the rest of the hop did not. Same-machine A/B against `main`, three runs each:

| payload | before | after |
| ---: | ---: | ---: |
| 4 B | 70–75 ns | 44–47 ns |
| 64 B | 70–74 ns | 43–47 ns |
| 512 B | 76–77 ns | 43–47 ns |

The *flatness* is the result worth reading: the old hop grew with payload size because it copied
the payload twice, and the gathered hop does not grow at all. The terminus leg is untouched by
this change (~106–110 ns before and after) — it writes into the graph rather than re-emitting.

Zero allocations *in the router* is not zero in the transport. A link that overrides the gather
form (tcp, udp, ws-server) writes these spans straight to the socket; one that does not (can,
loopback, ws-client) falls into the default concatenation in `transport_t::send(iov)`, which
allocates once. That is still strictly better than the two allocations and two copies it
replaces, so no transport regresses — but the honest claim is "zero in the router", not "zero on
the wire".

### The parse nobody was measuring

`path_t::parse` turns an address into the canonical PATH-TLV payload, and **every** path-keyed
read, write and subscribe calls it. Nothing in this suite measured it: `inproc-path` parses its
addresses once into a vector and reuses the `path_t`s, so it times the registry lookup on an
already-parsed key.

That mattered, because the parse built its payload by geometric doubling — a two-segment address
walked a 1→2→4→8→16 realloc chain, four throwaway blocks and four frees to produce fifteen bytes.
Pre-sizing it exactly (the separator count *is* the segment count, so the total is
`4*segments + address bytes` with no estimate) removes them:

| segments | before | after |
| ---: | ---: | ---: |
| 1 | 34 ns | **12 ns** |
| 2 | 49 ns | **20 ns** |
| 4 | 75 ns | **29 ns** |
| 8 | 80 ns | **38 ns** |

Per-vertex registration drops from **7 allocations to 3**, and a by-path write's window from 6 to
2 — visible in the `heap allocs per … (probe)` series above. Resident bytes are unchanged, because
this was transient churn, not residency.

Worth stating plainly: these are **host** figures, where glibc's tcache serves a hot same-size
malloc/free in tens of nanoseconds. On an MCU allocator a round-trip is hundreds, so the same four
saved round-trips are worth proportionally more on the target than this table shows.

This lever was found by *refuting* the framing of the issue that asked for a vertex arena. That
issue assumed a registration made 5-7 resident sub-allocations dominated by allocator headers; a
bare leaf actually leaves **one** resident block, and the ESP-IDF header is 4 bytes. The real cost
was never in the vertex at all — it was in the parse every caller runs first.

The owning decode cost three allocations for the tree spine plus five more re-encoding a payload
that was **already contiguous in the frame** — together more than half a warm terminus frame. It
had been justified as flow-setup cost (ADR-0041 §5), a classification ADR-0062 invalidated by
making a warm `COMPACT` the steady-state per-sample data frame rather than setup.

`crc_check_t::VERIFY` is passed explicitly on that path: the decode being replaced verified every
node's CRC as a side effect, so reading by offset without asking for it would silently start
accepting a frame whose own trailer says it is corrupt. That is pinned by a test which fails if
the argument is dropped.

The last column swaps the forwarding leg's egress to the **nothrow exact-reserve** encoder, which
also removes an abort path: the growth-doubling encoder grows `std::vector`s, and under
`-fno-exceptions` an exhausted heap aborts rather than shedding the frame (#477). One caveat when
reading its bytes column: the counter sums *requested* bytes, so an exact reservation totals
higher than a doubling ladder at large payloads while peak footprint and fragmentation are
strictly better — judge that change on allocation count and latency, not on bytes.

The forward hop remains **zero-heap** regardless (`bench_forward_heap`, `allocs=0`, CI-gated)."""


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


HOW_TO_READ = """\
## How to read this page

Every number below belongs to exactly ONE measurement surface. Surfaces use different
harnesses, processes and units — **a value is only comparable to values from the same
surface**, never across surfaces.

**A surface is a property of a series, not of a chapter.** The chapters below group by
*subject* — everything about allocation in one place, everything about routing in
another — because that is how a reader arrives with a question. A subject can be
answered by more than one instrument, so §3 and §4 each carry two, and the column
saying which is the one that matters when comparing two numbers.

| surface | what it measures | harness | where it appears | discipline |
| --- | --- | --- | --- | --- |
| Cross-core conformance | byte-exactness across cores (not speed) | `run-all.py` | §1 | any DISAGREE fails CI |
| In-process timing | per-op latency and throughput in one process | `bench_libtracer` | §2, and the timed charts in §3 (fold, `path_t::parse`) and §4 (LKV, allocator-under-contention) | gated per PR **and** per `main` push, same-runner |
| Framed-hop timing | what one wire frame costs before any payload is touched | `bench_forward_demux` + `bench_compact_delivery` | §3 | batch-amortized, self-calibrating |
| Allocation counting | exact allocs and bytes around ONE operation — counted, never timed | `bench_forward_heap` probes + max RSS | §4 | forward hop hard-gated at ZERO allocs |
| Engine comparison | absolute side-by-side, both engines in one pass | `bench_libtracer` + `bench_zenoh` (+ loopback net) | §5 | same runner, same pass — no ratios |
| Cross-core codec | decode→encode roundtrip per implementation | cpp / ts / rust codec benches | §6 | same v1 vectors for all cores |

The two that share a chapter are the pair most easily confused: **§4's counted bytes and
its timed nanoseconds answer different questions** — how much an object holds, versus
what it costs to get and give back. Neither number bounds the other.

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

### Every chart on this page is the same chart

There is one chart type here and no second one. Each is a **family** — a set of series
answering one question, drawn as lines on shared axes. Most vary a single parameter
(fan-out, payload size, thread count, fold width, registry size, engine) and those get
the extra views described below; a few are deliberate matrices, such as the LKV chart
(operation × backend × payload) and the allocator-under-contention chart (allocator ×
thread count), where the comparison IS the cross-product. The x-axis is recorded `main`
commits, oldest to newest, except in §5 where it is the swept parameter. Learn to read
one and you have read all of them.

- **Two kinds of vertical marker, and they mean different things.** 🏷 *dashed* = a release
  tag (**≈** when the tag's own commit is not a recorded point, so the marker sits at the
  nearest following one). 🔧 *dotted* = the **benchmark itself** changed at that commit.
  A release marker says the code moved; an instrument marker says the ruler moved, so
  points on either side of one are **not comparable**. Core changes are deliberately not
  marked — a core change moving the line is the signal the chart exists to show.
- **Colors are global within the history charts.** A series label is assigned one color
  once, so "fan 8" is the same color on the latency chart and on the throughput chart
  beside it, in any chapter. The §5 comparison charts are the exception and use a fixed
  three-color engine palette instead: their line dimension is which engine, not which
  parameter, so borrowing a parameter's color would suggest a relationship that is not
  there.
- **Families with a numeric parameter carry four views.** *trend* (value vs commit, one
  line per parameter), *sweep* (value vs parameter, one line per commit, recency-faded),
  *heatmap* (commit × parameter, color = value) and an isometric *3D* surface — the same
  three-axis data, switchable per chart. The sweep and heatmap views are drawn only over
  the commit sub-grid where **every** series of the family has a value, so a series that
  started late cannot fake a trend.
- **Hover any chart for exact values.** On a history chart the tooltip also names the
  commit and its subject line; on a §5 comparison chart it names the swept parameter
  value. Every point is readable this way, including the interior ones that carry no
  printed label.

### Reading the numbers


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
  including unrelated ones like the pure-codec `fold-b*` rows — is the runner; a
  move confined to one family is the code.** Read trends across several commits,
  not the third digit of one point. Reproducing locally: build Release
  (`-O3`), pin the bench to a core (`taskset -c N`), take the best of several runs,
  and compare only numbers measured on the same machine in the same session.
- **`inproc-path` is a resolver canary, not a hot pattern.** The write-by-path
  rows exercise the registry lookup on every write *on purpose*, so a resolver-cost
  regression is visible as its own series. Hot paths resolve the path once and
  write through the held vertex handle (the `inproc` / `inproc-borrow` rows) —
  compare against those, not `inproc-path`, when judging dispatch cost.

### Why "throughput" means different numbers in different chapters

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

The per-commit history store adds one more deliberate duplication: throughput is
recorded **twice** — natural `deliveries/s` in the bigger-is-better suite *and* inverted
`ns/delivery` in the smaller-is-better latency suite (so a slowdown always charts as a
rise there). Same measurement, two units.
"""


COMPARE_INTRO = """\
## 5 · libtracer vs Zenoh — measured, absolute

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

There is **no network throughput comparison on this page**, and that is deliberate. One
existed and was removed rather than restyled: its Zenoh side declared a publisher with no
subscriber and no peer, so `put()` never reached the wire — measured, **5 `sendto` calls for
520 000 puts**, and those five were multicast scouting beacons. It then reported one
K-independent put rate for every K. The libtracer side measured a real `sendmsg` rate but
published `rate × K`, egress-only with no receiver counting deliveries. Both K-curves were
arithmetic rather than measured, only one engine performed any I/O, and this page described
the scenario as "loopback UDP · two processes" when it was a single process. A valid version
needs a real subscriber in a second process on both sides with delivery counted at the
receiver — a new benchmark, not a fix, and it is tracked separately rather than left as a
placeholder. Network **latency** is the separate per-transport (**UDP** / **TCP**),
single-value, two-process measurement — the same two-process topology (one socket, one
paced value) for both engines, so it is fair. Each engine runs its own minimal transport
path (libtracer's framed `transport_t` send/receive vs Zenoh's session `put`/subscriber),
so this isolates **transport-substrate** latency, not a full graph write. Both **p50** and
the **p99 tail** are charted per transport: for a latency-first, RDMA-style substrate the
*tail* is the load-bearing number (jitter, not the median, is what a real-time consumer
feels), so it earns its own axis. The tail is also where the transports separate — an
unreliable datagram path can win the median yet spike at p99, which the p50 chart alone
would hide.

WebSocket and QUIC are not charted here, and that gap is stated in the **Transport
coverage** note under the network charts rather than left silent: libtracer's WebSocket
transport shows large single-run latency spikes under this bench (order-of-magnitude p50
jitter) and Zenoh has no WebSocket transport to compare against, while QUIC needs the
optional `-DLIBTRACER_WITH_QUIC` module (msquic + TLS). Full harness in
[`bench/`](https://github.com/avatarsd-llc/libtracer/tree/main/bench)."""


RAW_DATA_BLOCK = """\
## 8 · Raw data & provenance

The charts above are one view of a persisted store; this is where the store comes from
and how to get at it directly.

Every push to `main` runs the full bench on **three independently-drawn runners**, archives
all raw transcripts as a per-commit CI artifact (`bench-results-<sha>`, on the `perf`
workflow run), and records **every** `(mode, size, fanout, endpoints)` point — latency
(p50/p99 ns), throughput (deliveries/s), and memory footprint (heap-probe bytes per hop,
whole-run max RSS) as **separate series** — to a build-to-build history on the
machine-maintained `gh-pages` branch
([benchmark-action/github-action-benchmark](https://github.com/benchmark-action/github-action-benchmark)).
Per metric the recorded value is the **best across the three runners** (min latency / max
throughput): machine speed varies ~2× between runner draws, so best-of-3 approximates the
code\'s capability rather than the machine lottery. A commit that drifts a series past
**125 %** of the previous point gets an automatic soft-alert comment; the hard per-PR gate
stays in `bench/perf_gate.py`.

The store carries roughly three times as many series as this page charts — every recorded
point, including the ones no family groups. **[Open the raw per-series trend
browser ↗](https://libtracer.avatarsd.com/dev/bench/)** — one chart per series, zoomable,
with per-point commit links. It is the archive; the families above are the reading."""


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


def history_sections(data: dict | None) -> tuple[dict[str, str], str]:
    """@brief The family trend charts, split into the page's chapters.

    Every chart on this page is the same kind of object: one series-family, all
    its series as lines on shared axes, release + instrument markers, and — for
    numeric-parameter families — the sweep / heatmap / 3D views. There is
    deliberately no second chart style anywhere; a reader learns the idiom once.

    Returns (section -> raw-html block, reason-this-is-empty). The reason matters:
    three different things produce no blocks and they are NOT interchangeable —
    the store was unreachable, the store was reachable but unparseable, or the
    store was read fine and simply carries no family with two or more series yet.
    Reporting all three as "unreachable" publishes a false cause, and does it on
    a page that may be showing live charts from that same store two chapters up.
    """
    if not data:
        return {}, ("the per-commit history store was not reachable in this build")
    try:
        blocks = render_history.html_blocks(data)
    except Exception as e:  # a malformed store must never break the docs build
        return {}, f"the history store could not be rendered in this build — {e}"
    return blocks, "the history store carries no chartable family for this chapter yet"


def charts_for(sections: tuple[dict[str, str], str], name: str) -> str:
    """@brief One chapter's chart block, or a note naming why it is absent."""
    blocks, reason = sections
    if name in blocks:
        return blocks[name]
    return (f"_({reason} — the [raw per-series browser](#8-raw-data-provenance)"
            " serves the full store once published)_")


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
    """Run both grids and render the absolute-value comparison charts.

    Two absences are treated differently on purpose:

    - **Zenoh missing** is soft. `docs.yml` builds `bench_zenoh` best-effort and says so
      in its own comment: a failed vendor must never break a deploy. The chapter degrades
      to a note and libtracer's own numbers stand.
    - **libtracer's own rows missing** is hard under `LIBTRACER_DOCS_STRICT`. That means
      the sweep did not run at all, and the surrounding prose describes charts that are
      not there. CI always creates the TSV file (`|| true` in the workflow), so an EMPTY
      file is the shape this failure actually takes — checking that the path exists is
      not enough to catch it, which is why the check is on the parsed rows.
    """
    tsv = os.environ.get("LIBTRACER_COMPARE_TSV")
    combined = ""
    if tsv and pathlib.Path(tsv).exists():
        combined = pathlib.Path(tsv).read_text()  # CI runs the sweep once, shared with the PR comment
    if not combined.strip() and BENCH.exists():
        combined = run([str(BENCH), "grid"])
        if BENCH_ZENOH.exists():
            combined += "\n" + run([str(BENCH_ZENOH), "grid"])
    rows = render_compare.parse(combined)
    if not any(r["sys"] == "libtracer" for r in rows):
        return _missing("bench_libtracer (comparison sweep produced no rows)",
                        "cmake --build bench/build --target bench_libtracer")
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


def check_heading_depth(page: str) -> None:
    """@brief Fail the build on a heading hierarchy that skips a level.

    This is what shipped to the live page: a `##` section whose subsections
    were emitted at `####`, so the reader saw chapters belonging to nothing.
    It went unnoticed because each block owns its own heading level and no
    single file sees the assembled sequence — which is exactly why the
    assembled page is where the rule is enforced.

    Descending back up a level is legal and not flagged; only a downward jump
    of more than one is structurally wrong.

    Depth also has a hard ceiling here: `docs/conf.py` sets
    `myst_heading_anchors = 3`, so an `h4` gets **no anchor** and cannot be
    deep-linked. Nothing that a reader is meant to link to may sit below `###`;
    the tables under "Full history" are the only permitted `h4`, being an
    index rather than a destination.

    Fenced code blocks are skipped so a `#` comment inside one is never read as
    a heading.
    """
    depths: list[tuple[int, str]] = []
    fenced = False
    for line in page.splitlines():
        if line.startswith("```") or line.startswith(":::"):
            fenced = not fenced
            continue
        if fenced or not line.startswith("#"):
            continue
        level = len(line) - len(line.lstrip("#"))
        if level and line[level:level + 1] == " ":
            depths.append((level, line.strip()))
    prev = 0
    for level, text in depths:
        if prev and level > prev + 1:
            raise SystemExit(f"heading depth skips a level ({prev} -> {level}): {text}")
        prev = level


def main() -> int:
    summary, passed = cross_core_block()
    history = _load_history()
    charts = history_sections(history)
    assets = render_history.assets_block()
    page = f"""\
# Performance & Conformance

```{{note}}
This page is **auto-generated** from the live test + benchmark harnesses on each docs
build (`bench/gen_results_page.py`, ADR-0032). It is the published response surface,
not a hand-edited snapshot. All rates and latencies are **absolute measured values**,
representative of the CI runner (shared-runner variance is real — read trends, not the
third digit); the libtracer-vs-Zenoh charts plot both engines on the same axes. How
these numbers are produced — the measurement surfaces, the metrics, and the gating
discipline — is documented on the [Test & benchmark methodology](methodology.md) page.

{provenance()}
```

{HOW_TO_READ}

## 1 · Cross-core conformance (every native core must agree byte-for-byte)

The shared conformance vectors are decoded+re-encoded by every enabled core; a DISAGREE
fails CI (ADR-0028). Live driver summary:

{summary}

## 2 · Dispatch — in-process latency & throughput

The µs-latency / zero-copy thesis (ADR-0031), measured by `bench_libtracer`: what one
write costs when publisher and subscriber are in the same process, swept over fan-out,
payload size, topic count, thread count, endpoint type and dispatch path.

Four dispatch paths recur across these charts. **`inproc`** is a full write — store the
value, bump the readiness sequence, deliver. **`inproc-deliver`** stores once and then
only delivers, which is the apples-to-apples counterpart to a Zenoh `put` (§5).
**`inproc-borrow`** is the loaned path: the subscriber sees a borrowed view and nothing
is copied. **`inproc-path`** resolves the address on every write — a resolver canary,
not a hot pattern.

{charts_for(charts, "dispatch")}

## 3 · Wire & routing — what a framed hop costs

{DEMUX_BLOCK}

{charts_for(charts, "routing")}

{DEMUX_NOTES_BLOCK}

## 4 · Memory & allocation

A different instrument entirely: `bench_forward_heap` replaces the global allocator
with a counting wrapper and arms it around exactly one operation — so its probes are
exact allocation counts and bytes, not statistics. Six probes feed the store:

- **forward hop** — hard-gated at **zero** allocations every CI run (ADR-0038 §16KB-RAM);
- **terminus resolve** — report-only; a terminus may allocate (ADR-0039), the probe
  keeps the cost visible;
- **per-vertex steady heap** — LIVE usable-size bytes a default leaf holds, and the
  increment one small LKV write adds (the vertex-diet trend, #361);
- **per-vertex app-field table** — what an RFC-0010 five-field descriptor table adds
  on top of that leaf, measured for BOTH installs (ADR-0058): the owning
  `set_app_fields`, which copies the declaration into the table\'s backing, and the
  borrowed `set_app_fields_static`, whose slots view caller flash. The pair is what
  decides whether per-endpoint schemas beat the `/meta` child-vertex workaround on an
  MCU (#388), so both are gated rather than argued. Gating them is what showed the
  borrowed path was **not** the promised zero-declaration-RAM install — it still copied
  the caller\'s array into a vector, the largest single block on that path — which is now
  fixed (592 → 392 B per vertex, ADR-0058 erratum 1);
- **wide fan-out publish** — the per-write cost at large subscriber counts;
- **whole-run max RSS** — the coarse process-level footprint.

The timed rows in this chapter are a separate question from the probes: what the
*allocator* costs, pooled against the default heap, on one thread and under contention.

{charts_for(charts, "memory")}

```{{note}}
**Allocation churn is not resident footprint, and this page charts both — separately.**
"Resident bytes per vertex" is what a live object *holds*; "Heap & memory footprint" is
the transient churn of one forward or terminus operation; the timed charts are what it
costs to get a block and give it back. A release cycle can cut per-frame allocations from
14 to 0 — as this one did — and move resident bytes almost not at all, because churn buys
latency and fights fragmentation rather than shrinking the idle heap. Shrinking that is a
different lever: retiring mechanisms, not tuning the core. Read the resident chart before
concluding a release shrank anything.

Churn also matters more on the target than these host numbers suggest. glibc\'s tcache
serves a hot same-size malloc/free in tens of nanoseconds; an MCU allocator takes
hundreds. A host measurement of churn is therefore a *lower* bound on what it costs
where libtracer actually ships.
```

{COMPARE_INTRO}

{zenoh_compare_block()}

## 6 · Cross-core codec performance (decode→encode roundtrip, same v1 vectors)

Every native core (cpp-core / ts-core / rust-core) runs the SAME per-vector
decode→encode roundtrip over the shared v1 conformance vectors (ADR-0032 `lang`
axis, #96), so this is a like-for-like codec surface across implementations.
Figures are the **median across all v1 vectors** (one decode + one encode == one
roundtrip); a core whose toolchain is absent in this build degrades to a note.

{codec_block()}

## 7 · Test rollup (live ctest, unified with the perf surface)

{tests_block()}

{RAW_DATA_BLOCK}

{assets}
"""
    check_heading_depth(page)
    OUT.write_text(page)
    print(f"wrote {OUT.relative_to(REPO)} ({'conformance PASS' if passed else 'conformance check ran'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
