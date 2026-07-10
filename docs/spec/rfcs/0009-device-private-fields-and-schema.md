<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0009 — Device-private field catalogs and `:schema` discovery (opening the property plane)

| Field | Value |
| ---- | ---- |
| **RFC** | 0009 |
| **Title** | Device-private field catalogs and `:schema` discovery (opening the property plane) |
| **Status** | **draft (skeleton)** — §C's field inventory is blocked on the strawberry-fw io_layer port ([#365](https://github.com/avatarsd-llc/libtracer/issues/365)) |
| **Author(s)** | AvatarSD (maintainer), drafted by agent session |
| **Created** | 2026-07-10 |
| **Comment window closes** | opens when the skeleton is filled; ≥ 14 days after (maintainer may waive per the RFC-0007 precedent) |
| **Tracking issue** | [#378](https://github.com/avatarsd-llc/libtracer/issues/378) |
| **Target spec version** | v1 (draft refinement — additive; the wire format is still DRAFT, pinned per release tag) |

## Summary

The vertex's `:` control plane is doctrinally **two-kinded** — protocol-defined
**standard** fields (`:subscribers`, `:acl`, `:settings`, `:children`) and
**device-private** fields (a device's own control ops on the same vertex identity,
the driver-private-ioctl analogue; CONTEXT.md §Field-write) — but only the first
kind is specified. This RFC opens the property plane in two additive steps:

1. **Device-private fields become normative**: addressing, resolution order,
   ACL gating, and error semantics for a `:field` outside the standard set —
   served by the device observing its own field operations (no runtime
   registration API).
2. **`:schema`** becomes a new synthesized, read-only **standard** field that
   enumerates the vertex's **field catalog** (standard + private: name, value
   shape, rights), so a generic orchestrator/UI can drive an unknown device
   in-band — the `ENOTTY`-probing loop replaced by one read.

Driven by the strawberry-fw io_layer port ([#365](https://github.com/avatarsd-llc/libtracer/issues/365)
gap 3): io_layer endpoints carry per-endpoint properties that the four standard
fields cannot express, and today anything else is `tr::schema::not_found` with no
in-band discovery.

## Motivation

- **The doctrine already exists; the spec surface does not.** CONTEXT.md
  §Field-write ratifies "two kinds, like ioctls" and "the protocol owns the
  *addressing*; the device owns its *catalog*", and `SCHEMA_NOT_FOUND` is the
  specified answer for an unsupported field. What is missing is the normative
  half: how a private field is addressed and gated, and how its existence is
  discovered without out-of-band documentation.
- **A real port hit the gap.** The strawberry-fw io_layer (the first external
  adopter, ADR-0044/45/46) models per-endpoint properties (calibration, units,
  hardware config — final inventory pending, §C) that are control-plane data
  ("bare attributed data" per §Field promotion), not subscribable values. With
  the plane closed, the port's only options are abusing `:settings`, promoting
  every property to a `/` child vertex (paying a vertex where no subscription
  point is wanted — the exact anti-pattern §Field promotion warns against), or
  leaving the protocol.
- **Generic orchestration needs discovery.** The network-formation model
  (reference 13) has an ephemeral orchestrator driving devices it has never
  seen. Standard fields are uniform by definition; private fields are only
  usable cross-device if their catalog is readable in-band.

## Proposed change

> Wire-precise layouts below are **TBD** where marked; they firm up when §C's
> inventory lands. Everything here is additive to protocol v1.

### A. Device-private fields (normative addressing & semantics)

- **Addressing**: a private field is any `:name` (with the existing `[]` /
  `.member` forms) on a vertex, outside the reserved standard set. Standard
  names (`subscribers`, `acl`, `settings`, `children`, `schema`) are
  **reserved**: a device MUST NOT serve a private field under a standard name,
  and future protocol versions may reserve additional names (TBD: a reserved
  prefix — e.g. all-lowercase protocol names vs. a `x-`/vendor convention — vs.
  a "current list only" stance).
- **Resolution order**: standard-field resolution first; otherwise the
  operation is offered to the vertex's own logic (the handler seam observing
  its own field ops — the same seam that serves `:children[]` synthesis today);
  otherwise `tr::schema::not_found`.
- **No registration API**: the runtime carries **no** per-field descriptor
  storage and **no** field-registration surface (2026-07-08 design grill,
  standard-fields-only descriptor table). A device's catalog is its own code +
  its `:schema` answer. The runtime stays field-agnostic.
- **ACL gating**: a private-field read is gated by `READ`, a write by `WRITE`
  — the vertex-granularity rights, exactly like the data plane (TBD §C: whether
  any io_layer field needs admin-grade gating; if so the answer is
  READ_ACL/WRITE_ACL-style **standard** semantics, never per-field ACLs).
- **No per-field notification**: unchanged §Field promotion doctrine — a
  private field is bare attributed data; a datum consumers must observe
  independently is promoted to a `/` child vertex by the producer.

