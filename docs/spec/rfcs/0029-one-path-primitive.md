<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0029 — One path primitive: the owner-issued `(index, generation)` pair, carried per hop, local = forwarded

<!-- status: accepted; superseded-in-part-by: RFC-0031, RFC-0032 -->

| Field | Value |
| ---- | ---- |
| **RFC** | 0029 |
| **Title** | One path primitive: the owner-issued `(index, generation)` pair, carried per hop, local = forwarded |
| **Status** | **accepted** (2026-09-30; proposed 2026-09-29), maintainer-ratified on [PR #1634](https://github.com/avatarsd-llc/libtracer/pull/1634) with all recommendations; comment window waived by default per [GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window" (solo-maintainer clause) and not invoked. The design of §§4–11 was **ruled** by the maintainer on 2026-09-29 in [#1631](https://github.com/avatarsd-llc/libtracer/issues/1631) (the issue body and its feasibility comment, rulings 1–4); this document is its transcription in normative form, not a proposal seeking a direction. The first-fire learn of §7.2 was confirmed on 2026-09-29, and **§8.2's rule — a same-named re-add MUST bump the generation — stands as written.** **All five §16 questions are RULED at acceptance (2026-09-30)**: `PATH_REF_REVERSE` is spelled as a `PATH` of PAIR/NAME elements (§7.1), the first-fire learn answers to **one node-local endpoint per node** with the edge named in the reply's `dst` tail (§7.2), a **per-boot epoch** complements §8.2 rule 2 on transports that cannot report a session boundary (§8.2 rule 4), `kind = 0x16` is reused with `len = 8` (§5.1), and `vertex_handle_t` is `(u32, u32)` by value with any ISR pointer fast door left to S7's bench as a compile-time option (§4.3). **S3 deletes the RFC-0027 table**, superseding [#1668](https://github.com/avatarsd-llc/libtracer/issues/1668), [#1669](https://github.com/avatarsd-llc/libtracer/issues/1669) and [#1647](https://github.com/avatarsd-llc/libtracer/issues/1647); the RFC ships as **v0.18.0** (§13.3). Spec, reference, `CONTEXT.md` and code edits land slice by slice (§13); S0 is this document plus §12's text. **Amended by [RFC-0030](0030-host-api-walks-the-graph-reply-is-a-remote-write.md)** (accepted 2026-10-07): §4.3, §6.1, §6.2, §7.1 and §7.2. **Amended by [RFC-0031](0031-bus-session-anchors-are-children-of-their-door.md)** (accepted 2026-10-07): §10 (the shared-mount slice) is replaced. **Amended by [RFC-0032](0032-delete-compact-streams-ride-the-chain.md)** (accepted 2026-10-09): ruling 6 and §9.2 are withdrawn, together with §1's, §5.3's, §11's, §12.4's, §13.2 S3's and the 2026-10-02 erratum's text that kept `COMPACT`. |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-09-29 |
| **Comment window** | waived by default while solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"); invoke explicitly if outside input is wanted. Verified at drafting: `docs/implementations.md` still lists no registered implementation, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** This retires a type code's meaning (`0x14`), widens an escape record's payload (`kind = 0x16`), withdraws a flag bit's meaning (`FWD` `op` bit 7), changes what a relayed `REPLY` carries in `src`, and deletes a per-hop table that was normative. Every one of those is a wire-surface change; GOVERNANCE.md names a new or altered frame shape as amendment territory. **No backward compatibility is owed** — the project takes none (the RFC-0028 ruling, restated as ruling 10 in §3). |
| **Tracking issue** | [#1631](https://github.com/avatarsd-llc/libtracer/issues/1631) |
| **Target spec version** | v1 itself. `docs/spec/v1.md` still reads "(DRAFT)" and "The wire format is not yet stable". Same route RFC-0018, RFC-0023, RFC-0024, RFC-0026 and RFC-0027 took. |
| **Scope** | **v0.18.0** (ruled 2026-09-30: v0.17.0 is cut after [#1670](https://github.com/avatarsd-llc/libtracer/issues/1670) without it). Runs beside RFC-0028 (the lean value path, [#1627](https://github.com/avatarsd-llc/libtracer/pull/1627)) and blocks none of its slices (§13.3). |
| **Supersedes** | [RFC-0027](0027-label-switched-path-compression.md) as a *form* (the 16/16 path label and its per-hop table — §12.3); [RFC-0024](0024-bound-paths-node-scoped-vertex-ref-source-routing.md) §7.1 (request flag, reply trailing list, strip-whole-list, and amendments 1–2's bare-array body for `0x15`), §7.5 (`op` bit 7) and the `0x14` spelling of §4.1 (§12.2); RFC-0027 §9 ("local IO is out of scope" — withdrawn by ruling 2). |
| **Descends from** | [RFC-0024](0024-bound-paths-node-scoped-vertex-ref-source-routing.md) (the element — kept verbatim), [RFC-0027](0027-label-switched-path-compression.md) (the distribution rules — kept; §15 clause 5 is the falsifier this RFC fires), [RFC-0004](0004-remote-operation-addressing.md) §A/§B (path-as-route — unchanged model), [#830](https://github.com/avatarsd-llc/libtracer/issues/830) (the local edge binding), the [#1629](https://github.com/avatarsd-llc/libtracer/pull/1629) ruling (no per-request state at a hop) |

> **Numbering note.** Numbering gaps and why they are not reused are recorded in the
> [ADR and RFC index](../../adr-rfc-index.md#numbering-gaps).

---

## 1. Summary

libtracer names a vertex in a call through four mechanisms that were designed one at a time:
canonical strings resolved by a per-hop strip-and-descend; RFC-0024 bound paths (`PATH_REF`, a
mint-request flag, a trailing reply list, `PATH_REF_REVERSE`); RFC-0027 path labels (a 4-byte alias
minted from a per-hop, per-peer table); and, in-process, `vertex_handle_t`. The feasibility
inventory attached to [#1631](https://github.com/avatarsd-llc/libtracer/issues/1631) found that
every one of them already reduces to a single primitive — a **node-scoped `(u32 index, u32
generation)` into the owner's dense vertex index** — and that the only things that differ are the
wire spelling and *who holds a table*. RFC-0024's element carries the pair with **no hop state**,
because the vertex index *is* the table and it is sized by the graph, not by traffic. RFC-0027's
label is a 4-byte alias of that same pair that buys 4 bytes per hop with the only per-hop table in
the path plane. RFC-0027 §15 clause 5 named this outcome as its own falsifier.

**This RFC makes the pair the one primitive.** A vertex is named, everywhere, by the pair its owner
issues for it. An address is a **chain** of such elements, one per hop, spelled inside the ordinary
`PATH` body so that elements and literal names mix freely. Each hop consumes its head element with
a bounds check, a generation compare and a registered-bit test, evaluates the operation's right at
the vertex it reached — exactly as it does for a name — and forwards the tail. **A local call is a
chain of length one, and `vertex_handle_t` is the same pair.** The chain is learned passively on
the reply, per hop, with no flag and no frame; it is a cache of the canonical string, which stays
the only durable truth; and it is re-learned after a reboot, a departure, a retirement or a
refusal. No hop holds anything whose loss changes an answer. RFC-0027's table, ceilings, owner
stamps and census are deleted; RFC-0024's request flag, trailing reply list and bare `PATH_REF`
type are retired. RFC-0004 §E.1's `COMPACT` stream handle is the single named exception, for
streams only, and it is recoverable by construction.

## 2. Motivation

> **Corrected 2026-10-02 by the citation erratum ([#1701](https://github.com/avatarsd-llc/libtracer/issues/1701)) — see §Erratum at the end of this
> document.** Every code citation in this RFC names a symbol and its file; the `file:line`
> spellings it was drafted with did not resolve to the code they described.

### 2.1 Four spellings of one thing

The inventory (#1631, feasibility comment, and its §1) lists the shapes that hold the same pair
under different names at head:

| type | where | what it is |
| --- | --- | --- |
| `vertex_slot_t` | `core/include/libtracer/graph.hpp` | the pair, node-local |
| `path_ref_element_t` | `core/include/libtracer/path_ref.hpp` | the pair, on the wire (RFC-0024) |
| `path_label_target_t` | `core/include/libtracer/path_label_table.hpp` | `using` of the same struct — what a label *aliases* |
| `target_binding_t` | `core/include/libtracer/subscriber.hpp` | the pair, cached per local subscriber edge (#830) |
| `peer_handle_t` | `core/include/libtracer/peer_handle.hpp` | the pair, for a bus peer's slot |
| `vertex_handle_t` | `core/include/libtracer/graph.hpp` | a pointer that converts to the pair in O(1) both ways (`graph_t::vertex_slot` / `deref_vertex_slot`) |

Three of them cross the wire or a cache boundary; all six validate the same way (`bounds`,
`generation`, `registered`), and the one that does not carry a generation (`vertex_handle_t`) is
paired with one everywhere it is cached. There is one primitive here and six names for it.

### 2.2 Where local and forwarded diverge today, and which of it is essential

The inventory's §2 walked `route_fwd_ingress` (`core/src/fwd_router.cpp`) and the three subscribe
doors. What it found essential — and what this RFC keeps — is short:

1. a hop needs a **transport** to egress; a terminus needs a **graph op**;
2. a **shared (bus / multi-peer) mount** resolves a peer segment per frame that no element can stand
   for until each accepted session has an anchor vertex that can be egressed *through* (§10);
3. the **delivery plane has no reply leg** (`FWD{WRITE, src = empty}`, RFC-0004 amendment 2), so
   passive learning on a reply cannot by itself teach a subscription its chain (§7).

Everything else that differs is accidental, and this RFC removes it (ruling 9):

- **two name lookups for one tree** — `graph_t::find_ptr` (`core/src/graph.cpp`, the ADR-0057
  child walk) for a local target versus `child_registry_t::longest_prefix`
  (`core/include/libtracer/child_registry.hpp`, the strip-K registry scan) for a target below a
  mount, although a connection vertex is a graph vertex too (the ctx's `conn_slot`,
  resolved in `fwd_router_t::add_child`, `core/src/fwd_router.cpp`);
- **two reply-leg carriers for one learned chain** — RFC-0024 appends a trailing `PATH_REF` to the
  reply (`mint_fn` in `fwd_router_t::route_fwd_forward`, `core/src/fwd_router.cpp`) while RFC-0027 rewrites the hop's part of
  the reply's `src` in place (`fwd_router_t::label_src_prefix`, `core/src/fwd_router.cpp`);
- **eager versus post-auth mint** — an RFC-0024 mint *reads* a slot and is attached to the success
  arms for free, while an RFC-0027 mint *spends* a slot and therefore had to be a post-auth lambda
  (RFC-0027 §8.1). Once the element is the pair there is nothing to spend and the distinction
  vanishes;
- **a second stamp for one departure** — RFC-0027 needed a per-tenancy `label_peer` stamp
  (`fwd_router_t::next_label_peer_bits`, stamped in `fwd_router_t::acquire_ctx`, `core/src/fwd_router.cpp`) beside the vertex generation,
  because a label's meaning lived in the table's owner check rather than in the vertex. §8 gives the
  vertex generation that job outright.

### 2.3 What the table cost, and what it bought

RFC-0027 §12.4 / §14 measured its table on rv32: **+3 388 B flash** for the `path_label_table` TU,
**24 B per slot** (60 B on rv32 at the default, 304 B peak store at 8 mints), and **+2 276 B of
`.text`** in `fwd_router.cpp` for the origin car. It bought 4 bytes per hop over the pair (7 B
versus 11 B as an in-`PATH` element, §5.2) and terminus-residual compaction, which the pair form
gives equally (the terminus's element names the target vertex directly). It also bought the one
thing this RFC's statelessness definition forbids: a hop whose table, if lost, turns every labelled
frame from that peer into `NOT_FOUND` until the peer re-sends the string — an answer that changes
with the state (inventory §4). The pair form's cache, wherever it lives, falls through to the string
it was learned from and yields the same answer.

### 2.4 The stateless ruling that this design is written under

[#1629](https://github.com/avatarsd-llc/libtracer/pull/1629) shipped RFC-0028 slice 2 without its
pending-forward table, on the analysis that a hop holds **no per-request state**: RFC-0004 §A's
"replies are stateless and source-routed back", §B's "forwarders hold no per-hop request state",
and `docs/modules/fwd-router.md`'s "a hop may reboot mid-operation and the reply still routes" are
the texts that ruling kept true. This RFC extends the same discipline from per-request to per-flow
state in the *path* plane and, in §9, writes down the definition both rulings rest on.

## 3. The rulings this document encodes

Maintainer, 2026-09-29 (#1631 body and feasibility comment). Numbered here so later sections can
cite them:

1. **One primitive.** The owner-issued `(vertex index, retirement generation)` pair, carried per
   hop. There is **no per-hop label table**: RFC-0027's table is deleted. The mechanism is RFC-0024's
   element plus RFC-0027's distribution rules — passive mint on the reply `src`, per-element mixing
   inside `PATH`, `NOT_FOUND` falls back to the string — minus RFC-0027's table.
2. **Local = forwarded, including the host API.** `vertex_handle_t` is the same pair; RFC-0027 §9
   is superseded. A local call is a chain of length one.
3. **Minted, not hashed.** The owner hands out its own index; nothing is derived from the bytes of
   a name.
4. **The full string path is the durable truth**; the chain is a learned cache, re-learned after a
   reboot, a departure, a retirement or a refusal.
5. **"Stateless forwarder" means no HARD state** — nothing whose loss changes an answer. No
   per-request state at a hop, ever.
6. **RFC-0004 §E.1 `COMPACT` stream handles are the single named exception**: streams only,
   recoverable (`HANDLE_NACK` and re-advertise), never a wrong delivery.
7. **Subscriptions learn their chain on the request leg** (`PATH_REF_REVERSE` at subscribe),
   because deliveries have no reply leg. Two gaps close: the mount-routed subscribe that drops the
   reverse list (`graph_t::subscribe_wire`, `core/src/graph.cpp`), and `subscribe_toward`
   (`fwd_router_t::subscribe_toward`, `core/src/fwd_router.cpp`), which requests no chain at all.
8. **Shared (multi-peer / bus) mounts** refuse the pair form until a session anchor can be egressed
   through (#741's ruling). That anchor is in scope as a later slice (§13); until it lands, such
   mounts keep strings.
9. **Remove the accidental divergences** of §2.2: two name lookups for one tree, two reply-leg
   carriers, eager versus post-auth mint.
10. **No backward compatibility.** The wire may change; this is an amendment.

And one clause this RFC states as a **normative requirement of the unified design**, not as a
ruling: **the authorization check at every hop is a function of the dereferenced vertex, the
caller and the right, never of how the element was spelled** (§6.4).

## 4. The primitive

### 4.1 The element

**Normative.** A **path element** names one vertex on one node. It is either

- a **NAME** — a canonical segment, spelled as an RFC-0018 segment record `[u8 len][len bytes]`,
  `len` in `1..64`; or
- a **PAIR** — the vertex's owner-issued `(u32 index, u32 generation)`, little-endian, spelled as an
  RFC-0018 escape record `00 <kind = 0x16> <len = 8> <u32 index><u32 generation>` (11 bytes).

The PAIR's two fields are RFC-0024 §4.4's, unchanged and with its derivation standing: the index is
a slot in the owner's dense, append-only, pointer-stable vertex index (RFC-0024 §6.4; the
chunked, lock-free-to-read `graph_t::vertex_slots_`, `core/include/libtracer/graph.hpp`), bounds-checkable and unreachable on both targets
at `u32`; the generation is the vertex's retirement stamp, **saturating and never wrapping**
(`kGenerationSaturated`, `core/include/libtracer/vertex.hpp`), refused at the ceiling on both the
issuing side (`graph_t::vertex_slot_at`, `core/src/graph.cpp`) and the honouring side
(`graph_t::deref_vertex_slot`, `core/src/graph.cpp`).

A PAIR is **node-scoped**: it means something only on the node whose index it names. It is
**minted, never hashed** (ruling 3): the owner issues the index at registration, so two vertices
cannot share one and a false match is not constructible — the property RFC-0027 §4.2 argued once and
this RFC does not re-litigate. The three-way comparison (digest / pair / 16-bit label) is in the
inventory's §5 and is not repeated here; its conclusion is that the pair costs 4 bytes more per hop
than the label and buys away the table, the ceiling, the owner check and the peer-driven allocation
seam.

### 4.2 The chain

**Normative.** An address is a `PATH` whose body is a sequence of path elements, read
**head-first**. Element *k* is meaningful on the *k*-th node of the route and on no other. A hop
consumes exactly its own head element and forwards the tail (RFC-0004 §A/§B's monotonically
shrinking `dst`, unchanged). A **local call is a chain of length one**: its only element names the
target on the calling node, and the caller consumes it the same way a forwarder consumes a head
element (`bound_dispatch`, `core/include/libtracer/fwd_router.hpp` — already documented as "the same
act" as a forwarder's).

**Mixed spellings are legal and expected** (RFC-0027 §5.2, carried verbatim). Any element may be a
NAME or a PAIR, in any order. A node that cannot issue a PAIR for its part — a shared mount (§10),
a saturated slot, a connection vertex retired under its link — leaves that part as NAMEs and every
other node's part still compacts. Because every element self-describes by its kind and is read by
exactly one node, **skipping is not expressible**; RFC-0024 §7.1 erratum 1's strip-whole-list rule
exists only for positional arrays and is retired with both of them (§5.3, §7.1).

### 4.3 `vertex_handle_t` is the pair (ruling 2)

**Normative for the reference implementation.** `graph::vertex_handle_t` becomes the pair by value
— the same two `u32` a wire element carries — and every host-API entry (`read`, `write`, `await`,
`subscribe`, `retire`, the `:`-field doors) takes it. Dereference is `deref_vertex_slot`'s three
tests. RFC-0027 §9's boundary ("a label never appears in a host-API call") is withdrawn: there is
no longer a second thing that could appear there. **Representation, ruled 2026-09-30 (§16 Q5):**
`(u32 index, u32 generation)` **by value** — 8 B on both targets. Whether the ISR publisher
(`docs/reference/00-overview.md` claim 6) keeps a pointer-returning fast door is decided by S7's
bench; if it is kept, it is a **compile-time option** (the compile-time-by-default doctrine), never
a second runtime handle type.

*What this costs, stated.* The pointer handle of ADR-0056 has "identical codegen" to `vertex_t*`
(`vertex_handle_t`'s class doc, `core/include/libtracer/graph.hpp`), and RFC-0027 §3.2 measured `deref_vertex_slot` at a
flat **11 ns** (shared lock, bounds, compare). Ruling 2 spends that on every local hot-path call in
exchange for one identity across the host API, the cache records and the wire, so that a handle
cached anywhere validates itself and #830's per-edge `target_binding_t` becomes simply "the
handle". §13's slice for it is bench-gated (§13.2 S7) and §15 clause 3 is its falsifier.

## 5. The wire form

### 5.1 `PATH` (`0x06`) carries both kinds

**Normative.** The `PATH` body grammar of RFC-0018 §5 with RFC-0027 amendment 5's escape arm, one
change: the escape record of `kind = 0x16` carries **`len = 8`**, a `u32 index` then a `u32
generation`, little-endian. A `kind = 0x16` record of any other length is malformed and refuses the
address (`INVALID_PATH`), never the frame. **Reusing `kind = 0x16` rather than minting a fresh kind
is ruled (2026-09-30, §16 Q4)**: a retired 7-byte RFC-0027 record is simply "malformed", and no
diagnostic distinguishes it. A hop that does not implement this kind steps over it by
its declared length and relays it (RFC-0018 §5.4 amendment 1), as today.

`docs/spec/v1.md` §3.1's path-handle conformance is untouched: a **canonical key** — a path handle,
a pre-encoded `.rodata` literal, a `path_lookup_key` — carries **no** escape record. A chain is a
*frame* path and never a key.

### 5.2 Bytes per hop

| spelling | per hop | who holds what |
| --- | ---: | --- |
| canonical `net/<module>/<name>` (three segments) | 20–32 B | nothing per request; the registry descent |
| RFC-0027 path label (retired) | 7 B | a 24 B table slot per (peer, part) at the hop |
| RFC-0024 bare `PATH_REF` element (retired) | 8 B | nothing |
| **PAIR element in `PATH` (this RFC)** | **11 B** | **nothing** |

The pair form costs 3 B/hop over the bare array it replaces and 4 B/hop over the label. It gains,
over the array, per-element mixing and one spelling for every address; over the label, the deletion
of the table (§12.3). The inventory's §6 has the measured hot-path figures this rests on: a bound hop
at 261.7 ns versus a canonical hop at 304.7 ns (RFC-0024 §3.4), `deref_vertex_slot` flat at 11 ns at
every depth (#830), the mount descent itself at 25 ns per pass (`child_registry.hpp`). A pair deref
replaces both the descent and the terminus walk.

### 5.3 What is retired on the wire

- **`0x14` `PATH_REF`** — the bare fixed-stride address form. Its element survives as the PAIR
  inside `PATH`; the separate type code carries no meaning after this RFC and a host MUST refuse a
  frame that presents it as a `dst` (`INVALID_PATH`).
- **`FWD` `op` bit 7 (the mint request)** and RFC-0024 §7.5's flag semantics. The opcode masking
  rule `op & 0x3F` (RFC-0024 §9.3) is kept — it is harmless and it is what lets bits 7–6 be
  reserved-MUST-be-zero rather than reinterpreted.
- **The trailing reply `PATH_REF`** (RFC-0024 §7.1 step 2) and **erratum 1's strip-whole-list rule**
  for it.
- **The `kind = 0x16`, `len = 4` label element** of RFC-0027 amendment 5, with its `(u16, u16)`
  payload, `kPathLabelMaxGeneration` retirement and owner check.

**Kept, with its body re-spelled:** `0x15` `PATH_REF_REVERSE` (§7.1) keeps its type code, its
role and erratum 3's empty-`PATH` re-heading; its body becomes the `PATH` element grammar of §4
(ruled 2026-09-30), which retires RFC-0024 §7.1 amendments 1–2's bare-array grammar and erratum 1's
strip rule with it. **Kept, unchanged:** RFC-0004 §E.1's `ADVERTISE`/`COMPACT`/`HANDLE_NACK`
(§9.2); every `FWD`/`FIELD`/`REPLY` layout of RFC-0004 §B–§D.

## 6. The per-hop algorithm

**Normative.** A host receiving `FWD{op, dst, src, …}` reads the head element of `dst`:

1. **NAME head.** Resolve as today: the strip-K mount descent (ADR-0061) if the leading run names a
   connection vertex, else the local tree walk. Unchanged by this RFC except for §6.4 and §13.2 S6.
2. **PAIR head.** `deref_vertex_slot(index, generation)`: bounds, `bound_generation_matches`
   (which refuses a saturated element), `registered()`. Any refusal is **`NOT_FOUND`**, answered
   from the request's own `src` (§6.3); the host MUST NOT forward the frame, MUST NOT apply the
   operation and MUST NOT attempt any repair — RFC-0024 §5.3's drop-never-mis-route and RFC-0027
   §7.2's `NOT_FOUND` are one rule here.
3. **What the vertex is decides what happens next**, and nothing else does:
   - it is a **connection vertex of a point-to-point child** (its slot is some live child's
     `conn_slot`, `child_registry_t::by_conn_slot`) — this hop **egresses** over that child's link with `dst`
     shrunk by exactly the consumed element and `src` grown canonically by the inbound mount run
     (RFC-0004 §B). The residual tail MUST be non-empty; a PAIR that dereferences to a connection
     vertex as the *last* element addresses the connection vertex's own `:`-facets and is a local
     terminus, not a hop (RFC-0004 §A's dual nature, unchanged);
   - it is a **shared-mount vertex** (a bus child; `multi_peer`) — refused, `NOT_FOUND` (§10);
   - it is **any other vertex**, and the element is the last — **terminus**: apply the op with the
     same `apply_op` the NAME spelling reaches (`core/src/op_resolve_walk.hpp`); a bound terminus
     never write-creates (RFC-0005 amendment 1's remote rule, already the shipped behaviour);
   - it is any other vertex and the element is **not** the last — `INVALID_PATH`: a `/`-descent
     below a non-mount vertex names nothing.
4. **Authorization (§6.4)**, evaluated at the dereferenced vertex before any egress or op.

A `REPLY` is routed by the same step against its `dst` (the return route the request accumulated),
and a hop that has consumed the last element of a reply's `dst` is the originator (RFC-0004 §B's
terminus-reply asymmetry, unchanged). The forwarding hop is **zero-heap and holds nothing** across
frames (ADR-0038 invariant 2, RFC-0024 §2.1 restored as the property of the one form).

### 6.1 Reply routing — `src` on the request leg stays canonical

**Normative.** A forwarding hop grows the request's `src` by the **NAME** run of its inbound link,
as RFC-0004 §B specifies, and never by a PAIR. RFC-0024 §7.1 erratum 2 (a) gives the reason and it
still holds: the holder of a return route holds no other original — `src` as received *is* it — so
a return route spelled in PAIRs would be an address reachable in cached form alone, which ruling 4
forbids. The reply's `dst` is therefore a canonical route consumed hop by hop, exactly as today, and
every reply vector (`fwd/fwd-reply-*`) keeps its `dst` byte-identical.

### 6.2 First-call learning — the reply's `src` carries the chain

**Normative.** A host relaying or issuing a `REPLY` **prepends to that reply's `src` its own
part of the forward route**, spelled as a PAIR when it can issue one and as the canonical NAME run
when it cannot — **never nothing**:

- a **forwarding hop** contributes its PAIR for the connection vertex the reply **arrived over**,
  which is the one it egressed the request through (`fwd_router_t::hop_mint`, `core/src/fwd_router.cpp`,
  reads it off the frame's own arrival, never from a table); a hop that cannot — a shared mount, a
  connection vertex retired under its link, a saturated slot — contributes the inbound child's mount run as
  NAMEs instead;
- the **terminus** seeds the reply's `src` with its PAIR for the target vertex it applied the op to
  (the last element of the forward route), or the residual NAMEs it resolved if it cannot;
- the **origin** receives a `src` that is the **complete forward route from its first link onward**
  in head-first order, prepends its own element for hop 0 — its PAIR for the connection vertex of
  the link the request left by (`fwd_router_t::connection_ref`, `core/src/fwd_router.cpp`), or the target's
  PAIR for a local call — and caches the result in its `path_t` beside the canonical bytes, which are
  **never discarded**. Subsequent requests spell the cached chain as `dst`.

This is RFC-0027 §6.1 with erratum 2's region ruling (the reply's `src` is the only region that
survives to the origin) and erratum 4's direction ruling (minting rides the reply and only the
reply), carried into this RFC with two changes: the element is the 8-byte pair, and a non-minting
hop contributes its **string** part rather than nothing, so the origin never receives a route that
skips a hop and can adopt what it is handed verbatim (RFC-0027 amendment 8's origin rule, kept).
RFC-0004 §B's "a reply accumulates no return route" is **amended**: a reply's `src` is the
responder's address **from the origin's vantage** — which, under §A's location-dependence, is
exactly the provenance §B's "the responder's own endpoint" was reaching for and could never be from
any other vantage.

**Why no request flag.** RFC-0024 spent `op` bit 7 so that a hop only pays for a mint when asked.
A PAIR costs the issuing hop one `vertex_slot_at` — a shared lock, a bounds test and two loads —
and no allocation, so there is nothing to protect with a flag; and a mint that *reads* a slot is
post-auth by construction, because the reply it rides exists only after the terminus's gates passed
(RFC-0027 §8.1's intent, satisfied without a lambda — ruling 9's third divergence).

### 6.3 Refusal and fallback

**Normative.** A host that cannot validate a PAIR answers `tr::path::not_found` to the request's
`src` (when non-empty — RFC-0004 amendment 2's "no reply requested" applies to a delivery, §7).
The origin, on `NOT_FOUND` for a chain-spelled request, **clears its cached chain and re-sends the
canonical string it still holds**; the reply to that request re-teaches the chain. **One failed
operation is the entire cost.** There is **no withdraw frame, no unbind, no lease, no TTL and no
repair** (RFC-0027 §7.3, carried verbatim). A **multi-element forward refusal** now also answers
`NOT_FOUND` rather than dropping silently: RFC-0024 §5.3 erratum 4's asymmetry existed because the
reverse route of a bare `PATH_REF` frame was not reliably spelled; under §6.1 it always is.

### 6.4 Authorization at every hop is spelling-independent

**Normative.** RFC-0004 §F's two gates — the forward right at each intermediate connection vertex,
the operation's right at the target — are evaluated at the **dereferenced vertex**, for the
**caller's subject** and the **operation's right**, and the verdict is a function of those three
alone. A conforming host MUST reach the same verdict for a hop whether that hop's element arrived
as a NAME or as a PAIR, and MUST evaluate the check on every hop of every spelling. A generation
match **authorizes nothing** (RFC-0024 §6.2, `deref_vertex_slot`'s own doc comment): it says the
vertex is the same one, never that the caller may act on it. A PAIR is an address, not a
capability; a peer may present any pair, exactly as it may spell any string, and the gate at the
vertex is the gate. The reference implementation realizes this as **one `allows` site** shared by
both arms of §6 step 3 (§13.2 S6).

## 7. Subscriptions and deliveries

The delivery plane is `FWD{WRITE, dst = <route to the consumer>, src = empty}` — no reply leg
(RFC-0004 amendment 2). So a subscription edge cannot learn its chain from §6.2, and ruling 7
places the learning on the **subscribe request's** forward legs.

### 7.1 `PATH_REF_REVERSE` accumulates unconditionally

**Normative.** A forwarding hop relaying a **subscribe** request (a `SUBSCRIBER` write) prepends its
**reverse-direction element** — its PAIR for the identity the request **arrived** on: the inbound
connection vertex point-to-point, the accepted session's anchor for a bus arrival
(`fwd_router_t::reverse_hop_ref`, `core/src/fwd_router.cpp`) — to the request's trailing `PATH_REF_REVERSE`
(`0x15`) child, creating the child if the request carries none. No flag gates this (bit 7 is gone,
§5.3).

**The `0x15` body is a `PATH` of path elements** (§4.1–§4.2; ruled 2026-09-30, §16 Q1), not a
positional array. A hop that cannot issue a PAIR for its arrival contributes its inbound mount run
as **NAMEs** — never nothing, the §6.2 rule on the request leg — so the list is never stripped and
never skips a hop. RFC-0024 §7.1 erratum 1's strip-whole-list rule and amendments 1–2's bare-array
grammar are **retired** with the last positional list. The price is **+3 B per element, on the
subscribe request leg only**: deliveries spell the learned chain in `dst` exactly as §4.2 does, so
the steady-state delivery frame does not grow. The responder completes the list
with element 0 — its own PAIR (or NAME run) for the egress toward the writer — and stores it beside
the canonical return route (`subscriber_remote_t::reverse_route` / `return_route`,
`core/include/libtracer/subscriber.hpp`), exactly as amendment 1 already specifies.

Each delivery then spells `dst` as the stored chain: element 0 is consumed locally (§6 step 3, the
egress), elements 1.. ride the wire, `src` empty. A refusal at any hop is a **drop** — there is no
`src` to answer — and the edge falls through to its canonical `return_route` on the **next**
delivery and re-learns per §7.3. This is the shipped `deliver_remote` behaviour
(`core/src/fwd_router.cpp`), made unconditional.

### 7.2 The two gaps ruling 7 closes

**Gap 1 — the mount-routed target.** `subscribe_wire` splits a `PATH` target that names a local
mount into link + string residual and **drops the reverse list** (`graph_t::subscribe_wire`, `core/src/graph.cpp`:
"the mount route is canonical-only"). The drop is *correct* — that list spells the way back to the
**writer**, and a mount-routed edge delivers to a third party (RFC-0021) — but the edge is then
left with no chain forever. **Gap 2 — `subscribe_toward`** (`fwd_router_t::subscribe_toward`, `core/src/fwd_router.cpp`), the
host-local dual every board→peer wire uses, installs an edge with a string residual and puts
nothing on the wire at subscribe time, so there is no request leg to learn from.

**Normative — the first-fire learn (ruled 2026-09-29).** An edge whose target is reached through a link and that
holds **no chain** sends its **first** delivery with a non-empty `src` — the node's learn endpoint
followed by a tail that names the edge — so that the terminus answers a `REPLY` whose `src` carries
the forward chain per §6.2. The edge adopts that chain; every later delivery is `src`-empty as today. A chain the edge
holds is spent until a refusal or a departure clears it (§8), at which point the next fire is again
a learning fire. The cost is **one `REPLY` per edge, once per learn**, sent by the terminus to
the edge's learning fire. That is the one frame the learn adds; it is an ordinary `REPLY`, so it adds
no new frame *type* and no new field, and no frame at all on any later delivery. This is
RFC-0027 §6.2's "each subscription's first fire triggers minting" applied to the edges that have
no request leg, and it closes both gaps with one rule: the mount-routed edge and the
`subscribe_toward` edge both start with no chain and both learn on their first fire.

**Normative — the reply endpoint (ruled 2026-09-30, §16 Q2).** A node has **one** node-local learn
endpoint, shared by every edge on it. The learning fire's `src` is that endpoint's route with the
**edge identified in the tail**; the reply's `dst` is consumed back to the endpoint (§6.1) and the
residual tail selects the edge. There is **no per-edge endpoint segment**: that would cost RAM ×
edges, and the cost lands on the narrow targets that hold the most edges per byte. A reply whose
tail names no live edge (the edge was removed while the learn was in flight) is dropped; the next
fire of a surviving edge learns again. The tail's spelling is S5's; it is node-local and no other
host reads it.

### 7.3 The durable entry (lt#1623's shape)

**Normative.** A persisted subscription entry stores the **canonical string route** and its
enable state, and **never** the chain: generations are per-process on the far node, so after that
node's reboot every element is stale by construction. The chain is re-learned on the first fire
after restore (§7.2). An entry whose far end is unreachable costs nothing until it is reached — an
edge with no chain and no link is skipped, which is [#1623](https://github.com/avatarsd-llc/libtracer/issues/1623)'s
own requirement. The entry therefore has exactly the two halves `subscriber_remote_t` already
holds — `return_route` (durable) and `reverse_route` (cache) — and the durable-subscription design
takes its shape from here rather than the other way round (§13.3).

## 8. Invalidation

### 8.1 The generation is the entire mechanism

**Normative.** A PAIR stops validating when its vertex's generation moves. The generation moves on
**retirement** (`vertex_t::revert_to_placeholder`, `core/include/libtracer/vertex.hpp`, bump-before-teardown
per ADR-0062) and on the **tenancy and session events of §8.2**. It saturates and never wraps; a
saturated vertex is permanently unbindable and every mint for it falls back to the string
(RFC-0024 §4.4 rule 3, RFC-0027 §4.3.1 — one rule now, over one field). There is **no other**
invalidation axis: no owner check (there is no table to own a label), no per-peer stamp
(`label_peer` is deleted), no TTL. "Peer departure" is caught by three guards the inventory's §3
lists, all already shipped for the pair: the connection vertex's generation, the child tombstone
(`remove_child` tombstones the registry entry; `child_registry_t::by_conn_slot` tests `live()`, then answers null and the egress
drops), and the session anchor's retire for bus sessions (`fwd_router_t::bus_peer_down`, `core/src/fwd_router.cpp`).

### 8.2 The open fact, verified: a same-named re-add does NOT bump today, and MUST

**What the code does at head (`d390c680`).**

- `fwd_router_t::remove_child` (`core/src/fwd_router.cpp`) tombstones the child's registry
  slot and its receive ctx, releases the RFC-0027 label slot (`release_child_label`), clears the
  interned link token and evicts the link's subscription edges (`link_down` →
  `graph_t::evict_link_edges`, `core/src/graph.cpp`). **It does not retire, and does not
  otherwise touch, the connection vertex.** The vertex `/net/<module>/<name>` is owned by whoever
  registered it; the router only *finds* it (`graph_.find(ctx.mount_tlv)` in
  `fwd_router_t::add_child`, `core/src/fwd_router.cpp`).
- `fwd_router_t::add_child` of the same name reuses the tombstoned ctx (`fwd_router_t::acquire_ctx`,
  `core/src/fwd_router.cpp`), rewinds `conn_slot` to `kNoConnSlot`, and **re-resolves it against
  the same vertex** — same index, and, since nothing retired it, **the same generation**.
- The only thing that advances a vertex's generation is `revert_to_placeholder`, reached through
  `graph_t::retire` (`core/src/graph.cpp`). The RFC-0014 transport-vertex lifecycle *does*
  retire the connection vertex — after `remove_child`, in the same teardown transaction
  (`transport_vertex_t::ctl_txn_t::discharge`, `core/src/transport_vertex.cpp`) — so a
  hard `NAME` write that destroys and recreates a connection gets a bump **from the embedder's
  transaction, not from the router**. A `remove_child`/`add_child` pair through the router's own
  door (tests, SDK hosts, an embedder that keeps its connection vertices and rewires transports
  under them) gets none.
- RFC-0027 covered this window with a **second stamp**: `acquire_ctx` advances `label_peer` per
  registration so a label minted for the previous tenancy fails the table's owner check
  (`fwd_router_t::acquire_ctx`, `core/src/fwd_router.cpp`: "a departed tenancy and a departed vertex are different
  departures and each gets its own stamp"). That stamp is deleted with the table (§12.3).

**Why it matters more for a chain than for a single element.** The hop's element names its own
connection vertex, which under a same-named re-add still is "the connection to whatever is behind
`<name>`" — for *that one element*, resolving to the new tenancy is arguably what the string would
also do. But the elements **after** it were minted by the node that was behind the old tenancy.
A different node behind the same name has its own index space, and two fresh nodes issue
colliding pairs by construction (dense indices from 0, generations from 0). The hop's element is
the only place the chain can be cut before its tail is read against the wrong node, and §6.4's gate
cannot tell the two peers apart — the vertex and its ACL are the same. A second stamp is exactly
what RFC-0027 concluded it needed for this; with one stamp, that stamp must move.

**Rule adopted — the generation MUST bump.** A same-named re-add is a **new identity** for the pair
form; a pair issued against the previous tenancy MUST NOT resolve to it. Normatively:

1. A host MUST advance the generation of a point-to-point child's connection vertex **whenever the
   child's tenancy changes** (`remove_child`, whether or not the embedder retires the vertex
   afterwards). The reference implementation does this in `remove_child` itself, so the guarantee
   does not depend on the embedder's transaction order.
2. A host MUST also advance it **when the child's link goes down while the tenancy is kept**
   (`link_down` on a self-healing or dormant link). The far node may have rebooted behind the same
   socket, and a rebooted node re-issues the same small pairs; nothing in the frame can reveal that.
   The cost is one re-learn round trip per reconnect, which ruling 4 already prices in ("re-learned
   after … departure"). `bus_peer_down` already does exactly this for a session anchor — "Retire
   BEFORE the eviction. Retirement is what bumps the saturating generation"
   (`fwd_router_t::bus_peer_down`, `core/src/fwd_router.cpp`) — so the point-to-point arm adopts the bus arm's rule.
3. The bump MUST NOT re-virginize the vertex. `revert_to_placeholder` empties `:acl`, `:settings`
   and the app fields (`vertex_t::revert_to_placeholder`, `core/include/libtracer/vertex.hpp`), which is right for retirement
   and wrong for a connection vertex whose owner keeps it. The reference implementation adds a
   **generation-only door** on `graph_t` (working name `restamp(vertex_handle_t)`): the same
   saturating CAS as `revert_to_placeholder`'s, under the map lock, and nothing else. A vertex the
   embedder then retires bumps twice; on a saturating `u32` that is free.
4. **Per-boot epoch, for transports that cannot report a session boundary** (ruled 2026-09-30,
   §16 Q3). Rule 2 fires on `link_down`; a connectionless transport (UDP, CAN) never reports one,
   so a far node can reboot behind a kept tenancy unseen. Such a link carries the far node's
   **per-boot epoch** — a value that changes on every boot — **once per session or advertise, never
   per frame**; a host that observes a changed epoch treats it as a session boundary and applies
   rule 2's bump. The epoch is **not** part of the pair: generations still only move forward within
   a boot and the element stays `(u32, u32)`. Its wire spelling lands with S4.

**Rejected: "same-named re-add is the same identity, old pairs may resolve to it."** It is
consistent with ruling 4 for the hop's own element and inconsistent with it for every element after
that one, for the reason above; it would also make the pair form the only spelling that can reach a
peer's successor without any party having spelled the successor's name. The doc set closes the
mis-delivery class by construction and this would reopen it at the seam RFC-0027 built a whole stamp
for.

### 8.3 Exhaustion

**Normative.** There is no table, so there is no ceiling, no census and no refuse-on-full. The
only refusal a host can issue for a PAIR is a **saturated slot** (2³² retirements of one vertex),
which is permanent and answered by leaving that part as NAMEs (§4.2). RFC-0027 §8.3's ceilings,
`max_per_peer`, `max_peers`, `refused_mints`, `retired_slots`, the injected store and §10's peer
substrate for the ceiling are deleted (§12.3). RFC-0024 §6.4's node-local vertex index is the one
structure the form needs, and it is already paid for by every build since the routing car (4 B per
vertex on rv32, 8 B on a host; sized by the graph, not by traffic).

## 9. Statelessness, defined, and its one exception

### 9.1 Definition (ruling 5)

**Normative.** A forwarding hop is **stateless** when it holds **no hard state**: nothing whose
loss, refusal or absence changes the answer of any operation. Soft state — a cache whose miss falls
through to the canonical form and yields the same result — is permitted. **Per-request state is
not permitted at a hop, ever**, whatever its size or its recovery story; the originator holds the
request open and owns its deadline (RFC-0004 §A, and the [#1629](https://github.com/avatarsd-llc/libtracer/pull/1629)
ruling). Under this definition the path plane is stateless without qualification: the origin's
cached chain, the edge's `reverse_route`, and a local edge's binding are all soft; RFC-0027's table
was hard (§2.3), and it is gone. The qualifying admonitions RFC-0027 §11.4 placed on
`docs/reference/00-overview.md`, `02-graph-model.md` and `05-protocol-tlvs.md`, and in ADR-0038 /
ADR-0040, are replaced by this definition (§12.5).

### 9.2 The exception (ruling 6): RFC-0004 §E.1 `COMPACT`

**Normative.** RFC-0004 §E.1's route handle — `ADVERTISE` / `COMPACT` / `HANDLE_NACK`, a per-link
`u16` swapped at each hop — is the **single named exception** to §9.1, and only for the flows that
opt into it (`SUBSCRIBER.delivery_compact`). A `COMPACT` frame carries **no route**, so a hop's
ingress binding is hard state: lose it and deliveries stop until the producer re-advertises. It is
kept because it is what makes a high-rate stream affordable — about **10 B** of framing per sample
against about **30 B** for the one-element delivery of §7.1 (RFC-0024 §2.1's table) — and it is
admissible because it is **recoverable by construction, never wrong**: a lost or stale handle draws
`HANDLE_NACK` and a re-advertise (RFC-0004 §E.1's self-heal, including its cross-link clearing
rule), and a `COMPACT` can never be delivered to the wrong route because it names no route at all.
It applies to **streams only**: a one-shot op, a cold subscription and every request/reply pay the
full route, exactly as §E.1 already says. Nothing in this RFC changes §E.1's frames or rules.

## 10. Shared (multi-peer / bus) mounts

**Normative, until §13.2 S8 lands.** A PAIR MUST NOT dereference to a shared-mount vertex as an
egress (`bound_egress`'s `multi_peer` refusal, `fwd_router_t::bound_egress`, `core/src/fwd_router.cpp`; RFC-0020 §3: a bus
link's own NAME is not a routable next-hop, and its `send()` broadcasts). A hop relaying a **reply**
that arrived from a bus peer contributes its **string** part per §6.2 — the mount run plus the peer
segment — so the origin's chain toward anything behind a shared mount is mixed: PAIRs up to the
bus, NAMEs across it. This is the second essential divergence of §2.2 and it stays until an
**accepted session's anchor vertex** (`graph_t::register_session_anchor`, `core/src/graph.cpp`; the
#1254 anchor `reverse_hop_ref` already uses in the other direction) can be egressed **through** — a
directed per-peer send keyed by the anchor's slot, which is [#741](https://github.com/avatarsd-llc/libtracer/issues/741)'s
ruled boundary ("reject, never broadcast") given a door. That slice (S8) is in this RFC's scope.
Announce-census peers (CAN) create no vertex (ADR-0044 amendment) and stay NAMEs by construction.

## 11. What is deleted

> **Corrected 2026-10-02 by the citation erratum ([#1701](https://github.com/avatarsd-llc/libtracer/issues/1701)) — see §Erratum at the end of this
> document.** The third row no longer lists `on_stale_label`: it is the delivery-compaction
> (RFC-0004 §E.1 `COMPACT`) stale-label observer, which §9.2 keeps.

| item | where | measured cost recovered |
| --- | --- | --- |
| `path_label_table_t` and its slot store, ceilings, census | `core/include/libtracer/path_label_table.hpp`, `core/src/path_label_table.cpp` | **24 B/slot** RAM (60 B rv32 default, 304 B peak at 8 mints); **+3 388 B flash** rv32 for the TU (RFC-0027 §12.4 clause 5, §14) |
| `path_label_t` (16/16), `kPathLabelMaxGeneration`, the `len = 4` escape encoder/decoder | `core/include/libtracer/path_label.hpp` | in the TU figure above |
| `route_label_forward`, `label_src_prefix`, `child_label_record`, `terminus_label_record`, `release_child_label`, `configure_path_labels` (**not** `on_stale_label` — see the note below the table) | `fwd_router_t` members in `core/include/libtracer/fwd_router.hpp` / `core/src/fwd_router.cpp`; the `child_rx_ctx_t` fields `path_label`, `path_label_for`, `terminus_label`, `label_peer`; and `fwd_router_t::next_label_peer_bits` | the origin car's **+2 276 B `.text`** in `fwd_router.cpp` (RFC-0027 §12.4), plus the forwarder car's share (unmeasured; the symbol ratchet prices it in S3) |
| `adopt_path_label`, `label_dispatch`, `fall_back_on_label_refusal` | `core/include/libtracer/fwd_router.hpp` | in the origin-car figure |
| `op` bit 7 (`kFwdOpFlagMintRequest`, `parsed_fwd_t::mint_request`), the trailing reply `PATH_REF` (`mint_fn`) and erratum 1's strip logic in `rebuild_fwd_forward` | `kFwdOpFlagMintRequest` in `core/include/libtracer/op_resolve.hpp`; `parsed_fwd_t` / `parse_fwd` in `core/src/op_resolve_walk.hpp`; `mint_fn` in `fwd_router_t::route_fwd_forward`, `core/src/fwd_router.cpp`; `rebuild_fwd_forward` in `core/include/libtracer/fwd_frame_view.hpp` | unmeasured; small |
| `0x14` `PATH_REF` as a `dst` type: `route_bound_forward`'s type dispatch, `dst_bound` | `fwd_router_t::route_bound_forward`, `core/src/fwd_router.cpp`; `parsed_fwd_t::dst_bound`, `core/src/op_resolve_walk.hpp` | unmeasured; the element parser is kept for `0x15` |
| the pointer `vertex_handle_t` and `target_binding_t` as a separate cache record | `core/include/libtracer/graph.hpp`, `core/include/libtracer/subscriber.hpp` | none — a substitution (ruling 2), bench-gated |

**Not deleted: `fwd_router_t::on_stale_label`.** Despite its name it is not part of RFC-0027's
label plane. It is the observer for a dropped stale or unknown **delivery-compaction** handle — a
`COMPACT` frame whose RFC-0004 §E.1 route handle has no ingress binding on its link, answered with
`HANDLE_NACK`. §9.2 keeps that mechanism as the single named exception, so its observer stays.

**Total flash on rv32, from the measured parts alone: about −5.6 KB** (3 388 + 2 276), and
**zero table RAM**. The pair form adds no RAM: the vertex index it needs already exists. Bytes on
the wire: +3 B per hop over the bare `PATH_REF` it replaces, +4 B over the label (§5.2).
`path_t::cache_path_label` (`core/include/libtracer/path.hpp`) is **generalised** to the 8-byte
element rather than deleted — it is the origin-side cache §6.2 fills.

**What is added.** §6.2's "never nothing" rule has a wire cost that the deletions above do not
offset:

| item | where | cost (estimated; **measured at S2**) |
| --- | --- | --- |
| reply-`src` growth at a **non-minting** relaying hop: its inbound mount run as NAMEs, where RFC-0027 erratum 2 had it contribute nothing | every `REPLY` relayed over a shared mount, a retired connection vertex, or a saturated slot (§6.2) | one NAME run per such hop, **≈13–32 B** (one `1 + len` segment record per mount-run segment; 13 B for the shortest shipped `net/<module>/<name>` run, up to §5.2's 20–32 B canonical figure) |
| reply-`src` growth at a **minting** hop | every other relayed or issued `REPLY` | **11 B** per hop (one PAIR element, §5.2) |

Both are paid on the reply leg only. The `reply-spread` four-link arm (S2's gate, §15 clause 1)
prices them against today's replies.

## 12. Spec text changes

### 12.1 `docs/spec/v1.md` §3

- The `0x06` bullet: `kind = 0x16` carries `len = 8`, `(u32 index, u32 generation)` LE; the
  "reserved for the label element of RFC-0027" clause becomes "the path element of RFC-0029".
- The `0x14` bullet is **deleted**; `0x15` is kept as a type code, its body re-spelled as a `PATH`
  of path elements and its role restated as §7.1.
- The routing-semantics passage: delete the `op` bit 7 mint request, the "hop forwarding a reply
  that carries a mint answer MUST either prepend or strip" clause, and the multi-element
  silent-drop scope of §5.3 erratum 4; add §6 (per-hop algorithm), §6.2 (reply `src` carries the
  forward route, NAME or PAIR, never nothing), §6.3 (`NOT_FOUND` on every arm), §6.4
  (spelling-independent authorization), §8.2 (tenancy/session bump, and rule 4's per-boot epoch).
- The "§`0x06` §path label element" incorporation paragraph is replaced by one incorporating this
  RFC's §§4–10: one element kind, node-scoped, address-never-capability, passive learning on the
  reply `src`, `NOT_FOUND` fallback, saturate-never-wrap, no withdraw/lease/TTL.
- §3.1 (static path-handle conformance) is **unchanged**: canonical keys carry no escape record.

### 12.2 RFC-0024

Kept as normative and cited from here: §4.4 (the element), §5 (validation), §6 (ACL), §6.4 (the
index), §7.1 erratum 3 (`PATH_REF_REVERSE`'s re-heading) and amendment 2's `0x15` type code.
**Superseded**: §4.1's `0x14` type as an address form, §7.1 steps 1–4, erratum 1 (for both lists),
amendments 1–2's bare-array body for `0x15` (now a `PATH`, §7.1), §7.5 (bit 7 and the
forward-list ledger), §5.3 erratum 4's silent-drop arm, §9.3's flag clause (the mask stays). §2.1's
"no hop holds anything" is **restored as the property of the one form**. A status-row note on
RFC-0024 records this at acceptance.

### 12.3 RFC-0027

**Superseded as a form.** Withdrawn: §4 (the 16/16 label), §4.3.1's per-slot retirement (the
vertex's generation is the only stamp), §5.3.2 amendment 5's `len = 4` spelling (widened to 8),
§8.1's post-auth lambda (moot), §8.3 (table, ceilings, refuse-on-full), §8.4's owner-check
argument, §9 (local IO out of scope — ruling 2), §10 (the peer-handle substrate for the ceiling),
§11.1 collision 3 (the knowing surrender of statelessness — withdrawn), §11.4's qualifying
admonitions, §12.4's bench gate for the table. **Carried into this RFC, verbatim or by citation:**
§5.1 (per-element tag), §5.2 (mixed paths), §5.3.3 amendment 6 (one element per whole local part),
§6.1 with errata 2 and 4 (passive mint on the reply `src`), §6.2 (first-fire learn), §6.3 (a mint
is never load-bearing), §7.2 (`NOT_FOUND`, string fallback), §7.3 (no withdraw, no aging), §8.2
(ACL at the dereferenced vertex), amendment 8 (the origin adopts verbatim, stands up no table).
RFC-0027 wrote its own exit in §15 clause 5 and clause 2; this RFC is that exit.

### 12.4 RFC-0004

§A/§B (path-as-route, per-hop strip and prepend) remain the model. §B's "a reply … does not
accumulate `src`; the `src` child of a REPLY is … the responder's own endpoint" is **amended** per
§6.2: a reply's `src` is the responder's address from the origin's vantage, accumulated head-first
by every host on the way back, in PAIRs where they can be issued and NAMEs where they cannot. §F is
reaffirmed and made spelling-independent (§6.4). §D, §E and §E.1 are untouched (§9.2).

### 12.5 Reference pages, ADRs, `CONTEXT.md`

- `docs/reference/00-overview.md` claim 4's "Statelessness, qualified" admonition,
  `02-graph-model.md`'s qualifier and `05-protocol-tlvs.md`'s §`0x06`/§`0x14` sections are replaced
  by §9.1's definition and §6's algorithm; ADR-0038 / ADR-0040's RFC-0027 qualifications revert to
  the unqualified claim under the §9.1 definition, with §9.2 as the named exception.
- `docs/reference/03-addressing.md`'s "two forms" rule becomes **one form, two element kinds**.
- `docs/reference/04-communication-flows.md` §delivery: §7's first-fire learn and the unconditional
  reverse list.
- `docs/modules/fwd-router.md`: the hop algorithm of §6; the tenancy/session bump of §8.2.
- `CONTEXT.md`: the **Bound path**, **Vertex ref** and **Path label** entries merge into one
  **Path element** entry (NAME | PAIR) with the pair's properties; the **Path element** entry's
  "LABEL" kind is renamed PAIR; **Composite TLV**'s "all three with `opt.PL=0`" becomes two;
  `vertex_handle_t` is documented as the pair. Every _Avoid_ line that distinguishes vref from path
  label collapses to "bare `label` still means RFC-0004 §E.1's per-link `u16`".

## 13. Slice plan

### 13.1 Principles

Each slice is one PR, independently green and bench-gated where it touches a measured path, in the
RFC-0024 §8 / #807 A/B discipline (16 interleaved pairs, both arms collated, first round discarded).
Per ruling 10 no slice carries a compatibility shim. Symbol and flash move through the ratchet
(`symbol_ratchet.json`, `footprint-cortexm0.yml`), which is where the unmeasured rows of §11 get
their number.

### 13.2 Slices

| # | slice | contents | gate |
| --- | --- | --- | --- |
| **S0** | spec | this RFC accepted; §12's text lands (`v1.md`, the reference pages, `CONTEXT.md`, status rows on 0024/0027) | doc gates |
| **S1** | element + hop arm | `kind = 0x16, len = 8` encoder/decoder; §6's PAIR arm in `route_fwd_ingress` for a `PATH`-spelled `dst` (deref → egress / terminus / refuse); terminus applies `apply_op`; `NOT_FOUND` on every refusal; `0x14` refused as a `dst` | conformance vectors (§13.4); `bench_hop_chain` and `bench_forward_*` level |
| **S2** | learning | §6.2 reply-`src` accumulation (PAIR or NAME run, never nothing) at hop, terminus and origin; origin adoption into `path_t` (generalised `cache_path_label`), string fallback on `NOT_FOUND`; delete bit 7, `mint_request`, the trailing reply list, `route_bound_forward` | `reply-spread` four-link arm inside the A/A null band (RFC-0024 §8.2 clause 3); round-trip test replacing `path_label_origin_test` |
| **S3** | delete the table | every §11 row for RFC-0027; `label_peer` and `next_label_peer_bits`; **not** `on_stale_label`, the delivery-compaction observer §9.2 keeps; supersedes #1668, #1669, #1647 (§13.3) | ratchet shows the §11 deltas; `bench_path_label` retired |
| **S4** | the bump (§8.2) | `graph_t::restamp`; `remove_child` and point-to-point `link_down` call it; rule 4's per-boot epoch on UDP/CAN links (once per session or advertise); tests: remove + same-name re-add refuses the old pair; link loss + re-up refuses the old pair; the RFC-0014 teardown still works with the double bump | `bound_forward_test` extended; no hot-path change |
| **S5** | subscriptions (§7) | `PATH_REF_REVERSE` unconditional (no flag) and spelled as a `PATH`; first-fire learn for mount-routed (`graph_t::subscribe_wire`) and `subscribe_toward` edges; the node's one learn endpoint, edge named in the tail; lands after #1533; `deliver_remote` unconditional chain-first | `fwd_two_mount_test`, `bound_forward_test` delivery arms; `bench_compact_delivery` level |
| **S6** | one lookup, one gate (ruling 9) | a NAME-spelled prefix resolves by the tree walk to the connection vertex and `child_registry_t::by_conn_slot` yields the egress; the registry keeps egress state only; **one `allows` site** for both arms of §6 step 3 | `bench_mount_resolve` A/B: the tree walk must not lose to `longest_prefix`'s 25 ns/pass at W = 12, N = 64, or S6 keeps the registry index as an *accelerator* of the same lookup |
| **S7** | host API (ruling 2) | `vertex_handle_t` = `(u32, u32)` by value; `target_binding_t` folds into it; `graph_t` entries take the pair; the bench decides the ISR pointer fast door (compile-time option if kept) | local read/write/await A/B against the pointer handle — the 11 ns/op is the priced cost; a regression beyond it re-opens ruling 2 (§15 clause 3) |
| **S8** | shared mounts (§10, ruling 8) | egress **through** a session anchor: a directed per-peer send keyed on the anchor's slot; `bound_egress` admits an anchor; `reverse_hop_ref`'s anchor becomes bidirectional | ws-server multi-peer test; `bench_forward_demux` level |

S1→S2→S3 are ordered; S4 and S5 depend on S1 only; S6, S7, S8 are independent of each other and
follow S2. An embedder that enables none of the three optional mechanisms today pays nothing
until it opts into spelling chains.

### 13.3 Ordering against other work

**Ruled 2026-09-30 (acceptance):**

- **Release.** v0.17.0 is cut after [#1670](https://github.com/avatarsd-llc/libtracer/issues/1670)
  and does not carry this RFC; **RFC-0029 ships as v0.18.0**.
- **Superseded by S3.** [#1668](https://github.com/avatarsd-llc/libtracer/issues/1668) (label plane
  compile-time), [#1669](https://github.com/avatarsd-llc/libtracer/issues/1669) (`label_resolves_`
  counter) and [#1647](https://github.com/avatarsd-llc/libtracer/issues/1647) (unbounded label
  default) are closed as superseded: S3 deletes the table they tune.
- **RFC-0028 slice 8** ([#1621](https://github.com/avatarsd-llc/libtracer/issues/1621),
  [#1622](https://github.com/avatarsd-llc/libtracer/issues/1622)) is **unheld** and proceeds
  independently. The 32-bit write sequence on narrow targets is decided there, with modular
  (serial-number) comparison for wrap; nothing in this RFC constrains it.
- **[#1533](https://github.com/avatarsd-llc/libtracer/issues/1533) lands before S5** (below).

**Contacts:**

- **RFC-0028** ([#1627](https://github.com/avatarsd-llc/libtracer/pull/1627), merged; slices [#1628](https://github.com/avatarsd-llc/libtracer/pull/1628),
  [#1629](https://github.com/avatarsd-llc/libtracer/pull/1629) merged; [#1630](https://github.com/avatarsd-llc/libtracer/issues/1630) open)
  is the value path — LKV slot, fan-out blocks, tx handoff — and is wire-neutral. **No slice here
  blocks or is blocked by one there.** Two contacts, both constraints rather than dependencies:
  #1630 item 1 ("bound the other forwards") MUST stay the originator's deadline, never a hop table
  (§9.1); and RFC-0028's per-vertex retention does not touch `subscriber_remote_t`, so §7.3's entry
  shape does not collide.
- **lt#1533** (subscriber-list efficiency: enable bits or partitioning of disabled subscribers)
  touches the fan-out over the same `subscriber_t` records S5 and S7 change. **#1533 lands before
  S5** (ruled); S5 must not re-derive the edge layout #1533 settles.
- **lt#1623** (durable, enable-able subscriptions) **waits for S5**: its entry is §7.3's — string
  plus enable state persisted, chain never — and its re-arm is §7.2's first-fire learn.
- **RFC-0024 / RFC-0027 acceptance trains** ([#809](https://github.com/avatarsd-llc/libtracer/issues/809),
  [#1325](https://github.com/avatarsd-llc/libtracer/issues/1325)) are closed; nothing in flight there.

### 13.4 Conformance vectors

New under `tests/vectors/` (names indicative): `path/element-pair-encode`, `path/element-pair-bad-len`
(refuses the address, not the frame), `fwd/pair-hop-egress`, `fwd/pair-terminus`,
`fwd/pair-stale-generation-not-found`, `fwd/pair-last-element-connection-vertex-is-facet`,
`fwd/reply-src-accumulates-forward-route` (PAIR arm and NAME-fallback arm),
`fwd/subscribe-reverse-list-unflagged`, `fwd/subscribe-reverse-path-name-fallback`, `fwd/delivery-first-fire-requests-reply`,
`fwd/bus-mount-refuses-pair`. **Retired**: every `PATH_REF`-as-`dst` vector, the bit-7 mint vectors,
the trailing-reply-list vectors, RFC-0027 §12.5's label vectors. The `fwd/fwd-reply-*` vectors keep
their `dst` bytes and gain the §6.2 `src`.

## 14. Alternatives considered

- **Keep RFC-0027's label as the wire spelling and delete only the table** (a 4-byte label that is
  an *index into the vertex index*). It saves 4 B/hop but caps a node at 65 536 vertices and 65 535
  retirements per slot on the wire; the pair's `u32` × 2 derivation (RFC-0024 §4.4) exists precisely
  so neither cap is real. Rejected by ruling 1.
- **Keep `0x14` as the "all-PAIR" spelling beside the in-`PATH` element** (saves 3 B/hop when every
  hop mints). Two spellings of one address is the class §2.2 removes; a body that is all escape
  records is well-formed `PATH` already. Rejected — one spelling.
- **A hop that cannot mint contributes nothing to the reply `src`** (RFC-0027 erratum 2's letter).
  Leaves the origin a route that skips a hop; the origin would need to detect the skip and stay
  canonical, which is a second rule for one case. Rejected in favour of "NAME run, never nothing".
- **Same-named re-add keeps the identity** (§8.2, rejected there).
- **A per-boot epoch *in the generation*** (seed each node's generations from a boot counter) as an
  *alternative* to §8.2 rule 2's bump on link loss. It protects against the far-end reboot without a
  round trip, but it costs a persisted counter or a random seed with a collision story, and it
  weakens "generations only move forward" across boots to a probabilistic claim. **Rejected in that
  form.** What is **adopted** (§16 Q3, ruled 2026-09-30) is the epoch as a *complement* to rule 2,
  outside the pair: §8.2 rule 4 carries it once per session or advertise on transports that cannot
  report a session boundary (UDP, CAN), and a changed epoch triggers rule 2's bump.
- **A per-edge reply endpoint for the first-fire learn** (one segment per edge). Simpler
  correlation, but RAM × edges on the targets least able to pay it. Rejected (§7.2, §16 Q2).
- **Pointer `vertex_handle_t` kept, pair only on the wire** (the inventory's recommendation).
  Ruled otherwise (ruling 2); the cost is priced and gated in S7.
- **A fresh escape kind for the 8-byte pair** so a retired 7-byte record reads as "retired" rather
  than "malformed". Rejected (§16 Q4): ruling 10 owes the retired form no diagnostic.

## 15. What would falsify this RFC

1. **A measured regression on any shipped shape** under the RFC-0024 §8.2 protocol — the
   `reply-spread` four-link arm in particular (S2's gate). Same death as #504's memo.
2. **A reachable sequence lets a stale PAIR validate** other than through a saturated slot. §8.2's
   bump rules are the whole guard for tenancy and session changes; if a path to false validation
   survives them, the element needs a further stamp and ruling 1's "one primitive" needs re-ruling.
3. **The pair-by-value handle costs more than the priced 11 ns on the local hot path** (S7's gate),
   or breaks an ISR-context write (`docs/reference/00-overview.md` claim 6). Then ruling 2 is re-opened
   with the measurement.
4. **The tree walk cannot match `longest_prefix` at realistic widths** (S6's gate). Then the
   registry index stays as an accelerator and ruling 9's first item is narrowed to "one lookup
   semantics, two indexes".
5. **The first-fire learn (§7.2) proves unaffordable** on a board with many edges reconnecting at
   once — one reply per edge per reconnect. That would be a measurement on the HIL, and the answer
   would be a batched learn, not a table.

## 16. Questions ruled at acceptance

All five questions the draft left open were **ruled by the maintainer on 2026-09-30** on
[PR #1634](https://github.com/avatarsd-llc/libtracer/pull/1634), each as the draft recommended.

1. **`PATH_REF_REVERSE` as a `PATH` too? — RULED yes.** `0x15`'s body is a `PATH` of PAIR/NAME
   elements. It costs +3 B per element on the subscribe request leg only, and it removes the last
   positional list and the last strip rule. Incorporated in §5.3, §7.1 and §12.1–§12.2.
2. **The edge's reply endpoint for the first-fire learn — RULED one per node.** One node-local
   endpoint per node, the edge identified in the reply's `dst` tail; no per-edge segment, because its
   RAM × edges cost lands on narrow targets. Incorporated in §7.2 and §13.2 S5.
3. **Per-boot epoch — RULED adopted as a complement** to §8.2 rule 2, for transports that cannot
   report a session boundary (UDP, CAN). Carried once per session or advertise, never per frame; not
   part of the pair. Incorporated as §8.2 rule 4, §13.2 S4 and §14.
4. **`kind = 0x16` or a fresh kind — RULED reuse `kind = 0x16` with `len = 8`.** Incorporated in
   §5.1 and §14.
5. **`vertex_handle_t` representation — RULED `(u32, u32)` by value.** The S7 bench decides the ISR
   pointer fast door; if it is kept, it is a compile-time option (compile-time-by-default doctrine).
   Incorporated in §4.3 and §13.2 S7.

## 17. Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the comment window is waived by default while
the project is solo-maintained and is not invoked here. Sustained objections and their resolution
are recorded in this section as they arrive.

- **Open, for the S2 review (non-normative note):** the reference implementation's local origination ([#1645](https://github.com/avatarsd-llc/libtracer/issues/1645), `fwd_router_t::originate`) spells each request's return route as one `~o<hex>` NAME, so that prefix is taken on the origin side; origin learning (§6.2) must not assume it is free.

## Erratum (2026-10-02) — the code citations name symbols, and §11's third row keeps the delivery-compaction stale-label observer ([#1701](https://github.com/avatarsd-llc/libtracer/issues/1701))

**What the text said.** Two things, both about the reference implementation rather than the wire:

- **Every `file:line` code citation** across §§2–13 — for example `graph_t::find_ptr` at
  `core/src/graph.cpp:1755`, `hop_mint` at `core/src/fwd_router.cpp:1333`, the vertex index
  `std::deque` at `core/src/graph.cpp:1027-1060`, and §11's `core/src/fwd_router.cpp:1638`, `:2388`.
- **§11's third deletion row** — which S3 (§13.2) deletes in full — listed `on_stale_label` beside
  the RFC-0027 label-plane members.

**What was wrong.**

- The citations did not resolve to the code they described, and not because the tree moved after
  acceptance: they were already wrong at the authoring commit. Several named the wrong file as well
  as the wrong line — the vertex index is the `graph_t::vertex_slots_` member in
  `core/include/libtracer/graph.hpp`, not code in `graph.cpp`; the bit-7 flag constant lives in
  `core/include/libtracer/op_resolve.hpp`; and `rebuild_fwd_forward`'s strip logic lives in
  `core/include/libtracer/fwd_frame_view.hpp`. A "valid as of SHA" note cannot repair a citation
  that was never valid, and `docs/spec/` is deliberately outside the citation gate
  (`tools/check_doc_citations.py`), so line numbers here rot unchecked.
- `on_stale_label` is not an RFC-0027 member. It is the observer for a `COMPACT` frame whose
  RFC-0004 §E.1 route handle has no ingress binding on its link — the delivery-compaction
  mechanism that §9.2 keeps as the single named exception. Deleting it with the table would
  contradict §9.2.

**The correction.**

| | corrected reading |
| --- | --- |
| **code citations** | every citation names the **symbol** and its **file** (`fwd_router_t::hop_mint`, `core/src/fwd_router.cpp`), with no line number. Each was re-checked against `main` on 2026-10-02 to name the code its sentence describes; the wrong-file ones above now name the right file. The prose around them is unchanged. |
| **§11, third row** | lists `route_label_forward`, `label_src_prefix`, `child_label_record`, `terminus_label_record`, `release_child_label` and `configure_path_labels`, the `child_rx_ctx_t` fields `path_label`, `path_label_for`, `terminus_label` and `label_peer`, and `fwd_router_t::next_label_peer_bits`. **`on_stale_label` is removed** and a note under the table says why. |
| **§13.2 S3** | states that `on_stale_label` is **not** deleted. |

**Instrument: erratum, not amendment** ([GOVERNANCE.md](../../../.github/GOVERNANCE.md)). **No
wire surface moves.** No frame, TLV type, escape kind, flag bit, grammar, error identity or
conformance vector changes, and no normative statement of §§4–10 changes. The citations are
informative pointers into the reference implementation. Removing `on_stale_label` from §11 makes
the deletion list agree with §9.2's normative text, which already kept the mechanism.

## Erratum (2026-10-09) — S6 landed: the door lookup is `child_registry_t::by_conn_slot`, the PAIR arm takes no lock, and the registry stays as the accelerator ([#1939](https://github.com/avatarsd-llc/libtracer/issues/1939))

**What the text said.**

- §6 step 3, §8.1 and §13.2's S6 row named `ctx_by_conn_slot` as the lookup from a slot to the
  child whose connection vertex it is.
- §8.1 said a removed child's `conn_slot` is reset to `kNoConnSlot`, and §4.1 called the vertex
  index a `std::deque`.
- §13.2's S6 row and §15 clause 4 left open whether the tree walk or the registry's
  `longest_prefix` resolves a NAME-spelled mount.

**What was wrong.** S6 changed the reference implementation underneath the text.
`fwd_router_t::ctx_by_conn_slot` is deleted. The slot now sits on the registry entry
(`child_registry_t::child_t::conn_slot`, recorded for point-to-point and bus mounts alike), and
`child_registry_t::by_conn_slot` finds the door from it. The vertex index is now an append-only
chunked array whose size is published atomically (ADR-0063's pattern). So `deref_vertex_slot`, the
honouring side of a PAIR, takes no lock: it reads the generation on both sides of the
registration test instead. A door's ACL lookup, `graph_t::registered_vertex_at`, takes none
either, because it holds a slot and issues no generation. The **mint**, `vertex_slot_at`, keeps
the shared lock, so a mint sees a whole retire or none of it whichever order the retire
clears the registration and bumps the generation in. A lock-free mint that read the bumped
generation off a vertex still flagged registered would issue the successor tenant's generation
(#603).

**The correction.**

| | corrected reading |
| --- | --- |
| **§4.1** | the index is the chunked, lock-free-to-read `graph_t::vertex_slots_`. |
| **§6 step 3, §8.1, §13.2 S6** | the door lookup is `child_registry_t::by_conn_slot`. §8.1's tombstone is the registry entry's `live()` test; `conn_slot` itself is left alone on `remove_child`. |
| **S6 outcome** | **§15 clause 4 fired.** In `bench_mount_resolve`'s lookup table, even the floor of a tree walk (one keyed `graph_t::find` of the exact mount) loses to `longest_prefix` at S6's gate cell: at W = 12, N = 64 it takes 82 ns against 68 ns. At N ≤ 8 it loses by 4–5× at every width. It wins only at W ≤ 3, N = 64. So the registry index stays as the NAME spelling's accelerator, and ruling 9's first item reads "one lookup semantics, two indexes". What is ONE, as §6.4 requires, is the door and the gate: the NAME descent, a PAIR hop and a PAIR naming a session anchor all end on the registry entry whose `conn_slot` is the connection vertex, and all are decided by `fwd_router_t::door_allows`, the router's only `graph_t::allows` call. |

**Instrument: erratum, not amendment** ([GOVERNANCE.md](../../../.github/GOVERNANCE.md)). **No
wire surface moves.** No frame, TLV type, escape kind, flag bit, grammar, error identity or
conformance vector changes. The symbol names and the lock note are informative pointers into the reference implementation. §6.2's price of a mint, a shared lock, still
stands. The S6 outcome is the branch §15 clause 4 already wrote down, recorded
as taken. §6.4's normative rule, that the verdict is spelling-independent, is now what the code
does, including for an opcode the build names no right for, which both spellings refuse
identically when enforcing.

## Erratum (2026-10-09) — a link cannot exist without its connection vertex: §6 step 1 names the connection vertex, not a "registered child" ([#1940](https://github.com/avatarsd-llc/libtracer/issues/1940))

**What the text said.**

- §6 step 1 resolved a NAME head by the mount descent "if the leading run names a registered
  child".
- §4.2, §6.2 and §11 listed "a connection vertex that does not exist yet" and "a child with no
  connection vertex" among the hops that cannot issue a PAIR.

**What was wrong.** The text described a registry that could hold a link with no door. The model
this RFC is written under (§6.4, and the one-walk spec #1938) has one: the connection vertex is
the attachment point, and the shipped wiring (`transport_vertex_t::make_connection`) has always
registered the vertex before its link. The reference implementation now enforces that.
`fwd_router_t::add_child` refuses, by value, a mount whose connection vertex is not registered,
and `fwd_router_t::attach_link` registers the two together for a link wired by hand. So a
"registered child" and "a mount with a connection vertex" are one set, and `kNoConnSlot` is
deleted: `child_registry_t::child_t::conn_slot` is written once, before the entry is published.

**The correction.**

| | corrected reading |
| --- | --- |
| **§6 step 1** | the descent applies "if the leading run names a **connection vertex**". |
| **§4.2, §6.2, §11** | a hop cannot issue a PAIR for a shared mount, a saturated slot or a connection vertex **retired under its link**. A vertex that does not exist has no link to issue for. |

The same wording is corrected in the normative annex
([05-protocol-tlvs.md](../../reference/05-protocol-tlvs.md) §The per-hop algorithm).

**Instrument: erratum, not amendment** ([GOVERNANCE.md](../../../.github/GOVERNANCE.md)). **No
wire surface moves.** No frame, TLV type, escape kind, flag bit, grammar, error identity or
conformance vector changes. A node with no connection vertex for a run already answered every
spelling of it as "no link": since the 2026-10-07 "refuse" ruling a vertex-less mount refused the
hop under ACL enforcement, and the connection vertex is the only identity a PAIR can name.
