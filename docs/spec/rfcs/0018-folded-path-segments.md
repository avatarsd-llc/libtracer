<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0018 — Folded path segments: a `PATH` may carry minted route labels beside literal names

| Field | Value |
| ---- | ---- |
| **RFC** | 0018 |
| **Title** | Folded path segments: a `PATH` may carry minted route labels beside literal names |
| **Status** | **draft** (2026-07-30) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-30 |
| **Comment window** | waived by default while solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"); invoke explicitly if outside input is wanted |
| **Tracking issue** | to be filed with this document |
| **Target spec version** | v1 (draft refinement — `docs/spec/v1.md` is DRAFT; **amends** [RFC-0004](0004-remote-operation-addressing.md) §E.1 and interacts with `docs/spec/v1.md` §3.1.1 — see §7) |

> **Numbering note.** 0012 and 0015 remain skipped for the ghost history
> [RFC-0016](0016-composed-branch-read.md) records. This RFC takes **0018**, the next unused
> number after [RFC-0017](0017-element-addressing-value-plane-index.md).

---

## 1. Summary

A `PATH` TLV is today a flat sequence of `NAME` children, and every one of them travels on every
frame. This RFC lets a `PATH` child also be a **`FOLD`** — a minted, per-link label standing for a
run of segments the receiving node has already resolved. `FOLD` and `NAME` children mix freely in
one `PATH`, so an address can be partly folded and partly literal:

```
PATH { NAME "net", NAME "ws-client", NAME "n1", NAME "sensor", NAME "temp" }   ; today
PATH { FOLD 0x0042,                                NAME "sensor", NAME "temp" }   ; folded prefix
```

The folded run is whatever the binding covers. An unbound hop simply carries names, so the two
forms interoperate on the same link and in the same frame.

This is **not** a new label mechanism. [RFC-0004](0004-remote-operation-addressing.md) §E.1
already defines per-link minted labels, advertise-driven binding, and hop-by-hop swap. What §E.1
does not do is let a label appear **inside an address**: today a label replaces the *whole* route
of a *delivery*, for *producer-driven streams*, gated on a `SUBSCRIBER` QoS flag. This RFC keeps
every one of those mechanisms and widens where the resulting token may be written.

## 2. Motivation

### 2.1 The measured cost of carrying a full path

Measured on host x86-64, Release, both arms in one binary
(`bench/bench_hop_chain.cpp`, `bench/bench_originate.cpp`, `bench/bench_forward_demux.cpp`):

| | full path | minted label | |
| --- | ---: | ---: | --- |
| Originated op, 1 hop | ~150 ns | ~107 ns | 79 B → 18 B frame |
| Warm op, 4 hops | 1437 ns | 395 ns | 146 B → 18 B frame |
| Wire across 4 hops | 588 B | 72 B | 8.2× |

A four-hop destination is **thirteen** segments — `net/<module>/<name>` per hop plus the leaf —
because the routing address *is* the vertex path ([ADR-0061](../../adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)).
Every hop parses all of it.

### 2.2 Why folding *partially* is the interesting case

The per-hop saving is **~260 ns at four hops against ~44 ns at one**. The difference is not hop
count — it is that each hop is parsing a *longer* address. Cost tracks **address depth**, so the
run worth folding is the deep local prefix on each host, not the end-to-end route. A scheme that
can only fold everything or nothing misses this: it forces a caller to bind the entire path before
it may elide the three segments that are expensive on the very next hop.

