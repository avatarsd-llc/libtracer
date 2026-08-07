# The connection table is lock-free to read and mutex-serialized to write: an append-only chunked list, plus one control-plane lock

Status: **accepted; implemented** — decisions 1–4 all landed: the append-only chunked `children_` (`core/include/libtracer/child_registry.hpp:705`), permanently stable slot addresses (now depended on by ADR-0062's forward cache), the two control-plane `std::mutex`es (`transport_vertex.hpp:395`, `fwd_router.hpp:872`, with the lock order documented at `transport_vertex.hpp:392`) and `std::atomic<bool> child_t::multi_peer` (`child_registry.hpp:146`). **Corrects the load-bearing premise of [ADR-0061](0061-per-transport-mount-routing-strip-k-l5-demux.md)** — that the connection table is "immutable after setup" — which [RFC-0014](../spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md) invalidated by making connection create/remove a *runtime* operation. Originally reused [ADR-0060](0060-lkv-copy-store-injected-value-backend.md) §2's arch-selected sync trait; **Erratum 1 retires that** in favour of a plain `std::mutex`, which is the primitive the rest of the codebase already serializes with. Upholds [ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) §3 (the FWD demux is lock-free) and its invariant #2 (zero-heap forward). Resolves [#521](https://github.com/avatarsd-llc/libtracer/issues/521) and unblocks [#512](https://github.com/avatarsd-llc/libtracer/issues/512). Grounded by a `/grill-with-docs` session against the code and fresh measurement.

## Context

`child_registry_t` documents an invariant it does not have:

> Slots are stable for this object's lifetime: only appended to, never erased or reordered

Tombstoning (#494) makes a slot stable against **erase**. Nothing makes it stable against **append** — `children_` is a `std::vector`, and `push_back` reallocates, invalidating every slot reference and iterator in the table.

That was harmless while `fwd_router_t::add_child`'s own comment was true — "Registry is otherwise immutable after setup". **RFC-0014 ended that.** `make_connection` is wired as a graph child-type factory (`transport_vertex.cpp:100-106`), so it runs when a CREATE op resolves at the terminus — on whichever transport's receive thread delivered the frame. Meanwhile a FWD arriving on a *different* link's thread is inside `by_segments`, iterating `children_`, holding no lock, because ADR-0038 §3 makes that lock-freedom load-bearing rather than incidental.

So a forward read can race a connection-create across threads and walk a freed buffer. This is the same use-after-free class #494 closed in the registry, re-entering from the write side.

Exploring the write side found the problem is wider than the registry. `transport_vertex_t` has **no synchronization at all** — no mutex, no atomics — yet `make_connection` mutates three containers (`pending_links_`, `conns_`, `modules_`; `transport_vertex.hpp:403-416`), and the graph invokes the factory **outside** `map_mutex_` (`graph.cpp:1879`). Two concurrent CREATEs give concurrent `insert_or_assign` into a `std::map` — tree rebalancing under a racing insert, a worse failure than the vector realloc.

## Decision

**Readers are lock-free; writers are serialized by an arch-selected trait. Both halves are required and neither is redundant.**

1. **`children_` becomes an append-only chunked list with an atomic link.** The table is *never* erased from or reordered — teardown tombstones in place — so the only mutations are append plus an in-place pointer null. A chunked list makes both safe for a lock-free reader with **no reclamation problem at all**, because nothing is ever reclaimed. Chunk size is an allocation granularity, not a bound: the list grows without limit, so this introduces no synthetic cap (RFC-0006/0007, ADR-0051).

2. **Slot addresses become permanently stable**, which is a deliberate second effect, not a side effect — see Consequences.

3. **The control plane serializes writers with a plain `std::mutex`** — one on `transport_vertex_t`, one on `fwd_router_t` — covering `make_connection` / `remove_connection` / `provide_link` and `add_child` / `remove_child` in full, and **never taken by the forward path**. *(Revised — see Erratum 1. This decision originally specified the ADR-0060 §2 arch-selected sync trait; that was wrong, and the reasons are recorded below rather than quietly dropped.)*

4. **`child_t::multi_peer` becomes atomic.** Increment 1 made the *append* publish safely, but `add()` also **rebinds** an existing slot (the tombstone-reuse path that RFC-0014 create/remove churn takes constantly), and that rebind plainly writes `multi_peer` while the forward path plainly reads it (`fwd_router.cpp:212`, and the ADR-0062 cache probe at `:790`). Only `link` was atomic, so this was a genuine reader-vs-writer data race that increment 1 did not close — see Erratum 3.

5. **`graph_t`'s vertex map stays as it is**, under `map_mutex_`. Making it lock-free is explicitly out of scope; see Considered Options.

## Errata

Recorded after an adversarial re-judgement of this ADR against the code. The central claim — that
control-plane writers are unsynchronized and that this is reachable — **held**. Three of its
supporting specifics did not, and the mechanism it prescribed was unimplementable at the call site
it named.

**Erratum 1 — the arch-selected sync trait was the wrong mechanism, and could not have been built.**
Decision 3 originally specified ADR-0060 §2's trait (interrupt-disable on single-core, spinlock on
multi-core). Three grounds retire that:

- *The trait does not exist to be reused.* ADR-0060 §2's "arch-selected sync" is prose; its only
  realization, `sync_pool_t` (then at `mem_pool.hpp:158`), hardcodes a `std::atomic_flag` spinlock and its
  own comment defers the interrupt-disable variant as a follow-up. Adopting it here meant *building*
  the abstraction, not reusing one — so the "second mechanism would be incoherent" argument in
  Considered Options was comparing against something not yet there. *(Note, 2026-08-03: this
  ground has since LAPSED — the trait now exists. ADR-0060 Erratum 2 (#770) records
  `mem::synchronized_pool_t<Sync>` constrained by the `mem::pool_sync_policy` concept
  (`mem_pool.hpp:109,169-170`), with `spin_sync_t` in core and `tr::esp::portmux_sync_t` in the
  ESP-IDF component; `sync_pool_t` survives only as the host alias `synchronized_pool_t<spin_sync_t>`
  (`mem_pool.hpp:223`). The **ruling below is unchanged**, and ADR-0060's erratum says so itself:
  the policy guards the O(1) data-path free list and still cannot wrap a control-plane section
  that blocks on sockets and `map_mutex_`. Only this first ground has expired; the other two
  carry the decision on their own.)*
- *Interrupt-disable cannot wrap this section.* `make_connection` calls the transport factory —
  `socket()`, `bind()`, `connect()`, a WebSocket handshake, `pthread_create` — and then
  `graph_.register_vertex_key`, which blocks on `map_mutex_`. Blocking inside an interrupt-disabled
  critical section aborts. The ADR's own Consequences ("must not re-enter the graph's mutation APIs
  or block") states a constraint this call site already violates and cannot be made to satisfy.
- *A spinlock is actively worse here.* The section is milliseconds long (a DIAL can wait
  `connect_timeout_ms`). On single-core FreeRTOS a high-priority task spinning on a lock held by a
  lower-priority one is unbounded priority inversion with no way out; a mutex has priority
  inheritance. The ~2 µs semaphore round-trip that motivated the trait is a **data-path** figure
  imported into a **control-plane** decision this ADR itself labels non-hot — ~0.1% of a multi-
  millisecond socket setup.

The cost is ~4 B static per lock plus ~90 B of FreeRTOS mutex allocated on first lock (the figure
already recorded at `vertex.hpp:996`), i.e. ~188 B one-time for both locks — not per connection.
Against the priority order this also *converges* on the primitive `route_handle_t` and the vertex
stripes already use, rather than adding a second.

**Erratum 2 — `make_connection` mutates two containers, not three.** It writes `conns_`
(`transport_vertex.cpp:314`) and `pending_links_` (`:317`); it only *reads* `modules_` and
`transport_types_`, both written at setup. The Context paragraph's "three containers
(`pending_links_`, `conns_`, `modules_`)" is wrong about `modules_`. The exposure is nonetheless
*wider* than stated: `settings_of` / `link_of` / `remove_connection` all traverse `conns_` unlocked,
so readers race the insert's rebalance too, and `fwd_router_t::child_rx_` (a `std::deque`
`emplace_back`'d in `add_child`) and `child_registry_t::append` itself are both unsynchronized
writer-vs-writer. `append` is the sharpest: two writers read the same `used`, receive **the same
slot pointer**, both fill it and both publish — one child silently lost.

**Erratum 3 — the reachability example was wrong; the race is real by another route.** "Two peers
each creating a connection" does **not** race: `transport_ws_server` and `transport_tcp_server`
multiplex every peer on ONE poll thread (`transport_ws.hpp:61,224`, `transport_tcp.hpp:197,352`) — no
per-peer thread, because FreeRTOS stacks are the scarce resource. Two peers of one server are
serialized by construction. The race needs **two distinct links in distinct modules** (a CREATE over
`ws-server` while another arrives over `udp`, `tcp-server`, or `can`), or any application thread
calling `provide_link` / `graph.write` at runtime. That is the ordinary multi-transport node shape,
so the conclusion stands — but the mechanism is per-transport receive threads, not per-peer ones.

**Erratum 4 — increment 1's ordering is correct, and a naive TSan test will not fail.** Every memory
order on the append path checks out: the slot is filled before `link.store(release)` and before
`used.store(release)`, and `for_each` acquire-loads `head_`, `next`, and `used` before touching a
slot. So a test that hammers *new* names against a forward reader will pass and give false
assurance. The test that actually fails must churn **create → remove → create of the same name**, to
drive the rebind path of Erratum 3 and the writer-vs-writer paths above.

**Erratum 5 (2026-08-04) — the terminus figure in "Considered options" is stale; the rejection it
supports stands.** "`bench_forward_heap` measures the terminus at 9 allocations / 937 bytes per
resolve" does not reproduce. The instrument reports `RESULT terminus allocs=6 frees=6 bytes=601`,
and **no arm of that bench reports 9 allocations or 937 bytes**. Measured on `e058fe04` and again
on `e313f4d` (pre-#848) — identical, so the egress-nothrow work did not move it and the number was
simply carried forward. The "roughly 1%" ratio beside it was derived from the retracted figure and
is **not** re-derived here: no timing was taken, only allocation counts. The rejection is unaffected
— six allocations still dominate an uncontended 3 ns lock by orders of magnitude — but a reader must
not quote 937, or the 1%, as measured.

## Considered options

- **A `std::mutex` control-plane lock.** Originally rejected on embedded cost — an uncontended host lock/unlock is **3 ns**, but ADR-0060 §2 measures a **FreeRTOS semaphore round-trip at ~2 µs**, and a second mechanism beside an established trait looked incoherent. **This rejection is withdrawn (Erratum 1):** the trait was never built, cannot wrap a section that blocks on sockets and `map_mutex_`, and a spinlock there risks unbounded priority inversion. The 2 µs figure is also a data-path number applied to a control plane this ADR calls non-hot — ~0.1% of a multi-millisecond connection setup. **This is now the decision.**

- **`std::deque` instead of a chunked list.** Rejected as a half-measure: it stabilizes element *references* across `push_back`, but a reader iterating still walks the deque's spine, whose map an append can reallocate. It fixes the pointer hazard and leaves the iteration hazard.

- **Lock-free `graph_t` descent (fully lock-free writers).** Rejected on measurement, not on difficulty. The only reader that takes the graph lock on a hot path is the **terminus** (`deliver_local` → `graph_.find` → `find_ptr` → `std::shared_lock`, `graph.cpp:663-664`); deliveries themselves don't, since `write_impl` operates on an already-resolved `vertex_t*`. And `bench_forward_heap` measures the terminus at **9 allocations / 937 bytes** per resolve. Removing a 3 ns lock from a path that allocates 937 bytes is invisible — roughly 1% of a path this change cannot otherwise improve — in exchange for epoch or hazard-pointer reclamation across the whole L4 surface. If it is ever wanted, it is its own ADR with its own evidence.

- **Serializing creates by holding `map_mutex_` unique across the factory call.** Rejected: it widens the graph's global lock to cover socket construction and transport-plane state, coupling two layers and lengthening a hold that currently blocks every terminus resolution device-wide.

## Consequences

- **#512's cheapest invalidation model becomes available.** With slot addresses permanently stable, an ADR-0062 forward cache can hold a `const child_t*` — the registry **slot** — rather than a `transport_t*`. Teardown nulls `slot->link` in place, so a stale cache reads `nullptr`: the same clean miss as today, at one dereference instead of a scan. No second generation concept, no O(links × labels) teardown sweep, no forfeiting the win. **The tombstone becomes the invalidation.** This is why the container decision must precede #512's.

- **The forward path is unchanged and must stay so.** It already avoids the registry for the inbound mount run (#525, the run is carried on the link's receiver ctx), and its remaining `by_segments` scan is a plain traversal — a chunked list traverses as a linked walk instead of a contiguous one, which may cost a little locality. That must be measured against `bench_forward_demux`, not assumed: current baseline is ~112 ns fixed plus ~3.9 ns per registered link.

- **This class is currently unpoliced.** The `tsan` CI job exists but does not exercise concurrent creates. A TSan test that hammers create/remove against a live forward stream lands with this change, or the invariant is only asserted in prose.

- **`transport_vertex_t` gains its first synchronization.** Anything called under the control-plane trait must not re-enter the graph's mutation APIs or block — the same discipline `graph.hpp:942` already documents for resolvers, and sharper here if the trait resolves to an interrupt-disable critical section.

- **ADR-0061's "immutable after setup" premise is formally retired.** Its erratum already records that the registry mutates at runtime; this ADR is where that fact acquires a mechanism instead of a caveat.
