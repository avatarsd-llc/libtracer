<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0013 — Readable creatable-child-type catalog: the `:children.schema` read

| Field | Value |
| ---- | ---- |
| **RFC** | 0013 |
| **Title** | Readable creatable-child-type catalog: the `:children.schema` read |
| **Status** | **superseded** by [ADR-0059](../../adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md) (2026-07-17, maintainer-ratified grill) — see §Why this RFC is superseded |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-17 |
| **Comment window closes** | — (withdrawn before it closed; never accepted, nothing normative here) |
| **Tracking issue** | [#413](https://github.com/avatarsd-llc/libtracer/issues/413) |
| **Target spec version** | v1 (additive — occupies previously-erroring space only) |

## Why this RFC is superseded

**The problem is real and is being solved; the spelling proposed below is not the
solution.** Read this document for the problem statement (§Motivation is
accurate and still worth reading) and as the record of a design that lost.

This RFC hangs the catalog off a **field**: `read <vertex>:children.schema`. The
2026-07-17 grill ruled that **`:schema` is vertex-only** — exactly one per
vertex, describing *that vertex's own* structure, never its children's, and **no
schema hangs off a field**. That ruling leaves `:children.schema` nowhere to
live: it is not the vertex's schema, and there is no such thing as a field's
schema.

The ruling was not aimed at this RFC. `:children.schema` was **inherited, not
invented here** — [reference/05](../../reference/05-protocol-tlvs.md) §`0x0E`
literally calls the catalog "the `:children` field's `:schema`", and
[ADR-0017](../../adr/0017-in-band-vertex-creation-controller-orchestration.md)'s §Decision
spelled it the same way. This RFC faithfully specified what the docs said. The
docs were wrong, and that error stayed invisible precisely because **the read
half was never implemented** — nothing ever tried to address it, so nothing ever
discovered it had nowhere to live. (The same reason ADR-0017's error survived;
see [ADR-0059](../../adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md)
§Context.)

**Where the catalog actually lives:** on a **creator endpoint vertex**, per
ADR-0059. `write /net/export SPEC{type,name,config}` creates; `read
/net/export:schema` **is** the catalog — a vertex's own schema, so the
vertex-only ruling holds with no field-level schema anywhere. Everything this
RFC wanted survives: the catalog is readable, in-band, device-bounded, with no
new wire verb, type code, or error identity, and a device without a creation
surface still answers `SCHEMA_NOT_FOUND`. Only the *address* changed — from a
field on the parent to a vertex of its own.

**What must still be done, and is not done here:** ADR-0059 deliberately
specifies **no bytes**. The `SPEC` layout, the catalog reply shape, the
`unexport` payload, the gating right, and the write-only value semantics all
need pinning — that is the forthcoming creator-endpoint RFC's job, and it should
lift §B and §C below (the descriptor shape and the config-key
described-vs-opaque split, which the grill did not disturb) rather than start
from a blank page.

**Governance note.** This RFC was never accepted and contributes nothing
normative; superseding it before its comment window closed forecloses no
implementer's comment, because acceptance — not withdrawal — is what the window
gates, and [`docs/implementations.md`](../../implementations.md) registers no
third-party implementations to comment.

---

## Summary

The device's **creatable-child-type catalog** — the set of `type` selectors a
`SPEC` (`0x0E`) write into a vertex's `:children[]` will accept
([ADR-0017](../../adr/0017-in-band-vertex-creation-controller-orchestration.md))
— becomes **readable**, at `read <vertex>:children.schema`. The reply is one
`POINT` whose members are one descriptor per creatable type: the exact `NAME`
selector bytes a `SPEC` may use, plus an optional `SETTINGS` record describing
the type's config keys (universal keys described; kind-private keys stay
opaque, per the `conn_settings_t` / ADR-0043 §5 precedent). The spelling is a
**field-of-field read that already exists grammatically**: reference/05
literally calls the catalog "the `:children` field's `:schema`", and
reference/03's field grammar (`field-chain = field *( "." field )`) parses
`children.schema` today — no new grammar, no new top-level field name, no new
wire verbs, type codes, or error identities. A device with no dynamic-creation
surface keeps `SCHEMA_NOT_FOUND` on read exactly as it has it on write: the
catalog read and the `:children[]` write surface are **one capability, two
faces**, and MUST agree.

