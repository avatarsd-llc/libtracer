<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0016 — Composed branch read: a plain READ of a branch serves the folded POINT tree of its registered subtree

| Field | Value |
| ---- | ---- |
| **RFC** | 0016 |
| **Title** | Composed branch read: a plain READ of a branch serves the folded POINT tree of its registered subtree |
| **Status** | **accepted** (2026-07-20 — maintainer approval, window waived). **Amendment 1 (2026-08-24)**: the composed walk skips **enumeration-hidden** vertices — the hide is a property of every enumeration-shaped surface, not of `:children[]` alone; direct addressing still sees them. |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-20 |
| **Comment window** | waived by the maintainer (standing solo-maintainer ruling, per RFC-0002/RFC-0005) |
| **Tracking issue** | none — design ruled in-session 2026-07-20; implementation landed first via [PR #451](https://github.com/avatarsd-llc/libtracer/pull/451) |
| **Target spec version** | v1 (draft refinement — occupies behavior RFC-0005 §C explicitly left open) |

> **Numbering note.** 0012, 0014 and 0015 are skipped deliberately — each carries ghost
> history (0012: the dtype/direction draft of PR #416, closed unmerged; 0014: a phantom
> mislabel, never a file; 0015: PR #446, withdrawn under the type-agnosticism gate) — so
> this RFC takes **0016**, the lowest number with no prior use.

## Summary

A plain `READ` of a vertex with **≥ 1 registered child** serves the **composed branch
read**: the folded `POINT` tree of the target's registered subtree, each node carrying
that vertex's stored TLV **verbatim**. It is the read-side dual of the RFC-0005 §B
branch-write decomposition, and it fills exactly the seat RFC-0005 §C reserved ("a
composed subtree-read is a possible follow-on RFC"). The reply is a **view composed over
the live last-known-value ropes** — refcount-cloned links plus borrowed name records,
never a copied or frozen snapshot — which is why the operation is *not* called a
snapshot. The point-in-time consistency contract carries over from RFC-0005 §C
unchanged: each leaf's value is a consistent refcounted store, but **cross-leaf tearing
is allowed** — the composed reply is not a transaction.

Shipped in the reference implementation by
[PR #451](https://github.com/avatarsd-llc/libtracer/pull/451): the branch/leaf fork in
`graph_t::read` and the composer `graph_t::read_subtree_folded`
(`core/src/graph.cpp`), gated by `core/tests/subtree_read_test.cpp`. Every byte claim
below is code-pinned by those functions.

## Motivation

1. **The measured join-time cliff.** Priming a UI over the graph plane previously
   required a recursive `read(<parent>:children[])` walk plus one `read` per leaf. On
   the reference deployment this is a **76-request prime**; the 2026-07-20 A/B
   benchmark measured **22.6 s vs 1.15 s time-to-all-endpoints (19.6×)** against the
   predecessor firmware's single aggregate snapshot, with the request trickle — not
   bandwidth — as the dominant cost. One composed read of the parent replaces the
   entire prime.
2. **The seat was already reserved.** RFC-0005 §C pinned the one-store-per-vertex
   invariant and the atomicity non-promise, and explicitly deferred the composed
   subtree-read as a follow-on. Reference/05 §`0x07` already described a value-bearing
   `POINT` tree as "the read-side dual of the branch write"; this RFC makes that
   sentence true of the read surface itself.
3. **No new machinery.** The reply reuses the `POINT` grammar the branch write already
   defines — no new wire verbs, no new type codes, no envelope.

## Proposed change

### A. Wire shape — the exact dual of the branch-write decomposition

`READ` of a vertex with at least one **registered** child returns one `POINT`
(`0x07`, `opt.PL = 1`) composing the target's registered subtree:

```
composed(target) = POINT{ [stored TLV of target]?, child_node* }
child_node(c)    = POINT{ NAME(c), [stored TLV of c]?, child_node(grandchild)* }
```

- A node's value slot is that vertex's **stored TLV verbatim** — the landed
  last-known-value bytes, opaque to the composer. A stored non-`VALUE` TLV (e.g. a
  `STATUS` fault record, reference/02 §invalid/fault) composes as-is.
- `child_node` is byte-for-byte the node shape of the RFC-0005 §B branch write (leading
  `NAME`, optional value, recursive `POINT` sub-branches). The one asymmetry is at the
  root: the branch-*write* root carries a leading `NAME` echoing the target's leaf
  segment, while the composed-*read* root carries none — the addressed path is the
  root's identity, and its own stored TLV (if any) leads the root body.
- Headers are emitted exactly as `wire::emit_tlv` would: `opt.LL` auto-widens the
  length field from u16 to u32 at the 0xFFFF boundary, per level
  (`graph_t::read_subtree_folded`, `core/src/graph.cpp`).

Canonical bytes — the read-back of RFC-0005 §B's example (`/s` value-less, leaves
`/s/t` = `'a'`, `/s/u` = `'b'`):

```
07 40 1C 00                ; POINT PL=1, len 28 — composed(/s); no own stored TLV
   07 40 0A 00             ;   child_node(t), len 10
      02 00 01 00 74       ;     NAME "t"
      01 00 01 00 61       ;     VALUE 'a'   — stored TLV of /s/t, verbatim
   07 40 0A 00             ;   child_node(u), len 10
      02 00 01 00 75       ;     NAME "u"
      01 00 01 00 62       ;     VALUE 'b'   — stored TLV of /s/u, verbatim
```

### B. Semantics rules

All of the following are shipped behavior, pinned by `graph_t::read` /
`graph_t::read_subtree_folded` (`core/src/graph.cpp`) and gated by
`core/tests/subtree_read_test.cpp`:

- **The fork is on registered children.** A vertex with ≥ 1 registered child serves the
  composed branch read; a leaf serves its stored value **byte-identically to before**;
  a HANDLER-role target keeps `on_read` precedence (§C below); field reads
  (`read(v, field, caller)` — `:children[]`, `:schema`, `:acl`, …) are unchanged.

  > **Erratum (2026-08-13), [#1030](https://github.com/avatarsd-llc/libtracer/issues/1030)
  > — see §Erratum at the end of this document.** The fork's predicate is answered
  > lock-free; a read racing the retirement of the target's **last** registered child may
  > take the composed path and then find no registered child under the walk's own lock,
  > composing the **root alone** — a `POINT` with zero child records. That reply is
  > **legal**: it is byte-identical to the fully-pruned reply the READ-ACL prune below
  > already produces deterministically, so it is not a new shape.
- **Landed LKVs only.** Each node contributes one atomic load of its stored value.
  Descendant HANDLER `on_read` seams are **never invoked** during the walk — a
  descendant handler with no stored value simply contributes no value slot.
- **Placeholders and synthesized listings are absent.** Unregistered placeholder
  vertices are skipped exactly as `:children[]` enumeration skips them; synthesized
  `on_children` transport listings are not graph children and do not appear.

  > **Amendment 1 (2026-08-24) — see §Amendment at the end of this document.** "Exactly as
  > `:children[]` enumeration skips them" is the whole predicate, not just its placeholder
  > arm: an **enumeration-hidden** vertex (RFC-0014 §3's creator endpoint) is likewise not a
  > member of the composed tree, and neither is its subtree. Direct addressing still resolves
  > it — hiding governs enumeration, never reach.
- **Per-node READ ACL prune.** The root is gated as any read
  (`PERMISSION_DENIED` when the caller may not READ the target). Below the root, a
  vertex the caller may not READ **prunes its whole subtree** — silently, siblings
  unaffected; an explicitly-allowed vertex under a denied ancestor is still pruned
  (structural — set-equivalent to the gated enumerate-and-read walk it replaces).

  > **Erratum (2026-08-13), [#1030](https://github.com/avatarsd-llc/libtracer/issues/1030)
  > — see §Erratum at the end of this document.** The prune may remove **every** child: a
  > caller allowed READ on the branch and denied on each of its children receives a
  > composed `POINT` carrying the root's stored TLV (if any) and **zero child records**.
  > A decoder of the composed reply MUST accept the zero-child-record shape.
- **A value-free branch serves a names-only topology tree** — the nested `POINT`/`NAME`
  skeleton with no value slots — instead of the previous `NOT_FOUND`.
- **Consistency (carried from RFC-0005 §C, unchanged).** One store per vertex; each
  composed value is the latest landed store, ≥ what any subscriber last saw. Cross-leaf
  atomicity is **not** promised: a concurrent writer may land between per-node loads,
  so the composed reply may tear across leaves. Producers needing snapshot coherence
  use the coherent-sampling `(origin, ts)` group (ADR-0019), as before.

### C. The ambiguity pin — POINT-in-RESULT is unambiguously a composed read

On a **non-HANDLER** vertex, a stored TLV can never be a `POINT`: `is_branch_point`
(`core/src/graph.cpp`) decomposes **every** `POINT` (`0x07`, `opt.PL = 1`) written to a
stored-value vertex per RFC-0005 §B — the tree lands as per-descendant slices and the
`POINT` framing itself is never stored. A `POINT` in a read RESULT is therefore
**unambiguously the composed branch read**; a consumer needs no flag to distinguish
"stored value that happens to be a POINT" from "composed subtree" — the former cannot
exist.

The one carve-out is the HANDLER role, on both sides of the same line:
`is_branch_point` excludes HANDLER targets (a `POINT` written to one is handed to
`on_write` as-is, RFC-0005 §B), and a HANDLER target's `on_read` keeps precedence over
the composed read (the fork in `graph_t::read` sits **after** the handler seam; an
`on_read`-less handler stays `NOT_FOUND` even with registered children). A handler may
serve arbitrary bytes — including a `POINT` of its own construction — so **a consumer
reading a HANDLER vertex cannot assume the composed shape**. The pin holds for the
graph's stored-value plane, where the composer operates.

### D. Cost model and resolver contract (normative for the reference implementation)

The composer is zero-copy end to end: per node one atomic LKV load, value links
**refcount-cloned** (no byte copy), the child's canonical `NAME` record **borrowed in
place** over the pinned vertex, and one owned per-level `POINT` header — no flatten
anywhere. The walk is iterative over a **heap-backed stack**, so its bound is the allocator and
it needs no synthetic cap (per RFC-0006 doctrine); allocation failure is `BACKPRESSURE`.
See the erratum below — this clause previously attributed the bound to `kMaxSegments`.

With a subject resolver installed, `acl_allows` — and therefore the resolver
callback — runs **O(nodes) times per composed read under the shared map lock**
(`map_mutex_`). A resolver MUST NOT re-enter graph mutation APIs from that callback
(self-deadlock); see the contract note on `graph_t::read_subtree_folded`
(`core/include/libtracer/graph.hpp`).

### E. Non-goals

- **No delivery-plane change.** `delivery_scope = SNAPSHOT` producer-side
  re-aggregation stays deferred exactly as RFC-0005 §E left it; delivery remains the
  written TLV as-is. This RFC changes the *read* surface only.
- **Reply size is receiver-resource-bounded**, per the RFC-0006 model — no
  reply-size cap, no paging protocol, no magic-number limit in the runtime; bounds are
  injected resources / per-target configuration. *Implementation note (not a wire
  cap):* on the current ESP32-C6 reference deployment the WS egress path flattens the
  reply and thus bounds practical composed-reply size (~14 KB today); that is an
  egress-implementation ceiling scheduled for the zero-copy egress work, not a
  property of this operation.
- **No wire-path tagging.** The composed reply carries no concrete-path envelope
  beyond its own `NAME` nesting; wire-level path tagging of deliveries remains
  RFC-0003's seat (still draft) and is not changed here.

### Files this RFC edits

- `docs/spec/rfcs/0005-subtree-subscriptions.md` — §C composed-read deferral marked
  superseded; §E deferral list updated (this PR).
- `CONTEXT.md` — §graph-composition entry: the aggregate read is the composed branch
  read; the dead pre-RFC-0005 `read('/x:[]')` spelling removed (this PR).
- `docs/reference/02-graph-model.md`, `docs/reference/05-protocol-tlvs.md` — the read
  surface described descriptively, citing this RFC (this PR).
- `core/` reference implementation + `core/CHANGELOG.md` — landed ahead of this
  document via [PR #451](https://github.com/avatarsd-llc/libtracer/pull/451).

## Compatibility

- **New behavior occupies previously-erroring or unratified space.** Leaf reads,
  HANDLER-target reads, and every field read are byte-identical to before
  (regression-gated by `subtree_read_test`). What changes is the branch-target read:
  previously a branch vertex with no own stored value read `NOT_FOUND`, and one with a
  stored value served it bare; both are v1-draft refinements ratified before any
  release froze the old behavior (no released v1 yet).
- **No conformance-vector change.** The `tests/conformance/` harness is pinned to
  wire-codec round-trips (`encode(decode(input)) == input`); a graph-level
  READ-semantics vector has no seat there. The canonical composed bytes ship in §A of
  this document instead.
- Implementations migrate by serving the composed tree from the branch/leaf fork;
  consumers already parsing `POINT` (the branch-write grammar) parse the reply with
  the same code.

## Alternatives considered

- **A distinct wire verb or `:field` for the composed read** — rejected: the plain
  `READ` slot is unambiguous by §C's pin, and a new verb would violate the
  read/write/await surface (ADR-0006).
- **Invoking descendant `on_read` seams during composition** — rejected: it would turn
  a lock-scoped walk into arbitrary user code under the map lock, and make reply bytes
  depend on handler side effects; the composed read serves *landed* truth only.
- **A frozen (copying) snapshot** — rejected, and the reason the operation is not
  *named* snapshot: copying the subtree would abandon the zero-copy rope model
  (ADR-0053) for a coherence guarantee RFC-0005 §C deliberately does not promise;
  coherent sampling stays at the data level (`(origin, ts)`, ADR-0019).
- **Serving `NOT_FOUND` for value-free branches** (status quo ante) — rejected: the
  topology tree is real information (the member skeleton), and the `NOT_FOUND` forced
  clients back into the recursive enumerate walk this RFC exists to remove.

## Discussion

The 14-day comment window is waived by the maintainer (the standing solo-maintainer
ruling recorded on RFC-0002/RFC-0005). The design was ruled in the 2026-07-20 session
("approved, go ahead") driven by the 2026-07-20 A/B benchmark, and implemented
immediately in [PR #451](https://github.com/avatarsd-llc/libtracer/pull/451); this
document records the ratified semantics and rationale. Sustained objections: none.

---

## Erratum (2026-07-30): §D attributed the walk's bound to `kMaxSegments`, which does not bound it

§D read:

> The walk is iterative (graph depth is `kMaxSegments`-bounded structurally — no synthetic cap,
> per RFC-0006 doctrine)

Two things are wrong with that parenthesis, and they pull in opposite directions.

**`kMaxSegments` does not bound graph depth.** It is enforced in exactly one place —
`path_t::parse` (`core/src/path.cpp:110`), the LOCAL string→bytes builder. `graph_t::ensure_vertex`
takes raw key bytes and counts no segments, and `wire::path_key` / `path_lookup_key` count none
either. A peer can therefore write-create a vertex at arbitrary depth today, and the walk this
clause describes can already meet one. The word "structurally" claimed an invariant that is not
enforced on the path that matters.

**And citing "RFC-0006 doctrine" for a fixed constant inverts that doctrine.** RFC-0006 exists to
*remove* a fixed 32 and replace it with a receiver-resource bound. A clause that reassures the
reader by naming a magic number, and credits RFC-0006 for it, teaches the opposite of what
RFC-0006 decided.

**The walk was always safe, for a better reason.** It is iterative over a heap-backed stack, so
the bound is the allocator and exhaustion surfaces as `BACKPRESSURE` — a real injected resource,
exactly as [CONTEXT.md §Resource bound](../../../CONTEXT.md) requires. Nothing about the
implementation changes; only the justification is corrected, and the corrected one is the one that
survives `kMaxSegments` being lifted.

Corrected at the two inheriting code sites that carried the same sentence:
`core/include/libtracer/graph.hpp` (`parse_branch_node`'s contract) and `core/src/graph.cpp`
(pass 1 of the composed read).

**No normative requirement changes**, and the wire surface is untouched — hence an erratum per
[GOVERNANCE.md](../../../.github/GOVERNANCE.md), not an amendment.

---

## Erratum (2026-08-13) — a composed reply may carry zero child records; the racing-retire root-only compose is that same legal shape ([#1030](https://github.com/avatarsd-llc/libtracer/issues/1030))

§B's grammar walk-through and §A's examples show composed replies with one or more
`child_node` records, and nothing in the text said whether zero is possible. Two shipped
behaviors produce exactly that reply — the identical bytes, one deterministically and one
under a race:

**The READ-ACL prune already produces it, deterministically.** §B's per-node prune is
per-child: a caller allowed READ on the branch and denied READ on **every** child
receives `POINT{ [stored TLV of target]? }` — a composed reply with zero child records.
For a one-byte root value `0xDD` that is the five bytes `07 40 01 00 DD`. This is the
prune working exactly as §B specifies; it has been shipped behavior since the prune
landed, with no concurrency anywhere.

**The lock-free fork can produce it transiently.** Since
[#652](https://github.com/avatarsd-llc/libtracer/issues/652) the branch/leaf fork is
answered from the vertex's registered-child bit without the map lock; the composed walk
then takes the map lock separately and skips unregistered children. A read racing the
retirement of the target's last registered child can decide "branch" from the bit and
then find no registered child under the lock — composing the root alone. Measured at
roughly 3 % of reads under a four-reader register/write/retire hammer
([#1030](https://github.com/avatarsd-llc/libtracer/issues/1030)); byte-identical to the
ACL-prune reply above.

**The ruling (2026-08-12 grilling session, recorded on #1030): the shape is legal —
specify it, change no code.** A decoder that rejects the zero-child-record compose is
already broken against the deterministic ACL-prune case, which has nothing to do with
retirement; the race adds frequency, not a new shape. The alternative — re-checking the
fork under the walk's map lock and falling through to the leaf path — would put a branch
back on the read path #652 optimised while still leaving the ACL prune producing the
same shape, reducing nothing a decoder must handle.

Corrections, all in this document (§B, inline erratum notes at the two governing
bullets):

- The fork bullet gains the racing-retire transient: a read racing the retirement of the
  last registered child may compose the root alone, and that reply is legal.
- The prune bullet states the boundary case: the prune may remove every child, and a
  decoder MUST accept the zero-child-record shape.

Corrected alongside at the code sites that carry the same contract:
`core/include/libtracer/vertex.hpp` (`has_registered_child`'s "either side" weakening
gains the matching sentence) and the tests —
`core/tests/read_fork_test.cpp` now **classifies** every free-running read of the racing
harness (the root-only compose is counted and accepted; any other unexpected shape
fails), and `core/tests/subtree_read_test.cpp` pins the deterministic ACL-prune case: a
caller allowed on the branch and denied on every child receives a composed reply with
zero child records.

**Instrument: erratum, not amendment** ([GOVERNANCE.md](../../../.github/GOVERNANCE.md)).
The wire surface is unchanged — the zero-child-record reply is producible by shipped,
already-specified behavior (the §B prune), and this correction only writes down that the
composed grammar's `child_node*` repetition genuinely includes zero. No frame shape, type
code, or error identity changes. Maintainer ruling recorded on
[#1030](https://github.com/avatarsd-llc/libtracer/issues/1030) (grilled 2026-08-12).

## Amendment 1 (2026-08-24, ruled) — the composed walk skips ENUMERATION-HIDDEN vertices; the hide belongs to every enumeration-shaped surface

**What §B said.** *"Placeholders and synthesized listings are absent. Unregistered placeholder
vertices are skipped exactly as `:children[]` enumeration skips them."* The bullet names the
membership predicate by analogy to `:children[]` and then lists only one of its two arms. The
implementation followed the text literally: the composed walk tested `registered()`, while both
`:children[]` doors test `registered() && !enumeration-hidden`.

**What that produced.** [RFC-0014](0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)
§3 hides the creator endpoint `/net/<module>/conn` from its module's `:children[]`: the endpoint is
the control that *creates* connections, not one of them, so a peer walking the listing as a topology
of links would descend into a vertex with no peer behind it. A composed READ of `/net/<module>` served
it anyway — the same question ("what is under here?") answered two different ways by the same node.

**The amendment.** A vertex marked hidden from enumeration is **not a member of any
enumeration-shaped surface**. Concretely:

- **`:children[]`** (both the materialized and the folded door) — unchanged, it always hid it.
- **The composed branch read** — now shares that one predicate. A hidden child contributes no node,
  and therefore no subtree: its descendants are unreachable through the composed door for the same
  reason its own record is absent.
- **Direct addressing always sees it.** Hiding is about ENUMERATION, never reach: the hidden vertex
  resolves, reads, and is written exactly as before. That is what keeps RFC-0014 §6's
  `read <module>/conn:schema` **creatability probe** — the sanctioned discovery route for a surface
  that is deliberately unlisted — working at all.
- The ACL prune of §B is **unaffected and independent**: prune is per-caller, hiding is per-vertex,
  and a reply may be shaped by either or both.

**Why an amendment and not an erratum.** The composed reply of a branch with a hidden child loses a
child record, so bytes on the wire change for that one shape. No grammar, type code or error identity
changes — the resulting reply is an ordinary composed `POINT`, indistinguishable in shape from the
same branch without that child, and the zero-child-record boundary of the 2026-08-13 erratum already
covers the degenerate end. Window waived (solo-maintained).

**Pinned by** `core/tests/subtree_read_test.cpp` —
`test_hidden_child_absent_from_composed_read`: the composed tree before the hide (both children),
after it (only the enumerable child), `:children[]` agreeing, and the hidden vertex still resolving
and still answering `:schema`.
