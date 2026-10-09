<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0034 — The subscription selector's two fields: `active` and `options`

<!-- status: proposed -->

| Field | Value |
| ---- | ---- |
| **RFC** | 0034 |
| **Title** | The subscription selector's two fields: `active` and `options` |
| **Status** | **proposed** (2026-10-09). It needs the maintainer's approval. The comment window is waived by default while the project is solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"); invoke it explicitly if outside input is wanted. The maintainer ruled the placement and the field spelling on 2026-10-08 in [#2024](https://github.com/avatarsd-llc/libtracer/issues/2024): the selector is an adapter beside the core, `options` and `active` are ordinary fields on its own vertex with no new type codes, selecting is an ordinary field write, and the value format of `options` needs a short amendment. This is that amendment. |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-10-09 |
| **Instrument** | **Amendment.** Below `settings.app.` the bytes are owner-defined ([RFC-0010](0010-owner-app-fields-and-schema.md) §A.1), so nothing here changes the core. What this fixes is a value format a peer can observe on any node that offers the selector type, so that a remote controller written against one node reads and drives another. It mints no type code, no field outside `settings.app.`, no error and no `:stats` noun. |
| **Tracking issue** | [#2024](https://github.com/avatarsd-llc/libtracer/issues/2024) (selector: switch among named options of subscription refs); it builds on [#1533](https://github.com/avatarsd-llc/libtracer/issues/1533) (suspend a subscription in place) |
| **Target spec version** | v1 itself (`docs/spec/v1.md` still reads "(DRAFT)"). |
| **Amends** | Nothing normative is edited. It adds one type's field formats beside [RFC-0010](0010-owner-app-fields-and-schema.md) §A, which it relies on unchanged. |

## 1. Summary

A **subscription selector** is a vertex that switches its owner's existing subscriptions among
named **options**, each option a set of **refs** to subscriptions. It suspends and resumes those
subscriptions in place on their producers ([#1533](https://github.com/avatarsd-llc/libtracer/issues/1533)); it never creates or deletes one. It exposes
two application fields on its own vertex, and this RFC fixes their values.

## 2. The fields

| Field | Access | Retains |
| ---- | ---- | ---- |
| `:settings.app.active` | `rw` | nothing: a read answers the live selection |
| `:settings.app.options` | `ro` | nothing: a read answers the live list |

### 2.1 `active`

- A write of `NAME` (`0x02`) whose body is an option's name MUST make that option active.
- A write of an empty `STATUS` (`0x09`, `length = 0`) MUST select no option.
- A write naming no option MUST answer `tr::path::not_found` and change nothing.
- Any other value, an empty `NAME` included, MUST answer `tr::schema::type_mismatch`.
- A switch that a producer's memory refuses MUST answer `tr::flow::backpressure`. The written
  option is then active, every ref outside it is suspended, and some of its own refs are still
  suspended. Writing it again retries them. A node MUST NOT deliver one write twice to one target
  during a switch; a gap is permitted.
- A read answers `NAME <active option>`, or an empty `STATUS` when none is active.

Writing the option that is already active is a retry and is otherwise a no-op.

### 2.2 `options`

A read answers one `SETTINGS` (`0x0B`, `PL=1`). Options are listed in the order they were
created, and each option's refs in the order they were added:

```
SETTINGS {
  NAME <option>  SETTINGS {
    NAME "ref"  SETTINGS {
      NAME "producer"  PATH  <the producer's canonical key>
      NAME "slot"      VALUE <u32 LE: the ref's :subscribers[N] slot>
      NAME "state"     NAME  "live" | "suspended" | "inert"
    }
    ...                                  ; one NAME "ref" pair per ref
  }
  ...                                    ; one pair per option
}
```

- A ref is the address of the subscription it names: `<producer>:subscribers[N]`.
- `inert` marks a ref whose subscription is gone. It MUST stay listed, and a switch MUST skip it
  without failing. Nothing removes it automatically.
- A reader MUST ignore a member pair it does not know. A later amendment is expected to add
  `NAME "generation" VALUE <u32 LE>` to each ref once subscriptions carry a generation
  ([#1932](https://github.com/avatarsd-llc/libtracer/issues/1932)).
- `options` has no write surface. A write answers `tr::schema::not_found`, including the owner's.
  The owner lists refs through its host API (§4).

## 3. Compatibility

No type code, field namespace or error is added. A node without the selector type never serves
these fields. No conformance vector changes; the reference implementation's host tests pin the
shapes (`core/tests/subscription_selector_test.cpp`).

## 4. Out of scope, and why

- **A remote write of `options`.** A peer cannot name a subscription by `(index, generation)`
  until #1932 gives subscriptions a generation, and cannot suspend one remotely until #2019 (the
  `:subscribers[N]` suspend spelling) lands. Until then the owner lists refs locally.
- **Persistence.** The library keeps nothing across a restart. The owner stores the options and
  the active option and replays both at boot.
- **Which subscriptions a ref may name.** Any subscription the owner holds a handle to. The rule
  "no target twice within one option" is the owner's to keep; a selector does not see targets.

## 5. Alternatives considered

- **Protocol fields `:options` and `:active`.** Rejected by the 2026-10-08 ruling: the selector
  is an adapter, and the protocol mints its own flat field names forever while applications mint
  below `.app.` (05 §`0x0B`).
- **A new structured type for a ref.** Rejected: the ruling allows no new type codes, and the
  `NAME`-keyed `SETTINGS` pairs already carry a record with optional members.
- **Selecting by index.** Rejected: names survive reordering and read the same everywhere.
