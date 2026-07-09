<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0009 — Vertex removal (owner-initiated) and the subscriber-record lifecycle: disappearance delivery, transport-liveness eviction, unsubscribe

| Field | Value |
| ---- | ---- |
| **RFC** | 0009 |
| **Title** | Vertex removal (owner-initiated) and the subscriber-record lifecycle: disappearance delivery, transport-liveness eviction, unsubscribe |
| **Status** | **draft** (consumer-driven; strawberry-fw cutover rulings 2026-07-09) |
| **Author(s)** | strawberry-fw integration (drafted for maintainer review; owner-initiated-only constraint is a maintainer ruling, 2026-07-09) |
| **Created** | 2026-07-09 |
| **Comment window closes** | opens with the tracking PR (GOVERNANCE.md §Spec changes) |
| **Tracking issue** | the [#66](https://github.com/avatarsd-llc/libtracer/issues/66) residual (child-removal delivery, deferred by RFC-0005 §E); a dedicated `rfc` issue to be opened with this PR |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |

## Summary

Two coupled lifecycle gaps close together, because they share one mechanism —
**record removal**:

- **A. Vertex/subtree removal.** `graph_t` gains its first removal surface:
  a **local, owner-facing** `remove_vertex` — the exact mirror of
  `register_vertex` — that removes a vertex **and its whole subtree**. There is
  **no wire-level REMOVE operation**: a remote peer expresses removal *intent*
  by writing to a handler the owning subsystem exposes (symmetric with
  creation, where a `:children[]` SPEC lands in the owner-registered factory
  catalog rather than inserting a vertex directly); the owner tears down in its
  own order and calls the local remove as the final step. Removal emits the
  **disappearance notification** RFC-0005 §E deferred — the symmetric signal to
  appearance-via-first-write-bubbling — delivered once to each covered
  subscription point as an `ERROR{tr::path::removed, PATH}` TLV. Re-creating a
  removed path yields a **new vertex** (fresh epoch: no inherited subscribers,
  settings, ACL, history, or write sequence); stale `vertex_handle_t`s **fail
  safely** with `NOT_FOUND`.
- **B. Subscriber-record lifecycle.** The graph derives subscriber liveness
  from **transport liveness** ("split the layers"): a SUBSCRIBER record whose
  delivery rides a closed/gone connection is **evicted** — the same
  record-removal mechanism as an explicit unsubscribe, which this RFC also
  pins (idempotent re-append, indexed slot clear, caller-matched
  authorization). Consequence: a non-empty `:subscribers[]` becomes a
  **truthful "someone is listening" gate** (the §lazy-source pattern becomes
  sound), and **no application-level keepalive exists anywhere**.

No new wire verbs, no new type codes. The one wire-visible addition is the
registered error identity `tr::path::removed`; everything else is host-API
surface and normative semantics for existing fields.

## Motivation

1. **The #66 residual.** RFC-0005 resolved structural *observation* — a child's
   appearance is its first write bubbling — but explicitly deferred child
   **removal** delivery "tied to a future vertex-delete surface". That surface
   is now demanded by a real consumer: the strawberry-fw protobuf-WS cutover
   maps six of its command handlers (unit remove, user-IO remove, controller
   destroy, CAN mirror unbind, device remove, profile remove) onto vertex
   removal, and is currently shipping an interim tombstone convention
   (`:settings.retired` + walker filtering) explicitly designed to be erased
   when this RFC lands (fw `doc/gate-a-parity-checklist.md` §5 U7, ratified
   2026-07-09).
2. **Ghost subscribers run collectors forever.** The same consumer gates
   expensive producers (an 802.15.4 spectrum radio loop, a CAN observe
   collector) on `:subscribers[]` being non-empty — the documented
   §lazy-source pattern. Operationally this is unsound today: fw soak-1
   dropped its WebSocket session at ~80 minutes leaving a **ghost SUBSCRIBER**
   that would have kept the radio loop running indefinitely, and subscriber
   slots **accumulate across reconnects** with no way to clear them (fw
   checklist §5 N3, §2 T10). The old stack solved this with an
   application-level keepalive lease; the maintainer ruling is the opposite
   split: **liveness is the transport's fact**, and the graph should consume
   it, not re-derive it with pulses.
3. **The reference implementation already names the debt.** `graph_t`'s vertex
   map is insert-only, and its own comment records that retirement "must NOT be
   a bare detach-from-parent — that would dangle every outstanding handle …
   it needs a vertex lifetime scheme first" (`core/include/libtracer/graph.hpp`,
   the #220 dangling-ref lesson). `transport_vertex_t` documents "there is no
   child-removal / connection-teardown model yet (#66)". `acl_right_t::DELETE`
   is reserved with "no core surface yet". This RFC pins what those seams were
   left open for.

## Proposed change

### A. Vertex/subtree removal

#### A.1 Owner-initiated only — a local API, no wire REMOVE (normative constraint)

- `graph_t` gains **`remove_vertex(vertex_handle_t)`**: remove the named vertex
  and **every descendant** (a vertex is its subtree's root; there is no
  interior-only removal — it would orphan children). This is a **local,
  owner-facing** call, the exact mirror of `register_vertex`: registration and
  removal are the two ends of one owner-held lifetime.
- There is **NO wire-level REMOVE operation** and none is added. A remote
  consumer expresses removal **intent** by writing to a handler vertex the
  owning subsystem exposes — exactly symmetric with creation, where a
  `:children[]` SPEC write lands in the owner-registered **factory catalog**
  (ADR-0017) rather than inserting a vertex directly. The owner performs its
  real teardown in its own order — stop sampling, release driver/resource
  state, unregister — and calls the local remove as the **final** step, which
  is what emits the disappearance notification (§A.3).

  *Example:* removing a 1-Wire device. The 1-Wire controller must expose the
  removal interface (a handler leaf, or an op in its device-private catalog); a
  graph-level remove aimed at `/hw/w1/<rom>` **from outside is not a thing**.
  The controller stops the conversion schedule, releases the ROM slot, then
  removes the vertex.

  Rationale, recorded: (a) **symmetry with owner-mediated creation** — the
  catalog owner instantiates, the catalog owner tears down; (b) **the graph is
  a projection of device state, never an external lever that mutates
  internals** — a peer manipulating the projection must go through the device's
  own logic, as with every other side effect (§field-write doctrine); (c) it
  **eliminates the lifetime hazard of a peer yanking a vertex from under a
  running producer** — the owner sequences its producer's stop *before* the
  graph forgets the address, so no removal ever races the owner's own hot path.
- Consequently `acl_right_t::DELETE` (`0x10`) **stays reserved**: there is no
  wire surface for it to gate. An owner's removal-intent handler gates with the
  ordinary WRITE right on that handler leaf, like every side-effect write.
- Because the call is owner-facing and infallible-shaped like
  `register_vertex`, removing a vertex the graph does not hold is a source bug:
  `remove_vertex(vertex_handle_t)` takes a live handle, and the double-remove
  of an already-removed handle is defined to be a harmless no-op (fail-safe
  per §A.5, not UB).

#### A.2 What removal does (state plane)

On `remove_vertex(v)`:

- The subtree rooted at `v` is **unlinked from the tree**: `find`, path
  resolution, and `:children[]` enumeration of the parent cease to surface any
  removed vertex, atomically with respect to the map lock (a resolver never
  observes a half-removed subtree).
- **SUBSCRIBER records stored ON removed vertices are destroyed with them** —
  the edges are vertex state. Nothing further is delivered along them. (Their
  remote holders learn through §A.3 if they also held a covering ancestor
  subscription, and through §A.6 on their next operation regardless.)
- **In-flight refcounted views and ropes of the removed vertices' LKVs are
  unaffected** — the existing refcount semantics already cover memory, stated
  here normatively: removal drops the *graph's* references (LKV slots, history
  rings, stored SUBSCRIBER source views, stored routes); every view, rope, or
  `edge_view_t` snapshot a reader or an in-flight delivery still holds keeps
  its backing segments alive until the last reference drops. Removal never
  invalidates bytes a holder can still reach.
- RFC-0008 sweep state is purged: pending/unconditional membership of every
  removed vertex is erased (one ordered-set prefix-range erase — the same
  contiguity property the sweep iterates by).
- No listener-counter fixups propagate **upward**: a removed vertex's
  `own_subs` fed only its *descendants'* `listeners_above_` — all removed with
  it. Edges on surviving ancestors are untouched (they simply cover a smaller
  subtree).
- Placeholder intermediates left childless by the removal MAY be reclaimed
  (unobservable — placeholders never surface in `find` or `:children[]`).

#### A.3 The disappearance notification (propagation plane)

The symmetric signal to RFC-0005's appearance-via-first-write-bubbling:

- `remove_vertex(v)` MUST deliver a **removal notification** to every covered
  subscription point **strictly above** the removal root — i.e. to each
  surviving ancestor subscriber whose subtree subscription covered `v` — **once
  per subscriber**, through the ordinary delivery machinery (local callback /
  local target re-dispatch / remote return-route delivery), exactly like a
  bubbled write.
- The notification is delivered **once for the removed subtree root**, not once
  per removed descendant — mirroring the branch-write rule that a covered
  subscription point is notified once with the smallest unit covering
  everything that changed below it. The subtree's contents were enumerable
  before the removal; "this whole subtree is gone" is one fact.
- **Payload shape — no new type code.** The delivered TLV is an **`ERROR`
  (`0x08`)** carrying the new registered identity **`tr::path::removed`**, with
  one detail child: the **`PATH` (`0x06`)** of the removed root in the
  producer's canonical local form (the same identity scope as RFC-0003's
  concrete-path delivery proposal; a bridged consumer resolves it under its
  route prefix exactly as it resolves RFC-0003 wildcard tags). The graph — a
  protocol component, not an application — is a legitimate emitter under the
  closed error boundary (ADR-0010): applications still never mint protocol
  errors; the *runtime* reporting a structural fact about its own graph is
  precisely what the `tr::path` concept namespace is for.
