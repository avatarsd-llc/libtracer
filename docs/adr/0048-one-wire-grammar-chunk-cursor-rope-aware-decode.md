# One wire-grammar core behind a chunk-cursor: decode is rope-aware, sinks stay distinct, and the cast contract is the validating decode

Status: accepted. Concentrates the L2/L3 grammar that [ADR-0041](0041-terminus-arena-decode-span-contract.md) §5 deliberately forked (the fork's *targets* survive; its duplicated *grammar* does not); implements the rope-aware / link-walking decode CONTEXT.md §Two-compositions has promised since it was written; amends `docs/reference/08` §"Casting a view to a TLV" (the non-validating lazy accessor is dropped from the standard); maintainer-ratified 2026-07-04.

## Context

The wire grammar exists twice: `frame.cpp` `parse_one`/`decode` and `tlv_arena.cpp` `parse_header`/`decode_into` duplicate ~40 lines of header/trailer rules (type-0x00 reject, reserved-bit reject, `LL` width, trailer sizing, two-span CRC) plus the bounded iterative walk, held byte-for-byte equivalent only by a conformance-vector equivalence test — every future grammar rule is a two-file edit caught only if a vector happens to exercise it. Header *emission* is worse: hand-rolled in four places, with `op_resolve.cpp`'s `kStructOptMask{0x48}` a third representation of the `opt` bitfield bypassing `opt_t`. Meanwhile both decoders demand a **contiguous span**, so every scattered ingress (CAN reassembly rope, WS fragments) must flatten — one copy — even though CONTEXT.md commits to "a view boundary may fall anywhere, including mid-TLV-header — hence rope-aware / link-walking decode," and `reference/08` documents a rope-aware cast that was never implemented. Finally, `reference/08` specifies that cast as **non-validating** ("trusts the bytes") with a separate `view_validate_as_tlv`; the implemented `view_as_tlv` is the opposite (fully validating, materializing). The doc and the code cannot both be the standard.

## Decision

### 1. One grammar core, parameterized over a chunk-cursor (universal over byte *sources*, not access models)

The header/trailer grammar (`parse_header`) and the bounded iterative walk exist **once**, reading bytes through a small **chunk-cursor**:

- **span cursor** — the contiguous case, today's path, zero new cost;
- **rope cursor** — link-walking; a header or trailer that straddles a link boundary is reconstructed into a **≤ 16-byte stack scratch** (headers and trailers are small and bounded); **payload spans are always emitted zero-copy** as subviews (single link) or sub-ropes (straddling).

The cursor is a template internal to `tr::wire` — inside-a-module templating, exactly what ADR-0016/[ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) bless: a target that never links a rope-delivering transport never instantiates the rope cursor.

### 2. Sinks stay distinct — the access models were never the duplication

The one grammar feeds the existing consumers unchanged: the **terminus arena** (`decode_into`, ADR-0041 option B stands — resolve-scoped, pre-order, zero-heap under a stack `monotonic_buffer_resource`), the **owning `tlv_t` tree** (`decode` — API/tools/tests), and the **forwarder's offset peeks** (`fwd_router`'s hand-tuned reads of exactly three FWD fields — the only legitimate non-validating access, documented as an optimization, not a public cast). No lazy-accessor module is built: it has zero consumers (deletion test), and nothing in conformance needs it.

### 3. Emission unifies as the decode's dual: `compose` → rope

All header emission converges on `opt_t` + one cursor-capable emit primitive (`kStructOptMask` and the hand-rolled header pushes die). The app-built direction — a graph node constructed from typed TLV entities rather than parsed from wire bytes — is the **compose** module: a TLV-tree-of-views serializes to a **flat `rope_t`** (small owned header segments + payload views chained, zero-copy); `encode(t) = flatten(compose(t))` remains the contiguous convenience. There is **no nested-rope type**: "rope of ropes" resolves to composition on the *meaning* axis (the TLV tree) walking out flat on the *storage* axis (one rope) — the two-compositions rule of CONTEXT.md holds.

### 4. The cast contract: `reference/08` is amended to the validating decode

`view_as_tlv` **is** the decode entry point — validating (bounds, reserved bits, CRC, depth cap), rope-aware via the cursor (flat view = span fast path). The non-validating lazy accessor and `view_validate_as_tlv` are removed from `reference/08`; the forwarder's offset peeks are documented there as what they are — a net-plane optimization over already-validated framing, not a cast contract. If a real consumer for a lazy accessor ever appears (e.g. schema-aware L5 tooling over GB ropes), it can land additively over the same grammar core.

## Considered options

- **Keep two decoders, share nothing (status quo).** Rejected: the equivalence-by-test discipline already carries an admitted drift risk, and rope ingress would need a third copy of the grammar or a flatten.
- **Share only the header grammar, stay span-only.** Rejected: contradicts the glossary's standing rope-aware-decode commitment and leaves the CAN/WS ingress flatten (one copy per frame) permanent.
- **The `reference/08` lazy accessor as the unification.** Rejected: re-walks per access (worse than the arena for the terminus), has no current consumer, and would demote the two materializing decoders that real code paths depend on.
- **A nested `rope_t` (rope of ropes).** Rejected: conflates the storage and meaning composition axes; `rope_t::concat` already splices chains flat, and the emit-side tree lives at the TLV layer where it belongs.

## Consequences

- The wire grammar becomes a one-file change enforced by the compiler; the `tlv_arena_test` equivalence suite is retained as a regression net, no longer as the only guarantee.
- CAN delivers its reassembled group and WS its assembled frame as ropes that decode **without flattening** — the precondition for [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) §3's rope tier doing anything useful end-to-end.
- `reference/08` §cast and the frame/arena module docs are updated in the same change; the glossary needs no edit (it already promised this decode).
- Conformance vectors are unaffected (no wire change); the differential fuzzers gain a rope-source mode (same bytes split at adversarial link boundaries MUST decode identically to the contiguous case — including mid-header splits).
