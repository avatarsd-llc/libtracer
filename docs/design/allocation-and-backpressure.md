# Failable allocation and backpressure

> **Scope:** this page describes the C++23 reference implementation's allocation seams and its
> failure semantics. It is **not** the standard. The protocol-level obligation — that a receiver's
> bounds are injected resources and that exhaustion is answered by value — is described
> implementation-independently in [`../reference/09-memory-substrate.md`](../reference/09-memory-substrate.md);
> the normative surface is [`../spec/v1.md`](../spec/v1.md).

## The rule

**Every allocation a peer can provoke must be able to report exhaustion as a value. None may
abort.** A frame arrives from the network, a peer chooses its nesting depth, its node count and
its link count, and several of those allocations sit behind no ACL. If any of them reports failure
by throwing, then on a `-fno-exceptions` target the throw lowers to the toolchain's `abort()` stub
and a peer can reboot the node by sending a large frame.

`std::pmr::memory_resource::allocate` reports failure by throwing. That is not a defect of the
implementation using it — it is the contract of the type, and no wrapper can add a return channel
to a function whose only failure signal is an exception. So a target that compiles without
exceptions needs a **second, structurally different seam** for the failable allocations, one whose
failure is a `nullptr` return. That seam is `tr::mem::block_source_t`
([ADR-0065 — failable allocation gets its own seam](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0065-failable-allocation-gets-its-own-seam-block-source.md)).

The rejected alternative was routing failable allocation through the existing `mr_` seam with a
non-throwing `memory_resource` subclass. It fails for a reason that has nothing to do with taste:
`allocate` has no way to say "no", so a non-throwing resource must either abort or return a
pointer, and the caller has no branch to write.

## The three injected seams

`graph_t`'s constructor takes all three, each defaulted so an unconfigured host gets the platform
heap and byte-identical behaviour (`core/include/libtracer/graph.hpp:187-189`).

| Seam | Type | What it allocates | Exhaustion |
| --- | --- | --- | --- |
| `mr_` (`graph.hpp:971`) | `std::pmr::memory_resource*` | the small control *objects* of a stored write: the `shared_ptr` control block and the `rope_t` wrapping the value's links | throws — structurally cannot report by value |
| `value_backend_` (`graph.hpp:977`) | `mem::mem_backend_t*` | the durable byte `segment` holding a vertex's last-known value when the write path must own its bytes | `nullptr` → write rejects `BACKPRESSURE` |
| `ctl_` (`graph.hpp:1038`) | `mem::block_source_t*` | every allocation a peer can provoke | `nullptr` → the operation answers a status |

Three seams rather than one because the three contracts differ: cache hooks, `owns_bytes` and
ISR-safety belong to a byte buffer; object construction belongs to `std::pmr`; reporting exhaustion
by value belongs to `block_source_t`. `ctl_` is deliberately a *different C++ type* from `mr_` so
the two contracts — may-be-null versus must-not-be-null — cannot be transposed by a one-token edit,
and so retiring `mr_` later is a compile error rather than a silent rebind (`graph.hpp:174-186`).
`ctl_` is declared last in the object on purpose: no hot path reads it, so placing it there leaves
every other member at the byte offset it had before the seam existed, which keeps the forward-hop
bench measuring the same layout (`graph.hpp:1032-1036`).

`fwd_router_t` carries the same failable seam separately as its `rx` parameter
(`core/include/libtracer/fwd_router.hpp:148`), because the terminus arena decode belongs to the
router's receive thread rather than to the graph. It carries a **fourth** injection beside it, and
for a different contract: `flat` (`fwd_router.hpp:142`), the `mem_backend_t` **every rope flatten on
the forward and terminus paths** draws its owned `segment` from — the byte-buffer seam, with cache
hooks and a refcount, which the block source is not. The split is `graph_t`'s `ctl_` /
`value_backend_` split one layer out. Like `value_backend_`, an injected `flat` **MUST be
thread-safe** (ADR-0060 §2): all of those sites but one run on a transport child's receive thread
and the remaining one on the writer thread, and the `segment` it hands out self-routes its reclaim
on whichever thread drops the last reference. A bare `pool_t` is not thread-safe and must not be injected here;
`synchronized_pool_t<Sync>` (`core/include/libtracer/mem_pool.hpp:170`) is the in-tree
composition, and its critical section is a compile-time policy: `sync_pool_t` for the multi-core
spinlock, `tr::esp::critical_pool_t` for the single-core interrupt-disable variant.

