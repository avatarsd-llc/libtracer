# Address-shift totality is opt-in: tail-slice loss is not guaranteed-detectable

An address-shift group ([03-addressing.md](../reference/03-addressing.md) §address-shift slicing) is identified by **`(origin_peer_id, ts)`** — the same in-flight identity as cycle-dedup ([02-graph-model.md](../reference/02-graph-model.md), [07-host-embedding.md](../reference/07-host-embedding.md)) — and a receiver detects a missing **interior** slice as an index gap at deadline. A dropped **trailing** slice, however, is **not** detectable unless the group's total is known: without it the assembler infers `N = max-observed-index + 1`, so a lost tail looks like a shorter-but-complete message. v1 makes totality **opt-in** — the publisher MAY declare it (`expected_count`, or a leading `:manifest` index-set TLV) — but the protocol does **not** require it. Tail-loss is therefore guaranteed-detectable only when the publisher opts in.

## Considered options

- **Require an `expected_count` / total on every address-shift group.** Rejected: open-ended/streaming publishers (a continuous ADC or camera feed sliced per-timestamp) cannot always supply `N` up-front. Forcing it would either block streaming or invite a sentinel meaning "unknown" — the opt-in case in disguise.
- **A mandatory end-of-group marker on the final slice.** Stream-friendly (detects the tail without knowing `N`) and tenable, but it is a new wire/QoS mechanism that would need its own RFC. Deferred as a possible future addition, not a v1 requirement.

## Consequences

- A receiver MUST NOT assume a group is complete merely because indices `0..max` are contiguous; "complete" is only knowable with declared totality.
- Group identity is `(origin_peer_id, ts)`, unifying with the cycle-dedup recent-set; the assembler retains `origin_peer_id` across a bridge that sheds the ROUTER (it is the *originating* publisher, not the immediate hop). This corrects the prior `ts`-only grouping, which merged same-timestamp slices from different publishers.
- `tr::flow::address_shift_gap` ([ADR-0009](0009-built-in-error-model-tr-concept-namespace.md)) reports a *detected* gap; **undetected tail-loss produces no error by design**.
- Descriptive / no wire change (reference-prose track). An end-of-group marker, if later wanted, is the RFC-track escalation.
