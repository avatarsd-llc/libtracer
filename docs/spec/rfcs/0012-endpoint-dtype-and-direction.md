<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0012 — Endpoint value dtype and direction

| Field | Value |
| ---- | ---- |
| **RFC** | 0012 |
| **Title** | Endpoint value dtype and direction |
| **Status** | **in-comment** |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-17 |
| **Comment window closes** | 2026-07-31 (≥ 14 days after opening) |
| **Tracking issue** | [#412](https://github.com/avatarsd-llc/libtracer/issues/412) |
| **Target spec version** | v1 (additive — no wire-incompatible change) |

## Summary

Two **optional, owner-declared, read-only advertisement members** are added to
the protocol-owned flat namespace of the vertex `SETTINGS` container and served
in the synthesized protocol part of the `:schema` POINT: **`dtype`** — the
declared shape of the vertex's own `VALUE` payload, spelled in the
[RFC-0010](0010-owner-app-fields-and-schema.md) §B.1 dtype token vocabulary
(one vocabulary, now enumerated; never a second one) — and **`dir`** — the
endpoint's role, `produce` / `consume` / `both`, defined **from the
perspective of the vertex's owner** (the device): a sensor produces, an
actuator consumes. Both are **advertisements, not enforcement**: the runtime
stores and serves the declared tokens verbatim and MUST NOT validate `VALUE`
bytes against `dtype` or gate any operation on `dir` — the "a `VALUE` is
opaque to the protocol" invariant of [docs/spec/v1.md](../v1.md) §1 is
untouched. Absence means **unknown**: a vertex whose owner declares nothing is
byte-for-byte today's vertex, and clients MUST NOT assume a default. No new
type codes, no new wire verbs, no new error identities.

## Motivation

1. **The measured gap.** `core/src/graph.cpp` `read_schema` synthesizes the
   entire `:schema` read: a `POINT { NAME <vertex name>, SETTINGS { NAME
   "deadline_ns" VALUE u64, NAME "history_keep_last" VALUE u32 } }`, plus the
   RFC-0010 owner part when a field descriptor table is installed. Nothing in
   it describes the **data plane** — the vertex's own `VALUE`. The spec is
   deliberate that payload bytes are opaque ([docs/spec/v1.md](../v1.md) §1:
   "application payload semantics (a `VALUE` is opaque to the protocol)" are
   out of scope) — this RFC keeps that — but opacity **without
   self-description** means a capability-agnostic client (the tracking issue
   [#412](https://github.com/avatarsd-llc/libtracer/issues/412) editor) learns
   nothing at all: not the payload width, not its shape, and — the
   sensor-vs-actuator question — not whether reading or writing this endpoint
   is even meaningful.
2. **The docs already promise the slot and the discipline.**
   [docs/reference/02-graph-model.md](../../reference/02-graph-model.md)
   §Schema and field discipline opens with "a vertex exposes a **schema**
   describing every writable field" — the *field* surface got its substrate in
   RFC-0010, but the vertex's own value never did.
   [docs/reference/12-deployment-profiles.md](../../reference/12-deployment-profiles.md)
   §strawberry profile lists `schema_registry: :schema POINT (dtype/unit/range,
   the io_descriptor)` — imagining exactly these members, with no normative
   substrate anywhere. And
   [ADR-0021](../../adr/0021-vertex-field-list-with-standard-and-device-specific-fields.md)'s
   split makes this the **standard** half of the field discipline: uniform,
   protocol-minted names, so a generic orchestrator works cross-device without
   vendor knowledge.
3. **The driving consumer carries this today, off-protocol.** strawberry-fw's
   endpoint substrate is "`io_layer` endpoint + `io_descriptor_t`"
   ([ADR-0001](../../adr/0001-extract-reference-implementation-from-strawberry-fw.md)
   extraction table), and fw ADR-0057 is "a unified typed Value seam — the
   wire carries type + ts" (cited ibid.). The extraction deliberately did
   **not** carry the type tag onto the libtracer wire — `VALUE` went opaque,
   and the type half was destined for L4 schema, which never grew the member.
   Concretely blocked: the orchestrator/constructor flow
   ([CONTEXT.md](../../../CONTEXT.md) §Controller vertex, §Network formation)
   binds SUBSCRIBER edges between controller ports and device endpoints, and
   **before any wiring exists nothing observable distinguishes** a sensor to
   subscribe to from an actuator input to bind into; and the HA/MQTT
   auto-discovery walker (RFC-0010 Motivation 3) cannot pick a Home-Assistant
   component class (`sensor` vs `number`/`switch`) without a dtype and a
   direction.
4. **The vocabulary exists; only the attachment point is missing.** RFC-0010
   §B.1 established a SHOULD-level dtype token vocabulary for **app-field
   descriptors** (`dtype`/`access`/`unit`/`min`/`max`/`label`). The vertex's
   own `VALUE` — the thing most consumers actually read — has no slot to
   carry the same datum. Minting a second vocabulary for it would fork the
   language; this RFC reuses the RFC-0010 tokens at a second attachment point
   and enumerates the token list both uses share.

## Proposed change

### A. Two advertisement members: `dtype` and `dir`

#### A.1 Names, encoding, placement

Two names are added to the **protocol-owned flat namespace of the vertex
`SETTINGS` container**
([docs/reference/05-protocol-tlvs.md](../../reference/05-protocol-tlvs.md)
§`0x0B`): **`dtype`** and **`dir`**. This is exactly the minting move RFC-0010
§A.1 reserved room for ("the protocol keeps minting flat knob names forever;
applications only ever mint below `.app.`") — no collision with application
fields is possible by construction. Unlike the seven QoS knobs, these two are
**read-only advertisements**, not writable configuration (§A.4).

Each member is the standard `SETTINGS` NAME-keyed pair — a `NAME` key followed
by a `VALUE` whose payload is the UTF-8 token bytes (no NUL). `VALUE`-carried
tokens match the shape RFC-0010 §B.1 already uses for the descriptor `dtype`
and the runtime-projected `access` members.

When declared (§B), the members MUST appear in the synthesized protocol part
of the `:schema` POINT (§`0x07`), after the synthesized QoS knobs:

```
POINT (PL=1) {                            ; read <vertex>:schema — reference/05 §0x07
  NAME      <vertex name>                 ; as today
  SETTINGS (PL=1) {                       ; synthesized protocol part
    NAME "deadline_ns"        VALUE <u64> ; as today (core/src/graph.cpp read_schema)
    NAME "history_keep_last"  VALUE <u32> ; as today
    NAME "dtype"  VALUE <utf-8 token>     ; NEW — present iff owner-declared (§A.2)
    NAME "dir"    VALUE <utf-8 token>     ; NEW — present iff owner-declared (§A.3)
  }
  NAME "app"  SETTINGS (PL=1) { … }       ; RFC-0010 owner part — unchanged
}
```

Byte-precise, for `dtype = "f32"`, `dir = "produce"` — the added run is 34
bytes inside the protocol `SETTINGS` (4-byte TLV headers: `type` u8, `opt` u8,
`length` u16 LE; all `opt` bits zero):

```
02 00 05 00 64 74 79 70 65          NAME:  type=02 opt=00 len=0005  "dtype"
01 00 03 00 66 33 32                VALUE: type=01 opt=00 len=0003  "f32"
02 00 03 00 64 69 72                NAME:  type=02 opt=00 len=0003  "dir"
01 00 07 00 70 72 6F 64 75 63 65    VALUE: type=01 opt=00 len=0007  "produce"
```

The declared members MUST also appear in the full-container `read :settings`
([RFC-0010](0010-owner-app-fields-and-schema.md) §A.4 surface), carrying
**byte-identical** tokens to the `:schema` projection — the one-traversal
generic renderer keeps working from either read. Member position within a
`SETTINGS` is keyed by `NAME`, not order; the reference emits in the order
shown, and the conformance vectors pin the reference bytes.

#### A.2 `dtype` — the RFC-0010 §B.1 token vocabulary, enumerated

`dtype` declares the shape of the payload of the `VALUE` TLV the owner intends
to store at this vertex. Its tokens are **the RFC-0010 §B.1 dtype vocabulary**
— one vocabulary for app-field descriptors and for this member; a consumer
that parses one parses both. This RFC enumerates the SHOULD-level token list
(RFC-0010 left it an open sketch: "bool/i32/u32/f32/utf8/rgb/…"), aligned 1:1
with the dtype set the driving consumer already ships (fw ADR-0057's typed
Value seam, cited in
[ADR-0001](../../adr/0001-extract-reference-implementation-from-strawberry-fw.md)):

| Token | Declared `VALUE` payload shape | fw ADR-0057 dtype |
| ---- | ---- | ---- |
| `bool` | 1 byte; `00` = false, non-zero = true | `BOOL` |
| `i32` | 4-byte little-endian two's-complement | `I32` |
| `u32` | 4-byte little-endian unsigned | `U32` |
| `f32` | 4-byte little-endian IEEE-754 binary32 | `F32` |
| `f64` | 8-byte little-endian IEEE-754 binary64 | `F64` |
| `utf8` | UTF-8 text, no NUL terminator | `STR` |
| `bytes` | opaque octet string | `BYTES` |
| `f32[]` | N × 4-byte little-endian IEEE-754 binary32 (N = payload length / 4) | `F32_ARRAY` |
| `record` | one structured TLV (`opt.PL=1`, user-range type code `0x80–0xFF`) | `RECORD` |

- Little-endian multi-byte shapes match the wire format's own multi-byte rule
  ([docs/reference/01-data-format.md](../../reference/01-data-format.md),
  incorporated by [docs/spec/v1.md](../v1.md) §3).
- The `[]` suffix generalizes to any scalar token (`u32[]`, `f64[]`) — a
  fixed-stride homogeneous payload. This changes nothing about the wire:
  array-ness remains an **L4 schema property, never a wire bit**
  ([ADR-0008](../../adr/0008-arrays-are-schema-property-not-wire-type.md);
  [CONTEXT.md](../../../CONTEXT.md) §Fixed-stride array) — the token is
  schema, exactly where that ADR put array-ness.
- The token set is **open**: an owner MAY declare a token outside this table
  (RFC-0010's `rgb` example). A consumer that does not recognize a token MUST
  treat the dtype as unknown (§A.5) — it MAY still display the raw token.
- The vocabulary is SHOULD-level: owners SHOULD spell the shapes above with
  these tokens so generic consumers converge; the runtime never interprets
  them either way (§C).

#### A.3 `dir` — endpoint direction, from the owner's perspective

`dir` declares the endpoint's role in the dataflow. Its semantics are defined
**from the perspective of the vertex's owner — the device whose local host API
registered the vertex — and from no other perspective**. This sentence is
normative because the "input/output" family of vocabularies is ambiguous in
exactly this way (a device's output is every client's input); the tokens
therefore name the **owner's action**, not a direction of travel, so the
anchoring perspective rides in the token itself:

| Token | Meaning (owner's perspective) | Typical endpoint |
| ---- | ---- | ---- |
| `produce` | The owner **originates** this vertex's `VALUE`; the graph-facing contract is `read` / `await` / subscribe (a SUBSCRIBER on this vertex's `:subscribers[]`). | Sensor reading, status, controller **output port** |
| `consume` | The owner **applies** `VALUE`s that arrive at this vertex to the world behind it; the contract is that peers write — directly, or by binding a SUBSCRIBER edge whose `target` is this vertex. A `read` still serves the latest stored value (one-store-per-vertex) — the last command, not a measurement. | Actuator, setpoint, command endpoint, controller **input port** |
| `both` | The owner both applies external writes and originates updates on its own (announcing per the RFC-0010 §C convention when it does). | Setpoint with local override, mirrored register |

Clarifications, normative:

- `dir` is **advisory routing/UX metadata, never authorization**. Clients MUST
  NOT interpret it as an access statement: the write gate is, as everywhere,
  the vertex's `:acl` — a `produce` vertex with a permissive ACL still admits
  writes, and a single-input `consume` sink still refuses fan-in with its own
  write-`:acl` ([CONTEXT.md](../../../CONTEXT.md) §SUBSCRIBER direction), not
  with `dir`.
- The runtime MUST NOT gate `read`, `write`, `await`, or subscription
  admission on `dir` (§C).
- `dir` is a wire/schema name and is unrelated to the reference
  implementation's L0 `io_dir_t` cache-hook enum
  ([CONTEXT.md](../../../CONTEXT.md) §`io_dir_t`) — the two live in disjoint
  registers (a wire NAME vs a C++ symbol) and MUST NOT be conflated in docs
  or bindings.

#### A.4 No write surface

`dtype` and `dir` have **no write surface**. A field write naming
`settings.dtype` or `settings.dir` — per-field or inside an atomic
multi-field `SETTINGS` write — MUST be rejected with
`ERROR{tr::schema::not_found}` (`SCHEMA_NOT_FOUND`), **regardless of caller**:
the same caller-independent "no surface exists" identity RFC-0010 §A.3 uses
for `ro` app fields, and the behavior the §`0x0B` validation rule (unknown
core-namespace NAMEs reject) already produces on today's implementations —
old and new peers answer these writes identically. The owner changes the
advertisement only through the local declaration API (§B).

### B. Declaration: owner-initiated, local, SHOULD-level

- Declaration is a **local, owner-facing host API**, mirroring the RFC-0010
  §A.2 / RFC-0009 §A.1 owner-initiated doctrine: what a vertex's value *is*
  and which way it faces are device state, so only the device states them.
  There is **no wire operation that declares, alters, or removes** `dtype` or
  `dir`; remote peers read advertisements, never write them.
- Each member is declared independently (a vertex MAY carry `dir` and no
  `dtype`, and vice versa); composites, transport vertices, and other
  vertices that carry no meaningful own `VALUE` simply declare neither.
- Owners **SHOULD** declare both members on every data endpoint intended for
  cross-device orchestration or generic-consumer discovery (the ADR-0021
  standard-field posture: uniform advertisement is what makes the generic
  orchestrator possible). Declaring is never mandatory — a minimal 9-byte
  endpoint with no fields at all remains conformant.
- The owner MAY re-declare at any time (a gateway materializing downstream
  devices at runtime). A change to the advertisement is a property change
  like any other: it does **not** wake `await` and does not propagate; if
  consumers should notice, the owner follows with an ordinary **announce
  write** ([RFC-0010](0010-owner-app-fields-and-schema.md) §C — restated, not
  changed).
- Consistent with the RFC-0010 trust model, the declaration is
  **owner-declared, not runtime-verified**: the runtime guarantees only
  **presence-iff-declared and verbatim token bytes** (§C). An owner that
  declares `f32` and stores text has a device bug, visible to consumers and
  to design-time analyzer tooling — never policed by the runtime
  ([CONTEXT.md](../../../CONTEXT.md) §Resource bound: analyzers police
  designs, not the runtime).

### C. Advertisement, not enforcement — the opacity invariant

The load-bearing rule, stated once, normatively:

- The runtime MUST NOT validate, coerce, reject, or otherwise interpret a
  data-plane `VALUE`'s payload bytes against a declared `dtype` — on write,
  read, delivery, or storage. [docs/spec/v1.md](../v1.md) §1's "a `VALUE` is
  opaque to the protocol" holds exactly as before; this RFC adds description
  *beside* the bytes, never inspection *of* them (the same layer argument by
  which RFC-0010 rejected descriptor-driven validation of app fields, and by
  which [RFC-0008](0008-vertex-operations-assign-propagate.md) removed the
  runtime's last value inspection).
- The runtime MUST NOT gate any operation on `dir` (§A.3).
- What the runtime **does** guarantee: a declared member is present in the
  §A.1 read surfaces with its declared token bytes served **verbatim**, an
  undeclared member is absent, and the two surfaces agree byte-for-byte.
  Presence/absence and the bytes are the whole runtime contract.

### D. Absence means unknown

- A vertex whose owner declared neither member serves `:schema` and
  `:settings` **byte-for-byte as today** — no member, no default token, no
  placeholder.
- Clients MUST NOT assume a default for an absent member: absent `dtype` does
  **not** mean `bytes`, and absent `dir` does **not** mean `both` — both mean
  **unknown**, and a client needing the datum falls back to out-of-band
  knowledge or asks the user. An unrecognized token (§A.2) is treated the
  same as absent for interpretation purposes.

### Files this RFC edits (on acceptance)

- `docs/reference/05-protocol-tlvs.md` — §`0x0B` SETTINGS: `dtype` and `dir`
  added to the payload layout as protocol-reserved **read-only advertisement
  members** (their own table beside the five QoS knobs; validation rule: any
  write naming them rejects `tr::schema::not_found`); §`0x07` POINT: the
  `:schema` read shape gains the two members in the synthesized protocol part
  (§A.1 layout).
- `docs/reference/02-graph-model.md` — §Schema and field discipline: two new
  rows in the core field table (`:settings.dtype`, `:settings.dir` —
  read-only, owner-declared, absent = unknown) and an "endpoint
  self-description" paragraph carrying the §C advertisement-not-enforcement
  rule and the §A.3 perspective rule.
- `docs/reference/12-deployment-profiles.md` — the strawberry profile's
  `schema_registry: :schema POINT (dtype/unit/range, the io_descriptor)` line
  grounds to these members (cross-reference; the imagined slot now exists).
- `CONTEXT.md` — glossary entry *Value dtype / endpoint direction (advertised,
  never enforced)*: the token vocabularies, the owner-perspective rule, and
  the avoid-list ("`in`/`out`/`inout` direction tokens", "the runtime
  type-checks `VALUE`s against dtype", "`dir` gates writes", "absent dtype
  means bytes", "`dir` is `io_dir_t`").
- `core/` reference implementation + `core/CHANGELOG.md` (the per-vertex
  declaration verbs, `read_schema` / `read_settings` emission, the
  `field_write` reject) — follow this RFC, not normative.

## Conformance-vector sketches (what would prove it)

New vectors under `tests/conformance/vectors/v1/` (additive):

1. **`schema-dtype-dir-declared`** — vertex declared `dtype = "f32"`,
   `dir = "produce"`: `read :schema` serves the POINT with the two members at
   the §A.1 bytes exactly; `read :settings` serves the full container with
   byte-identical tokens.
2. **`schema-dtype-dir-absent`** — undeclared vertex: `read :schema` and
   `read :settings` are byte-for-byte today's shapes — no members, no
   defaults; a vertex declaring only `dir` serves only `dir`.
3. **`schema-dtype-dir-no-write-surface`** — a per-field write to
   `:settings.dtype`, a per-field write to `:settings.dir`, and an atomic
   `SETTINGS` write containing either NAME all return
   `ERROR{tr::schema::not_found}` — local-wire and remote (`FWD{WRITE}`),
   caller rights irrelevant; a subsequent read serves the declared tokens
   unchanged.
4. **`value-opacity-under-dtype`** — vertex declared `dtype = "bool"`,
   `dir = "consume"`: a data write whose `VALUE` payload is 8 arbitrary bytes
   is admitted, stored, and delivered **verbatim** (no coercion, no error);
   `read` of the `consume`-declared vertex is served normally. Proves
   advertisement-not-enforcement on both members in one vector.

## Compatibility

- **No protocol-v1 break; no existing conformance vector changes.** Both
  members are optional and absent unless declared: every existing vertex,
  frame, and vector is byte-identical. No new type codes, wire verbs, or
  error identities. Consumers that walk `SETTINGS` by NAME and skip unknown
  keys (the RFC-0010 unknown-member posture) are unaffected.
- **Wire:** two flat NAMEs (`dtype`, `dir`) become protocol-reserved inside
  the vertex `SETTINGS` namespace — constraining future *protocol* knob
  minting only; application fields are collision-proof already (RFC-0010
  §A.1: apps mint only below `settings.app.`). Writes naming the new members
  were `tr::schema::not_found` on old implementations (unknown core NAME) and
  remain `tr::schema::not_found` on new ones (no write surface) — old and new
  peers are indistinguishable on this path.
- **New vectors:** the four listed above.
- **Migration:** none required — deployed devices that declare nothing are
  conformant unchanged. The driving consumer (strawberry-fw) maps its
  existing `io_descriptor_t` dtype/direction onto the declaration call at
  endpoint registration during the ADR-0082 cutover; its HA discovery walker
  and the #412 capability-agnostic editor then read `:schema` instead of
  carrying a side-channel descriptor table.

## Alternatives considered

- **A type tag on the wire `VALUE` itself** (a dtype byte in the payload, or
  an `opt` bit — fw ADR-0057's "the wire carries type + ts" carried over
  verbatim). Rejected: it breaks the opacity invariant
  ([docs/spec/v1.md](../v1.md) §1) that the ADR-0001 extraction deliberately
  chose when it replaced the fw typed-Value seam with opaque `VALUE` + L4
  schema; it taxes every hot-path frame with bytes that are static per
  endpoint; and it repeats the rejected wire-level array marker
  ([ADR-0008](../../adr/0008-arrays-are-schema-property-not-wire-type.md) —
  shape metadata is an L4 schema property, never a wire bit).
- **Runtime enforcement** (reject a write whose `VALUE` bytes mismatch the
  declared `dtype`; reject data writes to `produce` endpoints). Rejected: the
  runtime never interprets application payload (load-bearing claim 5;
  [ADR-0012](../../adr/0012-modular-memory-binding-transparent-router.md)'s
  transparent byte router — a live/raw MMIO view cannot be "type-checked");
  [RFC-0008](0008-vertex-operations-assign-propagate.md) just *removed* the
  runtime's last value inspection, and RFC-0010 rejected descriptor-driven
  validation for the same reason. Enforcement would also turn a metadata typo
  into a data-plane outage. Bad wiring is analyzer territory
  ([ADR-0051](../../adr/0051-delivery-terminates-at-target-no-dispatch-limits.md)
  posture: tooling polices designs, the runtime does not).
- **Deriving direction from observable state** (who holds `:subscribers[]`,
  what the ACL admits). Rejected: the orchestrator needs direction **before
  any wiring exists** — an unbound actuator input has no subscribers and no
  writers yet, which is precisely when the constructor must tell it apart
  from a sensor; and the ACL is per-subject authorization, not role (reading
  it also needs `READ_ACL`).
- **`in` / `out` / `inout` tokens.** Rejected for the classic perspective
  ambiguity: "out" of the device is "in" to every client, so every consumer
  forever needs the whose-perspective footnote, and half will get it wrong
  (the RS-232 DTE/DCE lesson). `produce`/`consume` name the owner's action —
  the perspective is inside the token — and reuse the glossary's existing
  producer/consumer axis ([CONTEXT.md](../../../CONTEXT.md) §SUBSCRIBER
  direction, §Lazy / on-demand source).
- **Carrying the members as app fields** (`settings.app.dtype` by
  convention). Rejected: this datum is the **standard** half of ADR-0021's
  field split — it must be one protocol-minted name so a generic orchestrator
  and the HA walker work cross-vendor; an app-namespace convention is
  per-owner vocabulary by definition (RFC-0010 §A.1), so every consumer would
  need a vendor registry — the exact failure the standard/private split
  exists to prevent.
- **A second dtype vocabulary** (adopting fw ADR-0057's `BOOL`/`F32_ARRAY`
  SCREAMING tokens on the wire). Rejected: RFC-0010 §B.1 already put
  lowercase tokens in print for app-field descriptors; two spellings of one
  concept is a fork consumers pay for forever. This RFC enumerates the one
  vocabulary and gives the 1:1 fw mapping (§A.2) so the consumer's cutover is
  a table lookup.
- **A new descriptor TLV type code** (a dedicated `DTYPE`/`IODESC` code).
  Rejected: the retired-LIST lesson — structure is `opt.PL=1` plus a purpose
  type byte, and the purpose container for vertex properties already exists
  (`SETTINGS`); core codepoints are scarce and nothing here needs one.
- **Free-text `DESCRIPTION`** (`0x03`) as the carrier. Rejected: not
  machine-readable — #412's requirement is a datum an editor can branch on,
  not display.

## Discussion

Genuinely contentious points, flagged for the comment window:

1. **`dir` vs `direction` as the member name.** `dir` is chosen for symmetry
   with the terse knob style and the token-anchored semantics; the risk is
   proximity to the reference implementation's `io_dir_t` (different
   register, §A.3 note). If reviewers find the three characters too loaded,
   `direction` costs 6 more bytes per schema read and nothing else.
2. **Mirroring into `read :settings`** (§A.1). Chosen so the one-traversal
   renderer sees one container; the counterargument is that `:settings` is
   configuration and these members are immutable-by-wire metadata, so
   `:schema`-only would be purer. Note the adjacent, pre-existing wrinkle
   either way: today's synthesized `:schema` protocol part serves 2 of the 7
   implemented knobs — this RFC appends to it but deliberately does not
   reconcile that mismatch.
3. **How much vocabulary to freeze at v1.** Both token sets are SHOULD-level
   and the dtype set is open (§A.2). Freezing the table buys cross-vendor
   convergence at the cost of the spec owning application vocabulary — the
   same trade RFC-0010 Discussion 4 flags for descriptor members; the two
   should be resolved the same way when v1 freezes.
4. **Granularity of `both`.** One token covers every mixed case; there is no
   "consume, with readable echo" nuance. Deliberate — finer roles belong to
   application vocabulary (an app field or the RFC-0010 descriptor), not to a
   protocol enum that generic consumers must exhaustively understand.
5. **Not pulled up: `unit` / `min` / `max` / `label`.** RFC-0010 §B.1 defines
   them for app-field descriptors, and reference/12's imagined
   `schema_registry` lists `unit`/`range` for the vertex too. This RFC mints
   only the two members #412 needs; if consumers want the rest on the
   vertex's own VALUE, that is a follow-up that should reuse §B.1 verbatim —
   the attachment-point pattern is now established.

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the tracking issue
([#412](https://github.com/avatarsd-llc/libtracer/issues/412)) stays open at
least 14 days for implementer feedback before this document is merged.
Sustained objections and their resolution to be recorded here.