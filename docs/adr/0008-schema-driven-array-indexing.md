# Schema-driven array indexing: array-ness is an L4 schema property, not a wire type or `opt` bit

libtracer has **no wire-level array marker**. Whether a vertex field is an array — and whether its elements are fixed-size — is a property of the **vertex schema (L4)**, not of the TLV envelope (L3). On the wire an array is just a structured TLV (`opt.PL=1`) whose children are homogeneous; the bytes are identical whether or not anything treats them as an array. Indexed access `:field[N]` is resolved at L4: when the schema declares the field a **fixed-stride array** (all elements the same size), `[N]` resolves by direct offset (`base + N × stride`, O(1)) **on contiguous backing**; otherwise — variable-size elements, or an in-memory rope ([02-graph-model.md](../reference/02-graph-model.md) §rope) that scatters elements across segments — `[N]` resolves by **walking** children (O(n)). No `opt` bit is spent; the two forever-reserved `opt` bits (7, 0) stay reserved ([0002](0002-versioning-protocol-vs-release-no-per-frame-version.md); [01-data-format.md](../reference/01-data-format.md) §reserved bits).

## Considered options

- **A wire-level array/list type code, or an `opt.ARRAY` bit**, so a generic *schema-less* walker could index in O(1). Rejected: it would (a) burn one of the two forever-frozen reserved `opt` bits — a protocol-v2-only resource — for a benefit only a schema-less walker enjoys, and (b) re-introduce an L4 collection-semantic into the L3 envelope, the exact coupling that retiring `LIST` removed ([0003](0003-retire-list-type-code-0x05.md)). Every hot indexed-access path (`/camera/frame[N]`, address-shift `ep[N]`) is schema-aware and gets O(1) without a wire hint.
- **A per-array offset/index table prepended to the payload.** Rejected: costs bytes on every array and complicates the rope/view model for a marginal gain over a schema-declared stride.

## Consequences

- Element homogeneity is a **schema promise, not a wire-enforced invariant**: a receiver MUST NOT assume a `PL=1` payload is a uniform array without schema knowledge.
- The O(1) fast path is **contiguous-backing only**; the rope-walker falls back to link-stepping for arrays assembled in memory. This is an implementation-quality property, not a conformance gate.
- Reference prose must state array-ness as an L4 schema property and document the fixed-stride / walk split ([03-addressing.md](../reference/03-addressing.md) §index forms, [06-user-data-packing.md](../reference/06-user-data-packing.md), [02-graph-model.md](../reference/02-graph-model.md) §rope). This folds into the same pass as the `LIST` retirement sweep (the `03` "returns a LIST" lines).
- **No wire or conformance-vector change** — this is descriptive; it lands on the reference-prose track, not the RFC-gated track.
