<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0035 — `:subscribers[N]` as one surface: list, create, suspend, resume and clear any subscription

<!-- status: proposed -->

| Field | Value |
| ---- | ---- |
| **RFC** | 0035 |
| **Title** | `:subscribers[N]` as one surface: list, create, suspend, resume and clear any subscription |
| **Status** | **proposed** (2026-10-10). Awaiting the maintainer's rulings on §12. |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-10-10 |
| **Comment window** | Waived by default while the project is solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"). Invoke it explicitly if outside input is wanted. `docs/implementations.md` lists no registered implementation, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** It assigns a reserved `delivery_policy` bit, adds an arm to the `:subscribers[N]` write, changes which right that write demands, changes what a `:subscribers[]` read answers for a cleared slot, and states a reaping MUST. A conforming peer can observe each of them. |
| **Tracking issue** | [#2019](https://github.com/avatarsd-llc/libtracer/issues/2019) (wire spelling for a suspended subscriber slot). The core half is [#1533](https://github.com/avatarsd-llc/libtracer/issues/1533) (suspend a subscription in place), landed in [#2045](https://github.com/avatarsd-llc/libtracer/pull/2045). |
| **Target spec version** | v1 itself. `docs/spec/v1.md` still reads "(DRAFT)". |
| **Amends** | [RFC-0022](0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.A (one reserved bit); [RFC-0030](0030-host-api-walks-the-graph-reply-is-a-remote-write.md) §10.4 ("bits 9–15 stay reserved") and §9.7 items 2 and 3; [RFC-0009](0009-vertex-removal-and-subscriber-eviction.md) §D.1 (the `[N]` arms and their gate) and §D.2 (what a read shows for a cleared slot); [RFC-0005](0005-subtree-subscriptions.md) §A (a suspended edge is not delivered to). |
| **Relies on, unchanged** | [RFC-0034](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0034-subscription-selector-fields.md) §2.3 (who owns a selector ref's suspend bit), [RFC-0021](0021-wire-subscriber-target-frame-of-reference.md) §E (the gate is the writer's), [RFC-0014](0014-creator-endpoint-connection-lifecycle-and-link-liveness.md) §4 (the standing-binding hold), [RFC-0017](0017-element-addressing-value-plane-index.md) §B (`[n]` is structural). |

> **Numbering note.** RFC-0034 is the subscription selector, open in
> [#2068](https://github.com/avatarsd-llc/libtracer/pull/2068); this document takes the next
> number. Numbering gaps and why they are not reused are recorded in the
> [ADR and RFC index](../../adr-rfc-index.md).

---

## 1. Summary

A peer that holds the right access can manage any vertex's subscriptions through ordinary
element reads and writes of `:subscribers`. It can list them, create one, suspend it, resume it
and clear it. Most of that surface already exists. This RFC adds the missing pieces and makes
the whole set coherent:

- **`delivery_policy` bit 9, `suspended`**, settable at append time and through
  `:subscribers[N]`. A write that changes only this bit is a **toggle**: no re-admission, no
  re-mint, no latch and no peer traffic.
- **Read-back shows the live state**, and the `:subscribers[]` read keeps each record at its
  slot's position, so the list can be used to address `[N]`.
- **One gate rule for the `[N]` write.** A caller manages its own subscriptions with
  `SUBSCRIBE`. Managing someone else's needs `WRITE_ACL`.
- **A normative reaping rule.** A suspended slot goes only by the triggers that remove any
  slot, and no reaping may count its silence.

The hot path does not change: #2045 already keeps a suspended edge out of the fan-out.

## 2. What exists, and the gaps

| Operation | Spelling | Gate today | Defined in | Gap this RFC fills |
| ---- | ---- | ---- | ---- | ---- |
| list | `read :subscribers[]` | `READ` | reference/05 §`0x04`, CONTEXT §Array-whole read | The list is **compacted**: cleared and record-less slots are skipped, so entry *i* is not slot *i*. A reader cannot get from the list to `[N]` (§5.2). The state is not shown (§5.1). |
| read one | `read :subscribers[N]` | `READ` | RFC-0009 §D, reference/02 | The state is not shown (§5.1). |
| create | `write :subscribers[]` + `SUBSCRIBER` | `SUBSCRIBE` (producer), fan-in on the target | RFC-0009, RFC-0021, RFC-0030 §9.7 (answers `(index, generation)`, duplicate is idempotent) | No create-suspended (§4.3). The duplicate check compares the whole policy word, so a re-append with a different bit 9 would install a second edge (§6.2). |
| suspend / resume | none | — | host API only: `graph_t::set_suspended` (#2045) | The whole wire spelling (§3, §4). |
| clear | `write :subscribers[N]` + empty `STATUS` | `WRITE` | RFC-0009 §D.1; RFC-0030 §9.7 item 3 (generation guard) | `WRITE` is the value-writing right. Holding it lets a peer clear a subscription it did not make, while a consumer with only `READ` and `SUBSCRIBE` cannot clear its own (§7). |
| replace (retarget) | `write :subscribers[N]` + `SUBSCRIBER` | `WRITE`, then `SUBSCRIBE` | RFC-0009 §D.1 | Same gate question (§7). No generation guard (§6.1). |
| remote form of all of these | `FWD{READ \| WRITE}` with a `FIELD` level | as above, under the request's subject | RFC-0004 §C, RFC-0029, RFC-0030 | None. Every operation above reaches a remote vertex with no new frame. |

RFC-0010 adds nothing for `:subscribers`. It names the field in the closed protocol set and
leaves its handlers alone. RFC-0025 adds the class bits and the "carried verbatim, reads back
unchanged" rule this RFC builds on. RFC-0005 is about who receives a write, and §4.2 restates
it for a suspended edge.

## 3. The spelling: `delivery_policy` bit 9

**Normative; amends RFC-0022 §3.A and RFC-0030 §10.4.** `delivery_policy` **bit 9** (`0x0200`)
is assigned: **`suspended`**. Bits **10–15** stay reserved, with the existing rule unchanged: a
sender MUST write them as 0, and a receiver MUST ignore them and carry them verbatim.

| bits | field | values |
| ---: | --- | --- |
| 0–1 | `reliability` | unchanged |
| 2–4 | `priority` | unchanged |
| 5 | `durability_request` | unchanged |
| 6–7 | `delivery_class` | unchanged |
| 8 | `one_shot` | RFC-0030 §10.4 |
| **9** | **`suspended`** | `1` = the edge is held in its slot and receives nothing |
| 10–15 | reserved | MUST be written `0`, MUST be ignored on read |

The bit costs no wire byte. A sender that predates it wrote `0` there, so it is a
delivering-edge sender by construction, which is how RFC-0025 §4.1 assigned bits 6–7. The core
keeps the state in the slot (`subscriber_t::suspended`, in existing padding), so the hot path
is the same whichever spelling is chosen. §12 Q1 compares the alternative, a named `SETTINGS`
pair.

`suspended` is the one bit of the word that is **mutable state** rather than a property fixed
at admission. §4 and §5 are the rules that keep it honest: a write of it is a toggle, not a
re-admission, and a read of it shows the live state, whoever set it last.

## 4. What suspended means, and how a slot gets there

### 4.1 The state

**Normative.** A slot whose edge is suspended:

1. MUST receive no delivery. That covers its own vertex's writes, bubbled descendant writes
   (RFC-0005 §A) and the durability latch (§4.3).
2. MUST keep its slot index, its target and binding, its admission decision, and the link it
   delivers over. Resuming it MUST NOT re-run any admission step.
3. MUST still be listed by `:subscribers[]` and answered by `:subscribers[N]`, with bit 9 set
   (§5).
4. A `one_shot` edge (RFC-0030 §10.4) that is suspended does not fire. It fires on its first
   delivery after a resume. This gives "an await armed but held" with no extra state.

### 4.2 Who is a subscriber (RFC-0005 §A)

**Normative; amends RFC-0005 §A.** "The subscribers of W" in §A means the **delivering** edges
of W. A suspended edge is not delivered to, and it does not count as a subscriber for any rule
that observes subscriber presence. Two consequences follow:

- A lazy source (CONTEXT §Lazy / on-demand source) whose only edges are suspended sees no
  subscriber and may stop producing.
- The reference implementation's RFC-0005 bookkeeping (`own_subs`, `has_subscribers`, the
  ancestors' listener count) already counts delivering edges only, since #2045. A vertex whose
  edges are all suspended skips the fan-out exactly as one with no edges does.

### 4.3 Create suspended

**Normative.** An append whose record sets bit 9 MUST be admitted exactly as any other append:
the same gate, the same mint and the same identity answer (RFC-0030 §9.7). The new slot is
suspended from the start. Its durability latch, if `durability_request` is set, MUST NOT fire
at admission, because a suspended edge receives nothing. What a later resume does with
`durability_request` is §4.5.

This is what lets a controller stage a subscription before switching to it. The append pays the
admission once, and every later switch is a toggle.

### 4.4 The `[N]` write gains a toggle arm

**Normative; amends RFC-0009 §D.1.** After the shape checks that run ahead of every gate today
(addressed whole, `[*]` refused), an indexed write to `:subscribers[N]` resolves in this order:

| # | Payload | Effect |
| --- | --- | --- |
| 1 | empty `STATUS` | **clear** slot N (unchanged) |
| 2 | a `SUBSCRIBER` byte-identical to the **rendering** of slot N (§5.1) with bit 9 set to *x*, for either value of *x* | **set slot N's state to *x***. If it already holds *x*, nothing changes and the write succeeds. |
| 3 | any other `SUBSCRIBER` with a `target_path` | **replace** slot N's edge through the append door (unchanged) |
| 4 | anything else | `tr::schema::type_mismatch`, slot untouched (unchanged) |

Arm 2 is the toggle. It MUST NOT run the `SUBSCRIBE` admission, re-mint the binding, fire a
durability latch, replay a value, emit a subscription-observer `ADDED` or `REMOVED` event, or
send any frame to the subscriber. It changes one bit of slot state and nothing else. That bit is
also what a host `set_suspended` call changes (#2045), so both doors leave the same slot.

The toggle is **idempotent**. A retry after a lost reply carries the same record, and that
record is arm 2 with *x* equal to the state it already set. So the retry changes nothing and
cannot fall through to arm 3. The issue proposed treating an equal record as a replace, as
today. §12 Q3 asks to rule this.

**Why it compares bytes against the rendering.** A writer reads `[N]`, flips bit 9 and writes
the record back, so the comparison is a `memcmp` over a record of tens of bytes, with no parse
of the stored record. Every core renders the same bytes (§5.1, pinned by a vector in §10), so a
record built by one core toggles a slot on another. A writer that re-encodes the record some
other way gets arm 3, a replace, which is what any changed record gets today.

**A routed slot keeps its binding.** A slot admitted through the routed append door delivers
over its return route (RFC-0021 §4.D). A toggle of it never reaches the field door's parse, so
the slot is not rebound to a local target, which arm 3 would do.

### 4.5 Resume replays nothing

**Normative.** A resume MUST NOT replay the latched value. The edge delivers from the next
propagated value on. `durability_request` stays a property of admission, as #2045 implemented
it for the in-process consumer. §12 Q6 asks whether an opt-in replay is wanted.

## 5. Read-back

### 5.1 `:subscribers[N]` renders the live state

**Normative; amends RFC-0009 §D.** Let *S* be the record slot N was admitted with, carried
verbatim as today. A read of `:subscribers[N]` MUST answer *S* **rendered** under the slot's live
state *s*:

1. If *S* carries a `delivery_policy` pair, the answer is *S* with bit 9 of that word set to
   *s*. The length is unchanged.
2. Otherwise, if *s* = 0, the answer is *S* unchanged.
3. Otherwise (*s* = 1 and no word), the answer is *S* with `NAME "delivery_policy" VALUE <u16
   LE 0x0200>` appended as the last pair of its first `SETTINGS` child. When *S* has no
   `SETTINGS` child, the answer has `SETTINGS{ that pair }` appended as its last child. Every
   enclosing length grows to match.

Every other bit and byte is as last written, so the RFC-0025 verbatim-carry rule still holds.
The live state is the slot's, whoever set it: a wire toggle, a host `set_suspended`, a selector
switch (RFC-0034) or the append that created it.

**The stored record is not rewritten on a toggle.** The bit lives in the slot, and only a read
renders it. So a toggle allocates nothing, and the read pays, on its own reply source, for the
one copy of a record whose stored bit differs from the live state. §12 Q4 compares this with
storing the toggle's record.

### 5.2 `:subscribers[]` keeps slot positions

**Normative; amends RFC-0009 §D.2.** The array-whole read MUST answer one element per slot
position from 0 to the highest occupied slot. Child *i* is the rendering of slot *i* (§5.1). A
position that has no record to answer MUST be an **empty `STATUS`** (`09 00 00 00`), the same
sentinel that clears a slot. That covers a cleared slot, and an in-process callback edge, which
carries no record. Trailing empty positions are omitted.

Today the read skips those positions, so after any clear the *i*-th child is not slot *i*. That
breaks two things:

- RFC-0017 §B's rule that `[n]` selects the *n*-th child.
- The universal surface itself: a reader that lists and then writes `[N]` would suspend or
  clear the wrong subscription.

The cost is 4 bytes per hole on a control-plane read. §12 Q5 compares this with tagging each
element with its index.

## 6. Identity

### 6.1 The generation guard covers every `[N]` write

**Normative; amends RFC-0030 §9.7 item 3.** The optional generation that a `FIELD` level may
carry after `index` guards **every** arm of the `[N]` write: clear, toggle and replace. A write
whose generation does not match slot N's entry MUST answer `tr::path::not_found` and change
nothing. A read-modify-write toggle therefore cannot land on a subscription that took the slot
after the read. Until [#1932](https://github.com/avatarsd-llc/libtracer/issues/1932) gives
slots a generation, the RFC-0034 §2.2 and #2045 caveat applies: a stale `N` names whoever holds
the slot now.

### 6.2 A duplicate append ignores bit 9 and leaves the state alone

**Normative; amends RFC-0030 §9.7 item 2.** The duplicate check compares `delivery_policy`
words **with bit 9 masked**, an absent word reading as `0x0000`. A duplicate MUST NOT install a
second edge. It answers the existing entry's identity and MUST leave that entry's suspended
state as it is.

Without the mask, a consumer re-subscribing with bit 9 clear beside its own suspended edge
would get a second edge to the same target, and the target would receive every write twice
once the first edge is resumed. Leaving the state alone keeps the duplicate check from becoming
a resume that bypasses §7's rule for someone else's edge. The appender learns the identity and
can read `[N]` and toggle it, if §7 lets it.

## 7. Access control

Being able to manage any subscription through ordinary writes is a permissions question before
it is anything else. **Normative; amends RFC-0009 §D.1's gate.**

### 7.1 Who owns an edge

A slot's **owner** is the subject that the `SUBSCRIBE` gate admitted it under. That is the
writer of the append, as RFC-0021 §E has it ("the gate is the writer's"), and not necessarily
the target. A replace makes the replacer the owner. An edge admitted by a local host call is
owned by the local caller. Two subjects are compared as the node's ACL evaluation compares them.
The reference resolves both caller contexts through the same subject lookup at check time. An
edge whose stored context no longer resolves has no remote owner, so only the local caller and
`WRITE_ACL` holders can manage it.

### 7.2 The gate per operation

| Operation | Caller owns the slot | Caller does not |
| ---- | ---- | ---- |
| `read :subscribers[]`, `read :subscribers[N]` | `READ` (unchanged) | `READ` (unchanged) |
| `write :subscribers[]` (create, suspended or not) | `SUBSCRIBE` (unchanged) | `SUBSCRIBE` (unchanged) |
| `[N]` clear | **`SUBSCRIBE`** | **`WRITE_ACL`** |
| `[N]` toggle (suspend, resume) | **`SUBSCRIBE`** | **`WRITE_ACL`** |
| `[N]` replace | **`SUBSCRIBE`** | **`WRITE_ACL`**, and `SUBSCRIBE` for the new edge |

All rights are checked on the producer's `:acl`, the vertex that holds `:subscribers`. The local
(empty) caller passes every gate, as it does today. A node built without ACL enforcement admits
every caller to every operation, ownership included, which is also unchanged.

**Why `SUBSCRIBE` for one's own edge.** A caller with `SUBSCRIBE` can already clear its edge and
append it again, by link departure if nothing else. A toggle and a clear give it no power it
lacks, and they let a consumer that holds `READ` and `SUBSCRIBE` but not `WRITE` remove its own
subscription. Today it cannot.

**Why `WRITE_ACL` for someone else's.** Suspending, clearing or retargeting another party's
subscription decides what that party receives. `WRITE` is the value-writing right, and holding
it should not carry that decision. `WRITE_ACL` is the existing `admin` right (reference/05
§`0x0A`). Its holder can already reshape who may do what at the vertex, so managing the edges
there is the same level of trust. §12 Q2 compares this with a new right bit.

**Order and what a refusal tells.** The shape checks stay ahead of every gate, as today. A
caller that holds neither `SUBSCRIBE` nor `WRITE_ACL` is refused `tr::access::denied`
before slot N is looked up. A caller with `SUBSCRIBE` alone then learns whether slot N exists
and whether it owns it. The same caller learns that from a `READ` of the list, and a node that
wants to hide it withholds `READ`.

### 7.3 What stays as it is

- **Listing discloses targets under `READ`.** Each record names its target, so a `READ` holder
  sees where the vertex delivers. This is today's rule, and the RFC keeps it: hiding the list is
  a matter of withholding `READ` on the vertex.
- **A suspension by someone else is not a ban.** The owner keeps `SUBSCRIBE`, so it can resume
  its edge or append a fresh one. A node that wants a party to stop receiving revokes that
  party's `SUBSCRIBE` and clears the edge. A remote edge is also cleared when its link departs
  (§9), so a suspension never outlives the session that made the edge.
- **The subscription observer.** A toggle is not an admission, so the host observer seam
  reports no `ADDED` or `REMOVED` for it (§4.4). An implementation MAY give its host an audit
  hook for toggles. Nothing about that is on the wire.

## 8. A selector that owns a ref's bit (RFC-0034)

RFC-0034 §2.3 already settles the conflict, and this RFC relies on it unchanged. A selector acts
on what it last set and does not watch the slot. A toggle from anyone else holds until the
selector's next switch reaches that ref, and the selector's `options` read shows the live state,
because it re-reads it. The RFC-0034 §2.1 no-double-delivery bound holds only over refs whose bit
the selector owns, which RFC-0034 already states. A remote toggle of a listed ref passes §7's
gate like any other: the selector runs as the local caller and owns nothing on the wire.

So the rule is **last writer wins**. There is no per-slot lock and no "owned by selector" mark,
so a toggle never has to ask whether some selector lists the slot. §12 Q7 compares the
alternatives.

## 9. Remote subscribers and reaping

### 9.1 No peer traffic

**Normative.** The state lives in the producer's slot. Suspending or resuming MUST send nothing
to the subscriber or to any hop. The subscriber is not told. It can read `[N]`, which shows the
state (§5.1).

### 9.2 A suspended routed edge keeps its link hold

**Normative.** A suspended edge that delivers over a link MUST keep its standing-binding hold on
that link (RFC-0014 §4). Releasing the last standing hold closes the link and returns it to
dormant (RFC-0014 §4.1). That close is a link departure, and a departure evicts the edge (§9.3),
so suspending would turn into a slow unsubscribe and a resume could find nothing to resume. #2045 already keeps the hold. §12 Q8 asks
whether a suspended edge should instead let its link close.

### 9.3 The reaping rule

**Normative.** A suspended slot is removed only by the triggers that remove any slot:

1. an explicit clear: the `[N]` empty `STATUS`, or the host's `unsubscribe`;
2. a replace of slot N (RFC-0009 §D.1);
3. retirement of the producer vertex (RFC-0009 §D.3);
4. departure of the link the edge was admitted over or delivers over (RFC-0009 §D.5,
   `evict_link_edges`).

A suspended edge receives no delivery, so the two triggers that need one never fire on it while
it is suspended: route-refusal eviction (RFC-0024, `evict_route_edges`) and `one_shot` removal
(RFC-0030 §10.4). A suspended edge whose route has gone stale is therefore found when it is
resumed and its first delivery is refused, or when its link departs.

**No implementation may remove a slot, or count toward removing it, because it has delivered
nothing**, however long it has been quiet. That holds at the producer, at any hop and at the
subscriber's node. libtracer has no timers and no idle reaping to begin with. The rule binds
other implementations, and any application-side liveness policy built on the protocol, so that
none of them may treat a suspended edge's silence as a dead edge.

## 10. Conformance vectors

| Vector | Change |
| ---- | ---- |
| `subscriber/policy-suspended` (new) | `SUBSCRIBER{ PATH{ NAME "client" }, SETTINGS{ NAME "delivery_policy" VALUE u16=0x0200 } }`: `suspended = 1`, every other field 0. 44 bytes: `044028000600070006636C69656E740B40190002000F0064656C69766572795F706F6C696379010002000002`. The same bytes are also the §5.1 rule-3 **rendering** of `SUBSCRIBER{ PATH{ NAME "client" } }` when suspended, so the vector pins the read-back shape across cores. |
| `subscriber/policy-reserved-bits` (repaired in place, same bytes) | `0xFFC1` is unchanged. Its description and its three gates narrow from "bits 8–15 reserved" to **bits 10–15 reserved**: the same word now also decodes `one_shot = 1` (bit 8, which RFC-0030 §10.4 assigned without narrowing this vector) and `suspended = 1`. This is the RFC-0025 Amendment 3 clause 7 repair. The description's hex line, which predates RFC-0018's packed paths and no longer matches `input.bin`, is refreshed in the same change. |

The toggle, the gates, the array positions and the reaping rule are behaviour, not codec
bytes. Core host tests pin them in the reference implementation (§13 ticket 1).

## 11. Rust and TypeScript accessors

**Informative.** Both bindings are codecs. They carry the bit and render the record, and
neither honours a delivery.

- **Rust** (`bindings/rust/src/structured.rs`, `DeliveryPolicy`):
  - `ONE_SHOT: u16 = 0x0100` and `one_shot()`, if RFC-0030's tickets have not added them first.
  - `SUSPENDED: u16 = 0x0200`, `suspended()` and `with_suspended(bool) -> Self`.
  - `reserved()` narrows to bits 10–15 (`bits >> 10`).
  - `subscriber_rendered(record, suspended) -> Tlv` implements §5.1, so a caller builds the
    toggle record with it rather than by hand.
- **TypeScript** (`bindings/typescript/packages/client/src/tlv.ts`):
  - `DELIVERY_ONE_SHOT = 0x0100` (same proviso) and `DELIVERY_SUSPENDED = 0x0200`.
  - `isSuspended(policy)` and `withSuspended(policy, on)`.
  - `renderSubscriber(record, suspended)` implements §5.1.
  - On the client, `setSuspended(producer, n, on)` reads `:subscribers[N]`, renders it and
    writes it back: one `READ` and one `WRITE`. It carries the generation once #1932 lands.

The C++ core gains the same constants on `delivery_policy_t`.

## 12. Open questions for the maintainer

Each question has a recommendation. Perf is judged on the write hot path, the toggle and the
control-plane read, across NARROW (MCU, a fixed RX pool), MID and WIDE (host, thousands of
edges).

1. **Spelling.** Bit 9 (recommended) costs no byte and, after the vector repair, no new
   structure. A named `SETTINGS` pair `NAME "suspended" VALUE u8` costs about 15 bytes on every
   suspended record and needs no repair. The hot path is identical either way.
2. **The gate for someone else's edge.** `WRITE_ACL` (recommended) needs no new codepoint, and
   the ALLOW-only profile evaluates it as it does today. A new right bit `0x100`
   (`MANAGE_SUBSCRIBERS`, in the u32 mask's reserved range) would allow an orchestrator that may
   manage edges but not ACLs, at the cost of one more mask bit for every core. The status quo,
   plain `WRITE`, lets a value writer decide others' delivery.
3. **An equal record.** A state-set (recommended, §4.4) makes a retried toggle a no-op. The
   issue's "equal still replaces" makes it a re-admission, with a re-mint, observer events and a
   possible latch.
4. **Where the state is read from.** Rendering on read (recommended, §5.1): the toggle allocates
   nothing, and a read copies one record when the stored bit is stale. Storing the toggle's
   record instead would mean one draw per toggle, or pinning an RX segment per toggled edge on
   NARROW.
5. **List positions.** Empty-`STATUS` holes (recommended, §5.2) cost 4 bytes per hole on a read
   and keep the element type. Tagging each element with its index would change the element type
   for every reader. Keeping the compacted list leaves list-then-`[N]` broken.
6. **Replay on resume.** No replay (recommended): a resume stays a flat ~48 ns flip on every
   class. An opt-in replay would make a resume a delivery, with re-entrancy into callbacks and a
   latch read, for a value the subscriber can read itself.
7. **A selector-owned ref.** Last writer wins (recommended, §8): no check on the toggle path. A
   per-slot "owned" mark that refuses remote toggles costs a byte the slot may not have spare,
   and a reconciling selector would need a watch, which RFC-0034 rejected.
8. **The link hold while suspended.** Keep it (recommended, §9.2): a resume costs no redial, and
   the edge survives. Releasing it would save a socket and its power on NARROW, but it turns a
   suspend into an unsubscribe through link departure.

## 13. Implementation (follows acceptance)

1. **Core:**
   - the `[N]` toggle arm, the gate table of §7.2, the generation guard on every arm, the
     rendered read and the positional list;
   - the duplicate mask of §6.2 once #1932's duplicate check lands;
   - host tests for each rule, including a remote (routed) slot toggled over the wire.
   The toggle drives `vertex_t::set_edge_suspended`, which #2045 already provides for routed
   slots.
2. **Bindings:** §11, and the two vectors of §10 in all three gates.
3. **Annexes and glossary:**
   - reference/05 §`0x04`: the bit table (bit 8 too, if RFC-0030's row has not landed), the
     rendering rule and the positional read;
   - reference/02 §"The payload-discriminating `:subscribers[N]` write": the toggle row, the
     gate table, the intent table (suspend, resume) and a pitfall for the routed-slot rebind;
   - CONTEXT.md: widen "`:subscribers[N]` is the unsubscribe" to the whole surface, and add
     "Suspended subscription".

## 14. Perf, RAM and the NARROW–WIDE spectrum

The figures are from #2045's bench run (bench host, CPUs 2–6, best of rounds), which this RFC
does not change. The wire toggle adds only the frame and the `memcmp` in front of the same flip.

| | Cost | Source |
| ---- | ---- | ---- |
| a write to a vertex whose edges are **all** suspended | nothing: the fan-out is skipped, as with no edges | §4.2, #2045 |
| a write while some edges deliver | **one skipped published entry per suspended edge** (a load and a bit test; the entry is 56 B on LP64, 32 B on ILP32). Canonical in-process fan-out was within the A/A band of main: fan 1024 7.40 → 7.25 µs. | #2045 |
| a toggle (host) | ~46–52 ns at N = 1 to 1,024, flat, allocating nothing. Unsubscribe plus re-subscribe costs 187 / 212 / 349 / 5,244 ns at N = 1 / 8 / 32 / 1,024. | #2045 |
| a toggle (wire) | the inbound frame, a `memcmp` over the record (tens of bytes), then the same flip. No allocation, no frame out. | §4.4 |
| `[N]` read | unchanged, plus one record copy from the reply's source when the stored bit is stale | §5.1 |
| `[]` read | plus 4 bytes per hole | §5.2 |
| RAM per edge | none: the state is in `subscriber_t` padding (80 B / 44 B unchanged) | #2045 |

So "a suspended edge costs a write nothing" holds when all of a vertex's edges are suspended.
While others deliver, each suspended edge costs one skipped entry. That was #2045's trade for a
toggle that allocates nothing. Compacting the array on every toggle would remove the skip, at
the price of a republish (one allocation) per toggle. That is implementation, not wire, so this
RFC leaves it to #2045's ruling.

- **NARROW:** no RAM, no allocation on a toggle, and the ownership check is a subject compare on
  a cold path. The rendered read's copy comes from the reply source, so the receiver pays.
- **MID / WIDE:** a switch among thousands of staged edges stays O(1) per edge. The positional
  list adds at most 4 bytes per hole to a read that already carries tens of bytes per edge.

## 15. Compatibility

- **Bit 9.** A sender that predates it wrote 0, so its edges deliver as before. A receiver that
  predates it ignores the bit and delivers to an edge its peer meant to stage suspended. Such a
  receiver is a conforming v1-draft node until this RFC lands. No backward compatibility is owed
  (RFC-0028 ruling, restated as RFC-0029 ruling 10).
- **BREAKING, the gate (§7.2).** A peer that clears or replaces a slot it does not own, holding
  `WRITE` but not `WRITE_ACL`, is refused `tr::access::denied` where it used to succeed.
  In the other direction, an owner with `SUBSCRIBE` and no `WRITE` gains the clear. Local callers
  are unaffected.
- **BREAKING, the list (§5.2).** A `:subscribers[]` reader sees empty-`STATUS` elements where
  cleared slots are. A reader that assumed every child is a `SUBSCRIBER` must skip them. The
  in-tree readers are the two binding codecs and the core's own tests.
- **Vectors.** One new vector and one repaired in place, same bytes (§10).

## 16. Alternatives considered

- **A separate field `:subscribers[N].suspended`.** Rejected. `:subscribers` is addressed whole
  (reference/02 §"A multi-step selector under `:subscribers`"), and a member surface would undo
  the rule that stops a mistyped tail from clearing a slot.
- **A new `FWD` op or TLV for suspend.** Rejected. The surface is ordinary field reads and
  writes, the read/write/await API has no verbs to spare (ADR-0006), and the bit costs nothing.
- **Bit 9 as an admission property, so changing it means a replace.** Rejected. That is today's
  cost of switching (re-admission, re-mint, observer events), which #1533 exists to avoid.
- **Rewriting the stored record on a toggle.** This is §12 Q4's alternative: one draw or one
  pinned RX segment per toggle, to save a copy on a rare read.
- **Reaping suspended edges after a quiet period.** Rejected. It needs a clock, which libtracer
  does not read (CLAUDE.md §Design rules), and it would make suspension a delayed unsubscribe.

## Discussion

Drafted 2026-10-10 from #2019's scope section and the maintainer's framing of the same day:
`:subscribers[N]` is the surface for universal subscription manipulation, and a peer with the
right access can list, create, suspend, resume and clear any vertex's subscriptions through
ordinary element reads and writes. No comments recorded yet.
