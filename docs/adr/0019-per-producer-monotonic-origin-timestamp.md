# `origin_timestamp` is a per-producer monotonic (hybrid-logical-clock) value, not wall-clock, so the `(origin, ts)` identity survives clock divergence

The in-flight identity of a TLV is the pair `(origin_peer_id, origin_timestamp)`. It is the dedup key for the cycle recent-set ([reference 07](../reference/07-host-embedding.md) §cycle handling, [ADR-0014](0014-router-cycle-termination-hop-count.md)) and the group key for address-shift slice reassembly ([ADR-0011](0011-address-shift-totality-opt-in.md)). Reference 07 currently defines `origin_timestamp` as *"the wall-clock time (ns since epoch) at which the TLV was first published."* Designing the io_layer substitution against "a coherent system where node clocks diverge" exposed that this definition is **incorrect for an identity**: wall-clock is neither unique nor monotonic per producer.

## Decision

**`origin_timestamp` is a per-producer monotonic value — a hybrid logical clock (HLC), not literal wall-clock.** It is strictly increasing per `origin_peer_id`, never regressing or colliding: wall-clock-*seeded* where a clock is available, but **bumped logically** when the clock is too coarse to separate two writes, or when it would regress (NTP backward correction). The wire field and the **`(origin_peer_id, origin_timestamp)` identity tuple are unchanged** — only the *contract* on `ts` is sharpened — so ADR-0011 and ADR-0014 stand without modification.

**Consequences for meaning:** wall-clock interpretation is **advisory** (display, coarse correlation). **Cross-producer ordering is undefined by design** — two producers' timestamps are never comparable (consistent with [reference 04](../reference/04-communication-flows.md): no global clock, no CRDT, no vector-clock causality in v1). What the protocol guarantees is **per-producer total order** and a **collision-free identity**, both of which hold *regardless of how far node clocks have drifted*.

**`ts` and coherent sampling.** A `ts` is optional on any TLV (`opt.TS`). Its primary semantic use is **coherent sampling**: endpoints a producer stamps with the **same `(origin, ts)`** form one coherent sample-group / snapshot — the *same* group primitive that address-shift slicing uses for a split payload. Cross-producer coherence requires a coordinated trigger or external clock sync; it is never obtained by comparing `ts` across origins.

## Considered options

- **Keep `origin_timestamp` as literal wall-clock.** Rejected — it is the bug: on a coarse MCU clock two writes share a tick, or an NTP step makes `ts` regress, so two distinct TLVs collide on `(origin, ts)` → one is wrongly deduped, or two address-shift groups merge. An identity must be unique and monotonic; wall-clock is neither.
- **Add a separate `origin_seq` to the identity tuple** (the io_layer approach — it carries both `ts_us` and a monotonic `seq`). Rejected for v1: it is the cleaner *separation* of concerns, but it **changes the identity tuple** and ripples through ADR-0011, ADR-0014, and reference 07's wire description. Redefining `origin_timestamp` as monotonic achieves the same correctness while **preserving the tuple** and touching no other ADR.
- **Vector clocks / causal ordering.** Rejected: cross-node causal consistency is an explicit v1 non-goal ([reference 04](../reference/04-communication-flows.md)) and is heavy for MCUs; per-producer monotonicity is all the identity needs.

## Consequences

- Cycle-dedup and address-shift grouping are **correct under clock divergence, low-resolution clocks, and NTP backward jumps** — the property the divergent-time use case requires.
- Producers must maintain a **monotonic per-origin counter** (cheap: `max(wall_clock, last+1)` HLC update). Bridges set `origin_timestamp` when first putting a TLV on the wire (reference 07), so the monotonic source is the first bridge's HLC.
- **Reference 07's definition of `origin_timestamp` is corrected** from "wall-clock" to "per-producer monotonic (HLC)."
- **Coherent sampling** gains a precise meaning (shared `(origin, ts)` = one sample-group), unifying it with address-shift slicing rather than being a separate mechanism.
- No wire-format change; this is a sharpened contract on an existing field, so no RFC against the immutable spec is required.