- Delivery policy: the notification is an **UNCONDITIONAL, eager delivery** —
  it does not ride the pending set (the removed keys are being erased from it)
  and is never coalesced away by a sweep. `delivery_mode` of surviving
  ancestors is irrelevant: like a direct write's fan-out, disappearance is an
  explicit act performed at removal time.
- Local `await` on a removed vertex: waiters blocked in `await` at removal time
  MUST be woken and return `tr::path::not_found` (`NOT_FOUND`), not hang until
  timeout.

#### A.4 Path identity on re-create — a new vertex, a new epoch

- A later registration or write-creates at a removed path creates a **new
  vertex**: fresh `write_seq` (a new epoch), **no** inherited subscribers,
  settings, `delivery_mode`, ACL, or history. Path equality is *address*
  equality, never *identity* continuity — consumers MUST NOT assume any state
  survives a remove/re-create cycle. (This is the same doctrine as write-creates
  itself: appearance is a first write, and a re-created child's appearance is
  again its first write bubbling to covering subscribers.)
- Cross-references held **by handle** into the old vertex fail per §A.5;
  references **by path** bind to the new vertex on next resolution.

#### A.5 Stale handles fail safely (the ADR-0056 contract, amended)

ADR-0056's "a handle always names a live vertex for the graph's lifetime"
weakens to: **a handle names a live vertex until that vertex is removed;
thereafter every `graph_t` operation taking it returns `NOT_FOUND`
(`tr::path::not_found`) — never UB.**

