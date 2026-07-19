<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0015 — Endpoint value-dtype and direction/role in `:schema`

| Field | Value |
| --- | --- |
| **RFC** | 0015 |
| **Title** | Endpoint value-dtype and direction/role in `:schema` |
| **Status** | draft |
| **Author(s)** | strawberry-fw integration (drafted for maintainer review) |
| **Created** | 2026-07-19 |
| **Comment window closes** | opens with the tracking PR (GOVERNANCE.md §Spec changes); ≥ 14 days |
| **Tracking issue** | a dedicated `rfc`-labelled issue to be opened with this PR |
| **Target spec version** | v1 (additive; see §Compatibility) |
| **Roadmap item** | Grill D2 fast-follow — typed cross-device binding in the strawberry editor |

## Summary

Add two facts to an endpoint's self-description so a generic consumer can render and validate a **typed** binding without reading a live value and without relying on a non-standard per-endpoint child vertex: (1) the **dtype of the vertex's own principal VALUE** and (2) a **direction/role** tag distinguishing a fan-out producer/source from an input-port sink. This RFC DECLARES the shape and the design position for both; it DEFERS the concrete type-tag byte table, the direction enum encoding, and the exact POINT member names/positions to a code-pinning pass with conformance vectors, per the project's clause-kind rule.

## Motivation

### The gap today

libtracer gives a remote peer no protocol-legible way to learn either an endpoint's value dtype (without decoding a live sample by out-of-band convention) or whether a vertex is a producing source vs an input-port sink. Both are absent by design:

