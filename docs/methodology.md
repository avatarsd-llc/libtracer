# Test & benchmark methodology

> **This file is not a published page.** Its sections are spliced into the
> generated [Performance & conformance](performance.md) page by
> `bench/gen_results_page.py`, each next to the results it explains — the method
> for a surface belongs beside that surface's numbers, not one click away. It
> lives here as ordinary Markdown so it stays editable and reviewable as a diff
> rather than as a string inside a generator.
>
> Two consequences for editing it. **Section headings are the splice keys**: the
> generator looks them up by name and *fails the docs build* if one goes missing,
> so rename a heading only together with `bench/gen_results_page.py`. And the
> `##`/`###` levels here land unchanged under the generated page's chapters, so
> they must stay as they are.
>
> It describes the experiment behind each chart, never the chart.

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
so a value is never silently compared against an incomparable one. Which surface a
harness serves, and which chapter it lands in, is the table at the top of this page —
joined from the instrument registry in `bench/gen_results_page.py`, so it cannot
disagree with the benches on disk.

### 1 · Cross-core conformance (correctness, not speed)

The three native cores — the C++ **golden reference**, and the from-scratch
TypeScript and Rust reimplementations — must agree **byte-for-byte**. A shared set
of versioned conformance vectors (`tests/conformance/vectors/v1`) is decoded and
re-encoded by every enabled core; the driver diffs the results, and a single
`DISAGREE` fails CI
([ADR-0028](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md)).
This surface measures **truth, not time** — it is what lets every timed surface below
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
- `inproc-target-stored` / `inproc-target-handler` — the **path-target** dispatch leg:
  edges carrying a target key instead of a callback, which is what a wire `SUBSCRIBER`
  produces (the callback form is host-SDK sugar). Each delivery resolves the target in
  the registry, passes the fan-in ACL gate, clones the rope nothrow, and applies the
  target's write effects — measured at roughly **10× a callback edge** at fan-out. The
  `stored` / `handler` pair separates the target's own store from the dispatch itself.

Every mode above except the `inproc-target-*` pair subscribes with an in-process
callback, so a fan-out curve reads the **callback** leg unless its mode says otherwise.

This is the surface that carries the microsecond thesis — the zero-copy substrate
([ADR-0016](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md))
delivering values as loaned `view_t`s — and the one the per-PR gate watches most
closely.

### 3 · Memory footprint (allocations counted, not sampled)

A different instrument entirely. `bench_forward_heap` replaces the global allocator
with a counting wrapper and **arms it around exactly one operation**, so these are
*exact* allocation counts and byte totals — not statistics, not sampling. Bytes are
read from `malloc_usable_size`, so a resident figure is what the allocator really
holds rather than what the caller asked for; whole-run max RSS comes from
`/usr/bin/time -v` and is the coarse process-level number beside them.

Two invariants sit on this surface, and **the scope of the armed window is part of the
first one**. The steady-state forward hop's *own* work must touch no heap — the two-plane
forwarding model
([ADR-0038](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md))
requires it — so `bench_forward_heap` arms the counting allocator around one hop and CI
runs it at `ZEROHEAP_MAX=0`: on a **contiguous (single-link)** frame the hop dispatches by
offset, builds its replacement heads in fixed stack buffers and its gather table in a stack
`iov` array, and a single stray `malloc` inside *that* window fails the job.

Two terms sit **outside** that window, so this is not the claim that a forward hop is
heap-free end to end. The gate drives `capture_transport_t`, a stub link that only sums the
spans it is handed, so the shipping transports' `::iovec` table is never assembled —
`transport_udp` and `transport_tcp` each hold 16 spans inline and spill to the heap above
that, measured at **17 caller spans / ~288 B** by `bench_transport_iov`. And the
**multi-link rope** arm gathers into a block array drawn from the injected receive source,
because the sub-span count is the sender's choice and is known only at run time — nothrow,
so an exhausted heap drops the frame rather than aborting, but not allocation-free. Read
the two benches together; neither is sufficient alone. A terminus, by contrast, *may*
allocate
([ADR-0041](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0041-terminus-arena-decode-span-contract.md))
and is measured rather than gated. The per-vertex resident figure is the one the
constrained profile lives or dies by (a ~16 KB RAM budget on the ESP32 target),
which is why it is a series of its own rather than folded into RSS.

### 3b · Routing & delivery (the network plane, per frame)

The in-process surfaces above measure the graph. A separate set of benches measures the
**network plane** — what a frame costs between arriving and being applied — because the two
move independently and an improvement to one can hide a regression in the other. They cover
the three shapes a frame takes: a transit hop that never resolves, a cold terminus resolve
an established flow pays once, and the warm `COMPACT` frame that dominates a running system.