## Motivation

1. **The spec already names this surface; no implementation can serve it.**
   [Reference/05 §`0x0E` SPEC](../../reference/05-protocol-tlvs.md) — normative
   by incorporation (spec [v1.md](../v1.md) §3, [ADR-0007](../../adr/0007-normative-wire-format-by-incorporation.md))
   — says: "The device validates `type` against its **catalog** (the
   `:children` field's `:schema`); an unknown type returns
   `ERROR{tr::schema::not_found}`." The validation half is implemented
   (`core/src/graph.cpp` `create_child`: unknown selector ⇒ `SCHEMA_NOT_FOUND`,
   the `ENOTTY` of creation). The *read* half — "the `:children` field's
   `:schema`" — does not exist anywhere: `graph_t::read_schema` serves only the
   **per-vertex** schema (the RFC-0010 §B.2 two-part POINT), no per-field
   catalog read exists, and the field-read dispatch in `graph_t::read` returns
   `SCHEMA_NOT_FOUND` for the `children.schema` chain. The parenthetical is a
   dangling normative reference; this RFC makes it real.
2. **The doctrine promised visibility.** [ADR-0017](../../adr/0017-in-band-vertex-creation-controller-orchestration.md):
   the orchestrator "*selects a type* from the device's **controller-type
   catalog**, which is the creation field's **schema** (a POINT enumerating
   accepted `type`s and each one's config schema)" — pinned in 2026-05, never
   given a read surface. [CONTEXT.md](../../../CONTEXT.md) §In-band vertex
   creation repeats the promise ("what becomes visible is the device's
   controller-type catalog"), and §Module set's avoid-list asserts the catalog
   "is **runtime-queryable**, per-device" — asserted, currently false.
3. **The catalog already exists in-core; the read only exposes it.** The
   reference holds exactly this structure today: `graph_t::child_types_`
   (populated by `register_child_type` — the built-in `stored_value` in the
   `graph_t` ctor, `client`/`listener` in `core/src/transport_vertex.cpp`), and
   modules extend it at runtime (the ADR-0043 quic module extends the transport
   catalog through `register_transport_type` — the open/closed seam the
   `transport_vertex.cpp` comments document). Nothing new is stored; bytes the
   device already consults on every `SPEC` write become servable.
4. **A real consumer flow is blind without it.** Network formation
   ([reference/13](../../reference/13-network-formation.md)) is an orchestrator
   creating controllers and transport connections on devices it did not build
   (`:children[]` `SPEC` writes), then binding them. Today the orchestrator
   must know each device's types **out of band**, or probe by writing — a
   side-effecting, `CREATE`-gated discovery on what should be the read plane. A
   generic constructor UI (select-a-type-from-a-dropdown, the strawberry-fw
   wiring-diagram pattern) has no dropdown source.

## Proposed change

### A. The address: `read <vertex>:children.schema`

The catalog is read at the **`schema` sub-field of the `children` field** —
one `.subfield` step per [reference/03 §Field-path resolution](../../reference/03-addressing.md):

```
/dev/ctrl:children.schema
└───┬───┘ └──────┬──────┘
 vertex     field chain (depth 2 of the allowed 8)
```

Why this spelling and not the alternatives (§Alternatives has the full
engagement):

- **It is grammatically legal today.** Reference/03's one grammar:
  `path = root … [ field-sep field-chain ]`, `field-chain = field *( "." field )`
  — **one** `:` per path, dots thereafter. `:children.schema` parses in every
  conforming parser now (the reference's `field_path_t` already carries it to
  the dispatch, which returns `SCHEMA_NOT_FOUND` — exactly the
  previously-erroring space an additive v1 change may occupy). The
  double-colon spelling `:children:schema` is **not** in the grammar and would
  force a grammar change through every parser and the parse-once `path_t`
  ([ADR-0054](../../adr/0054-path-t-parse-once-constructor.md)) for zero
  expressive gain.
- **It spends no new top-level field name.** Top-level protocol bare names are
  the scarce resource ([RFC-0010](0010-owner-app-fields-and-schema.md) §A.1's
  economy argument); a `:catalog[]` sibling would mint one to say something
  `children` already owns.
- **It makes the existing prose literal.** "The `:children` field's `:schema`"
  (reference/05 §`0x0E`) *is* `​:children.schema` — the RFC changes the docs
  least by changing the address not at all.

Resolution and gating:

- Stage 1 (vertex address) unchanged: nonexistent vertex ⇒
  `ERROR{tr::path::not_found}`.
- The read is a control-surface read gated by the ordinary **READ** right on
  the vertex, like `:schema` — `tr::access::denied` otherwise.
- Only the **bare** two-step chain is a surface: indexed, append, or any
  deeper form (`:children.schema[]`, `:children.schema[0]`,
  `:children.schema.x`) MUST return `ERROR{tr::schema::not_found}`.
- The surface is **read-only**: a write to `:children.schema` MUST return
  `ERROR{tr::schema::not_found}` (no write surface — same identity as writing
  `:schema`; the catalog is device state, mutated only through the device's
  own registration API, the RFC-0010 §A.2 owner-initiated doctrine).
- Remote reads ride the ordinary `FWD{READ}` with the field chain per
  [RFC-0004](0004-remote-operation-addressing.md) — nothing new.

### B. Reply shape — byte-precise

One `POINT` (`0x07`, `opt.PL=1`); each member is one `POINT` (`PL=1`)
describing one creatable type:

```
POINT (0x07, PL=1) {                  ; the catalog — zero or more members
  POINT (PL=1) {                      ; one member per creatable type
    NAME <type>                       ; MUST be first: the SPEC `type` selector,
                                      ; byte-verbatim what a SPEC write may carry
    SETTINGS (PL=1) {                 ; MAY follow: the type's config descriptor
      NAME <key>  SETTINGS (PL=1) {   ; one record per described config key
        NAME "dtype"    NAME  <t>          ; SHOULD — value shape (utf8/u16/u32/bool/…)
        NAME "required" VALUE <bool u8>    ; MAY
        NAME "default"  VALUE <bytes>      ; MAY
        NAME "enum"     POINT (PL=1) {     ; MAY — closed value set, one NAME per value
          NAME <value> …
        }
        NAME "label"    NAME  <human>      ; MAY
        …                                  ; owner-defined extras — opaque
      }
      …
    }
  }
  …
}
```

- The **member wrap mirrors the `:children[]` member listing** (reference/05
  §`0x0E` read-members: `POINT{ POINT{NAME} … }`): a type with no config
  descriptor is byte-shaped exactly like a member entry, and the catalog read
  is visibly the schema dual of the members read. Members are self-delimiting,
  so the optional `SETTINGS` never creates pairing ambiguity.
- The **per-key record reuses the RFC-0010 §B.1 descriptor form**
  (`NAME <name> SETTINGS{…}`) and, like it, only the *structure* is normative:
  the key vocabulary (`dtype`/`required`/`default`/`enum`/`label`) is
  SHOULD-level convention so generic consumers converge; extras are opaque.
- The **empty catalog** is the outer POINT with no members — 4 bytes:

```
07 40 00 00        ← POINT, opt=0x40 (PL=1), length=0
```

- Worked example — a two-type catalog: `stored_value` bare, `client` with a
  one-key descriptor (`kind`, dtype `utf8`); 71 bytes total:

```
07 40 43 00                                        ← catalog POINT, len=67
  07 40 10 00                                      ← member POINT, len=16
    02 00 0C 00 73 74 6F 72 65 64 5F 76 61 6C 75 65  ← NAME "stored_value"
  07 40 2B 00                                      ← member POINT, len=43
    02 00 06 00 63 6C 69 65 6E 74                  ← NAME "client"
    0B 40 1D 00                                    ← config descriptor SETTINGS, len=29
      02 00 04 00 6B 69 6E 64                      ← NAME "kind"
      0B 40 11 00                                  ← key record SETTINGS, len=17
        02 00 05 00 64 74 79 70 65                 ← NAME "dtype"
        02 00 04 00 75 74 66 38                    ← NAME "utf8"
```

### C. Semantics — normative clauses

1. **One capability, two faces.** A device MUST serve
   `read <vertex>:children.schema` on every vertex where it accepts
   `:children[]` `SPEC` writes, and MUST return
   `ERROR{tr::schema::not_found}` on the read wherever it returns it on the
   write (a device without dynamic creation stays `ENOTTY` on both — the
   read and write surfaces of the creation field never disagree).
2. **Completeness at the instant of the read.** Every `type` selector that a
   `SPEC` write into this vertex's `:children[]` would accept **at the moment
   the read is served** MUST appear as a member, carrying the selector bytes
   verbatim. (The reference serves one device-wide table at every
   creation-capable vertex — informative; the clause deliberately binds
   per-vertex so an implementation MAY scope catalogs per creation point.)
3. **Snapshot, extend-only.** The catalog read is a snapshot. Types MAY be
   registered after node start (a module extending the catalog —
   [ADR-0043](../../adr/0043-quic-webtransport-optional-module-msquic.md)'s
   registration seam) and MUST appear on subsequent reads; a node SHOULD NOT
   remove catalog entries within its lifetime (the catalog is extended, never
   modified). Consumers MUST treat the catalog as **descriptive**: the `SPEC`
   write gate remains authoritative, and a write racing a registration may
   still get `SCHEMA_NOT_FOUND` — reading first confers no admission.
4. **Empty ≠ absent.** An empty-member POINT means "this vertex has a creation
   surface and currently zero creatable types" — distinct from
   `SCHEMA_NOT_FOUND` ("no creation surface"), exactly the declared-but-empty
   vs undeclared split RFC-0010 §A.4 draws (`NOT_FOUND` vs
   `SCHEMA_NOT_FOUND`) transposed to the catalog.
5. **Config descriptors are self-description, not a validation schema.** The
   runtime MUST NOT validate a `SPEC`'s `config` against the descriptor; the
   type's factory is the semantic gate (the RFC-0010 lazy-validation split:
   the runtime validates *addressing* — is the type in the catalog — and the
   owner of the vocabulary validates *shape*). A type SHOULD describe its
   universal config keys; it MAY omit or partially describe **kind-private**
   keys (the `conn_settings_t` precedent: `addr`/`kind`/`port`/`role`/
   `keepalive`/`max_frame` are universal and parsed centrally, while quic's
   `cert`/`key` never land in `conn_settings_t` — the kind's factory parses
   them from the raw config TLV, ADR-0043 §5 leanness). An undescribed key is
   *unstated*, not forbidden.