**A bounded node must inject all of them.** Injecting `mr_` and `value_backend_` alone leaves every
peer-driven allocation on the global heap, where the failure mode on a `-fno-exceptions` target is
the abort this seam exists to remove. A host reaching for "one slab, whole stack" points all three
plus the router's `rx` **and its `flat`** and the transport-receive backend at the same underlying
slab; the composition guidance is in
[`../reference/09-memory-substrate.md`](../reference/09-memory-substrate.md).

**"One slab, whole stack" is a direction, not a state the tree is in.** One gap still stops a node
that has injected every seam above from actually holding the whole stack in its slab:

- the byte-buffer seams need a thread-safe backend, and the only synchronised pool built is a
  spinlock — wrong for a single-core target ([zero-copy and flatten](zero-copy-and-flatten.md) §5),
  so a constrained node keeps `value_backend` and `flat` on `heap_backend()` today.

The **terminus** gap that stood beside it is closed: until #766 the resolver's rope-tier flattens
(`view_node::ensure_cache`, `view_node::own_wire` in `core/src/op_resolve_view.cpp`) never saw
`flat` and drew from the global heap on every fragmented terminus request — measured: with `flat`
armed to refuse everything, a 4-link `FWD{READ}` consulted it **zero** times and still made 20
global-heap `new` calls. The router now hands `flat` to its `op_resolver_t`, which threads it
through the rope-tier node reader, and `core/tests/terminus_flatten_backend_test.cpp` pins both
halves: the same request costs strictly fewer global allocations with `flat` on a slab, and a
refusal is answered rather than read short.