Every bench on this surface reports latency **and** exact allocation counts, and every one
calibrates its own batch size against the host clock rather than hardcoding a number: a
routing operation costs the same order as `clock_gettime`, so timing one operation at a time
measures the clock instead of the code.

`bench_forward_demux` and `bench_compact_delivery` are recorded into the build-to-build
history alongside the in-process series, so a routing or delivery regression shows up as a
trend rather than being noticed later. They emit the same `RESULT` rows as `bench_libtracer`,
so they need no separate aggregation. Their transcripts are tolerated-empty — a bench that
fails to run must not cost a commit its whole history point — but an empty one emits a build
warning naming the file, because a silently-empty transcript is otherwise a green job that
recorded nothing.

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
| **per-PR hard gate** ([`perf_gate.py`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/perf_gate.py)) | every PR | PR build vs `main` build, **one runner, interleaved A/B** | p50 **+15%** · mean **+12%** · deliveries/s **−12%** · per-vertex bytes **+2%** — *and* disjoint ranges *and* a majority of pairs | fails the PR |
| **push ratchet** | every `main` push | HEAD vs its parent, **three independently-drawn runners** | same as above | turns `main` red |
| **forward-hop zero-alloc gate** | every CI run | absolute | `> 0` allocations on the forward hop | fails the build |
| **soft trend alert** | per `main` commit | vs previous point, **cross-runner** | series drifts past **125%** | a comment, *not* a verdict |

Details that make these trustworthy:

- The per-PR gate watches **twelve canonical points** — a representative slice of the
  fan-out / payload / topic sweeps plus a fold-width point, one per *gated* family
  (`inproc` and `inproc-borrow` share one), so a pullback on any of those legs is caught and
  not just the 1:1 write. They are **not** the whole dispatch surface, and this page should
  not be read as claiming they are: the path-target delivery legs
  (`inproc-target-stored` / `inproc-target-handler`) are measured and charted but **ungated**,
  as are `inproc-deliver`, the `eptype-*` sweep and the `-batch` twins. A pullback confined to
  those ships without the gate objecting. They are
  `perf_gate.py`'s `POINTS`, named here in full because this page is hand-written and a
  bare count says nothing about what is covered; each is keyed
  `mode/payload/fan-out/endpoints`: `inproc/64/1/1` — the canonical 1:1 write;
  `inproc-borrow/64/1/1` — its zero-copy twin, the same write handed a *borrowed* view
  instead of an owned copy; `inproc/64/1024/1` — the
  1024-subscriber fan-out loop; `inproc-path/64/1/8192` — the resolver canary, one
  registry lookup per write across 8192 registered vertices; `mixed/0/6/128` — the
  composed topology, 128 topics whose fan-out varies 1–16 (mean 6) over payloads of
  1 B–8 KiB, which is why its payload column reads 0; `fold-b4/512/1/1` — the L0
  fold walk, 512 bytes held constant across four rope links and timed over a batch; and
  `lkv-store-heap/64/1/1` + `lkv-store-pool/64/1/1` — the L1 **rope-to-contiguous copy**
  (`rope_t::materialize`: one segment allocated from the backend plus the payload
  `memcpy`), against the default heap and against a pooled backend.

  Those last two are here because of what happened without them
  ([#1250](https://github.com/avatarsd-llc/libtracer/issues/1250)): reshaping
  `rope_t::flatten`'s wrapper cost **25–48%** on every path through `materialize` —
  branch and field writes, `op_resolve` reads, FWD COMPACT emission, the RX span sink —
  and no gated point at the time was downstream of that call, so the loss shipped with
  every gate green. They are read out of `RESULT` rows the default sweep already emits,
  so they add no wall-clock. Note the name: `lkv-store-*` measures the **copy-store
  allocation**, not the last-known-value slot.

  Four of the twelve come from OTHER bench binaries, and they are here because of what
  happened without them (#1173): `compact-forward` moved **+41%** across the v0.8.0 →
  v0.9.0 window while every gated point stayed flat, so the gate had nothing to object to.
  They are `compact-forward/64/1/1` and `compact-terminus/64/1/1` — the compact-delivery
  tier's forward hop and its terminus, from `bench_compact_delivery`; and
  `fwd-demux-fixed/79/1/1` and `fwd-demux-scan/79/64/64` — the fixed-slot and scanning
  arms of the FWD demux, from `bench_forward_demux`. Each `POINTS` entry names the binary
  that produces it; every one of them emits the same 12-column `RESULT` format, so this
  costs two extra processes per arm per pair and no new parsing.

  The points are `mode` values of the benches described above, and the numbers are
  their `size` / `fan-out` / `endpoint` columns. The binaries are run
  **interleaved** — `A B / B A / A B / B A`, four pairs, alternating which one starts —
  so a slow window in the machine is shared by both arms rather than donated to
  whichever one holds it. Because the baseline is *the same PR's `main` rebuilt on the
  same runner in the same pass*, the comparison is machine-neutral.
- A point fails only when **all three** of these hold, and the gate prints every one of
  them: the **medians** breach the threshold (the effect is big enough), the two arms'
  **[min..max] ranges are disjoint** (a sign flip inside the ranges reads as
  indistinguishable and can never fail), and a **strict majority of the interleaved
  pairs** breach on their own (the effect reproduces). All three are needed because a
  best-of-3 estimator rejects a bad *sample* but not a bad *window*: a runner that goes
  slow for the whole of one arm's block produces a clean, reproducible, entirely false
  breach, and the majority-of-interleaved-pairs rule is what a window cannot fake.
- Each arm's own spread across the pairs is printed, and the **baseline arm's worst
  spread is reported as the run's drift figure** — the baseline binary cannot be moved
  by the change under test, so its spread is the invariant control leg. It does not
  gate; it tells a reader whether the run was worth believing.
- The same gate additionally checks **five memory probes** (`perf_gate.py`'s
  `MEM_POINTS`, named here in full because this page is hand-written and a bare count
  rots): `vertex` — a default leaf at rest; `vertex_value` — the increment one LKV write
  adds; `vertex_app5` — a leaf carrying a *copied* five-field app-field table;
  `vertex_app5_static` — the *borrowed* twin of that table (ADR-0058); and `reg_escape` —
  the global-heap blocks a runtime registration takes that the graph's injected
  `memory_resource` never sees (ADR-0039 / RFC-0014), whose target is zero. Each ratchets
  on two quantities: **live bytes** per vertex, tolerant at **+2%** because it is
  host-allocator-dependent, and the **number of heap blocks**, which is host-independent
  and therefore ratchets *exactly* — one extra block is a regression, full stop. All come
  from the counting allocator (`bench_forward_heap`), so they are exact rather than
  sampled and need no repetition at all, with the baseline binary probed same-runner via
  `--baseline-bench-fwd`. Supplying that binary for one arm and not the other **fails**
  the gate as a wiring error; supplying it for neither prints an explicit `SKIP` — a
  probe that cannot run never passes silently.
- A per-vertex cost a **ratified clause already prices** is not a pullback, and the memory
  ratchet has one narrow way to say so: a **charged step** (`perf_gate.py`'s
  `MEM_CHARGED`), declared per probe, in bytes, naming the clause that charges it. A
  charge absorbs *at most* its own bytes — a step of exactly that size passes, one byte
  more fails and the failure names the unpriced remainder — and it is **printed on every
  run**, spent or not, so an allowance can never be a gate that quietly moved. It also
  expires by construction: the paired baseline is built from `main`, so once the step
  lands there the delta is zero, the charge prints as `UNSPENT`, and the entry is deleted.
  It is not a tolerance: it does not scale, does not accumulate, and is not a budget to
  spend later.
- The gate's decision rules have their **own unit tests**
  ([`bench/test_perf_gate.py`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/test_perf_gate.py)),
  run in the same CI job before anything is timed, pinning both directions: the recorded
  false-failure sample sets must pass, and the recorded real regressions must still fail.
- The **push ratchet** re-runs that gate on **three separate runner draws** and
  requires the regression to reproduce — one noisy machine cannot fail `main`, and a
  regression that slips through the PR gate still gets caught the moment it lands.
- The **forward-hop zero-alloc gate** is the one **absolute** gate: it is a
  structural invariant, not a speed target. Steady-state forwarding must allocate
  nothing, so the threshold is literally zero.
- The **soft alert** compares *across* runners, so it is only a prompt to look at
  the trend, never a merge-blocker.

The per-PR gate never compares against a *recorded* number at all: both binaries are
in hand on the runner, so every sample on both sides is drawn in the same interleaved
rotation, minutes apart at most. (`bench/perf_baseline.json` still exists for the
local / root-commit path, where there is no baseline binary to interleave with. It is
**host-specific and never committed as a fixed number**, precisely so the gate can
never encode one machine's speed as another machine's target.)

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
- **No network throughput comparison is published.** A valid one needs a real subscriber
  in a second process on **both** sides, with deliveries counted at the receiver rather
  than sends counted at the publisher — an engine whose publisher has no peer emits
  nothing to the wire at all, and a per-send rate multiplied by a composition width is
  arithmetic, not a measurement. That bench now **exists** (`bench/run_compose.sh`): two
  processes per engine over real loopback UDP, the composition width K swept on both
  sides, and every rate taken from the subscriber's own count over the subscriber's own
  clock. It carries two guards, and reports nothing unless both hold — the receiver emits
  no row at all when it observed no values, any malformed record, or fewer throughput
  datagrams than the sample floor the driver hands it, and a **wire-use audit** runs the
  publisher under `strace` and fails the point below a send-syscall floor (`COMPOSE_SEND_FLOOR`,
  default 50), which is exactly the check whose absence let the withdrawn version report a
  rate for an engine that had only ever emitted scouting beacons. What is still absent is
  the **chart**: publishing this comparison is a claim, so it is a separate, reviewed step,
  and until it is taken the chapter states the gap rather than showing a number.
- **Network latency is the surviving network comparison**, and it is fair: a
  single-value, two-process, same-clock measurement over the real loopback kernel path,
  identical topology for both engines. **p50**, the **p99 tail** and the **p999 deep
  tail** are charted per transport — for a latency-first substrate the tail is the
  load-bearing number, and it is where transports separate, since an unreliable datagram
  path can win the median and still spike at p99.
- **The p999 has to earn its own publication.** It is the cost of one message in a
  thousand — on a control path, the size of the deadline it misses — and it is the one
  figure on this page whose *publication* is gated rather than only its value. A p999
  read off `n` samples is the order statistic at `floor(0.999·n)`, so only
  `n − 1 − floor(0.999·n)` samples lie above it: **three** at n = 4 000, and **none at
  all** at n ≤ 1 000, where "p999" is the maximum wearing a percentile's name. The
  accumulator records `n` beside every percentile and flags whether it clears its own
  adequacy floor; below the floor the number is **withheld**, and the shortfall is
  named in the transport-coverage note instead of being drawn as a line that moves for
  reasons the code never touched. The sample count, the number of samples above the
  p999, and the worst single message of the pass are printed under the charts,
  computed from the same rows that drew them.

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
- **Tail percentiles are published, not gated — and here is the measurement that
  decided it.** The deeper into a distribution a statistic reaches, the fewer samples
  stand behind it and the more it moves for reasons that are not the code. Running the
  *same binary* against itself and taking the worst ratio between repeats gives the
  floor any threshold on that leg would have to clear before it stopped firing on its
  own noise. On the **in-process gated points** (18 runs, replayed through the gate's
  then-current best-of-3 estimator, worst of 15 disjoint pairs — the numbers below are
  a property of the *machine*, not of the estimator, so the interleaved gate inherits
  them unchanged):

  | leg | worst same-binary ratio | threshold it would need | gated today |
  | --- | ---: | ---: | --- |
  | p50 | 1.16× | +16 % | yes, at +15 % |
  | mean | 1.22× | +22 % | yes, at +12 % |
  | deliveries/s | 1.36× | −36 % | yes, at −12 % |
  | **p99** | **1.67×** | **+67 %** | **no — published only** |

  A p99 gate would have to sit near **+67 %**, and a regression that large has already
  tripped the +15 % p50 gate several times over: the leg would add no detection while
  adding a new false-fail source on every PR.

  On the **two-process network bench**, the same experiment run at two probe counts
  isolates how much of the tail's instability is simply too few samples (5 repeats each,
  same idle host, only `n` changed):

  | probes per point | p50 | p99 | p999 | worst sample |
  | ---: | ---: | ---: | ---: | ---: |
  | 4 000 | 1.09× | 5.01× | 62× | 21× |
  | **10 000** (published) | **1.09×** | **1.40×** | **17×** | **13×** |

  The median does not care — it is 1.09× either way — while the p99 tightens 3.6× and
  the p999 3.7× purely from sample count. That is why the published run pays for the
  larger count. Even so the deep tail stays **17×** unstable: a loopback p999 is
  dominated by scheduler wake-up, not by either engine's code path. Read the p999 chart
  as the **jitter floor this topology inherits**, and read engine-against-engine on the
  p50 and p99 charts, which at the published probe count are stable enough to carry a
  comparison. (Measured on an idle 24-core host; a shared CI runner is not quieter than
  that, so these are lower bounds.)
- **A throughput pullback with flat latency is a machine, not a regression.** Every
  gated point is measured by two instruments over the same operation — a per-op clock
  (p50, mean) and a bulk timer (deliveries/s) — and a change in what the operation costs
  moves both. When only the bulk timer moves and both latency legs come back
  flat-or-better, the two instruments contradict each other, and the gate reports the
  contradiction rather than failing on it. This is not hypothetical: one PR that touched
  only L4 was failed at −33 % throughput on an L0 codec point it has no call path to,
  with p50 identical and the mean *better*. The guard is deliberately narrow — any
  upward move in either latency leg, of any size, leaves the failure standing.
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

**The A/B protocol — what a two-arm comparison may and may not vary.** An A/B of a code change runs two binaries and attributes the difference to the change.
That attribution is only sound if **nothing else** differed. Two things that look
harmless routinely do.

**Build directory: harmless, and now measured.** A cross-worktree A/B — build
`origin/main` in one worktree, the change in another, interleave — was suspected
(#807, from PR #806) of measuring *code layout* rather than code: identical
`origin/main` source built at two paths appeared to differ by **+1.7 % (65589 B
frames) to +6.7 % (53 B frames)** on `bench_terminus_tier`'s `terminus-arena` leg
over 12 interleaved rounds, with disjoint ranges. That cause is **refuted**. Building
the same commit at two paths of different lengths produces, on this toolchain,
**byte-identical output**: all 27 `libtracer` object files, `libtracer.a`, and the
`bench_terminus_tier` executable `cmp` equal (one md5 for both arms). There is no
layout to be sensitive to, so no layout lever is warranted, and none ships —
`-falign-functions=64` over the whole bench + library build reads **−1.50 % to
+1.56 %** against stock across 10 interleaved rounds, sign varying by frame size,
i.e. nothing. Re-run at two paths under this protocol the same leg reads **−0.64 % to
+0.87 %** (unpinned) and **−0.64 % to +0.73 %** (pinned), 12 interleaved rounds.

**CPU placement: the real hazard, and it is large.** The reference host is
**heterogeneous** — 4 Zen 5 cores at 5.16 GHz (`cpu0–7`) and 8 Zen 5c cores at
3.29 GHz (`cpu8–23`). The *same binary* on the `terminus-arena` leg reads **+47.0 % to
+53.7 %** slower pinned to a compact core than to a classic one, 5 rounds each, ranges
disjoint and tight within each arm (e.g. 53 B: 308–313 ns vs 474–481 ns). That is the
signature a confounded A/B wears: tight, reproducible, and entirely about the machine.
Even the *choice* of pinning moves the figure — at 53 B, 15 runs each: unpinned median
228 ns, `taskset -c 2` 234 ns, `taskset -c 2,3` 238 ns (**+4.4 %** across pinning
policies, from SMT sharing). A few percent between two arms needs no exotic
explanation on such a host; it needs only that the two arms were placed differently.

So the rules for any latency A/B, on this leg or any other:

1. **Pin both arms identically**, to the same *single* logical CPU on the same core
   class (`taskset -c 2`). Pinning one arm and not the other, or to different core
   classes, is not a comparison.
2. **Interleave** the arms round-robin within one session and report **medians *and*
   ranges** of at least ~10 rounds per arm. A single round per arm cannot separate the
   change from the machine.
3. **Discard the first execution.** A cold process's first measured point on this leg
   reads ~313 ns against a 228 ns steady state (**+37 %**) — it is idle-state wake-up,
   not code.
4. Prefer **same-directory A/B** (`git stash`, rebuild, re-run in one session) when it
   is available; it varies strictly less than a two-worktree run.
5. When the expected effect is **smaller than the leg's own noise floor**, do not
   reach for a stopwatch at all — use **object-file `cmp`** against the baseline tree
   (the method PR #799/#806 established). A change that leaves 25 of 27 objects
   byte-identical has proved more about the unchanged paths than any bench can.

The stack-offset hypothesis was also tested and is negative: sweeping the environment
block from 0 to 4000 bytes (which shifts the initial stack pointer, and with it the
bench's 64 KiB stack slab) moves the leg 2.8–4.4 %, non-monotonically — indistinguishable
from run-to-run noise.

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
host or a different day, and never with one arm pinned differently from the other.
The full two-arm protocol, and the measurements behind each of its rules, is above
under **The A/B protocol**, in *Reading the numbers*. The single-CPU `taskset -c 2` in the commands above is part of the measurement, not
decoration: this host is heterogeneous and an unpinned arm can land on a core class
50 % slower than its partner's.

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
