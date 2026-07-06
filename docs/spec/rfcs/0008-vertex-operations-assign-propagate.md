<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0008 — Vertex operations: `assign` and `propagate`; structural selective propagation; remove value-based delivery filtering

| Field | Value |
| ---- | ---- |
| **RFC** | 0008 |
| **Title** | Vertex operations: `assign` and `propagate`; structural selective propagation; remove value-based delivery filtering |
| **Status** | **accepted** (2026-07-06, maintainer-ratified design discussion) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-06 |
| **Comment window** | waived by the maintainer (solo-maintainer project, GOVERNANCE.md window dead ceremony) |
| **Tracking issue** | [#235](https://github.com/avatarsd-llc/libtracer/issues/235) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |

## Summary

The conflated `write` operation (store the value **and** notify subscribers, in one
call) splits into the **two irreducible operations** it was always hiding:

- **`assign`** — the vertex-local state transition: replace the vertex's value. Reads
  no edge and sends nothing. (In graph terms, relabel a vertex; in C++ terms, the
  `operator=` — and with rope-valued vertices ([ADR-0053](../../adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md) §6) it is literally an atomic
  last-known-value swap.)
- **`propagate`** — the edge transition: deliver a vertex's value along its
  subscription edges to other vertices (and remote subscribers). Sends; does not
  mutate.

The runtime **stops inspecting vertex values to decide delivery.** The
`delivery_mode` field (`EVERY` / `THROTTLED` / **`ON_CHANGE`**) is **removed
entirely** — including its wire encoding in `SUBSCRIBER.qos_settings`. In its place,
selective propagation is **structural**: `assign` marks a vertex **dirty**;
`propagate(root)` flushes **only the dirty descendants** of `root` — the vertices
actually assigned since the last flush — then clears their marks. What "changed"
means is decided by *which operations the caller performed*, never by comparing
bytes.

This resolves [#235](https://github.com/avatarsd-llc/libtracer/issues/235). It
**supersedes** the `delivery_mode` / `ON_CHANGE` clauses of accepted
[RFC-0004](0004-remote-operation-addressing.md) §E and
[RFC-0005](0005-subtree-subscriptions.md) §A and reference/05 — a reversal the
maintainer has authorized. No new wire verbs, no new type codes; the one wire change
is a **removal** (the `delivery_mode` / `min_interval_ns` QoS keys).

## Motivation

`ON_CHANGE` — "deliver only when the value bytes differ from the last delivered" —
asks the protocol runtime to read application data and make an application decision.
That is the wrong layer: a vertex stores bytes it never parses ([ADR-0053](../../adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md) §1),
there is no meaningful `operator!=` for an opaque value, and whether an update is
worth delivering is the application's judgement, not the graph's. Byte-diff
suppression is also a performance trap at the exact scale it is meant to help:
comparing an *N*-byte value costs the same *O(N)* memory traffic the delivery would.

The genuine requirement `ON_CHANGE` was trying to serve is different and better
served structurally. A producer updates **some** vertices of a subtree, then — on a
timer, at a rate it chooses — calls **one** `propagate` at a parent covering that
subtree, and wants **only the vertices it actually touched** delivered, not the whole
subtree re-sent. That is *coalescing keyed on which vertices were operated on*, and
the signal for it is a **dirty mark set by `assign`** — a fact the runtime owns,
recorded at the moment of the operation — not a value comparison. Separating `assign`
from `propagate` makes this expressible and makes every delivery an explicit act: the
graph never fans out behind the caller's back.

## Proposed change

### A. The two operations

A vertex sits in two orthogonal planes. The **state plane** holds its value; the
**propagation plane** is its outgoing subscription edges. Exactly two primitives act
on a vertex, and they touch disjoint planes:

- **`assign(v, value)`** — replace `v`'s value. Effects, all vertex-local: swap the
  last-known-value (atomic), append to the stream ring if `v` is a stream, wake any
  `await` waiter on `v`, and **mark `v` dirty** (§B). No fan-out. Idempotent in
  state (assigning the same value twice leaves the same state; it does, of course,
  re-mark dirty).
- **`propagate(v)`** — deliver, along subscription edges, the values of the **dirty**
  vertices in the subtree rooted at `v` (§B). No value argument: the last-known-value
  is the single source of truth, so there is no ambiguity about *which* value is
  sent. Delivers unconditionally — it does not get to decide *whether* to deliver,
  only to deliver.

`read` and `await` are unchanged and both live in the **state** plane: `await`
observes `assign`s at its own vertex (the readiness plane of a single identity,
[RFC-0005](0005-subtree-subscriptions.md) §A), independent of propagation.

**`write` is removed.** The one call that did both is gone; a caller performs the two
operations explicitly. The wire mapping is unchanged (§D): a `FWD{WRITE}` arriving at
a terminus *is* an `assign` followed by a `propagate` of that vertex.

### B. Structural dirty-tracking and selective subtree propagation

`assign(v, …)` records `v` in the graph's **dirty set** — the set of vertices
assigned since the last flush that covered them.

`propagate(root)` **drains the dirty descendants of `root`**: for each vertex `u`
that is `root` or a descendant of `root` **and** is currently dirty, it delivers
`u`'s current value to `u`'s observers, then clears `u`'s dirty mark. A vertex that
was not assigned since the last covering flush is **skipped** — not because its bytes
match anything, but because the runtime holds no record that it was operated on.

Two rules keep this minimal and consistent:

1. **Per-vertex dirty (one bit of state per vertex), delivery to the full observer
   set.** When `propagate` flushes `u`, `u` is delivered to *every* subscriber that
   observes it — `u`'s own edges **and** every ancestor carrying a subtree
   subscription (the [RFC-0005](0005-subtree-subscriptions.md) §A fan-out + vertical
   bubbling), including ancestors **above** `root`. The `root` argument selects
   *which* dirty vertices flush; it does **not** cap *who* receives them. Consequence:
   if `u` is observed from two different ancestors, the first `propagate` that covers
   `u` delivers it to *both* (bubbling reaches both) and clears the mark, so a second
   overlapping `propagate` correctly finds `u` clean — no double-send, no missed
   observer, one bit of state. (Capping delivery at `root` was considered and rejected;
   it would force per-`(vertex, root)` dirty bookkeeping — see Alternatives.)
2. **Coalescing is free.** `assign` overwrites the last-known-value (last-writer-wins)
   and re-marks dirty; it does not enqueue. So *k* assigns to the same vertex between
   two `propagate`s flush **once**, with the latest value. A producer may `assign` at
   any rate and `propagate` on a timer at a lower rate; the timer rate is the delivery
   rate, and only touched vertices ride it.

**Efficiency.** `propagate(root)` costs *O(dirty-in-subtree)*, not *O(subtree-size)*:
the dirty set is an **ordered set of canonical PATH keys**, and a subtree is a
contiguous **prefix range** of that order (a parent's key is a byte-prefix of every
descendant's, the property `key_view_t::is_ancestor_of` and the bubbling walk already
rely on). `propagate(root)` iterates the range `[key(root), …)` while the prefix
holds. A large, mostly-clean subtree with three dirty leaves flushes those three and
touches nothing else. (As an idle optimization, `assign` MAY skip marking a vertex
that has no subtree subscriber above it, reusing RFC-0005's `listeners_above_`
counter — a `propagate` would deliver it nowhere. This is an optimization of the
mechanism, not a change to the semantics: with no observer, dirty-or-not is
unobservable.)

**Branch assign composes.** A `POINT` tree assigned to `v`
([RFC-0005](0005-subtree-subscriptions.md) §B branch write) decomposes exactly as
before, except the store half is now `assign` at each value-carrying descendant
(marking each dirty) and there is **no implicit notify**. The producer then calls one
`propagate(v)` to flush the whole touched set selectively — which is precisely the
"update part of a subtree, then propagate the parent" workflow this RFC exists to
support.

### C. Remove `delivery_mode` (host and wire)

The per-subscriber delivery policy is deleted:

- The host `delivery_mode_t` enum (`EVERY` / `THROTTLED` / `ON_CHANGE`) and the
  `min_interval_ns` throttle floor are removed, along with all per-subscriber
  last-delivered state and the value-comparison code path.
- On the wire, `SUBSCRIBER.qos_settings` **loses** the `delivery_mode` and
  `min_interval_ns` keys (reference/05 §`0x0C`). Because `qos_settings` children are
  optional and NAME-tagged, a peer that still emits them is handled by the standard
  unknown-key rule (ignored); no type code or verb changes. `delivery_compact`
  ([RFC-0004](0004-remote-operation-addressing.md) §E.1, label compaction) is a
  **separate, orthogonal** hint and is **retained** — it concerns *how* a route is
  encoded, not *whether* a value is delivered.

Delivery is now unconditional: a `propagate` delivers each dirty vertex to every
observer. An application wanting rate-limiting or change-suppression implements it in
application terms — the natural place, since only the application knows what
"changed" or "too often" means for its data — and expresses it by choosing when to
`propagate`.

### D. Wire mapping: a delivery is still a write

The wire is unchanged except for the §C removal. "A delivery *is* a write"
([RFC-0004](0004-remote-operation-addressing.md) §D) still holds: a `FWD{WRITE}`
carrying a `VALUE`, arriving at its terminus, means **`assign` the addressed vertex,
then `propagate` it** — the two host primitives in sequence, delivering to that
vertex's own subscribers (local and remote) and bubbling to ancestor subtree
subscriptions. A producer's selective subtree flush therefore reaches a **remote**
subtree subscriber as one `FWD{WRITE}` per dirty vertex (driven by the single host
`propagate`), exactly as a local subscriber receives one callback per dirty vertex.
No wire batch primitive is introduced; `propagate` simply drives the sends it selects.

### E. Stream vertices are a queue, not a coalesce

The dirty-flush coalescing of §B is the **stored-value** semantic (last-writer-wins:
flush the latest once). A **stream** vertex (bounded history ring) is a *queue* — its
contract is "observe every buffered entry," not "the latest." Its propagation is a
**drain** of the entries buffered since the last flush, in order, not a coalesce. The
two roles keep their existing distinction; `propagate` dispatches on the vertex role,
and a stream's flush delivers each ring entry appended since the previous flush.

### F. Determinism properties (the contract)

- **No value inspection.** No operation reads a stored value's bytes to decide
  anything about delivery. Removing this is the point.
- **Explicit composition, caller-ordered.** `assign` and `propagate` are sequenced by
  the caller; nothing fans out implicitly. `assign(A); assign(B); propagate(v)`
  deterministically propagates `B` (last-writer-wins).
- **One effect each.** `assign` touches only state (+ the dirty mark); `propagate`
  touches only edges (+ clearing marks). Neither leaks into the other's plane.
- **Suppression is the application's, by construction.** To not deliver, do not
  `propagate` — or `read`, compare in application terms, and decide. The graph never
  substitutes its own comparison for that judgement.

### Files this RFC edits

- **`docs/reference/05-protocol-tlvs.md`** — remove `delivery_mode` and
  `min_interval_ns` from `SUBSCRIBER.qos_settings`; note `delivery_compact` is
  retained.
- **[RFC-0004](0004-remote-operation-addressing.md) §E** — supersede the
  `delivery_mode` reference (the QoS-hint list loses `delivery_mode`/`min_interval_ns`;
  `delivery_compact` stays).
- **[RFC-0005](0005-subtree-subscriptions.md) §A** — supersede the "per-subscriber
  delivery policy (`delivery_mode`, ON_CHANGE byte-diff, …)" clause; restate bubbling
  in terms of `propagate` over the dirty set rather than an implicit per-write fan-out.
- Reference-implementation changes (`graph_t` API: `assign`/`propagate`, dirty set,
  removal of `delivery_mode_t`/`ON_CHANGE`) are recorded in `core/CHANGELOG.md` and
  are **not** normative; they follow this RFC.

## Compatibility

**Breaking, and deliberately so — pre-1.0.** No v1 is released, so no v2 is needed
(the RFC-0005 precedent). The host API break (`write` → `assign`/`propagate`,
`delivery_mode` gone) is a mechanical caller migration. The wire break is a pure
removal of two optional NAME-tagged `qos_settings` keys; a stale peer's leftover
`delivery_mode` is ignored under the unknown-key rule, so there is no framing
incompatibility — only the (correct) loss of the suppression behavior it requested.

## Alternatives considered

- **Keep `ON_CHANGE` (value byte-diff).** Rejected — the motivation. Wrong layer
  (runtime inspecting application data), no meaningful `operator!=` on opaque bytes,
  and *O(N)* comparison at the scale it is meant to relieve.
- **Keep `write` coupled, drop only `ON_CHANGE`.** Rejected — it removes the symptom
  but keeps the disease (implicit fan-out on every store), and does not give the
  caller the two operations the selective-subtree-propagation workflow needs.
- **Cap `propagate` delivery at its `root`.** Rejected — it makes the dirty mark
  per-`(vertex, root)` (or per-subscriber) instead of one bit per vertex, to avoid a
  subscriber above `root` being starved. The "root filters *which* vertices flush;
  delivery follows the subscription graph" rule (§B.1) needs one bit and is
  consistent under overlapping roots.
- **A per-vertex dirty flag + full subtree walk on `propagate`.** Rejected in favor of
  the ordered dirty-key set — a walk is *O(subtree-size)* even when three leaves are
  dirty; the prefix-range drain is *O(dirty-in-subtree)*.

## Discussion

The split reframes what a graph vertex *is*: not a mailbox you `write` to (store and
send fused), but a **value cell** you `assign` and a **fan-out point** you
`propagate` — two operations the caller composes. `ON_CHANGE` was the runtime
guessing the caller's intent from the data; dirty-tracking is the runtime
**recording the caller's intent from the operations**. The first is value-coupled and
nondeterministic at the layer boundary; the second is structural, value-agnostic, and
exactly the coalescing a timer-driven producer wants.
