<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0024 — Bound paths: node-scoped vertex-ref source routing

| Field | Value |
| ---- | ---- |
| **RFC** | 0024 |
| **Title** | Bound paths: node-scoped vertex-ref source routing |
| **Status** | **draft** (2026-08-02) |
| **Author(s)** | AvatarSD (maintainer) — written up from the 2026-08-02 grill, in which the design below was **ruled**, not proposed |
| **Created** | 2026-08-02 |
| **Comment window** | waived by default while solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"); invoke explicitly if outside input is wanted. Verified: `docs/implementations.md:13` still reads `_(none yet)_`, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** GOVERNANCE.md:53-54 — an erratum "may not alter the wire surface"; this adds a type code, a new frame shape and a new MUST for every forwarder, which GOVERNANCE.md:54 names as amendment territory by example ("a new frame shape"). |
| **Tracking issue** | [#809](https://github.com/avatarsd-llc/libtracer/issues/809) |
| **Target spec version** | v1 itself. `docs/spec/v1.md:1` reads "(DRAFT)" and `:3` reads "The wire format is not yet stable. Pin to a specific commit if you depend on this." Same route RFC-0006, RFC-0018, RFC-0019 and RFC-0023 took. |
| **Scope** | **v-NEXT. This RFC explicitly does not gate v0.7.0.** |
| **Descends from** | [#504](https://github.com/avatarsd-llc/libtracer/issues/504) (client-originated binding — closed on the bench-gated ruling), [#788](https://github.com/avatarsd-llc/libtracer/issues/788) (the five questions, made normative here in §8) |

> **Numbering note.** 0012 was closed unmerged, 0015 was withdrawn (PR #446); neither gap is
> reusable — see [RFC-0016](0016-composed-branch-read.md) §ghost history and RFC-0023's numbering
> note. 0014 is a real document. 0024 is the next unused number.

---

## 1. Summary

There is exactly one way to address a remote vertex today: spell the whole route as names, from
the caller's own root, and make every hop re-resolve its own prefix out of those names
(§Path-as-route, `CONTEXT.md`). It is the right *canonical* form — self-describing, injective,
resolvable by a peer that has never spoken to you — and it is a poor *repeat* form, because the
second and thousandth operation to the same vertex re-transmit and re-resolve an answer both ends
already computed.

This RFC adds a **second, optional normative path form** and changes nothing about the first.

- **Canonical `PATH` (`0x06`, NAME children) is UNTOUCHED.** It stays the only form a cold peer
  can use, and it stays the **mint key** — the bytes a bound path is derived from, and the bytes
  a failed bound path falls back to. Its canonical-bytes property is what makes that key work
  (`core/src/op_resolve_walk.hpp:660-669`: `path_lookup_key` returns `path.body()` unchanged when
  the PATH is canonical — the frame IS the key, ADR-0041 §3).
- **`PATH_REF` (`0x14`)** is the new form: a TLV whose body is a bare array of **fixed 8-byte
  elements**, **one per host on the route**. Each element is *that host's own* reference to its
  **next-hop connection vertex**; the last element is the **terminus vertex itself**. Nothing in
  an element means anything anywhere but on the host that minted it — a bound path is a stack of
  node-scoped references, not a global name.
- An element is `(u32 vertex-map index, u32 generation)`, little-endian. Both widths are
  **derived** in §4.4, in the discipline [RFC-0023](0023-path-segment-cap-repriced-32-to-255.md)
  set: *from the wire's own widths and the RAM record, not chosen.*
- Validation is **existing machinery**: a bounds check into the pinned, pointer-stable,
  insert-only vertex map (`core/include/libtracer/graph.hpp:56`) plus a compare against the
  validate-on-use stamps that already exist (`core/include/libtracer/graph.hpp:275`,
  `core/include/libtracer/child_registry.hpp:266-268`). A failed validation **never mis-routes**:
  drop, NACK with the failing hop index, and the origin re-resolves canonically and re-mints.
- Binding is minted **in-band, in the reply of an ordinary canonical op**, with **zero added
  request bytes** (§7.5). No handshake, no registry, no per-hop table.

The whole thing is one sentence: **the canonical path says who; the bound path says which of the
things you already resolved.**

## 2. What this is NOT

Stated first, because every reviewer's first two questions are "isn't this §E.1?" and "isn't this
the thing #504 closed?"

### 2.1 It is not RFC-0004 §E.1's route-handle, and does not replace it

[RFC-0004](0004-remote-operation-addressing.md) §E.1 (RFC-0004:178-188) is **delivery
compaction**: a per-link `u16` **label** that aliases an established delivery route, swapped at
every hop MPLS-style, advertised in-band, self-healed by `HANDLE_NACK` → re-advertise. It is
implemented (`core/include/libtracer/route_handle.hpp`, `0x11`–`0x13` at
`docs/reference/05-protocol-tlvs.md:952-971`) and **it stays exactly as it is**.

| | **§E.1 label** (stays) | **bound path** (this RFC) |
| --- | --- | --- |
| what it names | a per-**link** route alias | a per-**host** vertex reference, stacked |
| state model | every hop holds `label → (down link, out label)` | **no hop holds anything** |
| who pays | each forwarder, per compact flow | the origin (one path object) and the terminus edge |
| wire cost | **10 B flat** in hops (`COMPACT` hdr 4 + `VALUE` label 4+2) | **4 + 8×H** (H = hosts) |
| direction | producer → consumer (delivery) | origin → target (any op) |
| origination | producer advertises | origin requests, hops answer in the reply |
| teardown | link-scoped: `clear_link` drops the table (`route_handle.hpp:286-287`) | nothing to tear down; a stale element fails validation and re-mints |
| exhaustion | label space saturates per link (`core/src/route_handle.cpp:175-180`) | none — no allocation, no space to exhaust |

On raw bytes for a long-lived stream §E.1 **wins and keeps winning** (10 B beats 28 B at H=3).
This RFC does not compete for that traffic. It serves the traffic §E.1 structurally cannot: a
**client's repeated request** to a `dst` (not a producer's delivery), across hops that must stay
**stateless** (a forwarder holding no per-flow table is the property RFC-0004 §E.1 itself calls
"the cost, made precise"), and one-shot-shaped ops that never justify an advertise round.

**Vocabulary rule (normative for the doc set).** "**Label**" remains RFC-0004 §E.1's per-link
`u16` and nothing else. The new concept is a "**bound path**", built of "**vertex ref (vref)**"
elements. A sentence that calls a vref a label is wrong (§11).

### 2.2 It is not the resolve-once memo #504 closed

[#504](https://github.com/avatarsd-llc/libtracer/issues/504) closed on a **measurement**, and the
measurement is binding on this RFC: the per-delivery `by_name` scan `deliver_remote` pays is
**9–12 ns/delivery, and only at W≈32 registries** — "at W≈4 the arms overlap; nothing there" —
and the one-slot memo that recovered exactly that delta **regressed the existing four-link
`reply-spread` shape by 140–311 ns/write**, which is a latency regression on a shipped shape,
which is a reject.

**So the local-terminus nanosecond win is NOT the motivation of this RFC**, and no version of
this document may claim it. The prize is §3: **wire bytes on constrained links, the per-hop mount
descent on multi-hop routes, and MCU cycles.** The 9–12 ns figure appears here once, as the thing
that is *not* being sold. #504's rejection also supplies this RFC's acceptance bar directly (§8):
`reply-spread` is a mandatory must-not-regress arm.

## 3. Motivation

### 3.1 The canonical form re-pays a resolution both ends already made

A remote address is the full path from the caller's root, walking *through* transport vertices
(`CONTEXT.md` §Path-as-route). Each hop strips its **whole mount run** — `net/<module>/<name>`,
RFC-0014 S2a / [ADR-0061](../../adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)'s
strip-K descent — and forwards the residual. Concretely, per hop, the forwarder runs
`resolve_mount_at` (`core/src/fwd_router.cpp:331`), which folds a digest chain over the leading
segments and compares candidate mount slots, falling back to `resolve_mount_deep`
(`core/src/fwd_router.cpp:270`) when the address is longer than the in-place window or split by a
rope; both sit on `resolve_mount_by` (`core/src/fwd_router.cpp:200`). Every hop of every operation
re-derives the same split from the same bytes.

That is correct and it is the right default. It is also, for the Nth identical op, an answer
recomputed from scratch on both sides of every link.

### 3.2 The bytes, derived

Cost rule: a canonical `PATH` body is concatenated NAME TLVs, each costing `4 + len`
(`docs/reference/05-protocol-tlvs.md:377`), plus the 4-byte PATH header. Mount runs are three
segments at today's width (ADR-0061). Using the same mount-run shapes RFC-0023 priced
(RFC-0023:185-188): `net/ws-client/board-01` = 32 B of body, `net/can/c0` = 20 B, a
`/sensor/temp` residual = 18 B.

| route | canonical `dst` (hdr + body) | hosts H | `PATH_REF` (4 + 8H) | saved |
| --- | ---: | ---: | ---: | ---: |
| direct link, `…/board-01/sensor/temp` | 4 + 50 = **54 B** | 2 | **20 B** | 34 B (−63 %) |
| one forwarder, `…/board-01/net/can/c0/sensor/temp` | 4 + 70 = **74 B** | 3 | **28 B** | 46 B (−62 %) |
| two forwarders | 4 + 90 = **94 B** | 4 | **36 B** | 58 B (−62 %) |
| RFC-0018's worked 5-segment `dst` (RFC-0018:32) | **49 B** | 2 | **20 B** | 29 B (−59 %) |

The shape to notice: **canonical grows with segments, bound grows with hosts**, and a host costs
one mount run (≥ 3 segments ≥ 15 B today) but exactly 8 B bound. The ratio is roughly constant at
~2.6× because the mount run is the dominant term at every hop.

The stream figure, honestly. RFC-0004:180 prices a 1 kHz 4-byte sensor over a 3-hop path at
"**~60 B of route on a 4 B payload (~16×)**". Bound: 36 B on 4 B = **9×**. §E.1's label: 10 B on
4 B = **2.5×**. So on that specific traffic §E.1 remains the answer and this RFC is second-best —
which is exactly why §2.1 keeps it.

### 3.3 Constrained links: the frame arithmetic

A classic CAN frame carries 8 payload bytes (`docs/reference/14-can-transport.md:142`), so an
address costs `⌈bytes / 8⌉` frames before any payload:

| route | canonical frames | bound frames |
| --- | ---: | ---: |
| direct link (54 B / 20 B) | 7 | **3** |
| one forwarder (74 B / 28 B) | 10 | **4** |
| two forwarders (94 B / 36 B) | 12 | **5** |

**Honest caveat, load-bearing:** CAN itself is *not* the beneficiary. `14-can-transport.md:274`
records the rule — "CAN always labels (no route fits in 8 bytes)" — so the CAN plane is already
served by the id↔path map and §E.1. The 8-byte frame is used here as the *unit of a constrained
link*: the beneficiaries are full-TLV links with small MTUs and **no label plane** (LoRa,
BLE-GATT, framed serial), and any link where the origin is an MCU counting bytes per op.

### 3.4 The per-hop work

Under a bound path a forwarder does not descend at all: read element *i*, bounds-check the index,
load the vertex pointer, compare one `u32`, egress. No digest fold, no segment compare, no
variable-length walk — the `resolve_mount_*` family (§3.1) is not entered.

**This is UNMEASURED** and is labelled as such per the standing rule (§10). It is a structural
argument about which code runs, not a number; §8 makes measuring it a condition of acceptance.

## 4. The wire form

### 4.1 `PATH_REF` — type `0x14`

`0x14` is the first unassigned code in the fast-track range: `0x0F`–`0x13` are assigned and
`0x14`–`0x1F` are free (`docs/reference/05-protocol-tlvs.md:901`). A receiver that does not
implement this RFC treats it per the forward-compatibility rules that section names, i.e. it does
not crash — but it also cannot route, so §9.3 states the fallback obligation.

```
PATH_REF (0x14, PL=0, LL=0) {
  ; body: H bare 8-byte elements, in route order, no per-element framing
  ; element i, little-endian:
  ;   u32 index       — the minting host's vertex-map index
  ;   u32 generation  — that vertex's retirement generation at mint time
}
```

- Element **0** is the **origin's own** reference to the connection vertex for its first hop.
- Element **i** (0 < i < H−1) is **forwarder i's** reference to its connection vertex for hop
  i+1.
- Element **H−1** is the **terminus host's** reference to the **target vertex itself**.
- Each forwarder consumes element 0 and forwards the remainder — the same monotone shrink the
  canonical `dst` performs (RFC-0004 §B), so a bound path is **loop-free by construction** for
  the identical reason and needs no visited set.

### 4.2 The byte ledger — every byte, accounted

The maintainer directive for this RFC: *account for every byte a `PATH_REF`-addressed operation
puts on the wire; any byte that cannot be justified from a measured bound or an existing wire rule
is removed from the format, not defended.* The format below is what survived that pass.

**Envelope — 4 bytes, and there is no fifth.**

| off | bytes | field | why exactly this |
| --- | --- | --- | --- |
| 0 | 1 | `type = 0x14` | one byte is the TLV grammar's own type width (`01-data-format.md` §header); `0x14` is the first free fast-track code (`reference/05:901`) |
| 1 | 1 | `opt = 0x00` | the grammar's own options byte. **PL = 0** and **LL = 0**, derived below |
| 2 | 2 | `length` (u16 LE) | the default length width (`01-data-format.md:17`, `:73` — "0 = u16 … DEFAULT"). Max body is 2040 B (§4.3) — three orders inside u16 |
| 4 | 8×H | element array | §4.4 |

- **`opt.PL` MUST be 0.** `PL = 1` asserts "the payload is concatenated child TLVs"
  (`reference/05:22`), and it is not: it is a fixed-stride record array. A generic `PL = 1` walker
  would read the first four body bytes as a TLV header — `type` = the low byte of an index,
  `opt` = the next — and mis-frame the whole body. This is the same flip
  [RFC-0018](0018-packed-path-segments.md) makes to `PATH` for the same reason (RFC-0018:36).
- **`opt.LL` MUST be 0**, and `LL = 1` is therefore never applicable to a `PATH_REF`. `LL = 1`
  buys a u32 length for bodies above 65 535 B (`01-data-format.md:102`); the normative element
  cap (§4.3) puts the maximum body at **2040 B**. There is no reachable `PATH_REF` for which
  `LL = 1` is anything but two wasted bytes, so it is forbidden rather than merely unused.
- **`opt.TS`/`opt.CR` are the enclosing frame's business**, not this TLV's. A `PATH_REF` nested
  in a `FWD` carries no trailer of its own, exactly as `PATH` does not today.
- **Alignment.** With `LL = 0` the payload starts at offset 4 (`01-data-format.md:36` — "the
  default `LL=0` case keeps payload at offset 4, naturally aligned for u32 access"), so every
  element field is u32-aligned whenever the TLV is. Receivers must still tolerate unaligned reads
  per that same line; the layout simply does not force one.

**Rejected: per-element TLV framing.** Wrapping each element as its own `VALUE` TLV costs
`4 + 8 = 12 B` per element:

| H | bare array (4 + 8H) | per-element TLV (4 + 12H) | overhead |
| ---: | ---: | ---: | ---: |
| 2 | 20 B | 28 B | +40 % |
| 3 | 28 B | 40 B | +43 % |
| 4 | 36 B | 52 B | +44 % |

A per-child header exists to **delimit** a variable-length child — which is precisely why `NAME`
children carry one inside `PATH`. Elements are fixed-width: element *i* is `body[8i .. 8i+8)`,
computed, not parsed. A header that delimits a record whose length is a constant is 4 bytes
buying nothing, so it is not in the format.

### 4.3 The element-count bound, derived (not chosen)

Two bounds, and the tighter one is not the normative one:

1. **Reachable.** A bound path is always minted from a canonical route (§6), so its host count is
   bounded by what a canonical `dst` can spell. The canonical body cap is 1024 B
   (`reference/05:299-300`) and a hop costs at least one 3-segment mount run — `3 × (4+1) = 15 B`
   today, `3 × (1+1) = 6 B` under RFC-0018's packing. So **H ≤ 69 today** (68 runs + the
   terminus element) and **H ≤ 171** under packing.
2. **Normative.** `PATH_REF` element count **MUST be ≤ 255**, body ≤ **2040 B**. This is
   RFC-0023's discipline applied unchanged: 255 is the largest count for which every per-element
   quantity — the count, the largest index (254), a receiver's per-element table dimension —
   fits `u8`, and it sits *above* both reachable ceilings, so it is an encoding-independent
   ceiling rather than an artifact of whichever body grammar `PATH` currently uses.

The wire carries **no element-count field**: the count is `length / 8`, and a `length` not
divisible by 8 is `tr::frame::invalid`. A count field would be a byte (or two) restating what the
length already says — removed under the directive, not defended.

### 4.4 The element — 8 bytes, and why each 4 is exactly 4

#### The index: `u32`, derived from the RAM floor

The index addresses the pinned, insert-only vertex map. Its width is bounded by **how many
vertices can physically exist**, so the derivation is a RAM floor:

- The smallest vertex costs `sizeof(vertex_t)`, gated at **80 B on rv32**
  (`core/include/libtracer/config.hpp:165`) and **120 B on a 64-bit host**
  (`core/include/libtracer/config.hpp:153`), enforced at compile time
  (`core/include/libtracer/vertex.hpp:2452`, `:2455`).
  [ADR-0070](../../adr/0070-configuration-is-a-named-traits-type.md):40 records that rv32 sits at
  *exactly* 80 with zero headroom — so 80 is a floor, not a budget.
- Add the index slot itself: one pointer, 4 B on rv32, 8 B on a host (§6.4).
- Ignore, deliberately, the parent's child-container slot and the key bytes — every one of them
  makes the floor higher, so leaving them out only strengthens the bound.

| target | floor B/vertex | vertices in the largest plausible RAM | as a power of 2 |
| --- | ---: | --- | --- |
| rv32 | ≥ 84 | 4 GiB address space ⇒ ≤ **51.1 M** | 2^25.6 |
| 64-bit host | ≥ 128 | 2^32 vertices would need **549 GB** | — |

So **u32 is unreachable on both targets**: on rv32 by ~84× against the entire address space, on a
host by needing more RAM than a single node has.

**Why the next smaller honest width fails.** `u24` (16 777 216) × 128 B = **2.1 GB** — reachable
on commodity hardware *today*, so it is a real ceiling rather than a formality; and 24 bits is not
a machine width, costing a mask and a shift on both sides of every deref for the privilege. `u16`
(65 536) × 80 B = 5.2 MB — reachable on an MCU, let alone a host, so it fails outright. **u32 is
the smallest natural width that cannot be exhausted**, which is the definition of derived.

#### The generation: `u32`, and it MUST NOT be narrower

The generation is the **anti-mis-route guard** — the only thing standing between a stale reference
and a delivery into whatever now occupies that slot. Its width is not a space decision.

1. **It is the width of the stamp that already exists.** `graph_t::retire_generation` returns
   `std::uint32_t` (`core/include/libtracer/graph.hpp:275`); `child_registry_t::mount_generation`
   likewise (`core/include/libtracer/child_registry.hpp:285`). A narrower wire field would be a
   **truncating** conversion of a live counter — aliasing every 2^width retires by construction,
   with no code anywhere doing anything wrong.
2. **Wrap arithmetic.** A `u16` generation wraps after **65 536** retires. A dynamic controller
   graph retiring one vertex per second wraps in **18.2 hours**; at 10/s, 1.8 hours. A `u32`
   reaches 4.29 × 10⁹: **136 years at 1 retire/s**, but only **49.7 days at 1000/s** — so width
   alone is not the safety argument, which is why rule 3 exists.
3. **Therefore, normatively: the generation MUST saturate, never wrap.** This is
   [#603](https://github.com/avatarsd-llc/libtracer/issues/603)'s ruling transposed. The
   `route_handle` `u16` label allocator wrapped, and `core/src/route_handle.cpp:175-177` records
   what that cost: *"a wrapped `next_label` handed out the reserved 0 and then re-issued 1, 2, …
   while those labels still aliased LIVE routes — a delivery on the reused label resolves the
   wrong route, which is a misroute, not a drop."* A wrapped **generation** is that same failure
   with the guard instead of the address: a stale vref **validates falsely**, and the operation
   lands on the vertex's successor. On saturation a vertex becomes permanently unbindable and
   every mint for it falls back to canonical — the identical degrade the label allocator already
   takes at exhaustion (`route_handle.cpp:175-187`: return 0, record nothing, send the full route,
   "which always works"). With saturation, the wrap failure class is closed **by construction**,
   and 2^32 is then a statement about *how long before a hot vertex stops being bindable*, not
   about safety.

#### Byte order

Little-endian, both fields. This is not a choice: `docs/reference/01-data-format.md:35` —
"little-endian for every multi-byte field."

### 4.5 Rejected element shapes

- **`u48` index + `u16` generation** (same 8 bytes, "spend the width where the cardinality is").
  Rejected on both halves. The index gains nothing — u32 is already unreachable by 84× on rv32
  and by 549 GB on a host (§4.4), so the extra 16 bits buy headroom above a ceiling that cannot be
  approached, and `u48` is not a machine width. The generation **loses the property the element
  exists for**: at 65 536 it wraps in under a day of ordinary churn, a wrapped stale ref validates
  falsely, and the result is a **wrong-route delivery** — the exact failure class #603 fixed by
  saturating the label allocator. Trading the guard's width for headroom above an unreachable
  ceiling is the trade that produced #603 in the first place.
- **A raw pointer on the wire.** Rejected on unforgeable-validation grounds: **there is no bounds
  check a receiver can apply to a peer-supplied address.** Any 32/64-bit value is a
  syntactically-valid pointer, so the guard degenerates to "dereference and hope" — a crash or a
  forge primitive handed to whoever can put bytes on a link. An index is checkable against a
  cardinality the receiver knows. The reference implementation already refuses this shape
  internally, for the weaker in-process case: `vertex_handle_t` "exposes no `operator*` or
  raw-pointer accessor" (`core/include/libtracer/graph.hpp:54-55`); putting on the wire what is
  not exposed to a local caller is not arguable. It is also 8 B on a host where 4 suffices.
- **A globally unique vertex id (UUID / node-key + local id).** Rejected as a *different design*:
  it would need a mint authority, a resolution table at every hop, and a global namespace — three
  structures this RFC exists to avoid. The node-scoped element needs none, because it is only ever
  read by the node that wrote it.

## 5. Validation — existing machinery, no new registries

### 5.1 The check

On receipt of a `PATH_REF`-addressed op, a host reads element 0 and:

1. **Bounds-check** `index` against its vertex-map cardinality. The map is *pinned,
   pointer-stable, insert-only* — `core/include/libtracer/graph.hpp:56`, and ADR-0057's
   never-freed rule (`graph.hpp:109`) — so an in-range index always names a live allocation and
   the deref itself cannot fault. Out of range ⇒ **reject** (§5.3).
2. **Compare the generation** against `graph_t::retire_generation(vh)`
   (`core/include/libtracer/graph.hpp:275`). Mismatch ⇒ **reject**. That doc comment already
   states the contract this RFC leans on verbatim: a holder "records this alongside it and
   re-reads it before use: a mismatch means the path was retired (and possibly re-created for a
   DIFFERENT owner) since the resolution, so the cached answer must be discarded rather than
   delivered into whatever now occupies that path."
3. **Authorize** — §6. The same comment is equally load-bearing in the negative:
   "Callers must NOT cache an authorization decision this way — a generation match says the vertex
   is the same one, never that the caller may still act on it (ACL stays per-operation)."
4. Egress (or, at the terminus, apply the op).

Generations only ever move **forward** (`retire_generation` is bumped under retirement's own
ordering, `graph.hpp:271`), so a stale element can only ever compare *lower*. It never becomes
valid again by waiting.

### 5.2 The three stamps, and which one the wire carries

`core/include/libtracer/child_registry.hpp:266-268` states the set: the mount-shape generation is
"the third validate-on-use stamp, beside `graph_t::retire_generation` (a revived vertex) and the
slot tombstone (a departed link)". All three remain in force under this RFC; the division is:

| stamp | catches | where it lives under a bound path |
| --- | --- | --- |
| `retire_generation` (`graph.hpp:275`) | a retired-and-revived vertex | **on the wire** — the element's second u32 |
| slot tombstone | a departed link | **node-local** — the egress slot the deref'd connection vertex resolves to is checked as it is used today; zero wire bytes |
| `mount_generation` (`child_registry.hpp:285`) | the *split point* moving — a `dst` prefix starting or stopping resolving to a different mount (#765) | **node-local, and structurally not applicable to the deref**: a bound element names a connection vertex *directly*. There is no cached prefix split to go stale because there is no prefix. The stamp still guards the **mint** (§6.2) and every canonical-form op, unchanged |

That third row is the honest consequence of the design, not an exemption claimed for it: the
hazard `mount_generation` exists for — "bind a label through mount `net/ws/s`, then register
`net/ws/s/rack`, and a full `FWD` resolves against the NEW, deeper mount while a `COMPACT` riding
the old label still dereferences the binding made against the old split"
(`child_registry.hpp:268-270`) — requires a **cached split**. A vref caches a vertex, and a vertex
is not a split.

### 5.3 Failure is a drop, never a mis-route

**Normative.** A host that cannot validate element 0 MUST NOT forward, MUST NOT apply the
operation, and MUST NOT attempt any repair of its own (no re-resolution, no nearest-match, no
retry against a different vertex). It drops the frame and returns a NACK.

The NACK carries the **index of the failing hop** — the position of the element that failed,
counted from the origin's element 0, so the origin knows *where* the route broke rather than only
*that* it broke. Two spellings are open and the choice is deferred to implementation review
(§9.2): extend `HANDLE_NACK` (`0x13`) with an optional second child, or take a sibling code from
`0x15`–`0x1F`. `HANDLE_NACK`'s body is a bare `VALUE label(u16)` today
(`core/include/libtracer/route_handle.hpp:427-429`; `docs/reference/05-protocol-tlvs.md:966-969`)
and the peek path already tolerates a missing second child
(`core/tests/fwd_frame_view_test.cpp:215-217` — "bare-label `HANDLE_NACK` has no child[1]"), so
the extension is additive; the argument against it is that reusing a route-handle control frame
for a non-label concept violates §2.1's vocabulary rule.

The origin's recovery is the one that already exists and is already known to work: **fall back to
the canonical form and re-mint.** RFC-0004 §E.1's self-heal in one line — drop, signal,
re-establish — reached here without a re-advertise round, because the canonical path the origin
still holds *is* the fallback (§1: canonical stays the mint key). This is also why §9.3 can make
canonical support mandatory: every bound path has a canonical original by construction.

## 6. ACL — all existing machinery, stated as conformance requirements

No new access-control concept appears in this RFC. What follows is the existing machinery restated
as requirements, because a route form that skipped a gate would be a capability, and a vref is
**an address, never a capability** (#504's own framing).

### 6.1 Mint is gated by the full existing check

**Normative.** A host MUST NOT mint a vref for a vertex the requesting caller could not have
reached canonically in the same operation. Since a mint rides an ordinary canonical op (§7), this
is automatic: the op already ran `acl_allows` at every gate on its way through
(`core/src/graph.cpp:655`), and a denial is `PERMISSION_DENIED` before any vref is produced.

**The anti-enumeration property, stated:** because denial happens at resolve time, **no vref is
ever minted for a destination an ancestor ACL hides**. Probing a bound-path mint therefore yields
exactly what probing the canonical form yields — *exists + denied*, never *exists + here is a
handle to it*. A bound path cannot be used to discover a namespace its holder cannot already walk.

### 6.2 The hot path evaluates the same predicate at the deref'd vertex

**Normative.** Every operation arriving on a bound path MUST evaluate `acl_allows` at the
dereferenced vertex, for the operation's own right, exactly as the canonical form does. A
generation match authorizes nothing (`graph.hpp:271-273`).

The evaluation is the shipped one: `graph_t::acl_allows` (`core/src/graph.cpp:655`) walks to the
nearest **bearing** ancestor lock-free and evaluates that vertex's **cached effective-ACE merge**
through the `kAceInherit` projection —
[ADR-0050](../../adr/0050-acl-pure-policy-cached-effective-ace-merge.md), one pre-merged list, no
per-operation ancestor rebuild (`graph.cpp:673-682`).

**Revocation is immediate; there is no snapshot to go stale.** An `:acl` write marks the subtree
dirty (`graph_t::mark_subtree_acl_dirty`, `core/src/graph.cpp:705`) and the next check rebuilds.
A bound path holds no ACL state of any kind, so a revoked right takes effect on the very next
operation over an already-minted binding — the property `graph.hpp:271-273` demands and the reason
this RFC stores nothing authorization-shaped.

### 6.3 Equivalence by construction — and a vector pair anyway

Path-form operations check `acl_allows(target)` and nothing else: `graph_t::read`
(`core/src/graph.cpp:713`), `graph_t::write_impl` (`core/src/graph.cpp:949`). A bound-form
operation dereferences to the same `vertex_t*` and calls the same function with the same right
and the same caller context, so the outcomes are **identical by construction** — there is no
second policy to keep in sync, which is the property that makes this section short.

**A by-construction argument is not a test.** Conformance MUST carry a **paired vector set** —
canonical and bound spellings of the same operation against the same graph — for **allow** and
**deny**, asserting **byte-identical** outcomes (`RESULT` bytes in the allow case;
`ERROR{tr::access::denied}` `0x0050`, `docs/reference/05-protocol-tlvs.md:561`, in the deny case).
Named in §9.4. The reason to require it despite the argument is on the record: RFC-0014's lesson —
two silent misroutes shipped because no test used the production wiring.

### 6.4 The one new structure, named honestly

This RFC adds **no wire registry and no per-hop route table**. It does require one node-local
structure that does not exist today, and the RFC would be dishonest not to name it:
`graph.hpp:56`'s "vertex map" is *described* as a map but *stored* as the ADR-0057 composite
vertex tree (`core/include/libtracer/graph.hpp:1074-1085`) — a tree of non-moving `unique_ptr`
allocations with no dense index. A **dense, append-only `vector<vertex_t*>`**, one slot appended
per registration, is therefore required to give an index meaning.

Its cost and its properties:

- **4 B/vertex on rv32, 8 B on a host** — the figure already used in §4.4's floor.
- It is **append-only**, which the registration path already is ("vertices are added, never
  erased", `graph.hpp:1077`), so it introduces no new lifetime rule and no new invalidation event.
- It is **node-local** and unobservable on the wire; a peer never learns another node's
  cardinality.
- It is **not** a route table: its size tracks the graph, not the traffic, so it does not
  reintroduce the per-flow state §2.1 credits this design with avoiding.

The **binding budget** referenced in §7.2 is likewise an **injected** bound, never a magic number
— the shape `route_handle_table_t` already uses (`core/include/libtracer/route_handle.hpp:163`:
"unbounded — the default … A bounded host sizes it from its" own resources; enforced at
`core/src/route_handle.cpp:184-187`, which refuses a *new* flow and never an established one).

## 7. Minting a bound path

Three activation modes are ruled in, and they compose rather than compete.

### 7.1 (b) via (c): request in the op, answer in the reply

The primary path, and the only one that needs wire support.

1. The origin issues an ordinary **canonical-form** op and sets a **bind-request flag** (§7.5).
2. Each forwarding hop, as it forwards, notes the connection vertex it selected. On the way
   **back**, each hop **appends its vref for the next hop** to a `PATH_REF` accumulating on the
   reply — the mirror of the way `src` accumulates on the way in (RFC-0004 §B, "prepends to
   `src`"), and equally a rope operation rather than a rewrite.
3. The terminus appends its own reference to the **target vertex** as the last element.
4. The origin receives the complete forward vref list and stores it in its path object.

**Symmetry, ruled in.** In the same round trip, each hop MAY also append its **reverse-direction**
vref, so the responder learns the return list too. One round trip binds both directions. This is
what makes the delivery direction (§7.4) free rather than a second exchange.

### 7.2 (a) implicit, heuristic-free

A hop **MAY** offer a vref whenever *both* hold:

- it is forwarding a canonical `dst` for which it **already holds the resolution**, and
- its binding budget (**injected**, §6.4) has room.

**And nothing else.** No use counters, no hit thresholds, no hotness estimate, no timers. The
standing rule applies (`CONTEXT.md` §Resource bound; the no-synthetic-limits doctrine): a bound
comes from an injected resource. The condition above is a fact the hop is already holding, not a
prediction — which is the whole reason it is admissible.

### 7.3 (c) the transport carries it

No new exchange, no handshake, no setup frame. Binding is a **rider on work that was happening
anyway**, which is what keeps a cold or one-shot op at exactly its present cost when the flag is
not set.

### 7.4 Where a binding lives

**Origin side — the user-held path object.** `path_t` stays a **value type**; the binding is an
**opaque slot** on it that the graph/net tiers fill and validate. The layering rule holds
(dependencies point up the layers, `CLAUDE.md`): L1/L2 never learns what an L4 vertex index means;
it carries bytes it cannot interpret. The reuse candidate for the slot's shape is
`resolved_binding_t` (`core/include/libtracer/route_handle.hpp:71-86`) — an allocation-free view
carrying exactly `{target, target_gen, mount_gen}` plus its validity flags, already the
"resolution + its staleness signal" shape this needs, and already deliberately expressing "no
resolution yet" *outside* the handle (`route_handle.hpp:77-79`) rather than as an invalid handle.

**Delivery side — the subscription edge.** [RFC-0021](0021-wire-subscriber-target-frame-of-reference.md)
is **accepted** (maintainer ruling 2026-08-01, RFC-0021:12) in favour of **(b), the producer's
frame** — a `SUBSCRIBER`'s `PATH` child is the delivery target resolved from the producer's own
root — with **implementation deferred past v0.7.0**. That deferred implementation needs somewhere
to keep the producer-frame resolution it computes. **This RFC provides that home**: the edge holds
a bound path, minted at subscribe time from the very resolution RFC-0021 §4.B performs, and
re-minted on the ordinary validation failure. The two RFCs are complementary and neither blocks
the other; RFC-0021 defines *what is resolved*, this one defines *what the answer is stored as*.

### 7.5 The mint exchange — every byte

**Request: zero added bytes.** The bind request is a flag in the **existing `op` byte**, not a new
child and not a TLV option bit.

- It cannot be a TLV `opt` bit: all six defined bits are assigned (`opt_t`,
  `core/include/libtracer/tlv.hpp:59-65`) and bits 7 and 0 are reserved-MUST-be-zero, with a set
  reserved bit making the frame `tr::frame::invalid`
  (`core/include/libtracer/tlv.hpp:66-72`). There is no free option bit and this RFC does not take
  a reserved one.
- `FWD`'s first child is `VALUE op`, a `u8` carrying `READ=0, WRITE=1, AWAIT=2, REPLY=3`
  (RFC-0004 §B; `core/include/libtracer/op_resolve.hpp:42-43`) — **two bits of four in use, six
  free.** The flag takes **bit 7**, and this RFC adds the masking rule that makes that safe:
  **the opcode is `op & 0x3F`; bits 7–6 are flags.**
- **Cost: 0 bytes.** The alternative — a dedicated presence child — costs a 4-byte TLV header
  (plus a body byte if it carries anything) to express one bit, so it is not in the format.
- **Compatibility is not free, and is stated rather than hidden.** Today `peek_fwd_op` casts the
  raw byte straight to `fwd_op_t` (`core/include/libtracer/fwd_frame_view.hpp:409-414`), so a
  pre-amendment peer sees an unknown opcode and rejects — a clean error, not a mis-execution, but
  an error. §9.3 makes the masking rule normative for every forwarder; until it is deployed, a
  bind request to an unknown peer costs one failed op.

**Reply: `4 + 8H` per direction.**

| direction | added bytes | when |
| --- | ---: | --- |
| forward list (origin learns the route out) | 4 + 8H | whenever a mint completes |
| reverse list (responder learns the route back, §7.1) | 4 + 8H | only when the symmetry option is exercised |

**Break-even, computed.** Per-op saving once bound is `canonical_dst − (4 + 8H)` (§3.2), paid
once against a mint cost of `4 + 8H` per direction:

| route | H | saving/op | mint (fwd only) | break-even | mint (both dirs) | break-even |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| direct link | 2 | 34 B | 20 B | **1st op** | 40 B | **2nd op** |
| one forwarder | 3 | 46 B | 28 B | **1st op** | 56 B | **2nd op** |
| two forwarders | 4 | 58 B | 36 B | **1st op** | 72 B | **2nd op** |

**A bound path pays for its own mint on the first or second repeat, at every hop count.** The
mint is cheap for the same structural reason the form is: the reply's vref list is smaller than
the `dst` it replaces.

## 8. The bench gate — normative for acceptance

An implementation of this RFC is **not acceptable** until it answers the questions below with
measurements taken under the recorded protocol. This section is a conformance requirement on the
*implementation train*, not on peers.

### 8.1 It must answer #788's five questions

[#788](https://github.com/avatarsd-llc/libtracer/issues/788) exists because #504's obvious design
lost on a shape that already ships. Its five questions transfer to this design unchanged, and the
implementation must answer each **for a bound path**:

1. **Where the cache lives** — here, the origin path object and the subscription edge (§7.4);
   show that neither is router-scoped state that a fan-out re-reads.
2. **Multi-link fan-outs** — `reply-spread` is the shape that decides it. A bound path must not
   cost more than the scan it replaces at K=1, and must not regress at K=4.
3. **Invalidation** — here, the §5 stamps. Show that nothing narrower is relied on.
4. **Whether this is the right thing to fix at all** — §2.2 already concedes the terminus-ns
   answer is *no*; the measurement must therefore land on the **byte** and **per-hop-descent**
   axes (§3.2–§3.4), not on the delivery leg.
5. **Thread placement** — `deliver_remote` runs on the **writer** thread; anything a delivery-side
   binding memoizes is reachable from every writing thread. Show the scoping that makes this a
   non-question, as #788 requires.

### 8.2 Under the #807 A/B protocol

Every latency figure MUST be taken per `docs/methodology.md` §"The A/B protocol"
(`docs/methodology.md:387-436`), which is normative for this gate:

1. **Pin both arms identically**, to the same single logical CPU on the same core class
   (`taskset -c 2`) — the recorded confound is large: the same binary reads **+47.0 % to +53.7 %**
   slower on a compact core than a classic one (`methodology.md:407-411`).
2. **Interleave** round-robin in one session, **≥ 10 rounds per arm**, reporting **medians *and*
   ranges** (`methodology.md:421-424`).
3. **Discard the first execution** — a cold first point reads ~313 ns against a 228 ns steady
   state, **+37 %** (`methodology.md:425-427`).
4. Prefer **same-directory A/B** (`methodology.md:428-429`).
5. Where the expected effect is **below the leg's noise floor**, do not reach for a stopwatch —
   use **object-file `cmp`** against the baseline tree (`methodology.md:430-434`).

Cross-worktree build layout is **not** a valid explanation for a difference: it was measured and
refuted, byte-identical output at two paths (`methodology.md:391-405`).

### 8.3 The must-not-regress arm

**The four-link `reply-spread` fan-out is a mandatory arm.** It is the exact shape that killed
#504's memo (140–311 ns/write). A measured latency regression on **any shipped shape** rejects the
implementation, per the standing ruling that a latency regression is never an acceptable trade.

### 8.4 What must be measured, not argued

- **The per-hop descent saving** (§3.4) — currently UNMEASURED and labelled. Instrument:
  `bench_forward_demux` (forward hop vs registry size, `methodology.md:458`) with a bound arm.
- **The terminus deref** against the canonical resolve — expected below the noise floor, so §8.2
  rule 5 applies (object-file `cmp`) before any stopwatch.
- **rv32 flash/RAM delta**, including §6.4's index vector at a realistic vertex count. The
  standing census rule applies: **no "beat" is banked without it.**

## 9. Proposed change

Spec edits land **after acceptance, in a follow-up PR**; this PR adds only the RFC document and
the two glossary entries (§11).

### 9.1 New normative text

- `docs/reference/05-protocol-tlvs.md` — a new `## 0x14 — PATH_REF` section carrying §4's grammar,
  the byte ledger, the `PL=0`/`LL=0` MUSTs, the ≤ 255-element bound, and the `length % 8 != 0` ⇒
  `tr::frame::invalid` rule; `:901`'s "Unassigned: `0x14`–`0x1F`" becomes `0x15`–`0x1F`; `:16`'s
  first-block census line gains the new assignment.
- `docs/reference/05-protocol-tlvs.md` §reserved-range — `FWD`'s `dst` (and `src`) MAY be a
  `PATH_REF`; the `op` byte gains the `& 0x3F` masking rule and the bind-request flag.
- `docs/spec/v1.md` §3 — incorporate the `PATH_REF` constraints alongside the existing PATH
  incorporation bullet (`v1.md:62-64`).
- `docs/reference/03-addressing.md` — the two path forms, and the rule that canonical is the mint
  key and the fallback.
- `docs/reference/13-network-formation.md` — the diameter statement gains the bound-path row
  (H ≤ 255 normative, ≤ 69 reachable today, §4.3).
- `RFC-0004` §B — amended: the `op` byte's flag bits; `dst`/`src` may be `PATH_REF`.

### 9.2 Deferred to implementation review

The NACK spelling (§5.3): extend `HANDLE_NACK` with a hop-index child, or take a sibling code from
`0x15`–`0x1F`. Both are additive; the vocabulary argument (§2.1) favours the sibling.

### 9.3 Conformance obligations on a peer

- **Canonical support stays mandatory.** `PATH_REF` is optional to *emit* and optional to
  *accept*; a peer that does not accept it MUST answer such a frame per the forward-compatibility
  rules of `docs/reference/01-data-format.md` §handling unknown type codes, and the origin MUST
  fall back to canonical. No address is reachable only in bound form — guaranteed by construction,
  since every bound path is minted from a canonical one (§7).
- **A forwarder MUST mask the `op` byte** (`op & 0x3F`) rather than switching on the raw value, so
  an unrecognised flag degrades to the plain opcode instead of an unknown-opcode reject.
- **A host MUST NOT wrap a generation** (§4.4 rule 3); on saturation it refuses to mint.
- **A failed validation MUST drop, never repair** (§5.3).
- **Every bound-form op MUST re-check `acl_allows` at the deref'd vertex** (§6.2).

### 9.4 Conformance vectors

- `path-ref/ref-2host`, `path-ref/ref-3host` — round-trip encodings pinning the ledger of §4.2
  byte for byte.
- `path-ref/ref-len-not-multiple-of-8` — reject.
- `path-ref/ref-pl-set` — `opt.PL=1` on a `PATH_REF` ⇒ reject.
- `path-ref/ref-256-elements` — over the normative cap ⇒ reject.
- **`acl/bound-vs-canonical-allow` and `acl/bound-vs-canonical-deny`** — §6.3's mandated pair,
  asserting byte-identical outcomes between the two spellings.
- Zero existing vectors change: `PATH` is untouched, and every existing frame is canonical.

### 9.5 Code, after acceptance

`core/include/libtracer/tlv.hpp` (`type_t::PATH_REF = 0x14`), the dense index vector (§6.4), the
element codec, the `path_t` binding slot (§7.4), the forwarder's bound-form branch beside
`resolve_mount_at`, and the NACK. Bindings (`bindings/rust`, `bindings/typescript`) and the
Wireshark dissector (`tools/wireshark/libtracer.lua`) follow. Public-header changes go in
`core/CHANGELOG.md`.

## 10. Interactions

- **[RFC-0017](0017-element-addressing-value-plane-index.md) (element addressing, `[n]`)** —
  **orthogonal.** RFC-0017 indexes the **value plane** *inside* a vertex; `PATH_REF` addresses the
  **graph plane** *between* vertices. `PATH` is untouched, so RFC-0017's `[n]` grammar is
  unaffected; a bound path reaching a vertex composes with a `FIELD` selector exactly as a
  canonical one does.
- **[RFC-0018](0018-packed-path-segments.md) (packed segments)** — **orthogonal; both can land.**
  RFC-0018 makes the *canonical* form cheaper (a 5-segment `dst` 49 B → 34 B, RFC-0018:32-33);
  this RFC adds a second form. They compete only in the sense that a cheaper canonical form
  narrows the bound form's margin — from ~2.6× to ~1.8× on §3.2's shapes — and neither changes
  what the other does. Both land independently; whichever lands second re-runs §3.2's table.
- **[RFC-0004](0004-remote-operation-addressing.md) §E.1** — **stays**, as the per-link
  delivery-compaction tier. §2.1 is the comparison; the vocabulary rule is normative.
- **[RFC-0021](0021-wire-subscriber-target-frame-of-reference.md)** — accepted, implementation
  deferred; **this RFC is the home its deferred producer-frame resolution needs** (§7.4).
- **[#419](https://github.com/avatarsd-llc/libtracer/issues/419)** — the open adjudication is
  whether `fwd/fwd-routed-multihop` encodes one three-segment mount or two successive hops; the
  ruling notes "reading the bytes cannot settle it, because both models produce the same `dst`
  string." **A `PATH_REF` makes hop structure explicit on the wire** — H elements is H hosts, with
  no reading required — so a bound-form companion vector is unambiguous by construction. This does
  **not** settle #419 (the canonical vector's meaning is still the maintainer's call) and this RFC
  does not block on it.
- **[RFC-0019](0019-path-depth-bounded-by-bytes.md) / [RFC-0023](0023-path-segment-cap-repriced-32-to-255.md)
  byte bounds** — `PATH_REF` is bounded by **hop count**, not segment count, and derives its own
  bound in §4.3 (normative ≤ 255 elements / 2040 B; reachable ≤ 69 today, ≤ 171 packed).
  `kMaxSegments` and `kMaxPathBytes` (`core/include/libtracer/path.hpp:34`, `:32`) are untouched
  and continue to govern the canonical form alone.

## 11. `CONTEXT.md`

Two entries are added to §Graph, addressing & API in this PR, in the existing format:

- **Bound path** — the second normative path form; node-scoped vref elements; canonical is the
  mint key and the fallback; a failed validation drops and re-mints, never mis-routes.
- **Vertex ref (vref)** — the 8-byte element; index + generation; node-scoped, meaningless
  elsewhere; an address, never a capability.

Both carry an `_Avoid_` line, and the load-bearing one is the vocabulary rule of §2.1:
**"label" stays RFC-0004 §E.1's per-link `u16`.** Calling a vref a label is the confusion this
whole document is arranged to prevent.

## 12. Rejected alternatives

Element shapes are in §4.5. Design-level rejections:

- **Extend §E.1's label to client-originated binding** (#504's original shape). Rejected on state:
  it puts a table on **every hop**, which is the property §E.1 itself scopes to compact flows only
  ("a constrained ws node forwarding 50 cold reads holds **zero** label state", RFC-0004:187), and
  it needs an advertise round before the first op. The bound path holds nothing at any hop and
  mints inside work already happening.
- **A per-hop route cache keyed on canonical bytes.** Rejected: it is the ADR-0062 reverse-index
  shape twice refused — "it moves work onto the control plane's lock to serve the minority flow,
  and it is a SECOND invalidation mechanism beside one that works"
  (`core/include/libtracer/child_registry.hpp:281-283`).
- **A negotiated capability for bound-path support.** Rejected for minimalism: v1 has no
  capability negotiation and deliberately so (`CONTEXT.md` §Capability negotiation, ADR-0013). The
  fallback in §9.3 is a closed-form local decision needing no negotiation — try bound, take the
  NACK or the unknown-type answer, use canonical.
- **Making `PATH` itself carry either form** (a mode bit on `0x06`). Rejected: it would make the
  canonical-bytes property conditional, and that property is what
  `path_lookup_key` (`core/src/op_resolve_walk.hpp:660-669`) and every peer, cache and router
  keyed on PATH bytes depend on — the injectivity failure `op_resolve_walk.hpp:649-652` records
  (two byte-different PATHs addressing one vertex) is exactly what a mode bit reintroduces. A
  separate type code costs one codepoint out of eleven free and keeps `0x06` meaning one thing.

## 13. What is UNMEASURED

Labelled per the standing rule — a quantitative claim names its instrument or is labelled.

- **Every byte figure in §3.2, §3.3, §4.2 and §7.5 is arithmetic** over the shipped encoding rule
  (`reference/05:377`) and the recorded mount-run shapes (RFC-0023:185-188). Not a capture. A
  routed capture at H ≥ 3 does not exist; `bench_hop_chain` remains recorded as **confounded** and
  must not be cited.
- **The per-hop descent saving (§3.4) is UNMEASURED.** Structural argument only. §8.4 names the
  instrument.
- **The terminus deref cost is UNMEASURED**, and expected to sit below the noise floor — §8.2
  rule 5 applies before any stopwatch.
- **rv32 flash/RAM delta is UNMEASURED**, including §6.4's index vector. The size census is the
  instrument; no "beat" is banked without it.
- **The `u32` generation lifetime figures in §4.4** are arithmetic over an *assumed* retire rate.
  No retire-rate measurement from a deployment exists. This is why the saturation rule, not the
  width, carries the safety.

## 14. What would falsify this RFC

1. **A measured latency regression on any shipped shape** — especially the `reply-spread` four-link
   arm (§8.3). This design dies the same death #504's memo did, and by the same rule.
2. **The per-hop descent saving measures to nothing.** If a bound hop is not measurably cheaper
   than `resolve_mount_at` at realistic registry widths, §3.4 collapses and the case reduces to
   bytes alone — at which point §E.1's 10 B flat beats 8 B/host from H ≥ 2, and this RFC should be
   withdrawn in favour of extending §E.1.
3. **The index vector's RAM cost is not affordable on rv32** at a realistic vertex count (§6.4).
   4 B/vertex is small, but ADR-0067's census banked "libtracer's own static RAM is approximately
   zero", and this is not zero.
4. **A generation-wrap path survives the saturation rule.** If any reachable sequence lets a stale
   vref validate, the guard has failed and the element shape must be re-derived — §4.4's entire
   argument rests on saturation closing that class.
5. **The maintainer rules that two normative path forms is one too many.** Then the design is
   sound but unwanted, and the answer is to extend §E.1 rather than to add a form.

## 15. Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window",
this is an **amendment**: RFC plus maintainer approval, comment window **waived by default** while
solo-maintained (verified in the header; the waiver reverts the moment `docs/implementations.md`
gains a registered implementer). Scope is **v-NEXT** — this document explicitly does not gate
v0.7.0, and the spec edits of §9 land in their own PR after acceptance.
