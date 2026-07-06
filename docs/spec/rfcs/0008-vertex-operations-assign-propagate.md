<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0008 — Vertex operations: `assign` and `propagate`; structural selective propagation; value-agnostic per-vertex `delivery_mode`

| Field | Value |
| ---- | ---- |
| **RFC** | 0008 |
| **Title** | Vertex operations: `assign` and `propagate`; structural selective propagation; value-agnostic per-vertex `delivery_mode` |
| **Status** | **accepted** (2026-07-06, maintainer-ratified design discussion; amended 2026-07-06b) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-06 |
| **Comment window** | waived by the maintainer (solo-maintainer project, GOVERNANCE.md window dead ceremony) |
| **Tracking issue** | [#235](https://github.com/avatarsd-llc/libtracer/issues/235) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |

> **Amendment 2026-07-06b (maintainer, live design discussion).** The first draft of this
> RFC (merged in [#236](https://github.com/avatarsd-llc/libtracer/pull/236)) said the
> `delivery_mode` field was **removed entirely**. That is corrected here. Removing the
> **value-based** filter (`ON_CHANGE` byte-diff) and the **throttle** (`THROTTLED` /
> `min_interval_ns`) stands. But `delivery_mode` itself is **not deleted — it is redefined
> as a value-agnostic, per-vertex policy** governing whether an *ancestor's* `propagate`
> sweep includes a vertex, with three modes: `UNCONDITIONAL`, `IF_NEWER` (default), and
> `EXPLICIT`. The default `IF_NEWER` *is* the structural dirty-flush described in §B; the
> other two are per-vertex overrides. The wire change is unchanged from the first draft (a
> pure removal from `SUBSCRIBER.qos_settings`); the new policy is a **per-vertex host
> attribute**, and configuring it over the wire reuses the vertex `:settings` mechanism
> (deferred, §C). §§Summary, B, C, F, Files, and Alternatives are revised accordingly.

## Summary

The conflated `write` operation (store the value **and** notify subscribers, in one
call) splits into the **two irreducible operations** it was always hiding:

- **`assign`** — the vertex-local state transition: replace the vertex's value. Reads
  no edge and sends nothing. (In graph terms, relabel a vertex; in C++ terms, the
  `operator=` — and with rope-valued vertices ([ADR-0053](../../adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md) §6) it is literally an atomic
  last-known-value swap.) It also bumps the vertex's monotonic **write sequence** (§B).
- **`propagate`** — the edge transition: deliver a vertex's value along its
  subscription edges to other vertices (and remote subscribers). Sends; does not
  mutate.

The runtime **stops inspecting vertex values to decide delivery.** The value-based
delivery filter (`delivery_mode == ON_CHANGE`, "deliver only when the value bytes
differ from last delivered") and the throttle (`THROTTLED` / `min_interval_ns`) are
**removed** — including from the `SUBSCRIBER.qos_settings` wire encoding. In their
place, selective propagation is **structural**: `assign` advances a vertex's write
sequence; a `propagate(root)` sweep flushes **only the descendants actually assigned
since the sweep last covered them** — no value comparison. What "changed" means is
decided by *which operations the caller performed*, never by comparing bytes.

`delivery_mode` survives this, **redefined and relocated**: from a per-subscriber,
value-based filter to a **per-vertex**, value-agnostic policy — `UNCONDITIONAL` /
`IF_NEWER` (default) / `EXPLICIT` — that governs how a vertex participates in an
ancestor's sweep (§C). It never reads a byte; it selects, by ordering and intent,
*which* vertices an ancestor sweep pulls in.

This resolves [#235](https://github.com/avatarsd-llc/libtracer/issues/235). It
**supersedes** the value-based `delivery_mode` / `ON_CHANGE` clauses of accepted
[RFC-0004](0004-remote-operation-addressing.md) §E and
[RFC-0005](0005-subtree-subscriptions.md) §A and reference/05 — a reversal the
maintainer has authorized. No new wire verbs, no new type codes; the one wire change
is a **removal** (the `delivery_mode` / `min_interval_ns` / `keepalive_ns` QoS keys),
with the new per-vertex policy carried as host state (wire config deferred, §C).

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
the signal for it is the vertex's **write sequence** advancing under `assign` — a fact
the runtime owns, recorded at the moment of the operation — not a value comparison.
Separating `assign` from `propagate` makes this expressible and makes every delivery
an explicit act: the graph never fans out behind the caller's back. And where a
producer wants a vertex *always* re-sent by the sweep (a slow keepalive), or *never*
swept up by an ancestor (deliver it only by hand), that too is an ordering/intent
choice — the three `delivery_mode` values (§C) — never a value one.

## Proposed change

### A. The two operations

A vertex sits in two orthogonal planes. The **state plane** holds its value; the
**propagation plane** is its outgoing subscription edges. Exactly two primitives act
on a vertex, and they touch disjoint planes:

- **`assign(v, value)`** — replace `v`'s value. Effects, all vertex-local: swap the
  last-known-value (atomic), **increment `v`'s write sequence** (§B), append to the
  stream ring if `v` is a stream, and wake any `await` waiter on `v`. No fan-out.
  Idempotent in *stored state* (assigning the same value twice leaves the same value)
  but it always advances the write sequence — the record that an operation happened.
- **`propagate(v)`** — deliver, along subscription edges, the value of `v` itself
  (always — §C) **and** of the qualifying descendants in the subtree rooted at `v`
  (§B/§C). No value argument: the last-known-value is the single source of truth, so
  there is no ambiguity about *which* value is sent. `propagate` does not get to decide
  *whether* to deliver what it selected — only to deliver it.

`read` and `await` are unchanged and both live in the **state** plane: `await`
observes `assign`s at its own vertex (the readiness plane of a single identity,
[RFC-0005](0005-subtree-subscriptions.md) §A), independent of propagation.

**`write` is removed.** The one call that did both is gone; a caller performs the two
operations explicitly. The wire mapping is unchanged (§D): a `FWD{WRITE}` arriving at
a terminus *is* an `assign` followed by a `propagate` of that vertex.

### B. Structural selective propagation: the write sequence

Every vertex carries a monotonic **write sequence** `write_seq` (a counter, never the
value's bytes), incremented by every `assign`. A sweep records, per vertex it
includes, the `write_seq` value at that inclusion (`swept_seq`). A vertex is **pending**
— it was assigned since a sweep last covered it — exactly when `write_seq > swept_seq`.
This is the structural, value-agnostic replacement for a "dirty bit"; a counter (rather
than a bit) is chosen so inclusion is well-defined under overlapping sweeps and leaves
room for future per-observer sequencing.

`propagate(root)` **sweeps the subtree rooted at `root`** and delivers each vertex it
selects to that vertex's observers, then advances the vertex's `swept_seq` to its
current `write_seq`. Selection is governed per vertex by its `delivery_mode` (§C); in
the **default** mode (`IF_NEWER`) a descendant is selected exactly when it is pending —
so a vertex not assigned since the last covering sweep is **skipped**, not because its
bytes match anything, but because the runtime holds no record that it was operated on.

Two rules keep this minimal and consistent:

1. **Per-vertex sequence (one small counter per vertex), delivery to the full observer
   set.** When a sweep selects `u`, `u` is delivered to *every* subscriber that
   observes it — `u`'s own edges **and** every ancestor carrying a subtree
   subscription (the [RFC-0005](0005-subtree-subscriptions.md) §A fan-out + vertical
   bubbling), including ancestors **above** `root`. The `root` argument selects
   *which* vertices flush; it does **not** cap *who* receives them. Consequence: if `u`
   is observed from two different ancestors, the first sweep that covers `u` delivers it
   to *both* (bubbling reaches both) and advances its `swept_seq`, so a second
   overlapping sweep correctly finds `u` no longer pending — no double-send, no missed
   observer, one counter of state. (Capping delivery at `root` was considered and
   rejected; it would force per-`(vertex, root)` bookkeeping — see Alternatives.)
2. **Coalescing is free.** `assign` overwrites the last-known-value (last-writer-wins)
   and advances the sequence; it does not enqueue. So *k* assigns to the same vertex
   between two sweeps flush **once**, with the latest value. A producer may `assign` at
   any rate and `propagate` on a timer at a lower rate; the timer rate is the delivery
   rate, and only touched vertices ride it.

**Efficiency.** A default (`IF_NEWER`) sweep costs *O(pending-in-subtree)*, not
*O(subtree-size)*: pending vertices are held in an **ordered set of canonical PATH
keys**, and a subtree is a contiguous **prefix range** of that order (a parent's key is
a byte-prefix of every descendant's, the property `key_view_t::is_ancestor_of` and the
bubbling walk already rely on). `propagate(root)` iterates the range `[key(root), …)`
while the prefix holds; `assign` inserts the key (unless the vertex is `EXPLICIT` —
§C — which never rides an ancestor sweep). A large, mostly-clean subtree with three
pending leaves flushes those three and touches nothing else. `UNCONDITIONAL` vertices
(which must be swept even when not pending) are held in a second ordered key set the
sweep also iterates by prefix range — so the cost is *O((pending + unconditional)-in-
subtree)*, still independent of the clean-and-quiet remainder. (As an idle
optimization, `assign` MAY skip inserting a vertex that has no subtree subscriber above
it, reusing RFC-0005's `listeners_above_` counter — a sweep would deliver it nowhere.
This optimizes the mechanism, not the semantics: with no observer, pending-or-not is
unobservable.)

**Branch assign composes.** A `POINT` tree assigned to `v`
([RFC-0005](0005-subtree-subscriptions.md) §B branch write) decomposes exactly as
before, except the store half is now `assign` at each value-carrying descendant
(advancing each write sequence) and there is **no implicit notify**. The producer then
calls one `propagate(v)` to flush the whole touched set selectively — which is precisely
the "update part of a subtree, then propagate the parent" workflow this RFC exists to
support.

### C. `delivery_mode`: a value-agnostic, per-vertex propagation policy

`delivery_mode` is not a per-subscriber value filter (that is deleted). It is a
**per-vertex** attribute — a property of the vertex's storage, like its role — that
governs **whether an ancestor's `propagate` sweep includes this vertex**. Three modes:

| Mode | An ancestor sweep includes this vertex… |
| ---- | ---- |
| `UNCONDITIONAL` | **always** — deliver its current value on every covering sweep (a sweep-driven keepalive; the producer's timer sets the rate). |
| `IF_NEWER` *(default)* | **only if pending** (`write_seq > swept_seq`, §B) — the structural coalescing flush. |
| `EXPLICIT` | **never** — an ancestor sweep skips it entirely; it is deliverable only by a **direct `propagate` on the vertex itself**. |

Two invariants make the modes coherent with §A:

- **`assign` is never gated.** Whatever the mode, `assign` swaps the value and advances
  the write sequence. The mode governs *propagation*, not *storage*.
- **A direct `propagate(v)` always delivers `v`.** The **argument** of a `propagate`
  call is its explicit target and is delivered regardless of its mode — the mode governs
  only the *descendants* a sweep pulls in. This is what makes `EXPLICIT` reachable
  ("deliver it only by calling `propagate` on it by hand") and matches the intuition
  that asking to propagate `v` propagates `v`. In particular, updating a vertex
  (`assign`) and notifying its own subscribers (a direct `propagate` on it) are both
  unaffected by `delivery_mode`; the mode changes only how the vertex behaves when an
  *ancestor* sweeps.

`delivery_mode` is therefore held as **host state on the vertex**, defaulting to
`IF_NEWER`. On the wire it **leaves `SUBSCRIBER.qos_settings`** (a subscription no
longer carries it — the source vertex owns the policy, not the observer). Configuring a
vertex's mode from a remote peer reuses the ordinary vertex-`:settings` write path (a
`delivery_mode` NAME/VALUE under the vertex's own `SETTINGS`, mirroring how
`:subscribers` / `:acl` hang off a vertex); this wire configuration is **deferred** and
optional — the core semantics need only the host attribute and its `IF_NEWER` default.
The value-based `ON_CHANGE` and the `min_interval_ns` / `keepalive_ns` throttles are
gone for good; the sole "re-send even when unchanged" need is served by `UNCONDITIONAL`
plus the producer's own `propagate` cadence. `delivery_compact`
([RFC-0004](0004-remote-operation-addressing.md) §E.1, label compaction) is a
**separate, orthogonal** subscriber hint and is **retained** — it concerns *how* a
route is encoded, not *whether* a value is delivered.

### D. Wire mapping: a delivery is still a write

The wire is unchanged except for the §C `qos_settings` removal. "A delivery *is* a
write" ([RFC-0004](0004-remote-operation-addressing.md) §D) still holds: a `FWD{WRITE}`
carrying a `VALUE`, arriving at its terminus, means **`assign` the addressed vertex,
then `propagate` it** — the two host primitives in sequence, delivering to that
vertex's own subscribers (local and remote) and bubbling to ancestor subtree
subscriptions. Because the terminus vertex is the **argument** of that `propagate`, it
is delivered regardless of its own `delivery_mode` (§C) — a directed write always lands.
A producer's selective subtree flush therefore reaches a **remote** subtree subscriber
as one `FWD{WRITE}` per selected vertex (driven by the single host `propagate`), exactly
as a local subscriber receives one callback per selected vertex. No wire batch primitive
is introduced; `propagate` simply drives the sends it selects.

### E. Stream vertices are a queue, not a coalesce

The write-sequence coalescing of §B is the **stored-value** semantic (last-writer-wins:
flush the latest once). A **stream** vertex (bounded history ring) is a *queue* — its
contract is "observe every buffered entry," not "the latest." Its propagation is a
**drain** of the entries buffered since the last flush, in order, not a coalesce. The
two roles keep their existing distinction; `propagate` dispatches on the vertex role,
and a stream's flush delivers each ring entry appended since the previous flush.

### F. Determinism properties (the contract)

- **No value inspection.** No operation reads a stored value's bytes to decide
  anything about delivery. `delivery_mode` decides by *ordering and intent* (the write
  sequence, and the three per-vertex modes), never by comparing bytes. Removing value
  inspection is the point; the modes preserve it.
- **Explicit composition, caller-ordered.** `assign` and `propagate` are sequenced by
  the caller; nothing fans out implicitly. `assign(A); assign(B); propagate(v)`
  deterministically propagates `B` (last-writer-wins).
- **One effect each.** `assign` touches only state (value + write sequence);
  `propagate` touches only edges (delivery + advancing `swept_seq`). Neither leaks into
  the other's plane.
- **Suppression is the application's, by construction.** To not deliver, do not
  `propagate` — or set the vertex `EXPLICIT`, or `read`, compare in application terms,
  and decide. The graph never substitutes its own value comparison for that judgement.

### Files this RFC edits

- **`docs/reference/05-protocol-tlvs.md`** — remove `delivery_mode`, `min_interval_ns`,
  and `keepalive_ns` from `SUBSCRIBER.qos_settings`; note `delivery_compact` (and the
  reserved `delivery_scope`) are retained; note `delivery_mode` is now a value-agnostic
  per-vertex attribute (default `IF_NEWER`), wire-configured via vertex `:settings`
  (deferred).
- **`docs/reference/12-deployment-profiles.md`** — the dispatcher's delivery policy is
  now **per-vertex** (value-agnostic `delivery_mode`), not per-subscriber; the throttle
  knob is gone.
- **[RFC-0004](0004-remote-operation-addressing.md) §E** — supersede the value-based
  `delivery_mode` reference (the QoS-hint list loses `delivery_mode`/`min_interval_ns`;
  `delivery_compact` stays; the value-agnostic per-vertex `delivery_mode` is the new
  home).
- **[RFC-0005](0005-subtree-subscriptions.md) §A** — supersede the "per-subscriber
  delivery policy (`delivery_mode`, ON_CHANGE byte-diff, …)" clause; restate bubbling
  in terms of `propagate` over the write-sequence selection rather than an implicit
  per-write fan-out.
- Reference-implementation changes (`graph_t` API: `assign`/`propagate`, the write-
  sequence selection sets, the redefined `delivery_mode_t`, removal of `ON_CHANGE` /
  `min_interval_ns` / `keepalive_ns`) are recorded in `core/CHANGELOG.md` and are
  **not** normative; they follow this RFC.

## Compatibility

**Breaking, and deliberately so — pre-1.0.** No v1 is released, so no v2 is needed
(the RFC-0005 precedent). The host API break (`write` → `assign`/`propagate`,
`delivery_mode` redefined per-vertex and value-agnostic) is a mechanical caller
migration. The wire break is a pure removal of the value-based/throttle
`qos_settings` keys; a stale peer's leftover `delivery_mode` under `qos_settings` is
ignored under the unknown-key rule, so there is no framing incompatibility — only the
(correct) loss of the value-based suppression it requested. The new per-vertex
`delivery_mode` adds no required wire field (host default `IF_NEWER`; wire config
deferred).

## Alternatives considered

- **Keep `ON_CHANGE` (value byte-diff).** Rejected — the motivation. Wrong layer
  (runtime inspecting application data), no meaningful `operator!=` on opaque bytes,
  and *O(N)* comparison at the scale it is meant to relieve.
- **Remove `delivery_mode` entirely** (this RFC's own first draft). Rejected on
  amendment — deleting the field throws out a genuine, value-agnostic need: a producer
  wanting a vertex *always* re-sent by the sweep (keepalive) or *never* swept up by an
  ancestor (hand-delivered only). Those are ordering/intent choices the runtime can
  honor without reading a byte; `UNCONDITIONAL` / `IF_NEWER` / `EXPLICIT` express them.
- **Keep `write` coupled, drop only `ON_CHANGE`.** Rejected — it removes the symptom
  but keeps the disease (implicit fan-out on every store), and does not give the
  caller the two operations the selective-subtree-propagation workflow needs.
- **`delivery_mode` per-subscriber (its old home).** Rejected — inclusion in a sweep is
  a property of the *source vertex* (does *this* vertex ride *its* ancestor's sweep),
  uniform across every ancestor that observes it, so it belongs on the vertex, not on
  each observing edge. Per-subscriber would also re-introduce the "who receives"
  coupling §B.1 deliberately avoids.
- **Cap `propagate` delivery at its `root`.** Rejected — it makes the selection state
  per-`(vertex, root)` (or per-subscriber) instead of one counter per vertex, to avoid a
  subscriber above `root` being starved. The "root filters *which* vertices flush;
  delivery follows the subscription graph" rule (§B.1) needs one counter and is
  consistent under overlapping roots.
- **A per-vertex flag + full subtree walk on `propagate`.** Rejected in favor of the
  ordered key sets — a walk is *O(subtree-size)* even when three leaves are pending; the
  prefix-range drain is *O((pending + unconditional)-in-subtree)*.

## Discussion

The split reframes what a graph vertex *is*: not a mailbox you `write` to (store and
send fused), but a **value cell** you `assign` and a **fan-out point** you
`propagate` — two operations the caller composes. `ON_CHANGE` was the runtime
guessing the caller's intent from the data; the write sequence and the three
`delivery_mode` modes are the runtime **recording the caller's intent from the
operations and the vertex's declared policy**. The first is value-coupled and
nondeterministic at the layer boundary; the second is structural, value-agnostic, and
exactly the coalescing — with keepalive and hand-delivery as the two deliberate
exceptions — that a timer-driven producer wants.