### B. The `:schema` standard field (catalog discovery)

- **Read-only, synthesized** (like `:children[]` synthesis): `read(v:schema)`
  returns a structured (`PL=1`) TLV enumerating the vertex's field catalog.
  Writes to `:schema` are `tr::access::permission_denied` (TBD: or
  `tr::schema::not_found` for symmetry with absent fields).
- **Entry shape (TBD, byte-precise on acceptance)** — per field, at minimum:
  - the field **NAME** (one NAME TLV, `0x02`);
  - a **shape hint** (opaque scalar vs. structured; array-ness and stride are
    L4 schema properties per ADR-0008 — the hint's exact vocabulary is the main
    open design question, see §Open questions);
  - **rights** (readable / writable, as the vertex's ACL applies to it);
  - a **standard/private** marker.
- **Optionality**: `:schema` is optional like every field — a minimal endpoint
  (the 9-byte-input char-device) omits it (`tr::schema::not_found`), and a
  device MAY list only its standard fields.
- **Type code**: reuse of an existing structured type vs. a new core type code
  is TBD (candidate: entries as SETTINGS-shaped (`0x0B`) children vs. a new
  `SCHEMA` code; note `0x0E SPEC` is the *creation*-spec type and stays
  unrelated).

### C. The io_layer field inventory (BLOCKED — strawberry input)

> **This section is the skeleton's hole.** Awaiting from the strawberry-fw
> session ([#365](https://github.com/avatarsd-llc/libtracer/issues/365)):
>
> 1. the concrete field list io_layer needs per endpoint (names + semantics);
> 2. each field's value shape (scalar / structured / array; size bounds);
> 3. read/write/persistence expectations (boot-config vs. live-tunable);
> 4. which (if any) need stronger-than-WRITE gating;
> 5. which are genuinely per-endpoint vs. per-device (a per-device datum may
>    belong on an ancestor vertex instead).
>
> This inventory decides §B's shape-hint vocabulary and whether `:settings`
> absorbs any of them as new standard members instead.

### D. Files the accepted RFC will edit

- `docs/spec/v1.md` — field addressing (§paths), the reserved standard-name
  set, `:schema` normative shape.
- `docs/reference/02-graph-model.md` (control fields) and
  `docs/reference/05-protocol-tlvs.md` (the `:schema` reply layout).
- `CONTEXT.md` — §Field-write gains the `:schema` sentence; a new §Field
  catalog term.
- `core/` — resolver: private-field dispatch to the handler seam; `:schema`
  synthesis; conformance vectors (below).

## Compatibility

- **Additive**: no existing frame changes meaning; devices without private
  fields or `:schema` answer `tr::schema::not_found` exactly as today. No v2.
- **Conformance vectors**: new vectors for (a) a `:schema` read reply, (b) a
  private-field read/write roundtrip, (c) `tr::schema::not_found` for an
  unknown field and for absent `:schema`.
- **Migration**: none required; adopting devices add `:schema` when they add
  private fields (RECOMMENDED, not MUST — TBD in comment window).

## Alternatives considered

- **A runtime field-registration API** (device registers descriptors, runtime
  dispatches): rejected 2026-07-08 (design grill) — it adds per-field runtime
  state and a second catalog of truth; the device already owns its catalog in
  code, and the standard-fields-only descriptor table stays closed.
- **Properties as `/` child vertices**: rejected as the *general* answer —
  control facet ⇒ `:`, distinct identity ⇒ `/` (CONTEXT.md §Field-write);
  promotion stays the opt-in for the observable subset (§Field promotion).
- **Everything into `:settings`**: rejected — `:settings` is the protocol-owned
  QoS record with fixed cross-device meaning; device-private data inside it
  destroys its uniformity (and its atomic multi-field-write shape).
- **Out-of-band schema documents**: rejected — the ephemeral-orchestrator model
  (reference 13) requires in-band discovery; out-of-band docs recreate the
  driver-header problem `ioctl` catalogs have.

## Open questions (for the comment window)

1. **Shape-hint vocabulary** in a `:schema` entry: how much typing? (none /
   opaque-vs-structured bit / a small scalar-kind enum). More typing helps
   generic UIs; every bit of it is schema the protocol must then own forever.
2. **Reserved-name policy** for future standard fields (reserved prefix vs.
   list-at-version).
3. **Catalog versioning**: does `:schema` need a generation marker, or is
   "read it again" sufficient (catalogs are per-device-firmware, effectively
   static per boot)?
4. Whether any §C field motivates per-field rights beyond the vertex ACL
   (current stance: no).

## Discussion

Tracked in [#378](https://github.com/avatarsd-llc/libtracer/issues/378). The
skeleton merges as **draft**; the comment window opens once §C is filled with
the strawberry-fw inventory and the TBDs above are proposed concretely.
