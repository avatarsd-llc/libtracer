<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0022 — Delivery policy is per-subscription; the vertex keeps only storage policy

| Field | Value |
| ---- | ---- |
| **RFC** | 0022 |
| **Title** | Delivery policy is per-subscription; the vertex keeps only storage policy |
| **Status** | **accepted** — maintainer ruling 2026-08-01; 14-day comment window waived by sole maintainer. To be implemented and tested **before v0.7.0**. |
| **Amends** | [RFC-0004](0004-remote-operation-addressing.md) §E (delivery/fanout), [RFC-0010](0010-owner-app-fields-and-schema.md) §B.2 (the synthesized `:schema` protocol part) |
| **Supersedes** | [#617](https://github.com/avatarsd-llc/libtracer/issues/617) (intern the QoS profile) — the struct it shrinks mostly ceases to exist |
| **Dissolves** | [#706](https://github.com/avatarsd-llc/libtracer/issues/706) (`:schema` under-reports knobs) — there is nothing left to under-report |
| **Tracking** | [#756](https://github.com/avatarsd-llc/libtracer/issues/756) |

## 1. Summary

`settings_t` attaches seven QoS knobs to the **vertex**. Four of them are **inert** — writable, readable, and consumed by no code. The three that work split cleanly into *storage* policy (a property of the vertex as a value holder) and *delivery* policy (a property of one producer→subscriber relationship).

This RFC:

- moves **delivery policy to the subscription**, packed into **two bytes**, carried in the `SUBSCRIBER` TLV's already-existing `SETTINGS` child;
- leaves the vertex only its **storage** policy — two `u32`s, stored **only when non-default**;
- **removes** the two knobs that are inert *and* have no coherent per-vertex meaning.

## 2. Motivation — measured, not asserted

Every knob accepts a `:settings.<knob>` write and appears in the settings read. Only three drive behaviour:

| knob | consumed at | verdict |
| --- | --- | --- |
| `durability` | `vertex.hpp:1398`, `:1472` — the transient-local latch | **live**, delivery-side |
| `history_keep_last` | `vertex.hpp:1245` — ring trim, re-read on **every store** | **live**, storage-side |
| `store_ref_min_bytes` | `op_resolve.hpp:99` — the zero-copy store-by-subview threshold | **live**, storage-side |
| `reliability` | stored `graph.cpp:1630`, emitted `:1801` — nothing else | **inert** |
| `priority` | stored `:1636`, emitted `:1809` — nothing else | **inert** |
| `deadline_ns` | knob map, store, emit — nothing else | **inert** |
| `queue_max_bytes` | knob map, store, emit — nothing else | **inert** |

`grep -rn 'deadline_ns\|queue_max_bytes' core/src core/include` yields only the `qos_knob_t` name mapping, the assignment, and the `emit_value`. No comparison, no arithmetic, no branch.

**Why they were never implemented is the design finding.** A single per-vertex `reliability` or `priority` has no coherent meaning when one vertex fans out to a CAN peer and a WebSocket peer at once — they describe a *relationship*, and the vertex is not one. DDS places exactly these on the reader/writer pair for the same reason.

A client today may write `:settings.deadline_ns`, read it back, and see it in `:schema` — and nothing will ever honour it. That is strictly worse than an unsupported field, which answers `SCHEMA_NOT_FOUND` honestly.

## 3. Decision

### A. Delivery policy moves to the subscription — two bytes

A `SUBSCRIBER` MAY carry a delivery-policy value in its **existing `SETTINGS` child** (the same child that carries `delivery_compact` today, so this introduces no new wire structure). The policy is a packed 16-bit field:

| bits | field | values |
| ---: | --- | --- |
| 0–1 | `reliability` | 0 = best-effort, 1 = reliable; 2–3 reserved |
| 2–4 | `priority` | 0–7, 0 = default |
| 5 | `durability_request` | 1 = deliver the latched last value on join |
| 6–15 | reserved | MUST be written 0, MUST be ignored on read |

Absent ⇒ all-zero ⇒ today's default behaviour. **No magnitudes** are packed: a bit-width on a magnitude is a synthetic limit, which this project forbids (bounds come from injected resources or per-target config, never a magic constant).

`durability` becomes a **request**, matched at `admit_subscriber` where the latch already fires. This is strictly more correct than the status quo, in which one vertex-level flag silently applies to every subscriber.

### B. The vertex keeps storage policy only

`settings_t` reduces to the two storage magnitudes:

- `history_keep_last` — the ring trim depth. It is **not** derivable from the ring: the ring is an unbounded `std::deque` trimmed to this value after every append (`vertex.hpp:1245`), so the value is a live policy, re-read per store.
- `store_ref_min_bytes` — the zero-copy threshold. It is a decision made **before any subscriber exists**, so no subscription can own it.

Both default to "off"/1, and a vertex at defaults stores **nothing**: `adopt_identity` already skips the extension block entirely for a default, handler-less, non-STREAM vertex, and that behaviour is preserved.

### C. Storage policy inherits by copy at registration

A child inherits its parent's storage policy **by value, at registration**. An override materialises the extension block on the overriding vertex and its inheriting descendants — *the override grows the subtree that opted in, and nothing else*.

Resolution is **not** an ancestor walk: `store_ref_min_bytes` is read per write and `history_keep_last` per store, so a walk on those paths is disqualifying under the project's latency-first ordering. Copy-at-registration keeps both reads a single inline load, identical in cost to today.

### D. `deadline_ns` and `queue_max_bytes` are removed

They are inert and have no coherent per-vertex meaning. Moving dead fields is worse than deleting them; if per-subscription deadlines or queue bounds are wanted later, they are added when something implements them — as magnitudes in the subscription's cold half, never packed into §A's flags.

## 4. Wire and API impact (BREAKING)

- The `:settings` knob-name grammar loses `reliability`, `priority`, `deadline_ns`, `queue_max_bytes`. A write to any of them answers `SCHEMA_NOT_FOUND` — the honest answer, and the one an unsupported field already gives.
- `:schema`'s synthesized settings part changes shape. RFC-0010 §B.2's "the implemented `settings.*` knobs" now enumerates exactly the two storage knobs, which is both complete and true — the condition #706 was filed about.
- The `SUBSCRIBER` `SETTINGS` child gains one key. Existing senders that omit it are unaffected; absent ⇒ default.
- `settings_t` shrinks from 24 B to 8 B, stored only when non-default — net **−24 B** on a typical extension-bearing vertex whose QoS is default, **+2 B** per subscription.

## 5. Conformance vectors

1. `subscriber/policy-absent` — no `SETTINGS` child ⇒ default behaviour, byte-identical to today.
2. `subscriber/policy-durability` — `durability_request` set ⇒ the latched value is delivered on join; unset ⇒ it is not.
3. `subscriber/policy-reserved-bits` — reserved bits set ⇒ ignored, not an error.
4. `settings/removed-knob` — `:settings.deadline_ns` write ⇒ `SCHEMA_NOT_FOUND`.
5. `settings/schema-enumerates-storage` — `:schema` lists exactly the storage knobs.
6. `settings/inherit-storage` — a child registered under an overriding parent inherits by value; a later child likewise; an overriding child stops inheriting.

## 6. Alternatives considered

1. **Intern the QoS profile** ([#617](https://github.com/avatarsd-llc/libtracer/issues/617)). Shrinks the struct by sharing it, at the cost of an intern table, a `BACKPRESSURE` contract, and — decisively — a reclamation question against an unlocked reader that returns a reference (`vertex.hpp:935`). Superseded: deleting fields beats sharing them.
2. **Keep the inert knobs, document them as reserved.** Cheapest and honest, but leaves a control surface that accepts writes it ignores — the `:liveness.*` fiction pattern #586 removed.
3. **Implement the inert four per-vertex.** Requires first defining what one `reliability` means across a heterogeneous fan-out. No coherent answer was found; that absence is why §3 moves them.
4. **Squash the magnitudes into the two-byte policy.** Rejected: a bit-width on a magnitude is a synthetic limit.

## 7. Open questions

1. Should `history_keep_last` eventually be **derived from an injected resource** (RFC-0006) rather than configured, making the ring bounded by the store it draws from? Out of scope here; noted because it would remove the last storage magnitude.
2. Does the delivery policy interact with route-handle compaction (RFC-0004 §E.1)? Expected not — compaction is keyed on `(link, route)`, both unchanged — but vector 2 should be run in a compacted flow.
