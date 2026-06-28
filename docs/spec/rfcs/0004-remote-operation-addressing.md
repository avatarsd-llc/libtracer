<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0004 — Remote operation addressing: path-as-route + the `FWD`/`FIELD` frames

| Field | Value |
| ---- | ---- |
| **RFC** | 0004 |
| **Title** | Remote operation addressing: path-as-route + the `FWD`/`FIELD` frames |
| **Status** | **accepted** (2026-06-28) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-06-28 |
| **Accepted** | 2026-06-28 — maintainer/BDFL; no registered second-implementer to object, so the 14-day window is nominal (GOVERNANCE.md §Roles). Implementation tracked by ADR-0035. |
| **Tracking issue** | [#125](https://github.com/avatarsd-llc/libtracer/issues/125) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |

## Summary

`docs/spec/v1.md` §3 (the wire encoding of a **remote** `read`/`write`/`await` against a `path:field`) is stubbed ("to be written"). Locally the three primitives are direct router calls and never hit the wire; the wire only appears for a *remote* operation across a transport, and today the only remote mechanism is a bridge **mounting** inbound data under a fixed prefix ([reference/04](../../reference/04-communication-flows.md) §bridge republish). There is no frame for "operate on an **arbitrary** remote `path:field`," so a web UI cannot `read`/`write`/`await`/`subscribe` a vertex behind another node — which blocks the TypeScript client SDK higher operations (#56, [ADR-0034](../../adr/0034-typescript-client-sdk.md)), the declarative reconciler (#58), and transport-as-vertex orchestration (#83).

This RFC fills §3 with the smallest design consistent with the accepted model:

- **Path-as-route.** A remote endpoint is addressed by its *full path from the caller's own root, walking through transport vertices*. A transport/connection vertex ([ADR-0027](../../adr/0027-transport-and-connections-are-vertices.md)) **mounts its peer's graph under itself**: its `:`-facets are the link's own control, its `/`-subtree is the peer's tree. Routing is **hop-by-hop source-routing** — each transport vertex strips its own leading segment and forwards the unresolved remainder to its peer.
- **`FWD` (`0x0F`)** — a self-describing frame `FWD{ op∈{READ,WRITE,AWAIT,REPLY}, PATH dst, FIELD? selector, PATH src, payload? }`. `dst` (forward route) **shrinks** per hop; `src` (return route) **grows** per hop by a **zero-copy prepend** of the inbound-link `NAME`, so forwarders stay **stateless** and the reply self-routes home via `src`. Loop-free by construction → needs **none** of `ROUTER`'s dedup/`MAX_HOPS`.
- **`FIELD` (`0x10`)** — encodes the `:field` tail (`:subscribers[]`, `:settings.x`) that `PATH` (NAME-segments only) cannot.
- **Replies are stateless and source-routed back** — a `FWD{ op=REPLY, kind∈{RESULT,ERROR} }` whose `dst` is the accumulated `src`. **No end-to-end correlation-id** (matching a reply to a specific concurrent request *at* an endpoint is the **transport's** concern); works over unidirectional links; survives a hop reboot.
- **Two planes.** `READ`/`WRITE`/`AWAIT` are the **one-shot** plane (works on data *and* `:field` control). **Streaming never per-sample-remote-writes**: a consumer wires one `SUBSCRIBER`, then the producer **produces locally + flushes**, fanning out. A **delivery *is* a `FWD WRITE`** to the subscriber's data vertex (delivery-is-a-write, [ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md)), so a subscription delivery and a one-shot command are the same frame.

`subscribe` remains a `WRITE` to `:subscribers[]` ([ADR-0006](../../adr/0006-read-write-await-api-no-connect.md)/[ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md)); `ROUTER` (`0x0D`) is **unchanged** and earns its keep only on the *cyclic/multi-path* delivery side. The consumer-stored `SUBSCRIBER.target` *is* the `src` route its subscribe accumulated — producer-holds and this RFC are one mechanism.

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

`FWD` is the frame a transport vertex emits to its peer to carry an operation one hop onward. **Structured** (`opt.PL=1`); **source-routed forward**, **return-route-accumulated backward**; children in order:

```
FWD (0x0F, PL=1) {
  VALUE   op            ; required, FIRST child — u8: READ=0, WRITE=1, AWAIT=2, REPLY=3
  PATH    dst           ; required — UNRESOLVED forward route (segments only); SHRINKS per hop
  FIELD   selector      ; optional — the :field tail (§C), resolved at the final hop
  PATH    src           ; required — accumulated RETURN route; GROWS per hop (§D)
  VALUE   kind          ; REPLY only — u8: RESULT=0, ERROR=1
  <payload TLV>         ; WRITE: value/SUBSCRIBER/SETTINGS/… to write. READ/AWAIT: absent.
                        ; REPLY: the result (VALUE for a READ result; STATUS for WRITE-ack/ERROR).
  VALUE   await_timeout ; optional, AWAIT only — u64 ns; absent ⇒ 1 s default (reference impl)
}
```

- **Forward (source-routed).** The receiving transport vertex resolves the **first** segment of `dst`. If it names one of *its own* transport children, it strips that segment from `dst` and re-emits `FWD` over that link. When `dst` empties, it resolves to a **local** vertex here, and the op (+ `selector`) is applied.
- **Return-route accumulation (zero-copy prepend).** On *every* forwarding hop, the vertex **prepends to `src`** the one `NAME` that — in *this node's own* address space — names the link the `FWD` arrived on (the way back). Since `src` is a structured `PATH` (concatenated `NAME` children), the prepend is a **rope head-insert: existing bytes never move** — only the outer `PATH`/`FWD` `length` is rewritten (and the trailer is recomputed at egress per hop regardless, so no extra CRC cost). The originator **seeds `src` with its own reply endpoint**; when `dst` empties, `src` is the **complete reverse route** in per-node-local segments. The consumer therefore also receives the full source route — exactly the provenance [RFC-0003](0003-bridged-wildcard-delivery-path.md) wanted, now inherent.
- **REPLY** is itself a `FWD` routed *back*: `dst = src` (the accumulated return route), `kind ∈ {RESULT, ERROR}`, payload = the result. A reply expects no reply, so it **does not accumulate** `src`; the `src` child of a REPLY is still required and is set to the **responder's own endpoint** (the vertex that produced the result) — uniform with the other ops, available as provenance, unchanged hop-to-hop.
- **Terminus-reply asymmetry (load-bearing).** Each `src` segment is meaningful only **at the node that prepended it** (its local name for an inbound link), so the **terminus does *not* resolve `dst[0]` of the reply** — it emits the `FWD{REPLY}` (whose `dst` is the request's accumulated `src`) **unmodified over the link the request arrived on**. The **first reverse hop** performs the first `dst`-strip (by the same forward step), and so on back to the originator. A REPLY routes by the ordinary forward step but **never accumulates `src`**. This asymmetry is what makes the per-node-local return route compose correctly.
- `op` is the first child so a forwarder can dispatch without parsing the whole frame.
- `FWD` is **loop-free by construction** (the forward `dst` is explicit; a `dst` revisiting a node is malformed → `ERROR=INVALID_PATH`). It carries **no** `origin`/`hop_count`/dedup — those stay in `ROUTER` on the multi-path delivery side (§E).

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

| `op` | Payload | One-shot `REPLY` (`op=REPLY`, routed back via `src`) |
| ---- | ---- | ---- |
| `READ=0` | none | `kind=RESULT` + the value TLV, or `kind=ERROR` + `STATUS=ERROR(NOT_FOUND)` |
| `WRITE=1` | the TLV to write | `kind=RESULT` (empty/OK) or `kind=ERROR` + `STATUS=ERROR(...)` |
| `AWAIT=2` | none (+ optional `await_timeout`) | `kind=RESULT` + the next write's TLV, or `kind=ERROR` + `STATUS=ERROR(TIMEOUT)` |

`subscribe` is a `WRITE` of a `SUBSCRIBER` to a `:subscribers[]` field — **no new op** ([ADR-0006](../../adr/0006-read-write-await-api-no-connect.md)). QoS is a `WRITE` of `SETTINGS` to `:settings…`.

A `READ` of an **array `:field`** (e.g. `:subscribers[]`) returns its members wrapped in a **`POINT` (`0x07`)** — the established structured introspection-result container (the `:schema` `POINT` already carries `SUBSCRIBER` children) — whose children are the populated slot TLVs in slot order, each a zero-copy view (§E / ADR-0035 reply rule). A `READ` of a single slot (`:subscribers[3]`) or scalar field returns that TLV directly.

**Replies are stateless, source-routed back.** A one-shot reply is a `FWD{ op=REPLY }` whose `dst` is the `src` that was **accumulated on the way in** (§B). Forwarders hold **no per-hop request state** — the return route lives in the frame, so a hop may even reboot mid-operation and the reply still routes. The reply works over **unidirectional** transports (it does not need the inbound link to be bidirectional). The reply-`kind` is just **`{RESULT, ERROR}`**; there is **no `DELIVERY`/`KEEPALIVE`/`DIGEST` reply-kind** — those are not replies (see §E). A `kind=ERROR` reply's **payload is a `STATUS` TLV carrying one `ERROR` (`0x08`) child** ([RFC-0002](0002-protocol-error-model.md) error model); the concrete `ERROR` code set is pinned by the ERROR registry (RFC-0001 §C/E, [#8](https://github.com/avatarsd-llc/libtracer/issues/8)), so until #8 lands the `fwd-reply-error` vector uses a provisional `STATUS{ ERROR u8 }` shape and the codes finalize with #8.

**There is no end-to-end correlation-id.** The `src` route delivers a reply to the right *endpoint*; matching it to a *specific* outstanding request *at* that endpoint (e.g. a consumer with many concurrent `READ`s) is the **transport's** concern — a transport-level stream tag for WebSocket, pipelining/ID-matching for CAN — never a field in `FWD`. Route = inter-hop; tag = intra-endpoint; a route is not an id.

**Two planes — direct (one-shot) vs standing (streaming).** `FWD` `READ`/`WRITE`/`AWAIT` are the **one-shot/synchronous** plane: read a snapshot, write once, block for one. **Streaming data does *not* per-sample remote-write.** A consumer creates a local receiving endpoint and `WRITE`s one `SUBSCRIBER` into the producer's `:subscribers[]` (a control write, §C); thereafter the **producer produces locally and fans out** — fills its own endpoint's segment memory (rope-chaining slices for large/scatter data) and **flushes**, which delivers to each subscriber. The flush, not a client, drives every delivery.

**A delivery *is* a `FWD WRITE` (load-bearing).** When a producer fans out to a *remote* subscriber, that delivery **is** a `FWD{ op=WRITE, payload=VALUE }` routed to the subscriber's data vertex via the stored return-route. So a **subscription delivery and a one-shot command are the identical wire frame** — the only difference is whether a standing `SUBSCRIBER` produced it or a client issued it once; the target cannot (and per ADR-0026 claim 2 must not) tell them apart. Likewise **keepalive / digest / liveness / async-write-ack are ordinary `VALUE`/`STATUS` writes on the standing channel**, not reply-kinds.

### E. Delivery / fanout is unchanged

Producer-holds fan-out ([ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md)) is **unified** with this RFC, not changed by it: the `src` route a consumer's `subscribe`-`FWD` **accumulated on the way in** (§B) is exactly what the producer stores as the `SUBSCRIBER`'s `target`. Each later delivery is a `FWD{ op=WRITE, payload=VALUE }` source-routed back along that stored route — i.e. a delivery *is* the same primitive as a one-shot write (§D). For a delivery that crosses a **cyclic / multi-path** region of the mesh (where the same data could arrive two ways), the delivery `FWD` is wrapped in **`ROUTER` (`0x0D`)** for `(origin, ts)` dedup + `MAX_HOPS` ([ADR-0014](../../adr/0014-router-cycle-termination-hop-count.md)), carrying the [RFC-0003](0003-bridged-wildcard-delivery-path.md) concrete-path child. Strict source-routed deliveries (a single accumulated route) need no `ROUTER`; `ROUTER` earns its keep only where the topology folds.

### E.1 Delivery compaction — the route-handle (generalized header-elision)

Taken literally, "a delivery *is* a `FWD WRITE`" (§D) makes *every* streamed sample carry its full return route. For a 1 kHz, 4-byte sensor over a 3-hop path that is **~60 B of route on a 4 B payload (~16×)** — fine for one-shots, prohibitive for streams (and impossible on CAN, which has no room for a route at all). The fix is **not** a new mechanism: it is **header-elided framing ([ADR-0022](../../adr/0022-transport-framing-modes-elided-full-tlv-advertise.md)) generalized to every transport** — a compact **per-link label** that aliases an established delivery route.

- **Per-link label-switching.** A label is meaningful only on the link it was bound for; each forwarding hop **swaps** it (label-in → its own label-out / route), exactly as a CAN-ID is re-resolved against each bus's `identity↔path` map. There is **no** end-to-end handle — "matching is the transport's concern" (§D) applied to delivery.
- **Advertise-driven binding (generalize [ADR-0030](../../adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md)).** The upstream **advertises** the `label ↔ route` binding **in-band** when a flow starts; each hop learns `label → (downstream link, out-label)` and swaps. There is no setup handshake. **Re-advertise on (re)connect *is* the self-heal** (it is also what producer-holds already triggers on reconnect, [ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md)); a delivery bearing an unknown/stale label is dropped with an error that prompts re-advertise. On a **header-elided** transport this advertise *is* the existing id-assignment (`transport_can`, #55) — no new code.
- **Framing-mode decides whether labels are used (no global policy):**
  - **Header-elided transports (CAN):** **always** labeled — the ID *is* the path; the `identity↔path` map is mandatory; no threshold.
  - **Full-TLV transports (ws/UDP):** **default full-route** (stateless forwarders, no label table). Labels are **opt-in compaction**, requested **declaratively** by a `SUBSCRIBER` QoS hint (a `delivery_compact` flag in `qos_settings`, alongside `delivery_mode`/`min_interval_ns`). A transport **MAY** *also* promote a hot full-route flow to a label adaptively (SHOULD), but the hint is the contract.
- **State boundary (the cost, made precise).** One-shot ops and cold/low-rate subscriptions stay **stateless** (route in the frame). A hop holds a `label↔route` binding **only** for the flows explicitly flagged compact that cross it — bounded by *(number of compact subscriptions through this hop)*, and on CAN it is the `identity↔path` map already paid for. So a constrained ws node forwarding 50 cold reads holds **zero** label state.

With a 2–4 B label, the 1 kHz example drops from ~16× to **~1.5×** overhead. Net rule: **one-shot ops pay the full route; high-rate established streams amortize it to a label, by the transport's framing mode.**

### F. ACL across hops

A `FWD` is gated **twice**, by the existing ACL machinery ([ADR-0018](../../adr/0018-access-control-authorization-pluggable-subject-token.md)/[ADR-0020](../../adr/0020-acl-nfsv4-style-aces-with-inheritance.md)):

1. each intermediate **transport vertex's `:acl`** authorizes "may this `origin_peer_id` forward *through* me" (the forward right);
2. the **target vertex's `:acl`** authorizes the actual `READ`/`WRITE`/`AWAIT` at the final hop.

No new ACL machinery; `FWD` is subject to the same subject-token model as a local field-write.

### G. Type-code budget

`FWD=0x0F` and `FIELD=0x10` consume two of the sixteen `0x0F`–`0x1F` v1 fast-track slots ([reference/05](../../reference/05-protocol-tlvs.md)). Both are structured (`opt.PL=1`), each declaring its own purpose — no generic container is introduced.

### H. Conformance vectors (proposed)

Add to `tests/conformance/vectors/v1/`, so the 3-core machine (C++/TS/Rust) validates `FWD`/`FIELD` like every other TLV:

- `fwd-read` — `FWD{ op=READ, dst=/sensor/temp, src=/reply-ep }` (seeded `src`).
- `fwd-write-value` — `FWD{ op=WRITE, dst=/sensor/temp, src=…, VALUE u32 }`.
- `fwd-await-timeout` — `FWD{ op=AWAIT, dst=/sensor/temp, src=…, await_timeout=1e9 }`.
- `fwd-write-subscriber-field` — `FWD{ op=WRITE, dst=/sensor/temp, FIELD :subscribers[], src=…, SUBSCRIBER{...} }`.
- `fwd-routed-multihop` — `FWD{ op=READ, dst=/net/<peer>/can[0]/ow/x, src=/reply-ep }` (un-stripped `dst`).
- `fwd-src-accumulated` — the same op **after two hops**: `dst` shrunk by two segments, `src` grown by two (the prepend invariant).
- `fwd-reply-result` / `fwd-reply-error` — `FWD{ op=REPLY, kind=RESULT, dst=<accumulated route>, VALUE }` and `kind=ERROR, STATUS=ERROR(...)`.
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

## Resolved during design (see §B/§D/§E)

- **Reply framing** → a thin **`FWD{ op=REPLY, kind∈{RESULT,ERROR} }`** routed back via the accumulated `src`. Not a bare TLV (we need the route) and not a rich reply-kind (deliveries are `WRITE`s, not replies).
- **Return route** → **accumulated in the frame** (zero-copy `src` prepend), *not* per-hop connection state — so forwarders are stateless and replies survive a hop reboot.
- **Unidirectional transport** → handled: the reply self-routes via `src`; it does **not** need the inbound link to be bidirectional.
- **Streaming vs one-shot** → two planes; streaming never per-sample-remote-writes (producer local-produce + flush + fan-out); a delivery *is* a `FWD WRITE`.
- **Streaming route overhead** → the **route-handle** (§E.1): a per-link, advertise-driven, framing-mode-gated label (header-elision generalized) amortizes the return route on established high-rate subs, keeping forwarders stateless for the one-shot/cold case. Drops the 1 kHz example from ~16× to ~1.5× overhead.

## Open questions (for the comment window)

1. **`await_timeout` cap** — the *default* is pinned (1 s, reference impl) when no child is present; whether a *normative upper bound* should exist is still open.
2. **Forward-right delegation** — does an intermediate hop forward under the *original* `origin_peer_id` (end-to-end identity) or re-originate as itself at each hop? (Affects §F's first ACL check; leaning end-to-end identity preserved, each hop authorizes by it — note `src` already records the per-hop forwarder chain.)
3. **`src` exposure / privacy** — the accumulated return route reveals the topology to the destination (and the full source route to the consumer — usually desirable as provenance). Is a redacted/opaque-segment mode ever needed for an untrusted intermediate, or is per-hop ACL sufficient?
4. **Stream-tag interop** — each transport defines its own request↔reply matching tag; do we want a *recommended* (non-normative) tag shape so independent transport implementations converge?

## Relates

- [ADR-0006](../../adr/0006-read-write-await-api-no-connect.md) — read/write/await, no connect/subscribe (the verb set `FWD` carries).
- [ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md) — consumer-as-client / subscription is a write.
- [ADR-0027](../../adr/0027-transport-and-connections-are-vertices.md) — transports/connections are vertices (the mount points).
- [ADR-0031](../../adr/0031-direct-browser-to-robot-binding-and-webtransport.md) — browser↔robot (the consumer of this).
- [ADR-0034](../../adr/0034-typescript-client-sdk.md) — the TS client SDK whose higher ops this unblocks.
- [RFC-0003](0003-bridged-wildcard-delivery-path.md) — concrete-path delivery (the delivery-side companion).
- [reference/03](../../reference/03-addressing.md) (addressing grammar), [reference/04](../../reference/04-communication-flows.md) (flows + mount), [reference/05](../../reference/05-protocol-tlvs.md) (TLV registry).
