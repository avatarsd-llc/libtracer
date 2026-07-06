<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0005 — Subtree subscriptions: vertical bubbling, branch-write decomposition, write-creates

| Field | Value |
| ---- | ---- |
| **RFC** | 0005 |
| **Title** | Subtree subscriptions: vertical bubbling, branch-write decomposition, write-creates |
| **Status** | **accepted** (2026-07-03, maintainer-ratified design grill) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-03 |
| **Comment window** | waived by the maintainer (solo-maintainer project, GOVERNANCE.md window dead ceremony) |
| **Tracking issue** | [#66](https://github.com/avatarsd-llc/libtracer/issues/66) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |

> **Partially superseded by [RFC-0008](0008-vertex-operations-assign-propagate.md) (2026-07-06):** the per-subscriber `delivery_mode` / ON_CHANGE byte-diff delivery policy (§A) is removed; selective propagation is now **structural** (dirty-tracked `assign` / `propagate`), not value-based. The subtree-subscription, vertical-bubbling, branch-write-decomposition, and write-creates semantics all stand — RFC-0008 restates bubbling in terms of `propagate` over the dirty set.

## Summary

Every subscription becomes a **subtree subscription**: a `SUBSCRIBER` edge on a
vertex V observes writes to V **and to every descendant of V** (a leaf
subscription is the trivial case). A write at a vertex therefore delivers to
subscribers at that vertex and at each of its ancestors — **vertical bubbling** —
carrying the **written TLV as-is** (the frame at the granularity the producer
chose), through the existing delivery machinery. Symmetrically in the write
direction, a **branch write** — a `POINT` (`0x07`) tree written to V —
**decomposes**: each value-carrying node lands at the corresponding descendant
vertex as a refcount **subview** of the written frame (address-shift-style
zero-copy slicing, no re-encoding), and each covered subscription point is
notified once with its slice. A write (including a decomposed one) targeting a
vertex that does not exist **creates it**, `mkdir -p` style, gated by the
existing `CREATE` ACL bit. Together these resolve
[#66](https://github.com/avatarsd-llc/libtracer/issues/66)'s structural-
observation ask and the batching story, with **no new wire verbs and no new type
codes** — the one wire-layout change is that `POINT` gains an optional `VALUE`
child (the vertex's own value).

## Motivation

1. **Structural observation (#66).** `io_layer`-style "notify on child
   appear / value change" maps onto "subscribe to the parent" only if the parent
   subscription actually sees descendant writes. Previously reference/02
   §composite subscription *described* this, but the semantics (what exactly is
   delivered, when, at what cost to non-subscribed writers) were unratified and
   unimplemented. This RFC pins them. A newly **appeared** child surfaces as its
   first write bubbling to the parent subscriber (and write-creates means
   appearance *is* the first write); a child's **value change** surfaces the
   same way. Child **removal** delivery remains the one open point of #66 —
   explicitly out of scope here (there is no vertex-delete surface yet).
2. **Batching without a wire container.** A producer that samples many leaves
   coherently needs a way to push them together. The branch write gives it one
   frame per subtree — decomposed at the terminus into per-leaf truth — while
   "several subtrees" is simply N self-contained frames in one `send(iov)`.
   There is **no wire batch container** (the retired-LIST lesson, ADR-0003).
3. **The producer owns cadence.** Rate caps, flush intervals, dirty tracking and
   timers are **explicitly not libtracer concerns**: the producer decides when
   and at what granularity to push (leaf, branch, several subtrees), and there
   is no per-subscriber QoS beyond the existing byte-agnostic delivery policy.
   Recording this boundary is part of this RFC's rationale.

## Proposed change

### A. Subtree subscription semantics (vertical bubbling)

- A subscription edge stored at vertex V (`:subscribers[]`, docs/reference/05
  §`0x04`) MUST observe writes to V and to any descendant of V.
- A write at vertex W MUST deliver to the subscribers of W and of each ancestor
  of W, **once per subscriber**, in addition to W's own fan-out.
- The delivered payload is the **written TLV as-is** — the exact frame the
  producer wrote, at the granularity it was written. No re-encoding, no
  path-tagging envelope. Local subscribers receive the same `view_t`/span they
  do today; remote subscribers receive it through the existing return-route
  `FWD{WRITE}` delivery path unchanged. Any provenance a consumer needs beyond
  this travels **in the data** (CONTEXT.md §SUBSCRIBER direction); wire-level
  concrete-path tagging for remote deliveries remains the separate, still-draft
  RFC-0003 proposal and is not changed by this RFC.
- The per-subscriber delivery policy (`delivery_mode`, ON_CHANGE byte-diff,
  `delivery_compact`, …) applies to bubbled deliveries exactly as to direct
  ones — the policy is evaluated at the subscriber's edge, producer-side.
- `await` is unchanged: it observes stores **at its own vertex** (the readiness
  plane of a single identity). Subtree observation is a subscription concern.

**Cost model (normative for the reference implementation, informative for
others).** The write path MUST stay near-free when nobody listens: the
implementation maintains per-vertex listener bookkeeping updated at
subscribe/unsubscribe time so a write performs the ancestor walk **only when a
subscriber exists at or above it**. The reference implementation stores on each
vertex (a) its own active-subscriber count and (b) an **ancestor-listener
count** — the number of active subscriber slots on strict ancestors —
maintained by a subtree walk at subscribe/unsubscribe (control-plane frequency)
and summed from the ancestor chain when a vertex is created (O(depth), so
vertices born under a live subscription are covered). The write hot path pays
exactly **one relaxed atomic load** when idle. This scheme was chosen over the
alternative (a boolean "listener-at-or-above" flag set by subscribe walking the
subtree) because a counter composes under multiple overlapping subscriptions
and unsubscribe without re-walking, and the creation-time sum covers
late-created descendants for free; both schemes walk the same subtree at
subscribe time. The walk-only-when-listening property is observable via
`graph_t::ancestor_walks()`.

### B. Branch write (`POINT` decomposition)

**Wire layout.** `POINT` (`0x07`, docs/reference/05 §`0x07`) gains one optional
child, immediately after the leading `NAME`:

```
POINT (PL=1) {
  NAME           vertex_name        ; required — the leaf segment (first child)
  VALUE          value              ; NEW, optional — the vertex's OWN value
  DESCRIPTION    description        ; optional (descriptor use)
  SETTINGS       default_settings   ; optional (descriptor use)
  SUBSCRIBER     sub_N…             ; zero or more (descriptor use)
  POINT          child_N…           ; zero or more, recursive
}
```

**Semantics.** A write whose payload TLV is a `POINT` is a **branch write**: the
written tree is rooted **at the target vertex** (the root `POINT`'s `NAME` MUST
equal the target's leaf segment — mismatch is `tr::path::invalid`), and it
DECOMPOSES:

- Each **value-carrying node** (a `POINT` carrying a `VALUE` child) MUST be
  stored at the corresponding descendant vertex — the vertex whose path is the
  target's path extended by the chain of `NAME`s — as a refcount-bumped
  **subview of the written frame** (zero copy; the ADR-0041/0042 small-payload
  one-copy rules at a remote terminus apply to the whole frame once, before
  decomposition, exactly as today).
- **Values are the truth at the vertices where they land; a branch is a view.**
  A node without a `VALUE` stores nothing (its vertex's stored value, if any,
  is untouched).
- A landing vertex that does not exist is **created** (§D).
- Each covered subscription point is notified **once**, with the smallest
  subview of the written frame covering every value landed at-or-below it: the
  `VALUE` slice for a leaf landing site, the node's whole `POINT` subtree for
  an interior node, and the written TLV as-is at the root and (via §A
  bubbling) above it.
- **Strictness.** In a branch write, a node's children MUST be exactly: the
  leading `NAME`, at most one `VALUE`, and zero or more `POINT` sub-branches;
  any other child type, a second `VALUE`, or any trailer-carrying node
  (`opt.TS/CR/CW/TF` set anywhere in the tree) is rejected with
  `tr::schema::type_mismatch` and nothing lands. (A stored slice is a subview —
  a trailer could not be sliced off without a copy; stored values are
  trailer-less at rest, ADR-0041 §4.) A branch carrying no `VALUE` anywhere is
  a valid no-op: nothing stored, nothing delivered.
- A `POINT` written to a HANDLER-role vertex is handed to its `on_write` as-is
  (the handler owns its own semantics); decomposition applies to the graph's
  stored-value plane.

Example — one frame updates two leaves and the parent observes the whole branch:

```{mermaid}
sequenceDiagram
    autonumber
    participant P as Producer
    participant S as /s (subscriber here)
    participant T as /s/t (subscriber here)
    participant U as /s/u (created on the fly)
    P->>S: write POINT{NAME s, POINT{NAME t, VALUE a}, POINT{NAME u, VALUE b}}
    Note over S: decompose — admission (ACL/create) first, then land
    S->>T: store subview VALUE a (refcount, zero copy)
    S->>U: write-creates /s/u, store subview VALUE b
    T-->>T: notify subscriber with its VALUE-a slice
    S-->>S: notify subscriber with the written branch TLV as-is
    Note over S: …and bubble the same frame to any ancestor subscriber
```

### C. Reads — one store per vertex; atomicity non-promise

- **Invariant:** ONE store per vertex — a write at any granularity lands in the
  same canonical last-known-value slot that reads serve. A read MUST return the
  **latest stored value**, which is **≥** (at least as new as) what any
  subscriber of that path last saw — never behind a notification, but
  legitimately newer (a later write may have landed since).
- **Cross-leaf atomicity is explicitly NOT promised.** Each leaf store is a
  consistent refcounted snapshot; the branch is **not a transaction**. In the
  reference implementation, branch **admission** (shape validation, creation
  gating, per-landing-vertex WRITE gating) is all-or-nothing — a denial rejects
  the whole branch with nothing landed — but **application** is per-leaf: a
  concurrent reader may observe some leaves updated before others, and a
  handler-role landing site may refuse its slice without un-landing the rest.
  Producers needing snapshot coherence use the existing coherent-sampling
  group identity (`(origin, ts)`, ADR-0019), not a transactional write.
- `READ` of a vertex keeps its existing meaning: it returns **that vertex's**
  stored value only. This RFC does **not** add a composed subtree-read
  operation (a read that re-assembles a `POINT` tree from descendant stores) —
  the existing `read(<parent>:children[])` member enumeration and per-leaf
  reads cover the current need. A composed subtree-read is a possible
  follow-on RFC.

### D. Write-creates (`mkdir -p`, CREATE-gated)

- A **data write** (no `:field` selector) targeting a vertex that does not
  exist MUST create it — and every missing intermediate level — as
  stored-value vertices, then proceed as a normal write. This replaces the
  previous `tr::path::not_found` outcome for local `write(path)` and for the
  remote `FWD{WRITE}` terminus.
- Creation is gated by the **existing `CREATE` access bit** (docs/reference/05
  §`0x0A`, ADR-0017/0020) evaluated on the **nearest existing ancestor**'s
  effective ACL — exactly the ACL every vertex of the missing chain would
  inherit. Denial is `tr::access::denied` (`PermissionDenied`), and nothing is
  created. With no existing ancestor at all, creation is open (the
  ACL-presence opt-in). As with `mkdir -p`, intermediates created during a
  branch write's admission may persist even if the write is subsequently
  denied at a deeper gate.
- **`:field` writes do not create** (there is no vertex whose control surface
  they could address), and `read`/`await` of a nonexistent vertex keep
  `tr::path::not_found`. Subscribing to a not-yet-existing vertex is therefore
  still an error; pre-create it with a data write (or `:children[]`) first.

### E. Explicitly out of scope (recorded rationale)

**Not libtracer's, by design:** rate caps, flush intervals, dirty tracking,
timers — the producer owns cadence and explicitly pushes (a leaf, a branch, or
several subtrees; batching is N self-contained frames in one `send(iov)`, never
a wire batch container). No per-subscriber QoS beyond the existing
byte-agnostic delivery policy. **Deferred:** child-removal delivery semantics
(#66's remaining open point — tied to a future vertex-delete surface);
wire-level concrete-path tagging of remote deliveries (RFC-0003, draft);
`delivery_scope = SNAPSHOT` producer-side re-aggregation (the snapshot remains
available as a read; the default — and currently only — delivery is the written
TLV as-is); a composed subtree-read op (§C).

### Files this RFC edits

- `docs/reference/05-protocol-tlvs.md` — §`0x04` SUBSCRIBER (subtree
  semantics + bubbling), §`0x07` POINT (the `VALUE` child + branch-write
  decomposition).
- `docs/reference/02-graph-model.md` — §write semantics (write-creates, the
  one-store invariant, the atomicity non-promise), §observing structural
  change (aligned to bubbling).
- `CONTEXT.md` — glossary entries: *subtree subscription / vertical bubbling*,
  *branch write / decomposition*, *write-creates*; the composite-subscription
  entry updated to the ratified delivery semantics.
- `core/` reference implementation + `core/CHANGELOG.md`.

## Compatibility

- **No existing conformance vector changes.** The `POINT` `VALUE` child is
  optional and NAME-position compatible (older descriptor consumers skip
  unknown/extra children by type, per §`0x07`'s existing child-typing rule);
  the codec already parses it generically. New behavior occupies previously-
  erroring space: writes that returned `tr::path::not_found` now create, and
  `POINT` payloads that stored opaquely now decompose — both are v1-draft
  refinements ratified before any release froze the old behavior.
- New vectors MAY be added under `tlv-types/` for POINT-with-VALUE (additive;
  adding a vector is not a spec change per spec §4).
- Implementations migrate by (1) bubbling writes to ancestor subscribers,
  (2) decomposing `POINT` writes, (3) creating on data-write miss. A node that
  has not migrated interoperates for all previously-valid traffic; it differs
  only in the newly-defined cases.

## Alternatives considered

- **A `:children_changed` facet / structural-event TLV** — rejected in #66
  triage already: it would duplicate what subscribe-to-the-parent delivers.
- **Per-leaf SUBSCRIBER edges for subtree coverage** — rejected: O(subtree)
  edges, and misses late-created children; the composite subscription with
  bubbling is one edge and covers write-created descendants by construction.
- **Delivering ancestors a re-encoded delta tagged with the concrete path** —
  rejected for this RFC: it re-encodes on the hot path and invents delivery
  metadata; the written-TLV-as-is rule keeps delivery zero-copy
  (scatter-gather/refcount of the written frame) and leaves wire path-tagging
  to RFC-0003 where remote interop actually needs it.
- **A wire batch container for coherent multi-leaf pushes** — rejected
  (ADR-0003 retired LIST): the branch write already *is* the container with
  semantics (a `POINT` tree), and multi-subtree batching is N frames in one
  `send(iov)`.
- **A boolean listener flag instead of counters** — see §A cost model.
- **Transactional branch application** — rejected: cross-leaf atomicity would
  require a global or subtree-wide lock across stores and handler dispatch,
  violating the lock-free LKV hot path (ADR-0015) for a guarantee coherent
  sampling already provides at the data level.

## Discussion

The 14-day comment window is waived by the maintainer for this RFC (the
standing solo-maintainer ruling also recorded on RFC-0002); the design was
ratified in the 2026-07-03 maintainer grilling session and is recorded here
with its rationale. Sustained objections: none.
