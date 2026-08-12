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
| `run_compose.sh` | **composition throughput**: values a *subscriber process* observes per second as the composition width K grows — libtracer ships a K-link rope as one `sendmsg(iovec)`, an engine with no composite send needs K messages. Two processes on both sides, deliveries counted at the receiver, publisher audited for wire use — see [what it measures](#run_composesh--composition-throughput-two-processes-both-sides). |
| `grid.sh` | **response-surface grid** for both engines → a self-contained `preview.html` of the absolute-value comparison charts (the same ones CI publishes, via `render_compare.py`). |
| `bench_forward_heap` | **16KB-RAM zero-heap gate**: a global `operator new` counter measures how many heap allocations one FWD *forward hop* costs (ADR-0038). Drives a **stub** link — see [what it does not cover](#bench_forward_heap--the-16kb-ram-zero-heap-forward-gate-adr-0038). |
| `bench_transport_iov` | **the term the zero-heap gate cannot see**: at what scatter-gather width does a *real* transport start allocating per frame? Measured spill: 17 caller spans / ~288 B, against a structural `kFwdMaxIov` of 9. |
| `bench_fanout_clone_storm` | **many-core refcount contention**: T threads clone+release one shared segment — the per-subscriber fan-out primitive under wide fan-out (ADR-0032 128-core row). |
| `bench_await_wakeup_storm` | **many-core await fan-in**: one writer storms writes while W threads `await` one vertex — condvar/notify_all + vertex-lock scaling (ADR-0032 128-core row). |
| `bench_route_handle_contention` | **per-link-lock contention**: T producers doing steady-state `ensure_egress` reuse-reads on one hot link (its header carries the finding that `shared_mutex` does not help). |
| `bench_ram_census_tcp` | **whole-node RAM census**: the heap a 100-vertex graph (values 4..64 B, mixed int / array / STREAM) holds, staged from an empty `graph_t` through a `/net:children[]`-created TCP listener to steady state under a real remote peer **process**. |
| `bench_rx_source_topology` | **RX failable-source topology**: T receive threads forwarding rope frames through a shared heap, one shared pool, or one pool per child — the measurement behind [ADR-0067](../docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md) §3. |

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
(`perf.yml`, `ZEROHEAP_MAX=0`). What that buys, precisely: on a **contiguous (single-link)**
frame the hop's *own* work allocates nothing — offset dispatch (no `wire::decode`), fixed
stack header buffers + a stack `iov` array (no `std::vector`), views straight into the
untouched inbound frame.

**What the gate does NOT cover.** It drives `capture_transport_t`, a stub link that only sums
the span sizes it is handed (`bench_forward_heap.cpp:8-14`). Two terms sit outside the armed
window, so **"the forward hop is heap-free by construction" is false as stated**:

- **The shipping transports' `::iovec` table.** `transport_udp.cpp` (`kMaxInlineIov = 16`) and
  `transport_tcp.cpp` (`prefixed_iov_t::kMaxInlineIov = 16`, `+1` for the length prefix) both
  spill to the heap above a fixed inline width. `bench_transport_iov` measures the boundary at
  **17 caller spans / ~288 B**; headroom from the structural `kFwdMaxIov` (9) is 8 regions, and
  a rope source may split any region further. The stub never runs that code, so `allocs=0` says
  nothing about it (`core/src/transport_tcp.cpp:51-55`).
- **The multi-link rope arm.** A rope source's sub-span count is the sender's choice and is
  known only at run time, so that arm gathers into a `mem::block_array_t` drawn from the
  injected receive source (`core/src/fwd_router.cpp:1430`). Nothrow (ADR-0065 — exhaustion
  drops the frame rather than aborting), but **not** allocation-free.

Read `bench_forward_heap` and `bench_transport_iov` together; neither is sufficient alone.

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

### `run_compose.sh` — composition throughput, two processes both sides

The claim: libtracer batches by **composition**, not by a timer. A composite endpoint's
value is a **K-link rope already in memory**, and the transport lowers that rope to its
native scatter-gather form — one `sendmsg(iovec)` carrying K values. An engine with no
composite send issues K messages for the same K values. So the separating quantity is
**values delivered per message**, and the prediction is that libtracer's per-value cost
stays flat as K grows.

That claim was published once on a harness that could not test it, and the numbers were
withdrawn ([#568](https://github.com/avatarsd-llc/libtracer/issues/568)): the Zenoh side
declared a publisher with **no subscriber and no peer**, so `put()` never reached the wire
— 5 `sendto` and 10 `write` for 520 000 puts, and the five were multicast scouting beacons
— while the libtracer side reported a `sendmsg` rate multiplied by K, egress-only, with
nothing counting deliveries. Both K-curves were computed, not measured.

**What this harness measures.** The number of values a **separate subscriber process**
observed arriving over a real loopback UDP socket, per second, at each K; the number of
messages those values arrived in; and the one-way latency of a whole K-value group
(stamped in the group's first record, taken when its last lands, so a per-value engine is
timed on when its group *completed*, not on its first value). Every published figure is
the receiver's own count over the receiver's own clock. The publisher's send count is
captured only to report **loss**.

**What it does not measure.** Any in-process graph cost — there is no `graph_t`, no vertex
and no subscription on either side; this is the transport seam and the kernel path, which
is where the one-datagram-per-K property lives. Nor what a composite costs to *build*: the
K records and the iovec over them are constructed **once, before the timed loop**, because
the value is supposed to be a rope that already exists. (The withdrawn bench rebuilt its
iovec inside the timed loop and understated libtracer by 33–58% — in libtracer's own
disfavour, and still not a measurement of what it claimed.) And it does not measure a
receiver fanning K values out to K consumers: the subscriber walks the datagram's records
and counts them.

**What each timed loop still allocates.** The harness does *not* claim an allocation-free
timed loop — an earlier revision of this section did, and it was wrong on both arms. What
it claims is that no allocation left in either loop is one the *harness* added:

* libtracer's arm gathers into a stack `::iovec` array up to `kMaxInlineIov = 16`
  (`core/include/libtracer/iov_table.hpp`) and takes **one nothrow heap block per
  datagram** above it. Counted with a replaced global `operator new` over 1 000 sends per
  width: K = 1, 8, 16 → **0 per send**; K = 17, 64, 256 → **exactly 1 per send**. Of the
  four default widths (`1 8 64 256`) that is `64` and `256` paying it. The spill is inside
  `udp_transport_t::send`, it is on the shipping forward path too, `bench_transport_iov`
  is the in-tree instrument that located the boundary, and it is deliberately left visible.
* the per-value arm must not pay a **staging copy** the composite arm does not:
  `Bytes(const std::vector<uint8_t>&)` selects `z_bytes_copy_from_buf`, which the vendored
  API documents as converting "by copying" — inside the blast loop that is every payload
  byte copied per put, harness overhead on one side of a comparison.
  `bench_zenoh_compose.cpp` therefore aliases the staging buffer into the payload with the
  documented non-copying twin (`z_bytes_from_buf`, NULL deleter) instead. That difference
  is the vendored API's contract, not something measured here; whatever an engine then
  allocates internally is its own cost and this harness makes no claim about it.

**The sample budget.** `LIBTRACER_BENCH_COMPOSE_VALUES` is a *value* budget, so the
datagram count `VALUES / K` falls by K — at the default that is 400 000 datagrams at K=1
but 1 562 at K=256, and K=256 is exactly where "per-value cost stays flat" gets read off.
The driver therefore floors the group count at `COMPOSE_MIN_GROUPS` (default 20 000), and
hands the receiver a floor of a quarter of that to enforce on what it actually *observed*
(the quarter is loss headroom; loss itself is reported separately). The floor is a
datagram count, not a duration: how long they take is a property of the host.

**Two guards, and a number is published only if both hold.**

1. **The receiver refuses to report.** A point that observed no values, no throughput
   window, any malformed record, or fewer throughput datagrams than the sample floor emits
   **no `RESULT` row at all** and exits non-zero. A record carries a magic word, its own K,
   and the driver's **per-point nonce** — the nonce being the part that matters on a shared
   host, because the magic is a compile-time constant and the K is the swept parameter, so
   without it a *concurrent run of this same harness at the same K* on the same
   deterministic port walk would have been folded into the rate rather than rejected.
2. **The wire-use audit** (`syscall_guard.py`) runs a short pass of the same pub/sub pair
   with the publisher under `strace -c`, and fails the point if it issued fewer
   send-family syscalls than `COMPOSE_SEND_FLOOR` (default **50**). Both ends of that
   default are measured, not guessed: a **peerless** publisher — the original defect,
   reproduced — issued **3 `sendto` / 8 `write` for 4 000 puts**, while a publisher with a
   live subscriber **batches**, turning 3 200 puts into **479** `sendto`. So the floor
   cannot be "one send per message" without failing honest batching, and `write` cannot
   count toward it at all, because in a `-c` summary a socket write and a `printf` are the
   same row. The audit is its own pass because tracing costs tens of microseconds per
   syscall; the measured pass is untraced. No `strace`, no ptrace permission, or an
   unparseable summary all **fail** — a guard that downgrades itself to "not checked" is
   the defect it exists to prevent, wearing a passing badge.

```sh
bash bench/fetch_zenoh.sh          # the comparison arm; libtracer's arm needs nothing
./run_compose.sh ./build           # RESULT / RESULT_COMPOSE / SYSCALL_AUDIT / COMPOSE_LOSS rows
python3 test_compose_guard.py      # the wire-use guard's own tests, both directions
./build/test_compose_record        # the receiver's guards: the nonce and the sample floor
```

**Nothing here is charted.** Re-adding the comparison chart is
[#568](https://github.com/avatarsd-llc/libtracer/issues/568)'s fifth criterion and is gated
on maintainer sign-off of the published number, so this script prints rows and stops.

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

### `bench_ram_census_tcp` — what a 100-vertex node costs over TCP

```sh
cmake --build bench/build --target bench_ram_census_tcp -j
./bench/build/bench_ram_census_tcp                    # RESULT arm=<mix> metric=<stage>
./bench/build/bench_ram_census_tcp --reps=9 --ops=5000 --arm=mixed
```

`bench_conn_ram` prices one **connection**; this prices the **node** around it. A counting
`operator new` (all variants — sized, array, aligned, **nothrow**) reads the LIVE heap
balance at five points: an empty `graph_t`, +100 vertices registered, +their values written
once, +the `fwd_router_t` / `transport_vertex_t` / SPEC-created TCP listener, +a peer
**process** connected over loopback, and +steady state after N mixed FWD read/write ops from
that peer. The listener is created the way the wire creates one — `write /net:children[] +=
SPEC{listener, kind=tcp, port}` — so no byte is counted on a hand-wired path.

Three arms differing in exactly one variable (every value 4 B, the 4..64 B mix, every value
64 B) give the value-size sensitivity. A NULL arm is printed with the rest and must read 0;
the first repetition is discarded. The figures are near-deterministic — a spread wider than a
few bytes means an unwaited thread or a broken instrument, not noise.

Diagnostic, **not** a `perf.yml` gate, and deliberately **not** wired into the generated
performance page. It does not count pthread stacks (`mmap`ed, invisible to any `operator new`
counter) or static RAM — the footprint sentinel prices those.

### `bench_rx_source_topology` — where a bounded RX source may sit (ADR-0067 §3)

```sh
cmake --build bench/build --target bench_rx_source_topology -j
./bench/build/bench_rx_source_topology   # RESULT mode=rx_source_<topology>, fanout=T threads
```

ADR-0067 §3 rules that a `pool_source_t` on a per-frame path is owned by **one** thread.
This bench is the evidence for that rule at the seam it governs: T receive threads each
drive a multi-link rope through their own inbound child to their own egress sink — so the
source is the only object two threads can contend on — under three wirings. The two pool
configurations are given **equal total slab**, so what is compared is topology, not budget.

The signature to look for is the shared pool's per-thread rate collapsing while the heap
and the per-child pool both scale. On a 12-core / 24-thread host the shared pool falls to
**~1/67** of its own single-thread rate by T=24 (244 ns → 16.4 µs per forward) while
per-child tracks the heap within run-to-run spread. Peak slab is reported on stderr:
**128 B per child, one size class**, independent of T.

Diagnostic, **not** a `perf.yml` gate, for the same reason as the two storms above. Run it
at least three times before drawing a conclusion — a single run of this workload showed a
27 % single-thread pool win that three runs deleted.

## What is measured

The swept axes (`bench_common.hpp`): payload **1..8192 B**, fan-out
**1/8/128/1024/8192** subscribers per endpoint, endpoints **1..8192** topics. To
keep the comparison fair and the wall-clock bounded, each run targets a roughly
constant number of *deliveries* (high fan-out does proportionally fewer publishes).

The `inproc` fan-out sweep additionally runs the **mid arms 16/32/64/256/512**
(`kFanoutsMid`, #844), appended after every other row. A publish costs about
`fixed + F x marginal` (~118 ns + ~16 ns per delivery on this host), and two arms already
determine a straight line — so extra arms pay for themselves only where the line KINKS.
The dispatch path kinks at **`vertex_t::kInlineFanout` (8)**: fan-8 is the last width that
snapshots into the raw stack buffer and fan-9 is the first that spills to the overflow
vector. The coarse ladder samples that boundary from below and then jumps 16x past it, and
a cost paid only on the overflow path decays as 1/F — so it is **largest at the first width
past the boundary and already amortized toward noise by fan-128**.

Measured, against a build with the per-publish overflow allocation deliberately
reintroduced (#844's non-vacuity check — 15 interleaved pairs, alternating start, pinned;
an A/A null in the same window spans 0.955x..1.041x on the mean with coin-flip pair counts):

| fan | 1 | 8 | **16** | **32** | 64 | 128 | 256 | 512 | 1024 | 8192 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| mean | 0.980x | 1.003x | **1.101x** | **1.068x** | 1.011x | 0.974x | 0.992x | 1.002x | 0.968x | 0.956x |
| 1/throughput | 0.966x | 1.010x | **1.077x** | **1.070x** | 1.010x | 0.993x | 0.989x | 1.020x | 0.970x | 0.970x |
| pairs worse (mean) | 3/15 | 4/15 | **12/15** | **11/15** | 9/15 | 6/15 | 5/15 | 7/15 | 3/15 | 2/15 |

Both instruments move together at 16 and 32 and a majority of pairs breach on their own;
every arm the coarse ladder already had reads inside the A/A null with a coin-flip pair
count. **Fan-16 and fan-32 are the arms that resolve this shape — fan-64 does not** (+1.1 %,
9/15), which is worth recording because a single fan-64 point was the original proposal.
The other three arms are included on a stated rationale rather than a measured one: they
make the 8 -> 128 and 128 -> 1024 octaves resolvable at <= 2x steps, so a wide-band step can
be *located* instead of only detected — #841's regression was a bytes-streamed defect in the
band where `F * sizeof(edge_view_t)` outgrows L1, and the gate saw it as one number at
fan-1024 with no neighbour to place it against.

The arms are **charted, not gated**. An 8-10 % step is inside the gate's noise-calibrated
thresholds (+15 % p50 / +12 % mean / -12 % deliveries) and the run above never produced
disjoint [min..max] ranges, so a gated arm here would add a false-fail surface without
adding detection. `bench_libtracer fan` runs the whole ladder (coarse + mid, ascending) in
about 1.7 s, which is the mode to A/B anything that touches dispatch width.

### Selecting a sweep (`bench_libtracer [mode]`)

Run with **no argument** for the full default sweep — that is what CI, `perf_gate.py` and
the gh-pages history store consume, and its row set is the one nothing may reorder. A
single recognised mode name runs that sweep **alone**, announcing itself on stderr as
`MODE <name>` before its first row so an A/B driver can assert it got the arm it asked
for. Anything else is **refused**: the binary prints the mode list to stderr and exits
non-zero without emitting a `RESULT` row.

That refusal is a measurement guard, not a courtesy (#1040). An unrecognised argument used
to fall through into the default sweep and exit 0 — so in an A/B against a binary built
from a commit that predates a mode, the candidate ran one axis for seconds while the
baseline ran every axis for minutes, both emitting well-formed rows under the same
`(mode, size, fan, ep)` keys for the harness to join on. A typo (`taget`) did the same.
`bench/test_bench_cli.py` pins it.

Module compositions are surfaced as distinct `mode`s — "different approaches to
craft libtracer":

| mode | what it exercises |
| --- | --- |
| `inproc` | **write (store+notify+deliver)**: `write` → subscriber callback. Per message this pays one segment alloc + one memcpy (the owned view) + one LKV `make_shared` store + the await/readiness sequence bump, then delivers. *Not* zero-copy per message — that is `inproc-borrow`. |
| `inproc-deliver` | **deliver-only (`propagate`)**: the value is stored once, then each op is `graph_t::propagate(v)` — deliver the current LKV to the subscribers, no per-op store/alloc/copy. The semantic analogue of Zenoh's transient put (RFC-0008 edge transition). |
| `inproc-borrow` | the **zero-copy loaned path** — a borrowed view, *zero alloc, zero copy* (a refcount handoff). libtracer-only semantics: Zenoh's matched rows use its copying put. |
| `inproc-path` | write **by path** (registry hash + lookup per publish) — the "many topics" cost. |
| `inproc-target-stored` / `inproc-target-handler` | the **path-target dispatch leg** — edges carrying a `target_key` (what a wire `SUBSCRIBER` produces) instead of a callback. Each delivery resolves the target in the registry, passes the fan-in ACL gate, clones the rope nothrow, then applies the target's own write effects: **~10x a callback edge** at fan-out. `stored` lands in a `STORED_VALUE`'s LKV, `handler` in a `HANDLER`'s `on_write`; the gap between them is the target's store. Every row above subscribes by callback, so read these against `inproc` at the same fan-out. `bench_libtracer target` runs the pair alone. |
| `inproc-pool` / `inproc-pool-borrow` | the same 1:1 write, but through an **injected `std::pmr::unsynchronized_pool_resource`** as `mr_` instead of the global heap — the pooled-vs-heap LKV persist, swept over payload at fan=1. Measured on this host the pool is ~19 ns **slower** (~104 vs ~85 ns/op): it is a determinism / bounded-ceiling lever, not a latency one, and the inversion flips only on an MCU allocator. Gated by the `lkv` same-run throughput ratio (`perf_gate.py lkv_ratio_gate`), not by a latency percentile. |
| `inproc-mt{1,2,4,8}` | **n-cores (parallel-dispatch) axis**: T threads, each driving its *own* `graph_t` + endpoint over the zero-copy in-process path, measured for aggregate throughput + per-op latency under load. Each thread reuses one borrowed view over a stable per-thread buffer, so the timed loop allocates nothing — what scales is dispatch itself. Thread counts are clamped to `hardware_concurrency()`, so a small runner emits fewer rows. `ep` carries the thread count. |
| `acl-inherit-d4` / `acl-inherit-d4-mt4` | ACL-gated reads with inheritance (ADR-0050 cached effective-ACE merge) at depth 4 — the uncontended gate cost, and the shared-ancestor contended case at 4 threads. |
| `mixed` | 128 topics, varied fan-out + payloads. |
| `net` | two processes over real UDP (`run_net.sh`). |
| `eptype-lean` | ep-type axis: minimal sink (see below). |
| `eptype-lean-cached` | ep-type axis: loaned / `out_cache` read (see below). |
| `eptype-stream` | ep-type axis: `STREAM`-role vertex (see below). |
| `fold-b1` … `fold-b8` | n-layer-folded axis: same total bytes folded across N segments (see below). |

> **Retired: `loopback` and `routers-h{1,2,4,8}`.** Both benchmarked the ROUTER-flood
> `bridge_t` envelope, deleted with the bridge itself in
> [ADR-0040](../docs/adr/0040-net-plane-is-explicit-source-routed-only.md) — the net plane is
> explicit-source-routed `FWD` only. Neither mode is emitted today
> (`bench_libtracer.cpp:16`, `:1235`); `bridge_t`, `router_wrap`, `router_unwrap`, `kMaxHops`,
> `export_vertex` and `run_routers` survive in `core/` and `bench/` only inside comments
> and `core/CHANGELOG.md`'s record of their removal — not one declaration, definition or
> call of any of them is left (`grep -rn` over both trees, 2026-08-08), and the
> two-process `bench_libtracer_net` was retired with them (`bench/CMakeLists.txt:30`). FWD
> forward cost is now measured by `bench_forward_heap` + `bench_transport_iov` + the `fwd_*`
> tests; multi-hop end-to-end delivery is the `net` harness.

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
| `fold-b1` | 1 (flat) | a single contiguous segment — one link to walk. |
| `fold-b2` | 2 | a 2-link rope of the same total bytes. |
| `fold-b4` | 4 | a 4-link rope of the same total bytes. |
| `fold-b8` | 8 | an 8-link rope of the same total bytes — most scatter-gather work. |

Expected trend: cost **rises** with fold depth — more folds ⇒ more links to gather/walk
⇒ higher per-op time and lower throughput, even though the byte count is fixed. (This is
the cost the rope *trades for* zero-copy composition; the win is that those bytes are
never copied.) **The naming "n-layer-folded" /
"fold depth" is provisional** (the depth sweep is what matters); each line keeps the
12-field shape so `collate.py` / `perf_gate.py` still parse. `size` carries the constant
total bytes (512); `fan` and `ep` are 1.

The `b` in `fold-b*` is **batch-amortized**, and the rename was not cosmetic. The rows
were `fold-n*` and timed ONE ~11 ns operation between two `steady_clock` reads, so all
four widths published `p50=30` — the chart was four identical flat lines and the fold
cost it exists to show was invisible. The batch form resolves them (~1 / 2 / 4 / 6 ns).
The old series were ended rather than continued, because the number means something
different now.

- **Throughput** — back-to-back publishes; `deliveries / elapsed`.
- **Latency** — one publish at a time (publish, wait for receipt, repeat); p50/p99/mean.
- libtracer builds at **`-O3`** (Release); Zenoh is the upstream **prebuilt** zenoh-c
  release binary, so "both at `-O3`" was never quite true — both are optimized builds
  measured in one pass, which is what parity actually rests on. The app payload size (not the
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