6. **Runtime-extended selectors below the type level.** A transport **kind**
   registered through `register_transport_type` (udp/tcp/ws built-ins; quic
   from its module) is **not** a catalog type: the `SPEC` grammar has one
   selector (`type`), and kinds are config vocabulary *of* the
   `client`/`listener` types. A device MAY advertise the currently-registered
   kind set as the `kind` key's `enum` in those types' descriptors, refreshed
   per read (snapshot semantics, clause 3), and SHOULD omit the `enum` when
   its kind set is open. This keeps the catalog N types, not N×M
   type-kind products, and keeps the module seam open/closed — a new kind
   module changes a descriptor's *bytes*, never the catalog's *shape*.
7. **Interaction with write-creates** ([RFC-0005](0005-subtree-subscriptions.md)):
   unchanged. A plain **data** write still creates stored-value vertices
   `mkdir -p`-style without consulting any catalog; the catalog governs only
   the typed `:children[]` `SPEC` path. (The reference's built-in
   `stored_value` catalog entry is the explicit spelling of the same
   capability — it appears in the catalog read like any other type.)

### D. Reference-implementation seam (informative)

`register_child_type` gains an optional per-type **descriptor** — static bytes
or a provider callback (the [ADR-0044](../../adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)
`on_children` synthesized-listing precedent), so `transport_vertex_t` can
serve a live `kind` enum. The field-read dispatch in `graph_t::read` gains one
branch (`children` + `schema` plain steps) that emits the members from the
`child_types_` snapshot — the same emit-and-wrap pattern as `read_schema` /
`read_children`. No cross-vertex orchestration, no storage change.

