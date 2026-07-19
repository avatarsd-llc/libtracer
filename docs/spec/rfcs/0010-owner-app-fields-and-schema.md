<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0010 — Owner-writable application property fields: the field descriptor table, the reserved `settings.app` namespace, and owner-defined `:schema`

| Field | Value |
| ---- | ---- |
| **RFC** | 0010 |
| **Title** | Owner-writable application property fields: the field descriptor table, the reserved `settings.app` namespace, and owner-defined `:schema` |
| **Status** | **accepted** (2026-07-19 — maintainer ruling, window waived; in-comment from 2026-07-17, draft since 2026-07-09) |
| **Author(s)** | origin-firmware integration (drafted for maintainer review) |
| **Created** | 2026-07-09 |
| **Comment window closes** | 2026-07-31 (≥ 14 days per GOVERNANCE.md §Spec changes) |
| **Tracking issue** | [#411](https://github.com/avatarsd-llc/libtracer/issues/411) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |
| **Roadmap item** | this RFC is the specification of the **"field descriptor table"** item of the 2026-07-08 architecture-review backlog (the vertex-verbs / lazy-validation / field-descriptor-table cluster; item 2 of that backlog — the `vertex_t` verb seam — shipped as [#338](https://github.com/avatarsd-llc/libtracer/pull/338), and is the seam §D slots behind) |

## Summary

The `:` field plane opens to the **vertex owner**. Today the writable field set
is closed: the reference `field_write` admits `:subscribers[]`, `:acl`,
`:children[]`, and seven fixed QoS knobs under `:settings.*`; every other field
name is `SCHEMA_NOT_FOUND` on write **and** read, and `:schema` reads are
synthesized from two of those knobs. Consequence: an application cannot attach
*any* metadata or configuration to its own vertex on the property plane —
ADR-0021's promise of **device-private fields** ("like driver-private ioctls")
has no substrate. This RFC provides it, in four coupled pieces:

- **A. Owner-declared application fields.** The vertex **owner** — through the
  **local host API**, mirroring the owner-initiated doctrine
  [RFC-0009](0009-vertex-removal-and-subscriber-eviction.md) §A.1 establishes
  for removal — declares named fields under **one reserved application
  namespace inside the settings container: `:settings.app.*`**. Declared
  fields are readable and (where declared writable) writable through the
  ordinary `:field` grammar, locally and remotely; **undeclared names remain
  `SCHEMA_NOT_FOUND`** — the `ENOTTY` default survives, opened only where the
  owner named a hole.
- **B. The field descriptor table.** Declaration, validation, and
  self-description are **one structure**: the owner installs a per-vertex
  descriptor table (name, writability, and an opaque-to-the-runtime descriptor
  record — dtype, unit, range, …). `field_write` consults it after the
  protocol branches; `read_schema` serves its projection **merged alongside**
  the synthesized protocol part, with defined precedence — making
  reference/02's "a vertex exposes a schema describing every writable field"
  implementable for application fields.
- **C. Change notification — restated, not changed.** Field writes do **not**
  wake `await` and do **not** propagate. A property change is followed by an
  ordinary **announce write** at the vertex (the consumer convention already
  ratified in the 2026-07-08 backlog ruling and fw ADR-0076); this RFC states
  it normatively so consumers have one documented contract.
- **D. Storage: field bytes ride the vertex.** A declared field's value is a
  **bare TLV stored inside the vertex** — no subscriber list, no edge
  machinery, no per-field vertex; cost = its bytes plus one table slot. On the
  reference implementation it is one more verb pair on the `vertex_t` seam
  (#338), exactly the `set_acl` / `acl_bytes` store-verbatim pattern.

No new wire verbs, no new type codes, no new error identities. The wire-visible
change is that field paths which previously *always* errored become serviceable
when — and only when — the owner declared them, plus one reserved key (`app`)
inside the vertex `SETTINGS` namespace.

## Motivation

1. **The measured gap (F2 slice 1 of the originating production firmware — an
   ESP32-C6 smart-agriculture node; pin `94fc98d`, 2026-07-09).**
   `core/src/graph.cpp` `field_write` is a closed set: `subscribers[]` /
   `acl` / `children[]` plus exactly seven `settings.*` QoS knobs
   (`reliability`, `durability`, `priority`, `history_keep_last`,
   `queue_max_bytes`, `deadline_ns`, `store_ref_min_bytes`); every other field
   name returns `SCHEMA_NOT_FOUND` on write **and** on read. `:schema` reads
   are synthesized (the vertex name plus two QoS knobs) — there is nothing an
   owner could put there. And a `PL`-set `POINT` written as a *value* is
   branch-write grammar ([RFC-0005](0005-subtree-subscriptions.md) §B — it
   decomposes), so a POINT-shaped descriptor record cannot even be parked in a
   data vertex as an opaque value. An application therefore has **no way to
   attach metadata or configuration to its own vertex** anywhere on the
   property plane.
2. **The doctrine already promises this.** ADR-0021 rule 3: fields are
   "standard *and* device-private — like ioctls"; the device "owns the
   **catalog** of what each field accepts". Reference/02 §Schema and field
   discipline: "a vertex exposes a **schema** describing every writable
   field", and §The graph imposes no shape: a vertex's writable fields are
   "whatever the schema says". None of that is implementable today: there is
   no way to *have* a device-private field, and no way for the schema to say
   anything the runtime did not synthesize. This RFC is the missing mechanism
   — while keeping the collision-proofing that motivated the closed set
   (§A.1) and the closed-by-default `ENOTTY` posture (§A.2).
3. **A real consumer is blocked on it, and its workaround is the exact cost
   this protocol exists to avoid.** The origin firmware's device-graph schema
   (its ADR-0076, device-graph taxonomy and path algebra) rests on two
   decisions with no upstream substrate: *properties are bare fields* (only
   subscribable things are vertices) and *app configuration lives under one
   property container, `:settings.*`, sibling to protocol-owned names*. Its
   Gate-A parity checklist (fw `doc/gate-a-parity-checklist.md`) collapses the
   majority of 64 protobuf handlers onto exactly these writes —
   `:settings.persist`, `:settings.ha_exposed`, per-parameter
   `CTRL:settings.<param>` writes described by `:schema` (dissolving an opaque
   params blob and its `STALE_CLIENT` failure class), `/hw/.../ :settings.*`
   device config — and its HA/MQTT auto-discovery becomes a `:schema` walker.
   With no writable substrate, fw's interim stores a descriptor record in a
   **`<endpoint>/meta` child vertex — at a measured ≈ 900 B of heap per vertex
   on the ESP32-C6** (fw `doc/libtracer-cutover-estimates.md` §6 and §7,
   measured 2026-07-09: the full ~100-endpoint mirror exhausted the heap),
   i.e. the per-vertex cost the MCU vertex diet fights, spent on machinery
   (subscriber lists, ACL slots, LKV) that a static descriptor never uses. The
   fw estimates table lists "owner-writable app property fields +
   owner-defined `:schema`" as **Gate-A blocking**.
4. **The roadmap already names it.** The 2026-07-08 architecture-review
   backlog carries a "field descriptor table" item in the vertex-verbs /
   lazy-validation / field-descriptor-table cluster. Item 2 of that backlog
   (the `vertex_t` verb interface, #338) built the seam; this RFC specifies
   the table that was to slot behind it.

## Proposed change

### A. Owner-declared application fields

#### A.1 The namespace: one reserved subkey in the settings container — `:settings.app.*`

Application fields live under **`:settings.app.<name>…`** — a single reserved
subkey of the existing vertex settings container. `app` becomes a **reserved
NAME inside vertex `SETTINGS`** (reference/05 §`0x0B`): the protocol MUST
never mint a QoS or machinery knob named `app`, and implementations MUST NOT
accept `settings.app` as a protocol knob. Everything below `settings.app.` is
**owner-defined**: names, nesting (nested `SETTINGS` per §`0x0B`'s existing
module-namespacing shape), and value bytes are the application's, opaque to
the runtime.

Why this spelling and not a sibling container (`:app.*`) or bare names
(`:count`):

- **Bare app names can never be collision-proofed.** Any future protocol field
  name collides forever with somebody's app field; tooling would need a
  registry to tell machinery from settings. (This is the same reasoning that
  led the driving consumer to a container — fw ADR-0076, "Considered options".)
- **A flat share of `settings.*` is collision-prone too.** The protocol
  already owns names *inside* the settings container (the seven implemented
  knobs, and RFC-0008 §C plans `settings.delivery_mode` wire config there).
  An app knob named `delivery_mode` — a perfectly plausible controller
  parameter — would collide. One reserved subkey ends the race: the protocol
  keeps minting flat knob names forever; applications only ever mint below
  `.app.`; **one reservation, collision-proof both ways**.
- **It matches the namespacing precedent already in print.** Reference/02
  §Module-namespaced extension fields and reference/05 §`0x0B` already nest a
  module's fields under the module's NAME inside `SETTINGS`
  (`NAME "transport_tcp" SETTINGS{…}`). The application is one more namespace
  owner; `app` is its name. No new grammar.
- **One config container keeps one traversal.** A generic settings-panel
  renderer (the fw ADR-0076 use case; HA discovery) reads **one** property
  tree — `read :settings` — and gets protocol knobs and app config in a single
  record, distinguished by the reserved subkey. A sibling `:app` container
  would split configuration across two reads and two schema sections, and
  would spend a new top-level protocol bare name (the scarce resource) to do
  it.

The **alternative** (`:app.*` as its own container, giving apps a shorter
spelling and a container-level ACL story of its own) is recorded in
§Alternatives and flagged in §Discussion — including the consequence for the
driving consumer's spelling (fw ADR-0076 wrote `:settings.count`; under this
RFC that is `:settings.app.count` — a one-token rename fw has ruled
acceptable at cutover time, before any name freezes).

#### A.2 Declaration is owner-initiated and local — the RFC-0009 §A.1 doctrine

- Declaration of application fields is a **local, owner-facing host API** —
  the mirror of `register_vertex`, exactly as RFC-0009 §A.1 makes removal
  owner-facing: the graph is a projection of device state, and what fields a
  vertex has *is* device state. There is **no wire operation that declares a
  field**; a remote peer can write declared fields (§A.3), never invent them.
- Reference shape (normative for the reference, informative for others): the
  owner installs, per vertex, a **field descriptor table** — an ordered set of
  entries `{ name (below settings.app.), access, descriptor-bytes,
  initial-value? }` where `access ∈ { ro, rw, wo }` is the **owner-declared
  remote-writability** and the descriptor bytes are the §B schema record for
  the field. Installation happens at any time the owner chooses (typically
  registration time); replacing the table is allowed and takes effect
  atomically with respect to concurrent field operations on that vertex.
- **Undeclared stays `ENOTTY`.** A `:field` write or read naming anything
  under `settings.app.` that the table does not declare returns
  `SCHEMA_NOT_FOUND` — as does every non-protocol name outside
  `settings.app.` (unchanged). The closed set stays closed **by default**;
  the descriptor table is the owner naming the holes. This preserves
  ADR-0021's `ENOTTY` posture and keeps `lazy validation` cheap: validation
  of an app-field write is one table lookup, not a schema parse.

#### A.3 Write semantics and gating

- **Local (owner) writes** to a declared field always succeed regardless of
  the declared `access` — the owner is updating its own projection (`ro`/`wo`
  constrain *remote* callers; the owner is not a caller).
- **Remote writes** (a `FWD{WRITE}` with a `:settings.app.…` field tail
  arriving at the terminus, or any caller-attributed field write) are admitted
  iff **both** gates pass, in this order:
  1. the field is declared with `access = rw` or `wo` — otherwise
     `SCHEMA_NOT_FOUND` (a field the schema does not declare writable has no
     write surface: the `ENOTTY` of writing a read-only ioctl; deliberately
     caller-independent — see §Discussion 2);
  2. the caller holds the ordinary **WRITE** right on the vertex — otherwise
     `tr::access::denied`, like any field write (control-plane writes are
     ACL-gated; nothing new here). Owners that want *no* remote app-field
     writes at all simply declare everything `ro` — remote writability is
     owner-declared, exactly as the mission of ADR-0021 rule 3 intends ("the
     device owns the catalog of what each field accepts").
- The written value is stored **verbatim** (bytes in, bytes out — §D). The
  runtime performs **no dtype/range validation against the descriptor**: the
  descriptor is self-description for consumers, opaque to the runtime (§B);
  semantic validation is the owner's, in the §C apply step. (An
  implementation MAY offer opt-in shape checks as a host convenience; they
  are not conformance surface.)
- **Field writes to a nonexistent vertex do not create it** — the existing
  rule (`:field` control writes keep `tr::path::not_found`; write-creates is
  a data-plane behavior) is unchanged and applies to app fields.
- Writes to a declared field **notify the owner** through a host seam (an
  optional per-vertex handler, the reference's `handlers_t` pattern): the
  owner learns "field F was written" and applies the configuration to the
  device — possibly restructuring children (the fw LED-strip `count`
  example), and then announcing (§C). A vertex with no handler simply stores
  the bytes (a passive metadata field — e.g. a label a UI writes and other
  UIs read).

#### A.4 Read semantics

- `read <v>:settings.app.<name>` of a declared field serves the stored TLV
  bytes verbatim (refcount view, like `:subscribers[N]`), gated by the
  vertex READ right like every control-surface read. A declared-`wo` field
  (secrets: the fw HA password, WireGuard private key) returns
  `SCHEMA_NOT_FOUND` on read — write-only means *no read surface*, so a
  secret is never mirrored back.
- `read <v>:settings.app` serves the whole app container as one
  `SETTINGS`-shaped record (declared, non-`wo` fields with their current
  values). `read <v>:settings` serves the full settings container: the
  protocol knobs **and** the nested `app` record — the one-traversal property
  tree the generic renderer walks. (Bare `:settings` and `:settings.app`
  container reads are new read surfaces this RFC adds; the per-knob protocol
  reads they subsume are today unimplemented anyway.)
- A declared field that has never been written and carries no initial value
  reads as `NOT_FOUND` (declared but empty — distinct from `SCHEMA_NOT_FOUND`,
  undeclared), and is omitted from container reads.

### B. Owner-defined `:schema`

#### B.1 The owner installs a schema record

Through the same local API (the descriptor table §A.2 — the schema *is* the
table's projection), the owner provides, per vertex, a **schema record
describing its application surface**: one descriptor per declared field. The
descriptor is a structured TLV, **opaque to the runtime** (stored and served
verbatim, like `:acl` bytes); its *vocabulary* is a SHOULD-level convention so
generic consumers (settings renderers, HA discovery walkers) converge:

```
NAME <field-name>  SETTINGS (PL=1) {
  ; NOTE: `access` is NOT here — the runtime projects it (see below).
  NAME "dtype"  VALUE <type tag>        ; SHOULD — value shape (bool/i32/u32/f32/utf8/rgb/…)
  NAME "unit"   NAME  <unit>            ; MAY  — display unit
  NAME "min"    VALUE …  NAME "max" VALUE … ; MAY — advisory range
  NAME "label"  NAME  <human label>     ; MAY
  …                                     ; owner-defined extras — opaque
}
```

**The owner MUST NOT supply an `access` member; the runtime projects it.** The
served record leads with a runtime-projected `access` — read from the
descriptor table (§A.2), not from these bytes — and the owner's descriptor
follows verbatim. `access` is the one member the runtime holds natively, which
is exactly why it owns the spelling: a projected member **cannot contradict the
write gate**, whereas an owner-supplied one could claim `rw` on a field the
table declares `ro`. An owner that supplies `access` anyway does not override
anything — it emits a **duplicate member**, and a consumer reading NAME-keyed
members would see two.

This is normative already: [docs/reference/05](../../reference/05-protocol-tlvs.md)
§`0x07` fixes the record as `NAME "access" VALUE <"ro"|"rw"|"wo">` — "runtime-projected
from the table — the one member the runtime owns (it cannot be lied about)" —
followed by `<owner descriptor bytes, verbatim>`, and `read_schema` emits exactly
that. An earlier draft of this section listed `access` inside the owner's
descriptor with "MUST match the declared access (§A.2)", contradicting both; the
projection is the design that survived contact with the compiler, and this RFC
now says so.

The runtime enforces none of the rest: everything but `access` is
self-description. This is the "lazy validation" of the backlog cluster: the
runtime validates *addressing* (declared or not) eagerly and cheaply, and
leaves *shape* validation to the consumers and the owner, who own the
vocabulary.

#### B.2 `read :schema` — merge shape and precedence

`read <v>:schema` serves **one `POINT`** containing both parts:

```
POINT (PL=1) {
  NAME      <vertex name>              ; as today
  SETTINGS  <protocol part>            ; synthesized by the runtime — the protocol
                                       ; fields/knobs this vertex actually serves
  NAME "app"  SETTINGS (PL=1) {        ; owner part — present iff a descriptor
    <field descriptors, §B.1>          ; table is installed; served VERBATIM
  }
}
```

- The **protocol part is synthesized and authoritative** for protocol fields:
  the runtime, not the owner, says which machinery fields exist
  (`:subscribers[]`, `:acl`, `:children[]`, the implemented `settings.*`
  knobs). The owner part MUST NOT be consulted for them; an owner cannot lie
  about — or hide — protocol machinery.
- The **owner part is authoritative for `settings.app.*`** and is served
  verbatim (no runtime merge *into* it — precedence by position, zero merge
  logic on an MCU: the read is two stored/synthesized byte runs concatenated
  inside one POINT).
- A name collision cannot occur by construction: the two parts describe
  disjoint namespaces (flat protocol knobs vs the reserved `app` subtree) —
  which is the quiet payoff of the §A.1 namespace decision.
- This makes reference/02's discipline — "a vertex exposes a schema describing
  every writable field", "module fields MUST appear in the vertex's `:schema`
  output" — **implementable** for the application namespace: the fw HA walker
  reads `:schema`, finds the app part, and builds discovery from it; the fw
  constructor reads a controller's `:schema` and renders its parameter panel
  with no compiled-in layout knowledge (killing the `STALE_CLIENT` class —
  fw checklist row 25).

### C. Field-change notification — the announce-write convention (restated)

Unchanged and now normative in one place (this restates the 2026-07-08
backlog ruling / fw ADR-0076 decision 1; it changes nothing):

- A field write — protocol or app, local or remote — does **NOT** wake `await`
  on the vertex, does **not** advance the vertex's write sequence, and does
  **not** propagate along subscription edges. The property plane is silent by
  design; `await` on a single field is deliberately unsupported.
- A property change that consumers should notice is followed by an ordinary
  **announce write at the vertex** — the owner performs a data-plane
  `assign` + `propagate` (RFC-0008) of the vertex once the change is applied.
  Subscribers (which observe the vertex, and via RFC-0005 bubbling its
  ancestors' subscribers too) receive one ordinary delivery and re-read the
  small property tree if they care which knob moved. Notification is at
  **vertex granularity**, by the owner's act, on the owner's cadence.
- For a **remote** app-field write, the announce is the **owner's**
  responsibility, in its §A.3 apply handler — after the device state actually
  changed (which may include restructuring children; the structural change is
  announced the same way). The graph never announces on the writer's behalf:
  an announce is a statement that the device applied the change, and only the
  owner can truthfully make it.
- Consumers MUST NOT poll fields for change detection (fw cutover risk
  register: "UI must not poll") and MUST NOT expect per-field wakeups. A
  config datum that genuinely needs independent subscribers is **promoted to
  a child vertex** — the ratified field-promotion doctrine (CONTEXT.md §Field
  promotion), which this RFC leaves exactly as is.

### D. Storage: field bytes ride the vertex

The memory model, stated normatively so the MCU cost is explicit:

- A declared field's value is a **bare TLV stored inside the vertex**: no
  subscriber list, no ACL slot of its own, no LKV/refcount machinery, no
  vertex-map entry, no path-key allocation. Cost = the value's bytes + one
  descriptor-table slot (name + access + descriptor bytes). The descriptor
  table itself is per-vertex bytes, installable from ROM/flash-resident
  statics on constrained profiles (an implementation SHOULD allow the
  descriptor bytes to be non-owning/static, since they are typically
  compile-time constants).
- Contrast, measured: the consumer's interim — a `/meta` **child vertex** per
  endpoint carrying the descriptor record — costs **≈ 900 B of heap per
  vertex on the ESP32-C6** (fw estimates §7, 2026-07-09: the ~100-endpoint
  mirror exhausted the heap and the boot died; a 20-vertex subset boots with
  75 KB free). A descriptor that would ride the vertex as ~40–120 bytes of
  TLV pays ~10× that as a vertex, all of it machinery the descriptor never
  uses. The property plane exists precisely so that *only subscribable things
  are vertices* (fw ADR-0076 decision 1); this RFC is what lets a consumer
  actually obey that rule.
- Reference-implementation seam (informative): app-field storage and the
  descriptor table slot behind the **`vertex_t` verb interface** (#338) as
  one more verb pair — the exact `set_acl(raw, parsed)` / `acl_bytes()`
  pattern: bytes stored under the vertex mutex, served back verbatim,
  in-flight reads keep refcounted views. `graph_t::field_write`'s closed-set
  tail (`return SCHEMA_NOT_FOUND`) gains one descriptor-table lookup for the
  `settings.app.` prefix; `read_schema` gains the verbatim owner-part
  append. No cross-vertex orchestration is touched — which is precisely what
  the #338 seam was cut for.
- **Persistence is out of scope**: fields persist exactly as the owner
  persists them (the fw TLV-replay journal is a consumer choice). The graph
  holds bytes; it does not own durability.

### Files this RFC edits (on acceptance)

- `docs/reference/02-graph-model.md` — §Schema and field discipline: the core
  writable-field table gains the `:settings.app.*` row (owner-declared,
  descriptor-gated); §Module-namespaced extension fields gains the `app`
  reservation; the announce-write convention is recorded beside the
  stale/invalid discipline; §The graph imposes no shape cross-references the
  descriptor table.
- `docs/reference/05-protocol-tlvs.md` — §`0x0B` SETTINGS: `app` becomes a
  reserved key (owner-defined subtree, opaque values); §`0x07` POINT /
  `:schema`: the two-part read shape (§B.2).
- `CONTEXT.md` — glossary entries: *application field / field descriptor
  table* (owner-declared, local-API-only declaration, `ENOTTY` default),
  *announce write* (the notification convention, promoted from consumer lore
  to spec vocabulary); §Field promotion gains the "app field vs promoted
  vertex" boundary sentence.
- `core/` reference implementation + `core/CHANGELOG.md` (the descriptor-table
  verbs on `vertex_t`, the `field_write` tail lookup, `read_schema` owner-part
  append, container reads for `:settings` / `:settings.app`) — follow this
  RFC, not normative.

## Conformance-vector sketches (what would prove it)

New vectors under `tests/conformance/vectors/v1/` (additive), plus host-API
tests where the surface is local:

1. **`app-field-declare-read-write`** — owner declares
   `settings.app.kp` (`rw`, f32 descriptor) with an initial value: local
   `read :settings.app.kp` serves it; a remote `FWD{READ}` of the same field
   serves identical bytes; a remote `FWD{WRITE}` (caller holding WRITE)
   stores; both sides read back the new bytes verbatim.
2. **`app-field-undeclared-enotty`** — on the same vertex:
   write and read of `settings.app.undeclared`, of a bare unknown field
   (`:frobnicate`), and of an unknown protocol knob (`settings.nope`) all
   return `SCHEMA_NOT_FOUND` — before and after the descriptor table is
   installed (the table opens only its own names).
3. **`app-field-gating`** — declared `ro` field: remote write ⇒
   `SCHEMA_NOT_FOUND`; local owner write succeeds. Declared `rw` field,
   caller without WRITE ⇒ `tr::access::denied`. Declared `wo` field: remote
   write (with WRITE) succeeds; read ⇒ `SCHEMA_NOT_FOUND` (the secret never
   mirrors back).
4. **`app-schema-merge`** — vertex with an installed table:
   `read :schema` returns one POINT holding the synthesized protocol
   SETTINGS **and** `NAME "app" SETTINGS{…}` with the owner descriptors
   byte-verbatim; without a table, the POINT has no `app` member (today's
   shape — no existing vector changes).
5. **`app-field-announce-flow`** — subtree subscriber above the vertex;
   remote app-field write: **no delivery occurs and no `await` wakes** from
   the field write itself; the owner's apply handler fires, owner performs
   the announce write; the subscriber receives exactly ONE ordinary delivery
   (the announce), after which a re-read of `:settings.app` shows the new
   value.
6. **Host: storage cost** — declaring N fields on a vertex adds no vertices
   to the map, no subscriber slots, no edges (`ancestor_walks()` /
   edge-count observables flat); memory delta ≈ descriptor + value bytes
   (informative assertion, the §D contrast with the ≈ 900 B/vertex child
   workaround).
7. **Host: container reads** — `read :settings` returns protocol knobs +
   nested `app` record in one SETTINGS; `read :settings.app` returns the app
   record alone; a never-written declared field is omitted and reads
   `NOT_FOUND` individually.

## Compatibility

- **No existing conformance vector changes.** Every behavior this RFC defines
  occupies previously-erroring space (`SCHEMA_NOT_FOUND` on all of it); a
  vertex whose owner installs no descriptor table is byte-for-byte today's
  vertex, including its synthesized `:schema` shape. No new wire verbs, type
  codes, or error identities.
- **Wire:** the `app` key reservation inside `SETTINGS` constrains future
  *protocol* minting, not any existing frame; the two-part `:schema` POINT is
  additive (consumers ignoring unknown members per the unknown-key rule are
  unaffected).
- **Host API:** additive (the descriptor-table install verbs, the container
  reads). No existing caller changes.
- **The driving consumer** migrates by deleting: the `/meta` child-vertex
  workaround and its walker filtering go away; fw ADR-0076's `:settings.*`
  spellings gain the `.app` segment (`:settings.count` →
  `:settings.app.count`) — a rename fw takes at cutover, before any name
  freezes (fw checklist §naming already batches cheap renames there).

## Alternatives considered

- **Bare app field names (`:count`) on the property plane.** Rejected — never
  collision-proofable against future protocol field names; the consumer's own
  ADR rejected it for the same reason (fw ADR-0076, considered options).
- **Flat sharing of `settings.*` (the fw ADR-0076 spelling as written).**
  Rejected — the protocol already owns flat names inside the container and
  plans more (RFC-0008's `settings.delivery_mode` wire config); every future
  knob is a potential app collision. One reserved subkey (`app`) is the same
  decision, collision-proofed; the cost is one extra path token.
- **A sibling top-level container (`:app.*`).** Workable and cleanly
  separated, but it spends a new top-level protocol bare name, splits the
  config surface across two container reads and two schema homes (a generic
  renderer walks both), and duplicates the gating story `settings` already
  has. Kept as the recorded alternative (§Discussion 1) — it is the right
  answer only if the maintainer wants app config *visually* segregated from
  QoS at the path level.
- **A wire-level field-declaration operation** (remote peers defining fields).
  Rejected — the RFC-0009 §A.1 doctrine: the graph is a projection of device
  state, and the field catalog is device state; a peer manipulating the
  projection must go through the device's own logic. Remote peers write
  declared fields; only the owner declares.
- **Store descriptor records in child vertices** (the fw interim, as the
  permanent model). Rejected — measured ≈ 900 B/vertex on the target MCU for
  bytes-worth of static description; pays subscriber/ACL/LKV machinery a
  descriptor never uses; and doubles the enumeration surface every walker
  must filter. It is a fine consumer-side interim precisely because it erases
  cleanly when this lands (same pattern as RFC-0009's tombstone interim).
- **Runtime-enforced dtype/range validation of app-field writes.** Rejected —
  the runtime would be parsing application vocabulary it does not own (the
  ADR-0053/RFC-0008 layer argument: the runtime never interprets application
  bytes); the owner's apply step is the semantic gate. Addressing validation
  (declared / undeclared, writability) stays in the runtime — one table
  lookup, the cheap "lazy validation" split.
- **An open container (any name under `settings.app.` writable without
  declaration).** Rejected as the default — it surrenders the `ENOTTY`
  posture, turns typos into silent state, and leaves `:schema` unable to
  describe the surface (the descriptor table IS the schema). An owner that
  wants open-world behavior can declare a single structured field and version
  its own bytes. (Flagged in §Discussion 3 in case a consumer surfaces a
  genuine need.)
- **Per-field `await`/subscription.** Rejected — re-litigates the settled
  vertex-granularity decision (fw ADR-0076 decision 1; upstream 2026-07-08
  ruling: field-notification RFC dissolved). Field promotion covers the rare
  datum that truly needs its own subscribers.

## Discussion

Genuinely contentious points, flagged for the maintainer:

1. **`settings.app.*` vs a sibling `:app.*` container** (§A.1). The RFC picks
   the shared container with one reserved subkey — namespacing precedent,
   one-traversal renderer, no new top-level name. The sibling container is
   defensible on separation grounds (app config never visually mingles with
   QoS; container-level ACL knobs could differ). Note the consumer-spelling
   consequence either way: fw ADR-0076's flat `:settings.count` becomes
   `:settings.app.count` (chosen form) or `:app.count` (alternative) — fw
   has said renames are cheap until cutover freezes names, so this decision
   should not outlive that window.
2. **The read-only-write error identity** (§A.3): a remote write to a
   declared-`ro` field returns `SCHEMA_NOT_FOUND` (no write surface exists —
   caller-independent, like `ENOTTY`), not `tr::access::denied` (which would
   suggest a different caller might succeed). The opposite reading — "the
   field exists, you may not write it" — is also coherent; whichever is
   chosen should also cover the `wo`-read case symmetrically.
3. **Strictness of declaration**: is the declared-fields-only rule (§A.2)
   ever too rigid — e.g. a gateway materializing fields learned from a
   downstream device at runtime? The owner *can* re-install the table
   dynamically (declaration is not one-shot), which seems sufficient, but if
   a consumer surfaces a real open-container need, the `access` vocabulary
   could grow a container-level entry rather than abandoning `ENOTTY`.
4. **How much descriptor vocabulary to freeze** (§B.1): the RFC makes only
   `access` normative and the rest (dtype/unit/min/max/label) SHOULD-level
   convention. Freezing more buys cross-vendor renderer portability at the
   cost of the runtime owning application vocabulary; the HA-walker and
   generic-renderer consumers should weigh in before v1 freezes.
5. **The unimplemented core-table fields.** reference/02's table also lists
   `:description` and `:liveness.*`, which the reference implementation does
   not serve either. This RFC deliberately does not adopt them —
   `:description` in particular could become a conventional app field
   (`settings.app.label`) instead of core surface; the 02 table should be
   reconciled (trimmed or implemented) when this RFC's edits land, so the
   spec stops listing fields nothing serves.

   **Audited 2026-07-17 — the debt is exactly four fields, all phantom:**

   | `reference/02` row | implementation |
   | --- | --- |
   | `:liveness.heartbeat_hz` | none |
   | `:liveness.last_seen_ns` | none |
   | `:liveness.missed_deadlines` | none |
   | `:description` | none |

   Nothing anywhere serves a `:liveness` field; the only `liveness` in `core/`
   is transport-internal (the QUIC idle timeout, the ADR-0044 CAN peer TTL) and
   is unrelated to this surface. The whole `:liveness.*` family is a **design
   that was never built**, not a partial one — there is no heartbeat plane to
   report on. Note `deadline_ns` exists as a *knob* and its 02 row says "max
   time between writes before liveness fault", but no code raises that fault
   either, so the knob is stored and served and otherwise inert.

   Because reference/02 is normative by incorporation
   ([ADR-0007](../../adr/0007-normative-wire-format-by-incorporation.md)), trimming these
   rows **is a spec change** and cannot be a drive-by docs fix — which is
   precisely why it belongs to this RFC's edit list rather than to a bug.

   **Recommendation: trim all four, adopt none.** `:liveness.*` presupposes a
   runtime that polices timing, which is the posture
   [RFC-0007](0007-delivery-terminates-at-target.md)/[ADR-0051](../../adr/0051-delivery-terminates-at-target-no-dispatch-limits.md)
   deliberately removed ("analyzers police designs, not the runtime"); building
   it now would re-mint the thing that was just deleted. `:description` is
   pure self-description with no runtime role, which is exactly what
   `settings.app.` is for — this RFC's own substrate — so it costs nothing to
   drop and can be spelled `settings.app.label` today by any owner who wants
   it. If a consumer later needs a liveness plane, it should arrive as its own
   RFC with a design, not be inherited from a table row.

   **The maintainer rules; this is not the author's to decide** — trimming
   normative rows is a spec change with a live comment window.

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the tracking issue stays
open at least 14 days for implementer feedback before this document is merged
(unless the standing solo-maintainer waiver is applied, as on
RFC-0002/0005/0008). Sustained objections and their resolution to be recorded
here.
