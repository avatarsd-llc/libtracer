# The connection table is lock-free to read and trait-serialized to write: an append-only chunked list, plus an arch-selected control-plane sync trait

Status: proposed. **Corrects the load-bearing premise of [ADR-0061](0061-per-transport-mount-routing-strip-k-l5-demux.md)** — that the connection table is "immutable after setup" — which [RFC-0014](../spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md) invalidated by making connection create/remove a *runtime* operation. Reuses [ADR-0060](0060-lkv-copy-store-injected-value-backend.md) §2's arch-selected sync trait rather than introducing a second mechanism. Upholds [ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) §3 (the FWD demux is lock-free) and its invariant #2 (zero-heap forward). Resolves [#521](https://github.com/avatarsd-llc/libtracer/issues/521) and unblocks [#512](https://github.com/avatarsd-llc/libtracer/issues/512). Grounded by a `/grill-with-docs` session against the code and fresh measurement.

## Context

`child_registry_t` documents an invariant it does not have:

> Slots are stable for this object's lifetime: only appended to, never erased or reordered

Tombstoning (#494) makes a slot stable against **erase**. Nothing makes it stable against **append** — `children_` is a `std::vector`, and `push_back` reallocates, invalidating every slot reference and iterator in the table.

That was harmless while `fwd_router_t::add_child`'s own comment was true — "Registry is otherwise immutable after setup". **RFC-0014 ended that.** `make_connection` is wired as a graph child-type factory (`transport_vertex.cpp:100-106`), so it runs when a CREATE op resolves at the terminus — on whichever transport's receive thread delivered the frame. Meanwhile a FWD arriving on a *different* link's thread is inside `by_segments`, iterating `children_`, holding no lock, because ADR-0038 §3 makes that lock-freedom load-bearing rather than incidental.

So a forward read can race a connection-create across threads and walk a freed buffer. This is the same use-after-free class #494 closed in the registry, re-entering from the write side.

Exploring the write side found the problem is wider than the registry. `transport_vertex_t` has **no synchronization at all** — no mutex, no atomics — yet `make_connection` mutates three containers (`pending_links_`, `conns_`, `modules_`; `transport_vertex.hpp:348-361`), and the graph invokes the factory **outside** `map_mutex_` (`graph.cpp:1533`). Two concurrent CREATEs give concurrent `insert_or_assign` into a `std::map` — tree rebalancing under a racing insert, a worse failure than the vector realloc.

## Decision

**Readers are lock-free; writers are serialized by an arch-selected trait. Both halves are required and neither is redundant.**

1. **`children_` becomes an append-only chunked list with an atomic link.** The table is *never* erased from or reordered — teardown tombstones in place — so the only mutations are append plus an in-place pointer null. A chunked list makes both safe for a lock-free reader with **no reclamation problem at all**, because nothing is ever reclaimed. Chunk size is an allocation granularity, not a bound: the list grows without limit, so this introduces no synthetic cap (RFC-0006/0007, ADR-0051).

2. **Slot addresses become permanently stable**, which is a deliberate second effect, not a side effect — see Consequences.

3. **The control plane serializes writers with the ADR-0060 §2 sync trait**, not an OS mutex: an interrupt-disable critical section on single-core targets (ESP32-C6, Cortex-M), a spinlock on multi-core (ESP32-S3, host). It covers `make_connection` / `remove_connection` in full — the three `transport_vertex_t` containers *and* the registry's scan-then-append — and is **never taken by the forward path**.

4. **`graph_t`'s vertex map stays as it is**, under `map_mutex_`. Making it lock-free is explicitly out of scope; see Considered Options.

## Considered options

- **A `std::mutex` control-plane lock.** Rejected on the embedded cost. Measured on host, an uncontended `std::mutex` lock/unlock is **3 ns** — identical to an atomic RMW, so on host the choice is a wash. But ADR-0060 §2 measures a **FreeRTOS semaphore round-trip at ~2 µs**, and having already established a trait for exactly this reason, introducing a second, heavier mechanism next to it would be incoherent.

- **`std::deque` instead of a chunked list.** Rejected as a half-measure: it stabilizes element *references* across `push_back`, but a reader iterating still walks the deque's spine, whose map an append can reallocate. It fixes the pointer hazard and leaves the iteration hazard.

- **Lock-free `graph_t` descent (fully lock-free writers).** Rejected on measurement, not on difficulty. The only reader that takes the graph lock on a hot path is the **terminus** (`deliver_local` → `graph_.find` → `find_ptr` → `std::shared_lock`, `graph.cpp:538`); deliveries themselves don't, since `write_impl` operates on an already-resolved `vertex_t*`. And `bench_forward_heap` measures the terminus at **9 allocations / 937 bytes** per resolve. Removing a 3 ns lock from a path that allocates 937 bytes is invisible — roughly 1% of a path this change cannot otherwise improve — in exchange for epoch or hazard-pointer reclamation across the whole L4 surface. If it is ever wanted, it is its own ADR with its own evidence.

- **Serializing creates by holding `map_mutex_` unique across the factory call.** Rejected: it widens the graph's global lock to cover socket construction and transport-plane state, coupling two layers and lengthening a hold that currently blocks every terminus resolution device-wide.

## Consequences

- **#512's cheapest invalidation model becomes available.** With slot addresses permanently stable, an ADR-0062 forward cache can hold a `const child_t*` — the registry **slot** — rather than a `transport_t*`. Teardown nulls `slot->link` in place, so a stale cache reads `nullptr`: the same clean miss as today, at one dereference instead of a scan. No second generation concept, no O(links × labels) teardown sweep, no forfeiting the win. **The tombstone becomes the invalidation.** This is why the container decision must precede #512's.

- **The forward path is unchanged and must stay so.** It already avoids the registry for the inbound mount run (#525, the run is carried on the link's receiver ctx), and its remaining `by_segments` scan is a plain traversal — a chunked list traverses as a linked walk instead of a contiguous one, which may cost a little locality. That must be measured against `bench_forward_demux`, not assumed: current baseline is ~112 ns fixed plus ~3.9 ns per registered link.

- **This class is currently unpoliced.** The `tsan` CI job exists but does not exercise concurrent creates. A TSan test that hammers create/remove against a live forward stream lands with this change, or the invariant is only asserted in prose.

- **`transport_vertex_t` gains its first synchronization.** Anything called under the control-plane trait must not re-enter the graph's mutation APIs or block — the same discipline `graph.hpp:449` already documents for resolvers, and sharper here if the trait resolves to an interrupt-disable critical section.

- **ADR-0061's "immutable after setup" premise is formally retired.** Its erratum already records that the registry mutates at runtime; this ADR is where that fact acquires a mechanism instead of a caveat.