### Files this RFC edits (on acceptance)

- `docs/reference/05-protocol-tlvs.md` — §`0x0E` SPEC gains a **"The catalog
  read (`:children.schema`)"** subsection carrying the §B layout; the
  existing "(the `:children` field's `:schema`)" parenthetical becomes a
  cross-reference to it. (Normative via v1.md §3 incorporation — no
  `docs/spec/v1.md` edit needed.)
- `docs/reference/02-graph-model.md` — §Core writable fields table gains the
  `:children.schema` row (structured TLV, read-only, creation-catalog
  self-description); §Schema and field discipline cross-references it beside
  the `:schema` two-part read.
- `docs/reference/03-addressing.md` — §Examples gains
  `/dev/ctrl:children.schema`; §Field-path resolution notes `.schema` as an
  ordinary sub-field step (explicitly **no grammar change**).
- `CONTEXT.md` — §In-band vertex creation / creation field: the catalog gains
  its read spelling, and the term widens from "controller-type catalog" to
  **creatable-child-type catalog** (it was never controllers-only — transport
  connections ([ADR-0027](../../adr/0027-transport-and-connections-are-vertices.md))
  and `stored_value` create through it); §Module set's avoid-note
  ("runtime-queryable, per-device") points at the read.
- `core/` reference implementation + `core/CHANGELOG.md` (§D seam) — follow
  this RFC, not normative.

