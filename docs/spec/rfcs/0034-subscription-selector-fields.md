<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0034 — The subscription selector's two fields: `active` and `options`

<!-- status: accepted -->

| Field | Value |
| ---- | ---- |
| **RFC** | 0034 |
| **Title** | The subscription selector's two fields: `active` and `options` |
| **Status** | **accepted** — maintainer ruling 2026-10-10 on the [#2068](https://github.com/avatarsd-llc/libtracer/pull/2068) review: approved as an amendment once renumbered from 0033 (that number went to the Noise link binding), shipped without a ref generation ([#1932](https://github.com/avatarsd-llc/libtracer/issues/1932)) under the documented caveat, with an owner-explicit remove, and with the partial switch on `backpressure` that §2.1 states. Comment window waived by the sole maintainer per [GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window". The maintainer ruled the placement and the field spelling on 2026-10-08 in [#2024](https://github.com/avatarsd-llc/libtracer/issues/2024): the selector is an adapter beside the core, `options` and `active` are ordinary fields on its own vertex with no new type codes, selecting is an ordinary field write, and the value format of `options` needs a short amendment. This is that amendment. |
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
- A switch that a producer's memory refuses MUST answer `tr::flow::backpressure`, and is then
  partly applied: the written option is active, every ref outside it is suspended, and some of
  its own refs are still suspended. Writing it again retries them. This is the one refused write
  that leaves state behind, and it is accepted as such: the write is idempotent, so a writer that
  retries on `backpressure`, as it does for any write, completes it.
- A write that arrives while another switch, `options` read or owner call is running on the same
  selector MAY answer `tr::flow::backpressure` and change nothing; a retry completes it.
- During a switch a node MUST NOT deliver one write twice to one target through two refs it
  lists; a gap is permitted. The bound is over the refs whose suspend bit the selector owns (§2.3).
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
  without failing. Nothing removes it automatically; the owner may remove it (§4), and listing
  the same address again (a later subscribe that reuses the slot) makes it live or suspended
  again, as the graph says.
- A reader MUST ignore a member pair it does not know. A later amendment is expected to add
  `NAME "generation" VALUE <u32 LE>` to each ref once subscriptions carry a generation
  ([#1932](https://github.com/avatarsd-llc/libtracer/issues/1932)).
- `options` has no write surface. A write answers `tr::schema::not_found`, including the owner's.
  The owner lists refs through its host API (§4).
- **The size of a read.** A ref can be listed in every option, so a selector of `R` refs and `O`
  options renders at most `R × O` ref records, each carrying a producer key of up to 1 KiB
  (`kMaxPathBytes`). At the reference implementation's widest documented sizing, 64 refs and 16
  options, that is 1,024 records and about 1.1 MiB encoded. The node stages the levels and then
  copies the result out, so one read holds about twice its encoded size at its peak, on the
  node's own table source; a refused draw answers `tr::flow::backpressure`. A node that cannot
  afford that sizes its selectors smaller; the default (8 refs, 4 options) is at most 32 records.

### 2.3 Who owns the suspend bit

A selector owns the suspended-or-delivering state of every subscription it lists. It does not
watch it: it acts on what it last set, and re-reads it only for `options`, for one ref's state,
and when the owner lists the ref again. So:

- Anything else that resumes a listed subscription (the owner toggling it directly, or a
  subscribe that reuses the slot of one that was removed) leaves it delivering until the owner
  lists it again or a switch's suspend reaches it.
- A peer's (re)subscribe is admitted delivering, like every subscribe, and delivers until the
  owner lists it and switches. A remote spelling for "admit suspended" is #2019's
  (`:subscribers[N]` suspend), not this RFC's.

The no-double-delivery rule of §2.1 holds over the refs a selector lists while it owns their
bit; outside that, two subscriptions to one target deliver twice, as they would with no selector.

## 3. Compatibility

No type code, field namespace or error is added. A node without the selector type never serves
these fields. No conformance vector changes; the reference implementation's host tests pin the
shapes (`core/tests/subscription_selector_test.cpp`).

## 4. Out of scope, and why

The owner's host API lists a ref in an option, removes a ref from every option, and selects; a
remove leaves the subscription in the state it holds.

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
