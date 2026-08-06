# The CAN advertise carries a producer generation, and reassembly is keyed by it — not by the recurring base endpoint

Status: **proposed** (2026-08-06, for [#909](https://github.com/avatarsd-llc/libtracer/issues/909)).

The CAN transport shifts a libtracer path onto a scarce 12-bit endpoint space (`can::kEndpointBits = 12`, `kEndpointMax = 4095`), allocating a run of `slice_count` slots per send and **wrapping** the base back to `kCanFirstDataEndpoint` when the space is exhausted (`transport_can::alloc_base`, `core/src/transport_can.cpp:158-166`). Base endpoints therefore recur after ~4094 consumed slots — this is routine, not exceptional. We decide that a producer-monotonic **generation** rides every `advertise_t`, and that the receive-side reassembly group is identified by `(origin, generation)`, so a reused base can never be confused with a live or stale binding of the same base.

## Context

Two receive-side maps key on the base endpoint, and both alias under wraparound:

1. **`learned_` (base id → binding).** Written only via `operator[]` on the exact base id and never erased (`transport_can.cpp:307`). After the endpoint space wraps, the map holds overlapping stale ranges. `process_data` resolves a slice by scanning `learned_` in ascending base order and taking the **first** binding whose `[base, base + slice_count)` contains the endpoint (`:335-346`) — so a stale, wider range with a lower base **shadows the live binding**, filing the slice under the wrong group at the wrong index.

2. **The reassembly group key.** `reassembly_key_t` is nominally `(origin, ts)`, and `ts` is documented as "the group's per-producer monotonic timestamp" (`core/include/libtracer/can_reassembly.hpp:61-69`) — a slot that exists precisely to distinguish two groups from the same origin. But the CAN path never populates it as a generation: `key_of(node, base_endpoint)` stuffs the **base endpoint** into `ts` (`transport_can.cpp:77-78`). So the effective key is `(origin, base_endpoint)` with no generation, and a recurring base merges slices left over from an incomplete group thousands of sends ago into the new group. `is_complete` can then be satisfied by a mix of old and new slices, and a **byte-corrupted frame is delivered as valid** — a silent data-integrity failure, not a crash.

The affordance for the fix already exists in the type: `reassembly_key_t` was built with a generation slot; only the CAN producer's failure to mint and carry a generation, and `key_of`'s substitution of the base endpoint for it, leave it unused.

## Decision

1. **`advertise_t` gains a producer-monotonic `generation` (`seq`) field**, incremented per group by the sending node, encoded into the advertise frame. This is a change to the CAN transport's advertise framing — **not** to the normative v1 wire protocol (`docs/spec/v1.md` defines no CAN framing; CAN framing is reference/implementation-domain and the protocol is DRAFT).

2. **`key_of` uses `(origin, generation)`** — populating the `ts` slot with the value it was always meant to hold — so the reassembly group identity is deterministic across base recurrence. Reused base endpoints produce distinct keys; stale slices can never merge into a fresh group.

3. **`learn_advertise` erases overlapping stale `learned_` entries** for the same node before inserting the new binding, so base-id resolution cannot be shadowed by a stale wider range. This half is pure local logic with no wire effect and is separable from (1)/(2).

## Considered options

- **Base endpoint as the reassembly key (status quo).** Rejected: the endpoint space wraps by design, so the key is not unique over the node's lifetime. No amount of receive-side hygiene makes a 12-bit recurring value a stable group identity.

- **Eviction/expiry only** (the [#912](https://github.com/avatarsd-llc/libtracer/issues/912) direction: bound `pending_`/`reasm_` with a TTL). This *narrows the window* in which a recurring base can collide with a stale group, but it is timing-dependent — a slow or paused producer, or an adversarially-timed one, can still land a recurrence inside a lingering group's lifetime. Eviction is worth doing for its own reasons (unbounded state) but is **not** a correctness fix for aliasing. The generation is deterministic; #909 and #912 are separate PRs solving separate problems.

- **A wall-clock timestamp in `ts`** (matching the field's documented name literally). Rejected: an MCU producer has no reliable monotonic wall clock, two producers' clocks are incomparable, and the group only needs *per-producer* distinctness — a monotonic counter is smaller, always available, and exactly sufficient. The field's doc comment should be corrected to "per-producer monotonic generation" rather than "timestamp."

## Consequences

- **Bus interop is version-gated.** Once a generation field is in the advertise framing, a mixed-version CAN bus (one board emitting the field, one not) must have a defined behavior. The implementing PR must state the compatibility rule (e.g. absent generation ⇒ treated as generation 0, preserving old-peer decode) and gate it. This is the property that makes the decision hard to reverse and is the reason it is recorded here.
- **A future reader** looking at `reassembly_key_t::ts` will see a field named for a timestamp carrying a counter; the corrected doc comment and this ADR explain why.
- `core/CHANGELOG.md` gets a note (public CAN advertise framing change); the CAN transport reference doc gains the generation field and the mixed-version rule.
