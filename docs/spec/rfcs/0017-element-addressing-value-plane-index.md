<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0017 — Element addressing: `[n]` on the value plane, and per-element delivery

| Field | Value |
| ---- | ---- |
| **RFC** | 0017 |
| **Title** | Element addressing: `[n]` on the value plane, and per-element delivery |
| **Status** | **draft** (2026-07-28) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-28 |
| **Comment window** | waived by default while solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"); invoke explicitly if outside input is wanted |
| **Tracking issue** | to be filed with this document |
| **Target spec version** | v1 (draft refinement — `docs/spec/v1.md` is DRAFT and §3 is a stub; **amends** [RFC-0004](0004-remote-operation-addressing.md) §C) |

> **Numbering note.** 0012 and 0015 remain skipped for the ghost history
> [RFC-0016](0016-composed-branch-read.md) records; 0014 was subsequently issued as a real
> document, so this RFC takes **0017**, the next unused number.

## Summary

`[n]` today addresses an array **only inside a `:field`** — `:subscribers[3]` is expressible,
`/sensor/queue[3]` is not. This RFC extends the existing `FIELD` selector to the **value plane**
by allowing a `FIELD` with **zero levels**: one carrying only `index` + `index_mode` and no
`NAME`, which reads as *"no field name — the vertex's own value, element n."* `PATH` is
untouched.

The result is one array notation across the whole protocol. `[n]` selects the **n-th child TLV**
of whatever the addressed thing holds, whether that is the subscriber list, the child list, or a
vertex's stored value; `[]` appends exactly one element; the empty-`STATUS` sentinel of
[RFC-0009](0009-vertex-removal-and-subscriber-eviction.md) §D.1 clears one. A delivery
**mirrors the shape of the write that caused it**, so a producer that writes one element
notifies with one element — which makes per-element notification fall out of the addressing
rather than requiring a comparison, a cursor, or per-subscriber state.

## Motivation

**An array of values costs an array of vertices.** A node exposing 32 related values today
registers 32 vertices to make them individually addressable and individually notifiable.
`vertex_t` is gated at ~~112 B on 64-bit and **80 B on 32-bit**
(`core/tests/vertex_size_test.cpp`)~~ **96 B on 64-bit and 72 B on 32-bit
(`config_t::kMaxVertexBytes64` / `kMaxVertexBytes32`, asserted in
`core/include/libtracer/vertex.hpp`)** *(Erratum 1, 2026-08-24: both figures and the citation
were stale — the ratchets moved to `config_t` and are now pinned to the measurement; #1487's
census re-took them field by field on both ABIs. The motivation is unaffected in kind: a
32-vertex array still costs 32 vertex bodies.)*,
and each vertex additionally carries a canonical PATH key and a vertex-map entry. On an
ESP32-C6 whose measured idle heap floor is ~4.3 KB, 31 redundant vertex bodies are ~2.2 KB
before keys and map entries — a cost paid purely for addressability, since the values are one
logical array that a single vertex could hold as one structured TLV.

**The notation already exists and is arbitrarily restricted.** [RFC-0004](0004-remote-operation-addressing.md) §C
defines `index` / `index_mode` (`SCALAR=0`, `ELEMENT=1`, `WILDCARD=2`) and the conformance
suite pins four spellings of it (`field-scalar`, `field-append`, `field-indexed`,
`field-wildcard`). The mechanism is complete; it is simply unreachable unless a field name
precedes it. The restriction is an artifact of `FIELD` having been introduced as *the `:field`
tail*, not a decision anyone made about arrays.

**Fine-grained notification currently has no expression at all.** A subscriber to a vertex
receives the whole value on every write. The only way to be notified about one element is to
make that element its own vertex — the same cost as above, now paid on the notification axis
too. And the node cannot narrow it on its own: comparing the old and new value to discover
what changed is prohibited (the runtime does not filter delivery by comparing values —
[RFC-0008](0008-vertex-operations-assign-propagate.md), and see `subscriber_t::active`'s
contract in `core/include/libtracer/vertex.hpp`). The information has to come from the
**writer**, and `[n]` is exactly the writer stating it.

## Proposed change

### A. Wire shape — a `FIELD` with zero levels selects the value plane

[RFC-0004](0004-remote-operation-addressing.md) §C defines `FIELD` as one *level* per
field-chain element, each level beginning with a `NAME`:

```
FIELD (0x10, PL=1) {
  level_1 ... level_K            ; K <= 8
}
where each level =
  NAME    field_name
  VALUE   index                 ; optional u32 - the [N] index
  VALUE   index_mode            ; optional u8  - SCALAR=0, ELEMENT=1, WILDCARD=2
```

