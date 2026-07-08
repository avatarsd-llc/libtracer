# libtracer ↔ Zenoh benchmark

A side-by-side **speed and latency** comparison of libtracer and
[Eclipse Zenoh](https://zenoh.io), swept across a parameter **matrix** (payload
size × subscriber fan-out × topic count) plus a mixed workload — so the result is a
*response surface*, not a single point.

The live numbers + interactive **absolute-value** charts are auto-generated in CI on
every docs build and published on the
**[Performance page](https://avatarsd-llc.github.io/libtracer/docs/performance.html)** — no
committed figures, no speed-up ratios. This README is the methodology; run `./grid.sh`
for a local `preview.html` of the same charts.

## Harnesses

| script | what it measures |
| --- | --- |
| `run.sh` | **in-process** sweep: dispatch + (optionally) serialization cost, on one process. |
| `run_net.sh` | **network**: two processes over real localhost **UDP** (the kernel path). |
| `grid.sh` | **response-surface grid** for both engines → a self-contained `preview.html` of the absolute-value comparison charts (the same ones CI publishes, via `render_compare.py`). |
| `bench_scatter` | **scatter-gather egress**: one `sendmsg(iovec)` ships a K-value composite rope. |
| `bench_forward_heap` | **16KB-RAM zero-heap gate**: a global `operator new` counter measures how many heap allocations one FWD *forward hop* costs (ADR-0038). |
| `bench_fanout_clone_storm` | **many-core refcount contention**: T threads clone+release one shared segment — the per-subscriber fan-out primitive under wide fan-out (ADR-0032 128-core row). |
| `bench_await_wakeup_storm` | **many-core await fan-in**: one writer storms writes while W threads `await` one vertex — condvar/notify_all + vertex-lock scaling (ADR-0032 128-core row). |

### `bench_forward_heap` — the 16KB-RAM zero-heap forward gate (ADR-0038)

A minimal node has ~16KB of RAM: the FWD *forward path* must allocate **zero** heap per
hop (offset-dispatch + pooled segment heads + stack iov — [ADR-0038](../docs/adr/0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md)
invariants #1/#2/#5). The gate is **measured, not asserted**: this bench replaces the
global `operator new`/`delete` (all variants — the aligned-nothrow form `heap_alloc`
uses *and* the plain form STL uses) with a counting wrapper, brackets exactly one
forward hop, and reports `allocs` / `frees` / `bytes`. Single-threaded by construction
(the synchronous-substrate model: a 16KB CAN node forwards inline on its receive).

```sh
./build/bench_forward_heap              # report-only: prints the current per-hop alloc count
ZEROHEAP_MAX=0 ./build/bench_forward_heap   # hard gate: exit 1 if allocs > 0 (LIVE in perf.yml)
```

**Current: 0 allocs / 0 B per forward hop — the gate PASSES and is enforced in CI**
(`perf.yml`, `ZEROHEAP_MAX=0`). The forward hop is heap-free by construction: offset
dispatch (no `wire::decode`), fixed stack header buffers + a stack `iov` array (no
`std::vector`), views straight into the untouched inbound frame.

| Stage | allocs / hop | bytes / hop | how |
| --- | --- | --- | --- |
| Stage-1 baseline | 24 | 2044 | full-decode every frame + rebuild headers with `std::vector` |
| Brick 1 (offset dispatch) | 15 | 220 | forward hop stops decoding — the tree it discarded is gone |
| **Brick 2 (stack heads + iov)** | **0** | **0** | the four per-hop `std::vector`s → stack buffers |

The counter overrides **every** `operator new` variant (the aligned-nothrow form
`heap_alloc` uses *and* the plain STL form), so a `pmr` resource that falls through to
the heap is caught too ([ADR-0039](../docs/adr/0039-pmr-memory-model-host-aligned-allocation.md)).
"Zero" is the *steady-state* hop — init / terminus / the host application allocate
freely, out of the armed window.

```sh
./fetch_zenoh.sh   # vendors prebuilt zenoh-c 1.9.0 + zenoh-cpp (x86_64 linux; not committed)
./run.sh           # in-process matrix — side-by-side terminal table
./grid.sh          # sweep both engines → preview.html (absolute-value charts, no extra deps)
```

Without `fetch_zenoh.sh`, only the libtracer numbers appear.

`grid.sh` sweeps both the **in-process** axes and the **network transports** (UDP / TCP
over the loopback kernel path) via `run_net.sh`, which launches a two-process pub/sub pair
— `bench_transports` (libtracer) and `bench_zenoh_net` (Zenoh) — for each engine and
protocol, the **same two-process topology** so the comparison is fair; each subscriber
emits `net-<proto>` RESULT rows. (WebSocket is built but held: libtracer's WS transport
shows order-of-magnitude single-run latency jitter under this bench. QUIC needs msquic + a
TLS cert and the `-DLIBTRACER_WITH_QUIC` module, gated like the dedicated `quic` CI job.)

## Many-core contention microbenchmarks (Wave 0e, ADR-0032)

The 128-core scaling review left two data-plane questions to *measurement, not
redesign* ([docs/research/2026-07-04-architecture-deepening-review.md](../docs/research/2026-07-04-architecture-deepening-review.md),
"128-core scaling"): segment-refcount cacheline contention under wide fan-out, and
`await`/condvar wakeup scaling. These two benches answer them. Both are **diagnostic**
— thread-contention numbers are hardware-dependent, so they are deliberately **not**
wired into `perf.yml`'s regression gate; run them on the real many-core target (the
nightly row), not a shared CI runner.

```sh
cmake -S bench -B bench/build -DCMAKE_BUILD_TYPE=Release
cmake --build bench/build --target bench_fanout_clone_storm bench_await_wakeup_storm -j
./bench/build/bench_fanout_clone_storm   # RESULT mode=clone_storm, fanout=T threads
./bench/build/bench_await_wakeup_storm   # RESULT mode=wake_storm,  fanout=W waiters
```

- **`bench_fanout_clone_storm`** parks T threads on one shared value view and has each
  clone+release it (`view_t copy = hot;` — a `segment_ptr_t` inc/dec) in a time-boxed
  loop. `pub_per_s` is per-thread clone+release throughput; `deliv_per_s` is aggregate.
  The contention signature is **aggregate plateauing while per-thread collapses ~1/T** —
  the single refcount cacheline saturating. (A 24-core run: aggregate holds ~30–56 M
  ops/s from T=2 up, while per-thread falls 90 M → 0.25 M and per-op climbs 11 ns → ~4 µs.)
- **`bench_await_wakeup_storm`** runs one writer storming writes while W threads `await`
  the same vertex, in steady state. `pub_per_s` is writer throughput (falls as `notify_all`
  + the contended vertex lock get costlier — a 24-core run: ~2.1 M writes/s at W=1 down to
  ~39 k at W=128, i.e. 0.47 µs → ~26 µs per write); `deliv_per_s` is aggregate wakeups/s.
  Steady-state throughput is used over single-shot latency so no fragile "all W parked"
  barrier is needed — the bench is not flaky.

## What is measured

The swept axes (`bench_common.hpp`): payload **1..8192 B**, fan-out
**1/8/128/1024/8192** subscribers per endpoint, endpoints **1..8192** topics. To
keep the comparison fair and the wall-clock bounded, each run targets a roughly
constant number of *deliveries* (high fan-out does proportionally fewer publishes).

Module compositions are surfaced as distinct `mode`s — "different approaches to
craft libtracer":

| mode | what it exercises |
| --- | --- |
| `inproc` | **write (store+notify+deliver)**: `write` → subscriber callback. Per message this pays one segment alloc + one memcpy (the owned view) + one LKV `make_shared` store + the await/readiness sequence bump, then delivers. *Not* zero-copy per message — that is `inproc-borrow`. |
| `inproc-deliver` | **deliver-only (`propagate`)**: the value is stored once, then each op is `graph_t::propagate(v)` — deliver the current LKV to the subscribers, no per-op store/alloc/copy. The semantic analogue of Zenoh's transient put (RFC-0008 edge transition). |
| `inproc-borrow` | the **zero-copy loaned path** — a borrowed view, *zero alloc, zero copy* (a refcount handoff). libtracer-only semantics: Zenoh's matched rows use its copying put. |
| `inproc-path` | write **by path** (registry hash + lookup per publish) — the "many topics" cost. |
| `loopback` | the M4 bridge: encode + ROUTER-wrap + cross-thread queue + decode. |
| `mixed` | 128 topics, varied fan-out + payloads. |
| `net` | two processes over real UDP (`run_net.sh`). |
| `scatter` | composite rope shipped in one `sendmsg(iovec)` (`bench_scatter`). |
| `eptype-lean` | ep-type axis: minimal sink (see below). |
| `eptype-lean-cached` | ep-type axis: loaned / `out_cache` read (see below). |
| `eptype-stream` | ep-type axis: `STREAM`-role vertex (see below). |
| `routers-h{1,2,4,8}` | n-routers axis: end-to-end delivery across `H` bridge/ROUTER hops (see below). |
| `fold-n1` … `fold-n8` | n-layer-folded axis: same total bytes folded across N segments (see below). |

### ep-type (endpoint-dispatch-class) axis (#96 / ADR-0032)

A fourth axis over the *dispatch class* a write takes to an endpoint, measured on one
fixed workload (**64 B, fan-out 1, 1 endpoint**) so the three classes are directly
comparable. Each emits a `RESULT` line whose `mode` names the class (same 12-field
shape, so `collate.py` / `perf_gate.py` still parse). **The names are provisional**
(the class boundaries are what matter); the map to the underlying paths is:

| ep-type | maps to | what it exercises |
| --- | --- | --- |
| `eptype-lean` | minimal sink (`inproc`) | plain write+deliver to a `STORED_VALUE` vertex, heap view per publish. |
| `eptype-lean-cached` | loaned / `out_cache` (`inproc-borrow`) | the zero-alloc loaned read path — a borrowed view (zero alloc, zero copy). |
| `eptype-stream` | `STREAM` role | each write appends to the bounded history ring (retention work) *then* fans out — strictly more work than lean. |

Expected cost ordering: **lean-cached** (fastest / zero-alloc) < **lean** < **stream**
(heaviest — pays history retention on every write). lean / lean-cached reuse the
existing `inproc` / `inproc-borrow` code paths, re-emitted under the `eptype-*` tag (the
original lines still print).

### n-routers (bridge-hop) axis (#96 / ADR-0032)

A fifth axis over **how far a frame travels**: the cross-node fan cost as a frame
traverses `H` bridge/ROUTER **hops**. Where the others stay in one process's graph, this
sweep builds a real **chain of `H` bridges** wired by the in-process loopback transport
(`bench_libtracer.cpp::run_routers`) and measures end-to-end delivery (publish at the
head, receive at the tail):

```
node[0] --ch[0]--> node[1] --ch[1]--> ... --ch[H-1]--> node[H]
```

`H+1` nodes (each its own `graph_t` with one `/bench/chain` vertex), `H` loopback
channels, `2H` `bridge_t`s. node[k]'s **egress** bridge `export_vertex`-es `/bench/chain`
onto `ch[k].a()`; node[k+1]'s **ingress** bridge `set_mount`s `ch[k].b()` back onto its
own `/bench/chain`. So an intermediate node **re-wraps on egress what it just unwrapped
on ingress** — a fresh hop per channel. **Every hop pays the full cross-node cost**:
`router_wrap` (egress) + `router_unwrap` + **recent-set dedup** (left enabled, default
capacity) + ROUTER strip (ingress), plus one cross-`wire` thread handoff. `H ∈ {1, 2, 4,
8}`, all well under `kMaxHops` (32). One `RESULT` line per `H`, `mode = routers-h<H>`,
`size=64`, `fan=1`, `ep=H` (the hop count) — same 12-field shape, so `collate.py` /
`perf_gate.py` still parse.

**Expectation: end-to-end latency rises roughly *linearly* with hop count** — each added
hop is one more `wrap` + `unwrap` + dedup-probe + cross-thread wakeup on the critical
path — and aggregate throughput falls as the pipeline lengthens. This is an in-process
loopback chain (deterministic, no socket flakiness); it isolates the **per-hop
bridge/ROUTER cost**, not network latency (that is the `net` harness). Because each
loopback channel runs a receive thread, the chain is built + torn down with the
`bridge_test.cpp` lifecycle discipline — `channel.shutdown()` joins every receive thread
before the bridges it dispatches into are destroyed (clean under the CI TSan / ASan jobs).
### n-layer-folded (fold-depth) axis (#96 / ADR-0032)

The **last** axis, over the L0/L1 zero-copy *composition* itself (ADR-0016): how does
the cost scale with the **fold depth** — how many memory layers / segments a value is
**folded across**? A `rope_t` is a chain of views over segments, so the "same" value
can live as one flat segment or as a rope of N links. We sweep the fold depth
**N ∈ {1, 2, 4, 8}** while holding the **total bytes constant (512 B)**: at N=1 the value
is one flat 512 B segment, at N=8 it is an 8-link rope of 64 B segments — identical
bytes, different fold depth. Per op we serialize the folded value for egress the way a
transport does — build the scatter-gather descriptor (`rope_t::to_iovec`, spans into the
N segments, no copy) and walk it. Because only the fold depth varies, the delta isolates
the **view-chain walk / scatter-gather** cost.

| mode | fold depth | what it exercises |
| --- | --- | --- |
| `fold-n1` | 1 (flat) | a single contiguous segment — one link to walk. |
| `fold-n2` | 2 | a 2-link rope of the same total bytes. |
| `fold-n4` | 4 | a 4-link rope of the same total bytes. |
| `fold-n8` | 8 | an 8-link rope of the same total bytes — most scatter-gather work. |

Expected trend: cost **rises** with fold depth — more folds ⇒ more links to gather/walk
⇒ higher per-op time and lower throughput, even though the byte count is fixed. (This is
the cost the rope *trades for* zero-copy composition; the win is that those bytes are
never copied — see `scatter` for the egress payoff.) **The naming "n-layer-folded" /
"fold depth" is provisional** (the depth sweep is what matters); each line keeps the
12-field shape so `collate.py` / `perf_gate.py` still parse. `size` carries the constant
total bytes (512); `fan` and `ep` are 1.

- **Throughput** — back-to-back publishes; `deliveries / elapsed`.
- **Latency** — one publish at a time (publish, wait for receipt, repeat); p50/p99/mean.
- Both sides build at **`-O3`** (Release) for parity; the app payload size (not the
  on-wire envelope) is used for MB/s.

## Results

The comparison is **generated in CI on every docs build** and published — with
interactive **absolute-value** charts (throughput / latency / bandwidth vs fan-out,
payload, and topic count, libtracer and zenoh-c as two series on shared axes) — on the
**[Performance page](https://avatarsd-llc.github.io/libtracer/docs/performance.html)**. No
numbers or figures are committed here on purpose: absolute values are runner-dependent,
so the only honest snapshot is the one CI just measured, stamped with its commit + runner.
Run `./grid.sh` to reproduce the same charts locally in `preview.html`.

## How to read the numbers

- **Absolute, not ratios.** Both engines are measured in the same pass on the same
  runner, so the two curves are directly comparable; you read the real numbers off the
  axes rather than trusting a single speed-up figure. Shared-runner variance is real —
  read trends and orders of magnitude, not the third digit.
- **In-process, what each mode actually does:** the charted `inproc` row is libtracer's
  `write` — per message it heap-allocates one segment, memcpys the payload into it,
  stores it as the last-known-value (one `make_shared`), bumps the await/readiness
  sequence, then delivers. It is **not** a zero-byte publish. The zero-copy claims are
  the *other* modes, each measured separately: `inproc-borrow` hands off a borrowed
  view (refcount only, zero alloc / zero copy), and fan-out beyond 1 clones views by
  refcount (subscribers share the segment — no per-subscriber copy). `inproc-deliver`
  (`propagate`) moves no bytes per op because the value is already stored — the
  semantic match for Zenoh's transient put, which runs its full sample machinery but
  neither persists the value nor bumps a readiness sequence. The differences show up
  most on the **fan-out** axis; on the **topic-count** axis the engines are close —
  the charts show exactly where each holds.
- **Network transports:** CI also publishes a **UDP** and **TCP** comparison — a separate
  publisher and subscriber **process** per engine over the real loopback path (the same
  two-process topology for both, so it is fair). **WebSocket** is built but held out of the
  published charts: libtracer's WS transport shows order-of-magnitude single-run latency
  jitter under this bench. **QUIC** needs the `-DLIBTRACER_WITH_QUIC` module (msquic + TLS).
- **Caveats:** single machine; absolute numbers are representative of the runner.
