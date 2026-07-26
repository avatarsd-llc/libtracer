# Test & benchmark methodology

> How libtracer's numbers are produced — the measurement surfaces, the metrics,
> and the discipline that turns a benchmark into a *gate*. This page is the
> durable companion to the auto-generated
> [Performance & conformance](performance.md) page: that page carries the live
> measured values; this one explains what they mean and how to read them.
> Nothing here describes a chart — it describes the experiment behind the chart.

libtracer's central claim is a **sub-microsecond, zero-copy dispatch substrate**
that stays byte-exact across three independent native cores. A claim like that is
only worth the harness that keeps it honest, so the harness is treated as a
first-class artifact: every number on the Performance page is **measured on the CI
runner, auto-published on each docs build** ([ADR-0032](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0032-continuous-cross-core-perf-conformance-matrix.md)),
and the ones that matter are **gated** — a regression turns a pull request, or
`main` itself, red.

Two ideas run through everything below:

1. **A value is only comparable within one measurement surface.** The surfaces use
   different harnesses, processes, and units. Nanoseconds from the in-process
   bench are not comparable to nanoseconds from the network bench; bytes from the
   allocator probe are not RSS. Never compare across surfaces.
2. **Absolute numbers are a trend signal; the gate is always a same-runner
   relative comparison.** Shared CI runners vary roughly **2×** in raw speed
   depending on which machine is drawn, so raw chart height is a *direction*, not
   a verdict. Every hard gate compares two builds **measured on the same runner in
   the same pass**, where the machine's speed cancels out.

---

## The measurement surfaces

Every number belongs to exactly one of these. They are deliberately kept separate
so a value is never silently compared against an incomparable one.