This RFC admits **K = 0**: a `FIELD` whose children are the `index` / `index_mode` `VALUE`s
with **no leading `NAME`**. Such a selector addresses the **vertex's own value**, and `index`
selects a child TLV of it.

Parsing is unambiguous and needs no sentinel: **levels are delimited by `NAME`, so a `FIELD`
whose first child is not a `NAME` has no level and is a value-plane selector.** A decoder that
already walks levels needs one test, not a new grammar.

`/sensor/queue[3]` — `FIELD{ VALUE u32=3, VALUE u8 index_mode=ELEMENT }`:

```
10400d0001000400030000000100010001
```

`/sensor/queue[]` (append) — `FIELD{ VALUE u8 index_mode=ELEMENT }`, no index `VALUE`:

```
104005000100010001
```

These are the exact duals of the existing `field-indexed` and `field-append` vectors with the
leading `NAME` record removed, and nothing else changed.

`PATH` (`0x06`) remains **untouched** — its "children MUST be `NAME`" invariant, restated by
RFC-0004 §C, is preserved. An element index never appears in `dst` or `src`; it rides the
`FIELD` selector, which `FWD` §B already carries as an optional child. **No new TLV type, no
new frame, no new `opt` bit.**

### B. Semantics — `[n]` is structural, never temporal

`[n]` selects the **n-th child TLV of the stored value**, counting from 0 in encounter order.
It is a selector over bytes the vertex already holds; it introduces no storage concept.

- The value MUST be a **structured** TLV (`opt.PL=1`) for any indexed selector to resolve.
  An indexed selector against an unstructured value answers `TYPE_MISMATCH`.
- The node MUST NOT interpret what a child *means*. Element addressing is type-agnostic:
  a child is a TLV at an offset, and its type code is the application's business
  ([CONTEXT.md](../../../CONTEXT.md) §Application field).
- `[n]` MUST NOT be read as an index into a vertex's `STREAM` history ring
  (`role_t::STREAM`, whose depth is the owner-declared `set_history_depth`). The ring is **not** addressable by this
  RFC and remains drain-only. `[n]` indexes *content*; the ring indexes *time*, and one
  syntax must not mean both.

**Storage semantics stay the vertex's.** Whether a vertex overwrites or keeps history is its
**role**, fixed by the application at registration. `[n]` does not select a storage mode, and a
remote writer MUST NOT be able to change one: element addressing mutates **content**, never
**policy**.

### C. Operations

