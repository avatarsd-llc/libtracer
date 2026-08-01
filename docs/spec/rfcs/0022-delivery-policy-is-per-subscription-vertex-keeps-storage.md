<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0022 — Delivery policy is per-subscription; `settings_t` dissolves

| Field | Value |
| ---- | ---- |
| **RFC** | 0022 |
| **Title** | Delivery policy is per-subscription; `settings_t` dissolves |
| **Status** | **accepted** — maintainer ruling 2026-08-01; comment window waived by sole maintainer per [GOVERNANCE](../../../.github/GOVERNANCE.md) §Errata, amendments, and the comment window. **Amendment 1** (2026-08-01, below) replaced §3.B–§3.D before any implementation landed. To be implemented and tested **before v0.7.0**. |
| **Amends** | [RFC-0004](0004-remote-operation-addressing.md) §E (delivery/fanout), [RFC-0010](0010-owner-app-fields-and-schema.md) §B.2 (the synthesized `:schema` protocol part), §A.4 (the `:settings` read container) |
| **Supersedes** | [#617](https://github.com/avatarsd-llc/libtracer/issues/617) (intern the QoS profile) — the struct it proposed to intern ceases to exist |
| **Dissolves** | [#706](https://github.com/avatarsd-llc/libtracer/issues/706) (`:schema` under-reports knobs) — there is nothing left to under-report |
| **Tracking** | [#756](https://github.com/avatarsd-llc/libtracer/issues/756) |

## 1. Summary

`settings_t` attaches seven QoS knobs to the **vertex**. Four are **inert** — writable, readable, and consumed by no code. Of the three that work, one is a property of a *delivery relationship* and two are *construction parameters* that were never QoS at all.

This RFC:

- moves **delivery policy to the subscription**, packed into **two bytes**, carried in the `SUBSCRIBER` TLV's already-existing `SETTINGS` child;
- **deletes `settings_t` entirely** — the vertex keeps no QoS state, and the `:settings.<knob>` remote write surface is removed;
- rehomes the two survivors to where they belong: a STREAM ring depth becomes owner-side vertex state, and the zero-copy store threshold becomes a per-target configuration constant.

## 2. Motivation — measured, not asserted

Every knob accepts a `:settings.<knob>` write and appears in the settings read. Only three drive behaviour:

| knob | consumed at | verdict |
| --- | --- | --- |
| `durability` | `core/include/libtracer/vertex.hpp` — the transient-local latch | **live**, delivery-side |
| `history_keep_last` | `core/include/libtracer/vertex.hpp` — ring trim, re-read on **every** append | **live**, storage-side |
| `store_ref_min_bytes` | `core/src/op_resolve_walk.hpp` — the zero-copy store-by-subview threshold | **live**, storage-side |
| `reliability` | stored and emitted in `core/src/graph.cpp` — nothing else | **inert** |
| `priority` | stored and emitted in `core/src/graph.cpp` — nothing else | **inert** |
| `deadline_ns` | knob map, store, emit — nothing else | **inert** |
| `queue_max_bytes` | knob map, store, emit — nothing else | **inert** |

`grep -rn 'deadline_ns\|queue_max_bytes' core/src core/include` yields only the `qos_knob_t` name mapping, the assignment, and the `emit_value`. No comparison, no arithmetic, no branch.

**Why they were never implemented is the design finding.** A single per-vertex `reliability` or `priority` has no coherent meaning when one vertex fans out to a CAN peer and a WebSocket peer at once — they describe a *relationship*, and a vertex is not one. DDS places exactly these on the reader/writer pair for the same reason.

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

### B. `settings_t` is deleted

The type is removed, not shrunk. With §A taking `durability` and §C/§D rehoming the two survivors, nothing remains that is simultaneously per-vertex, QoS, and remotely writable.

`register_vertex` loses its fourth parameter across all three overloads:

```cpp
vertex_handle_t register_vertex(const path_t& path, role_t role, handlers_t handlers = {});
```

`adopt_identity`'s extension-block gate correspondingly loses its `settings == kDefaultSettings` term, so **strictly more vertices stay extension-less than today** — a vertex allocates the cold block only when it is STREAM, carries a handler, or is given app fields.

**Consequence: vertex QoS state becomes immutable, and one open hazard closes with it.** `graph_t`'s `:settings.<knob>` write branch is the *only* caller of `vertex_t::update_settings` in the tree; removing the surface removes the mutation path. The unlocked accessor that returns `const settings_t&` — the accessor §6.1 names as the decisive blocker against interning ("a reclamation question against an unlocked reader that returns a reference") — has nothing left to race against, because it has nothing left to return. #617's blocker is dissolved rather than engineered around.

### C. `history_keep_last` becomes owner-side STREAM state

The ring depth is not protocol QoS. It is what the **application** wants retained, and only the application can supply it; `vertex.hpp`'s own role table already concedes the ownership — *"Role 2: bounded history ring sized by `settings.history_keep_last`"*. The role owns it; `settings` was only ever the carrier.

- It becomes a private member of the vertex extension block, guarded by the vertex mutex that already guards the ring it bounds.
- It is set by an owner-side wiring call, `graph_t::set_history_depth(vertex_handle_t, std::uint32_t)`, matching the established shape of `set_delivery_mode` and `set_app_fields` — declarations an owner makes host-side, after registration, never over the wire.
- It costs **zero additional bytes on the vertices that use it**: a STREAM vertex already always allocates the extension block.
- It has **no wire surface at all** — neither readable nor writable remotely.

This is the distinction the removal rests on: what is withdrawn is the **remote write surface**, not owner-side configuration.

### D. `store_ref_min_bytes` is deleted; the pin decision measures amplification

The threshold chose between two ways to keep a written value: copy it out of the inbound frame, or refcount a subview of that frame (`own_or_ref_tlv`). The economics are asymmetric and were mis-modelled:

- **copy** — one allocation and one `memcpy`; steady-state memory held = the payload
- **pin** — no allocation, no copy; steady-state memory held = **the whole inbound segment**

Pinning therefore *always* holds more memory, by exactly `segment − payload`. It never saves RAM; it buys latency and pays in RAM.

An absolute byte threshold measures the wrong quantity. A 4 KB payload in a 4 KB frame (waste ≈ 0) and a 4 KB payload in a 256 KB frame (waste ≈ 252 KB) satisfy `payload >= N` identically, yet they are opposite trades — and the current form places **no bound on the waste at all**.

The predicate becomes the ratio the decision actually turns on, using two quantities already in hand at the decision site:

```
pin  iff  payload_bytes * K >= segment_bytes   (and the payload is trailer-less)
```

with `K = tr::graph::config_t::kPinPayloadRatio`, a per-target configuration constant alongside `kVertexLockStripes` and `kHazardReaderSlots` ([ADR-0070](../../adr/0070-configuration-is-a-named-traits-type.md)). A reserved sentinel value means **never pin**, which is the off switch a target with a small receive pool may require.

`K` is **not** a synthetic limit. It bounds nothing; both branches are correct, and `K` selects which correct branch is cheaper. Waste is now bounded at `(K−1)×` the payload where it was previously unbounded.

`config_t` — not the memory backend — is the owner. A `mem_backend_t` is an allocator; asking it to parameterise wire-frame economics would ask every implementer (`mem_heap`, `mem_pool`, `mem_borrowed`, `mem_cuda`, and any application backend) to answer a question about a subsystem it has no knowledge of.

### E. `deadline_ns` and `queue_max_bytes` are removed

They are inert and have no coherent per-vertex meaning. Moving dead fields is worse than deleting them; if per-subscription deadlines or queue bounds are wanted later, they are added when something implements them — as magnitudes in the subscription's cold half, never packed into §A's flags.

### F. Nothing is inherited

Earlier drafts of this RFC (see Amendment 1) specified subtree inheritance of a residual per-vertex storage policy. With §B–§D applied there is **no per-vertex policy left to inherit**, so no inheritance mechanism is introduced: no ancestor walk, no cached ancestor reference, no flag bit, and no propagation question when a parent's configuration changes.

## 4. Wire and API impact (BREAKING)

- **The `:settings.<knob>` write surface is removed entirely.** A write to any of the seven names answers `SCHEMA_NOT_FOUND` — the honest answer, and the one an unsupported field already gives. `settings.app.*` writes (RFC-0010 §A) are untouched.
- **The `:settings` read container keeps its shape and loses its knobs.** It becomes `SETTINGS{ [NAME "app" SETTINGS{…}] }` — the reserved `app` subkey and the single-traversal renderer contract of RFC-0010 §A.4 survive; a vertex with no declared app fields reads an empty `SETTINGS{}`, which is honest rather than absent. `:settings.app` and `:settings.app.<name…>` are unchanged.
- **`:schema`'s synthesized settings part loses its knob enumeration.** RFC-0010 §B.2's "the implemented `settings.*` knobs" becomes empty and therefore complete — the condition #706 was filed about, resolved by removing the inputs rather than by extending the view.
- **`register_vertex`, `try_register_vertex` and `register_vertex_key` lose their `settings_t` parameter**; `graph_t::settings(vertex_handle_t)` and `settings_t` itself are removed. `graph_t::set_history_depth` is added.
- The `SUBSCRIBER` `SETTINGS` child gains one key. Existing senders that omit it are unaffected; absent ⇒ default.
- **A behaviour change that is not a pure removal:** referencing is off by default today (`store_ref_min_bytes` defaults to `0`, and the code requires `> 0`). Under §D it becomes on-by-default whenever the payload dominates its segment. See §6.

## 5. Conformance vectors

1. `subscriber/policy-absent` — no `SETTINGS` child ⇒ default behaviour, byte-identical to today.
2. `subscriber/policy-durability` — `durability_request` set ⇒ the latched value is delivered on join; unset ⇒ it is not.
3. `subscriber/policy-reserved-bits` — reserved bits set ⇒ ignored, not an error.
4. `settings/removed-knob` — a `:settings.deadline_ns` write ⇒ `SCHEMA_NOT_FOUND`; likewise each of the other six names, including the two survivors, which are no longer remotely writable.
5. `settings/read-container-shape` — `:settings` on a vertex with app fields reads `SETTINGS{ NAME "app" SETTINGS{…} }`; on one without, an empty `SETTINGS{}`.
6. `settings/schema-enumerates-nothing` — `:schema` carries no protocol-knob entries.
7. `stream/history-depth-host-only` — `set_history_depth` changes the retained ring depth; no wire operation can read or write it.
8. `store/pin-ratio` — a payload dominating its segment is stored as a subview; the same payload inside a much larger segment is copied; the sentinel `K` disables pinning entirely.

## 6. Implementation gate — measurement before landing

§D turns pinning on by default. That is a latency win on every large write and a RAM cost bounded by `K`, and it must be **measured on both halves of the dual target before it lands**, not asserted:

- the ESP32-C6 profile, where the RAM constraint binds and the receive pool is small;
- the many-core host profile, where the latency win is the point.

If the MCU numbers are unfavourable, `kPinPayloadRatio`'s sentinel is already the remedy — the MCU never pins, the host does, from one codebase.

**A related hazard is recorded but out of scope here.** Pinning refcounts the inbound *receive* segment, so a long-held value holds a receive buffer for as long as it lives; on a transport with a small fixed pool this reduces receive capacity for the value's lifetime. The ratio bounds *how much* is wasted per value, not *how long*, and retention time is not observable at store time. This predates this RFC and outlives it, and is tracked separately.

## 7. Alternatives considered

1. **Intern the QoS profile** ([#617](https://github.com/avatarsd-llc/libtracer/issues/617)). Shrinks the struct by sharing it, at the cost of an intern table, a `BACKPRESSURE` contract, and — decisively — a reclamation question against an unlocked reader that returns a reference. Superseded: deleting the fields beats sharing them, and §B closes the reclamation question outright.
2. **Keep the inert knobs, document them as reserved.** Cheapest and honest, but leaves a control surface that accepts writes it ignores — the `:liveness.*` fiction pattern #586 removed.
3. **Implement the inert four per-vertex.** Requires first defining what one `reliability` means across a heterogeneous fan-out. No coherent answer was found; that absence is why §A moves them.
4. **Squash the magnitudes into the two-byte policy.** Rejected: a bit-width on a magnitude is a synthetic limit.
5. **Keep a residual two-field `settings_t` on the vertex, inherited down the subtree.** This RFC's own original §3.B/§3.C. Rejected in Amendment 1 — see below.
6. **Source the pin threshold from `mem_backend_t`.** Rejected on ownership: an allocator cannot reason about frame/payload economics, and every backend implementer would be obliged to answer a question it has no basis to answer. `config_t` already exists for per-target policy constants.

## 8. Open questions

1. Should the STREAM ring depth eventually be **derived from an injected resource** (RFC-0006) rather than declared, making the ring bounded by the store it draws from? Out of scope: unlike the pin threshold, a retention depth encodes application intent that no resource can supply.
2. Does the delivery policy interact with route-handle compaction (RFC-0004 §E.1)? Expected not — compaction is keyed on `(link, route)`, both unchanged — but vector 2 should be run in a compacted flow.
3. What value should `kPinPayloadRatio` default to? To be set by §6's measurement, not by argument.

## Amendment 1 — 2026-08-01: the residual vertex policy dissolves

**Status: accepted, before any implementation landed.** Recorded here rather than as an erratum because it changes what a conforming implementation does ([GOVERNANCE](../../../.github/GOVERNANCE.md) §Errata, amendments, and the comment window).

The RFC as first accepted kept a two-field `settings_t` on the vertex (`history_keep_last`, `store_ref_min_bytes`) and specified subtree inheritance for it. A design grill against the code before implementation falsified that shape on four counts:

1. **The vertex had nothing left that was QoS.** With `durability` moved to the subscription and the four inert knobs deleted, both survivors are *construction parameters* — one an application retention intent, one a deployment-level copy/pin trade. Neither is a protocol quality-of-service property, and neither belongs on a remote write surface. Shrinking the type preserved a category error that deleting it removes.
2. **Inheritance had a hole with no good answer.** The original §3.C specified inheritance "by value, at registration" but said nothing about a parent whose policy changes *after* its children exist. Copy-at-registration silently fails to reach them; propagating requires a subtree walk under the map lock while the settings write holds a stripe lock — a new lock edge, in a codebase that has just spent three implementation rounds on #576 learning what a new lock edge costs. Inheriting by *reference* instead removes the copy cost but not the hole: a parent that becomes a policy bearer after its descendants resolved is unreachable either way.
3. **Copy-at-registration was worse for RAM than doing nothing.** Carrying eight bytes of policy into a descendant forces the whole extension block onto it — an atomic handler pointer, a history `unique_ptr`, two vectors, an ACE merge cache and an app-field pointer. Materialising that across a subtree to deliver two `u32`s inverts the RAM argument the inheritance existed to serve.
4. **The pin threshold measured a proxy for a fact available three lines away.** `store_ref_min_bytes` encodes a guess about payload size, while the decision site holds both the real payload size and the real segment size. Configuration standing in for a directly measurable quantity is configuration that should not exist.

Removing the per-vertex policy resolves all four at once: there is nothing to inherit, so the hole and the lock edge do not arise; nothing to copy, so no extension block is forced; and the predicate measures amplification directly instead of approximating it.

**Changed:** §3.B (was "the vertex keeps storage policy only" — now "`settings_t` is deleted"), §3.C (was inheritance by copy at registration — now the STREAM ring depth's owner-side home), §3.D (was the removal of the two inert magnitudes — now the pin predicate; the removal moved to §3.E), and consequently §4, §5, §6 and §7. §3.A is unchanged and was not in question.