Reference-implementation scheme (normative for the reference, informative for
others): removal **retires** rather than frees — the removed `vertex_t`
allocations are unlinked from the tree, marked removed (one flag/epoch checked
at op entry — a relaxed load on the same cache line the op already touches),
and moved to a retire list; retired storage is **reclaimed only after
concurrent operations drain** (quiescent/epoch reclamation — the same
writer-priority-barrier family the project has used elsewhere), so a stale
handle race hits the removed-flag check, not freed memory. A constrained
profile MAY defer reclamation indefinitely (retire-only) — memory then bounds
the *owner's* remove/re-create churn, which the owner controls by construction
(§A.1). This is exactly the "vertex lifetime scheme first" precondition the
insert-only map comment records (the #220 route_handle dangling-ref class);
`remove_vertex` is specified to be impossible to implement as a bare
detach-from-parent.

#### A.6 Remote holders and the write-creates interaction

For a route-addressed remote holder of a removed path (its next `FWD` op
arrives at the terminus after removal):

- `READ`, `AWAIT`, and any `:field` operation return **`tr::path::not_found`**
  — the standard error reply on the return route. (State it: the remote holder
  simply gets NOT_FOUND on its next op; nothing is pushed to non-subscribers.)
- A **data WRITE re-creates**, per RFC-0005 §D, CREATE-gated on the nearest
  existing ancestor — yielding a **new** vertex per §A.4. Removal does not
  carve a hole in write-creates; the two compose. An owner that must prevent
  resurrection of a removed path withholds the CREATE right — the ACL, not a
  new mechanism, is the fence.
- **Exception — fan-out re-dispatch does not create.** A subscription edge's
  delivery into a **local target vertex** that has been removed MUST NOT
  write-create the target: creation authority belongs to explicit acts
  (a producer's write, an owner's registration), never to a dangling edge
  firing. Instead the delivery is dropped **and the edge record is evicted**
  (§B.4 — target-gone eviction, the local analogue of a dead connection).

### B. Subscriber eviction on dead connections ("split the layers")

Eviction is **liveness bookkeeping, not semantic removal** — it never touches
vertices, emits no `tr::path::removed`, and is invisible to the removed
subscriber (it is gone; that is the point). It IS, however, the same
**record-removal mechanism** as §C's unsubscribe: one slot-clear path
(deactivate + RFC-0005 listener-counter bookkeeping + route/label state drop),
entered from three doors — explicit unsubscribe, transport-liveness eviction,
target-gone eviction.

#### B.1 The normative rule

- The graph derives a remote SUBSCRIBER record's liveness from **the liveness
  of the transport connection its deliveries ride**. When that connection is
  closed or gone, the record MUST be evicted.
- Two seams feed it, both transport-plane facts:
  1. **Connection teardown (prompt).** When a connection is torn down — the
     connection vertex's link-down transition (`set_link_state(name, false)`,
     ADR-0027), or the link's removal from the router's child registry
     (`fwd_router_t::clear_link`) — every SUBSCRIBER record in the graph whose
     stored `link` names that connection is evicted. This is the primary path:
     it does not wait for a delivery to fail.
  2. **Delivery failure (backstop).** When the remote delivery of a value
     cannot be performed because the subscriber's link no longer exists (the
     registry lookup fails) or the transport reports the connection closed on
     send, the record is evicted. Today's silent drop in `deliver_remote`
     ("link torn down between subscribe and this write") becomes an eviction.
     The remote-sink seam grows a way to report this outcome (a status return
     or an eviction callback into the graph — an implementation seam, not wire
     surface); best-effort transports that cannot distinguish "lost datagram"
     from "peer gone" simply never fire this seam and rely on seam 1.
