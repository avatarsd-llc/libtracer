# Implementing RFC-0004 (remote operation addressing): `FWD`/`FIELD` in `tr::wire`, hop-by-hop forwarding in the router, zero-copy `src` accumulation, the route-handle inside the transport

Status: accepted. Records *how* the reference cores (C++/TS/Rust) implement the now-accepted
[RFC-0004](../spec/rfcs/0004-remote-operation-addressing.md) (path-as-route, `FWD` `0x0F` /
`FIELD` `0x10`, accumulated return-route, the route-handle). RFC-0004 fixes the *wire* (the
cross-implementer contract); this ADR fixes the *reference-impl* shape and the incremental
slicing, so the cross-core conformance machine ([ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md)/
[ADR-0032](0032-continuous-cross-core-perf-conformance-matrix.md)) validates each step.

## Context

RFC-0004 is accepted but unimplemented. It is the keystone "remote" wire surface and unblocks
the core orchestration (#59), in-band remote `:children[]` (#82/#83), the reconciler (#58), and
browser↔robot (#92). It must land in all three cores **without** reshaping any existing TLV, and
it leans on machinery that already exists (the rope substrate; `transport_can`'s `identity↔path`
map). The risk is doing it big-bang; the mitigation is to slice it so each piece is cross-validated.

## Decision

**Place each part at its natural layer (six-layer model, CLAUDE.md namespaces) and land it in
conformance-validated slices.**

### Layer placement

- **`FWD`/`FIELD` encode + decode → `tr::wire` (L2/L3).** Two new structured type codes in the
  registry, parsed by the *same* iterative depth-capped parser as every other TLV. No new codec
  engine; `FWD`/`FIELD` are TLVs. This is what the conformance vectors gate.
- **Forward resolution → the L4 router (`tr::graph`) + the `tr::net` transport seam.** On a `FWD`,
  resolve the first `dst` segment against the existing **PATH-keyed dispatch table**. If it names a
  local vertex, apply the op (`READ`/`WRITE`/`AWAIT`) + `FIELD` selector and build the `REPLY`. If
  it names a **transport** child vertex ([ADR-0027](0027-transport-and-connections-are-vertices.md)),
  strip it and hand the shortened `FWD` to that transport for the next hop. dst-resolution is the
  router's existing dispatch, made transport-aware.
- **`src` accumulation → zero-copy rope head-prepend (`rope.hpp`, `tr::view` L1).** Each forwarding
  hop prepends its inbound-link `NAME` to `src` as a rope head segment; existing bytes never move,
  only the outer `PATH`/`FWD` length is rewritten (the trailer is recomputed at egress regardless).
- **Route-handle (the per-link label map) → inside each transport.** `transport_can` **already** holds
  the `identity↔path` map + advertise (#55, [ADR-0030](0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md))
  — that *is* the CAN route-handle; **no new code there**. `transport_ws` gains a per-link label
  table + the advertise annotation + the `SUBSCRIBER.qos_settings.delivery_compact` opt-in.

### Incremental slices (each gated by the 3-core machine + diff-fuzz)

1. **`FWD`/`FIELD` codec + conformance vectors** (the `fwd-*` / `field-*` / `fwd-reply-*` cases in
   RFC-0004 §H). Encode/decode in C++/TS/Rust, cross-validated like every TLV, and added to
   `diff_fuzz.py`. *This is the first slice — low risk, immediately validated.*
2. **Local op resolution + the `FWD`-`REPLY`.** A node resolves a `FWD` to a *local* vertex, applies
   `READ`/`WRITE`/`AWAIT` (+ `FIELD`), and builds `REPLY{ kind∈{RESULT,ERROR} }`. Host-testable, no
   transport.
   - **Zero-copy reply rule — the read result is a nested sub-TLV, never flattened.** The reply payload
     (a `VALUE` for data; or a `PL=1` structured TLV of the field's child TLVs for a `:field` — e.g.
     `:subscribers[]` → the populated `SUBSCRIBER` slot TLVs in order, `:settings` → `SETTINGS`,
     `:schema` → `POINT`) is the read result **nested as a child** of `FWD{REPLY}`. The builder MUST
     compose it as a **rope**: a small *freshly built head* (`FWD{REPLY}` header + `op` + `dst` route +
     `kind`) **prepended to refcount-clones of the vertex's stored payload view(s)** — `read` returns a
     view (refcount += 1, reference/04), so **no bytes are copied**. For `:subscribers[]` this is a rope
     of the N slot views under a fresh `PL=1` wrapper header (the same scatter-gather that ships a GB
     RTSP frame-group). Nesting is clean because stored TLVs are **trailer-less** (the wire trailer is an
     egress/ingress artifact), so the nested child needs no trailer-strip copy and the `FWD{REPLY}`'s own
     egress trailer covers the whole rope. The reply therefore rides the existing scatter-gather send
     (`transport_t::send(iov)` / `rope::to_iovec()`) with **zero flatten**. A data read and a `:field`
     read are identical here — only the child TLV type differs. **Do not** build the reply by serializing
     the payload into a fresh buffer.
3. **Multi-hop forwarding + `src` accumulation over `transport_ws`.** Extend the existing
   `ws_interop_server` harness to a graph-backed *forwarding* node; assert `dst` shrinks / `src`
   grows byte-exactly and the reply source-routes home. Integration-tested.
4. **Route-handle: advertise + label swap.** CAN: reuse the existing map. ws: the label table +
   `delivery_compact` + advertise-on-(re)connect self-heal. Implemented as `tr::net::route_handle_t`
   (the per-link `label ↔ route` tables) owned by `fwd_router_t`, with transport-plane
   `ADVERTISE`/`COMPACT`/`HANDLE_NACK` frames (`0x11`–`0x13`) riding the link alongside `FWD`; the
   per-link **u16 label** is swapped each hop. A `delivery_compact`-flagged stream amortizes its
   full return route to the label; one-shot/cold flows stay stateless. Integration-tested over live
   `transport_ws` (`core/tests/fwd_compact_test.cpp`): byte-delta vs full-route `FWD{WRITE}`,
   byte-exact ordered delivery, stale-label drop + `HANDLE_NACK`, re-advertise self-heal, and
   zero label state for a parallel non-compact flow.

   **Slice-4 completion — the producer fan-out (#136).** Slice 4 landed the route-handle *mechanism*
   (`advertise`/`send_compact`/NACK) but left it driven explicitly by the test; nothing read a
   subscriber's `delivery_compact` to *originate* deliveries. The completion wires the producer side:
   an inbound `:subscribers[]` `FWD{WRITE}` now binds a **remote subscriber** carrying the request's
   accumulated return route (`src`) + inbound link (`graph_t::add_remote_subscriber`, fed by
   `op_resolver_t::resolve(fwd, inbound_link)`), and `graph_t::fan_out` hands each delivery to an
   **injected sink** the `fwd_router_t` registers (`set_remote_delivery_sink`). The sink emits a
   full-route `FWD{WRITE}` by default, or — for a `delivery_compact` subscriber — **auto-promotes**:
   `route_handle_t::ensure_egress` advertises a label once per `(link, route)` flow, then streams
   `COMPACT`; `clear_link` on reconnect drops the binding so the next delivery re-advertises (lazy
   self-heal, no transport "up" event). The sink is an opaque `std::function`, so L4 (`graph`) gains
   no dependency on `tr::net`. A **transient-local** producer (`durability == 1`) latches its LKV to a
   fresh subscriber on subscribe. Tested in `core/tests/fwd_fanout_test.cpp` (full-route routing, the
   latch, the auto-promote byte-delta, reconnect re-advertise, a TSan writer×`clear_link` race) and
   end-to-end against the TS client over a live socket (`fwd_node_server.cpp` no longer hand-rolls a
   delivery — the real fan-out drives it). Delivery semantics are described in
   [reference/05 §SUBSCRIBER](../reference/05-protocol-tlvs.md); no new wire bytes, so no new vector.

### Cross-cutting

- **No `ROUTER` change.** `FWD` is a sibling frame; `ROUTER`'s dedup/`MAX_HOPS` stay on the
  multi-path delivery side (RFC-0004 §E).
- **Error-reply codes depend on the ERROR registry.** `REPLY{ kind=ERROR }` carries
  `STATUS=ERROR(...)`, whose code set is being pinned by **RFC-0001 §C/E (#8)**. Until #8 lands, use a
  provisional `STATUS` payload; finalize the codes when #8 does. (The only cross-RFC dependency.)

## Considered options

- **A standalone forwarding module vs. reuse the router dispatch.** Reuse — `dst` resolution *is* a
  path-dispatch lookup the router already does; a transport-child segment is the only new branch.
- **Copy-prepend `src` vs. rope head-prepend.** Rope — zero-copy is RFC-mandated and the whole reason
  the accumulated-route model is cheap; a copy-prepend would reintroduce per-hop O(route) copies.
- **Big-bang vs. incremental.** Incremental — the cross-core machine validates each slice exactly as it
  did for the transports and the third core; big-bang would land an unvalidated keystone.
- **A new CAN map for the route-handle vs. reuse `transport_can`'s.** Reuse — RFC-0004's route-handle
  *is* header-elision generalized; CAN already implements it.

## Consequences

- The codec slice lands first and is cross-validated immediately — the keystone enters through the
  lowest-risk door.
- The router becomes transport-aware (a `dst` segment may name a transport vertex) — a defined
  extension of [ADR-0027](0027-transport-and-connections-are-vertices.md)'s dispatch model, not a new plane.
- `transport_can`'s map is reused as the CAN route-handle — no duplication; `transport_ws` gains the
  label table.
- Error-reply codes wait on #8; everything else proceeds.
- Unblocks #59 (implement ADR-0026/0027), #82/#83 (remote `:children[]`), #58 (reconciler), #92.

## Relates

- [RFC-0004](../spec/rfcs/0004-remote-operation-addressing.md) — the accepted wire spec this implements.
- [ADR-0006](0006-read-write-await-api-no-connect.md)/[ADR-0026](0026-consumer-initiated-subscription-client-write.md)/[ADR-0027](0027-transport-and-connections-are-vertices.md) — the model RFC-0004 realizes.
- [ADR-0022](0022-transport-framing-modes-elided-full-tlv-advertise.md)/[ADR-0030](0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md) — header-elision / advertise (the route-handle's basis).
- [ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md)/[ADR-0032](0032-continuous-cross-core-perf-conformance-matrix.md) — the cross-core machine that gates each slice.
- Issues #59, #82, #83, #58, #92 (unblocked); #8 (ERROR registry — error-reply code dependency).