| Selector | Operation | Behavior |
| ---- | ---- | ---- |
| `/path[n]` | `READ` | The n-th child TLV, served as a view. `NOT_FOUND` if the value is unset; `INVALID_PATH` if `n` is past the last child |
| `/path[]` | `READ` | The children wrapped in a `POINT` (`0x07`), in order — the same rule RFC-0004 §D already gives for reading an array `:field` |
| `/path[n]` | `WRITE`, payload = any TLV | Replaces child `n` |
| `/path[n]` | `WRITE`, payload = empty `STATUS` (`0x09`, no payload, no children) | **Clears** child `n` — the RFC-0009 §D.1 sentinel, unchanged |
| `/path[]` | `WRITE` | **Appends exactly one** child |
| `/path[*]` | `WRITE` | `INVALID_PATH` — the WRITE grammar has no wildcard axis ([#579](https://github.com/avatarsd-llc/libtracer/issues/579)) |

The payload-type discrimination is deliberately **identical** to the one
[RFC-0009](0009-vertex-removal-and-subscriber-eviction.md) §D.1 defines for
`:subscribers[n]` and that shipped in [#611](https://github.com/avatarsd-llc/libtracer/pull/611).
One rule, two planes.

### D. Growth and bounds

**`[]` appends exactly one element; `[n]` never grows anything.**

This is the whole bound, and it is a consequence of what the two operators mean rather than a
guard bolted on:

- Growth is 1:1 with frames. A peer cannot turn a 2-byte index into 65536 slots, because the
  index operator does not allocate.
- The one element `[]` appends is drawn from the receiver's **injected resource**, so
  exhaustion answers `BACKPRESSURE` and the bound is a property of what the node was given —
  never a constant this document invents
  ([RFC-0006](0006-resource-bounded-nesting-depth.md), [ADR-0065](../../adr/0065-failable-allocation-gets-its-own-seam-block-source.md)).
- `[n]` past the last child is not a policy decision; it is the absence of an element.

This also states, retroactively and more simply, why `vertex_t::replace_edge` refuses to grow
`subs_` (#611): not as hardening, but because `[n]` is not the growth operator. The two are the
same rule.

> **Implementation prerequisite (normative for the reference implementation).** The arrays this
> rule bounds MUST actually draw from the injected resource. They do not yet: `subs_` is a plain
> `std::vector<subscriber_t>` on the global allocator (`core/include/libtracer/vertex.hpp`), so a
> bounded node today injects a pool and appends past it regardless. This is the same hole
> [#597](https://github.com/avatarsd-llc/libtracer/issues/597) tracks on the block-source seam,
> and it is a **predecessor** of this RFC's storage half, not an adjacent cleanup.

### E. Delivery mirrors the shape of the write

A delivery is already a `FWD{ op=WRITE }` carrying the same selector a client's write would —
RFC-0004 §D makes that identity load-bearing ("*a subscription delivery and a one-shot command
are the identical wire frame*"). This RFC extends it to elements, which requires no new
mechanism:

- A write to `/path[n]` delivers **that element**, with the same `FIELD{ index=n }` selector.
- A write to `/path` (no selector) delivers the **whole value**, as today.

Three consequences, all of them falling out rather than being chosen:

1. **No comparison anywhere.** The writer named the element, so the node never has to discover
   what changed. This satisfies the no-value-change-detection rule structurally.
2. **The `SUBSCRIBER` TLV is unchanged.** A subscriber subscribes to the *vertex*, exactly as
   today; it carries no index and needs no new field. Granularity is a property of the **event**,
   not of the **edge** — so `subscriber_t` does not grow, and the dispatch path gains no
   per-edge index test.
3. **An element delivery landing past the end of the receiver's array is dropped.** A delivery
   is a one-way write with nobody to answer, and per §D the index operator does not grow. The
   producer is not told; a subsequent whole-value write repairs the receiver.

Element deliveries **retain the self-repair property** for any element that is written more than
once: a reordered element is corrected by the next write to that same element, exactly as a
reordered whole value is corrected by the next whole write. The two differ only for an element
written **once and never again**, on a transport that reorders. A deployment that requires more
already has the concept: `reliability` is bits 0–1 of the subscription's delivery policy ([RFC-0022](0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.A; it was the per-vertex `settings.reliability` knob when this was written).

### F. Conformance vectors (proposed)

| Vector | Shape |
| ---- | ---- |
| `field/value-indexed` | `FIELD{ VALUE u32=3, index_mode=ELEMENT }` — the `/path[3]` selector, byte-pinned |
| `field/value-append` | `FIELD{ index_mode=ELEMENT }`, no index — the `/path[]` selector |
| `op/element-write-replace` | `WRITE` of a `VALUE` to `/path[1]` replaces child 1, leaves 0 and 2 |
| `op/element-write-clear` | `WRITE` of the empty-`STATUS` sentinel to `/path[1]` clears child 1 |
| `op/element-write-oob` | `WRITE` to `/path[9]` of a 3-child value answers `INVALID_PATH`, mutates nothing |
| `op/element-write-unstructured` | indexed `WRITE` against a scalar value answers `TYPE_MISMATCH` |
| `op/element-delivery` | a write to `/path[1]` delivers `FWD{WRITE, FIELD{index=1}, <child>}`, not the whole value |

### G. Non-goals

- **The `STREAM` ring stays unaddressable.** §B forbids the temporal reading.
- **No batching across vertices.** `FWD` carries one `op` and one `dst`; values that should
  travel together belong in one array-valued vertex, which is a modeling decision the
  application makes. This RFC adds no multi-vertex frame.
- **No receiver-side session state.** No cursors, no per-subscriber positions, no per-element
  sequence numbers. See §Alternatives.

## Compatibility

- **Does not break protocol-v1 implementations.** `docs/spec/v1.md` is DRAFT and its §3 is a
  stub; `FWD`/`FIELD` live in RFC-0004, whose own header records "no released v1 yet, so no v2
  needed". No existing encoding changes meaning: a `FIELD` beginning with a `NAME` parses
  exactly as before.
- **A decoder that does not implement this fails safe, verifiably.** The reference decoder's
  level loop already begins each level with
  `if (cur->type() != type_t::NAME) return std::unexpected(status_t::INVALID_PATH);`
  (`core/src/op_resolve_walk.hpp:331`), so a pre-RFC node handed a value-plane selector answers
  **`INVALID_PATH`** — it cannot mistake it for a whole-value write, and it cannot silently
  ignore the index. That is the correct answer for a node that does not offer element
  addressing, and it needs no new code to produce it.
- **New conformance vectors**: §F. Existing FIELD vectors are unchanged.
- **Migration**: none required. Element addressing is additive; an implementation that does not
  offer it answers `SCHEMA_NOT_FOUND` for indexed value selectors, the same `ENOTTY` convention
  an unsupported `:field` already uses.

## Alternatives considered

**`[n]` as a write-mode selector (append vs overwrite chosen by the writer).** Rejected: it
would let a remote peer choose the vertex's *storage semantics*, which belong to the
application that registered it. `role_t::STREAM` already expresses "buffer this" and is set
locally. Element addressing mutates content; it must not select policy.

**`[n]` indexing the `STREAM` history ring (the temporal reading).** Rejected: the ring is one
deque per vertex with a single producer-side flush cursor (`vertex_ext_t::last_flushed_seq`),
so it has no per-consumer position; element deletion on it is unsafe with more than one
subscriber, and giving it one would make the node a broker. A consumer that wants a queue
makes **its own** receiving vertex a `STREAM` — peer symmetry applied to storage, costing
nothing and requiring no protocol.

**An index carried on the `SUBSCRIBER` (subscribe to one element).** Rejected as strictly worse
than carrying it on the delivery: it grows every edge, adds a per-edge index test to the
dispatch path, and expresses less — a whole-array subscriber cannot then receive element-scoped
updates. Putting the index on the *event* gives element granularity with an unchanged
`SUBSCRIBER` and an unchanged `subscriber_t`.

**Per-element sequence numbers in deliveries, with the receiver dropping stale elements.**
Rejected. It was motivated by a claim that does not hold: element deliveries *do* self-repair
for any repeatedly-written element (§E), so the exposure is confined to write-once elements on
a reordering transport. The cost was disproportionate — wire bytes on every delivery plus
per-element last-applied state on the receiver, which would have been the **first receiver-side
session state in the system**. A single per-vertex sequence does not even work: reordered
writes to two *different* elements would make the newer one suppress the older-but-unrelated
one.

**`transport_t::ordered()`, restricting element deliveries to ordered links.** Rejected with
the above, for the same reason: it prices a real but narrow exposure at a new transport
capability plus a silent behavior fork between links.

**Extending `PATH` with an index segment.** Rejected: it breaks the "children MUST be `NAME`"
invariant every existing PATH parser and the byte-keyed vertex map depend on
([reference/02](../../reference/02-graph-model.md) §dispatch keys). The `FIELD` selector already
travels beside `dst` in `FWD` and costs nothing to reuse.

**A reserved empty `NAME` instead of a zero-level `FIELD`.** Rejected: empty segments are
rejected elsewhere in the addressing grammar, so an empty `NAME` would be a special case in
every validator. "No level" needs no sentinel — the absence of a leading `NAME` is already
distinguishable.

## Consequences

**Positive**

- An array of N values costs **one** vertex instead of N — roughly 80 B × (N−1) of vertex
  bodies on a 32-bit target, plus N−1 path keys and map entries.
- Per-element notification with **no** new per-edge cost, no comparison, and no receiver state.
- One `[]` rule spanning `:subscribers`, `:children`, and vertex values — including the
  `[*]`-on-write rejection and the empty-`STATUS` clear sentinel, which stop being
  subscriber-specific quirks.
- Delivered bytes shrink from the whole array to one element for fine-grained updates.

**Negative / risks**

- An element write is a read-modify-write and therefore needs a concurrency contract that
  whole-value last-writer-wins did not: see [ADR-0066](../../adr/0066-element-write-is-a-single-attempt-cas.md).
- A write-once element on a reordering transport can stay wrong until it is written again
  (§E). Bounded and stated, not eliminated.
- §D's bound is only real once the arrays draw from the injected resource — a prerequisite,
  tracked as #597.

## Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment
window", this is an **amendment** — it changes the normative surface — and the 14-day window is
waived by default while the project is solo-maintained. Record sustained objections and their
resolution here.

## Relates

- **Amends** [RFC-0004](0004-remote-operation-addressing.md) §C (the `FIELD` grammar) and §D
  (operation semantics).
- **Generalizes** [RFC-0009](0009-vertex-removal-and-subscriber-eviction.md) §D.1 — the
  payload-discriminating indexed write, shipped for `:subscribers[n]` in #611.
- **Upholds** [RFC-0008](0008-vertex-operations-assign-propagate.md) (delivery never compares
  values) and [RFC-0006](0006-resource-bounded-nesting-depth.md) (bounds are injected
  resources).
- **Depends on** [#597](https://github.com/avatarsd-llc/libtracer/issues/597) for §D's bound.
- **Implementation contract**: [ADR-0066](../../adr/0066-element-write-is-a-single-attempt-cas.md).