- **Connectionless links (CAN) evict on link-level peer-presence lapse.** On a
  bus transport one `link` carries many peers, so connection-vertex teardown is
  too coarse. The bus transport already owns per-peer presence — the ADR-0030
  advertise/id-match map is self-establishing and self-healing, and ADR-0044's
  `bus_link_t` synthesizes the live peer roster from the kind's own
  announce/heartbeat traffic with zero stored graph state. The rule: when the
  transport's **link-level peer-presence mechanism** declares a peer lapsed
  (kind-defined window — e.g. a missed re-advertise), the transport reports the
  peer gone, and every SUBSCRIBER record whose delivery rides that (link, peer)
  is evicted. The graph consumes the transport's presence fact; it never runs
  its own timer.
- **In-process edges are never liveness-evicted**: a local callback/target
  edge's holder is this process (alive by definition). Its removal paths are
  explicit unsubscribe (§C) and target-gone eviction (§A.6).
- **No application keepalive exists anywhere.** Implementations MUST NOT
  require application-level keepalive pulses, lease TTLs, or periodic
  re-subscription to keep a subscription alive. (This retires the fw interim
  TTL handler-lease and the old stack's `SPECTRUM_LEASE_MS` convention; the
  RFC-0008 note stands that the removed `keepalive_ns` QoS key does not come
  back.)

#### B.2 The consequence this buys: a truthful listener gate

With eviction in force, `:subscribers[]` non-empty is a **truthful "someone is
listening" signal**, transparently across transports. The §lazy-source pattern
(CONTEXT.md — a handler-role vertex observing its own subscriber-count edge,
`0→1` starts producing, `1→0` tears down) becomes *sound*: a consumer that
vanishes without unsubscribing takes its record with it on connection teardown
(or the first failed delivery), and the collector stops. This is precisely the
gate strawberry-fw needs for its spectrum radio loop and CAN observe collector
(fw checklist T10/T12), and the ghost-subscriber-at-80-minutes soak failure is
the conformance scenario for it.

#### B.3 What eviction is NOT