| § | surface | what it measures | harness | discipline |
| --- | --- | --- | --- | --- |
| 1 | Cross-core conformance | byte-exactness across cores (not speed) | [`run-all.py`](https://github.com/avatarsd-llc/libtracer/blob/main/tests/conformance/run-all.py) | any DISAGREE fails CI |
| 2 | In-process latency & throughput | single-process dispatch cost (the µs thesis) | [`bench_libtracer`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/bench_libtracer.cpp) | gated per PR **and** per `main` push, same-runner |
| 3 | Memory footprint | heap allocations counted, not timed | [`bench_forward_heap`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/bench_forward_heap.cpp) probes + max RSS | forward hop hard-gated at ZERO allocs |
| 4 | libtracer vs Zenoh | absolute side-by-side, both engines one pass | `bench_libtracer` + [`bench_zenoh`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/bench_zenoh.cpp) (+ loopback net) | same runner, same pass — no ratios |
| 5 | Cross-core codec | decode→encode roundtrip per implementation | cpp / ts / rust codec benches | same v1 vectors for all cores |

### 1 · Cross-core conformance (correctness, not speed)

The three native cores — the C++ **golden reference**, and the from-scratch
TypeScript and Rust reimplementations — must agree **byte-for-byte**. A shared set
of versioned conformance vectors (`tests/conformance/vectors/v1`) is decoded and
re-encoded by every enabled core; the driver diffs the results, and a single
`DISAGREE` fails CI
([ADR-0028](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md)).
This surface measures **truth, not time** — it is what lets the other four surfaces
trust that a fast C++ number describes the same protocol the other cores speak.

### 2 · In-process latency & throughput (the dispatch thesis)

`bench_libtracer` exercises the hot path — resolve a vertex, write a value, notify
and deliver to subscribers — entirely in one process, and reports per-operation
**latency** (p50 / p99 / mean nanoseconds) and **throughput** (deliveries or
publishes per second). It sweeps three axes independently:

- **fan-out** — subscribers per write (dispatch amortization);
- **payload** — value size in bytes (copy cost);
- **topic count** — number of registered vertices (registry / resolver pressure).

Several named *modes* isolate distinct costs on the same axes:

- `inproc` — the full write (store + notify + deliver);
- `inproc-borrow` — the zero-alloc loaned-view path;
- `inproc-deliver` — deliver-only (`propagate`), value stored once;
- `inproc-path` — **write-by-path**, resolving the registry on *every* write. This
  is a deliberate **resolver canary**, not a hot pattern: real code resolves a path
  once and writes through the held handle. Judge dispatch cost against `inproc` /
  `inproc-borrow`, never against `inproc-path`.

This is the surface that carries the microsecond thesis — the zero-copy substrate
([ADR-0016](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md))
delivering values as loaned `view_t`s — and the one the per-PR gate watches most
closely.

### 3 · Memory footprint (allocations counted, not sampled)

A different instrument entirely. `bench_forward_heap` replaces the global allocator
with a counting wrapper and **arms it around exactly one operation**, so these are
*exact* allocation counts and byte totals — not statistics, not sampling. Four
probes:

- **forward hop** — a value forwarded to a remote subscriber. **Hard-gated at zero
  allocations** every CI run: the two-plane forwarding model
  ([ADR-0038](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md))
  requires the steady-state hop to touch no heap, so a single stray `malloc` on the
  forward path fails the build.
- **terminus resolve** — report-only; a terminus *may* allocate
  ([ADR-0041](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0041-terminus-arena-decode-span-contract.md)),
  and the probe keeps that cost visible without gating it.
- **per-vertex steady heap** — the **live usable-size bytes** a default leaf vertex
  holds at rest (measured against `malloc_usable_size`, so it is the real resident
  cost, not the requested size), plus the increment one small last-known-value write
  adds. This is the vertex-diet trend, and the per-vertex figure is now **gated
  same-runner** (a >2% growth fails the build): the count is exact — the allocator
  wrapper is deterministic, not sampled — so a few bytes per vertex is a hard
  regression, not noise, on the constrained target's budget.
- **whole-run max RSS** — the coarse process-level footprint, read from
  `/usr/bin/time -v`.

The per-vertex figure is the one that matters for the constrained targets (the
ESP32 profile lives inside a ~16 KB RAM budget), which is why it is tracked as its
own series rather than folded into RSS.

### 3b · Routing & delivery (the network plane, per frame)

The in-process surfaces above measure the graph. Three benches measure the **network
plane** — what a frame costs between arriving and being applied — because the two move
independently and an improvement to one can hide a regression in the other.

- **`bench_forward_demux`** — one FWD forward hop: resolving the `dst` mount against the
  connection table and scatter-gathering the egress. Swept over registry size, on two axes
  (`fixed` = target first, isolating the size-independent term; `scan` = target last,
  exposing the marginal per-link cost). It never resolves a vertex, so it measures routing
  alone.
- **`bench_terminus_tier`** — the terminus *resolve*, driven through both reader tiers on
  the **same frame**: the eager arena reader and the lazy rope reader, plus a
  flatten-then-arena arm. This is the surface that settled the
  [ADR-0053](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md)
  erratum — the eager arena wins 2.2–2.5× on latency *and* allocations for single-link
  frames at every size, and the flatten crossover sits at roughly 16–64 KB.
- **`bench_compact_delivery`** — the **steady state**: the Nth `COMPACT` frame on a warm
  binding, which is the case delivery compaction exists for and the one the other two miss
  (the demux bench never resolves; the terminus bench measures a *cold* resolve an
  established flow pays once).

All three report latency **and** exact allocation counts, and all three calibrate their own
batch size against the host clock rather than hardcoding one — a routing operation costs the
same order as `clock_gettime`, so per-op timing measures the clock. An early revision did
exactly that and reported a whole-table scan *beating* a first-hit lookup.

`bench_forward_demux` and `bench_compact_delivery` are recorded into the build-to-build
history alongside the in-process series, so a routing or delivery regression shows up as a
trend rather than being noticed later. They emit the same `RESULT` rows as `bench_libtracer`,
so they need no separate aggregation. Their transcripts are tolerated-empty — a bench that
fails to run must not cost a commit its whole history point — but an empty one now emits a
build warning naming the file, because a silently-empty transcript once produced a green job
that recorded nothing at all.

### 4 · libtracer vs Zenoh (absolute, one pass, same runner)

A side-by-side against [Eclipse Zenoh](https://zenoh.io) (zenoh-c, peer mode). Both are
measured **in the same pass on the same runner** — libtracer compiled from source at `-O3`,
Zenoh as the upstream prebuilt `zenoh-c 1.9.0` release binary that `bench/fetch_zenoh.sh`
downloads (we do not build it, so its optimization profile is upstream's, not ours). And
the charts plot **absolute** throughput / latency / bandwidth — both engines as
series on shared axes. There are **no speed-up ratios**: every point is a measured
number you can read off directly. Fairness is discussed in its own section below,
because a naive put-vs-write comparison would be misleading.

### 5 · Cross-core codec (like-for-like across implementations)

Each native core runs the *same* per-vector decode→encode roundtrip over the shared
v1 vectors, reported as the **median across all vectors** (one decode + one encode =
one roundtrip). Because the input is identical for all cores, this is a genuine
like-for-like codec comparison across implementations; a core whose toolchain is
absent in a given build degrades to a note rather than failing the docs build.

---

## The metric taxonomy

Across the surfaces, a measurement is one of the following dimensions. Each has a
distinct instrument, and a distinct rule for *what a "worse" number means*.

| dimension | unit | instrument | direction | status |
| --- | --- | --- | --- | --- |
| **latency** | ns (p50 / p99 / mean) | wall-clock per op, `bench_libtracer` | lower better | gated ✅ per-PR + push |
| **throughput** | deliveries/s, publishes/s | ops / elapsed, `bench_libtracer` | higher better | gated ✅ per-PR + push |
| **alloc bytes** | bytes & count per op | counting allocator, `bench_forward_heap` | lower better | forward hop gated ✅ = 0; other probes tracked |
| **memory footprint** | live bytes / vertex, max RSS | `malloc_usable_size` balance + `/usr/bin/time` | lower better | gated ✅ per-vertex (+2% same-runner); RSS tracked |
| **wire bytes** | encoded frame bytes | TLV frame size over the v1 vectors, codec surface | lower better | being promoted to a first-class series |
| **CPU** | work per op | per-op cost on a pinned core | lower better | latency is today's proxy; dedicated counter planned |

Three notes on reading this table honestly:

- **Latency vs CPU.** Today's per-op cost is measured as **wall-clock latency on a
  quiesced, core-pinned runner**, which on an idle machine is a close proxy for CPU
  work. A dedicated cycles/CPU-time counter is a finer instrument for the same
  dimension; where a chart is labelled "CPU" it means per-op compute isolated from
  I/O and wait.
- **Wire bytes are a codec property, not a dispatch property.** They come from the
  encoded size of a message over the wire (the TLV frame), measured on the same v1
  vectors the codec surface uses — so they are comparable across cores and
  independent of runner speed.
- **Alloc bytes vs footprint.** *Alloc bytes* is the transient heap a single
  operation churns (gated to zero on the forward hop). *Footprint* is the resident
  memory the graph holds at rest (per-vertex live bytes, whole-run RSS). A design can
  be zero-churn yet heavy at rest, or lean at rest yet allocation-happy per op — so
  the two are tracked separately and never summed.

---

## What actually stops a regression

Absolute nanoseconds vary ~2× with the runner drawn, so the gates are all
**same-runner relative** comparisons, where machine speed cancels. Three jobs, three
thresholds, one hard invariant:

| mechanism | when | comparison | threshold | effect |
| --- | --- | --- | --- | --- |
| **per-PR hard gate** ([`perf_gate.py`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/perf_gate.py)) | every PR | PR build vs `main` build, **one runner** | p50 **+15%** · mean **+12%** · deliveries/s **−12%** · per-vertex bytes **+2%** | fails the PR |
| **push ratchet** | every `main` push | HEAD vs its parent, **three independently-drawn runners** | same as above | turns `main` red |
| **forward-hop zero-alloc gate** | every CI run | absolute | `> 0` allocations on the forward hop | fails the build |
| **soft trend alert** | per `main` commit | vs previous point, **cross-runner** | series drifts past **125%** | a comment, *not* a verdict |

Details that make these trustworthy:

- The per-PR gate watches **six canonical points** (a representative slice of the
  fan-out / payload / topic sweeps plus a fold-width point), each taken as the
  **best of three runs** (min p50 / max deliveries) so single-iteration jitter
  cannot manufacture a failure. Because the baseline is *the same PR's `main`
  rebuilt on the same runner in the same pass*, the comparison is machine-neutral.
  The same gate additionally checks **three memory probes** — per-vertex live bytes,
  the increment one LKV write adds, and a leaf carrying a five-field app-field table.
  These come from the counting allocator (`bench_forward_heap`), so they are exact
  rather than sampled: they need no best-of-N and ratchet tightly at **+2%**, with
  the baseline binary's bytes recorded same-runner via `--bench-fwd`.
- The **push ratchet** re-runs that gate on **three separate runner draws** and
  requires the regression to reproduce — one noisy machine cannot fail `main`, and a
  regression that slips through the PR gate still gets caught the moment it lands.
- The **forward-hop zero-alloc gate** is the one **absolute** gate: it is a
  structural invariant, not a speed target. Steady-state forwarding must allocate
  nothing, so the threshold is literally zero.
- The **soft alert** compares *across* runners, so it is only a prompt to look at
  the trend, never a merge-blocker.

The baseline the per-PR gate compares against (`bench/perf_baseline.json`) is
**host-specific and regenerated on every CI run** — it is never committed as a
fixed number, precisely so the gate can never encode one machine's speed as another
machine's target.

---

## Fairness in the Zenoh comparison

An honest side-by-side has to account for the two engines doing *different amounts
of work per operation*.

- **Write does strictly more than put.** libtracer's `write` row also **persists**
  the value (it becomes the vertex's last-known-value) and bumps the `await` /
  readiness sequence on every op. Zenoh's `put` is transient delivery only. So the
  libtracer *write* row is charted against a Zenoh row that does **less** semantic
  work — the `inproc-deliver` (`propagate`) series is the apples-to-apples
  counterpart: value stored once, each op only delivers, matching put semantics. Both
  libtracer series are shown so the reader sees the full-work and the like-for-like
  number side by side.
- **ACL is disabled in the comparison rows.** No subject resolver is installed, so
  the access gate is a single null check. The *cost of enforcement* is measured
  separately (the `acl-inherit` rows), never hidden inside the comparison.
- **There is no network throughput comparison, and the one that existed was
  withdrawn rather than restyled.** It charted effective values/s against composition
  size K, on the argument that libtracer ships a K-link rope as one datagram while
  Zenoh's timer-batched put rate is K-independent and plots as a flat reference. The
  argument was fine; the measurement was not. Its Zenoh side declared a publisher with
  **no subscriber and no peer**, so `put()` never reached the wire — measured under
  `strace`, **5 `sendto` calls for 520 000 puts**, and all five were multicast scouting
  beacons. The libtracer side measured a real `sendmsg` rate but published `rate × K`,
  egress-only, with no receiver counting deliveries. Both K-curves were therefore
  arithmetic rather than measured, only one engine performed any I/O, and the page
  described the scenario as "loopback UDP · two processes" when it was a single
  process. A valid version needs a real subscriber in a second process on **both**
  sides with delivery counted at the receiver — that is a new benchmark, not a fix, and
  it is tracked separately rather than left on the page as a placeholder.
- **Network latency is the surviving network comparison**, and it is fair: a
  single-value, two-process, same-clock measurement over the real loopback kernel path,
  identical topology for both engines. Both **p50** and the **p99 tail** are charted per
  transport — for a latency-first substrate the tail is the load-bearing number, and it
  is where transports separate, since an unreliable datagram path can win the median and
  still spike at p99.

A transport that is **not** charted always says so in the transport-coverage note under
the charts, including when neither engine produced rows for it. WebSocket and QUIC are
the standing cases: the WebSocket transport shows order-of-magnitude single-run p50
jitter under this bench that would make a published chart misleading, and QUIC needs the
optional TLS module. An absent transport must never be readable as a tie.

---

## Reading the numbers (noise & variance)

- **Runner lottery.** Shared CI runners vary ~2× in absolute speed. **The tell:** a
  move that hits *every* series at once — including unrelated ones like the pure-codec
  `fold-b*` rows — is the runner; a move confined to one family is the code. Read
  trends across several commits, not the third digit of one point.
- **Per-point noise floor.** Each recorded point is the **median of the repeated
  RESULT rows** one run emits, so per-iteration jitter does not move a series. Points
  are then recorded as the **best across three runner draws**, approximating the
  code's capability rather than the machine lottery. Sub-microsecond points sit on a
  ~10 ns timer grain — do not over-read a 5 ns wiggle.
- **Sign conventions in the history store.** The latency suite is
  smaller-is-better nanoseconds; throughput also appears there **inverted** as
  `ns/delivery` so a slowdown always charts as a *rise*; memory metrics live in that
  same smaller-is-better suite. The throughput suite is bigger-is-better natural
  `deliveries/s`. The same measurement can therefore appear twice, in two units — by
  design, so each suite reads monotonically.
- **A discontinuity in the instrument is annotated; a discontinuity in the code is
  not.** When a commit changes a *bench source* — `bench_libtracer.cpp`,
  `bench_forward_demux.cpp`, the shared harness, the emitter — every series that bench
  feeds is marked at that commit, because points either side of it were taken with
  different rulers and are not comparable. Changes under `core/**` are deliberately
  **never** marked: a core change moving the line is the signal the series exists to
  show, and marking both would reduce the annotation to "something happened," which is
  worth nothing. Comment- and whitespace-only bench edits are filtered out for the same
  reason — an alert that fires on nothing trains readers to ignore it.
- **Renaming beats reinterpreting.** If a fix changes *what a row measures*, the mode
  string changes too: the old series ends visibly and a new one starts at one point.
  What is never done is keeping the name while changing the meaning, which produces a
  continuous-looking line that silently stops being about the same thing. If a fix only
  makes the *same* quantity more accurately measured, the name is kept and the
  instrument marker carries the discontinuity.

---

## Reproducing locally

The gates are same-runner by construction, so a local reading only means something
against another local reading taken the same way:

```sh
# Build the bench Release (-O3) — same flags CI uses.
cmake -S bench -B bench/build -DCMAKE_BUILD_TYPE=Release
cmake --build bench/build -j

# Pin to a core, take the best of several runs, compare only same-machine numbers.
taskset -c 2 ./bench/build/bench_libtracer          # the sweep matrix
taskset -c 2 ./bench/build/bench_forward_heap        # the allocation probes (zero-alloc gate)

# The network plane (§3b). Each takes a per-point wall-clock budget via
# LIBTRACER_BENCH_SECONDS; longer means tighter percentiles, never a different measurement.
taskset -c 2 ./bench/build/bench_forward_demux       # forward hop vs registry size
taskset -c 2 ./bench/build/bench_terminus_tier       # terminus: eager arena vs lazy rope reader
taskset -c 2 ./bench/build/bench_compact_delivery    # steady-state warm compacted delivery

# The comparison surface needs Zenoh vendored first:
bench/fetch_zenoh.sh && cmake --build bench/build -j
```

Compare a change against its own baseline **on the same machine in the same
session** (`git stash`, rebuild, re-run) — never against a number from a different
host or a different day.

---

## Provenance & auditability

Because the Performance page is regenerated on every docs build, each render carries
a CI stamp — date, commit, run, and runner OS — so any published figure is auditable
back to the exact deploy that produced it. Every `main` push additionally archives
**all raw benchmark transcripts** as a per-commit CI artifact and records every
`(mode, size, fan-out, endpoints)` point — latency, throughput, and memory — into a
persisted build-to-build history on the machine-maintained `gh-pages` branch. The
numbers on the Performance page are one run; that history is the durable signal, and
it is what the trend charts and the soft alert read from.
