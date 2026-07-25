# Resolve once, then dereference: a label binding holds the *resolved* target — a generation-stamped `vertex_handle_t` or a `transport_t*` — not a path and a link NAME

Status: proposed. **Refines [RFC-0004](../spec/rfcs/0004-remote-operation-addressing.md) §E.1 / [ADR-0035](0035-implementing-rfc-0004-remote-operation-addressing.md)** — it changes what a `handle_binding_t` *holds*, not the label plane, so the wire bytes are byte-identical and **no RFC applies**. Upholds [RFC-0009](../spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md) §B.6 (re-virginize: a revived vertex inherits nothing) and [ADR-0056](0056-vertex-handle-infallible-register.md) (a handle is opaque and pointer-sized). Depends on [#494](https://github.com/avatarsd-llc/libtracer/issues/494) having made `link_down` the single funnel for link teardown. Grounded by a `/grill-with-docs` session against the code.

## Context

RFC-0004 already delivers "resolve the route once, then address it by a small token": a producer `advertise`s a `label ↔ route` binding over a link, and every subsequent sample rides as `COMPACT{label, payload}` with the route absent from the wire. Each hop swaps the label MPLS-style; the terminus expands it and applies the write.

What the label eliminated is the **wire** cost. It did not eliminate the **resolution** cost, because a binding stores bytes and names rather than resolutions:

- **Terminus** (`fwd_router.cpp:488-492`): `deliver_local(binding->local_route, …)` — and `local_route` is, by its own doc, "the local dst PATH TLV bytes **to resolve** + write" (`route_handle.hpp:56`). Every delivery on an established flow re-walks the path.
- **Forwarding hop** (`fwd_router.cpp:497`): `registry_.by_name(binding->down_link)` — `down_link` is a `std::string` (`route_handle.hpp:53`), so every compact frame pays a linear registry scan. The `bench_forward_demux` baseline measures that scan at **~1.0–1.4 ns per registered link**, linear in table size.

So an established, hot flow — the case delivery compaction exists for — still re-resolves both ends on every frame. The resolution is *known* and *stable*; only its cached form is missing.

## Decision

**A binding holds the resolved target.** `handle_binding_t` gains, alongside (not replacing) what it stores today:

1. **Forwarding hop** — a dereference instead of a registry scan. The originally-stated invalidation argument was wrong (see the erratum below); **as implemented, the binding caches the registry SLOT (`const child_registry_t::child_t*`), not the `transport_t*`** — teardown nulls `slot->link` in place, so a stale cache reads `nullptr`. The tombstone *is* the invalidation.

2. **Terminus** — a **generation-stamped** `vertex_handle_t`: the pair `(handle, generation)`, delivered only when the vertex's current generation matches.

The generation stamp is load-bearing and is the non-obvious half of this decision. A `vertex_handle_t` never dangles — the vertex map is pinned, pointer-stable and insert-only (`graph.hpp:46-57`) — but `retire()` **re-virginizes the same object**: a retired-then-revived path keeps its `vertex_t` while shedding all owner state, ACEs included (RFC-0009 §B.6; `retire_test.cpp`'s confused-deputy case). Today's per-delivery re-resolution is *accidentally* safe against this: a retired path fails to resolve, so delivery stops. Caching a bare handle would therefore **introduce the bug that re-resolution currently prevents** — an established subscriber flow continuing to deliver into a path after it was retired and re-created for a different owner. A misdelivery across an ownership boundary is a confused deputy, not merely a stale read.

**A generation mismatch is treated as exactly what it is: a stale label.** It drops the frame, fires the existing `on_stale_label` observer, and NACKs upstream to prompt a re-advertise — the RFC-0004 §E.1 self-heal path, already implemented and tested (`fwd_router.cpp:477-486`). No second invalidation mechanism is introduced, and the flow re-binds itself without a handshake.

**A binding caches the address, never the authorization.** ACL gating stays per-operation on the resolved vertex. A label is an addressing shortcut; it must not become a capability that outlives the grant that created it.

## Considered options

- **Retire evicts bindings directly**, via a reverse path→labels index. **Rejected:** exact and eager, but it requires maintaining a reverse index on every bind and puts that work on `retire()`'s path under the map lock — a cost paid by every retire to serve the compact-flow minority, and a second invalidation mechanism alongside the stale-label one that already works.
- **Cache the `transport_t*` only**, leaving the terminus re-resolving. **Rejected as the whole answer:** it sidesteps the generation question but forfeits the same-host dereference, which is the larger of the two wins (a full path walk, versus a short-string scan).
- **Encode the registry key as raw TLV bytes and `memcmp`** — the originating idea. **Rejected on conformance:** a `NAME` may legally be emitted with `opt.LL=1`, and `opt` also carries `TS`/`CR`, so the same path has multiple valid encodings; receivers MUST accept every `LL`/`CW`/`TF` variant (CONTEXT §Capability negotiation). A raw-byte key silently fails to route a conforming peer. Caching the *resolution* rather than matching the *encoding* gets the same win without the conformance hazard.
- **Do nothing** — the measured strip-K delta is ~3 ns on an ~82 ns hop. **Rejected:** this is not about strip-K's delta. It removes work from the steady state of every established flow, and it is what makes the ADR-0061 descent affordable *by construction* — a hot flow bypasses the demux walk entirely rather than paying a cheaper version of it.

## Erratum (2026-07-25, found while implementing increment 2)

The Decision above claimed the cached `transport_t*` is "safe only because #494 made teardown funnel through `remove_child` → `link_down` → `clear_link`, which already drops every label binding on the departing link." **That is wrong.**

`route_handle_t::clear_link(L)` erases the tables *keyed by* link `L` — `L`'s own ingress and egress bindings. It does **not** touch a binding stored under a *different* link that merely *points at* `L`. And that is the normal shape of a forwarding binding: `bind_ingress(inbound_name, label, {down_link, out_label})` stores it under the **inbound** link while `down_link` names the **outbound** one.

So when the downstream link departs, an upstream binding retains `down_link = "B"`. Today that is harmless: the next delivery calls `by_name("B")`, gets `nullptr` (since #494 tombstones the entry), and drops. **Caching the pointer would convert that clean miss into a use-after-free** — re-introducing, in a second place, precisely the dangling-`transport_t*` class #494 closed in the registry.

The terminus half is unaffected: its generation stamp ([#511](https://github.com/avatarsd-llc/libtracer/pull/511)) is a different mechanism and does not rely on this claim.

**Options considered for the forward half:**

- **Generation-stamp the link too**, mirroring the vertex stamp: a binding holds `(transport_t*, link_generation)` and re-checks. Symmetric with the terminus half, but adds a second generation concept.
- **Sweep on teardown**: `remove_child` walks every link's bindings clearing any `down` matching the departing transport. Exact, but O(links × labels) on a path that already runs under the graph locks `link_down` takes.
- **Cache the terminus only**, leaving the forward hop resolving by name. Forfeits the registry-scan win but keeps the larger terminus win.

**RESOLVED — a fourth option none of those named: cache the registry SLOT, not the link.** Teardown already nulls `slot->link` in place (the #494 tombstone), so a stale cached slot reads `nullptr` — the same clean miss an unresolved lookup gives, at one dereference instead of a scan. It needs **no second generation concept and no teardown sweep**, because the invalidation mechanism already exists and is simply read one level up.

That option was unavailable when this erratum was written: slot addresses were not stable, because `children_` was a `std::vector` whose `push_back` reallocated ([#521](https://github.com/avatarsd-llc/libtracer/issues/521) — measured: 16 of 17 slot addresses moved after later appends). [ADR-0063](0063-connection-table-lock-free-reads-trait-serialized-writes.md) made the table an append-only chunked list, and slot addresses permanently stable, which is what unblocked it. **The container decision therefore had to precede this one**, and that ordering is the real lesson of this erratum.

**Implemented** in increment 2 alongside a second finding this erratum did not anticipate: `route_handle_t::lookup_ingress` returned the binding **by value**, copying a `std::string` and a `std::vector` out of the label table on *every* frame — before anything checked whether the flow was already resolved. The steady-state lookup now returns `resolved_binding_t`, ~24 trivially copyable bytes with no route payload; the owning lookup is taken only on the cold re-resolve.

**Measured** (`bench/bench_compact_delivery.cpp`, host p50, branch vs `main` built in a separate worktree):

| | main | increment 2 |
| --- | ---: | ---: |
| warm terminus delivery | 298 ns | **202 ns** (−32%) |
| allocations per frame | 13 | **9** |
| bytes per frame | 655 | **443** |
| warm forwarding hop (512 B) | 202 ns | 180 ns (−11%) |

The forwarding gain is small because the scan it removes was already cheap at that link count; that path is dominated by `encode_compact`'s fresh vector and the COMPACT frame's own owning decode — **neither of which is resolution**, and which are the next lever here.

## Consequences

- **The forward-demux scan stops being the steady-state cost.** ADR-0061's strip-K descent is paid on a flow's first frame; established flows dereference. This weakens the case for optimizing the descent further, and should be stated in ADR-0061 so the two are not tuned independently.
- **`handle_binding_t` grows**, and the binding becomes invalidation-sensitive in two directions: link teardown (already handled by `clear_link`) and vertex retirement (the new generation check). Both funnel into the one stale-label path.
- **`vertex_t` needs a retire generation** — a counter bumped on retire, readable for comparison. This is new state on the vertex, so it is subject to the same per-vertex footprint scrutiny as [#388](https://github.com/avatarsd-llc/libtracer/issues/388)/[#392](https://github.com/avatarsd-llc/libtracer/issues/392); it should share a word with existing flags rather than adding one.
- **Concurrency:** the generation compare sits on a lock-free delivery read while `retire()` writes it, so the counter is atomic and the pairing is TSan-gated — the same gate RFC-0014 S5 already carries.
- **Bench gate:** `bench_forward_demux` extends with a compact-flow point, so the win is measured rather than asserted. `bench_forward_heap`'s `allocs=0` forward gate must continue to hold — a cached binding must not allocate on delivery.
- **The client-originated half is a separate, wire-visible surface and needs an RFC.** Letting a *client's* repeated `read`/`write`/`await` on one `dst` bind a label once — rather than only producer-side `delivery_compact` flows — is the full "every call becomes a dereference". It reverses who originates the `ADVERTISE`, so it is a wire-behavior change; replies are unaffected (`src` still accumulates hop-by-hop, so the return route needs nothing new). Tracked as [#504](https://github.com/avatarsd-llc/libtracer/issues/504); this ADR covers only the local, byte-identical half.