- Not a delivery guarantee change: reliability/backpressure semantics of live
  connections are untouched; eviction fires on *gone*, not *slow*.
- Not semantic removal: no vertex changes, no disappearance notification, no
  `tr::path::removed`. A producer observing its own `:subscribers[]` (the
  lazy-source edge) sees the count drop — that is the whole observable.
- Not authorization: eviction ignores ACLs (there is no caller; the graph is
  reaping its own bookkeeping).

#### B.4 One mechanism (shared with §A and §C)

Every record-removal door ends in the same internal step — deactivate the
slot, run the RFC-0005 listener-counter bookkeeping
(`note_subscriber_removed`), release the stored route/source views (refcounts;
in-flight deliveries keep their clones per the existing ADR-0041 §2 rule), and
drop any per-flow egress label state for evicted remote flows (the
`route_handle` binding — already cleared per-link on reconnect). Mirroring
ADR-0049's single admission door: **one removal door, three entries** (explicit
unsubscribe, liveness eviction, target-gone eviction), so gate and bookkeeping
semantics cannot diverge per entry point.

### C. Unsubscribe — the explicit record-removal surface

The wire has an append (`:subscribers[]` += SUBSCRIBER) but no ratified
removal; the reference implementation carries an indexed clear
(`:subscribers[N]`, WRITE-gated) that no spec text pins, and consumers cannot
learn their slot index race-free. Fw evidence: subscriber slots accumulate
across reconnects with no way to clear them. Pinned:

- **C.1 Idempotent append (dedupe at the door).** A `:subscribers[]` append
  whose SUBSCRIBER TLV bytes, link, and caller context all equal an existing
  **active** record's MUST replace/refresh that record in place (same slot) and
  MUST NOT grow the array. Re-subscribing is therefore always safe — the
  reconnect-resubscribe pattern stops leaking slots even before eviction has
  fired. (A byte-different SUBSCRIBER — e.g. changed `delivery_compact` — is a
  distinct subscription and appends normally.)
- **C.2 Indexed clear.** A field-write of an **empty value** to
  `:subscribers[N]` clears slot N (deactivates the record — the unsubscribe).
  Gated by the **SUBSCRIBE** right (the fan-out control plane: the door that
  admits also removes), superseding the reference implementation's current
  WRITE gating.
- **C.3 Caller-matched authorization (self-service unsubscribe).** A
  remote-issued clear MUST additionally match the record's stored caller
  context (the link/subject the record was created under) — a peer removes
  **its own** records only — unless the caller holds WRITE_ACL (admin) on the
  vertex. A mismatched clear returns `tr::access::denied`. This also makes slot
  reuse safe: a stale index aimed at a reused slot fails the match instead of
  killing a stranger's subscription.
- **C.4 Knowing your slot.** The REPLY to a successful `:subscribers[]` append
  SHOULD carry the assigned slot index (a VALUE detail in the reply), so a
  consumer holds its N without a racy array read. `:subscribers[]` array reads
  continue to serve only active records; cleared/evicted slot **storage** MAY
  be reused or compacted (indices name slots, not subscriptions — C.3 is the
  safety), so the array is not a leak.

### Files this RFC edits (on acceptance)

- `docs/reference/02-graph-model.md` — new §vertex removal (owner-initiated
  local surface, disappearance delivery, re-create epoch, stale-handle
  contract, write-creates interaction); §observing structural change gains the
  removal half.
- `docs/reference/05-protocol-tlvs.md` — §`0x04` SUBSCRIBER: idempotent
  append, indexed clear + gating + caller match, eviction semantics, the
  truthful-gate property; §`0x08`/registry: the `tr::path::removed` registered
  code; §`0x0A`: note DELETE remains reserved (no wire removal surface — by
  design, not omission).
- `docs/spec/rfcs/0002-protocol-error-model.md` registry (or its registry
  home) — assign `tr::path::removed` (severity `warn`, disposition
  `permanent`).
- [RFC-0005](0005-subtree-subscriptions.md) §E — the deferred child-removal
  point resolves here (cross-reference note).
- `CONTEXT.md` — glossary entries: *vertex removal / disappearance
  notification* (owner-initiated only, no wire REMOVE), *subscriber eviction
  (transport-liveness)*, *unsubscribe*; the §lazy-source entry gains the
  truthful-gate consequence; §SUBSCRIBER direction gains the record-lifecycle
  sentence.
