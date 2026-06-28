<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0004 — Remote operation addressing: path-as-route + the `FWD`/`FIELD` frames

| Field | Value |
| ---- | ---- |
| **RFC** | 0004 |
| **Title** | Remote operation addressing: path-as-route + the `FWD`/`FIELD` frames |
| **Status** | draft |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-06-28 |
| **Comment window closes** | 2026-07-12 (≥ 14 days) |
| **Tracking issue** | [#125](https://github.com/avatarsd-llc/libtracer/issues/125) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |

## Summary

`docs/spec/v1.md` §3 (the wire encoding of a **remote** `read`/`write`/`await` against a `path:field`) is stubbed ("to be written"). Locally the three primitives are direct router calls and never hit the wire; the wire only appears for a *remote* operation across a transport, and today the only remote mechanism is a bridge **mounting** inbound data under a fixed prefix ([reference/04](../../reference/04-communication-flows.md) §bridge republish). There is no frame for "operate on an **arbitrary** remote `path:field`," so a web UI cannot `read`/`write`/`await`/`subscribe` a vertex behind another node — which blocks the TypeScript client SDK higher operations (#56, [ADR-0034](../../adr/0034-typescript-client-sdk.md)), the declarative reconciler (#58), and transport-as-vertex orchestration (#83).

This RFC fills §3 with the smallest design consistent with the accepted model:

- **Path-as-route.** A remote endpoint is addressed by its *full path from the caller's own root, walking through transport vertices*. A transport/connection vertex ([ADR-0027](../../adr/0027-transport-and-connections-are-vertices.md)) **mounts its peer's graph under itself**: its `:`-facets are the link's own control, its `/`-subtree is the peer's tree. Routing is **hop-by-hop source-routing** — each transport vertex strips its own leading segment and forwards the unresolved remainder to its peer.
- **`FWD` (`0x0F`)** — a self-describing frame `FWD{ op∈{READ,WRITE,AWAIT}, PATH suffix, FIELD? selector, payload? }` that carries the remaining route + the operation. Source-routed, so it needs **none** of `ROUTER`'s dedup/`MAX_HOPS` machinery.
- **`FIELD` (`0x10`)** — encodes the `:field` tail (`:subscribers[]`, `:settings.x`) that `PATH` (NAME-segments only) cannot.
- **Replies are connection-oriented** — a `READ`/`AWAIT` result (or a `WRITE` status) retraces the *same bidirectional link*, hop-by-hop. There is **no end-to-end correlation-id** in the protocol; request/reply matching on a link is the **transport's** concern.

`subscribe` remains a `WRITE` to `:subscribers[]` ([ADR-0006](../../adr/0006-read-write-await-api-no-connect.md)/[ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md)); `ROUTER` (`0x0D`) is **unchanged** and keeps its role on the fanout/delivery side.

## Motivation

The whole point of the reference suite is cross-implementation interop. A second implementer (the TS client, #123) can build a `VALUE`/`SUBSCRIBER`/`PATH` and decode a delivered `VALUE`, but it **cannot express "read `/sensor/temp` on the device behind this WebSocket"** — there is no wire frame for it. The audit in #123 surfaced this precisely: spec §1–§3 are stubbed, no conformance vector carries a remote-operation envelope, and the C++ reference only mounts inbound data. Until §3 exists:

- the web UI cannot read/write/await/subscribe a remote vertex (the browser↔robot thesis of [ADR-0031](../../adr/0031-direct-browser-to-robot-binding-and-webtransport.md) has no wire);
- producer-holds fan-out to a **remote** `target` ([ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md)) has no carrier beyond a fixed mount — a producer cannot deliver to an arbitrary consumer-named target across a transport;
- the reconciler (#58) and remote `:children[]` creation (#82/#83) have no addressing primitive.

This is the keystone wire gap for everything "remote."

## Proposed change — **Proposed (open for comment)**

### A. Path-as-route (normative model)

A remote operation is `read`/`write`/`await` (and `subscribe`/QoS as field-writes) issued against a path that **traverses transport vertices**. Per [ADR-0027](../../adr/0027-transport-and-connections-are-vertices.md), a transport/connection is a `/` vertex; this RFC fixes its **dual nature**:

- a transport vertex's **`:`-facets** (`:settings`, `:stats`, `:acl`, `:children`, `:status`) are the link's *own* control surface, resolved **locally**;
- a transport vertex's **`/`-subtree is its peer's graph, mounted** — any `/`-segment below it is **not local**; it is the address of a vertex *on the peer*, reached by forwarding the unresolved suffix.

Example (from a web UI rooted at its own node):

```
/net/<ws://board-ip>/can[0]/ow/<temp_sensor>
└─ local ws connection vertex ──┘ │      │
   forwards "/can[0]/ow/<temp_sensor>" ───┘      │
   over the ws link to the board                 │
        board resolves "can[0]" (its CAN vertex), │
        forwards "/ow/<temp_sensor>" over CAN bus 0 ┘
            the 1-Wire bus resolves "<temp_sensor>"
```

Segments already carry the identifiers — `can[0]` is the bus number; `<temp_sensor>` is the device id deduced from CAN advertisement — so **the path needs no separate name or destination field**: the path-suffix *is* the address.

**Consistency requirement.** The suffix a caller routes *through* a transport vertex MUST equal the prefix that vertex **mounts inbound data under** ([reference/04](../../reference/04-communication-flows.md) §bridge republish). Send-side routing and receive-side mounting are duals and MUST agree.

**Location-dependence (consequence, not a bug).** A path encodes the route; it is relative to the caller's root, like a URL or a filesystem mount path. The same physical vertex has different paths from different vantage points. Provenance a consumer needs still travels in the data per [RFC-0003](0003-bridged-wildcard-delivery-path.md), not inferred from the route.

### B. `FWD` — `0x0F` (forward / remote-operation frame)

`FWD` is the frame a transport vertex emits to its peer to carry an operation one hop onward. **Structured** (`opt.PL=1`), source-routed, children in order:

```
FWD (0x0F, PL=1) {
  VALUE   op            ; required, FIRST child — u8: READ=0, WRITE=1, AWAIT=2
  PATH    suffix        ; required — the UNRESOLVED remaining route (segments only)
  FIELD   selector      ; optional — the :field tail (§C); absent ⇒ the vertex itself
  <payload TLV>         ; required for WRITE (the value/SUBSCRIBER/SETTINGS/… to write);
                        ; MUST be absent for READ; for AWAIT see §D
  VALUE   await_timeout ; optional, AWAIT only — u64 ns; absent ⇒ implementation default
}
```

- The receiving transport vertex resolves the **first** segment of `suffix`. If that segment names one of *its own* transport children, it strips it and re-emits a `FWD` carrying the shortened `suffix` over that link (the next hop). When `suffix` resolves to a **local** vertex on this node, the op is applied there.
- `op` is the first child so a forwarder can dispatch without parsing the whole frame.
- `FWD` is **source-routed and loop-free by construction** (the path is explicit; a suffix that revisits a node is malformed → `ERROR=INVALID_PATH`). It therefore carries **no** `origin`/`hop_count`/dedup — those stay in `ROUTER` on the delivery side (§E).

### C. `FIELD` — `0x10` (control-plane selector)

The `:field` tail of an address. `PATH` (`0x06`) encodes only `/`-segment `NAME`s; `FIELD` encodes the chain after the `:` separator ([reference/03](../../reference/03-addressing.md)). **Structured** (`opt.PL=1`); one *level* per field-chain element, in order, depth ≤ 8:

```
FIELD (0x10, PL=1) {
  level_1 ... level_K            ; K ≤ 8
}
where each level =
  NAME    field_name            ; e.g. "subscribers", "settings", "deadline_ns"
  VALUE   index                 ; optional — u32 index for [N];
                                ;   absent + index_mode=ELEMENT ⇒ append/list "[]";
                                ;   index_mode=WILDCARD ⇒ "[*]" (subscriber-path targets only)
  VALUE   index_mode            ; optional — u8: SCALAR=0 (no index), ELEMENT=1 ("[N]"/"[]"),
                                ;   WILDCARD=2 ("[*]"); default SCALAR
```

`:subscribers[3]` → `FIELD{ NAME "subscribers", VALUE u32=3, VALUE u8 index_mode=ELEMENT }`.
`:subscribers[]` (append) → `FIELD{ NAME "subscribers", VALUE u8 index_mode=ELEMENT }` (no index VALUE).
`:settings.deadline_ns` → `FIELD{ NAME "settings", NAME "deadline_ns" }`.
`[*]` (`index_mode=WILDCARD`) MUST be rejected with `ERROR=INVALID_PATH` outside a subscriber-path context, mirroring [reference/03](../../reference/03-addressing.md).

`PATH` (`0x06`) is **untouched** — its "children MUST be NAME" invariant and every existing parser stand.

### D. Operation semantics + replies

| `op` | Payload | Reply (retraces the link) |
| ---- | ---- | ---- |
| `READ=0` | none | the addressed value as a TLV view, or `STATUS=ERROR(NOT_FOUND)` |
| `WRITE=1` | the TLV to write | `STATUS` (empty = OK) or `STATUS=ERROR(...)` |
| `AWAIT=2` | none (+ optional `await_timeout`) | the next write's TLV, or `STATUS=ERROR(TIMEOUT)` |

`subscribe` is a `WRITE` of a `SUBSCRIBER` to a `:subscribers[]` field — **no new op** ([ADR-0006](../../adr/0006-read-write-await-api-no-connect.md)). QoS is a `WRITE` of `SETTINGS` to `:settings…`.

**Replies are connection-oriented.** A reply retraces the *same bidirectional link* the request arrived on, hop-by-hop; each transport vertex holds the per-hop request state needed to route the reply back down the link it came from. There is **no end-to-end correlation-id** in any core TLV.

**Concurrency / request-reply matching is the transport's concern.** When a link carries multiple in-flight `FWD`s, matching replies to requests is defined by **that transport's** framing (e.g. a transport-level stream tag for WebSocket; pipelining/ID-matching for CAN), *not* by a field in `FWD`. The core frame stays `{op, path, field, payload}`.

### E. Delivery / fanout is unchanged

Producer-holds fan-out ([ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md)) is unaffected: a stored `SUBSCRIBER`'s `target`, when it points across a transport, is the producer's **local connection-vertex path** that leads back to the consumer; delivering to it forwards over the link (the reverse of §A). Cyclic/multi-path *delivery* keeps using **`ROUTER` (`0x0D`)** dedup + `MAX_HOPS` ([ADR-0014](../../adr/0014-router-cycle-termination-hop-count.md)) and the [RFC-0003](0003-bridged-wildcard-delivery-path.md) concrete-path child. `FWD` (forward requests) and `ROUTER` (delivery) are different directions and stay separate frames.

### F. ACL across hops

A `FWD` is gated **twice**, by the existing ACL machinery ([ADR-0018](../../adr/0018-access-control-authorization-pluggable-subject-token.md)/[ADR-0020](../../adr/0020-acl-nfsv4-style-aces-with-inheritance.md)):

1. each intermediate **transport vertex's `:acl`** authorizes "may this `origin_peer_id` forward *through* me" (the forward right);
2. the **target vertex's `:acl`** authorizes the actual `READ`/`WRITE`/`AWAIT` at the final hop.

No new ACL machinery; `FWD` is subject to the same subject-token model as a local field-write.

### G. Type-code budget

`FWD=0x0F` and `FIELD=0x10` consume two of the sixteen `0x0F`–`0x1F` v1 fast-track slots ([reference/05](../../reference/05-protocol-tlvs.md)). Both are structured (`opt.PL=1`), each declaring its own purpose — no generic container is introduced.

### H. Conformance vectors (proposed)

Add to `tests/conformance/vectors/v1/`, so the 3-core machine (C++/TS/Rust) validates `FWD`/`FIELD` like every other TLV:

- `fwd-read-path` — `FWD{ op=READ, PATH /sensor/temp }`.
- `fwd-write-value` — `FWD{ op=WRITE, PATH /sensor/temp, VALUE u32 }`.
- `fwd-await-timeout` — `FWD{ op=AWAIT, PATH /sensor/temp, await_timeout=1e9 }`.
- `fwd-write-subscriber-field` — `FWD{ op=WRITE, PATH /sensor/temp, FIELD :subscribers[], SUBSCRIBER{...} }`.
- `fwd-routed-multihop` — `FWD{ op=READ, PATH /net/<peer>/can[0]/ow/x }` (the un-stripped suffix).
- `field-indexed`, `field-nested`, `field-append` — the three `FIELD` index-modes.
- `fwd-wildcard-reject` — `[*]` outside subscriber-path ⇒ `INVALID_PATH`.

## Considered options

- **An RPC verb + correlation-id envelope** (the #123 framing). **Rejected** — directly contradicts [ADR-0006](../../adr/0006-read-write-await-api-no-connect.md) ("no connect/subscribe; radical minimalism") and [ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md) ("a subscription *is* a write"). `FWD` carries the three *native* primitives (not a new verb layer) and **no** end-to-end correlation-id, so it is the API on the wire, not RPC.
- **Carry the target in `ROUTER`** (extend `0x0D` with a forward-path child). **Rejected** — pulls dedup/`MAX_HOPS` into a source-routed request that is loop-free by construction; conflates the request direction with the delivery direction. One overloaded envelope is worse than two purpose-built frames.
- **Fold the `:field` chain into `PATH`** (extend `0x06`). **Rejected** — breaks `PATH`'s "children MUST be NAME" invariant and touches every existing `PATH`/`SUBSCRIBER`/`ROUTER` parser. A separate `FIELD` keeps the existing 3-core machine unperturbed for one extra type code.
- **Reduce `read`/`await` to transient-subscribe writes** (so `FWD` is write-only, no `op`). **Rejected** — over-clever: a trivial read would have to mint a reply-target and one-shot flags. An explicit `op` of the three blessed primitives is simpler and still ADR-0006-clean.
- **A bare `[PATH][payload]` transport convention** (no new type). **Rejected** — not self-describing, so invisible to the conformance/cross-core machine that has caught every regression; each transport would re-implement the rule.
- **A separate global destination name / address layer.** **Rejected** — the path already encodes the route and the segment ids; a second naming layer is redundant (the load-bearing insight of this RFC).

## Consequences

- **The remote surface is tiny:** two structured TLVs (`FWD`, `FIELD`) + the path-as-route rule. `read`/`write`/`await`/`subscribe`/delivery all reduce to "route a payload+op to a path, hop-by-hop"; `subscribe`/QoS are field-writes; replies retrace the link.
- **Unblocks** the TS client higher ops (#56), browser↔robot ([ADR-0031](../../adr/0031-direct-browser-to-robot-binding-and-webtransport.md)), the reconciler (#58), and remote `:children[]` (#82/#83).
- **`ROUTER` and all existing TLVs are unchanged;** the 3-core conformance machine extends by adding vectors, not by reshaping anything.
- **Each transport spec must define its own request/reply matching** (the one cost of keeping `FWD` pure) — `transport_ws` and `transport_can` reference docs gain a "remote-op multiplexing" section.
- **`PATH` parsers are untouched** — `:field` lives only in the new `FIELD` selector.

## Open questions (for the comment window)

1. **Reply framing.** Is a reply a bare result TLV (`VALUE`/`STATUS`) with the per-hop state supplying "which request," or a thin `FWD`-reply wrapper? (Leaning bare TLV — the link state already identifies the pending request.)
2. **`await_timeout` default + cap** — implementation-default vs a normative bound.
3. **Forward-right delegation** — does an intermediate hop forward under the *original* `origin_peer_id` (end-to-end identity) or re-originate as itself at each hop? (Affects §F's first ACL check; leaning end-to-end identity preserved, hop authorizes by it.)
4. **`FWD` over a non-connection transport** (e.g. a one-shot datagram) — does path-as-route require a bidirectional link, or can a `READ` reply use a caller-named reply path as a fallback when the link is unidirectional?

## Relates

- [ADR-0006](../../adr/0006-read-write-await-api-no-connect.md) — read/write/await, no connect/subscribe (the verb set `FWD` carries).
- [ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md) — consumer-as-client / subscription is a write.
- [ADR-0027](../../adr/0027-transport-and-connections-are-vertices.md) — transports/connections are vertices (the mount points).
- [ADR-0031](../../adr/0031-direct-browser-to-robot-binding-and-webtransport.md) — browser↔robot (the consumer of this).
- [ADR-0034](../../adr/0034-typescript-client-sdk.md) — the TS client SDK whose higher ops this unblocks.
- [RFC-0003](0003-bridged-wildcard-delivery-path.md) — concrete-path delivery (the delivery-side companion).
- [reference/03](../../reference/03-addressing.md) (addressing grammar), [reference/04](../../reference/04-communication-flows.md) (flows + mount), [reference/05](../../reference/05-protocol-tlvs.md) (TLV registry).
