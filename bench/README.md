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

> **Never commit a bench build tree.** Everything below builds in-tree, and PR #1356
> managed to commit 34 compiled ELFs (~49 MB) from one. The `no-binaries` CI job
> (`tools/check_no_binaries.py`) now fails any PR whose diff adds binary content, with the
> conformance vectors as the sole exemption; run it locally before pushing if you have
> built here.

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
| `bench_conn_ram` | **per-connection RAM census**: what one connection costs on each transport (TCP / WS / UDP / CAN) — link base, per-connection bytes, what survives teardown. **Gated (warn)** on the pinned host — see [the RAM censuses in CI](#the-ram-censuses-in-ci-1228). |
| `bench_ram_census_tcp` | **whole-node RAM census**: the heap a 100-vertex graph (values 4..64 B, mixed int / array / STREAM) holds, staged from an empty `graph_t` through a `/net:children[]`-created TCP listener to steady state under a real remote peer **process**. **Trend-only** on the pinned host. |
| `bench_path_label` | **RFC-0027 §12.4's normative gate**: what a minted **path label** saves per hop, as a slope over hop count and over registry width, and what it saves on the TERMINUS RESIDUAL against address depth — clause 2's axis, the one §3.3 nominates as deciding. See [the RFC-0027 gate](#bench_path_label--the-rfc-0027-124-gate-1325-car-5). |
| `bench_forward_rope` | **the forward hop over a MULTI-LINK ROPE** (`on_frame_rope`, ADR-0053 §5), swept over link count — the only bench that instantiates `rope_cursor`, the specialisation `symbol_ratchet.json` pins. Diagnostic, not gated; quote it only off a quiescent host — see [its A/A null](#bench_forward_rope--the-rope-forward-hop-and-what-its-aa-null-is-worth-1358). |
| `bench_rx_source_topology` | **RX failable-source topology**: T receive threads forwarding rope frames through a shared heap, one shared pool, or one pool per child — the measurement behind [ADR-0067](../docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md) §3. |
| `bench_pin_ratio` (`run_pin_ratio.sh`) | **RFC-0022 §6, the LATENCY half**: the WRITE store leg over a (payload × segment) grid, every `K` arm rotating inside ONE process. Answered §8 Q3 → Amendment 2 (the on-by-default flip does not land). Collated by `collate_pin.py`. |
| `bench_pin_net` (`run_pin_net.sh`) | **RFC-0022 §6, the RAM half**: two processes over real UDP, deliveries counted by the RECEIVER, receiver backed by a **bounded RX pool** whose free-slot floor and `dropped_rx()` are the occupancy instrument. Sweeps the live-vertex count across the slot count — the axis on which a pin's borrow starves receive. See [receive-pool occupancy](#bench_pin_net--rfc-0022-6-receive-pool-occupancy-760). |
| `bench_subscribe_index` (`run_subscribe_index.sh`) | **#1266's subscriber-index interning A/B**: what keying `link_index_` by an interned token instead of a link NAME costs and saves, in latency and in bytes at rest, over 4/8/16/32/65 links. Four index arms in one binary plus the live `subscribe_wire` path, and the A/A null is collected in the same window. Collated by `collate_subscribe_index.py`. See [what it resolves](#bench_subscribe_index--link-identity-interning-1266). |

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
  injected receive source (`core/src/fwd_router.cpp:2231`). Nothrow (ADR-0065 — exhaustion
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

Not a `perf.yml` gate: it runs on the **pinned host** (`perf-local.yml`), where it is
**trend-only** — see below. It does not count pthread stacks (`mmap`ed, invisible to any
`operator new` counter) or static RAM — the footprint sentinel prices those.

### The RAM censuses in CI (#1228)

Steady-state RAM is the axis that decides whether a node fits the 16 KB target, and it used
to be the only cost axis measured **exclusively by hand** — latency, allocations, code size
and flash all had a gate or a recorded trend. Both censuses now run on every main push in
[`perf-local.yml`](../.github/workflows/perf-local.yml), on the fixed pinned host, and
publish into the **same** `dev/bench-local` store on the same commit axis as the latency
trend (`bench/ram_census.py emit` merges them into the file `perf_emit_benchmark.py` just
wrote — one collation and publish pipeline, not two). Series names are
`conn-ram <arm> <metric>` and `node-census <arm> <metric>`, in bytes (or `blocks` for the
allocator block counts); a row whose median is **0** is not recorded, because the store
trends a ratio and a ratio against zero can never alert.

| instrument | in CI | why |
| --- | --- | --- |
| `bench_conn_ram` (per-connection) | published **+ gated (warn)** | its per-connection columns are byte-stable run to run |
| `bench_ram_census_tcp` (whole node) | published, **trend-only** | it gains a ratchet only after the per-connection one has proven quiet |

**Trend-only is not gated.** The whole-node census has **no pins at all** — nothing about it
fails or warns today. It forks and reaps its own remote peer process, and a peer that fails
to come up costs that commit its census point with a `::notice::`, never the job.

**The warn-first ratchet.** `bench/ram_census.py gate` compares the per-connection figures
against **measured** pins in [`ram_census_pins.json`](ram_census_pins.json) — the same shape
as the flash-footprint drift check: a pin, a band, a warning annotation, and a re-pin that
only ever happens in a PR. It is never auto-ratcheted. The band is **±8 B or ±0.5%,
whichever is larger**, and that comes from measurement, not from taste: over 4 process
invocations × 9 repetitions on a quiet host every pinned row's median was **byte-identical**
(0 B, 0.00% cross-run) and the widest within-run `[min..max]` on any pinned row was **4 B**
(`tcp-server per_conn`, 280..284). 8 B is 2× that worst observed spread — the same
2×-observed-noise rule the Cortex-M0 drift check adopted. The 0.5% term exists only for the
two ~64 KB `link_base` rows, where an allocator/libc packaging change can move the overhead
term without any design regression. The **high-water** columns are deliberately unpinned:
`tcp-server hw_peak` moved 66% across runs and ~41 KB within one, so a band on it would fire
on a transient buffer rather than on a connection's cost.

A **failure to measure is not a budget verdict** (#982's precedent): a missing or unparsable
transcript, a drifting NULL arm, or a pinned row the run does not produce fails the gate in
**either** mode. And because the warn path is invisible by construction, every pinned-host
run re-proves it with a doctored baseline — if a 4 KB induced growth does not produce a
warning annotation, that step reds the job.

```sh
cmake --build bench/build --target bench_conn_ram -j
./bench/build/bench_conn_ram --no-can --reps=5 | tee conn.txt   # CI's exact invocation
python3 bench/ram_census.py gate --conn-raw conn.txt --pins bench/ram_census_pins.json
```

**Re-pinning** (growth that is understood and accepted, or a shrink — a pin left above the
truth makes the next growth free):

```sh
python3 bench/ram_census.py gate --conn-raw conn.txt \
    --pins bench/ram_census_pins.json --emit-pins > /tmp/pins.json
cp /tmp/pins.json bench/ram_census_pins.json     # then commit it, with the reason, in a PR
```

Take the transcript on the **pinned host** where the gate runs. These figures contain the
`sizeof` of every transport, so they are compiler- and allocator-bound: measured on a
toolchain other than the one recorded in the pin file, the gate still reports drift but
enforces in **warn** mode only — an unattributable difference must never red main.

**Activation.** The flip from `--mode warn` to `--mode fail` in `perf-local.yml` is a
deliberate PR edit, and its criterion lives in the pin file (`activation`), next to the pins
it governs, rather than in a comment that would rot: **20 consecutive pinned-host samples
banked with no unexplained warning, and the pins re-taken on the bench host's own
toolchain**. That is about a release's worth of main pushes here. A threshold picked before
the noise is known is the unreachable-budget failure mode this repo already hit once.

### The perf gate's verdict tiers (#1251)

`perf_gate.py` makes the **same** comparison wherever it runs — same points, same
thresholds, same interleaved A/B rules. What the caller declares is whether a breached
ratchet may **stop the job**:

| tier | a breached ratchet | declared by |
| --- | --- | --- |
| `--tier blocking` | fails the job (exit 1) | `perf.yml` → `gate-pr`, the per-PR gate |
| `--tier advisory` | printed, annotated `::warning::`, exit 0 | `perf.yml` → `ratchet`, the three push replicas |

This lived in a `perf.yml` comment for its whole life before #1251, and the gate had no
tier concept at all — so "the pinned host blocks, the GitHub runners warn" described a
mechanism that did not exist, and a policy that cannot be violated cannot be obeyed either.

**The tier is not a threshold.** An advisory run reports exactly what a blocking run would
have said; it must never quietly measure something weaker, or the two tiers stop being
comparable and the advisory output stops being worth reading.

**Why the push replicas are advisory.** Three replicas each rendered an independent
blocking verdict, so any one of them could red `main` alone — stronger than the ruled
policy, and the shape behind a CI failure later traced to the runner environment rather
than to any commit (#1261). The ruled escalation is **2-of-3 agreement**, which needs an
aggregation job the matrix does not have yet; until it exists a replica *reports*. Nothing
is lost pre-merge: `gate-pr` already gated that commit against the same `main` baseline,
blocking, before it could land.

**The default is `advisory`.** A forgotten flag then under-enforces *loudly* — the breach
is still printed and still annotated — rather than false-failing on a machine whose noise
floor the bar was never calibrated against, which is what an unpinned laptop is. That is
only safe because every CI invocation names its tier and `test_perf_gate.py`'s
`WorkflowsDeclareTheirTier` reds the build if one does not.

**A flagged sample cannot fail a PR.** `--sample-note` takes `host_guard.py bracket`'s
verdict for the sample this run is about; if the sample is flagged `CONTAMINATED`, the
blocking tier refuses to render a verdict on it and downgrades to advisory. That is the
consumer half of the guard's rule — a suspect sample is flagged, never deleted, and the
flag only means something if the gating consumer stops believing it. The predicate is
imported from `host_guard.py`, not re-derived, so writer and reader cannot drift apart.

**The pinned host runs no comparison gate.** `perf-local.yml` records the absolute trend
and invokes `perf_gate.py` nowhere. The seam for making it the blocking tier exists and is
tested; the HEAD-vs-parent leg that would use it does not. Do not read "pinned host =
blocking tier" off any comment until a step in that workflow calls the gate.

### The banked series' own validity (#1301)

`host_guard.py` guards the conditions **one sample** is taken under. `store_guard.py`
guards the **series** those samples form, which is where the #1173 investigation found
the last two ways a regression hides. Both run out of `perf-local.yml`; neither measures
anything, so both are fully testable off a fixture store (`test_store_guard.py`).

```sh
python3 bench/store_guard.py density   --data data.js --shas-file main.txt --max-gap 8
python3 bench/store_guard.py drift     --data data.js --window 20 --threshold 15
python3 bench/store_guard.py toolchain --data data.js
```

**A gap is a measurement defect, and the guarantee bounds it.** Push-to-`main` behind a
`paths:` filter publishes nothing when the run does not happen — filtered out, failed,
skipped by the quiescence guard, or cancelled while *queued* (GitHub keeps one queued run
per concurrency group, and `cancel-in-progress: false` protects only in-progress ones).
Sixteen consecutive merges once banked nothing, which is how the compact-forward +41 %
step became a range rather than a commit. A daily `schedule:` now asks `density` how far
the store lags the bench-relevant history of `main` and measures HEAD when the gap exceeds
**8** merges. What is guaranteed is the **bound on the gap**, not a point per commit: any
regression range stays narrow enough to bisect in three builds.

**A contaminated point is not coverage.** The gap is counted in *trusted* points only. A
sample the A/A pair flagged records what the machine was doing, not what the code cost, so
letting it close the gap would satisfy the guarantee with exactly the points it exists to
distrust — and a density guarantee that raised the sample count while lowering the trust
per sample would buy nothing.

**The rolling comparator sees the staircase the 115 % bar sleeps through.**
benchmark-action compares each point with the one immediately before it, so five merges of
+4 % never trip a 15 % step and the series ends 22 % slower with no alert raised. `drift`
compares the newest trusted point with the **best** of the last 20 trusted points — the
estimator [`docs/methodology.md`](../docs/methodology.md) makes normative, because
contamination is one-sided across points of a fixed-host series for the same reason it is
across rounds of one measurement. Its output prints both figures side by side, so a
staircase reads as a large baseline delta beside a small point-to-point one.

**A best-of-window baseline is a floor, so a breach must clear the whole range.** Measured
against the best alone, ordinary run-to-run spread breaches 15 % constantly: on the real
store, **59** metrics do, and an alert that fires on nothing trains the reader to ignore
it. Requiring the point to fall outside the window's entire `[min..max]` — the same
disjoint-ranges criterion an A/B must satisfy before it may be believed — leaves **2**.
A step-change therefore alerts **once** and then stops, because the step is inside the
range it established.

**Warn-only, and proved live every run.** The drift leg never fails the job; the hard
verdicts stay with `perf.yml`'s interleaved gates. A warn-only comparator is invisible when
it is right and equally invisible when it is broken, so — as with the RAM ratchet above —
each run doctors the newest point by half a step in the *worse* direction for that suite's
sign and fails if no warning comes out.

**The compiler stamp finally has a reader.** `host_guard.py stamp --compiler` has written
the toolchain onto every point since #1301's item 3; `toolchain` reads it back, prints
every recorded transition and how many points predate the stamp, and `drift` marks any
alert whose baseline window crosses a bump as not attributable to code.

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

### `bench_pin_net` — RFC-0022 §6 receive-pool occupancy (#760)

```sh
cmake -S bench -B bench/build -DCMAKE_BUILD_TYPE=Release
cmake --build bench/build --target bench_pin_net -j
python3 bench/host_guard.py wait
VERTEX_SET="1 8 24 32 48" COUNT=200000 WINDOW_MS=6000 \
  bash bench/run_pin_net.sh bench/build 12 512 1024 24 > /tmp/pinnet-512.tsv
VERTEX_SET="1 8 24 32 48" COUNT=200000 WINDOW_MS=6000 \
  bash bench/run_pin_net.sh bench/build 12 64  1024 24 > /tmp/pinnet-64.tsv
python3 bench/collate_pin.py /tmp/pinnet-512.tsv /tmp/pinnet-64.tsv
```

RFC-0022 §6 recorded a hazard it declared out of scope: pinning refcounts the **inbound receive**
segment, so a long-held value holds a receive buffer for as long as it lives, and on a small fixed
pool that is receive capacity gone for the value's lifetime. #760 is that hazard, and the
maintainer ruling gated its fix — copy-on-retain — on this measurement: build the relocate-on-
retain mechanism **iff** realistic workloads starve the pool *even under* the sentinel posture the
library ships.

**Geometry is taken, not invented.** `slot = 1024`, `slots = 24` are the ESP32-C6 profile's own RX
pool constants (`integrations/esp-idf/examples/pin_bench/main/app_main.cpp`, `kSlotBytes`/`kSlots`
— "deliberately small: that is §6's premise"). `pool_t` carves 29 slots from the slab the bench
sizes from them, which is the `pool slots` column. **The vertex count is the RAM axis**: one
STORED_VALUE vertex holds one value, so pinning it holds one slot however large the segment; the
held quantity is `live pinned values × segment_bytes`. `VERTEX_SET` therefore spans the slot count
— below it the pool absorbs the borrow, above it the transport must refuse datagrams.

#### The A/A null, quoted first

`collate_pin.py`'s paired sign test covers only the **store-leg** table; `RESULT_PINNET` carries no
round index, so the net table is unpaired min/med/max and per-round pairing is unavailable on this
leg. The null is therefore an **in-band duplicate arm**: `B2-null` is byte-for-byte `B-sentinel`
(same `K = 0`, same binary, same pool, same transport) rotated through the same interleave, and the
`B-sentinel`-vs-`B2-null` spread per cell *is* the instrument's null.

| verts | payload B | `B-sentinel` med d/s | `B2-null` med d/s | spread | free-slot floor S/N | rx drops S/N |
| ---: | ---: | ---: | ---: | ---: | :---: | :---: |
| 1 | 64 | 107 946 | 106 312 | −1.5 % | 28 / 28 | 0 / 0 |
| 1 | 512 | 103 556 | 98 894 | −4.5 % | 28 / 28 | 0 / 0 |
| 8 | 64 | 108 776 | 109 240 | +0.4 % | 28 / 28 | 0 / 0 |
| 8 | 512 | 109 974 | 101 688 | −7.5 % | 28 / 28 | 0 / 0 |
| 24 | 64 | 99 658 | 105 824 | +6.2 % | 28 / 28 | 0 / 0 |
| 24 | 512 | 109 676 | 99 746 | **−9.1 %** | 28 / 28 | 0 / 0 |
| 32 | 64 | 107 600 | 108 032 | +0.4 % | 28 / 28 | 0 / 0 |
| 32 | 512 | 107 430 | 108 952 | +1.4 % | 28 / 28 | 0 / 0 |
| 48 | 64 | 106 818 | 104 694 | −2.0 % | 28 / 28 | 0 / 0 |
| 48 | 512 | 110 430 | 104 212 | −5.6 % | 28 / 28 | 0 / 0 |

So: **±9.1 % on delivered throughput; exactly 0 on the free-slot floor and 0 on rx drops.** No
throughput delta narrower than 9.1 % may be quoted off this instrument. The RAM axis, by contrast,
has a null of *zero* — twenty identical cells, floor 28 and drops 0 in every one — which is what
makes it the discriminating axis rather than the throughput column.

#### What the run says

12 interleaved rounds × 5 vertex counts × 7 arms × 2 payloads = 840 process pairs, 60 balanced
cells per arm, on a quiescent host (`host_guard.py wait`: load 0.77–1.35 against a bar of 7.75 on
31 CPUs; no orphaned busy-loops).

| leg | arm(s) | result |
| --- | --- | --- |
| **1 — does the shipped posture starve?** | `B-sentinel`, `K = kPinNever` | **No.** Free-slot floor **28 / 29** and **zero** rx drops at *every* vertex count to 48, both payloads; ~100 k deliveries/s throughout. `pins = 0`, so no RX slot is ever retained by a stored value — the ADR-0041 one-copy trailer-sliced store branch, by construction. |
| **2 — can the instrument redden?** | `C-pin-always`, `D16`, `D2`/`D4`/`D8` @ 512 B | **Yes, violently.** All report `pins > 0`. Floor decays 28 → 27 (1 vertex) → 20 (8) → **4** (24) → **0** (32 and 48), and at 32 vertices delivery collapses from ~107 k/s to **72/s** with **~10⁶** dropped datagrams. |
| **3 — is the ratio bound the remedy?** | the `D` sweep at fixed payload | **No.** At 512 B *every* `K` that pins — 2, 4, 8, 16, ∞ — produces the identical collapse (floor 0, ~10⁶ drops, 348 delivered over 12 rounds) at 32 and 48 vertices. Exactly RFC-0022 Amendment 2: "every `K` that pins the deployed workload collapses the pool identically". |

The 64 B rows make the predicate legible on their own: `payload × K ≥ segment` means a 64 B payload
needs `K ≥ 16` at a 1 KiB slot, so `D2`/`D4`/`D8` **never pin** there and track the sentinel exactly
(floor 28, zero drops) while `D16` collapses with the rest. `K` is doing precisely what RFC-0022
§3.D says it does — bounding waste *per value* — and precisely nothing about the *number* of
retained values, which is the quantity that empties the pool.

The collapsed cells are worth reading exactly: **348 delivered over 12 rounds is 29 per round, the
pool's whole capacity.** The pool fills once, every slot is borrowed by a live pinned value, and
the transport never receives again — not degraded throughput, a **latched** dead node. That is the
failure mode #760 named, reproduced at the C6 geometry, and it is why the borrow is documented as
an occupancy budget rather than as a performance trade.

**Verdict: the gate PASSES on the sentinel arm, so copy-on-retain is NOT built.** The shipped
posture (`config_t::kPinPayloadRatio = kPinNever` on both targets) already bounds occupancy, and
the deliverable is the borrow contract in the docs — see
[02 §"The pin is a BORROW, and the application owns the budget"](../docs/reference/02-graph-model.md).
The failing arms are the evidence for the class guidance, not a defect: they are what a deployment
that sets a non-sentinel `K` on a retain-heavy NARROW node is buying.

#### Two properties of the runner worth knowing

**Reachability is measured as an OUTCOME.** The `pins`/`copies` columns come from segment-pointer
identity between the stored value and the segments `recording_pool_t` handed out — available with
or without `LIBTRACER_PIN_INSTRUMENT` (which is `ON` by default here and arms the decision-site
counters separately). An arm that intends to pin and reports zero pins **invalidates its own row**;
that check is not optional, and a Leg-1 pass proves nothing without it.

**Bind failures are retried, not fatal.** A full sweep burns two UDP ports per pair and wraps the
port window several times, so it will eventually land on a port the kernel has not released. That
used to abort the sweep mid-round under `set -e` — and because the arms are interleaved, a
truncated transcript is also an **unbalanced** one. `run_cell` now retries on the next port (five
consecutive failures still stops the run loudly) and checks *both* ends, since a publisher that
failed to bind would otherwise leave the subscriber to emit a zero-delivery row that reads as
starvation but is a bind error.

### `bench_path_label` — the RFC-0027 §12.4 gate (#1325 car 5)

```sh
cmake --build bench/build --target bench_path_label -j
taskset -c 24 ./bench/build/bench_path_label   # RESULT mode=plabel-{string,label} / presid-*
```

[RFC-0027](../docs/spec/rfcs/0027-label-switched-path-compression.md) §12.4 makes a bench
gate **normative for acceptance of the implementation**, and no instrument in the tree
answered it. `bench_hop_chain` measures RFC-0004 §E.1's per-link route handle (a `COMPACT`
frame), not a **path label**; it is fixed at four hops, and its own arms self-void as
non-comparable. So this harness exists.

A chain of `H + 1` real `fwd_router_t` hops carries the **same** `FWD{op=READ}` twice,
differing only in how `dst` is spelled — the full canonical address, or the 7-byte label
element each hop minted for its own mount run. The labels are read off the reply the
implementation itself emits (§6.1 erratum 2), never hand-spelled, so the bench doubles as an
end-to-end check that §6.1 point 4 holds. Per-direction frame counts are compared between
the arms and a mismatch **voids the run** with a non-zero exit — `bench_hop_chain`'s lesson,
which cost that bench a published number that was ~40 % reply leg.

**The per-hop saving is a function of registry width, and reporting it as one number would
be wrong.** Re-measured on an EPYC 9115 vCPU, `taskset -c 24`, 10 runs, medians of the p50,
**after the terminus mint (#1357) and deref (#1363) landed** — so both arms now carry a
label at the terminus as well as at every hop:

| children per hop | string ns/hop | label ns/hop | label's per-hop saving |
| ---: | ---: | ---: | ---: |
| 1 | 307.1 | 304.9 | **+2.3 ns (−0.7 %)** |
| 8 | 324.7 | 317.4 | **+7.3 ns (−2.2 %)** |
| 64 | 496.4 | 432.4 | **+64.0 ns (−12.9 %)** |

Read as slopes over `H ∈ {1, 2, 4, 8}`, because a single-hop point is the wrong instrument
for a per-hop claim — the correction RFC-0024 §8.4 had to make on itself. **The per-hop win
is still a WIDE-node property** and the 2026-08-16 ruling on #1325 stands: at width 1 the
slope difference is 2.3 ns/hop, inside what this instrument should be asked to resolve.
What changed is the FIXED term — the label arm was +12 ns at width 64 before and is now
−21 / −30 ns at widths 1 and 8, because the terminus's own residual stopped being a string.
The originator's frame shrinks **17 B/hop → 7 B/hop** in every arm.

**§12.4 clause 2, the axis §3.3 nominates as deciding, now runs** — it could not before,
because §6.1 point 3's terminus mint was unimplemented (#1357) and, once it was, a labelled
residual addressed at a local vertex still took §7.2's `NOT_FOUND` for want of the terminus
deref (#1363). One hop, width 8, residual depth 1→12, 10 runs:

| residual depth | string p50 | label p50 | delta | string frame | label frame |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 757 ns | 720 ns | **−4.9 %** | 51 B | 45 B |
| 2 | 820 ns | 720 ns | **−12.2 %** | 54 B | 45 B |
| 4 | 827 ns | 720 ns | **−12.9 %** | 60 B | 45 B |
| 8 | 870 ns | 720 ns | **−17.2 %** | 72 B | 45 B |
| 12 | 905 ns | 720 ns | **−20.4 %** | 86 B | 45 B |

**String 13.5 ns per residual segment; label 0.0 — flat, at every depth, to the nanosecond.**
Every pair's ranges are disjoint, against an A/A null whose widest p50 excursion over the same
10 runs is ±1.38 % (≤ 0.42 % on these five modes). That is the shape the claim predicts and
the one §3.3 says the byte column cannot show: the label stands for the WHOLE residual
(§5.3.3), so its cost does not grow with the address it replaces.

Diagnostic, **not** a `perf.yml` gate: both arms are in one binary and the comparison is
between two *spellings*, not two builds.

### `bench_subscribe_index` — link-identity interning (#1266)

```
cmake -S bench -B bench/build -DCMAKE_BUILD_TYPE=Release
cmake --build bench/build --target bench_subscribe_index -j
python3 bench/host_guard.py wait --timeout 900
bash bench/run_subscribe_index.sh bench/build 13 2 > /tmp/sidx.tsv
python3 bench/collate_subscribe_index.py /tmp/sidx.tsv
```

`graph_t::index_link_vertex` keys the per-link departure index by the link's NAME: a hash, a
map find, and a `std::pmr::string` key copy on a miss, all under `link_index_mutex_`. #1266
asks whether keying it by an interned token instead is worth doing. Two attempts stalled for
want of an instrument — #1290 measured an interned probe table with a purpose-built variant
that was then discarded, and #1366 recorded *"Measurement: not taken. No checked-in A/B
harness for the subscribe path exists."*

**Four index arms in ONE binary**, because this host's cross-build layout sensitivity runs to
+9.8 % on an untouched leg (below) — larger than part of the effect being hunted, so a
two-build A/B could not resolve it:

| arm | what it is |
| --- | --- |
| `S-sentinel` | the driver loop with no index operation — the control every delta is taken against |
| `idx-string` | today's shape, transcribed line-for-line from `core/src/graph.cpp` |
| `idx-token` | interning with the token **already in hand**: no hash, no map, no key copy |
| `idx-token-intern` | the **graph-only re-key**: the name still arrives as a `string_view`, so the hash is still paid and the token vector is paid on top |
| `sub-wire` | the live `graph_t::subscribe_wire`, for proportion — never compared to the control |

`idx-token` deliberately charges **nothing** for obtaining the token, so `idx-string −
idx-token` is the **ceiling** on what interning could ever save, not an estimate of it. In the
real system the token has to be minted upstream and carried across the `tr::net`/`tr::graph`
seam, and that carry is not free.

#### The faithfulness oracle — why the transcription can be trusted

A `graph_t` draws **nothing** from its injected `std::pmr::memory_resource` during
construction or vertex registration (measured: 0 bytes, 0 allocations after a ctor plus eight
`register_vertex` calls — vertex storage comes from the `ctl` block source and the global
heap). In this workload the counted arena therefore sees exactly one structure: `link_index_`
itself. So the live arm's byte count **is** the shipped index's byte count, and `--calibrate`
asserts on every invocation that `idx-string` reproduces it **byte-for-byte and
allocation-for-allocation** at 4, 8 and 65 links (746 B / 23 allocs, 1388 B / 45, 11467 B /
361). A transcription that drifts from the code it claims to transcribe fails the run rather
than quietly mis-reporting it.

#### What the harness can and cannot resolve

It **can** price the index operation itself to well under a nanosecond, separate the lookup
from the list maintenance and the mutex, and report bytes at rest on the same axis.

It **cannot** answer whether the carry is affordable. It measures what a carried token would
be worth *at the index*; the cost of minting one per admitted link and threading it through
the router's per-link state is a different measurement against different code.

It also does **not** measure a name-digest probe table. #1290 built one and recorded the
hazard: real link names share long prefixes (`p0`, `p1`, …, `192.168.4.N:PORT`), so a digest
that folds the tail without mixing strands the entropy and degrades the table to a full scan.
The link names this bench generates carry that prefix structure deliberately, so any future
probe-table arm added here is measured against names that can expose it.

#### The estimator, and the two calibration rules it rests on

Best-of-rounds on the per-round p50, never median (`docs/methodology.md`). The batch is sized
by **window** (`calibrate_batch_for_window`), never by plateau: the plateau rule compares two
timed quantities, so the machine votes on which batch is latched, and `bench_common.hpp`
records same-binary A/A differences of up to ~8 % from nothing but that lottery — which would
eat the entire effect. Arms rotate their order every round inside one process, on the
`run_pin_ratio.sh` template.

The **A/A null is carried in the run**, not quoted from history: each round executes the same
binary twice under tags `A` and `B`, ABBA-interleaved, and the collator takes the widest
per-cell excursion as the band. Any delta below it prints `within-null`.

#### Measured, 2026-08-20, pinned host, `taskset -c 2`, 13 rounds x 2 tags

Load average 6.87 before / 5.34 after; no orphaned busy-loops; no `perf-local` run queued.
Null band **0.89 %**. **Four** independent windows were taken across two source revisions and
a `clang-format` reflow, and every one reproduced the table below to ~0.2 % — which also says
the in-binary comparison is insensitive to the layout term that defeats a cross-build one.

The window quoted here was taken while the host's 5-minute load average was still decaying
from 17.9, and it is quoted anyway **because its own A/A null certifies it**: at 0.89 % it is
tighter than a window taken at load 0.82 (2.23 %). That is the point of carrying the null in
the run rather than asserting quiescence from `uptime` — the instrument, not the operator,
decides whether the window was clean.

ns per index operation, best-of-rounds, at 8 vertices:

| links | sentinel | `idx-string` | `idx-token` | `idx-token-intern` | ceiling | verdict | graph-only | verdict | whole subscribe |
| ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: | --- | ---: |
| 4 | 3.39 | 13.74 (+10.35) | 5.82 (+2.43) | 13.78 (+10.39) | +7.92 | FASTER | −0.04 | within-null | 389 |
| 8 | 3.39 | 14.77 (+11.38) | 5.82 (+2.42) | 14.80 (+11.41) | +8.95 | FASTER | −0.03 | within-null | 471 |
| 16 | 3.39 | 16.05 (+12.65) | 5.83 (+2.43) | 16.09 (+12.70) | +10.22 | FASTER | −0.04 | within-null | 788 |
| 32 | 3.39 | 15.44 (+12.05) | 5.82 (+2.42) | 15.61 (+12.22) | +9.63 | FASTER | −0.17 | **SLOWER** | 1417 |
| 65 | 3.39 | 16.70 (+13.31) | 5.81 (+2.42) | 16.79 (+13.39) | +10.89 | FASTER | −0.08 | within-null | 2378 |

At 32 vertices: ceiling +8.36 → +11.12 ns, graph-only `within-null` at all five points
(−0.04 … +0.10).

Note the graph-only column: `within-null` at nine of the ten cells and **SLOWER** at the
tenth. It never wins anywhere. Parenthesised figures are net of the control.

Note also that `idx-token` is **flat in link count** — 5.82 ns at 4 links, 5.81 ns at 65 —
while both name-keyed arms climb ~21 % across the sweep. That flatness states the interning
result structurally rather than statistically: a vector subscript does not care how many links
exist, and a hash over a set of long-shared-prefix names does.

Bytes at rest, 8 vertices — and these are the SHIPPED index's bytes, per the oracle above:

| links | `idx-string` | `idx-token` | `idx-token-intern` |
| ---: | ---: | ---: | ---: |
| 4 | 746 | 416 | 778 |
| 65 | 11467 | 6760 | 11987 |
| **per link** | **175.8 B** | **104.0 B** | **183.8 B** |

**Three findings.**

1. **A carried token is worth having.** It removes 7.9–11.1 ns of a 13.7–17.9 ns operation —
   77 % of the index's cost net of the control at 4 links, 82 % at 65 — and 41 % of its bytes,
   at every one of the ten cells, far outside the null band.
2. **A graph-only re-key is worth nothing.** `idx-token-intern` is `within-null` at nine of the
   ten cells and measurably **SLOWER** at the tenth, negative-signed at eight, and it wins
   nowhere: the hash does not go away, the token vector is paid on top, and the footprint goes
   **up** (175.8 → 183.8 B/link). The saving is entirely in the carry, not in the keying.
3. **Read against a whole subscribe, the latency win is small.** `subscribe_wire` costs
   389 ns at 4 links and 2378 ns at 65, so the ceiling is **2.0 % of a subscribe at 4 links
   falling to 0.46 % at 65** — the whole-subscribe cost grows ~6x across the sweep while the
   index saving grows ~1.4x. The RAM figure, not the latency figure, is what this change is
   worth on a user-pinned arena.

   That ~6x is itself worth recording: it is not the index (whose whole contribution is
   ~13 ns), so a subscribe on this path gets steadily dearer with peer count for reasons this
   bench localises but does not explain. It is a larger effect than the one #1266 is about.

Diagnostic, **not** a `perf.yml` gate: the arms are four spellings in one binary, not two
builds, and nothing here is banked to the perf store.

### `bench_forward_rope` — the rope forward hop, and what its A/A null is worth (#1358)

```sh
cmake --build bench/build --target bench_forward_rope -j
taskset -c 24 ./bench/build/bench_forward_rope   # RESULT mode=fwd-rope-hop, fanout = link count
```

The multi-link arm of the forward hop (`on_frame_rope`, ADR-0053 §5), swept over link count
at a fixed frame and registry. It is **the only bench that instantiates `rope_cursor`** — the
specialisation `symbol_ratchet.json` pins `route_fwd_forward<tr::wire::grammar::rope_cursor>`
against — so it is the instrument any change to that symbol has to be priced on.

**It was measured at a +0.99…+7.23 % p50 A/A null with bimodal ranges** during #1325 car 5,
which would make it unable to resolve anything smaller than its own noise. That figure is
now accounted for, and it was two independent causes, neither of them the rope path:

- **The host, and it is the larger term.** Re-measured on the pinned host, 11 interleaved
  ABBA rounds of the same binary against itself at `LIBTRACER_BENCH_SECONDS=0.3`, the null is
  **±0.34 % p50 median-of-rounds / ±0.42 % best-of-rounds** with unimodal ranges (worst
  max/min 1.06) when the 1-minute load average is quiet. Inject a memory-bandwidth neighbour
  (`stress-ng --vm 24 --vm-bytes 256M`, load average 16–36) and the SAME pair of binaries
  reads **+17 % … +81 %**, with ranges that split into two clusters about **2× apart** —
  `[238..492]` at one link, decaying to `[4290..6055]` (1.4×) at 64. That two-cluster shape is
  the signature car 5 recorded (`[245..497]`), reproduced on demand. It is **not** a property
  of this bench: `bench_compact_delivery`, run in the same windows, degraded further
  (**+30 % … +46 %**) from **±0.00 %** quiet. Pure *CPU* neighbours are harmless by
  comparison — five spinners, load average 6.8–7.1, just under `host_guard.py`'s 7.75 bar on
  this 31-CPU host, left the null at **±0.83 % / ±0.27 %**. What poisons the rope hop is
  memory-subsystem contention, and the existing quiescence bar does separate the two.
- **The batch calibrator, worth up to ~8 % on its own.** The plateau rule stops doubling at
  the first batch that is not `kBatchPlateau` better than the last, which is a comparison
  between two *timed* quantities — so the machine picks the batch, and the batch moves the
  number, because the sample's two `now_ns()` reads (~22 ns here) are charged per SAMPLE.
  Forcing the batch on the one-link point measures **260 / 250 / 242 / ~238 ns at batch
  1 / 2 / 4 / ≥8**, and repeated executions of one binary were seen latching 2, 4, 8, 16 and
  32 on that point. Two arms could therefore differ by ~8 % with nothing but the calibrator
  between them — discretely, in clusters, exactly the reported shape. This bench now calls
  `bench_common.hpp`'s `calibrate_batch_for_window`, which doubles until the *window* reaches
  20 µs instead: the batch becomes a function of the hop's own cost, lands on
  128 / 64 / 32 / 32 / 16 / 16 / 8 across the sweep, and repeats. Against the plateau-built
  binary the change reads **−1.44 % … +0.08 %** best-of-rounds — it removes the clock term it
  was measuring, and does not step the series.

**Preconditions for quoting this bench, all three of which car 5 was missing.** Run
`python3 bench/host_guard.py wait` first and record `/proc/loadavg` either side of the
measurement — a number without its load context is not evidence here. Estimate each arm with
the **best (or 25th-percentile) of its rounds, never the median of its rounds**: contamination
is one-sided, so a low order statistic rejects a dirty round while the median simply counts
them. On a run this car took while a neighbouring agent got busy — load average 13 → 21, not
injected — median-of-rounds put a binary against itself at **−33 % … +54 %** while
best-of-rounds on the same samples stayed inside **1.44 %**. And read the `[min..max]` range
as a *diagnostic*, not decoration: a spread near 2× with the two ends clustered means the
window was contaminated and the run must be repeated, whatever the medians say.

**The band.** On a quiescent host this instrument's A/A null is **inside ±0.5 % p50**, so it
resolves effects at the ~1 % level and up — enough to price its pinned symbol. It is a
**diagnostic, not a `perf.yml` gated point**: adding it to `perf_gate.py`'s `POINTS` is a
separate decision about wall-clock and about shared-runner variance, which is a different and
looser noise regime than the pinned host measured above.

### `bench_forward_demux` axis 3 — the RFC-0018 falsifier, and what may be quoted off it (#1346)

```sh
cmake --build bench/build --target bench_forward_demux -j
python3 bench/host_guard.py wait && taskset -c 24-30 ./bench/build/bench_forward_demux
```

Axis 3 splits the fixed hop into `fwd-demux-resolve` (what a resolve-once cache could remove)
and `fwd-demux-rebuild` (what it could not), and alongside them runs
**`fwd-demux-resolve-legacy`** — the retired pre-RFC-0018 `NAME`-child resolve leg, kept in
this bench and nowhere else so RFC-0018 falsifier 1 is an **in-binary** comparison rather than
a comparison against a remembered number.

**That arm is only worth its faithfulness, and the first one was not faithful.** It was a
cleaned-up walk: no `PATH_REF` arm on the gate, three `dst` segments walked where
`dst_seg_walk_t::prefill` walks four, and `pos += h->total` in registers instead of the walker.
Ablated in one binary on the quiet pinned host — best-of-12 rounds, `calibrate_batch_for_window`,
**A/A null 0.0 %** — those cost **+13 / +4 / +14 ns** respectively, so the arm read `25 ns`
where the retired code reads `56 ns`. Taken at face value that arm had the packed leg *losing*
(32 vs 25 ns); it only ever "passed" because the retired plateau calibrator inflated it. It is
now a line-for-line transcription of `peek_fwd_dst_any` + `dst_seg_walk_t` at `5e7659e3`,
verified against a build of that actual commit at **56 vs 56 ns** in one binary. **Do not tidy
`legacy_peek_fwd_dst_any` or `legacy_dst_seg_walk_t`** — every branch, store and
`std::optional` in them is what the arm claims to be.

**Quote the in-binary delta, never a cross-binary one.** This leg is unusually layout-sensitive:
with the executed source held **byte-identical** (`read_fwd_header`, `grammar.hpp` and
`fwd_pre_t` all diff-clean between `5e7659e3` and today's `main`), the same transcription reads
**44 ns in one binary and 56 ns in another**, and merely adding an unrelated function to the
translation unit moved the untouched `fwd-demux-resolve` leg from **51 to 56 ns (+9.8 %)**. That
is the #1235 mechanism, and it is the same size as the effects axis 3 is asked to resolve. The
`FALSIFIER-1` line the bench prints brackets the packed arm either side of the legacy one and
reports their spread for exactly this reason: a delta smaller than that spread is not a result.

### `bench_target_binding` — what `target_binding_t` buys, and why both arms are shipped code (#830)

```sh
cmake --build bench/build --target bench_target_binding -j
python3 bench/host_guard.py wait && taskset -c 24-30 ./bench/build/bench_target_binding
```

#1174 landed `graph::target_binding_t` and #830 stayed open because nothing measured it: the
`+21.3 ns/segment` and `flat 11 ns` in its changelog are #830's own prior measurement of
`find_ptr` and `deref_vertex_slot` **in isolation**, not of the shipped delivery leg, and not
taken on this host.

**Both arms are the same function in the same binary, and neither is hand-written.**
`dispatch_edge_target` already carries both spellings and picks per edge, so the in-binary form
costs nothing to arrange: what selects the arm is the shipped mint rule in `admit_subscriber` —
*a target that does not exist yet stays unbound and keeps the canonical spelling*. Registering
the target **before** the subscribe produces a bound edge; registering it **after** produces a
canonical one. Everything downstream of the resolve (the fan-in ACL gate, the nothrow rope
clone, `store_value`) is byte-identical between them. That removes the #1346 unfaithful-control
hazard at the root: there is no transcription whose faithfulness has to be argued, because the
control arm **is** the shipped fallback leg reached through the shipped mint rule — and which
leg each point took is **counted** by `graph_t::target_canonical_resolves()`, per point, not
assumed. A point whose count disagrees with its own name prints `ablation=FAIL` and is excluded
from every summary line.

**The one asymmetry is priced, not waved away.** The canonical arm pays the relaxed `fetch_add`
on `target_canonical_resolves_`, which the pre-#1174 code did not have, so it INFLATES the
canonical arm and the raw delta overstates the win. The `tgt-bind-ctr` / `tgt-bind-noctr` legs
measure that atomic in the same binary — **4.70 ns** on this host — and the summary prints the
corrected delta beside the raw one. Only the corrected figure is a claim.

**Measured** on the quiet pinned host, best-of-12 rounds (`taskset -c 24-30`, load average
2.05 → 3.91 across the window, every round far under the 7.75 bar; per-arm `max/min` 1.013–1.082,
unimodal — no two-cluster shape), `calibrate_batch_for_window`, zero ablation failures in
12 × 14 arms:

| target-key depth | bound (deref) | canonical (`find_ptr`) | corrected delta | A/A null |
| --- | --- | --- | --- | --- |
| 1 | 154 ns | 162 ns | **+3.3 ns (2.1 %)** | 0.00 % |
| 4 | 154 ns | 171 ns | **+12.3 ns (8.0 %)** | 0.00 % |
| 12 | 154 ns | 212 ns | **+53.0 ns (34.3 %)** | 0.65 % |

The bound leg is **flat at 154 ns across depth 1 → 12**, which is the claim the binding was built
on, and the canonical leg's slope is **4.5 ns per segment** — the same order as the
+5.75 ns/segment #830 measured on `find_ptr` alone before any of this shipped.

**Both arms register `/src` first, not just the canonical one.** `find_ptr` walks the ROOT's
child table on its first segment, so if the arms disagreed about whether `src` or the target
subtree was inserted there first, the canonical arm would carry an insertion-order term with
nothing to do with the binding. It is not a hypothetical: an earlier revision that registered
the target first on the bound arm read the deep point **5.6 ns high** (+58.6 against +53.0 ns).
Registering `/src` up front in both arms makes the root's table identical and removes the term
outright rather than bounding it.

**The A/A null, by four constructions.** Each depth runs three arms of the *identical* bound
construction — two `tgt-bind-bound` readings bracketing the canonical one, plus an adjacent
`tgt-bind-bound-aa` — and best-of-12 they span **0.00 % / 0.00 % / 0.65 %** at depths 1 / 4 / 12,
under a nanosecond, an order of magnitude below the shallowest effect. The fourth is the campaign
repeated: independent best-of-12 runs of this binary agree to **≤ 1 ns on every one of the
fourteen arms**, including one taken in a visibly busier window (one-minute load 7.47 decaying
from a five-minute 15.28) that best-of-rounds absorbed to within 1 ns. Read the `bracket_ns`
column the bench prints as the error bar — a delta inside it is not a result.

**A diagnostic, not a gated point.** No perf workflow runs this binary, so it banks no series;
promoting it to `perf_gate.py`'s `POINTS` would be a separate decision about wall-clock and about
which arm a gate should watch.

**A discarded warm-up arm runs first, and it is load-bearing.**
`calibrate_batch_for_window` warms each arm's caches but cannot warm the PROCESS — lazy statics,
the first heap growth, the first touch of the delivery leg's code pages. Left in the sweep that
cost showed up as a **24 ns bracket spread at the first depth and nowhere else**, an A/A null
twenty times worse than the 1 ns the later depths read, which would have swallowed the shallow
arm's entire effect.

**What this bench does NOT answer.** A with-#1174 against without-#1174 comparison is
cross-binary and is therefore not quoted here at all, on this host's own evidence (see the
`bench_forward_demux` section above). The per-edge cost of the binding is instead stated as a
byte count, which is exact and needs no timing: `pub_edge_t` **48 → 56 B** and `edge_view_t`
**152 → 160 B**, i.e. the 8 B lands in real storage rather than in existing padding.

### The #504 reply-spread must-not-regress arm, and the honest limit of it (#830)

#830's acceptance gate carries the #504 rejection precedent: *a memo that wins one shape but
moves the four-link reply spread is a REJECT*. `bench_reply_leg`'s `reply-spread` mode is that
shape — the same fan-out rotated over `kSpreadLinks` (4) destinations, so consecutive deliveries
never repeat a link. Best-of-8 rounds on the quiet host (load average 2.55 → 3.74, every round
under the bar; `max/min` 1.012–1.017 on the spread arm):

| fan-out (w=32) | `reply-last` ×2 | `reply-spread` ×2 | spread A/A null | spread − last |
| --- | --- | --- | --- | --- |
| 8 | 625 / 622 ns | 657 / 660 ns | 0.46 % | +5.61 % |
| 64 | 3950 / 3960 ns | 4240 / 4230 ns | 0.24 % | +7.08 % |

**What that is and is not.** The bench interleaves `LAST, SPREAD, LAST, SPREAD`, so the two
readings of each arm are an in-binary A/A null and the spread−last figure is an in-binary delta.
What it is **not** is a with-#830 against without-#830 comparison: that is cross-binary, and this
harness does not quote cross-binary deltas. Rather than dress one up, the mechanism is closed
off directly, in two facts that need no timing at all:

- **`dispatch_edge_remote` never reads `e.binding`.** Its `remote_delivery_t` is built from
  `link`, `return_route`, `reverse_route`, `caller` and `delivery_compact`, and nothing else.
  There is no code path by which the binding can enter this leg.
- **The only residual mechanism is bytes streamed, and it has no headroom here.** The fan-out
  loop streams `F × sizeof(pub_edge_t)`, which the binding grew 48 → 56 B. At this gate's widest
  arm that is **3584 B against 3072 B** — both L1-resident on this host, which is why the arm
  the suite relies on to catch a per-edge bytes defect is the wide `inproc` ladder at fan
  1024/8192 (`kFanoutsMid`, #844 / #841), not a four-link spread that tops out at 64.

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
| `lkv-alloc-heap` / `lkv-alloc-pool` | **the segment allocation alone** — `backend.alloc` + `backend.destroy`, no payload moved. The ADR-0060 pooled-vs-heap ratio the `lkv` gate asserts, and a *null control* for the pair below: it shares their loop and their binary but never flattens, so an arm that moves while these stay at 1.00x is the flatten and not the host. |
| `lkv-store-heap` / `lkv-store-pool` | **the rope-to-contiguous copy** — `rope_t::materialize` over a 2-link rope (backend alloc + payload `memcpy`), pooled backend vs the default heap. Gated points since [#1250](https://github.com/avatarsd-llc/libtracer/issues/1250). **The name is misleading and is kept anyway:** the "store" is the *copy-store allocation*, NOT the LKV slot — there is no `graph_t`, no vertex and no last-known-value publish in this loop. A rename would end the `gh-pages` history series keyed on these names (the `fold-n*` → `fold-b*` precedent below), so the meaning is documented instead. Read it as "`materialize` got slower", never as "the LKV store got slower" — #1250 was triaged the wrong way round for exactly that reason. |
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
> (`bench_libtracer.cpp:16`, `:1245`); `bridge_t`, `router_wrap`, `router_unwrap`, `kMaxHops`,
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