- `core/` reference implementation + `core/CHANGELOG.md` (`remove_vertex`, the
  retire/reclaim scheme, the eviction seams in `fwd_router_t` /
  `transport_vertex_t` / `bus_link_t`, the unsubscribe gating change) — follow
  this RFC, not normative.

## Conformance-vector sketches (what would prove it)

New vectors under `tests/conformance/vectors/v1/` (additive, spec §4), plus
host-API tests where the surface is local:

1. **`removal-disappearance-delivery`** — subtree subscriber at `/s`; owner
   removes `/s/t` (which has children): subscriber receives exactly ONE
   `ERROR{tr::path::removed, PATH(/s/t)}`; a second subscriber at the root
   receives its own single copy; a subscriber at `/s/t/u` receives nothing
   (destroyed with the subtree).
2. **`removal-remote-not-found`** — remote holder of a removed path:
   `FWD{READ}` and `FWD{AWAIT}` reply `tr::path::not_found`; a `:field` write
   replies `tr::path::not_found`; a data `FWD{WRITE}` re-creates (new vertex)
   when CREATE allows and replies `tr::access::denied` when it does not.
3. **`removal-recreate-epoch`** — remove, write-create the same path:
   `:subscribers[]` reads back empty, `:settings` back at defaults, history
   empty; the old subtree subscriber above sees the re-created child's first
   write bubble (appearance again).