A decomposition of one 87 ns transit hop (`bench_forward_demux`, axis 3) puts **35 ns (40%) in
`peek_fwd_dst_segs`** — walking the `dst` PATH by offset, eight `read_fwd_header` calls — against
18 ns in the per-frame head rebuild. The registry scan a name lookup pays is **~0 at ≤16 links**
after the [#660](https://github.com/avatarsd-llc/libtracer/pull/660) name digest. **The cost is
reading the address, not matching it**, which is why a shorter address is the lever and a faster
lookup is not.

### 2.3 Why this is not already possible

§E.1's label plane is bound by three properties this RFC relaxes:

| §E.1 today | This RFC |
| --- | --- |
| A label replaces the **whole** route | A label folds **a run**; the rest stays literal |
| Applies to **deliveries** (producer-driven streams) | Applies to any address, including one-shot `read`/`write`/`await` |
| Requested by a `SUBSCRIBER` QoS flag (`delivery_compact`) | Requested by whoever originates the address (see [#504](https://github.com/avatarsd-llc/libtracer/issues/504)) |

§E.1's own net rule — *"one-shot ops pay the full route; high-rate established streams amortize it
to a label"* — is a **correct** rule that this RFC does not overturn. §6 preserves it as the
default and gives the measured threshold at which departing from it wins.

## 3. Wire encoding

`FOLD` is a new core type code, structured `opt.PL=0`, payload a little-endian `u16`:

```
FOLD (0xTBD, PL=0, len=2) { u16 label }
```

The label's value space, allocation and lifetime are **exactly** §E.1's: per-link, minted
monotonically by the node that will *receive* frames bearing it, `0` reserved for "none",
65 535 usable per link, swapped at each hop. This RFC introduces no second label namespace.

`PATH` (`0x06`) relaxes one clause. `docs/reference/05-protocol-tlvs.md` §`0x06` currently reads
*"Each child MUST be a NAME TLV (`type=0x02`); other types are invalid in PATH context."* It
becomes: each child MUST be a `NAME` (`0x02`) **or a `FOLD`**; any other type is invalid.

**Ordering.** A `FOLD` MAY appear at any child position. It is resolved in sequence like a `NAME`:
the receiver substitutes the run the binding names and continues with the following children.

**Nesting.** A `FOLD`'s binding MUST NOT itself expand to a `PATH` containing a `FOLD`. Bindings
are flat, so resolution terminates in one step and a hop cannot be made to chase a chain a peer
controls.

## 4. Semantics

1. **Minted, never computed.** A `FOLD` label is allocated by the receiving node. It MUST NOT be
   a hash of the folded segments. This is deliberate and is the project's existing ruling in a
   different place: `child_registry_t`'s name digest is *a filter, never a decision* — the scan
   reads `c.name_digest == want && c.name.size() == need && c.live() && matches(c.name, segs)`,
   so a digest collision costs a wasted compare and never a wrong delivery. A hash used as an
   address makes a collision a **silent misroute**. Minting also keeps the token 2 bytes: a label
   is small precisely because it is scoped to one link, where a globally content-addressed token
   would need to be large enough to make collisions negligible.
2. **Per-link scope, swapped per hop.** Unchanged from §E.1. A `FOLD` is meaningful only on the
   link it arrived over. A forwarding hop resolves it, and re-emits with **its own** token for the
   next link, or with literal names if it holds no binding there.
3. **Graceful degradation is mandatory, not optional.** A node that cannot resolve a `FOLD` MUST
   NOT guess. It replies `ERROR` with the code §E.1 already assigns to an unknown or stale label,
   which prompts re-advertise. A node that holds no bindings at all is conforming: it emits
   literal names and every peer must accept them.
4. **The originator chooses.** Nothing obliges a node to fold. §6 gives the condition under which
   it pays.

## 5. Invalidation, and the hard prerequisite

A stale `FOLD` is the one way this feature can be **wrong** rather than merely slow, so it gates
the RFC.

Within one address space libtracer already solves this twice, and
[ADR-0062](../../adr/0062-resolve-once-label-bindings-hold-resolutions-not-names.md) records why
the obvious approach fails: a cached `transport_t*` dangles, because `clear_link(L)` erases the
tables *keyed by* L and not a binding stored elsewhere that merely *points at* L. The shipped
answer is to cache the **registry slot**, which is nulled in place — the tombstone *is* the
invalidation — and, for a terminus, a **generation-stamped** handle.

**Neither mechanism crosses the wire.** A peer holding a `FOLD` holds a number in another node's
table. There is no slot to null and no generation to re-check without a round trip. Staleness
therefore surfaces as **misdelivery**, not as a clean miss — unless the binding is reference-held
and explicitly released.

This makes **[#603](https://github.com/avatarsd-llc/libtracer/issues/603) a hard prerequisite,**
not a related cleanup. It reports, verified in tree, that `route_handle`'s label tables grow
without bound on a peer-driven path, allocate through throwing `std::pmr` under
`-fno-exceptions`, and increment an unchecked `uint16` through the reserved `0` — while
`clear_link` restarts the allocator at 1 with a peer's ingress bindings still live. **Two flows
plus one reconnect already misdeliver today with §E.1's single whole-route labels.** Widening
where labels may be written before that is closed multiplies a live correctness bug.

The intended shape, to be settled in #603 and referenced here rather than duplicated: a binding is
**reference-held with explicit release**, so exhaustion is a bounded resource question (a leak,
observable and fixable) and never a misroute. The bound is derived from the injected resource, not
a magic number.

## 6. When folding loses — and the measured threshold

A `FOLD` is not free. The binding must propagate before it can be used, and that cost lands on
the first operation.

Measured over four hops (`bench_hop_chain`), cold is a **single timed operation including the
ADVERTISE walk**, so read it as a direction and not a tight magnitude:

| | cold (first op) | warm (steady state) |
| --- | ---: | ---: |
| full path | ~16.5 µs | 1437 ns |
| folded | ~22 µs | 395 ns |

The binding costs **~5–6 µs extra once**, and saves **~1042 ns each time after**, so it pays for
itself at roughly **five operations to the same destination**. Below that the full path is
cheaper, and **a caller that addresses a destination once should not bind at all.**

This is the measured form of §E.1's existing rule, and it is why this RFC does not make folding a
default. It also bounds the state: a node holds bindings only for addresses actually re-used, so a
node serving fifty cold one-shot reads holds zero.

## 7. Normative interaction: `v1` §3.1.1 byte-equivalence

`docs/spec/v1.md` §3.1.1 requires that a path handle *"MUST resolve, deterministically and without
allocation, to a byte sequence equal to the canonical PATH TLV"*, and calls the byte-equivalence
requirement *"the load-bearing one: a write through a path handle MUST be indistinguishable on the
wire from a write through the equivalent string-form path."*

**A folded `PATH` is by construction not byte-identical to the string form.** This RFC must
therefore either narrow that clause or stay outside it. It stays outside it, and the distinction
is worth stating precisely because it is the clause most likely to be misread as blocking this
work:

- §3.1.1 governs the **path handle** — the application-facing object guaranteeing that addressing
  a vertex by a pre-encoded handle is indistinguishable from addressing it by string. That
  guarantee is about *the application's* two ways of naming the same vertex, and it is preserved
  verbatim: a handle still renders literal `NAME` children.
- A `FOLD` is a **transport-scoped substitution applied when the frame is emitted onto a
  particular link**, by the node that holds a binding for that link. It is the same class of thing
  as §E.1's existing label swap, which already produces frames not byte-identical to their
  full-route form and which §3.1.1 has never been read to forbid.

**Proposed clarification** to §3.1.1, as part of this RFC: byte-equivalence binds the *path handle
API*, not the *emitted frame*, and a conforming implementation MAY substitute a bound `FOLD` for a
resolved run at egress. Without this sentence the two clauses can be read as contradictory, and
that reading should not be left to a future implementer to discover.

## 8. Alternatives considered

- **Keep §E.1 as-is; require whole-route labels.** Rejected on §2.2: the cost tracks address
  depth, so the case worth optimising is the deep local prefix, and an all-or-nothing label
  cannot express it without binding the entire route first.
- **Content-addressed (git-style) folding.** A hash of the folded segments needs no negotiation,
  which is genuinely attractive. Rejected on three grounds: a collision becomes a misroute,
  reversing the #660 filter-never-a-decision ruling; the token must be large precisely because it
  is unscoped, erasing most of the wire saving (four 16-byte digests against a 79-byte frame); and
  computing it is expensive on the target that needs it most — an FNV-1a digest over a short name
  measured **~30 ns** and had to be replaced with a three-multiply per-segment fold. A Merkle-style
  digest remains a good fit for *validating* a cached binding — "has this subtree changed?" — and
  that is a separate question from naming a destination.
- **Flatten the frame layout instead.** The 35 ns is eight `read_fwd_header` calls, so a
  fixed-offset header would capture part of the same win with no binding, no minting and no
  release protocol. This is **not** mutually exclusive with this RFC and is the cheaper experiment;
  it is recorded here so it is not lost, and it does not address the wire-size term at all
  (146 B → 18 B), which is what matters on CAN and on a metered link.

## 9. Open questions

1. **Type code for `FOLD`.** Core codes are `0x01–0x1F` and the space is not empty; the assignment
   should come from the registry rather than this document.
2. **Does `src` fold too?** §B accumulates the return route by prepending one `NAME` per hop, a
   rope head-insert that never moves existing bytes. Folding `src` would preserve the measured
   saving on the reply leg — the ×2 across a request/reply pair — but the prepend's zero-copy
   property needs re-checking against a mixed child sequence before this is claimed.
3. **Interaction with `retire()`.** A folded run naming a vertex that is later retired and revived
   is the terminus half of ADR-0062's generation problem, arriving over the wire. The generation
   stamp is local; what a peer holds is not.
4. **Conformance vectors.** A mixed `PATH` needs vectors in both directions, plus a
   negative vector for the unresolvable-`FOLD` reply, before this leaves draft.

## 10. Prerequisites and sequencing

1. **#603 — bounded, non-throwing, wrap-safe label tables.** Hard prerequisite (§5).
2. **#504 — client-originated binding.** This RFC is the wire form that proposal needs; #504 is
   the negotiation half.
3. This RFC.

The measurements in §2 and §6 exist and are reproducible today
(`bench_forward_demux`, `bench_originate`, `bench_hop_chain`). The instrument that would falsify
this RFC — a folded arm failing to beat the literal arm on a warm multi-hop address — is the same
one that produced the numbers above, and it asserts terminus delivery so an arm that silently
drops cannot be reported as fast.
