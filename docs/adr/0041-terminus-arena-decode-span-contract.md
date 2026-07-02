# The terminus reads a flat arena tree, not `tlv_t` — `wire::decode_into`, the borrowed-span contract, span-aliased `path_key`, and trailer-sliced stores

Status: accepted. **Implements [ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) invariant #5 / [ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md) §3** (the terminus arena), pinning the design grilled 2026-07-02. Brick 5 of the #83 Stage-2 flip.

## Context

The terminus is the one place a node must read the whole FWD tree ([ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) inv. #1 confines full decode there). Today it pays the `wire::decode` `std::vector<tlv_t>` spine (~8–12 heap allocs per frame) and then re-materializes bytes it already has: `path_key` re-emits every `dst` NAME, the reply head stages the route bytes through **four** copies (`encode` → `head_children` → `head` → segment), and a WRITE stores the payload via `encode` + `over_bytes` (two copies — and `encode` re-emits the trailer, violating the *"stored TLVs are trailer-less"* convention of [ADR-0035](0035-implementing-rfc-0004-remote-operation-addressing.md)).

Two shapes were weighed: **(A)** a general `tlv_view` concept templating the codec over both backings, and **(B)** a terminus-local arena with the reply built from byte spans, leaving the shared `tlv_t` codec untouched. **B was chosen** (maintainer call): smaller blast radius, faster (spans, no re-encode), and it does not template the cross-core codec type that three implementations keep byte-identical. A is unnecessary generality — no reader outside the terminus needs the arena.

## Decision

### 1. `wire::decode_into(span, std::pmr::memory_resource&) → tlv_arena_t`

A new decoder (own header, `tlv_arena.hpp` — `frame.hpp`/`tlv_t`/`decode`/`encode` are **untouched**) that parses a frame into a **flat, pre-order array of arena nodes** drawn from the injected resource. Each node holds *structure only*; every byte span points into the **inbound frame**:

- `type`, `opt`;
- `wire` — the TLV's header+body span, **trailer excluded** (for whole-TLV copies: the stored value, the route bytes);
- `body` — the body span (payload for opaque nodes; the children region for `PL=1` nodes);
- `end` — one-past-the-last-descendant index (pre-order subtree encoding: children of `i` start at `i+1`; iterate `j = i+1; while (j < node[i].end) j = node[j].end`);
- `canonical_path` — for a PATH node, true iff every child header is exactly `02 00 <u16 len>` (a bare canonical NAME).

Same validation as `decode` (bounds, reserved bits, `kMaxDepth`, CRC via the two-span overloads), iterative, no recursion. The arena borrows both the resource's memory and the input frame; it is a *resolve-scoped* object, never stored.

### 2. The borrowed-span contract (the review rule)

**A span into the inbound frame may be *read*, *copied once to its owner*, or *sub-viewed off a refcounted owner* — it may never be stored as a borrowed span.** Applied:

| datum | treatment |
| --- | --- |
| dispatch reads (op, timeout, field walk) | raw span, zero-copy |
| vertex lookup key | **span-aliased** (below), zero-copy |
| reply head route bytes | copied **once**, directly into the head segment |
| WRITE stored value | ownership transfer ⇒ `over_bytes(node.wire)` — **one** copy, trailer-sliced by construction |
| remote-subscriber `return_route` | ownership transfer ⇒ **one** copy at subscribe (into a refcounted segment — staged follow-up so each later full-route delivery *ropes* the stored route instead of copying it: O(deliveries) → O(1) copies, and in-flight ropes keep it alive across unsubscribe) |
| `field_path_t` step names | stay owned strings — control-plane frequency, mostly SSO; a resolve-local `string_view` variant is a profiling-gated non-goal (`field_path_t` is shared with long-lived `path_t`) |

When the [ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) §4 refcounted receiver seam lands (M5+), the WRITE store may become a **subview of the refcounted frame** (zero-copy) under a copy-small/reference-big threshold (the subview pins the whole frame until the next write — amplification is the price; threshold via `:settings`).

### 3. Span-aliased `path_key` — the frame *is* the key

`path_key`'s output (`02 00 <len LE> <bytes>` per NAME) is **byte-identical to a canonical PATH body**. So for a `canonical_path` node, `graph_.find(node.body)` dispatches with **zero key materialization** — the flag is computed for free during the arena walk (it reads every child header anyway). Non-canonical PATHs (a foreign encoder using LL-widened or trailer-carrying NAMEs) fall back to the re-emit. Our own encoders always produce the canonical form, so the common case pays nothing.

### 4. Trailer-sliced stores (behavior fix, ratified)

The stored WRITE value and the stored return route copy `node.wire` — header+body, **trailer excluded by construction**. This *fixes* the existing violation where `encode(*payload)` re-emitted an arriving CRC trailer into the stored value, contradicting the trailer-less-at-rest convention. Stored bytes change for CRC-carrying writes (they lose the trailer — the convention's intent).

### 5. The resolver walks the arena; the `tlv_t` overloads are removed

`op_resolver_t::resolve` is **rewritten** over `tlv_arena_t` (maintainer: the rewrite is desired, not avoided). The `resolve(const tlv_t&)` overloads are **deleted** (public API change → `core/CHANGELOG.md`); callers (`fwd_router_t` terminus, tests, the FWD node server) migrate to `decode_into`. The reply head is **direct-emitted** into one exactly-sized segment (all lengths are known from node spans) — the four-stage staging dies. `fwd_router_t` gains the [ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md) §1 `std::pmr::memory_resource*` constructor parameter (defaulted to `get_default_resource()`); per terminus frame it fronts the resource with a stack-buffer `monotonic_buffer_resource`, so a typical terminus decode allocates **nothing anywhere** and large frames spill to the injected resource. Control frames (ADVERTISE/COMPACT/NACK) and the observability sinks (`inbound_cb_`/`reply_cb_`, test/SDK-facing) keep `wire::decode`/`tlv_t` — they are flow-setup/originator paths, allowed to allocate per [ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md).

### Deliberate non-goals

Roping small route bytes as borrowed views into the reply (a `view::borrow` costs a ~32 B control-block allocation to avoid a ~30 B copy — the copy wins, same lesson as the Brick-2 stack heads); templating the codec (option A); `field_path_t` string_view-ification.

## Consequences

- Terminus decode+dispatch: ~10–14 heap allocs → **0** (arena from the stack buffer / injected resource; span-aliased key); route bytes: 3–4 staged copies → **1** into the head segment; WRITE store: 2 copies → **1**, trailer-less; the READ reply payload stays the existing zero-copy refcount-roped view.
- New surface: `wire::decode_into`, `tlv_arena_t`/`arena_tlv_t` (`tlv_arena.hpp`); `fwd_router_t(graph, memory_resource*)`. Removed: `op_resolver_t::resolve(const tlv_t&, …)` overloads. `wire::decode`/`encode`/`tlv_t`/conformance vectors: byte-for-byte untouched.
- Equivalence is testable: for every conformance vector, `decode` and `decode_into` must agree node-for-node (type/opt/payload bytes/trailer handling) — the arena is gated by the same vectors as the tree decoder.
- The heap bench gains a terminus mode (armed window around one `resolve`) reporting the alloc count — decode+dispatch at 0, reply construction O(1), the ownership copies visible and bounded.