## Conformance vectors (what proves it)

New vectors under `tests/conformance/vectors/v1/` (additive; no existing
vector changes):

1. **`field/field-children-schema`** — the canonical encoding of a path
   carrying the `children.schema` field chain (the addressing/dispatch
   vector, sibling of `field/field-nested`).
2. **`tlv-types/point-catalog`** — the §B worked example: the 71-byte
   two-member catalog (`stored_value` bare; `client` with the `kind`/`utf8`
   key record), decoded meaning asserted member by member.
3. **`tlv-types/point-catalog-empty`** — the 4-byte empty catalog
   (`07 40 00 00`): creation surface present, zero types.

Host-side tests (local surface, like RFC-0010's):

4. **read⇔write agreement** — a graph with creation: catalog read lists
   exactly the registered types and a `SPEC` naming each succeeds; an
   unlisted type's `SPEC` and the read on a no-creation build both return
   `SCHEMA_NOT_FOUND`.
5. **runtime extension** — `register_child_type` after first read ⇒ the new
   type appears on re-read; a `register_transport_type` kind appears in the
   `client` descriptor's `enum` on re-read (when advertised).
6. **gating and shape** — read without READ ⇒ `tr::access::denied`; write to
   `:children.schema`, and any indexed/deeper form, ⇒ `SCHEMA_NOT_FOUND`;
   nonexistent vertex ⇒ `tr::path::not_found`.

## Compatibility

- **No protocol-v1 break.** The read occupies space that is
  `SCHEMA_NOT_FOUND` in every existing implementation; a device that never
  serves it is exactly a device without dynamic creation, which is already a
  conforming posture (reference/05 §`0x0E`: no `:children[]` capability ⇒
  `SCHEMA_NOT_FOUND`). No new wire verbs, type codes, or error identities; no
  grammar change (the chain parses today).
- **New conformance vectors** as listed above; **no existing vector changes**.
- **Migration:** devices implement the read when they implement dynamic
  creation (clause C.1 couples them); orchestrators SHOULD switch from
  out-of-band type knowledge to reading the catalog, keeping the write gate
  as the authority (clause C.3 — no TOCTOU assumption to unlearn, because
  none was ever offered).

## Alternatives considered

- **`:children:schema` (a second `:`, literal field-of-field).** Rejected —
  outside the reference/03 grammar (`field-sep` appears once; `*` in the
  chain is `"." field`), so it is a grammar change rippling through every
  parser, `path_t` ([ADR-0054](../../adr/0054-path-t-parse-once-constructor.md)),
  and the FIELD frame ([RFC-0004](0004-remote-operation-addressing.md)) for a
  spelling the dot form expresses identically.
- **A new top-level field, `:catalog[]` / `:types[]`.** Rejected — mints a
  scarce top-level protocol bare name (the RFC-0010 §A.1 economy), splits the
  creation story across two field names, and the array forms are meaningless
  (types are named, not slotted — the same reason `:children[N]` falls
  through today).
- **Fold the catalog into the vertex's `:schema` POINT** (a third part beside
  the synthesized protocol part and the RFC-0010 owner part). Rejected — it
  bloats the every-vertex self-description read with a potentially large,
  rarely-needed listing, churns the two-part `:schema` shape RFC-0010 §B.2
  just fixed (and its `point-schema-app` vector), and loses the per-creation-
  point scoping clause C.2 permits. The catalog stays demand-paged behind its
  own address.
