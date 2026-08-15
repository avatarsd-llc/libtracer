<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0027 — Label-switched path compression: minting a per-host path label across the wire

| Field | Value |
| ---- | ---- |
| **RFC** | 0027 |
| **Title** | Label-switched path compression: minting a per-host path label across the wire |
| **Status** | **accepted** (2026-08-15; proposed 2026-08-15), maintainer-ratified; comment window waived by default per [GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window" (solo-maintainer clause) and not invoked. The design of §§4–10 was **ruled** in the 2026-08-15 grilling session; this document is its transcription in normative form, not a proposal seeking a direction. **All three §11.1 collisions the draft flagged are RESOLVED at acceptance** — see §11.1 and §16: **collision 1 (vocabulary)** is ruled **(a) qualify**, "**path label**" is ratified as a term distinct from RFC-0004 §E.1's per-link `u16` "label", carried into [RFC-0024](0024-bound-paths-node-scoped-vertex-ref-source-routing.md) §2.1 by **amendment 3**; **collision 2 (generation)** is ruled **saturate-and-retire, NOT wrap** — a slot whose 16-bit generation reaches its maximum saturates and is **retired permanently**, never reused and never wrapped, which preserves RFC-0024 §4.4 rule 3 / §9.3's "MUST saturate, never wrap" at **zero wire cost**; **collision 3 (per-hop state)** is **accepted knowingly** as a recorded decision — RFC-0027 buys per-element degradation and terminus compaction at the price of a per-hop mint table, and the doc set states that trade rather than claiming statelessness on both forms. What remains open is **only the byte layout** (§5.3, deferred pending conformance vectors in the RFC-0014 discipline) and the §12.4 bench gate, which is normative for acceptance of the **implementation**, not of this document. Spec, `CONTEXT.md` and code edits land after acceptance, car by car (§12). |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-08-15 |
| **Comment window** | waived by default while solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"); invoke explicitly if outside input is wanted. Verified: `docs/implementations.md:13` still reads `_(none yet)_`, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** This adds a new arm to the path-element encoding and a new MUST for any host that mints — `GOVERNANCE.md` §"Errata, amendments, and the comment window" names a new frame shape as amendment territory by example, and an erratum "may not alter the wire surface". |
| **Tracking issue** | filed on acceptance. The measured motivation is [#1266](https://github.com/avatarsd-llc/libtracer/issues/1266) and [#1294](https://github.com/avatarsd-llc/libtracer/issues/1294); the local precedent is [#830](https://github.com/avatarsd-llc/libtracer/issues/830). |
| **Target spec version** | v1 itself. `docs/spec/v1.md:1` reads "(DRAFT)" and `:3` reads "The wire format is not yet stable. Pin to a specific commit if you depend on this." Same route RFC-0018, RFC-0023, RFC-0024 and RFC-0026 took. |
| **Scope** | **v-NEXT.** This RFC gates no release. |
| **Descends from** | [#830](https://github.com/avatarsd-llc/libtracer/issues/830) (the local edge binding, merged — the mechanism this generalizes), [RFC-0024](0024-bound-paths-node-scoped-vertex-ref-source-routing.md) (the node-scoped `(slot, generation)` reference, accepted), [RFC-0004](0004-remote-operation-addressing.md) §E.1 (the per-link label plane, shipped) |

> **Numbering note.** 0012 was closed unmerged, 0015 was withdrawn (PR #446); neither gap is
> reusable — see [RFC-0016](0016-composed-branch-read.md) §ghost history. 0027 is the next unused
> number.

---

## 1. Summary

A libtracer address is a route spelled in names (§Path-as-route, `CONTEXT.md`), and today **every
frame re-handles those names at every hop**. A forwarder re-derives its mount split from the same
bytes it derived it from on the previous frame; a terminus re-walks the residual segment by segment;
a delivery edge re-hashes a link NAME to find its subscriber list. The route was resolved once and
is being re-resolved forever.

Locally, that is already fixed. Since [#830](https://github.com/avatarsd-llc/libtracer/issues/830) a
subscriber edge carries a **bound vertex slot** — `(index, generation)` — and delivery dereferences
it instead of walking a key: `graph_t::dispatch_edge_target` tries the bound spelling first and
falls through to `find_ptr` only when the deref refuses
(`core/src/graph.cpp:1307`, `core/src/graph.cpp:1316-1323`). The measurement that ruled that in is
§3.2's: canonical terminus resolution costs **+21.3 ns per address segment** and the deref is
**flat at 11 ns at every depth**.

**This RFC extends that binding across the wire, MPLS-style.** The first resolution of a route walks
strings exactly as it does today. As the reply comes back, **each hop mints a compact label for its
own local part of the path** and rewrites that part in the frame it relays. Subsequent frames carry
the labels, and each hop turns its label straight into a table-indexed vertex reference — no digest
fold, no segment compare, no name hash.

The shape, in seven ruled sentences:

- A **label is a 32-bit wire value**: 16-bit index, 16-bit generation. It is **host-assigned at mint
  time** — an array slot — and **never a content hash**, so it is collision-free by construction
  (§4).
- The path encoding grows a **per-ELEMENT tag: `string | label`**. **Mixed paths are legal and
  expected**: a hop that does not implement minting, or refuses it, simply leaves its part as a
  string. Hosts that don't mint don't change their part (§5).
- **Distribution is passive.** There are **no label-distribution frames**. Each forwarding hop, when
  it relays the **reply**, rewrites its own local part of `src`/`dst` from string to its minted
  label; the first reply therefore reaches the original sender with fully-minted `src` and `dst`,
  which the sender's `path_t` caches (§6).
- **Staleness is a generation bump.** Vertex departure bumps its slot's generation; a frame carrying
  a stale or unknown label answers a **`NOT_FOUND`-class error**; the sender falls back to the
  full-string path and re-mints from the next reply. **No withdraw protocol, no aging** (§7).
- **Minting is POST-AUTH only.** The label table draws from the **injected net-plane store**
  ([ADR-0079](../../adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md)'s axis),
  carries a **per-peer ceiling**, and on exhaustion **REFUSES new mints** — live labels are never
  evicted by pressure, and a refusal is invisible to correctness because the string path keeps
  working (§8). This mirrors the CAN control-map ruling: ceiling plus refuse-new.
- **Local IO is explicitly OUT of scope** (§9). The in-process fast path already meets the target
  and this RFC touches only wire ingress/egress and the forwarding plane.
- The **[#1294](https://github.com/avatarsd-llc/libtracer/issues/1294) peer handle** — the same
  `(slot, generation)` shape at the transport seam — lands first and independently. This RFC treats
  it as **prior art and available mechanism, not a dependency to define** (§10).

One sentence: **the canonical path says who; a bound path (RFC-0024) says which of the hosts you
already resolved; a label says which of the local parts *this hop* already resolved.**

## 2. What this is NOT

Stated first, because this document sits between two shipped mechanisms that both look like it.

### 2.1 It is not RFC-0004 §E.1's route handle, and does not replace it

[RFC-0004](0004-remote-operation-addressing.md) §E.1 is **delivery compaction**: a per-**link**
`u16` label that aliases an *established delivery route end to end*, swapped at every hop, advertised
in-band, self-healed by `HANDLE_NACK` → re-advertise. It ships
(`core/include/libtracer/route_handle.hpp`; `0x11`–`0x13`) and **it stays exactly as it is**.

| | **§E.1 route handle** (stays) | **path label** (this RFC) |
| --- | --- | --- |
| what it names | a per-**link** alias for a whole established delivery route | a per-**host** alias for **one element** of an address |
| granularity | the frame's whole address is replaced | per element, and elements mix freely with strings |
| direction | producer → consumer (delivery) only | any operation, either direction |
| origination | the producer **advertises** in a setup exchange | **passive**: each hop rewrites its own part on a reply it was relaying anyway |
| partial deployment | all-or-nothing per flow | **native** — a non-minting hop leaves its part a string and the rest still compacts |
| staleness | `HANDLE_NACK` → re-advertise round | `NOT_FOUND` → the sender's own string path, re-minted on the next reply |
| exhaustion | saturates per link, degrades to full route | per-peer ceiling, **refuse new**, string path unaffected |

They are complementary and they do not compete for the same traffic. §E.1 wins, and keeps winning,
on a long-lived one-way delivery stream, where 10 B flat beats any per-element scheme. This RFC
serves what §E.1 structurally cannot: **a repeated request/reply flow**, **a route that is only
partly label-capable**, and **the terminus residual** — the part of the address §E.1 compacts only
by having already compacted everything.

### 2.2 It is not RFC-0024's bound path, and does not replace it

[RFC-0024](0024-bound-paths-node-scoped-vertex-ref-source-routing.md) is **accepted** and its
`PATH_REF` (`0x14`) ships. It replaces the *whole* address with a list of **one 8-byte vref per
host**. This RFC leaves `PATH` a path and labels its **elements**.

| | **bound path (`PATH_REF`)** | **path label** (this RFC) |
| --- | --- | --- |
| unit | one element per **host** | one element per **path part**, in place |
| the form | a **second, whole-address form** beside `PATH` | a **new arm inside** the existing path-element encoding |
| activation | the origin sets a bind-request flag; hops answer on the reply (RFC-0024 §7.5) | passive; **no flag**, no request byte, nothing to ask for |
| partial routes | a hop that cannot contribute **MUST strip the whole list** (RFC-0024 §7.1 erratum 1) | a hop that cannot contribute **leaves its own element a string** and everything else stands |
| hop state | **none** — no hop holds anything | a **per-host label table**, ceilinged and injected (§8) |
| what it compacts | the mount runs, as hosts | the mount runs **and** the terminus residual |

**The honest relationship: this RFC is the cross-node generalization of #830's local edge binding,
and RFC-0024 is the cross-node generalization of the same primitive at a different granularity.**
They are two compressions of one address and they should not both be applied to one frame — §11.3
states the rule. Where this document's design and RFC-0024's *normative text* genuinely collide,
§11 flags it rather than overriding it; that is the house rule and there are **three** such
collisions, one of them load-bearing.

### 2.3 It is not a local-IO optimisation

Explicitly out of scope, §9. The in-process fast path already meets its target (a bound-slot deref;
**62–77 ns/delivery amortized**), and #830 is what got it there. Nothing in this RFC touches
`graph_t`'s local read/write/await path. Any version of this document that claims a local-IO win is
wrong.

## 3. Motivation

### 3.1 The wire path re-handles strings per frame

Three costs, all of them re-paid on every frame of a flow that never changes:

1. **The forwarder's mount descent.** Each hop strips its whole mount run and forwards the residual
   — `resolve_mount_at` folds a digest chain over the leading segments and compares candidate mount
   slots, per hop, per frame (RFC-0024 §3.1). Measured against registry size, **receiver demux costs
   140–184 ns across fan 1–64** (`bench/bench_forward_demux.cpp`, the ADR-0061 baseline
   instrument): it is not dominated by the scan, which is the point — a large part of it is the
   constant per-hop work of re-deriving a split that did not move.
2. **The terminus residual walk.** `find_ptr` is linear in address depth at **+5.75 ns/segment**,
   and the full canonical terminus leg scales at **+21.3 ns/segment**, because a deep `dst` scales
   three costs at once: arena-decoding the D-NAME `PATH`, the mount peek, and the descent
   ([#830](https://github.com/avatarsd-llc/libtracer/issues/830)'s measured motivation).
3. **The subscribe-path name lookup.** The per-link subscriber index hashes the link **NAME** — a
   string — and does an `unordered_map` find on every remote subscribe. After PR #1290 removed the
   list-maintenance half, the honest residual is **43 ns at 4 links, 57 ns at 65**
   ([#1294](https://github.com/avatarsd-llc/libtracer/issues/1294)'s four-arm ablation, the number
   [#1266](https://github.com/avatarsd-llc/libtracer/issues/1266) exists to remove). It is **flat in
   link count**, which is the signature of a cost that is the string, not the search.

None of the three is a scan that a better container fixes. All three are **the same string being
re-interpreted**, and #1266's own conclusion names the fix: *"the subscribe path never receives a
handle to intern"*.

### 3.2 The local precedent, measured

#830's A/B settled the shape of the answer node-locally. Full `on_frame(FWD{WRITE})` terminus leg,
medians, pinned CPU, 40 interleaved reps:

| depth | canonical | bound | delta |
| ---: | ---: | ---: | ---: |
| 1 | 422 ns | 392 ns | 30 (7.8 %) |
| 2 | 425 ns | 393 ns | 32 (8.1 %) |
| 4 | 481 ns | 393 ns | 88 (22.4 %) |
| 8 | 556 ns | 393 ns | 163 (41.5 %) |
| 12 | 662 ns | **395 ns** | **267 (67.6 %)** |

Two facts carry: `deref_vertex_slot` is **flat at 11 ns at every depth** (a shared lock, a bounds
check, a slot load, a generation compare), and the **crossover to a disjoint-real win is D = 2** —
which is to say, essentially every real address. That is the whole argument of this RFC, transposed
one seam outward: *a resolution both ends already made should be spelled as the answer, not
re-derived from the question.*

The mechanism it transposes is in the tree today and is worth reading before the design below.
`dispatch_edge_target` tries the bound spelling and treats a refusal as "this answer is no longer
trustworthy", **not** as "no target" — it falls through to the canonical walk
(`core/src/graph.cpp:1307-1323`), and it counts the fallthrough
(`target_canonical_resolves_`, `core/src/graph.cpp:986`, `:1321`). The mint happens **once**, at
subscribe admission, after every door has decided the target key, and **failure to mint is not an
error**: an unbound edge simply keeps the canonical spelling
(`core/src/graph.cpp:2097-2109`). Every one of those properties reappears below as a normative
requirement, because each of them is what makes "drop, never mis-route" survive the extension.

### 3.3 What a label costs, in bytes

Cost rule: a canonical `PATH` body is concatenated `NAME` TLVs, each costing `4 + len`
(`docs/reference/05-protocol-tlvs.md:381`); a mount run is three segments at today's width, so
`net/ws-client/board-01` is 32 B of body and `net/can/c0` is 20 B (RFC-0023's priced shapes,
RFC-0023:185-188). A label element under §5.3's leading candidate layout is a 4-byte TLV header plus
a 4-byte body — **8 B, replacing a whole mount run**.

| route | canonical (hdr + body) | fully minted | saved |
| --- | ---: | ---: | ---: |
| direct link, `…/board-01/sensor/temp` | 4 + 50 = **54 B** | 4 + 8 + 8 = **20 B** | 34 B (−63 %) |
| one forwarder, `…/board-01/net/can/c0/sensor/temp` | 4 + 70 = **74 B** | 4 + 8×3 = **28 B** | 46 B (−62 %) |
| two forwarders | 4 + 90 = **94 B** | 4 + 8×4 = **36 B** | 58 B (−62 %) |

**Read that table honestly: it is RFC-0024 §3.2's table, to the byte.** At full mint, one label per
host part is arithmetically the same 8 B/host a `PATH_REF` element costs, so **on bytes alone this
RFC ties the bound path and beats nothing it does not already beat**. The byte column is therefore
*not* the case for this RFC. The case is the three properties bytes do not show:

- **it compacts the terminus residual too** — the `/sensor/temp` tail that a `PATH_REF` reaches only
  by having replaced the entire address, and the depth term §3.2 prices at +21.3 ns/segment;
- **it degrades per element, not per route** — a route with one non-minting hop still compacts every
  other hop, where a `PATH_REF` with one non-contributing hop is stripped entirely (RFC-0024 §7.1
  erratum 1);
- **it needs no request** — no bind-request flag, no `op`-byte masking rule, no round trip spent
  asking. Minting rides a reply that was travelling anyway.

### 3.4 What is deliberately not claimed

- **No local-IO claim** (§9, §2.3).
- **No claim against §E.1** on long-lived one-way delivery (§2.1).
- **No claim of a byte win over a bound path** (§3.3).
- **No measured per-hop figure for this design.** RFC-0024's bound hop measured 261.7 ns/hop against
  a canonical 304.7 (RFC-0024 §3.4), and a label deref is structurally the same work as a vref
  deref, but *structurally the same* is an argument, not an instrument. §14 labels it, and §12.4
  makes the measurement a condition of acceptance under the same bench protocol RFC-0024 §8 fixed.

## 4. The label

### 4.1 Shape — 32 bits, split 16/16

```
label : u32, little-endian ("little-endian for every multi-byte field",
                            docs/reference/01-data-format.md:35)
  bits  0-15 : index       — a slot in the minting host's label table
  bits 16-31 : generation  — that slot's generation at mint time
```

- **The index is a slot, assigned by the host at mint time.** It is an array position, handed out
  from the minting host's own table. Bounds-checking it is a compare against the table's
  cardinality — the same unforgeable-validation property RFC-0024 §4.5 rejects a raw pointer for:
  *there is no bounds check a receiver can apply to a peer-supplied address*, and there is an
  obvious one for a peer-supplied index.
- **It is NEVER a content hash.** A hash of the path text would be collision-*prone* by
  construction, and a collision is a **mis-delivery** — the failure class the whole doc set treats
  as unacceptable (RFC-0024 §5.3; #603's wrapped-label misroute). A host-assigned slot is
  **collision-free by construction**: the host cannot hand out a live slot twice, so there is no
  probability to reason about and no digest length to argue over.
- **16 bits of index ⇒ 65 536 live labels per host.** That is a bound on *simultaneously minted path
  parts*, not on vertices, not on peers, and not on flows: a mint happens once per subscription's
  first fire per hop (§6.2). §8's per-peer ceiling sits below it, and exhaustion is a designed,
  benign state (§8.3), not a failure.
- **16 bits of generation** make slot-reuse aliasing effectively impossible. §4.3 states the
  residual precisely rather than waving at it.

**Label-scope rule, normative.** A label means something **only on the host that minted it**, and
means nothing anywhere else — exactly the node-scoping rule RFC-0024 §4.1 states for a vref. A host
MUST NOT interpret a label it did not mint, and MUST NOT relay a label of its own into a part of the
path another host reads. A label is an **address**, never a capability: §8.1's post-auth rule and
§8.2's per-operation re-check are what keep that true.

### 4.2 Why host-assigned and not a hash — stated once, so it is not re-litigated

A content hash is the obvious alternative and it is refused on one axis. With a hash, two distinct
path parts *can* collide, and the receiver has no way to notice: the label validates, the deref
succeeds, and the operation lands on the wrong vertex. Every mitigation is worse than the disease —
carrying the string alongside to disambiguate gives up the whole saving; widening the digest trades
bytes for a probability that never reaches zero; a per-hop collision table is the per-hop route cache
RFC-0024 §12 already rejected twice.

A slot has none of that. The host owns the namespace, hands out only free slots, and refuses when it
has none (§8.3). **The collision class is closed by construction, which is the only closure this doc
set accepts for a mis-delivery class.**

### 4.3 The residual risk, quantified — and CLOSED at acceptance

> **⚠ Resolved by the acceptance ruling of 2026-08-15 (§4.3.1 below, §11.1 collision 2).** The
> draft's residual below assumed a **wrapping** generation. The accepted rule is
> **saturate-and-retire**, so the wrap sequence this subsection quantifies is **not reachable** and
> the mis-delivery class it describes is **closed by construction**. The text is kept because the
> arithmetic is what *prices* the ruling — it is the reason saturating is affordable — not because
> the risk stands.

The generation exists so that a label a sender still holds cannot validate against a *different*
occupant of the same slot. A departure bumps the slot's generation (§7.1), so an ordinary stale label
compares unequal and is refused.

~~**The residual is precisely this:** a sender holds a label for slot *i* at generation *g*; between
that frame and its next, slot *i* is minted and released **65 536 times**, wrapping its generation
back to *g*; the sender's next frame then validates against an unrelated occupant. That is a
mis-delivery, and it is the only sequence that produces one.~~ **Withdrawn by the acceptance ruling:
under §4.3.1 the generation never returns to *g*, so this sequence does not exist.** What the
paragraph identified correctly is that this was the *only* sequence producing a mis-delivery — which
is precisely why closing it closes the class.

Quantified, and it is negligible:

- **A mint is not a per-frame event.** It happens once per subscription's first fire, per hop (§6.2)
  — a control-plane cadence, not a data-plane one.
- **Slots are handed out across the table, not into one position.** For the wrap to happen at slot
  *i*, that *one* slot must be re-minted 65 536 times while the sender holds one label. At an
  already-implausible 1000 mints/s **into a single slot**, that is **65.5 seconds** of uninterrupted
  single-slot churn with a sender silent throughout; over a table cycling round-robin through 65 536
  slots the same 65 536 reuses of *one* slot need **4.29 × 10⁹** mints, which at 1000 mints/s is
  **49.7 days**.
- **The window is a sender's silence.** A sender that transmits at all re-validates: the first
  refusal takes it back to strings, and §6 re-mints on the very next reply. The exposure is the gap
  between two frames of one flow, not the lifetime of the label.
- **The string fallback self-corrects on the next round trip** (§7.2). There is no state to repair
  and no protocol step to run: the sender already holds the canonical original, uses it, and receives
  a fresh mint.

~~**This is nonetheless a wrap, and RFC-0024 §4.4 rule 3 forbids wrapping a generation in normative
MUST terms. §11.1 collision 2 flags that collision explicitly, prices the zero-wire-cost mitigation, and leaves
the reconciliation to the maintainer rather than deciding it here.**~~ **Ruled 2026-08-15 at
acceptance: there is no wrap. §4.3.1 is the normative rule; §11.1 collision 2 records the
resolution.**

### 4.3.1 Generation semantics, normative: saturate and retire, NEVER wrap

**Ruled at acceptance, 2026-08-15 (§11.1 collision 2).** This is the
normative generation rule of this RFC and it supersedes every "wrap" reading of the draft text
above:

- **A slot's generation MUST saturate, never wrap.** When a slot's 16-bit generation reaches its
  maximum value, it **stops advancing**. It MUST NOT return to zero and MUST NOT return to any
  earlier value.
- **A saturated slot is RETIRED permanently.** It MUST NOT be minted into again — not after any
  reclamation, not after a peer departs, not after the table empties, and not after a restart of the
  flow. The slot is removed from the mintable set for the lifetime of the table.
- **Retirement is invisible to correctness.** A host that cannot mint leaves the part a string
  (§6.3), which is *exactly* §8.3's refuse-new degrade — the path this design already accepts as
  benign. A retired slot costs one slot out of 65 536 per 65 536 reuses **of that one slot**, which
  §4.3's arithmetic prices as unreachable in practice.
- **Zero wire cost.** The label stays 32 bits, split 16/16 (§4.1); the byte layout of §5.3 is
  unchanged; no frame, no flag and no field is added. The rule is entirely a constraint on what a
  minting host may do with its own table.

This preserves [RFC-0024](0024-bound-paths-node-scoped-vertex-ref-source-routing.md) §4.4 rule 3
(*"the generation MUST saturate, never wrap"*) and §9.3's conformance obligation (*"A host MUST NOT
wrap a generation"*) **verbatim, across both fields**, so the doc set has **one** answer to the
question rather than one per type code. The #603 misroute class is closed by construction here for
the same reason it is closed there, and §7.1's existing "MUST NOT mint on a slot whose generation
has saturated" is no longer a tension with anything — it is the whole mechanism.

### 4.4 Rejected label shapes

- **A content hash of the path part.** §4.2 — collision is mis-delivery, and the closure must be by
  construction.
- **A raw pointer.** RFC-0024 §4.5's argument, unchanged and equally binding: no bounds check exists
  for a peer-supplied address, and the reference core does not even expose a raw-pointer accessor to
  a *local* caller — a vertex handle "exposes no `operator*` or raw-pointer accessor"
  (`core/include/libtracer/graph.hpp:62`).
- **A `u64` label (32-bit index + 32-bit generation).** Rejected on the cost model: the index would
  gain headroom above a ceiling §8's per-peer mint budget already sits far below, and the
  generation's extra 16 bits buy time against a sequence §4.3 shows is not reachable — while
  doubling the per-element cost of the one thing this RFC exists to make small. **Confirmed at
  acceptance:** §11.1 collision 2 was ruled on **saturating** rather than widening (§4.3.1), so
  **no wire bit changes at all** and the `u64` shape stays rejected on the same cost model.
- **A `u16` label (no generation).** Rejected outright: a bare slot index with no staleness guard is
  the #603 failure — a reused slot mis-delivers, with no stamp to catch it, and no fallback
  triggers because nothing detects the staleness.
- **A per-flow label negotiated in a setup exchange.** That is §E.1, which already exists (§2.1).

## 5. Path encoding — a per-element tag

### 5.1 The new arm

**Normative.** The path-element encoding gains a second arm. Today a `PATH` (`0x06`) body is a
sequence of `NAME` (`0x02`) children (`core/include/libtracer/tlv.hpp:34`, `:37`). Under this RFC an
element is **either** a name **or** a label, and **an element self-describes by its type code** —
never by its position and never by a mode bit on the enclosing `PATH`.

That last clause is not a preference; it is RFC-0024 amendment 2's ruling applied unchanged: a child
identified by position *"breaks the moment any future RFC adds a second trailing child"* and *"is
the inconsistency an implementer working from the text gets wrong once"* (RFC-0024 §7.1 amendment 2).
A mode bit on `0x06` is likewise refused for RFC-0024 §12's reason: it would make `PATH`'s
canonical-bytes property conditional, and that property is what `path_lookup_key` and every consumer
keyed on `PATH` bytes depend on.

The **canonical form is untouched and remains the mint key and the fallback**, exactly as RFC-0024
§1 requires of the bound form. A path with no label element is byte-identical to today's.

### 5.2 Mixed paths are legal and expected

**Normative.** A `PATH` MAY carry any mixture of name and label elements, in any order. There is no
"fully minted" state a path must reach and no all-or-nothing rule.

This is the property §2.2 names as the design's distinguishing one, and it follows directly from the
ruling that **hosts that don't mint don't change their part**. A hop that does not implement this
RFC, or that refuses to mint (§8.3's exhaustion, §7.1's saturation, a policy choice), relays the
path with its own part still spelled as strings; every other hop's part still compacts, and every
hop still reads its own part in whatever spelling it arrives.

The reason this is safe here and unsafe for a bound path is worth stating, because RFC-0024 §7.1
erratum 1 rules the *opposite* way for `PATH_REF` and a reader will notice. A `PATH_REF` is a bare
fixed-stride array with **no per-element framing** and its elements are identified **by position**:
a list that skips a host is not a shorter route but a wrong one, because the next reader consumes
an element another host minted. A tagged path element has **no such positional coupling** — each
element is read by the hop whose part it is, in the same order the strings were, and a
string-spelled element is simply resolved the way it is resolved today. **Skipping is not
expressible**, so erratum 1's mis-route class does not arise.

### 5.3 Byte layout: proposed, pending conformance vectors

Following the discipline [RFC-0014](0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)
set — *"the byte-level clauses here are **proposed pending** code + conformance vectors"* — the
**semantics above are ruled and the bytes below are not**.

The leading candidate, for the reviewer's concreteness only:

```
PATH_LABEL (0x16, PL=0, LL=0) {
  ; body: u32 LE label — u16 index, u16 generation (§4.1)
}
```

- `0x16` is the next unused type code: `0x01`–`0x15` are assigned, `0x05` is a retired gap that is
  not reused, and `0x15` is `PATH_REF_REVERSE` (`core/include/libtracer/tlv.hpp:61`, `:73`).
- `opt.PL` MUST be 0 — the body is a fixed-width scalar, not concatenated child TLVs. `opt.LL` MUST
  be 0 — a 4-byte body is three orders inside u16. Both are RFC-0024 §4.2's rules, for the identical
  reason.
- Cost: **4 + 4 = 8 B per element**, which is §3.3's arithmetic.

Open with the bytes, and to be settled with the vectors of §12.5:

1. Whether the element is a TLV child of `PATH` (8 B, self-describing, composes with
   [RFC-0018](0018-packed-path-segments.md)'s packing untouched) or a tag inside RFC-0018's packed
   segment grammar (potentially 5 B, at the cost of coupling two encodings that currently do not
   know about each other). **RFC-0018 is accepted and lands independently; whichever of the two
   lands second re-runs §3.3's table**, exactly as RFC-0024 §10 requires of the same pairing.
2. Whether one label may cover a **multi-segment** part (a whole three-segment mount run — the
   assumption §3.3's arithmetic makes) or exactly one segment. §3.3's saving depends on the former;
   the semantics of §6's rewrite already are the former, since a hop's "local part" *is* its mount
   run.
3. Whether a `PATH` carrying a label element is admissible as a `path_lookup_key` (it must not be —
   a label is not canonical bytes — so the key is the canonical original the sender still holds).

## 6. Distribution is passive

### 6.1 There are no label-distribution frames

**Normative.** This RFC adds **no control frame, no advertise, no request flag, and no setup
exchange.** A host that implements it emits exactly the frames it emits today, with some path
elements spelled differently.

Minting rides the **reply**. Concretely, on an operation whose forward legs were spelled in strings:

1. Each forwarding hop resolves its local part canonically, as today, and forwards the residual.
2. On the way **back**, as it relays the reply, each hop **rewrites its own local part of `src` and
   `dst` from string to the label it minted** for that part. It rewrites its own part and nothing
   else.
3. The terminus does the same for the residual it resolved.
4. **The first reply therefore returns to the original sender with fully-minted `src` and `dst`**,
   and the sender's `path_t` caches them.

Two properties fall out and are both required:

- **Zero added bytes, in both directions.** The rewrite *replaces* string bytes with a shorter
  element; it never appends. This is strictly stronger than RFC-0024 §7.5's "zero added origin
  bytes", which still pays `4 + 8H` on the mint-carrying leg.
- **Nothing to ask for.** RFC-0024 §7.5 spends `op` bit 7 and a masking rule on a bind-request flag,
  and §9.3 makes the masking rule a MUST for every forwarder to keep a pre-amendment peer from
  rejecting an unknown opcode. **This RFC spends none of that**, because a hop that does not
  understand a rewritten element never receives one: only the hop that minted a label ever reads it.

### 6.2 Each subscription's first fire triggers path creation and minting

**Normative.** The mint trigger is the **first fire of a subscription** — the event that creates the
`path_t` and walks the route for the first time. Minting is a rider on that walk's reply, per §6.1.

This is deliberately the **cheapest admissible trigger**, in RFC-0024 §7.2's exact sense: a hop mints
when it is *already holding* the resolution, and on **no other condition**. **No use counters, no hit
thresholds, no hotness estimate, no timers, no aging.** The standing rule applies (`CONTEXT.md`
§Resource bound, the no-synthetic-limits doctrine): a bound comes from an injected resource and a
trigger comes from a fact already in hand, never from a prediction.

It also mirrors the local precedent exactly. #830 mints the edge's binding **once**, at subscribe
admission, *"after every door has finished deciding what `s.target_key` is"*, and treats a failed
mint as a non-event (`core/src/graph.cpp:2097-2109`). Same trigger, same non-event, one seam
outward.

### 6.3 A mint is never load-bearing

**Normative.** A host MUST behave correctly when no label is ever minted anywhere on a route. A mint
is an optimisation on a resolution that already succeeded; a refusal to mint, a stripped label, a
non-implementing hop and an exhausted table are **all the same case** — the string path — and none of
them is an error, a NACK, or an observable event on the wire.

## 7. Staleness

### 7.1 Departure bumps the generation

**Normative.** When the vertex a label resolves to departs — retirement, connection-vertex removal,
link teardown — the minting host **bumps that slot's generation**. The label the peer holds then
compares unequal and is refused. Generations only move forward, so a stale label never becomes valid
by waiting (RFC-0024 §5.1's property, and the reason it is stated there as well).

A host **MUST NOT** mint on a slot whose generation has saturated; the slot is **retired
permanently** (§4.3.1) and the host leaves the part a string (§6.3). ~~See §11.1 collision 2 for the
flagged tension between "saturate" here and §4.3's 16-bit wrap.~~ **Resolved 2026-08-15 at
acceptance: there is no tension — §4.3.1 makes saturate-and-retire the normative rule and withdraws
the draft's wrap reading. This clause is that rule's enforcement point.**

### 7.2 A stale or unknown label answers `NOT_FOUND`, and the sender falls back

**Normative.** A host that receives a label it cannot validate — out of range, generation mismatch,
unminted slot, a label it did not mint —

- **MUST NOT** forward it, **MUST NOT** apply the operation, and **MUST NOT** attempt any repair of
  its own: no re-resolution against a nearest match, no retry against a different slot, no guessing.
  This is RFC-0024 §5.3's **drop-never-mis-route** rule and it binds here identically.
- **MUST** answer a **`NOT_FOUND`-class error**. Unlike RFC-0024 §5.3's NACK — whose spelling that
  RFC still leaves open in its §9.2 — this needs no new frame: an unresolvable address is exactly
  what `tr::path::not_found` already means, and a label is an address.

The sender's recovery is the one that already exists and is already known to work: **fall back to the
full-string path it still holds, and re-mint from the next reply** (§6.1). One failed operation is
the entire cost. §5.1's rule that canonical is the mint key and the fallback is what makes this
closed-form: every label has a string original by construction, so no address is ever reachable in
label form alone.

### 7.3 No withdraw protocol, no aging

**Normative.** There is **no withdraw frame, no unbind, no lease, and no TTL.** A label is not
retired by its holder and not expired by its minter; it simply stops validating, and the next frame
discovers that. A host MAY reclaim a slot whenever it likes, because reclamation is *already* safe:
the generation bump is the entire mechanism.

This is the property that keeps the state cost bounded to a table and stops it from becoming a
protocol. It is also why §8's exhaustion policy can be "refuse new" rather than "evict oldest": there
is no aging axis to evict along, and inventing one would be a synthetic limit.

## 8. Security and bounds

### 8.1 Minting is POST-AUTH only

**Normative.** A host MUST NOT mint a label for a part of a path the requesting peer could not have
reached canonically in the same operation, and MUST NOT mint before that peer's authentication and
the operation's own ACL gates have passed. Since a mint rides an ordinary resolved operation (§6.1),
this is automatic — the operation already ran its gates on the way through — but it is stated as a
requirement because the negative property matters: **no label is ever minted for a destination an
ancestor ACL hides.**

Probing therefore yields what probing the string form yields — *exists + denied*, never *exists +
here is a handle to it*. A label cannot be used to discover a namespace its holder cannot already
walk. This is RFC-0024 §6.1's anti-enumeration property, restated because a route form that skipped
a gate would be a capability, and a label is an address.

### 8.2 Every labelled operation re-checks the ACL at the dereferenced vertex

**Normative.** An operation arriving on a labelled path MUST evaluate `acl_allows` at the
dereferenced vertex, for that operation's own right, exactly as the string form does. **A generation
match authorizes nothing** — it says the vertex is the same one, never that the caller may still act
on it (`core/include/libtracer/graph.hpp:491` states this contract for the local case in terms this
RFC does not improve on).

A label holds no authorization state of any kind, so a revoked right takes effect on the very next
operation over an already-minted label. There is no snapshot to go stale.

### 8.3 The table is injected, per-peer ceilinged, and refuses on exhaustion

**Normative, and each clause is a ruling:**

- **The label table draws from the injected net-plane store.** Not a static array, not a
  library-chosen capacity — the
  [ADR-0079](../../adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md) per-plane
  composition axis, sized by the embedder. This is the standing no-synthetic-limits rule and the same
  shape RFC-0024 §6.4 requires of its binding budget.
- **There is a per-peer ceiling.** A single peer cannot consume the table. The ceiling is a
  per-target configuration, not a magic number.
- **On exhaustion, a host REFUSES new mints.** It does not evict, does not grow, and does not fail
  the operation. The part stays a string and the operation proceeds normally — **refusal is invisible
  to correctness** (§6.3), which is exactly why refusing is affordable.
- **Live labels are NEVER evicted by pressure.** An established label is not a cache entry to be
  reclaimed; evicting one would convert a bounded resource problem into a stream of avoidable
  `NOT_FOUND` round trips on flows that were working.

**This mirrors the CAN control-map ruling** — a ceiling plus refuse-new, never eviction — and it is
the same degrade RFC-0004 §E.1's label allocator already takes at exhaustion: return nothing, record
nothing, send the full route, *"the full-route form that always works"* (`core/src/route_handle.cpp:259-267`). A
mechanism whose exhaustion policy is "do what we did before this mechanism existed" cannot make a
node worse.

### 8.4 What an attacker gets

A peer that can put frames on a link can, at most: spend its own per-peer ceiling (bounded, §8.3);
present labels it did not mint (refused by §7.2, one `NOT_FOUND` per attempt, no state); or present
a stale label (refused by the generation, §7.1). It cannot enumerate (§8.1), cannot escalate
(§8.2), and cannot cause an eviction of anyone else's labels (§8.3). ~~The one residual is §4.3's
wrap, flagged in §11.1 collision 2.~~ **As of the 2026-08-15 acceptance ruling there is no
residual**: §4.3.1's saturate-and-retire closes the wrap sequence by construction, so the worst an
attacker achieves against the generation is to retire slots it already paid for out of its own
per-peer ceiling — a bounded self-denial that degrades to the string path (§6.3).

## 9. Local IO is out of scope

**Explicitly, and normatively for this document's own claims:** this RFC touches **wire ingress and
egress and the forwarding plane, and nothing else.**

The in-process fast path already meets its target. A local delivery dereferences a bound slot
(#830, `core/src/graph.cpp:1307-1323`) and costs **62–77 ns/delivery amortized**. There is no
string re-handling left on that path to remove, `graph_t`'s read/write/await surface is unchanged by
this RFC, and no local API grows a label parameter. A label never appears in a host-API call, never
in a `path_t` the application constructs, and never in a local resolution — the `path_t` binding slot
of §6.1 is **opaque to the application**, filled and validated by the net tier, in the layering
discipline RFC-0024 §7.4 states (*"L1/L2 never learns what an L4 vertex index means; it carries bytes
it cannot interpret"*).

## 10. Substrate: the #1294 peer handle

[#1294](https://github.com/avatarsd-llc/libtracer/issues/1294) makes the `bus_link_t` peer-receiver
seam carry an **opaque per-peer handle — minted once at accept, valid until depart** — instead of
re-supplying a name string on every inbound frame. It is *"very likely the `(slot, generation)`
shape already minted for WS sessions"*, and it unblocks #1266 (intern the link identity), #375
Part 2 (per-peer auth subject) and #1278.

**It lands first and independently, and this RFC does not define it.** The relationship is prior
art and available mechanism, in two directions:

- **The shape is already ruled elsewhere.** `(slot, generation)` is the same primitive as #830's
  edge binding, RFC-0024's vref, and this RFC's label — a node-scoped index plus a validate-on-use
  stamp. This RFC inherits the pattern; it does not re-derive it, and it must not re-decide #1294's
  open questions (whether the handle unifies with `session_ref_t`, whether it carries the subject).
- **The mechanism it supplies is the one §8.1 wants.** A per-peer handle minted at accept is exactly
  the identity a per-peer mint ceiling is counted against, and exactly the post-auth subject §8.1's
  gate reads. If #1294 lands as described, §8.3's ceiling has an owner with no new bookkeeping.

**Nothing in this RFC blocks on #1294 and nothing in #1294 blocks on this RFC.** If #1294's handle
lands with a different shape, §8.3's ceiling is counted against whatever peer identity the net plane
does have, and the wire surface here is unaffected.

## 11. Compatibility

### 11.1 Relationship to RFC-0024 — the honest statement

RFC-0024 is **accepted**, `PATH_REF`/`PATH_REF_REVERSE` ship, and **this RFC withdraws nothing of
it.** The relationship is: **RFC-0027 is the cross-node generalization of #830's per-element
binding, and RFC-0024 is the cross-node generalization of the same primitive at host granularity.**
§2.2's table is the comparison. Where the two agree, this document reuses RFC-0024's rules verbatim
and says so (§4.4, §5.1, §5.3, §7.2, §8.1, §8.2, §8.3) rather than re-deriving them.

Three things in RFC-0024's *normative* text collide with this design. Per the house rule they were
**flagged** in the proposed draft, not silently overridden. ~~**none of them is decided by this
document.**~~ **All three were RULED by the maintainer on 2026-08-15 as part of this RFC's
transition proposed → accepted**, which is the point at which an RFC's own text may be edited; each
subsection below now records its resolution beside the collision it resolves, and the collisions are
retained rather than deleted so the reasoning survives:

| collision | ruling (2026-08-15) | where the change lands |
| --- | --- | --- |
| 1 — vocabulary (RFC-0024 §2.1) | **(a) qualify.** "**Path label**" is ratified as a distinct term; unqualified "label" still means RFC-0004 §E.1's per-link `u16`. | **RFC-0024 amendment 3** (§2.1 of that document); `CONTEXT.md` §12.2 entries in the acceptance train |
| 2 — the generation (RFC-0024 §4.4 rule 3, §9.3) | **Saturate-and-retire, NOT wrap.** A slot whose generation saturates is retired permanently. RFC-0024's invariant is preserved verbatim, at **zero wire cost**. | **§4.3.1 of this RFC** (normative); §4.3, §4.4, §7.1, §8.4, §14, §15 annotated |
| 3 — "no hop holds anything" (RFC-0024 §2.1, §12) | **Accepted knowingly.** The per-hop mint table is a deliberate, recorded surrender of RFC-0024's stateless-hop property, not an oversight. | **§11.1 collision 3 and §11.4 of this RFC** (recorded decision); no ADR is superseded |

#### Collision 1 — the vocabulary rule (RFC-0024 §2.1) — RULED (a), 2026-08-15

RFC-0024 §2.1 states a rule it marks **"normative for the doc set"**:

> "**Label**" remains RFC-0004 §E.1's per-link `u16` and nothing else. […] A sentence that calls a
> vref a label is wrong (§11).

`CONTEXT.md` §Vertex ref carries the same rule in its `_Avoid_` line. **This RFC's central concept is
a 32-bit, per-host, per-path-element value, and the maintainer's ruling names it a label** — the
MPLS analogy is the ruling's own framing and is load-bearing to how the design reads.

The collision is real and it is a naming collision, not a design one: RFC-0027's label and §E.1's
label are different objects (§2.1's table). Two reconciliations exist, and **the choice is the
maintainer's**:

- **(a) Qualify.** Narrow RFC-0024 §2.1's rule by amendment: an unqualified "label" continues to
  mean §E.1's per-link `u16`; this RFC's concept is always the qualified **"path label"**, and the
  `CONTEXT.md` entry carries both with an `_Avoid_` line separating them. Cheapest, and it preserves
  every existing sentence.
- **(b) Rename.** Give this RFC's concept a non-colliding term. It costs the MPLS reading the ruling
  leans on, and every reviewer will re-invent the word "label" in the first sentence of every
  discussion.

~~**This document uses "path label" throughout in anticipation of (a)**, and does **not** edit
`CONTEXT.md` — a proposed RFC does not move the canonical glossary, and moving it before this is
ruled would be exactly the silent override the house rule forbids. §12.2 lists the entries an
accepted RFC adds.~~

**RULING (maintainer, 2026-08-15, at acceptance): (a) QUALIFY.** "**Path label**" is ratified as a
term of the doc set, **distinct** from RFC-0004 §E.1's per-link `u16` "label":

- An **unqualified "label"** continues to mean **§E.1's per-link `u16` and nothing else**. RFC-0024
  §2.1's rule is not withdrawn; it is **narrowed to the unqualified word**, and *"a sentence that
  calls a vref a label is wrong"* stands unchanged.
- This RFC's concept is **always** the qualified "**path label**" — never bare "label" — and is a
  32-bit, per-host, per-path-element value. The three `(slot, generation)` concepts of the doc set
  (§E.1's link label, RFC-0024's **vref**, RFC-0027's **path label**) are three different objects
  and the vocabulary now separates them.
- Grounds, in the ruling's own order: it is **cheapest** (every existing sentence in the doc set
  stays correct), it **preserves the MPLS reading** the design's framing leans on, and a rename
  would be re-invented as "label" in the first sentence of every discussion — a vocabulary rule
  nobody can follow is not a vocabulary rule.

**Where it lands.** RFC-0024 §2.1 carries **amendment 3 (2026-08-15)** recording the narrowing —
appended in that document, with its original rule annotated rather than rewritten, per the house
amendment discipline. The `CONTEXT.md` entries §12.2 enumerates land in the acceptance train.

#### Collision 2 — the generation MUST saturate, and the slot retires — RULED, 2026-08-15

This is the **load-bearing** one, and it is **resolved**: **saturate-and-retire, NOT wrap.**
§4.3.1 carries the normative rule; this subsection records why. RFC-0024 §4.4 rule 3 reads:

> **Therefore, normatively: the generation MUST saturate, never wrap.** […] A wrapped **generation**
> is that same failure with the guard instead of the address: a stale vref **validates falsely**,
> and the operation lands on the vertex's successor.

and §9.3 restates it as a conformance obligation: *"A host MUST NOT wrap a generation."* The rule is
#603's ruling transposed, and #603 is on the record as a **misroute, not a drop**
(`core/src/route_handle.cpp:175-177`).

~~**RFC-0027's 16-bit generation wraps.**~~ **The draft's 16-bit generation wrapped.** §4.3 stated
the sequence exactly and quantified it as negligible. The draft's own analysis is what settles the
question: *"different field, same class" is how a doc set acquires two answers to one question* —
RFC-0024 §9.3's subject is a `PATH_REF` element's generation, a different field on a different type
code, **but the failure class is identical** (#603's misroute, on the record as a misroute and not a
drop, `core/src/route_handle.cpp:175-177`). A doc set that answers "MUST NOT wrap" for one
`(slot, generation)` field and "wraps, but negligibly" for the next is a doc set an implementer gets
wrong once.

**RULING (maintainer, 2026-08-15, at acceptance): SATURATE AND RETIRE. The generation MUST NOT
wrap.** The reconciliation the draft priced is taken exactly as priced: **saturate the slot's
generation instead of wrapping it, and retire the slot permanently.** A saturated slot becomes
permanently unmintable, the part stays a string, and §8.3's refuse-new policy already describes
exactly that degrade. **The wire format, the byte layout (§5.3), the 16/16 split (§4.1) and every
other clause of this RFC are unchanged** — the rule constrains only what a minting host does with
its own table. The cost is one slot out of 65 536 per 65 536 reuses **of that one slot**, which is
indistinguishable from the exhaustion path this design already accepts as benign.

**What this buys:** RFC-0024's *"the generation MUST saturate, never wrap"* (§4.4 rule 3) and *"A
host MUST NOT wrap a generation"* (§9.3) now read **across the whole doc set, unqualified**, for
every `(slot, generation)` field it defines. There is **one** rule, not one per type code, and the
#603 mis-delivery class is closed by construction in both places.

~~**Flagged, not decided.**~~ **Decided.** The normative text is §4.3.1; §4.3, §4.4, §7.1 and §8.4
carry the annotations. §15's falsification clause 3 is the other side of it and is likewise
narrowed: the mitigation was priced at zero wire bytes and has been taken, so what remains
falsifiable is only whether slot retirement churns the table in practice — a measurement, not an
argument.

#### Collision 3 — "no hop holds anything" (RFC-0024 §2.1, §12) — ACCEPTED KNOWINGLY, 2026-08-15

RFC-0024 credits the bound path with holding **no state at any hop** — its §2.1 table reads *"no hop
holds anything"*, and its §12 rejects *"a per-hop route cache keyed on canonical bytes"* as *"the
ADR-0062 reverse-index shape twice refused"*. **This RFC puts a table on every minting hop.** That is
a real surrender of a property RFC-0024 names as a virtue, and it must not be presented as anything
else.

What distinguishes it from the shape §12 rejected, stated so the distinction can be judged rather
than assumed:

- **It is not keyed on canonical bytes** and performs no lookup on the forwarding path. It is an
  array indexed by a number the peer supplies — a bounds check and a load, not a hash of an address.
- **It introduces no second invalidation mechanism**, which was §12's actual objection (*"a SECOND
  invalidation mechanism beside one that works"*). The generation bump on departure is the
  invalidation, and it is the same validate-on-use stamp discipline `graph_t::retire_generation` and
  `child_registry_t::mount_generation` already use.
- **It is bounded by an injected resource and refuses rather than evicts** (§8.3), so its size
  tracks minted parts, not traffic, and its worst case is today's behaviour.
- **It is not novel in the net plane.** §E.1 already puts per-link label state on hops that carry
  compact flows, and `docs/reference/05-protocol-tlvs.md:1020` already reconciles that with the
  stateless-forwarder property in the same terms: *"A node holds label state **only** for the compact
  flows crossing it […] which preserves the stateless-forwarder property."*

**The honest summary: RFC-0024 buys stateless hops at the price of an all-or-nothing route form;
RFC-0027 buys per-element degradation and terminus compaction at the price of per-hop state. They
are different trades of the same primitive, and both are defensible. What is not defensible is a doc
set that claims both properties at once, which is why §11.4 edits the reference pages.**

**RULING (maintainer, 2026-08-15, at acceptance): the surrender is ACCEPTED KNOWINGLY, and is
hereby a recorded decision rather than a flag.** In full, so it is not re-litigated as an oversight:

- **RFC-0027 hosts hold per-hop state.** A minting host holds a **path-label table**. RFC-0024's
  *"no hop holds anything"* is a property of the **bound path**, and it remains true of the bound
  path. It is **not** a property of the net plane as a whole once RFC-0027 ships, and this document
  says so in its own text rather than leaving the doc set to claim both.
- **The trade is deliberate.** What the table buys is the pair of properties §2.2 and §3.3 name as
  this design's distinguishing ones — **per-element degradation** (one non-minting hop does not
  strip the route) and **terminus-residual compaction** (the depth term §3.2 prices at
  +21.3 ns/segment). Neither is reachable with a stateless hop. The maintainer ruled the trade
  worth making.
- **It is not the shape RFC-0024 §12 rejected**, on the four grounds enumerated above: not keyed on
  canonical bytes, no second invalidation mechanism, bounded by an injected resource with
  refuse-not-evict, and not novel in a net plane that already carries §E.1's per-link label state.
  §12's actual objection was the second invalidation mechanism, and there is none — the generation
  bump is the invalidation, now saturating and retiring per §4.3.1.
- **The worst case is today's behaviour.** A host that holds no table, or refuses to mint, or
  exhausts its ceiling, is a host that forwards strings — which is what every host does today
  (§6.3, §8.3). The state is an accelerator, never a correctness dependency, and a hop may still
  reboot mid-operation because **the route still lives in the frame**.
- **The instrument.** This is recorded **in this RFC's text and in §11.4's reference-page
  qualifications**. Per §11.4's own rule the ADRs are **not** edited: the maintainer ruled the
  ADR-0040 / ADR-0038 / ADR-0037 entries in §11.4's table to be **qualifications, not
  contradictions**, so **no superseding ADR is required in the acceptance train**. §11.4 records
  that ruling.

### 11.2 Where this leaves the two forms on one frame

**Recommended, pending the §12.4 measurement.** A host SHOULD NOT mint path labels into an address
already spelled as a `PATH_REF`, and SHOULD NOT bind a `PATH_REF` over a path whose elements are
already labelled. They are two compressions of one address; applying both spends two mechanisms to
save the bytes one of them already saved (§3.3's table is the same table), and it doubles the
staleness surface for a single route. A host implementing both chooses per route, on the axis §2.2's
table gives: `PATH_REF` where the route is stable and every hop contributes, path labels where hops
are heterogeneous or the terminus residual is deep.

### 11.3 Peers that do not implement this RFC

- **A peer that never mints** is fully conformant and fully interoperable: it relays paths with its
  own part spelled as strings (§5.2), and nothing on the route notices.
- **A peer that receives a labelled element it does not understand** — impossible on a correct route,
  since only the minting host reads its own label — handles it per the forward-compatibility rules of
  `docs/reference/01-data-format.md` §handling unknown type codes if it happens anyway.
- **No `op`-byte flag, no opcode masking rule, and therefore no version window** of the kind
  RFC-0024 §7.5 had to state and §7.1 amendment 1 had to bound. Passive distribution costs nothing
  in compatibility because it asks for nothing.
- **String support stays mandatory.** A path is always expressible in strings, and every label has a
  string original by construction (§7.2).

### 11.4 The "stateless forwarder" claim in the reference pages and the ADRs

The doc set characterizes forwarding as **stateless** in several places. With §E.1's label plane that
claim was already qualified where it was made most precisely
(`docs/reference/05-protocol-tlvs.md:1020`). **This RFC's minting is learned per-flow state, so the
unqualified claims are no longer fair**, and the qualifying notes — an admonition or a clause citing
RFC-0027 — sit at the three places where the claim was made **bald**. The proposal PR added them
citing RFC-0027 as *proposed*; **the acceptance PR updates all three to cite it as accepted**:

- `docs/reference/00-overview.md:56` — *"a stateless `FWD` hop between links"*;
- `docs/reference/02-graph-model.md:31` — *"the stateless component that routes `FWD` frames"*;
- `docs/reference/04-communication-flows.md:279` — *"**Forwarders are stateless.** There is no
  per-request table"*.

The reference pages are **not otherwise rewritten**: the claim is true of the **bare** hop and stays
true, this RFC is now **accepted but not yet implemented**, and the note says exactly that. The
promotion of these notes to the fully qualified statement is §12.1's acceptance-train item, landing
with the code that mints.

**Deliberately NOT edited, and listed here instead** — an ADR is a dated record of a decision and is
not revised because a later RFC is proposed against it (`CLAUDE.md`: *"design rationale and history
… explains why it looks the way it does"*). These carry claims a shipped RFC-0027 would qualify, and
an accepted RFC records the qualification in its own text or in a superseding ADR, never by editing
these:

| ADR | the claim | how RFC-0027 bears on it |
| --- | --- | --- |
| [ADR-0040](../../adr/0040-net-plane-is-explicit-source-routed-only.md):41, :54 | *"the net plane becomes stateless and dedup-free"*; *"The net plane is now uniformly stateless and lock-free-eligible"* | **Uniformly** stateless would no longer hold: a minting hop holds a label table. Explicit-source-routed-only, no flooding, no auto-multipath and dedup-free are all **untouched** — a label is source-routing spelled shorter, and this RFC adds no dedup state. |
| [ADR-0038](../../adr/0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md):16, :20, :62 | the **two-plane** model — *"stateless full-route"* vs *"label-compacted"*, *"Route lives in the frame ⇒ the forwarder is stateless"* | RFC-0027 sits **between** the two planes ADR-0038 names, which is the honest reading: the route still lives in the frame (so a hop may still reboot mid-operation and the reply still self-routes), but part of it is spelled in per-host state. ADR-0038's ranking — stateless full-route > label-compacted — is unchanged and correctly places this design *below* the full-route plane on reliability-by-construction. |
| [ADR-0037](../../adr/0037-net-side-channels-dissolve-into-vertex-tree-compositor.md):8, :22 | *"a stateless hop-by-hop FWD forwarder"*; *"The composite's statelessness **is** the leanness mechanism"* | The **compositor** claim (:22) is about the *transport-vertex composite* holding no data state, which this RFC does not touch. The `fwd_router_t` characterization (:8) is the one a shipped RFC-0027 qualifies. |
| [ADR-0044](../../adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md) | peer enumeration is stateless and synthesized from live traffic | **No conflict.** ADR-0044's subject is the *peer listing* and its passive last-heard table; a label table is neither, and ADR-0044's own insert-only, population-bounded table is a close precedent for §8.3's shape. |

~~If the maintainer judges any of these to be a contradiction rather than a qualification, the
instrument is a superseding ADR in the acceptance train — not an edit here.~~

**RULED (maintainer, 2026-08-15, at acceptance): all four table entries are QUALIFICATIONS, not
contradictions. No superseding ADR is required, and none is opened.** The per-hop mint table is
accepted knowingly (§11.1 collision 3), and the reading the table already takes is the ruled one:

- **ADR-0040** — *explicit-source-routed-only*, *no flooding*, *no auto-multipath* and *dedup-free*
  are the load-bearing decisions and are **untouched**; a path label is source routing spelled
  shorter, and this RFC adds no dedup state. Only the word *"uniformly"* in "uniformly stateless"
  narrows, and it narrows to what §E.1 had already narrowed it to.
- **ADR-0038** — its **two-plane** model and its ranking (stateless full-route > label-compacted)
  are **unchanged and correct**, and they place this design exactly where it belongs: below the
  full-route plane on reliability-by-construction. The route still lives in the frame, so the
  reboot-mid-operation property survives.
- **ADR-0037** — the **compositor** claim (:22) is about the transport-vertex composite holding no
  *data* state and is untouched. The `fwd_router_t` characterization (:8) is qualified by the same
  note the reference pages carry.
- **ADR-0044** — **no conflict**, as recorded; its insert-only, population-bounded table is a close
  precedent for §8.3's shape rather than a claim RFC-0027 disturbs.

An ADR is a dated record of a decision, and none of these decisions is reversed by this RFC. The
qualification lives in §11.1 collision 3 and in the reference pages, which is where a reader looking
for current behaviour goes.

## 12. Proposed change

Spec edits land **after acceptance, in follow-up PRs, car by car** — a normative clause is
incorporated in the same train as the code that honours it, per RFC-0024 §9's precedent. **This
RFC's own PR adds this document and §11.4's three reference-page notes, and nothing else.**

### 12.1 New normative text (on acceptance)

- `docs/reference/05-protocol-tlvs.md` — a new `## 0x16 — PATH_LABEL` section carrying §4's label
  shape, §5.3's ledger, the `PL=0`/`LL=0` MUSTs, and the node-scope rule; the unassigned-range census
  line moves from `0x16`–`0x1F` to `0x17`–`0x1F`.
- `docs/reference/05-protocol-tlvs.md` §`0x06` — a `PATH` element MAY be a `PATH_LABEL`; elements
  self-describe by type; mixed paths are legal (§5.2).
- `docs/reference/05-protocol-tlvs.md` §routing semantics — the mint-on-reply rewrite (§6.1), the
  `NOT_FOUND` + fall-back-to-strings rule (§7.2), and the no-withdraw/no-aging rule (§7.3).
- `docs/spec/v1.md` §3 — incorporate the `PATH_LABEL` constraints alongside the existing `PATH` and
  `PATH_REF` incorporation bullets.
- `docs/reference/03-addressing.md` — the label arm beside the two path forms already described
  there, with the rule that strings are the mint key and the fallback.
- `docs/reference/00-overview.md`, `02-graph-model.md`, `04-communication-flows.md` — §11.4's notes
  are promoted from "proposed" to the qualified statement.

### 12.2 `CONTEXT.md` — unblocked by the collision-1 ruling, lands in the acceptance train

~~Pending §11.1 collision 1,~~ **§11.1 collision 1 is ruled (a) qualify (2026-08-15), so the
glossary move is unblocked.** The accepted RFC adds to §Graph, addressing & API:

- **Path label** — the 32-bit per-host per-element alias; host-assigned slot, never a hash; minted
  passively on a reply; stale ⇒ `NOT_FOUND` ⇒ string fallback; no withdraw, no aging. `_Avoid_`:
  "label" unqualified (that is §E.1's per-link `u16` — RFC-0024 §2.1's rule); "a label is a route
  cache"; "a label authorizes".
- The existing **Vertex ref** entry's `_Avoid_` line gains the third member of the family, so the
  three `(slot, generation)` concepts are separated in one place.

### 12.3 Code, after acceptance

`core/include/libtracer/tlv.hpp` (the type code), the label table behind the injected net-plane
store, the element codec, the forwarder's label branch beside `resolve_mount_at`, the reply-leg
rewrite, and the `path_t` label cache (opaque at L1/L2, §9). Bindings (`bindings/rust`,
`bindings/typescript`) and the Wireshark dissector (`tools/wireshark/libtracer.lua`) follow.
Public-header changes go in `core/CHANGELOG.md`.

### 12.4 The bench gate — normative for acceptance of the implementation

An implementation is **not acceptable** until it answers these with measurements taken under the A/B
protocol RFC-0024 §8.2 made normative (`docs/methodology.md` §"The A/B protocol": pin both arms to
one logical CPU on one core class, interleave ≥ 10 rounds per arm, report medians *and* ranges,
discard the first execution, prefer same-directory A/B, and reach for object-file `cmp` where the
effect sits below the leg's noise floor).

1. **The per-hop saving**, as a slope over hop count, both spellings, identical traffic semantics —
   the instrument RFC-0024 §8.4 had to correct itself to find. A single-hop point measurement is the
   wrong instrument for a per-hop claim.
2. **The terminus residual saving against depth** — §3.2's table, re-run with the label spelling.
   This is the axis §3.3 says the byte column cannot show, so it is the one that decides.
3. **The mandatory must-not-regress arm: the four-link `reply-spread` fan-out.** It is the shape that
   killed #504's memo (140–311 ns/write) and it is a mandatory arm of RFC-0024 §8.3. A measured
   latency regression on **any shipped shape** rejects the implementation.
4. **The subscribe path** — #1266's own acceptance bar: at or below the A/A null band at
   4/8/16/32/65 links.
5. **rv32 flash/RAM delta**, including the label table at a realistic mint count. The standing census
   rule applies: **no "beat" is banked without it.**

### 12.5 Conformance vectors

- `path-label/label-roundtrip` — a `PATH` with one label element, round-tripped, pinning §5.3's
  ledger byte for byte.
- `path-label/label-mixed` — a `PATH` mixing name and label elements in both orders. This is the
  vector §5.2's ruling lives or dies by.
- `path-label/label-pl-set` and `path-label/label-ll-set` — `opt.PL=1` / `opt.LL=1` on a
  `PATH_LABEL` ⇒ reject. Separate clauses need separate vectors: a core that drops one passes every
  other vector in the category (RFC-0024 §9.4's lesson).
- `path-label/label-wrong-length` — a body that is not exactly 4 bytes ⇒ `tr::frame::invalid`.
- `fwd/fwd-label-mint-reply` — a reply leg carrying a hop's own part rewritten from strings to a
  label, byte-exact against what the router emits. The harness routes nothing, so the behavioural
  claim is bound by a core test, as `fwd/fwd-bound-forward` is bound by `bound_forward_test.cpp`.
- `fwd/fwd-label-stale` — a stale generation answers `NOT_FOUND` and delivers nothing.
- **`acl/label-vs-string-allow` and `acl/label-vs-string-deny`** — §8.2's mandated pair, asserting
  **byte-identical** outcomes between the two spellings. A by-construction argument is not a test;
  RFC-0014's lesson is that two silent misroutes shipped because no test used the production wiring.
- Zero existing vectors change: `PATH` with no label element is byte-identical to today's.

## 13. Rejected alternatives

- **A content hash instead of an assigned slot.** §4.2 — collision is mis-delivery and must be closed
  by construction, not by digest width.
- **Active label distribution** (an advertise/withdraw protocol, MPLS-LDP-shaped). Rejected: it buys
  nothing the passive reply rewrite does not already buy, and it costs a control frame family, a
  state machine, and a second invalidation mechanism beside the generation that works. §E.1 already
  demonstrates the advertise shape where it is warranted — for a delivery route that has no reply leg
  to ride.
- **Aging / TTL / LRU eviction of labels.** Rejected on two grounds: an aging axis is a synthetic
  limit (`CONTEXT.md` §Resource bound), and evicting a live label converts a bounded-resource
  condition into avoidable `NOT_FOUND` round trips on flows that were working (§8.3).
- **A per-route label instead of a per-element one** — i.e. one label naming the whole address.
  Rejected: it is §E.1, which exists (§2.1), and it forfeits §5.2's partial-deployment property,
  which is this design's distinguishing one.
- **A mode bit on `PATH` (`0x06`) selecting "this path is labelled".** Rejected for RFC-0024 §12's
  reason unchanged: it makes `PATH`'s canonical-bytes property conditional, and that property is what
  `path_lookup_key` and every consumer keyed on `PATH` bytes depend on. Elements self-describe by
  type (§5.1).
- **Extending `PATH_REF` to cover the terminus residual** instead of adding an element arm. Rejected:
  `PATH_REF` is a fixed-stride positional array by construction (RFC-0024 §4.2), and everything that
  makes §5.2's mixing safe depends on elements *not* being positional. It would also drag RFC-0024's
  erratum-1 strip rule onto a form that does not need it.
- **A negotiated capability for label support.** Rejected for minimalism: v1 has no capability
  negotiation and deliberately so (`CONTEXT.md` §Capability negotiation, ADR-0013). Nothing needs
  negotiating — a non-minting hop is indistinguishable from a hop that chose not to mint.
- **Doing #1266's intern-the-link-identity work inside this RFC.** Rejected as scope: #1266 and
  #1294 are node-local API changes that land independently and are useful without any wire change.
  This RFC cites them as motivation and substrate (§3.1, §10), never as its own deliverable.

## 14. What is UNMEASURED

Labelled per the standing rule — a quantitative claim names its instrument or is labelled.

- **Every byte figure in §3.3 and §5.3 is arithmetic** over the shipped encoding rule
  (`docs/reference/05-protocol-tlvs.md:381`) and the recorded mount-run shapes (RFC-0023:185-188),
  **under a byte layout that is itself deferred** (§5.3). Not a capture. If §5.3 question 1 lands on
  the packed-segment spelling, the table changes.
- **The per-hop and terminus savings of this design are UNMEASURED.** §3.2's numbers are #830's, for
  the *local* binding, and §2.3/§3.4 forbid transferring them. RFC-0024 §3.4's 261.7 vs 304.7 ns/hop
  is the closest available evidence and it is for a **different form**. §12.4 makes the measurement
  a condition of acceptance rather than a promise made here.
- **The receiver-demux band (140–184 ns across fan 1–64) is measured** by
  `bench/bench_forward_demux.cpp`, on one host, one link class. Its GENERALITY is unmeasured, and it
  prices *today's* hop, not the labelled one.
- **The subscribe-path figures (43 ns at 4 links, 57 ns at 65) are measured** by #1294's four-arm
  ablation, post-#1290. They price the cost this RFC's substrate removes, not a saving this RFC
  delivers.
- **The local-delivery figure (62–77 ns/delivery amortized) is cited only to establish that §9's
  scope exclusion is justified**, never as a benefit of this RFC.
- **§4.3's wrap arithmetic is arithmetic over an assumed mint rate.** No mint-rate measurement from
  a deployment exists — which is precisely why §11.1 collision 2 ~~flags~~ **refused to bank** the
  probability and ruled the **saturating** mitigation instead (§4.3.1). **Nothing now rests on that
  arithmetic**: it prices how cheap slot retirement is, and it is no longer load-bearing for
  correctness. What is still unmeasured is the **retirement rate** — how often a real deployment
  saturates a slot at all — which is §15 clause 3's remaining falsifier.
- **rv32 flash/RAM delta is UNMEASURED**, including the label table. The size census is the
  instrument; no "beat" is banked without it.

## 15. What would falsify this RFC

1. **A measured latency regression on any shipped shape** — especially the four-link `reply-spread`
   arm (§12.4 clause 3). This design dies the same death #504's memo did, and by the same rule.
2. **The labelled hop measures no cheaper than the mount descent.** If a label deref is not
   measurably cheaper than `resolve_mount_at` at realistic registry widths, §3.1 collapses and the
   case reduces to bytes alone — at which point §3.3 concedes a tie with `PATH_REF`, which already
   ships, and this RFC should be withdrawn in favour of extending RFC-0024 to the terminus residual.
3. ~~**The maintainer rules that §11.1 collision 2's wrap must saturate**~~ — **RULED 2026-08-15:
   saturate-and-retire (§4.3.1).** That half of this clause has fired and cost zero wire bytes.
   What survives as a falsifier is its second half: **the saturating variant proves unaffordable**
   — which now fires only if slot retirement turns out to churn the table in practice, i.e. if a
   real deployment retires slots fast enough to exhaust the table (§8.3's benign degrade becomes the
   common case rather than the rare one). That would be a **measurement**, not an argument, and it
   is the residual §14 names.
4. **The per-hop label table's RAM cost is not affordable on rv32** at a realistic mint count (§8.3).
   ADR-0067's census banked *"libtracer's own static RAM is approximately zero"*, and a table is not
   zero.
5. **The maintainer rules that three address compressions is one too many** (strings, `PATH_REF`,
   path labels). Then the design is sound but unwanted, and the answer is to extend RFC-0024's bound
   path to cover the terminus residual rather than to add a third form.
6. **A reachable sequence lets a stale label validate other than §4.3's.** If any path exists to a
   false validation that the generation does not catch, the element shape must be re-derived — §4.1's
   entire argument rests on the generation closing that class.

## 16. Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window",
this is an **amendment**: RFC plus maintainer approval, comment window **waived by default** while
solo-maintained (verified in the header; the waiver reverts the moment `docs/implementations.md`
gains a registered implementer). Scope is **v-NEXT**; the spec edits of §12 land in their own PRs
after acceptance.

The design of §§4–10 was **ruled** in the 2026-08-15 grilling session and is transcribed, not
proposed.

### 16.1 Acceptance ruling — 2026-08-15

**RFC-0027 is ACCEPTED.** Maintainer ruling of 2026-08-15, the same day the document was proposed;
comment window **waived by default** under GOVERNANCE.md's solo-maintainer clause and **not
invoked** — `docs/implementations.md:13` still reads `_(none yet)_`, so the waiver's revert trigger
has not fired and a window would have had nobody to serve. The three points the draft left open to
the maintainer were ruled together with acceptance:

1. ~~**§11.1 collision 1** — the vocabulary rule. Qualify RFC-0024 §2.1 to "path label", or rename.
   `CONTEXT.md` is deliberately not edited until this is ruled.~~ **RULED: (a) QUALIFY.** "**Path
   label**" is ratified as a distinct term; an unqualified "label" still means RFC-0004 §E.1's
   per-link `u16`, and RFC-0024 §2.1's rule is narrowed — not withdrawn — by **RFC-0024 amendment 3
   (2026-08-15)**. `CONTEXT.md` §12.2's entries are unblocked and land in the acceptance train.
2. ~~**§11.1 collision 2** — the 16-bit generation wraps where RFC-0024 §9.3 says a generation MUST
   NOT. The saturating mitigation costs zero wire bytes and is recorded so ruling it is cheap.~~
   **RULED: SATURATE-AND-RETIRE, NOT WRAP.** A slot whose 16-bit generation reaches its maximum
   saturates and the slot is **retired permanently** — never reused, never wrapped. RFC-0024's
   *"the generation MUST saturate, never wrap"* invariant is preserved **verbatim across both
   fields**, at **zero wire cost**: no byte, no flag and no frame changes. Normative text: **§4.3.1**.
3. ~~**§11.4** — whether the ADR claims listed there are qualifications (the reading taken here) or
   contradictions warranting a superseding ADR in the acceptance train.~~ **RULED: the per-hop
   mint table is ACCEPTED KNOWINGLY**, and RFC-0024's *"no hop holds anything"* is surrendered
   deliberately for per-element degradation and terminus compaction. §11.1 collision 3 records the
   decision; §11.4's four ADR entries are **qualifications, not contradictions**, so **no
   superseding ADR is opened**.

**What acceptance does NOT settle**, and what the acceptance train still owes:

- **§5.3 — the byte layout**, deferred pending conformance vectors in RFC-0014's discipline, with
  its three open sub-questions (TLV child vs packed-segment tag; multi-segment vs single-segment
  part; `path_lookup_key` admissibility). The **semantics** of §§4–10 are ruled; the **bytes** are
  not, and no wire surface is frozen by this acceptance.
- **§12.4 — the bench gate**, which is normative for acceptance of the **implementation**. An
  implementation that regresses any shipped shape — especially the four-link `reply-spread` arm —
  is rejected regardless of this ruling.
- **§12.1–§12.3 and §12.5** — the spec/reference incorporation, the `CONTEXT.md` entries, the code
  and the vectors, landing car by car per RFC-0024 §9's precedent.

Sustained objections and their resolution will continue to be recorded here.
