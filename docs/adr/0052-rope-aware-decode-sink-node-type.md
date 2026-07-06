# Materializing a rope-delivered frame: the decode sink node type

Status: **accepted** (ratified 2026-07-06 in maintainer design review — **not** as this
document originally recommended; see *Ratification outcome* below. This is the
design-decision half of the [ADR-0048](0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md)
rope-aware-decode arc; the validation half — the rope cursor + `wire::validate_rope` —
landed in #225, and the differential fuzzer in #227. The ratified architecture is
specified in [ADR-0053](0053-lazy-rope-backed-decode-view-partial-path-routing.md).)

## Context

[ADR-0048](0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md) §1 promises that decode is rope-aware: a frame delivered as a scatter-gather rope (CAN reassembly, WS fragments) decodes with "**payload spans always emitted zero-copy as subviews (single link) or sub-ropes (straddling)**." #225 delivered the read-only half — `grammar::rope_cursor` + `wire::validate_rope`, which walk a rope through the one grammar core and reject a malformed frame **without flattening**. What #225 deliberately did **not** do is *materialize* a rope into a sink the terminus can dispatch from.

The blocker is concrete and load-bearing. Both materializing sinks name payload bytes with a **borrowed contiguous `std::span`**:

- `arena_tlv_t.wire` / `arena_tlv_t.body` — `std::span<const std::byte>` into the single inbound frame (`tlv_arena.hpp`), the ADR-0041 §2 contract: *"the arena holds structure only, never owns bytes."*
- `tlv_t.payload` — `std::span<const std::byte>` (`frame.hpp:55`), borrowing the input buffer; this type is kept **byte-identical across the C++/TypeScript/Rust cores** ([ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md)/[ADR-0032](0032-continuous-cross-core-perf-conformance-matrix.md) parity).

A rope-delivered payload breaks two assumptions a `std::span` bakes in:

1. **Contiguity.** A payload that straddles a link boundary has no single contiguous span.
2. **One borrowed buffer.** The bytes live in one-or-more **refcounted segments** (the rope's `view_t` links), not a single caller-owned input buffer. Even a *single-link, non-straddling* payload is a span into a segment whose lifetime is the rope's, not the terminus caller's — so the sink would have to hold a refcount to keep it alive, which the "never owns / pure borrow of the input" contract forbids.

So the promised zero-copy rope decode cannot be expressed against today's sink node types without changing what a sink node's bytes *are*. That change is the decision this ADR asks for.

Note the current fallback already works and is not broken: a rope-delivering transport can `validate_rope` (cheap reject, #225) and then `flatten()` **once** into a contiguous segment and `decode_into` the existing span arena. That is one copy per delivered frame — correct, just not zero-copy.

## Options as proposed (kept as record; superseded by *Ratification outcome* below)

The proposal recommended adopting **(C)** now with **(B)** as a measured-need
escalation, and rejecting (A). The maintainer ratified differently.

- **(C) Validate-then-flatten-once at rope ingress (no new sink type).** Keep `arena_tlv_t` / `tlv_t` exactly as they are. A rope-delivering transport calls `wire::validate_rope` (rejects a bad reassembled frame with no copy — the #225 win) and then, only for a good frame, `rope_t::flatten()` **once** into a contiguous refcounted segment that the terminus arena borrows as today. Cost: one bounded copy per *delivered* frame; **zero** copy for the (common) rejected-garbage case, which previously paid the flatten before it could even be rejected. No contract change, no cross-core change, smallest blast radius. This is the honest minimum and it fully closes the ADR-0048 §1 *"decode a rope without a flatten-then-reject"* obligation; it leaves only the *steady-state zero-copy-good-frame* payload unrealized.

- **(B) A parallel rope-native sink (`rope_arena_t`), gated on a measured hot path.** Introduce a second arena type whose node bytes are a `view_t` (single-link, a refcount-bumped subview — zero copy) with a `rope_t` fallback for a straddling payload (zero copy), decoded through the *same* `grammar::rope_cursor` + iterative walk (no grammar duplication — that is exactly what #225's cursor seam buys). The span arena (`tlv_arena_t`) and its ADR-0041 §2 contract are **untouched**; a transport that delivers ropes and has proven the per-frame flatten is a bottleneck opts into `rope_arena_t`, and the resolver gains a rope-arena overload (or is templated over the two node types). Additive, no contract break, but it is a second sink + resolver surface, so it should land against a **measured** CAN/WS throughput need, not speculatively (the same "design against a measured need" discipline ADR-0043 §3 applied to per-flow QUIC streams).

- **(A) Generalize the existing node's byte reference to `view_t | rope_t` — rejected.** Replacing `arena_tlv_t.body` (and, to be uniform, `tlv_t.payload`) with a view-or-rope slice type would make one code path zero-copy everywhere, but it: breaks the ADR-0041 §2 borrowed-span-only contract for *every* terminus (including the contiguous fast path that never wanted it); enlarges every node (a `view_t` is `segment_ptr_t` + two `size_t` ≈ 24–32 B vs a 16 B span) and adds a refcount bump per node on the hot terminus path; and, if applied to `tlv_t.payload`, mutates the cross-core parity type that three implementations keep byte-identical — a change that would have to be mirrored in TS and Rust for no benefit to their (span-only) decoders. The generality is unbought: outside a rope terminus, no reader needs it — the same reason ADR-0041 chose the terminus-local arena (option B there) over a general `tlv_view` codec (option A there).

## Considered options

- **Do nothing / leave it at flatten (status quo before #225).** Rejected as the *stated end state* only because ADR-0048 §1 explicitly commits to rope-awareness; but note (C) is very close to status quo — it keeps the flatten and adds only the cheap pre-flatten reject, which #225 already shipped. (C) is "status quo, honestly labelled as sufficient until measured otherwise."
- **(A) view-or-rope node bytes** — rejected above (contract break for all paths + cross-core blast radius + per-node cost).
- **(B) parallel rope arena** — held as the escalation, not adopted now (avoid a second sink + resolver surface without a measured need).
- **(C) validate-then-flatten-once** — recommended default.
- **A `view_t`-payload `tlv_t` for the owning tree** (needed for zero-copy `compose → rope` emission, ADR-0048 §3) — noted as **coupled**: the emit-side "TLV-tree-of-views → flat rope" also wants an owned-view payload, so if (B)/(A) is ever taken it should be co-designed with the compose module rather than bolted on. Under (C) both stay deferred together.

## Consequences

*(As drafted for the proposal; operative consequences now live in
[ADR-0053](0053-lazy-rope-backed-decode-view-partial-path-routing.md). Still-true parts:
the ingress recipe below survives as the interim migration path, and the Cortex-M0
span-only sentinel remains unaffected. No longer true: "`tlv_t` parity is preserved
under both (C) and (B)" — parity **is** preserved, but by ADR-0053's new-type shape,
not by adopting (C)/(B).)*

- **Under (C) (recommended):** no code churn beyond documenting the ingress recipe (`validate_rope` → `flatten` → `decode_into`) and, optionally, a helper `wire::decode_rope(rope, mr)` that performs exactly that (flatten once + span arena). ADR-0041 §2 is untouched. The Cortex-M0 sentinel is unaffected (the rope cursor is already its own TU, not linked by a span-only target). The only thing not delivered is steady-state zero-copy for a *straddling good frame* — quantify it before spending (B)'s complexity.
- **If (B) is later ratified:** a new `rope_arena_t` / `rope_tlv_t` + a resolver overload; the differential oracle extends naturally (the #227 fuzzer already proves the rope *grammar* matches the span grammar — a rope-arena would reuse the same walk, so equivalence is `rope_arena` node-for-node vs `decode`). ADR-0041 §2's contract text gains a scoped carve-out ("the *rope* arena's nodes hold refcounted `view_t`/`rope_t` slices; the *span* arena remains pure-borrow").
- **`tlv_t` parity is preserved** under both (C) and (B) — neither touches the cross-core type, keeping the ADR-0028 three-core equivalence intact. Only (A), rejected, would have disturbed it.

## Ratification outcome (2026-07-06)

The maintainer answered the three questions as follows:

1. **(C) is NOT the end state.** Steady-state zero-copy rope decode **is a hard
   requirement now** (question 2's branch), explicitly including WS message reassembly
   and CAN frame-group reassembly delivering ropes upward. On review this was already
   the position of the root glossary — [CONTEXT.md](../../CONTEXT.md) defines
   reassembly as *"constructing a rope by chaining views — zero-copy, never `memcpy`"*
   and permits a contiguous copy only at a transport-**egress** DMA boundary — so this
   proposal's (C)-as-end-state recommendation was in tension with an existing
   load-bearing commitment, and the ratification resolves that tension in the
   glossary's favor. (C) is demoted to the **interim migration recipe** for
   not-yet-converted paths and the explicit `materialize()` escape hatch.
2. **The realization is neither (B) nor (A) as written**, but a shape this proposal did
   not contain: a **new lazy decode-side type** (`tr::wire::tlv_view_t`) with
   on-demand child materialization, partial-path routing, and fully lazy validation —
   specified in [ADR-0053](0053-lazy-rope-backed-decode-view-partial-path-routing.md).
   It subsumes (B): an eager `rope_arena_t` is strictly less capable than a lazy view
   over the same `rope_cursor`, and eager whole-frame decode does work partial routing
   never asks for.
3. **(A) as literally proposed — mutating `tlv_t.payload` / `arena_tlv_t` — stays
   rejected.** `tlv_t` remains the eager encode-side / materialized representation and
   the cross-core parity type; the span arena keeps its ADR-0041 §2 pure-borrow
   contract. The zero-copy *goal* of (A) is achieved by the new type instead, and the
   compose→rope emission tier (ADR-0048 §3) is now unblocked and couples to
   ADR-0053's region vocabulary, not to a `tlv_t` mutation.
