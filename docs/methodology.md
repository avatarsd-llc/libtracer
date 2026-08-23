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

### 4 · libtracer vs Zenoh (absolute, best of 3 rounds, same runner)

A side-by-side against [Eclipse Zenoh](https://zenoh.io) (zenoh-c, peer mode). Both are
measured **on the same runner in the same rounds** — libtracer compiled from source at
`-O3`, Zenoh as the upstream prebuilt `zenoh-c 1.10.0` release binary that
`bench/fetch_zenoh.sh` downloads (we do not build it, so its optimization profile is
upstream's, not ours). The whole grid is swept **3 times** and each point keeps its best
observation across those rounds (`bench/best_of_rounds.py`): contamination on a shared
runner is one-sided — a busy neighbour can only ever make a round slower — so the best
round is the estimator closest to what the code does, while a median would be an estimate
of how busy the machine was. Both engines are executed in the same loop the same number of
times; giving one arm N tries and the other one shot would be a thumb on the scale rather
than a measurement. The **tail** rows (p999 / max) are deliberately **not** reduced this
way — a minimum p999 would advertise the calmest moment the host ever had as though it
were a worst case — so those are left as measured. And the charts plot **absolute**
throughput / latency / bandwidth — both engines as series on shared axes. There are **no
speed-up ratios**: every point is a measured number you can read off directly. Fairness is
discussed in its own section below, because a naive put-vs-write comparison would be
misleading.

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
| **push ratchet** | every `main` push | HEAD vs its parent, **three independently-drawn runners** | same as above | **advisory** — each replica reports (see the tier note below) |
| **forward-hop zero-alloc gate** | every CI run | absolute | `> 0` allocations on the forward hop | fails the build |
| **soft trend alert** | per `main` commit | vs previous point, **cross-runner** | series drifts past **125%** | a comment, *not* a verdict |

**Verdict tiers.** The comparison is the same wherever it runs; what a caller declares is
whether a breached ratchet may *stop the job*. `perf_gate.py --tier blocking` fails on a
breach, `--tier advisory` prints the identical numbers and the identical verdict line and
exits 0 — with a `::warning::` annotation, so an unenforced breach is never quiet. The tier
is **not** a second, looser threshold set: an advisory run reports exactly what a blocking
run would have said. The per-PR gate is blocking; the three push replicas are advisory,
because a single noisy runner should not carry a verdict alone and the ruled escalation
(two of the three replicas agreeing) is not built yet. Omit the flag and you get
**advisory** — the safe default for a maintainer's own machine, where the bar was never
calibrated; every CI invocation declares its tier explicitly and a unit test fails the
build if one does not.

Details that make these trustworthy:

- The per-PR gate watches **fifteen canonical points** — a representative slice of the
  fan-out / payload / topic sweeps plus a fold-width point, one per *gated* family
  (`inproc` and `inproc-borrow` share one), so a pullback on any of those legs is caught and
  not just the 1:1 write. They are **not** the whole dispatch surface, and this page should
  not be read as claiming they are: `inproc-deliver`, the `-batch` twins and the `eptype-*`
  sweep's two **lean** arms are measured and charted but **ungated**. A pullback confined to
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
  `memcpy`), against the default heap and against a pooled backend; and
  `inproc-target-handler/64/8/1` + `inproc-target-stored/64/8/1` — the **path-target**
  dispatch legs at fan-out 8, edges carrying a target key rather than a callback, which
  is the leg a wire `SUBSCRIBER` actually takes.

  Those last two are here because of what happened without them
  ([#1250](https://github.com/avatarsd-llc/libtracer/issues/1250)): reshaping
  `rope_t::flatten`'s wrapper cost **25–48%** on every path through `materialize` —
  branch and field writes, `op_resolve` reads, FWD COMPACT emission, the RX span sink —
  and no gated point at the time was downstream of that call, so the loss shipped with
  every gate green. They are read out of `RESULT` rows the default sweep already emits,
  so they add no wall-clock. Note the name: `lkv-store-*` measures the **copy-store
  allocation**, not the last-known-value slot.

  The `inproc-target-*` pair is gated at **fan-out 8** and nowhere else, for two measured
  reasons ([#1077](https://github.com/avatarsd-llc/libtracer/issues/1077)). Fan 8 sits
  exactly on `vertex_t::kInlineFanout`, the no-heap small-fan-out boundary, so it is the
  width that prices the narrow-fan snapshot path — which is where that issue's ~**+5.5%**
  step appeared, invisible to every gate at the time. And a layout control (the same
  source rebuilt at three `-falign-functions` settings) measured this row as the most
  layout-**stable** point of the whole sweep, **0.62%** across placements against ~15% for
  `fold-b4`, so the false-red risk from code placement is near zero here. Both legs are
  gated because `stored` carries the same shape as `handler`; it reads reliably now that
  [`host_guard.py`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/host_guard.py)
  rejects the contaminated windows that once put its own A/A null at −14.7%. Like the
  `lkv-store-*` pair they add **no wall-clock**: the default sweep already emits both rows
  at every fan width, so this is two more keys read out of output already collected. Both
  gate on **all three legs** at the nominal +15% / +12% / −12%: a fan-8 row's p50 and mean
  time the whole 8-subscriber publish (~640 ns and ~800 ns p50 respectively, against
  per-delivery costs of ~62–80 ns), so they sit far above the sub-100 ns band where the
  tick guard would demand an extra +25 ns absolute and blunt them the way it blunts
  `fold-b4`.

  `eptype-stream/64/1/1` is the fifteenth, and the reason it is gated while its two
  siblings are not is the whole point of adding it. `eptype-lean` and `eptype-lean-cached`
  are the `inproc` and `inproc-borrow` code paths *re-emitted* under an endpoint-type
  name — already gated, twice over, so gating them again would buy correlated evidence
  rather than coverage. `eptype-stream` is not a re-emission: it is the only point on the
  list that declares a bounded history depth, and therefore the only one downstream of the
  **`STREAM` role's retention work**. Nothing else on the list touches that path, so a
  pullback confined to retention was invisible to all fourteen predecessors, which is the
  same guard-gap shape as `lkv-store-*` and the compact/demux arms below. It costs **no
  wall-clock**: the default sweep already emits the row. And all three legs bite at the
  nominal thresholds — it measures **~190 ns** p50 and mean over five best-of-rounds on a
  busy 31-core host, far above the sub-100 ns band where the tick guard would demand an
  extra +25 ns absolute.

  **What that row prices changed underneath it**, and the change is worth naming because
  the series name did not. Until `bdd1066b` the ring was the *producer's*: a write to a
  stream vertex appended to its own history under the stripe mutex, before fan-out. Under
  RFC-0025 §4.6.1 Amendment 2 **a producer never queues** — the queue belongs to the party
  that wants depth, so the ring lives on the **receiving** vertex and the admission runs
  *after* the last-known-value publish rather than before fan-out. What the gated row times
  today is therefore the receiver leg: retire the entry the depth intent pushes out, admit
  the new one by reserving its retained width against that vertex's own injected block
  source, and append. The row keeps its name on purpose. The rule above — *renaming beats
  reinterpreting* — governs a change in what the **instrument** measures; the bench source
  is untouched and the operation it drives is still "one 64 B write into a depth-16 stream
  vertex". The mechanism beneath it moved, and a core change moving the line is exactly the
  signal the series exists to show.

  **The append-and-admission leg is read as a gap, not as a series of its own.**
  `eptype-stream` and `eptype-lean` are the same write at the same payload, fan-out and
  topic count, emitted from the same pass of the same binary and banked as separate series
  on the *Endpoint-type family* card — so the distance between the two lines is the
  receiver-ring cost, over the whole recorded history, with the runner shared by both arms.
  Two things that leg does **not** price, stated because a reader would otherwise assume
  them covered: a reservation is **handed straight on** when a retiring entry has the shape
  the new one needs, so a steady uniform stream costs its source zero `try_alloc` calls per
  write and this row measures the admission *bookkeeping*, not an allocator round-trip; and
  the resident footprint of the receiver's ring state has no memory probe. A bench for the
  shape-changing admission path — where the carried reservation cannot be reused and
  `try_alloc` fires on every write — **does not exist**, and neither does one for the ring's
  resident bytes. Both are gaps in this page, not numbers it is withholding.

  Four of the fifteen come from OTHER bench binaries, and they are here because of what
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

- **Deliveries are counted at the subscriber on both arms.** Neither engine's delivery
  figure is inferred from `publishes x fan-out`. Each side's timed window ends only once
  every delivery it owes has landed, each reports what its subscribers actually received,
  and each prints a warning naming any point that came up short. The two engines reach
  that guarantee differently, and the difference is not a handicap: libtracer dispatches
  inline, so `write()` returns only after the last subscriber callback and its publish
  loop *is* its delivery loop, whereas Zenoh delivers off the publishing thread and the
  harness spins on the receive counter inside the window until the backlog drains. This
  matters because the arithmetic form would not merely be imprecise, it would be
  unfalsifiable: libtracer's wide-fan-out snapshot truncates to its inline prefix when its
  overflow reserve fails, and its HANDLER and STREAM legs shed an entire fan-out on a
  clone or ring-append failure — in every one of those cases `write()` still returns
  success, so a `publishes x fan-out` figure would report deliveries that never happened
  and nothing in the harness would contradict it.

  The libtracer-only rows that audit deliberately left on the arithmetic — the
  `inproc-target-*` pair and `inproc-remote`, which have no Zenoh row beside them — were
  finished on the same terms
  ([#1481](https://github.com/avatarsd-llc/libtracer/issues/1481)). `inproc-target-handler`
  and `inproc-remote` count at their consumer like every other row. `inproc-target-stored`
  has no consumer to count at — its delivery terminates in the target's last-known-value,
  and hanging a counting subscriber off each target to see it would add an edge per target
  to the topology being timed — so that one subtracts `graph_t::delivery_drops()` from the
  ceiling instead. That is a measurement rather than a restatement of the arithmetic
  because RFC-0025 §4.4 forbids an unaccounted shed: every leg that drops one of those
  deliveries counts it.

- **Write does strictly more than put.** libtracer's `write` row also **persists**
  the value (it becomes the vertex's last-known-value) and bumps the `await` /
  readiness sequence on every op. Zenoh's `put` is transient delivery only. So the
  libtracer *write* row is charted against a Zenoh row that does **less** semantic
  work — the `inproc-deliver` (`propagate`) series is the apples-to-apples
  counterpart: value stored once, each op only delivers, matching put semantics. Both
  libtracer series are shown so the reader sees the full-work and the like-for-like
  number side by side.
- **The topic-count rows must agree on how the destination is spelled** — and the
  pre-existing `inproc-path` pair did **not**. libtracer's `inproc-path` row writes *by
  address*, so a registry resolution sits inside every timed iteration; the Zenoh row of the
  same name publishes through a **declared `Publisher`**, which is the bound form and resolves
  nothing per put. A resolution term therefore sat inside one arm and nowhere in the other, and
  any narrowing of the margin along that ladder could not be attributed to either engine's topic
  scaling. The `topics-bound` / `topics-addr` pair (`bench/run_topics.sh`,
  [#1485](https://github.com/avatarsd-llc/libtracer/issues/1485)) fixes
  it by measuring **both** spellings on **both** engines over the same ladder — pre-bound
  handle / declared publisher, and destination resolved inside the operation — so the resolution
  term is visible as its own difference instead of hidden inside one arm. What is compared is
  per-operation *resolution*: the `path_t` on one side and the `KeyExpr` on the other are both
  pre-built, because charging one engine for a string parse the other hoisted is the same
  mistake in the other direction. The completed two-sided run and what it does to the audit's
  conclusion are in *Designated model boundaries* below; the short form is that bound against
  bound both engines are near-flat, and the axis the audit reported as Zenoh's was the
  unmatched pair.
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

## Designated model boundaries

Three properties of libtracer's model are **designed**, not accidental, and each of them
carries a cost this page measures. They are listed here so that cost can be read for what
it is: the price of a capability that was chosen, not a defect awaiting a fix. Every
comparison on this page — the Zenoh chapter above most of all — has to be read against
them, because a comparison between two engines that solved *different problems* is only
informative once the problems are named. Nothing below proposes a redesign; where a
boundary has a **mitigable constant**, the mitigation is a measurement, and it is linked.

A boundary earns that name only from a measurement that **isolates** it, and the section is
held to that standard in both directions. A number that moves for some other reason is not
evidence for a wall, however convenient the shape of the curve — the first draft of
boundary 1 argued from exactly such a curve and the isolating control arm refuted it. Where
that has happened it is said outright below rather than quietly rewritten, because a
boundaries section that only ever accumulates walls is an argument, not an instrument.

**1 · Every distinct address is resident state.** A vertex is not a name that a message
happens to carry — it is an object that exists between writes. That is what pays for the
last-known-value (a reader gets the current value without waiting for the next publish),
for `await` (a readiness sequence has to live somewhere), for composed reads across a
subtree, and for the per-vertex ACL. An engine that only *routes* keeps no such object and
so has nothing to scale in the number of addresses.

**What residency costs is bytes.** That is the axis the trade is paid on, and it is
measured rather than derived: the decomposition sweep
([#1485](https://github.com/avatarsd-llc/libtracer/issues/1485) /
[#1496](https://github.com/avatarsd-llc/libtracer/pull/1496)) reads **132 B of heap and
140 B of RSS per vertex** at 10⁶ vertices, converging from above as the upper tree's fixed
cost amortizes, against a computed ~120 B floor — **+10 %** over the arithmetic. A million
addresses is **132 MB of heap and 140 MB of RSS**. The **O(#addresses) residency is the
model** and no footprint work turns it into O(1); the **constant** in front of it is fair
game and is actively ratcheted on this page, bytes at +2 % and block counts exactly.

This is also where the boundary actually bites the target the project cares most about. On
a **wide** host 132 MB for a million addresses is unremarkable; on the **narrow** end — the
constrained MCU profile living inside a ~16 KB RAM budget — the per-vertex byte figure is
the number that decides how many addresses a node may have at all. Memory, not latency, is
the axis on which "every distinct address is resident state" is a real constraint.

**Residency is NOT a per-operation LATENCY cost, and the control arm that decides this is
unambiguous.** Hold **one million vertices resident and touch exactly one**: a write costs
**90 ns** — the same 90 ns it costs with a thousand resident. Resolving a single hot address
moves 70 → 90 ns across three decades. Descent is population-independent outright
(ADR-0057, now verified rather than assumed): a fixed-shape probe address resolves in
**60 ns at 10³ and 60 ns at 10⁶** warm, and **90 ns flat** with its lines evicted between
samples. Everything that grows, grows with the **touched** set, not the resident set.

Read that refutation at exactly its own width. It kills the claim that residency is a
**latency** wall — which is what the earlier draft of this section asserted, and it was
wrong on its own evidence. It does **not** say the residency model is free: the bytes above
are the cost, and they are real. A boundary can be genuine on one axis and absent on
another, and saying which is which is the whole job of this section.

**The topic-count curve in the Zenoh chapter is not this boundary's evidence — and the
completed comparison retires it.** The fairness audit
([#1480](https://github.com/avatarsd-llc/libtracer/pull/1480)) went looking in both
directions and reported exactly one axis where Zenoh was the better engine outright:
**topic-count scaling**, Zenoh's p50 moving **220 → 230 ns (+5 %)** across 1 → 8192 topics
against libtracer's **140 → 190 ns (+36 %)**, narrowing our margin from **1.57× to 1.21×**.
Both rows were measured correctly and neither is withdrawn. They were **not the same
operation**: the libtracer row (`inproc-path`) re-resolved the destination address inside
every timed iteration, while `bench_zenoh` published through a declared `Publisher` and
resolved nothing per put. A resolution term sat inside one arm and nowhere in the other, so
the narrowing could not be attributed to either engine's topic scaling.

The `topics-bound` / `topics-addr` pair
([#1485](https://github.com/avatarsd-llc/libtracer/issues/1485), `bench/run_topics.sh`)
measures **both** spellings on **both** engines over one ladder, and it is now a result
rather than an instrument. 64 B payload, fan-1, deliveries counted at the subscriber,
best-of-5-rounds with both arm orders and both engine orders, **two independent ladders**
agreeing to ≤ 2 % on every arm:

| p50 / mean ns per operation | 1 topic | 100 topics | 10 000 topics |
| --- | ---: | ---: | ---: |
| libtracer `topics-bound` | 110 / 116 | 110 / 116 | 110 / 118 |
| Zenoh `topics-bound` | 210 / 214 | 210 / 215 | 220 / 232 |
| libtracer `topics-addr` | 140 / 140 | 170 / 174 | 190 / 196 |
| Zenoh `topics-addr` | 320 / 332 | 1 450 / 1 532 | 221 000 / 226 351 |

Read **bound against bound** — the like-for-like pair, and each engine's own recommended
spelling — libtracer moves **116 → 118 ns (+2 %)** and Zenoh **214 → 232 ns (+8 %)** across
four decades of topic count, so the margin *widens* slightly, 1.84× → 1.97×. The audit's
"+36 % against +5 %" was our resolve-per-operation arm charted against their bound one.
Matched, **neither engine has a topic-count problem in the bound form**.

Read **resolve against resolve**, the growth is on the other side and it is large.
libtracer's per-operation `find()` descent moves **140 → 196 ns (+41 %)** over the same
span — the audit's +36 %, reproduced at the shape that actually produces it. Zenoh's
undeclared `Session::put` moves **332 ns → 226 µs**, and the completed run settles the
"is that a different code path or a scaling curve" question #1485 left open, in the only
sense that mattered: it is **a curve, and a linear one**. From 100 to 10 000 declared keys
the log-log slope is **1.08** — O(N) within the instrument — with a marginal cost of about
**12 ns** per additional declared key over the first decade and **23 ns** over the last two.
It is of course reached by a different code path from `Publisher::put`; that is the arm's
whole purpose. What it is not is an outlier, a congestion effect or a measurement artefact:
inside that path the cost is exactly the linear match a declared publisher exists to hoist.

So the honest statement of this axis, replacing the one above: **both engines are near-flat
in the bound spelling and both degrade in the resolve-per-operation spelling — libtracer by
41 % across four decades, Zenoh by a factor of about 680.** The mitigation is the same on
both sides and both APIs already offer it: bind, or declare, once. There is no axis here on
which Zenoh scales better in topic count. There was an axis on which our own bench spelled
its destination worse than theirs did.

Host, recorded with the numbers: 31 CPUs, no `cc1plus` alive at any 20-second sample of
either ladder, 1-minute load 1.4–3.3 throughout except one ~60–90 s excursion per ladder
(peaks 10.4 and 9.4, unattributed, on a shared desktop). Best-of-rounds is the specific
mitigation for one-sided contamination, and the two ladders' ≤ 2 % agreement on every arm is
the evidence that it worked here.

Two caveats on that decomposition, because it is fresh. Its arms are **not gated** — they
are new and their run-to-run stability is unproven, and `POINTS` is a promise about
stability.

And **the `topics-*` arms are not on the generated results page.** One ladder costs about
nine minutes, almost all of it the Zenoh `topics-addr` rung at 10 000 keys, so wiring it
into the documentation job would add roughly twenty minutes to every docs build to
re-derive a result whose interesting content is structural. It is run by hand; the table
above is its output. Those numbers supersede the preliminary single-round, single-engine
Zenoh figures recorded on #1485, which were never quotable and are deliberately not
reproduced here. The decomposition beside them was taken on a quiet box (1-minute load
0.64–1.15, no `cc1plus` alive, 31 CPUs, best of 3 rounds with the arm order flipped on
alternate rounds, two full ladders agreeing to ~2 % at 10⁶).

One follow-up remains open and it is a ruling, not a redesign:
[#1486](https://github.com/avatarsd-llc/libtracer/issues/1486), on memoizing
`vertex_slot()`, whose reverse scan is exactly O(total vertices) — measured at 450 ns at
10³ rising to **410 µs at 10⁶**, taken under a shared mutex on every binding mint. That is
the number the ruling was blocked on; the ruling itself is #1486's to make.

**2 · Paths are routes, not location-independent names.** A path is resolved from the
vantage point of the graph doing the resolving; the same value can be reachable by
different paths from different places, and a path handed to a peer does not carry a
promise that it means the same thing there. This is the boundary that buys **composition
with no infrastructure**: two graphs are joined by mounting one into the other, and the
join needs no broker, no registry service, no name authority and no agreement between the
parties beyond the mount itself. A location-independent naming scheme would make a path
portable across vantage points, and would need exactly the infrastructure that absence is
the point of not having. Comparisons that assume a global namespace are comparing against
a system that has one.

**3 · The producer's write budget is fixed, fan-out is value-agnostic, and a producer
never queues.** A write costs what it costs regardless of who is listening: fan-out
carries the value without inspecting it, and the depth a consumer wants is the consumer's
own ring on the consumer's own vertex, charged in bytes to a source that vertex injected
(RFC-0025 §4.6.1 Amendment 2). What this buys is the hot write path — the reason a write
is a sub-100 ns operation at all, and the reason its cost does not move when a subscriber
decides it wants history. What it forecloses is symmetric and worth stating: there is no
producer-side buffering to smooth a slow consumer with, no content-dependent routing
decision inside the fan-out loop, and no way for a subscriber to make a producer pay for
its own depth. The budget is instrumented rather than asserted — the write path's compiled
size is ratcheted symbol by symbol (`bench/symbol_ratchet.json`), and a change that moves
it has to price the move on the bench before the pin is allowed to move with it.

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
- **Across rounds, take each arm's BEST — never its median — and read the range as a
  contamination diagnostic.** Contamination is one-sided: a busy neighbour can only
  make a round slower, never faster. A low order statistic across an arm's rounds
  therefore rejects a dirty round, while the median merely counts them, and the median
  flips as soon as half the rounds are dirty — which is why the gate estimates
  best-of-N and why an ad-hoc driver must too. Measured on the pinned host with one
  binary against itself
  ([#1358](https://github.com/avatarsd-llc/libtracer/issues/1358)): in a window where a
  neighbouring job got busy, median-of-rounds put `fwd-rope-hop` at **−33 % … +54 %**
  while best-of-rounds on the very same samples stayed inside **1.44 %**; quiet, both
  estimators agree inside **±0.5 %**. The `[min..max]` range is what tells the two
  situations apart — a spread near **2×** whose ends sit in two clusters rather than
  spreading smoothly is a contaminated window, and the run must be repeated rather than
  reported, whatever the medians say.
- **The same estimator rule governs the series, and a gap in it is a measurement
  defect.** Points of the fixed-host store are rounds of a slower experiment, so they
  inherit both halves of the rule above. First, a baseline drawn from them is the
  window's **best**, never its median:
  [`store_guard.py`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/store_guard.py)
  compares each banked point with the best of the last 20 **trusted** points, which is
  the comparison benchmark-action's point-to-point `alert-threshold` cannot express —
  five merges of +4 % never trip a 15 % step, and the series ends 22 % slower with no
  alert ever raised. A best is a *floor*, though, so a breach must also land outside
  that window's whole `[min..max]`: on the real store the percentage bar alone reports
  **59** breaches where the range guard leaves **2**. Second, a *flagged* point is not
  a sample — it answers what the machine was doing, not what the code cost — so it
  neither sets the baseline nor counts as coverage. And coverage is itself guaranteed:
  a run that never happens (filtered out, failed, or cancelled while queued) leaves a
  hole nothing notices, which is how one +41 % step became a **16-merge range** instead
  of a commit ([#1173](https://github.com/avatarsd-llc/libtracer/issues/1173)). A daily
  catch-up measures HEAD whenever the last trusted point is more than 8 bench-relevant
  merges behind `main`, so the guarantee is a **bound on the gap** — narrow enough to
  bisect — rather than a point per commit.
- **A banked series is a TREND instrument; the paired same-runner A/B is the gate.** The
  two stores answer "where has this point been going" across machines and months. They do
  not answer "did this commit cost anything", and reading them as if they did produces
  verdicts the code never earned. A textbook case, both halves measured on the same day:
  the rolling drift check on the banked series warned that `inproc 64B/fan1/1ep` p50 had
  gone to **400 ns against a 100 ns baseline (+300%)**, while the interleaved same-runner
  A/B on the **identical commit** read **1.02×**. Nothing regressed — the banked baseline
  and the banked point were taken on different machines under different load, and the
  quotient of two absolutes across that gap is instrument drift wearing a percentage sign.
  So: a drift warning is a **prompt to measure**, never a verdict; a verdict comes only
  from two arms interleaved on one runner in one session, which is what the per-PR gate
  is and what every ratchet in the table above compares. This holds for every banked
  series on this page, the memory ones included.
- **An absolute without its host is not a measurement, and this page never publishes
  one.** Load alone is worth more than most of the effects anyone argues about: the same
  gated point read **3.6 M deliveries/s while CI was building on the box and 6.0 M/s
  quiet — 1.6×**, no code between the two. Every absolute here therefore names the machine
  and the conditions it was taken under, and a figure that arrives without them is
  unusable rather than merely imprecise —
  [#1495](https://github.com/avatarsd-llc/libtracer/issues/1495) is open on exactly that
  defect in a **normative** document: RFC-0025 §4.6.2 states throughput caps as bare
  cross-machine numbers, and unmodified `main` misses two of them by **1.40× and 1.71×**
  purely because nobody recorded which host they were cut on. Where this page mentions
  those caps it links that issue; it does not report them as met.
- **Record the load context on both sides of every measurement, and wait for
  quiescence first.** `python3 bench/host_guard.py wait` before the run and
  `/proc/loadavg` either side of it: an absolute figure without its load context cannot
  be re-judged later, and this is the fact that the same pair of binaries reads inside
  ±0.5 % at load average ~2 and up to **+81 %** at load average 16–36. The contaminant
  that matters is **memory-subsystem** contention, not runnable-task count — five pure
  CPU spinners (load average 6.8, just under the guard's bar on the 31-CPU host) cost
  **0.83 %**, while memory-bandwidth neighbours at the same nominal pin cost tens of
  percent.
- **A per-op figure is only as honest as the batch it was timed in.** Timing a batch and
  dividing charges the window's two clock reads (~22 ns on the pinned host) once per
  *sample*, so the batch size is part of the measurement: forcing it on a ~238 ns
  operation reads 260 / 250 / 242 / ~238 ns at batch 1 / 2 / 4 / ≥8. A calibrator that
  picks the batch by comparing two timed quantities therefore lets the machine choose
  the answer, discretely — `bench_common.hpp`'s `calibrate_batch_for_window` doubles
  until the *window* reaches 20 µs instead, so the batch follows the operation's own
  cost and repeats across executions.

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