4. **Host:** stale-handle safety — `read/write/await/propagate` through a
   pre-removal handle return `NOT_FOUND`; double `remove_vertex` is a no-op;
   an `await` blocked at removal time wakes with `NOT_FOUND`; ASan/TSan soak
   of concurrent readers racing removal (the §A.5 retire scheme's proof).
5. **`eviction-on-close`** — WS pair: remote subscribe, verify delivery, kill
   the consumer's connection; producer's `:subscribers[]` reads back empty
   without any further write; a lazy-source handler observes `1→0`. (The fw
   soak-1 ghost-subscriber scenario, mechanized.)
6. **`eviction-on-send-failure`** — link removed from the registry between
   subscribe and the next propagate: the delivery attempt evicts; second
   propagate performs no remote-sink call (observable via the sink counter).
7. **CAN peer-lapse (vcan)** — two peers on one bus link; peer B's advertise
   lapses past the kind's window: records riding (link, B) evict; records
   riding (link, C) survive. (Extends `transport_can_peers_test`.)
8. **`unsubscribe-idempotent-append`** — same SUBSCRIBER bytes + caller
   appended twice ⇒ one active record; byte-different appends ⇒ two.
9. **`unsubscribe-indexed-clear`** — empty write to `:subscribers[N]` clears;
   SUBSCRIBE-gated; a clear from a different caller context without WRITE_ACL
   ⇒ `tr::access::denied`; with WRITE_ACL ⇒ allowed.
10. **Target-gone eviction (host)** — edge targeting a local vertex; remove
    the target; next delivery drops without creating the target and the edge
    is evicted (no vertex reappears at the target path; `ensure`-style
    creation counters stay flat).

## Compatibility

- **No existing conformance vector changes.** No wire verbs or type codes are
  added or altered; `tr::path::removed` is a new registry entry (additive).
  New behavior occupies previously-undefined or previously-erroring space:
  there was no removal surface at all, the indexed `:subscribers[N]` clear was
  implemented but unspecified (its gating changes WRITE→SUBSCRIBE — a
  pre-release refinement like RFC-0005/0008's), and duplicate re-appends
  previously accumulated where they now refresh.
- **Host API:** `remove_vertex` is additive; the ADR-0056 handle contract is
  amended (§A.5) from "always live" to "live until removed, then fail-safe
  NOT_FOUND" — no caller change for code that never removes. The remote-sink /
  transport seams grow an outcome/peer-presence report (implementation-defined
  per ADR-0013; the wire is untouched).
- **Migration:** consumers drop keepalive leases and rely on eviction +
  explicit unsubscribe; strawberry-fw's tombstone convention (`:settings.
  retired` + walker filtering) is erased in favor of real removal, per its own
  design note. A node that has not migrated interoperates for all
  previously-valid traffic; it differs only in the newly-defined cases (it
  leaks ghost records and cannot signal disappearance — today's behavior).

## Alternatives considered

- **A wire-level REMOVE operation / a DELETE-gated `:children[N]` clear.**
  Rejected by maintainer ruling (§A.1): creation is owner-mediated through the
  factory catalog, so removal must be owner-mediated through the owner's
  handler — the graph is a projection of device state, not a lever into it;
  and a peer-initiated graph-level remove reintroduces the exact
  yank-under-a-running-producer lifetime hazard §A.5 exists to prevent, at
  every terminus instead of one owner-sequenced site.
- **A `:children_changed` facet / structural-event TLV for disappearance.**
  Rejected in #66 triage already; RFC-0005 settled appearance as
  first-write-bubbling, and this RFC keeps removal on the same delivery
  machinery (one bubbled TLV) rather than a second mechanism.
- **A new type code for the disappearance notification.** Rejected — the
  ERROR TLV with a registered `tr::path::*` identity plus a PATH detail is
  byte-precise, already parseable by every conforming consumer, and keeps the
  no-new-type-codes streak; the graph is a protocol-plane emitter, so the
  closed error boundary (ADR-0010) is not breached.
- **Tombstone vertices as the permanent model** (the fw interim). Rejected as
  spec: tombstones bound memory only by convention, poison enumeration for
  every walker (all must filter), and make re-create-inherits-state the
  accidental default — the opposite of §A.4. They remain a fine consumer-side
  interim precisely because they erase cleanly when this lands.
- **Application-level keepalive / lease TTLs for subscriber liveness.**
  Rejected ("split the layers"): the transport already owns the liveness fact
  (connection state; ADR-0030 advertise presence); an app-level lease
  re-derives it worse (pulse traffic, a second timer, and RFC-0008 §C just
  removed `keepalive_ns` for being the wrong layer). Periodic SUBSCRIBER
  re-append as a lease is doubly rejected — without C.1 every refresh leaked a
  slot, and with C.1 it is still pulse traffic for a fact the transport knows.
- **Evicting on backpressure/slow consumers.** Rejected: eviction keys on
  *gone*, a binary transport fact; conflating it with *slow* smuggles a QoS
  policy into a liveness mechanism (the existing `queue_max_bytes`
  backpressure story is unchanged).
- **Per-`(vertex)` subscriber GC scans.** Rejected: eviction is event-driven
  off the transport seams (teardown, send-failure, peer-lapse); a periodic
  whole-graph sweep is a timer the graph was just relieved of.
- **Freeing removed vertices immediately.** Rejected — the #220 lesson class:
  outstanding handles and in-flight snapshots would dangle; retire-then-reclaim
  (§A.5) is the minimum honest scheme, and the insert-only map comment already
  said so.

## Discussion

Genuinely contentious points, flagged for the maintainer:

1. **The ERROR-shaped disappearance notification** (§A.3). It reuses existing
   machinery, but a *local target vertex* wired as a subtree subscriber's sink
   would have the ERROR TLV re-dispatched into it as an ordinary delivery-write
   (stored-value targets store it). Structural observers are callbacks/remote
   consumers in every known use, and RFC-0007 keeps the blast radius one hop —
   but if storing an ERROR at a data sink is judged unacceptable, the
   alternative is delivering disappearance only to callback/remote edges and
   skipping local target re-dispatch (asymmetric), or a dedicated marker shape.
2. **Bus peer-lapse windows** (§B.1). The RFC deliberately leaves the lapse
   window kind-defined (ADR-0030's advertise cadence is itself kind-internal).
   If cross-implementation predictability is wanted, a per-connection
   `:settings` knob (like `max_frame`) is the consistent home — not specified
   here.
3. **Unsubscribe gating** (§C.2/C.3): SUBSCRIBE-right + caller-match is a
   behavior change to the implemented (unspecced) WRITE-gated clear, and
   caller-match makes ACL-less deployments' clears effectively self-service
   only. If shared-admin unsubscribe without ACLs matters, the match rule could
   soften to SHOULD.
4. **Reply-carried slot index** (§C.4) adds reply detail to a field-write —
   small, but it is the RFC's only touch on reply shape; dropping it costs
   consumers a racy `:subscribers[]` read to find their N.

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the tracking issue stays
open at least 14 days for implementer feedback before this document is merged
(unless the standing solo-maintainer waiver is applied, as on RFC-0002/0005/
0008). Sustained objections and their resolution to be recorded here.
