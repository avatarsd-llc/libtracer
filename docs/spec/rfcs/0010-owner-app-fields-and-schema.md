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

  > **Erratum (2026-08-12), [#435](https://github.com/avatarsd-llc/libtracer/issues/435) —
  > see §Erratum at the end of this document.** This clause's `SCHEMA_NOT_FOUND` answers,
  > for names under `settings.app.`, govern the **owner and callers the vertex ACL
  > admits**. A caller the ACL denies the operation's right receives
  > `ERROR{tr::access::denied}` **before** any name under `settings.app.` is resolved, so
  > a protected vertex never discloses owner-field existence through the error channel.

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

  > **Erratum (2026-08-12), [#435](https://github.com/avatarsd-llc/libtracer/issues/435) —
  > see §Erratum at the end of this document.** The numbered order above is **corrected**:
  > for a caller-attributed write the ordinary **WRITE right (item 2) is evaluated
  > first**, and the declared-writability check (item 1) applies to callers that pass it.
  > Both checks remain; only their order changes. As written, the pre-gate
  > `SCHEMA_NOT_FOUND` disclosed which owner names exist (undeclared vs declared-`ro` vs
  > gated) to callers the ACL denies.
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

  > **Erratum (2026-08-01), [RFC-0022](0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.B/§4:** "the implemented
  > `settings.*` knobs" now enumerates **nothing**. RFC-0022 deleted `settings_t` outright:
  > `reliability`, `priority` and `durability` moved to the subscription's packed delivery
  > policy (they describe one producer→subscriber relationship, not a vertex); `deadline_ns`
  > and `queue_max_bytes` were deleted as inert and without a coherent per-vertex meaning;
  > and `history_keep_last` and `store_ref_min_bytes` became owner-side declarations with no
  > wire surface at all (they are construction parameters, not QoS). §Motivation item 1 below
  > still lists the pre-RFC-0022 set of seven; read it as the historical record it is.
  >
  > The clause above is unchanged in force — the runtime is still authoritative for the
  > protocol part — and its enumeration is now *empty and therefore complete*, which is the
  > condition [#706](https://github.com/avatarsd-llc/libtracer/issues/706) was filed about and
  > RFC-0022 dissolved rather than fixed. §A.4's `:settings` container is likewise unchanged in
  > shape and emptied of knobs: it reads `SETTINGS{ [NAME "app" SETTINGS{…}] }`, and an empty
  > `SETTINGS{}` when the vertex declares no app fields.
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

## Erratum (2026-08-12) — §A's `SCHEMA_NOT_FOUND` answers govern callers the ACL admits; a denied caller learns nothing about owner names ([#435](https://github.com/avatarsd-llc/libtracer/issues/435))

**What the text said.** §A.2's "undeclared names remain `SCHEMA_NOT_FOUND`" (restated
caller-unqualified in [reference/05](../../reference/05-protocol-tlvs.md) §`0x0B`:
*"undeclared names return `ERROR{tr::schema::not_found}` on read and write"*), and §A.3's
numbered gate order — declared-writability (`SCHEMA_NOT_FOUND`) **before** the caller's
WRITE right (`tr::access::denied`).

**What was wrong.** Read literally, the error channel disclosed the **owner's field-name
set** to callers the vertex ACL denies. The reference write door answered a denied caller
`SCHEMA_NOT_FOUND` for an undeclared or declared-`ro` `settings.app.` name *before* any
ACL evaluation, so probing spellings distinguished undeclared / `ro` / writable — while
the read door's READ gate sat above resolution and answered the same caller
`PERMISSION_DENIED` for the same spelling: one name, two answers, split by path and by
caller. The write-side pre-gate resolution arrived with the
[#430](https://github.com/avatarsd-llc/libtracer/issues/430) hoist, whose justification —
*"knob names are a fixed, published constant of the protocol, not a per-node secret"* —
is exactly the property owner-defined names do **not** have. And
[reference/05](../../reference/05-protocol-tlvs.md) §Gating `:identity` already names the
caller-dependent error split as the failure mode to avoid: *"a denied caller gets
`PERMISSION_DENIED` where an allowed caller gets `SCHEMA_NOT_FOUND` — which leaks the
caller's authorization state through an error code."*

**The correction — namespace-governed disclosure, applied identically on read and
write.** Whether a name's *existence* may be answered before the ACL gate is governed by
whether the name set is a published protocol constant or an owner secret; the gate always
protects the **value**:

| namespace | name set | order (name-validity), read **and** write | denied caller, nonexistent name | denied caller, existent facet |
| --- | --- | --- | --- | --- |
| protocol-owned (the `{subscribers, acl, children, settings, schema, identity}` field namespace; the withdrawn flat knobs) | published by spec, identical on every node | resolve-before-gate | `ERROR{tr::schema::not_found}` — discloses only published spec text | `ERROR{tr::access::denied}` — the value stays gated |
| owner-defined (`settings.app.*`) | owner-declared, per-node | **gate-before-resolve** | `ERROR{tr::access::denied}` | `ERROR{tr::access::denied}` |

Concretely:

- §A.2's and §A.3's `SCHEMA_NOT_FOUND` answers for `settings.app.` names (undeclared;
  declared-`ro` write; §A.4's `wo` read) are answers to the **owner and to callers the
  ACL admits the operation's right** — among those they remain exactly as specified, and
  caller-independent. A caller the ACL denies receives `ERROR{tr::access::denied}` before
  any `settings.app.` name is resolved: for a denied caller the answer is uniform over
  declared, undeclared, `ro` and `wo`, so the error channel discloses neither the owner's
  names nor which spellings exist.
- §A.3's numbered order is corrected accordingly (see the inline erratum note there): the
  WRITE-right gate is evaluated first for caller-attributed writes; both checks remain.
- The **protocol-owned** namespace resolves name-validity **above** the gate on both
  doors — the write door always did; the read door is aligned by this erratum (a denied
  caller's read of `:status`, of bare `:subscribers`, or of a withdrawn
  `:settings.<knob>` answers `ERROR{tr::schema::not_found}`, as its write already did).
  This is **name-validity only, never the value**: a denied caller against any existent
  facet (`:schema`, `:settings`, `:acl`, a declared app field, …) still receives
  `ERROR{tr::access::denied}`, the pinned selector-shape divergences
  ([#869](https://github.com/avatarsd-llc/libtracer/issues/869)) are untouched, and
  `:identity` remains the sole facet that resolves *fully* above the gate
  ([RFC-0011](0011-node-identity-facet.md) §C).

**Instrument: erratum, not amendment** ([GOVERNANCE.md](../../../.github/GOVERNANCE.md)).
The immutable spec pins none of these codes: which of the two errors a **denied** caller
sees lives in this RFC and reference/05, not in `spec/v1.md`. No grammar, frame shape,
type code or error identity changes — both codes are long in the RFC-0002 registry — and
the correction aligns the text (and the two reference-core doors) with the disclosure
rule the spec already commits to at `:identity` and with #430's own scoping. Maintainer
ruling in [#435](https://github.com/avatarsd-llc/libtracer/issues/435) (grilled
2026-08-06; instrument confirmed 2026-08-12).

**Record.** The governing principle is to be recorded as an ADR whose number settles
after the [#894](https://github.com/avatarsd-llc/libtracer/issues/894)/[#897](https://github.com/avatarsd-llc/libtracer/issues/897)
reclamation ADR is written; until then this erratum and its conformance vector are the
record. Conformance: `tests/conformance/vectors/v1/acl/denied-caller-undeclared-app-field`
(the denied-caller reply, byte-exact), bound behaviourally by
`core/tests/acl_test.cpp` — `test_denied_caller_disclosure_parity` (both doors, both
namespaces, plus the existent-surface controls). Text corrected alongside:
[reference/05](../../reference/05-protocol-tlvs.md) §`0x0B` (the caller qualifier) and
[reference/02](../../reference/02-graph-model.md) §Owner-declared application fields
(the gate order).

## Amendment 1 (2026-08-22) — the node-scoped seam census: a reserved `:stats` field namespace ([#1503](https://github.com/avatarsd-llc/libtracer/issues/1503) step 5)

**Status:** accepted (maintainer ruling 2026-08-22; the 14-day window is waived per
[GOVERNANCE.md](../../../.github/GOVERNANCE.md), as on RFC-0002/0005/0008/0011).

**Why here.** This RFC owns the reserved field namespaces — `:settings.app.*` and the `app`
subkey (§A.1), and the disclosure rule for the protocol-owned name set (§Erratum). A new
reserved field name is its business. [RFC-0011](0011-node-identity-facet.md) §C.1 is cited
as **shape precedent only** — the node-scoped field that "takes no vertex, and every vertex
answers identically" — and is not otherwise amended.

**Motivation.** After [RFC-0004](0004-remote-operation-addressing.md) Amendment 2 a
bulk-ingest producer may write with an empty `src` and thereby give up refusal-by-value
feedback. The sanctioned way such a producer observes downstream pressure is to **poll
ordinary READs of introspection counters** — which requires the
[#1503](https://github.com/avatarsd-llc/libtracer/issues/1503) seam census to be
wire-readable rather than a C++ accessor. Steps 1–4 of that issue built the counters; this
amendment gives them their read surface.

### D.1 The spelling — a node-scoped `:stats` field, in the `:identity` mould

A reserved field name **`stats`** joins the protocol-owned field namespace, which becomes
`{subscribers, acl, children, settings, schema, identity, stats}`. One seam is addressed as

```
<any-vertex>:stats.<seam-class>.<seam-name>
```

The field is **node-scoped**: it takes no vertex, every vertex of a node MUST answer it
identically, and the content describes the **node**, never the addressed vertex. That is
RFC-0011 §C.1's property, applied to a second field.

Nothing else moves. There are **no new vertices**, **no new type codes**, **no new error
identities**, and **no grammar change**: `:stats` is a NAME in the existing `:field.sub`
tail, so [reference/03](../../reference/03-addressing.md) §Reserved characters is
**untouched** — a leading `:` remains illegal inside a NAME segment, and the
`path/path-reserved-brackets` conformance vector stands.

**This clause SUBSTITUTES clause 2 of the 2026-08-22 scope-addition ruling** ("introspection
fields are ordinary vertices with their own (tiny) bounded lists"), which is withdrawn: it is
not expressible in the shipped path grammar without changing §Reserved characters, and the
property it was written to secure is delivered here by construction instead. The normative
isolation sentence is:

> **The read targets whatever vertex the peer already addressed, and is bounded by that
> vertex's existing list.**

A monitoring READ therefore contends for exactly the resources of the vertex the monitor
chose, and a flooded STREAM vertex cannot make a read of some quiet vertex queue behind it —
per-vertex designated pressure, unchanged, with no new namespace to explode and no
control-plane reserve clause.

**The honest caveat, unchanged from the ruling:** a physically saturated shared *link*
([#1494](https://github.com/avatarsd-llc/libtracer/issues/1494) TX-pool territory) delays
every frame including a monitoring read. That is precisely when read-timeout-as-hard-
congestion-signal semantics apply, and the link-seam counters are how the two cases are
distinguished afterwards.

### D.2 NAME validity resolves ABOVE the READ gate

The `stats` namespace is **published spec text, identical on every node** — the
protocol-owned row of this RFC's §Erratum. Accordingly, and on both doors:

- An **unrecognised** `:stats` spelling — bare `:stats`, a bare `:stats.<class>`, an unknown
  class or seam name, any `[N]` / `[]` / `[*]` selector at any level, or any tail deeper
  than `<class>.<name>` — MUST answer `ERROR{tr::schema::not_found}` (`0x0031`)
  **caller-independently**. No `:stats` spelling may have two answers split by who asked.
- A **recognised** seam's VALUE keeps its gate (§D.5).

Bare `:stats` and `:stats.<class>` name nothing **by design**: a census block is served
whole, and aggregating seams into a container is exactly what
[ADR-0079](../../adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md) forbids.

### D.3 One READ = one seam = the whole counter block in ONE TLV

A recognised seam read serves a single structured `SETTINGS` TLV whose members are the
seam's counters as `NAME` / `VALUE` pairs, **sampled in one call**:

```
SETTINGS (PL=1) {
  NAME "<noun>"  VALUE <u64>       ; one pair per counter, fixed-width u64 little-endian
  ...                              ; readers MUST ignore unknown NAMEs
}
```

Normative rules:

- **One call, one block.** The counters MUST be sampled in the single call that answers the
  read. Snapshot coherence (`core/STYLE.md` §Introspection — monotonic since construction,
  sampled unsynchronized, read as the DIFFERENCE between two snapshots) is unachievable
  across six separate field reads, which is why per-counter fields are **a named door and
  not this surface**.
- **The vocabulary is `core/STYLE.md` §Introspection's** — `capacity` / `in_use` / `peak` /
  `refused` / `dropped` / `largest_refused`, used-polarity always. A seam names only the
  nouns it has.
- **Fixed-width `u64` little-endian** values, per the [reference/05](../../reference/05-protocol-tlvs.md)
  integer conventions. A reader MUST ignore unknown `NAME`s and MUST NOT depend on the
  member count or on a seam's member set staying fixed: a seam may grow a noun.
- **Members appear in the seam's declared order**, so a block is a stable byte shape for one
  seam at one version; that order is not a wire guarantee across versions, and a reader keys
  on the `NAME`.

### D.4 The seams this field answers — and the boundary, stated honestly

The field is node-scoped, and the entity that answers it is the **graph** (`tr::graph`, L4).
It therefore serves exactly the seams reachable from the node's own graph:

| spelling | the seam | nouns |
| --- | --- | --- |
| `:stats.mem.control` | the injected failable block source (`graph_t::control_source`) — the #551 seam every peer-provokable allocation draws from | `capacity`, `in_use`, `peak`, `refused`, `largest_refused` |
| `:stats.mem.ring` | the graph-level default receiver-ring source (`graph_t::default_ring_source`) — where a STREAM vertex's admissions are charged absent a per-vertex source | the same five |
| `:stats.graph.delivery` | the graph's own delivery-drop door (`graph_t::delivery_drops`), which the net plane also counts through (`count_external_drop`) | `no_target`, `denied`, `out_of_memory`, `fan_out_truncated` |

**The boundary, and why it is where it is.** The router seams (`fwd_router_t::drop_stats`
and its four injected sources) and the label-table and per-link seams
(`route_handle_t::labels_used` / `labels_exhausted`, `transport_t::drop_stats`) — all shipped
by #1503 steps 3 and 4 — are **NOT** served by `:stats` in this amendment. Those counters are
**per-`fwd_router_t`, per-`route_handle_t` and per-link**, not per-graph: a node may run more
than one router, a router may be shared, and the graph is L4 while the router is the net
plane. Dependencies point **up** the layers only, so the graph cannot reach down to sample
them, and inventing a registration channel so it could is a larger surface than this
amendment was scoped to carry.

Concretely: **the entity that answers `:stats` is the graph, and it answers for the graph.**
Router and link seams stay C++ accessors on the entities that own them, exactly as
[reference/22](../../reference/22-backpressure-and-sizing.md) §6 Recipe A's in-process arm
uses them. Extending the census to the net plane — the router registering its own seam
samplers with the graph at configure time, the way it already installs its sinks — is a
**named door, not built here**, and is recorded as a residual on
[#1503](https://github.com/avatarsd-llc/libtracer/issues/1503). A future amendment that opens
it adds `<seam-class>` values (`router`, `labels`, `link`) and changes nothing above.

### D.5 The VALUE resolves BELOW the READ gate — the gating is INVERTED against `:identity`

A recognised seam's value MUST be gated on `READ` against the addressed vertex's effective
ACL. A denied caller receives `ERROR{tr::access::denied}` (`0x0050`).

This is the **opposite** of RFC-0011 §C.2's exemption, deliberately. `:identity` is pre-auth
because a public key is precisely what an unauthenticated peer must obtain in order to
authenticate at all; a node's **memory census is not first-contact material**, and nothing
deadlocks by gating it. The narrow, named pre-auth exemption therefore continues to apply to
`:identity` **alone**, and `:stats` MUST NOT be added to it.

**A denied caller's `PERMISSION_DENIED` discloses that the seam exists. That is intended.**
The seam namespace is published spec (this section's table), identical on every node — the
same disclosure `settings`, `children` and `schema` already make — so the answer reveals
nothing a reader of this document does not have. What it must not do is reveal *authorization
state through a code split on one spelling*, and §D.2 is what prevents that: unrecognised
spellings are caller-independent, so the only caller-dependent answer is on a name whose
existence was never secret.

### D.6 Readable; never writable; never awaitable

- **Never writable.** `:stats` has no write door. A write of any `:stats` spelling answers
  `ERROR{tr::schema::not_found}` (`0x0031`), caller-independently — the ENOTTY of a facet
  with no write surface, exactly as `:identity`'s write already answers.
- **Never awaitable, and this mints nothing.** Counters are updated off the failure path and
  **do not bump the vertex's write sequence**; making them real writes would put a publish on
  the hot path, which the `core/STYLE.md` §Introspection counting doctrine forbids. Nothing
  could ever fire a wait on this surface. No new status and no new guard are needed: **every
  field-tailed AWAIT already answers `ERROR{tr::schema::not_found}` unconditionally** — the
  [#585](https://github.com/avatarsd-llc/libtracer/issues/585) rule this RFC states in §C
  ("`await` on a single field is deliberately unsupported"), which holds for every selector
  including ones that read and write fine. This amendment **cites** that rule; it does not
  restate or narrow it.
- **Threshold-crossing notification stays a named door**, not built, exactly as ruled.
- **Per-counter fields stay a named door**, for MCU clients that cannot parse a block.

### Compatibility

Additive, and it occupies previously-erroring space only: before this amendment every
`:stats` spelling answered `SCHEMA_NOT_FOUND` on both doors, and after it every spelling
except the three recognised seams still does. No frame shape, type code, grammar rule or
error identity changes; both status codes it uses are long in the
[RFC-0002](0002-protocol-error-model.md) registry. An implementation that does not serve the
census keeps answering `SCHEMA_NOT_FOUND` and remains conformant for every other surface; a
monitor MUST therefore treat `SCHEMA_NOT_FOUND` on a seam as "this node does not publish that
seam", never as an error.

### Files this amendment edits (on acceptance)

- [reference/05](../../reference/05-protocol-tlvs.md) — the field-namespace set (twice), the
  new §`0x0B` census-record subsection, and a failure-mode entry beside the `:identity` one.
- [reference/22](../../reference/22-backpressure-and-sizing.md) §6 Recipe A — the deferred
  wire arm, completed with the ruled spelling (its gate note is removed).
- Reference core: the `:stats` read arm in `graph_t`'s field-read door.

### Conformance

`tests/conformance/vectors/v1/field/field-stats-block` — the graph-door block, byte-exact,
with the other three answers named against the byte-identical vectors already held
(`acl/denied-caller-undeclared-app-field` for the denied caller;
`settings/removed-knob` for an unrecognised spelling and for the AWAIT refusal). Bound
behaviourally by `core/tests/stats_field_test.cpp` (shape, node scoping, the gate inversion
with `:identity` as its control, every unrecognised spelling at both an admitted and a denied
caller, the read-only half) and by `core/tests/op_resolve_test.cpp` —
`test_await_field_selector_is_enotty`, which now drives a `:stats` selector beside its
field-less control.
