# Lazy rope-backed decode view: `tlv_view_t` and partial-path routing

Status: **accepted** (2026-07-06 — maintainer-ratified in design review; this is the
architecture that realizes the direction ratified in
[ADR-0052](0052-rope-aware-decode-sink-node-type.md): steady-state zero-copy rope decode
is a **hard requirement**, including WS/CAN reassembly, delivered by a **new lazy
decode-side type** rather than by mutating the existing sinks).

## Context

[ADR-0052](0052-rope-aware-decode-sink-node-type.md) established the crux: both existing
decode sinks name payload bytes with a borrowed contiguous `std::span`
(`tlv_t.payload`, `arena_tlv_t.wire`/`body`), and a rope-delivered payload breaks both
the contiguity and the single-borrowed-buffer assumption those spans bake in. The
maintainer ratified the zero-copy direction and rejected both flatten-once-as-end-state
and mutating the existing types. The root glossary
([CONTEXT.md](../../CONTEXT.md)) had in fact already committed to the same direction:
reassembly *is* rope construction ("zero-copy, never `memcpy`"), a contiguous copy is
legal only at a transport-**egress** DMA boundary, and a view boundary may fall anywhere
— including mid-TLV-header.

The parsing engine already exists: `grammar::rope_cursor` (#225) walks a rope through
the one grammar core ([ADR-0048](0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md)
§1), stitching straddled header fields and feeding CRC per-link, with the #227
differential fuzzer proving it byte-equivalent to the span cursor over 131k adversarial
splits. What was missing is the *output side*: the type a decoded rope frame becomes,
and *when* decoding happens at all.

The second ratified requirement reshapes that question. Delivery is hop-by-hop
source-routing ([CONTEXT.md](../../CONTEXT.md) §addressing): a hop needs only the next
PATH segment to forward a frame, and only the terminus needs the payload. Eagerly
decoding a whole frame at ingress therefore does work nobody asked for. The ratified
model is **partial decode**: parse only what the current consumer touches, hand the rest
onward as the rope region it already is.

## Decision

### 1. A lazy decode-side node: `tr::wire::tlv_view_t`

A new type in `tr::wire` (it interprets TLV meaning, so it lives at L2/L3 — same
placement rule as `view_as_tlv`) representing **one TLV whose bytes live in a rope**:

- Holds the decoded header facts (`type`, `opt`, lengths) plus **rope sub-regions** for
  the body and trailer — not copies, not spans: regions of refcounted `view_t` links.
- **Children are materialized on demand**, one `grammar::parse_header` over a
  `rope_cursor` per step: `child(i)` / iteration yields the next child's `tlv_view_t`
  by parsing only that child's header; the child's *body bytes are never touched*.
- **Nothing is decoded that is not accessed.** A structured payload handed to a
  consumer that does not descend into it is delivered as an opaque rope region — "up to
  the consumer to deal with it" is the contract, not a degraded mode.
- `materialize() → tlv_t` is the **single explicit copy point**: a consumer that wants
  the eager owning tree (or contiguous bytes) asks for it, and only then is anything
  flattened. `tlv_t` itself is untouched — it remains the eager encode-side /
  materialized representation and the cross-core parity type
  ([ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md)/[ADR-0032](0032-continuous-cross-core-perf-conformance-matrix.md)).

### 2. Ownership: the lazy view refcounts its links

`tlv_view_t` holds refcounts on the `view_t` links its region spans — the L1 substrate
already provides exactly this, so no new ownership machinery exists. A `tlv_view_t`
(and any sub-view handed to a next hop) keeps precisely its own links alive and may
outlive the transport read loop.

This is a **scoped revision of [ADR-0041](0041-terminus-arena-decode-span-contract.md)
§2**: the span arena (`tlv_arena_t`) remains pure-borrow ("holds structure only, never
owns bytes") for the contiguous non-owning tier; the lazy view tier **owns via
refcount** — that is what the owning delivery tier ([ADR-0042](0042-refcounted-receiver-seam-view-delivery.md),
generalized to ropes per the glossary) hands upward, and the two tiers do not mix.

### 3. Partial-path routing

Routing consumes the lazy view incrementally: a hop parses **only the PATH prefix it
needs** (the next NAME child), strips its segment, and forwards the remaining suffix +
payload **as rope sub-regions** — zero decode, zero copy of anything downstream of the
current hop. Final decoding of the payload happens at the terminus; a structured write
payload (several folded TLVs) may cross the entire graph undecoded.

### 4. Validation: fully lazy — bounds anchored at ingress, integrity at access

At ingress, the receiving edge does the **minimum that makes lazy access memory-safe**:
parse the root header fields and check `root.total == rope size`. That is a handful of
byte reads plus an O(links) size sum — **no byte-walk, no CRC, no structural descent**.
With the root bounds anchored, grammar containment does the rest lazily: every child
materialization checks that the child's declared extent fits inside its parent's region
(the cursor already enforces this, returning `err_t` on violation), so out-of-bounds
access is impossible without ever walking bytes that nobody reads.

**All integrity checking is deferred to access time, per TLV.** `opt.cr` is a per-TLV
trailer; the consumer that materializes a TLV verifies *that TLV's* trailer then — the
end-to-end argument placed in the codec. An ingress CRC pass was deliberately rejected:
it touches every byte, which is the same O(n) memory traffic as the flatten this design
exists to eliminate (a software CRC on an MCU costs *more* per byte than `memcpy`), and
it duplicates the link-layer integrity the transports below already provide (TCP/QUIC
checksums, CAN frame CRC).

**Partial-consumption is a feature, not a failure mode.** If one member of a TLV list is
corrupt, its own access fails with `err_t`; its siblings deliver normally; what to do
about the gap is the **final endpoint's policy** — the
[ADR-0051](0051-delivery-terminates-at-target-no-dispatch-limits.md) doctrine (delivery
terminates at the target; no middle-of-the-path gatekeeping) applied to integrity.
Every lazy accessor returns `std::expected` with the same `err_t` vocabulary as
`decode`.

The one hazard this places on endpoint authors is **torn application**: a structured
write whose members are one semantic transaction must not be applied member-by-member
as accesses succeed. The pattern for such an endpoint is **verify-all-then-apply** —
walk the payload's members (bounds + trailers) to completion *before* mutating any
state; the walk touches only bytes the endpoint was about to read anyway, so it costs
nothing extra over eager ingress validation while keeping the choice at the terminus,
where atomicity is actually definable. (Note an eager ingress CRC pass would *not*
provide this guarantee anyway: `opt.cr` is optional per-TLV, and a trailer-less frame
can still be semantically torn by a mid-list grammar error.)

`wire::validate_rope` (#225) remains available as an opt-in strict screen for
deployments that want full structural + CRC verification at ingress; it is not coupled
to the type.

### 5. Transports hand ropes upward

WS message reassembly and CAN frame-group reassembly construct ropes by chaining views
over their receive segments (the glossary's definition of reassembly) and deliver them
through the owning tier as-is. The pre-existing flatten-at-ingress paths are migration
debt, removed as each transport converts. A contiguous copy remains legal only where
the glossary already permits it: a substrate boundary the transport's own DMA cannot
span, at **egress**.

## Considered options

- **Mutate `tlv_t.payload` / `arena_tlv_t` to rope-capable bytes** ("A1") — rejected.
  It pays the cross-core parity break and per-node refcount cost on the *encode* side
  and the contiguous fast path, where ropes buy nothing; and eager decode is the wrong
  model anyway once routing is partial (§3). ADR-0052's rejection of its option (A)
  *as literally proposed* stands for this shape.
- **Eager rope arena** (`rope_arena_t`, ADR-0052's option (B)) — subsumed. A lazy view
  is strictly more capable (it can always walk fully; an eager arena can never un-walk),
  uses the same `rope_cursor`, and avoids a second arena + resolver surface.
- **Flatten-once at ingress** (ADR-0052's option (C)) — demoted to the **interim
  migration recipe** (`validate_rope → flatten → decode_into`) for paths not yet
  converted, and to the explicit `materialize()` escape hatch. It is not the end state;
  as end state it contradicts the glossary's "never `memcpy`" reassembly rule.
- **Hybrid validation (frame-CRC byte-walk at ingress, structure lazy)** — rejected:
  the CRC pass touches every byte, the same O(n) memory traffic as the flatten this
  design eliminates (software CRC on an MCU costs more per byte than `memcpy`); it
  duplicates the transports' link-layer integrity; it re-verifies nothing for the
  common trailer-less frame (`opt.cr` is optional); and per-hop re-checking would not
  localize the corruption classes that end-to-end trailers exist to catch. Torn
  application — the real hazard eager checking is imagined to prevent — is handled at
  the terminus by verify-all-then-apply (§4), the only place atomicity is definable.
- **Eager full structural validation at ingress** — not the default (every frame would
  pay a full parse when routing reads only a PATH prefix), but retained opt-in via
  `validate_rope`.

## Consequences

- **New public API** (`tlv_view_t`, ingress hook), each with a CHANGELOG note. No wire
  bytes change — this is decode-side only, so no spec RFC is required.
- **[ADR-0041](0041-terminus-arena-decode-span-contract.md) §2 contract text** gains
  the scoped carve-out of §2 above; the span arena's own contract is unchanged, and the
  Cortex-M0 span-only sentinel never links the rope TU (unchanged from #225).
- **Differential oracle extends**: a full lazy walk + `materialize()` over any rope
  split must equal `decode(flat)` node-for-node — the #227 fuzzer harness gains that
  mode. Late-surfacing grammar errors must also agree with `decode`'s `err_t`.
- **Cross-core**: `tlv_t` parity is untouched. TS/Rust cores add their own lazy view
  type independently, conformance-checked through the same wire vectors; until then
  they interop unchanged (wire bytes are identical by construction).
- **Emission symmetry** ([ADR-0048](0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md)
  §3 `compose → rope`): the encode-side dual (TLV-tree-of-views → flat rope, zero-copy
  egress until the DMA boundary) is now unblocked — it shares the ownership model
  ratified here and should be designed against `tlv_view_t`'s region vocabulary.
- **Migration order** (each step its own PR, each keeping the differential oracle
  green): ① `tlv_view_t` + ingress integrity screen + lazy-walk fuzzer mode;
  ② owning-tier delivery of ropes end-to-end (WS reassembly → rope); ③ CAN reassembly
  → rope; ④ partial-path routing consumes `tlv_view_t` at forwarding hops;
  ⑤ `compose → rope` emission; ⑥ remove interim flatten call-sites.