- **Value dtype is unmodeled.** `read <v>:schema` synthesizes a POINT whose protocol part is the vertex NAME plus a two-knob SETTINGS record (`deadline_ns`, `history_keep_last`) — `core/src/graph.cpp:1324-1372`, specifically the emit at `1329-1332`. The vertex's own data payload is explicitly opaque: the protocol "does NOT specify … the shape of an endpoint's data payload (a `VALUE` TLV's bytes are opaque to the protocol)" (`docs/reference/02-graph-model.md` §"The graph imposes no shape"). The wire TLV type is a routing concern, not a data type (`docs/reference/06-user-data-packing.md:509`) — even a live read only distinguishes VALUE `0x01` from STATUS `0x09`, never `u16` vs `f32`. The one in-band dtype surface that exists — RFC-0010's SHOULD-level app-field descriptor (`docs/reference/05-protocol-tlvs.md:419`) — describes `settings.app.*` **config fields**, not the endpoint's principal VALUE.
- **Direction/role is invisible.** "The protocol does not surface the role to peers" and there is no role-discovery wire format (`docs/reference/11-vertex-roles-and-aggregation.md:53, :374`; `CONTEXT.md:110`). The only dynamic proxies are weak: `read :subscribers[]` reveals who is bound *right now*, not a static role — a freshly created producer with no subscribers is indistinguishable from a sink, and an input port can itself fan out downstream (`docs/reference/13-network-formation.md:100-101`). The two-ACL guard answers "may I", not "what am I" (`docs/reference/13:120-121`).

### The strawberry consumer need

The strawberry web-ui endpoint editor is driven entirely by these facts and cannot obtain them from libtracer today. `endpoint-editor.component.ts` load-bearingly branches on **dtype** to encode a value write and to zero a value on stop, and gates its entire UI on **direction**: `writable()` returns whether the endpoint's role is `output`, choosing a value input + Set + Stop for a writable sink versus a lone "Stop producer" button for a producer. It also displays unit and range.

The editor obtains all of this from an interim `<endpoint>/meta` **child vertex**, not from `:schema`. `ws.service.ts registerEndpoint()` reads that child, deriving direction as `role = meta.writable ? 1 : 0` and dtype/unit/range from `parseMeta` (`tracer-edit.ts`). Firmware seeds the child in `io_graph.c fill_meta_record()` / `seed_field(leaf_path, "meta", …)`.

That interim carries a measured **≈ 900 B of heap per vertex** (RFC-0010 §Motivation `:101-102`, `:469-474`). ADR-0076's ratified amendment names the endgame: owner-defined `:schema` "additionally dissolves the interim `<endpoint>/meta` child projection — roughly halving measured per-endpoint RAM — once the upstream implementation lands and the pin is bumped" (`fw doc/adr/0076-…:48-56`). The strawberry cutover M2 ("self-descriptive node") lists **vertex-role legibility** as a required deliverable alongside `:schema` (`fw libtracer-cutover-workflow.md:65`).

So a native `:schema`-served endpoint **dtype + direction** would let the editor feed its descriptor block from the same `:schema` read it already performs for owner fields, deleting the `/meta` child vertex, its ≈900 B/endpoint heap cost, and the ad-hoc `fill_meta_record`/`collectEndpoints` "value-vertex-plus-meta-child" pairing convention.

## Proposed change

RFC-0010 already built the seam: `read_schema` (`core/src/graph.cpp:1324-1372`) synthesizes a two-part POINT and has a documented append point at `1328-1337`. This RFC adds an endpoint's own value description to that self-description.

**DECLARE (leads implementation):**

1. **An endpoint MAY expose, in its `:schema`, the dtype of its own principal VALUE** — the shape of the bytes a peer reads from / writes to the vertex (`bool` / `i32` / `u32` / `f32` / `utf8` / `rgb` / …). This is *self-description a consumer uses to encode/decode and render*, distinct from RFC-0010's `settings.app.*` field descriptors.
2. **An endpoint MAY expose a direction/role tag** distinguishing a fan-out producer/source from an input-port sink (a vertex a peer writes into), readable independent of current bindings — i.e. legible even with an empty `:subscribers[]`.
3. **Both are optional and additive.** A vertex that declares neither serves byte-for-byte today's POINT and behaves exactly as today.
4. **dtype, if runtime-held, is self-description the runtime never enforces** — mirroring `access`'s "projects but does not validate" split (`docs/reference/02-graph-model.md:398`): the runtime MUST NOT reject a write whose bytes disagree with the declared dtype. dtype declares shape; it does not gate.
5. Both facts describe the **one vertex** whose `:schema` is read — `:schema` is a vertex-only facet (recorded maintainer ruling), consistent with this addition.

**DEFER (must follow implementation; code-pinned via conformance vectors):**

- the concrete **type-tag byte table** (`bool`/`i32`/`u32`/`f32`/`utf8`/`rgb`/… → numeric tags);
- the **direction/role enum encoding** and its member cardinality (see §Discussion 2);
- the **exact POINT member name(s) and position** (e.g. whether dtype/direction ride the synthesized protocol SETTINGS beside `deadline_ns`, become new top-level POINT members, or land as an owner-convention descriptor — see §Discussion 1);
- whether reading either facet on a vertex that declares nothing is a silent absence or an error identity.

This RFC does **not** assert any of the above bytes normatively. Concrete example spellings appearing in discussion are illustrative, not the pinned wire.

**Files an accepted RFC would touch (indicative):** `core/src/graph.cpp` (`read_schema` synthesis at `1324-1372`), `core/include/libtracer/vertex.hpp` (native dtype/direction state, if option 1/3 wins — sibling to `app_field_t` at `118-131`), `docs/reference/02-graph-model.md` (§Schema, `369-445`), `docs/reference/11-vertex-roles-and-aggregation.md` (role legibility, reconcile `:53, :374`), and a new conformance vector under `tests/`.

## Conformance-vector sketches (what would prove it)

Each is `test-name` — condition → expected bytes/behavior. Bytes and error identities are pinned **here**, not in the prose above.

1. `schema-endpoint-dtype-declared` — vertex with a declared value-dtype, `read :schema` → POINT carries the dtype member with the pinned type-tag for that shape, at the pinned position.
2. `schema-endpoint-direction-declared` — producer vs input-port sink, `read :schema` → distinct pinned direction values; the sink's value is legible **with `:subscribers[]` empty** (proves it is static, not a `:subscribers` proxy).
3. `schema-undeclared-is-today` — vertex declaring neither → `:schema` bytes are byte-for-byte identical to the pre-RFC two-knob POINT (proves additivity).
4. `dtype-not-enforced` — write bytes disagreeing with declared dtype → write still succeeds; no dtype/range rejection (proves self-description parity with `access`).
5. `dtype-distinct-from-app-field-dtype` — a vertex with BOTH an RFC-0010 `settings.app.*` field carrying a descriptor `dtype` AND a declared value-dtype → the two are served in distinct positions and do not collide (proves the two dtypes are separate concepts).

## Compatibility

- **Does not break protocol-v1.** Purely additive: new optional members inside an existing facet's POINT. A peer that ignores them sees today's schema; a vertex that declares nothing emits today's bytes (vector 3). No framing, identifier, or existing MUST/SHOULD changes → stays v1, no v2 bump.
- **New conformance vectors:** the five above (names to be finalized at code-pin).
- **Migration for deployed devices:** none forced. Producers adopt the declaration opportunistically. On the strawberry side, once this lands and the pin bumps, the `<endpoint>/meta` child projection is removed (fw ADR-0076 `:48-56`), reclaiming ≈900 B/endpoint; until then the editor keeps reading `/meta` and the two coexist.
- **Wire-still-DRAFT posture:** consistent with the pin-to-tag preview; the concrete tags land in the code-pinning pass, not this draft.

## Alternatives considered

1. **Owner-app convention only (no runtime state).** Carry dtype/direction as SHOULD-level owner descriptor members (`settings.app.dtype`, `settings.app.direction`), reusing RFC-0010's §B.1 vocabulary. Zero new native state, no byte-agnosticism concern, no doctrine collision. Rejected as the *sole* answer because it is **not authoritative** — an owner may omit or lie, and a cross-vendor renderer needs the vocabulary frozen to depend on it. It remains live as the home for these facts if the maintainer prefers convention over runtime authority (see §Discussion 1); it is also the cheapest place for direction to land without touching "roles stay invisible" doctrine.
2. **Read a live value + out-of-band convention (status quo).** Rejected: impossible before first write, never protocol-declared, and the TLV type-code is not a data type (`docs/reference/06:509`).
3. **`:subscribers[]` as a producer proxy.** Rejected: dynamic, not static — empty on a fresh producer, non-empty on a fan-out input port (`docs/reference/13:100-101`).
4. **A dedicated `:dtype` / `:direction` native facet (new POINT sibling / read surface).** Most invasive; would add core-writable-field rows (`docs/reference/02:375-391`) and should reconcile with already-unserved core rows (`:description`, `:liveness.*`) flagged in RFC-0010 §Discussion 5. Held in reserve as option 3 in §Discussion 1.
5. **Keep the `/meta` child.** Rejected: the ≈900 B/endpoint heap cost is the exact thing ADR-0076 commits to dissolve; the child is "a fine consumer-side interim precisely because it erases cleanly when this lands" (RFC-0010 `:469-474`).

## Discussion

Records the comment window and any sustained objections. Open questions flagged for the maintainer:

1. **Synthesized-authoritative vs owner-convention (the central ruling).** Does endpoint dtype+direction live in the **synthesized protocol part** of `:schema` — runtime-authoritative, uniform cross-vendor, "an owner cannot lie about or hide protocol machinery" (RFC-0010 §B.2 `:284-288`) — or as a **SHOULD-level owner-app convention** like RFC-0010's descriptor vocabulary (Alternative 1)? dtype stays inside byte-agnosticism either way (self-description, runtime never interprets payload bytes — same posture as `access`). But **direction, if made authoritative, becomes native vertex state and collides with settled doctrine**: producer-holds-subscriber direction (`CONTEXT.md:87-91`) and "roles stay invisible" (`CONTEXT.md:110`, `docs/reference/11:53, :374`). Making direction wire-legible is a deliberate reversal of that stance and needs an explicit ruling. A third position (Alternative 4) is a dedicated native facet. Maintainer to choose the authority model for **each** datum — they need not match.
2. **What is "direction", exactly?** A two-value enum (producer / input-port-sink)? Or three (add bidirectional)? How does it relate to `:subscribers[]` (a producer is anything that *holds* subscriber edges — is direction then partly derivable, or fully independent so a sink with downstream subscribers is still legibly a sink)? And how does it map to strawberry ADR-0075's "controller inputs become lightweight input ports" and the fw `io_dir_t` (`DEVICE_TO_CPU` / `CPU_TO_DEVICE`, `CONTEXT.md:212-214`)? Note ADR-0075 is a **strawberry-fw ADR, not libtracer** (libtracer's ADR ceiling is 0059) — so the enum must be chosen on libtracer's own terms, not inherited. Enum cardinality and encoding are code-pinned regardless (vector 2).
3. **Does value-dtype duplicate a live read, and is a declared dtype authoritative or advisory?** A live VALUE read reveals bytes but not their shape (`0x01` vs `0x09` only). A schema-declared dtype adds shape *before first write* and cross-vendor. But if the runtime does not enforce it (Proposed change 4), is a declared dtype **authoritative** (consumers may trust it) or merely **advisory** (best-effort owner hint)? The editor's write-encoding branch needs to know whether it can rely on it. Maintainer to rule on the trust level; enforcement stays "projects but does not validate" per parity with `access`.
4. **Interaction with RFC-0010's owner descriptor dtype.** RFC-0010 §B.1 already reserves a `dtype` slot — but that is the dtype of an *app config field* (`settings.app.*`), not the endpoint's own VALUE. This RFC's dtype is the vertex's principal payload. They are two different concepts that could both be present on one vertex (vector 5). Maintainer to confirm the naming/position keeps them unambiguously distinct and to decide whether they should share a type-tag table or stay independent vocabularies.
5. **Read semantics on an undeclared vertex.** Silent absence (member simply not emitted, as with a vertex that has no app table today) vs an error identity — code-pinned, not asserted here.

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the tracking issue stays open at least 14 days for implementer feedback before this document is merged (unless the standing solo-maintainer waiver is applied, as on RFC-0002/0005/0008). Sustained objections and their resolution to be recorded here.