- **Discovery by probing** (issue a `SPEC` and interpret `SCHEMA_NOT_FOUND`).
  Rejected — side-effecting, `CREATE`-gated discovery on the read plane's
  job; a successful probe *creates a vertex*. This is the status quo being
  fixed, not a design.
- **Flatten kinds into first-class catalog types** (`client/udp`,
  `client/quic`, …). Rejected — the SPEC grammar has one selector; kinds are
  one type's config vocabulary behind an open/closed module seam
  ([ADR-0043](../../adr/0043-quic-webtransport-optional-module-msquic.md) —
  the quic module extends the kind map, never the creation grammar), and
  flattening makes the catalog a type×kind product that grows with every
  module.
- **A wire operation that edits the catalog remotely.** Rejected —
  [ADR-0017](../../adr/0017-in-band-vertex-creation-controller-orchestration.md)'s
  bound: the device instantiates only its **own** known types (that is what
  keeps in-band creation from being code injection); the catalog is device
  state, mutated only by the device's local registration API — the same
  owner-initiated doctrine as RFC-0010 field declaration.

## Discussion

Genuinely open points, flagged for the comment window
(per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the tracking issue
[#413](https://github.com/avatarsd-llc/libtracer/issues/413) stays open through
2026-07-31; sustained objections and their resolution to be recorded here):

1. **Does `.schema` generalize?** This RFC specs exactly one sub-field schema
   read (`children.schema`) because exactly one consumer need exists. A
   general rule ("every field MAY serve `.schema`" — `:subscribers.schema`,
   `:acl.schema`) is deliberately not proposed; if a second instance appears,
   it should generalize by its own RFC rather than this one speccing surfaces
   nothing serves (the reference/02 `:description`/`:liveness.*` lesson,
   RFC-0010 §Discussion 5).
2. **READ vs CREATE gating.** The read is READ-gated like every control-surface
   read. Gating it on CREATE (hide the catalog from peers who cannot create)
   is coherent but makes the catalog a secret, which it is not — it describes
   the device class, not the deployment; and split gating would diverge from
   `:schema`. Objections welcome.
3. **How much key vocabulary to freeze.** Clause C.5 makes only the
   `NAME <key> SETTINGS{…}` structure normative, vocabulary SHOULD-level —
   mirroring RFC-0010 §B.1 and its open Discussion 4. The constructor-UI and
   reconciler consumers should say whether `required`/`enum` need MUST
   strength before v1 freezes.
4. **Advertising open kind sets.** Clause C.6 lets a device omit the `kind`
   `enum` when its set is open, which means a consumer cannot distinguish
   "kinds unknown" from "no kinds". If that ambiguity bites, a `MAY`-level
   `open` marker inside the key record is the escape hatch — deferred until a
   consumer asks.
5. **Is the empty catalog reachable?** The reference always registers
   `stored_value`, so its catalog is never empty; clause C.4 keeps the empty
   POINT for implementations that scope catalogs per-vertex or register
   nothing. If the window concludes it is dead surface, collapsing empty into
   `SCHEMA_NOT_FOUND` would couple "has types now" to "has a creation
   surface", which clause C.3's snapshot semantics argue against — the RFC
   keeps them distinct.