**The bound covers the arena, and — since #730 and #766 — every rope flatten on the forward and
terminus paths beside it.** Read the arena claim below as a claim about the arena only: until #730
the router's four `materialize()` call sites all took `rope_t::materialize`'s DEFAULT global-heap
backend, so a node that had injected every seam above still flattened outside its own bound. Worse,
two of those sites were unguarded, and an empty flatten is not a visibly-failed one:
`view::over_bytes` maps an empty span to an ENGAGED-empty optional by design and `graph_t::write`
stores it and reports success, so an ingress `COMPACT` flatten that OOM'd REPLACED the subscriber's
last-known value with nothing. Both halves are closed at every one of those sites — they draw from
`flat`, and each answers a refusal by value (the rows in [Status legs](#status-legs) below). Read
*that* literally too: the named sites in that table, not "the router's allocations" — the terminus
arena is `rx`'s, and the reply head segment is neither's.

## The block source

`block_source_t` is a bytes-in / `void*`-out interface whose allocating method is
`[[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept`
(`core/include/libtracer/mem_source.hpp:192`). The `noexcept` is the whole point: the override
cannot throw, so the caller has exactly one branch to write.

Four implementations ship:

| Source | Construction | Behaviour |
| --- | --- | --- |
| `heap_source()` (`mem_source.hpp:138`) | free function, process-wide | wraps the platform allocator; the default for all three seams |
| `null_source()` (`mem_source.hpp:159`) | free function, process-wide | serves nothing; makes a `bump_source_t`'s buffer a hard bound |
| `bump_source_t` (`mem_source.hpp:184`) | `bump_source_t(std::span<std::byte> buffer, block_source_t& upstream = heap_source())` | carves from `buffer`, falls back to `upstream` once it cannot fit |
| `pool_source_t` (`mem_source.hpp:316`) | caller-supplied slab plus a caller-supplied span of size classes | segregated exact-size free lists; recycles, so it suits a long-lived seam |

`bump_source_t` is the nothrow twin of `std::pmr::monotonic_buffer_resource`, and the **upstream
parameter is what makes it a capability-preserving substitution**. A monotonic resource also spills
past its buffer, but it spills to a throwing resource — the abort again. A `bump_source_t` spills
to whatever `block_source_t` the caller named, so the same large input still succeeds where memory
exists and fails as a value where it does not (`mem_source.hpp:170-173`).

## The decode arena

The branch-write decode allocates its arena on the calling thread's stack and names the graph's
injected failable seam as the overflow upstream:

```cpp
std::array<std::byte, 4096> stack;
mem::bump_source_t src(stack, *ctl_);
```

(`core/src/graph.cpp:1021-1022`.) Three properties follow, and each closes a different failure mode:

- **A bounded node that injected `ctl` gets its own store here too.** The overflow leg draws from
  that injection rather than from the global heap, so the node's memory bound covers **this
  arena** (`graph.cpp:923-925`). Read that literally: it is a statement about the decode arena, not
  a general one about every allocation near it. Each seam is covered because it was injected and
  the site was pointed at it, one site at a time — the router's flattens went uncovered for a
  release precisely because they looked like they were included in a sentence like this one (#730).
- **The default reproduces heap behaviour exactly.** `ctl_` defaults to `heap_source()`, so a host
  that injects nothing sees the same capability it had with an unbounded resource.
- **Exhaustion is a value.** A tree larger than the slab still decodes where the upstream can serve
  it, and where the upstream cannot, `decode_into` returns an error the caller converts to a status.

The arena is *structure* storage — a node array and the walk's open-node stacks — so its size is
independent of the payload's byte count. No node-counting pre-pass exists, and none is needed: the
seam alone carries the failure. The three draws that make the RX decode path peer-provokable — the
node array's growth, the sink's open-node stack, and the walk stack's spill past its inline slots —
are enumerated in the changelog (`core/CHANGELOG.md:412`), which is the citation for that leg being
closed.

TLV nesting has no depth constant. Depth is bounded by the receiver's decode resources, and
exhaustion rejects with `TLV_NESTING_TOO_DEEP` — the status
[RFC-0006](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0006-resource-bounded-nesting-depth.md)
defines for "exceeds this receiver's decode resources".

## Status legs

| Allocation | Site | Failure answer |
| --- | --- | --- |
| Branch-write flatten into the value backend | `core/src/graph.cpp:1008-1009` | empty head with a non-zero rope length → `BACKPRESSURE` |
| Field-write flatten into the value backend | `core/src/graph.cpp:1245-1246` | empty head with a non-zero rope length → `BACKPRESSURE` |
| Branch-write root key render (`try_build_key`) | `core/src/graph.cpp:1040-1041` | `false` → `BACKPRESSURE` |
| Branch-write parse-key copy (`detail::try_assign`) | `core/src/graph.cpp:1043` | `false` → `BACKPRESSURE` |
| Branch-write decode arena | `core/src/graph.cpp:1021-1024` | decode error → `TYPE_MISMATCH` |
| Per-delivery COMPACT flatten (egress) | `core/src/fwd_router.cpp:1442-1443` | the delivery is **dropped** |
| Per-delivery frame build | `core/src/fwd_router.cpp:1447` | the delivery is **dropped** |
| Ingress `ADVERTISE` route flatten | flatten `core/src/fwd_router.cpp:897`, answered at `:905` | the empty flatten **fails the `wire::decode`** ⇒ the frame is **dropped**; the label stays **unbound** (the peer's COMPACTs draw a `HANDLE_NACK`) |
| Ingress `COMPACT` payload flatten | flatten `core/src/fwd_router.cpp:1107`, answered at `:1114` | the delivery is **dropped**; the subscriber keeps its last-known value |
| Bus-name rejection reply flatten (cold) | flatten `core/src/fwd_router.cpp:577`, answered by the `wire::decode` opening `reject_bus_name_hop` | the frame is **dropped** by value — no reply |
| Terminus per-node span materialize (rope tier) | flatten `core/src/op_resolve_view.cpp:216`, answered at `core/src/op_resolve_walk.hpp:654` / `core/src/op_resolve_walk.hpp:699` | a refusal on the reply's own route bytes ⇒ `BACKPRESSURE` on the error side ⇒ the frame is **dropped**; anywhere else before dispatch ⇒ an **addressed** `kind=ERROR STATUS{BACKPRESSURE}` reply |
| Terminus ownership flatten (rope tier, ADR-0053 ⑤) | flatten `core/src/op_resolve_view.cpp:119`, answered by the empty-value guards in `resolve_node` (`core/src/op_resolve_walk.hpp:813-814`) | the write is **not stored** — the vertex keeps its previous value — and the reply is `BACKPRESSURE` |

The two terminus rows are the resolver's, reached through the same `flat` the router injects (#766),
and both are exercised by `core/tests/terminus_flatten_backend_test.cpp` — including a sweep that
moves the refusal point across every flatten one request makes and requires each outcome to be a
drop or an addressed `BACKPRESSURE`, never a `kind=RESULT` built on a short span.

All three router-ingress rows draw from the router's injected `flat` backend, and all three are
exercised by `core/tests/fwd_flatten_backend_test.cpp`, which injects a backend that refuses on
command. The bus-name row was **not** exercised when it was first written down here: the #730
verify pass reverted both halves of that site and the suite still reported 72/72 passing. It has
its own case now, and reverting the site's seam fails it (1 of 72).

**Two of those three rows are answered by a decode, not by a guard.** The `empty()` early-outs
beside the `ADVERTISE` (`core/src/fwd_router.cpp:1094`) and bus-name (`core/src/fwd_router.cpp:767`) flattens are redundant with the `wire::decode`
that follows each — deleting either changes nothing observable, verified by ablation — and the code
says so at both sites. They are kept so the *reason* the operation failed is the OOM rather than the
codec's leniency, and nothing here cites them as proven guards. What the test pins at those two
sites is the **seam**: with the site back on the default heap the flatten succeeds under the
injection and the case fails. Only the ingress `COMPACT` row has a guard that is independently
observable — remove it and the LKV-preservation assertion fails, because that is the site where an
empty flatten was stored and reported as success.

That file is the reason these rows are worth writing down at all: option A on #730 — the guards
without the seam — was rejected because nothing could make those flattens fail, and a guard nobody
can fail is a guard nobody can prove.

The key render and its parse copy are nothrow so that OOM soft-fails the branch write as
`BACKPRESSURE`, the injected-resource status — **never an abort on the writer thread**
(`graph.cpp:975-978`).

The remote-delivery leg answers differently on purpose. A stored write that reached its LKV has
succeeded; the fan-out to one subscriber is a separate obligation, and a subscriber missing
one value under heap exhaustion is valid delivery behaviour where failing the write is not. Every
per-delivery allocation on that writer-thread leg is nothrow, and a failed flatten or frame build
drops that one delivery (`core/src/fwd_router.cpp:1442-1443`). Dropping *invisibly* is the part that
needs an answer, which is why `graph_t::delivery_drops()` exists
(`core/include/libtracer/graph.hpp:848`): three relaxed monotonic counters — `no_target`, `denied`,
`out_of_memory` (`graph.hpp:781-788`) — incremented only on a drop, so the delivering path is
byte-identical while nothing drops. Nothing in the library reads them; a deployment chooses whether
to alarm.

A dropped fresh ADVERTISE on the COMPACT leg self-heals: the peer answers the unknown label with
`HANDLE_NACK` and the next delivery re-advertises (`fwd_router.cpp:985-995`).

## Legs that throw, and their nothrow twins

`rope_t::to_iovec` builds the scatter-gather span table by value, and its `reserve` throws on OOM —
an `abort()` under `-fno-exceptions` (`core/include/libtracer/rope.hpp:213-218`). The terminus reply
egress builds that table on every send, so on a fragmented heap it was a reachable abort. The
nothrow twin is `rope_t::try_to_iovec(std::vector<std::span<const std::byte>>& out) noexcept`
(`rope.hpp:230-235`): it clears `out`, nothrow-reserves it to `link_count()`, and returns `false`
without touching `out` further when the table cannot be grown — the caller drops the reply
(`rope.hpp:220-229`).

Both forms remain. `to_iovec` is correct wherever the caller is on a host build with exceptions or
holds a table it sized beforehand; any path a peer can drive uses `try_to_iovec`.

The forward hop's own entry table has a length of `~6 + link_count` — again the sender's choice, and
not even at the terminus. It is gathered into a `mem::block_array_t` over the injected `rx`, and
exhaustion **drops the hop rather than emitting the entries that fit**: a partial iov is a truncated
FWD on the wire, which is strictly worse than silence, and FWD is not delivery-guaranteed so the
sender retries.

## The host TX gather

A host integration that marshals sends onto a separate task must copy the caller's spans before
returning, because the spans are gone by the time that task runs. The shape that matters is
**where the copy's allocator lives**.

A gather written as a braced initializer over a `std::vector` — building the work item and the
payload copy in one expression — allocates the vector with the *throwing* allocator even when the
work-item shell is guarded with `new (std::nothrow)`. The guard covers the shell; the payload copy
inside the initializer is unguarded, so a reply-sized copy meeting heap exhaustion aborts the node.

The nothrow-end-to-end shape splits them: size the total, allocate the payload buffer with
`new (std::nothrow)`, `memcpy` each span in, then allocate the work-item shell separately — also
`new (std::nothrow)` — moving the buffer in. If the shell allocation fails the initializer never
runs, so the buffer is not moved from and frees itself on return; there is no leak on either arm,
and an allocation failure becomes the same drop the link's send contract defines
(`integrations/esp-idf/libtracer/httpd_ws_link.cpp:476-499`).

## Pitfalls

**A bump block is never reclaimed.** `bump_source_t` has a cursor and no free list, so a source that
outlives one burst of work fills monotonically and then refuses everything — the node does not
abort, the seam behaves exactly as specified, it simply stops working. Construct one per operation
(as the branch-write decode does) or `reset()` it between operations (`mem_source.hpp:224`).
A **long-lived** bounded seam — a router's `rx`, a graph's `ctl` — wants `pool_source_t`, which
recycles (`mem_source.hpp:175-180`).

`bump_source_t` is also single-threaded by contract: a bump cursor is not synchronized, and its
intended use is a function-scoped buffer on the calling thread's stack (`mem_source.hpp:181-182`).

**Exhaustion of the block seam does not have one answer.** The reject belongs to the operation, not
to the seam, and the three in-tree consumers answer differently:

| Consumer | Answer on exhaustion | Why |
| --- | --- | --- |
| Terminus decode | `tr::tlv::nesting_too_deep` | the status RFC-0006 defines for "exceeds this receiver's decode resources" |
| Branch-write decode | `tr::schema::type_mismatch` | it cannot distinguish "the value did not parse" from "the arena ran out", and does not try |
| Rope forward hop | nothing at all | a forward hop has no reply channel; its only sound answer is silence |

`BACKPRESSURE` is what a *store* answers when its **value backend** is exhausted. It is not a
property of the block seam. A caller that assumes one status across all three will misclassify two
of them ([`../reference/09-memory-substrate.md:305`](../reference/09-memory-substrate.md)).

**A pool shared across receive threads is slower than the heap it replaced.** See the topology
result below; `fwd_router_t::add_child` takes an optional per-child source
(`fwd_router.hpp:192`, resolved at `fwd_router.hpp:604-605`) precisely so each transport's receive
thread owns one. A source shared at *wiring* frequency — a graph's `ctl` — is fine behind a lock.

**A `size_class_t` span is a bound the caller sets, not the library.** `pool_source_t` classes do
not share: a freed 64 B block cannot serve a 128 B request. `classes_used()` and `overflowed()`
report what to size the class span against.

## Sizing a bounded seam

### The bump seam's frame count is a capacity result, not a rate

An 8 KiB `bump_source_t` wired as a router's `rx`, **decoding a 53-byte FWD**, decoded **6 frames
and rejected the next 194**
([ADR-0067 §1](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md);
corroborated at `core/include/libtracer/mem_source.hpp:179` and
[`../reference/09-memory-substrate.md:280`](../reference/09-memory-substrate.md)).

The frame size is load-bearing and a frames-served count without it is not a measurement: what the
figure reports is 8192 bytes divided by the arena footprint of one decode of that frame, so a
different frame shape gives a different count. Being capacity arithmetic rather than a rate, it does
not vary with host or build flags — which is what makes it a *design* fact about the seam and not a
benchmark.

### A shared pool on a per-frame path measures worse than the heap

Instrument: the committed `bench_libtracer`, rows `poolalloc-mtN` / `heapalloc-mtN`, every thread
instrumented so latency and throughput describe the same workload. Host: 12-core.

| threads | pool ops/s | pool p50 | heap ops/s | heap p50 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 8.3 M | 60 ns | 15.8 M | 30 ns |
| 4 | 3.6 M | 802 ns | 25.5 M | 70 ns |
| 8 | **1.36 M** | **3587 ns** | 31.0 M | 70 ns |

The shared pool falls to roughly **a fifteenth of its own single-thread rate** while the platform
heap *scales*. Independently reproduced on the 4-core CI runner.

Pure serialization would hold flat at the T=1 figure; collapsing far below it is the signature of a
**cacheline storm**. Every waiter's read-modify-write steals the line holding the free-list head.
Two consequences that a reader sizing a seam has to carry:

- **A lock-free CAS does not fix it.** A lock-free `[index | ABA-tag]` CAS on the list head replaces
  one contended word with the same word. It removes the spin and keeps every thread hammering one
  cacheline, which is what costs the 15×. The shape that fixes it is per-thread free lists or
  magazines — which is what the heap does
  ([ADR-0060 erratum 1](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0060-lkv-copy-store-injected-value-backend.md)).
- **The single-threaded case is unaffected**, and the bounded-target rationale stands: on a
  single-core target with an interrupt-disable critical section there is no concurrent RMW to storm,
  and a deterministic ceiling is the point rather than throughput.

### The same result at the router's own RX seam

Those rows come from a different seam. Instrument: the committed `bench_rx_source_topology`.
Host: 12-core / 24-thread. Median of three 300 ms runs, aggregate forwards/s. T receive threads each
drive a multi-link rope through their own inbound child to their own egress sink, so the source is
the only object two threads share.

| T | shared `heap_source()` | one shared `pool_source_t` | per-child `pool_source_t` |
| ---: | ---: | ---: | ---: |
| 1 | 4.10 M | 4.09 M | 4.32 M |
| 4 | 10.99 M | 2.78 M | 10.80 M |
| 8 | 15.30 M | 1.90 M | 16.00 M |
| 24 | 21.77 M | **1.46 M** | 21.81 M |

The shared pool falls to **a sixty-seventh** of its own single-thread rate — per-thread 244 ns →
16,428 ns — a deeper collapse than the fifteenth above, while the per-child pool tracks the scaling
heap across the whole sweep. The one point where the medians diverge, **T=16, heap 22.6 M against
per-child 19.0 M**, is inside the per-child run-to-run range and is explicitly **not** a finding.

Two control arms belong with the table and must not be dropped from it. At T=1 the pool is **not**
faster than the heap at this seam either — 231 ns against 244 ns, ranges overlapping. A single run
suggested a 27 % pool win; three runs deleted it. The rule that follows is ownership, not
synchronization:

> A `pool_source_t` is owned by one thread wherever it sits on a per-frame path. Sharing one behind
> a lock is admissible only at wiring frequency.

Sizing, for a deployment bounding this seam: a 4-link rope's forward hop settles at **128 B** of
slab per child in **one** size class, independent of T
([ADR-0067 §3](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md)).

## Related decisions

- [ADR-0041 — the terminus reads a flat arena tree, not `tlv_t`](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0041-terminus-arena-decode-span-contract.md)
- [ADR-0060 — the LKV copy-store draws its owned segment from an injected `value_backend_`](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0060-lkv-copy-store-injected-value-backend.md)
- [ADR-0065 — failable allocation gets its own seam, `tr::mem::block_source_t`](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0065-failable-allocation-gets-its-own-seam-block-source.md)
- [ADR-0067 — a bounded seam recycles through segregated size classes and scales by ownership](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md)
- [RFC-0006 — resource-bounded nesting depth](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0006-resource-bounded-nesting-depth.md)
