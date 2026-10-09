<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0032 — Delete `COMPACT` and the per-link handle tables: every stream rides the chain, and no hop holds state for it

<!-- status: proposed -->

| Field | Value |
| ---- | ---- |
| **RFC** | 0032 |
| **Title** | Delete `COMPACT` and the per-link handle tables: every stream rides the chain, and no hop holds state for it |
| **Status** | **proposed** (2026-10-09), **awaiting maintainer approval**. The direction was **ruled** on 2026-10-07 in [#1938](https://github.com/avatarsd-llc/libtracer/issues/1938) (one walk, the connection vertex is the door), §"Addendum: stages 5 and 6", questions 5–8, "all rec". This document turns that ruling into normative text, and §15 lists the choices the ruling left open, each with a recommendation. |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-10-09 |
| **Comment window** | Waived by default while the project is solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"). Invoke it explicitly if outside input is wanted. At drafting, `docs/implementations.md` still lists no registered implementation, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** It retires three core type codes (`0x11`–`0x13`), retires a `SUBSCRIBER` key (`delivery_compact`), deletes two normative MUSTs (RFC-0004 §E.1's cross-link clearing and concurrent-refusal rules), withdraws a named exception to a normative definition (RFC-0029 §9.2) and removes three `:stats` nouns. A conforming peer can observe each of these, so GOVERNANCE.md reserves them for an amendment. **No backward compatibility is owed**: the project takes none (the RFC-0028 ruling, restated as RFC-0029 ruling 10). §7 still says exactly what an old peer meets. |
| **Tracking issue** | [#1950](https://github.com/avatarsd-llc/libtracer/issues/1950) (RFC: delete COMPACT and the per-link handle tables); parent spec [#1938](https://github.com/avatarsd-llc/libtracer/issues/1938) (one walk, the connection vertex is the door) |
| **Target spec version** | v1 itself. `docs/spec/v1.md` still reads "(DRAFT)". RFC-0018, RFC-0023, RFC-0024, RFC-0027, RFC-0029, RFC-0030 and RFC-0031 took the same route. |
| **Scope** | Stage 6 of #1938, milestone v0.20.0. The deletion [#1951](https://github.com/avatarsd-llc/libtracer/issues/1951) (delete COMPACT, the handle tables and every remnant) and the documentation sweep [#1952](https://github.com/avatarsd-llc/libtracer/issues/1952) (glossary, reference pages and the perf page describe chain delivery) are blocked on this RFC's approval. Stage 7, [#1953](https://github.com/avatarsd-llc/libtracer/issues/1953) (CAN link-local compression, private to the transport), follows them and is bounded by §5.5. |
| **Evidence** | The chain-versus-COMPACT report of [#1949](https://github.com/avatarsd-llc/libtracer/issues/1949) (bench: chain delivery against COMPACT, measured before COMPACT is deleted), landing as [PR #2037](https://github.com/avatarsd-llc/libtracer/pull/2037) (the `bench_chain_vs_compact` rows). **#2037 is being reworked; every figure in §8 is a marked placeholder until it lands.** The bench reports; it does not veto (ruling 4, §3). |
| **Amends** | [RFC-0004](0004-remote-operation-addressing.md) §E.1 (**removed in full**, with its implementation pins) and §"Resolved during design" (the route-handle bullet); [RFC-0029](0029-one-path-primitive.md) §3 ruling 6 (**withdrawn**), §5.3 (the "kept, unchanged" sentence), §9 (its heading and §9.2, **withdrawn**), the note under §11, §12.4 (the "§E.1 untouched" clause) and the 2026-10-02 erratum's `on_stale_label` row; [RFC-0010](0010-owner-app-fields-and-schema.md) Amendment 2 (three nouns leave the net-plane census, §6.3). |
| **Supersedes, on acceptance** | [ADR-0062](../../adr/0062-resolve-once-label-bindings-hold-resolutions-not-names.md) (resolve-once label bindings) in full; [ADR-0035](../../adr/0035-implementing-rfc-0004-remote-operation-addressing.md) slice 4 (the route-handle mechanism) in part; [ADR-0038](../../adr/0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) in part (its label-compacted plane). |
| **Confirms, not amends** | [ADR-0022](../../adr/0022-transport-framing-modes-elided-full-tlv-advertise.md) (framing modes are chosen by the adapter) and [ADR-0030](../../adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md) (the CAN transport's in-transport map): both are transport-private, which is exactly where §5.5 puts compression. [RFC-0025](0025-stream-class-values.md) §4.1.3 (Amendment 4, batching is user-orchestrated). [RFC-0030](0030-host-api-walks-the-graph-reply-is-a-remote-write.md) §3 ("COMPACT and the per-link handle tables are deleted in stage 6") and §8.5 (an empty `src` means no reply, at every hop). |

> **Numbering note.** Numbering gaps and why they are not reused are recorded in the
> [ADR and RFC index](../../adr-rfc-index.md#numbering-gaps).

---

## 1. Summary

RFC-0004 §E.1 gave streams a second address. A producer `ADVERTISE`s a per-link `u16` label for a
subscriber's return route, every hop learns `label → (downstream link, out-label)`, and later samples
ride as `COMPACT{label, payload}` with no route at all. A stale label draws `HANDLE_NACK` and a
re-advertise. RFC-0029 §9.2 kept this as the **single named exception** to the stateless forwarder:
the one place a hop holds state whose loss stops an answer.

Under the 2026-10-07 model (one walk, the connection vertex is the door, the router keeps no remote
state) that exception is the last thing a hop remembers about a flow. This RFC deletes it:

1. **RFC-0004 §E.1 is removed.** `ADVERTISE` (`0x11`), `COMPACT` (`0x12`) and `HANDLE_NACK`
   (`0x13`) are retired and not reassigned. `SUBSCRIBER.qos_settings.delivery_compact` is a retired
   key: ignored, never refused. The per-link handle tables, their clearing rules and their
   observers go with them.
2. **Every stream delivery rides the chain.** A delivery is a `FWD{WRITE}` addressed by the
   subscription edge's chain of PAIR and NAME elements (RFC-0029 §7), with an empty `src` that no hop
   grows (RFC-0030 §8.5). It is the same frame as any other write, and no hop holds anything for it.
3. **BATCH is optional and the application's decision.** An application that wants to amortize
   framing composes one BATCH value (`0x80`, RFC-0025 §4.1.3) and writes it. The library adds no
   counter, window, timer or flush.
4. **A forwarding hop holds no hard state, without exception.** RFC-0029 §9.1's definition stands
   unqualified.
5. **Compression survives only inside a transport, on one link.** A transport MAY compress what
   it carries between the two ends of one link. The state is private to that transport, bounded by
   the link's own identifier space, and expanded back to the full frame before the router sees it.
   CAN's mapping onto native CAN IDs is stage 7 (#1953).

The cost is bytes per sample on multi-hop streams. The maintainer accepted it on 2026-10-07: "don't
ask every person in the relay to remember every part". §8 reports it from #1949's bench.

## 2. Motivation

### 2.1 The one hard state left in a hop

RFC-0029 §9.1 defines a stateless hop as one that holds nothing whose loss, refusal or absence
changes an answer. §9.2 then names the one thing that fails the definition. A `COMPACT` frame
carries no route, so a hop's ingress binding is hard state: lose it and the flow stops until the
producer re-advertises. RFC-0029 admitted it because it was "recoverable by construction, never
wrong". That is true of the frames. It is not true of what the recovery costs to specify and to
build:

- **Two MUSTs exist only for it.** RFC-0004 §E.1's implementation pins require a node clearing a
  link to drop every binding *on any link* whose downstream half crossed it (cross-link clearing),
  and to refuse any binding resolved against the link's pre-clear state, with the refusal and the
  drop never interleaving (concurrent refusal). Both were found the hard way, as dead flows. Neither
  has any meaning for a frame that carries its own address.
- **A second authorization statement.** reference/05 has to say separately that a `COMPACT`
  terminus is gated exactly as the `FWD{WRITE}` it stands for, per frame, and that a binding caches
  the address and never the authorization. A chain delivery is a `FWD{WRITE}`, so it needs no such
  sentence: it is gated by the one gate every write meets.
- **A second lookup at every walk site.** Stage 1 of #1938 (one walk, one gate) removes the
  parallel ways the router finds a link. The handle table is one of them. The forward router's
  advertise, compact-forward, compact-deliver and NACK arms each resolve a link from a label rather
  than from the walk.
- **Its own state budget.** The reference core's handle store (`tr::net::route_handle_t`,
  `core/include/libtracer/route_handle.hpp` and `core/src/route_handle.cpp`, about 1,540 lines
  together) keeps per-link ingress and egress tables. They draw from an injected source, count
  exhaustion and refusals, and publish three `:stats` nouns about themselves.

### 2.2 What it bought, and what replaced it

§E.1 was written when a delivery's only address was the full canonical return route: about 60 B of
route on a 4 B sample over three hops (RFC-0004 §E.1's own figure). Against that, a 6 B label was a
large saving. Since then the address has changed. RFC-0029 made the PAIR element the one compact
address: 11 B per hop and nothing held at the hop (RFC-0029 §5.2). RFC-0030 §8.5 made an empty
`src` survive every hop, so a delivery carries no return route at all. RFC-0029 §9.2 measured
what is left: about **10 B** of framing per sample for `COMPACT` against about **30 B** for a
one-element chain delivery. The gap is now a few tens of bytes, not a route, and it shrinks as the
chain is consumed hop by hop.

### 2.3 Why the cost is accepted

The ruling (§3) does not ask the bench to prove the chain is as cheap as `COMPACT`. It asks the
bench to say what the difference is. A relay in which every runner must remember every part of every
message is a relay that stops when one runner forgets. The difference is paid in bytes on the wire,
which the sender controls (BATCH, §5.3) and a link can recover privately (§5.5). Hard state at a
hop is paid in failure modes that no single node controls.

## 3. The rulings this document encodes

Maintainer, 2026-10-07: the #1938 addendum (stages 5 and 6, "all rec") and the refinement recorded
with it. Numbered here so that later sections can cite them:

1. **COMPACT leaves the protocol and the router.** That means RFC-0004 §E.1 (`ADVERTISE`,
   `COMPACT`, `HANDLE_NACK`, `SUBSCRIBER.delivery_compact`) and the per-link handle tables.
   RFC-0029 ruling 6 / §9.2 is withdrawn. (Addendum Q6.)
2. **The universal addressing is the PAIR chain.** BATCH (`0x80`) is optional and the
   application's decision, with no flush machinery and no timer. (Addendum Q5.)
3. **An old peer's `delivery_compact` gets the general unknown-member treatment**, and the change
   carries one Breaking `CHANGELOG` entry. (Addendum Q6.)
4. **The bench reports; it does not veto.** Chain against `COMPACT`, measured before `COMPACT` is
   deleted: 1 and 3 hops; 64 B, 1 KiB and 16 KiB; unbatched and with BATCH at N = 8 and 32; classic
   CAN unbatched; latency, throughput, RSS and bytes on the wire. A pair-only 8-byte short form is a
   follow-up only if the CAN rows show that the 3 B per element matters. (Addendum Q8.)
5. **Compaction survives only as an optional link-local feature private to a transport.** For
   example, CAN maps a stream to native CAN IDs. Its state lives inside that transport, is bounded
   by the bus's ID space, and is shared only by the two ends of one link. The router never sees it:
   the link expands a frame back to the full chain on receive. It is stage 7, after stage 6, and
   needs its own amendment only if the CAN wire mapping is a normative annex. (Refinement; Addendum
   Q7.)
6. **Stage 6 is a full cleanup.** Code, tests, benches (the `bench_compact_delivery` rows), ratchet
   pins, perf-gate lists, the glossary, reference pages and the perf page. Descriptions and
   methodology are rewritten, not just rows dropped. (Addendum, "PLUS".)

## 4. Vocabulary

| Term | Meaning here |
| --- | --- |
| **chain** | A `PATH` of path elements, each a NAME run or a PAIR (RFC-0029 §4.2). A subscription edge holds the chain it learned toward its consumer (RFC-0029 §7). |
| **chain delivery** | A subscriber delivery as a `FWD{WRITE}` whose `dst` is the edge's chain (or its canonical route where no chain is held) and whose `src` is the empty `PATH`. |
| **retired code** | A core type code whose meaning is withdrawn and which is not reassigned, as `0x14` is (RFC-0029 §5.3). A receiver meets it as an unknown code (reference/01 §Handling unknown type codes). |
| **retired key** | A `SETTINGS` member a receiver ignores and never refuses, carried verbatim and read by nothing, as `batch_count` is (RFC-0025 §4.1.3). |
| **link-local compression** | Any encoding a transport applies to frames between the two ends of one link, invisible above that transport (§5.5). |

## 5. The normative change

### 5.1 RFC-0004 §E.1 is removed

**Normative.** RFC-0004 §E.1, including its implementation pins (the `u16` label, `ADVERTISE`,
`COMPACT`, `HANDLE_NACK`, cross-link clearing, concurrent refusal and the `delivery_compact` opt-in),
is removed from the protocol. In particular:

- a node MUST NOT emit a frame of type `0x11`, `0x12` or `0x13`;
- a node MUST NOT emit the `delivery_compact` key in a `SUBSCRIBER`'s `qos_settings`;
- a node holds no binding, label or handle keyed by a flow, a stream or a subscription crossing it.

§6 says how a receiver treats these members when an old peer still sends them.

### 5.2 Every stream delivery rides the chain

**Normative.** A producer delivers to a remote subscriber with one `FWD{WRITE}` per delivery:

- its `dst` is the subscription edge's chain where the edge holds one, and the edge's canonical
  return route otherwise (RFC-0029 §7, as amended by RFC-0030 §8.7);
- its `src` is the empty `PATH`, so no reply is requested and no hop grows it (RFC-0030 §8.5);
- its payload is the written value, as written (RFC-0005).

Each hop relays it by the per-hop algorithm of RFC-0029 §6, consuming its own element, and
authorizes it at the one gate every write meets (RFC-0029 §6.4, RFC-0031 §6). No hop holds anything
for the flow. A lost, refused or stale element falls back to the canonical route and re-learns, as
for any other address (RFC-0029 §6.3, §8). There is no delivery-only frame, no setup exchange and
no per-flow recovery path.

This is what RFC-0004 §D already said ("a delivery *is* a `FWD WRITE`"). Removing §E.1 removes the
one case where it was not literally true.

### 5.3 BATCH is the application's decision

**Normative.** A producer MAY deliver a batch: one BATCH record (`0x80`, reference/05 §User range)
that the application composes from its samples and publishes as one value, per RFC-0025 §4.1.3
(Amendment 4). The batch rides the chain as one `FWD{WRITE}`, like any other value. Whether to
batch, how many samples go in one, and when to publish it are the application's choices. An
implementation MUST NOT accumulate, delay or merge deliveries to form a batch on its own, and holds
no counter, window, timer or flush for one. This restates RFC-0025 Amendment 4 and the standing
no-timers rule. It adds no wire surface.

### 5.4 A forwarding hop holds no hard state, without exception

**Normative.** RFC-0029 §9 is retitled "Statelessness, defined", and §9.2 is withdrawn. §9.1's
definition now stands without exception: a forwarding hop holds no hard state and no per-request
state, ever. The router and the graph hold no per-flow state for a stream either.

A transport's link-local compression state (§5.5) is not router or graph state. It is soft by
construction: its loss changes the bytes on one link, never an answer.

### 5.5 Link-local compression is a transport's private business

**Normative.** A transport MAY compress the frames it carries between the two ends of one link,
provided that every one of these holds:

1. **Private to one link.** The state is held inside that transport instance and shared only by
   the two ends of that one link. Nothing of it is forwarded, and no other link or hop learns it.
2. **Bounded by the link and paid by it.** It is bounded by the link's own identifier space (for
   CAN, the CAN ID space), and its memory is drawn from that link's own source. A peer-provoked cost
   is paid by the receiving link.
3. **Invisible above the transport.** The receiving end expands every compressed frame back to the
   exact frame the sending router handed to its transport, before handing it up. The router never
   sees a compressed frame, a compression identifier or the compression state.
4. **Soft, never wrong.** Loss, exhaustion, refusal or disagreement of the state degrades to
   uncompressed frames. It never delivers to the wrong target and never drops a frame that the
   uncompressed link would have carried.

No core type code, TLV or `SUBSCRIBER` key is assigned for link-local compression. A transport's
private framing is described on that transport's own reference page. The pages v1 §3 incorporates as
normative annexes are reference/01, reference/05 and reference/03 §path syntax, and none of them
describes a transport's private framing. So a transport mapping such as CAN's stage 7
([#1953](https://github.com/avatarsd-llc/libtracer/issues/1953)) needs no amendment of its own unless
it touches one of those pages, for example by claiming a core code.

The CAN transport's existing in-transport `identity ↔ path` map and its private advertise frame
(reference/14 §the in-band advertise frame, ADR-0030) are already of this kind and are untouched.
They are not RFC-0004 §E.1's `ADVERTISE` (`0x11`), which this RFC retires.

## 6. Wire members that become unknown

### 6.1 `0x11`, `0x12` and `0x13` are retired, not reassigned

**Normative.** `0x11` (was `ADVERTISE`), `0x12` (was `COMPACT`) and `0x13` (was `HANDLE_NACK`)
are retired core type codes, like `0x14`. They are **not reassigned in v1** (§15 Q1). A receiver
meets them as unknown core-range codes (reference/01 §Handling unknown type codes):

- it MUST NOT crash, MUST respect `length` and MUST continue parsing;
- as an outer frame, none of the three carries a return path (an `ADVERTISE` route is a forward
  route, not a `src`), so it answers nothing and drops the frame;
- nested inside a structured TLV, the frame is opaque bytes;
- it creates no binding and no other state.

A forwarder MAY pass an unknown code through unmodified (reference/01). A conforming forwarder never
receives one of these addressed through it: none of them carries a `dst`.

### 6.2 `delivery_compact` is a retired key

**Normative.** `NAME "delivery_compact"` in a `SUBSCRIBER`'s `qos_settings` is a **retired key**,
like `batch_count` and `batch_window_ns` (RFC-0025 §4.1.3):

- a receiver MUST admit a `SUBSCRIBER` that carries it, with any value, and MUST NOT refuse it;
- it is read by nothing: the subscription's deliveries are chain deliveries (§5.2) whatever its
  value;
- it is carried verbatim, so a `:subscribers[N]` read returns the record as the subscriber wrote it,
  as for every other member of that record;
- the `SETTINGS` reader stays pair-consuming, so the key's value is never read as the following
  member (the property `subscriber/policy-absent` already pins).

This is the general unknown-member treatment (ruling 3). The retired line stays in the reference/05
layout so that a reader of an old peer's bytes can tell what the member was (§15 Q2).

### 6.3 Three `:stats` nouns leave the net-plane census

**Normative; amends RFC-0010 Amendment 2.** These nouns are drawn from the handle store and leave
with it:

| spelling | noun | source today |
| --- | --- | --- |
| `:stats.labels.table` | `labels_exhausted` | the handle store's label-space exhaustion count |
| `:stats.labels.table` | `refused_bindings` | the handle store's refusals at its per-link bound or on source exhaustion |
| `:stats.link.<child>` | `labels_used` | the handle store's per-link label occupancy |

reference/05's `:stats` table already marks these nouns "deleted with the table by RFC-0029 slice
S3". They are in fact the `COMPACT` handle store's, not the RFC-0027 path-label table's, so this RFC
is what deletes them, whichever of S3 and #1951 lands first. `label_not_found` and `label_resolves`
are the RFC-0027 label plane's and stay S3's. When a seam has no nouns left, a read of it answers
`ERROR{tr::schema::not_found}` (RFC-0010 §D.4.1), which a monitor already reads as "not published
here".

## 7. Old peers

No compatibility is owed (§Instrument). This section says what an old peer meets, so that a mixed
deployment fails in a known way.

### 7.1 What each old behaviour meets

| An old peer… | A node implementing this RFC… | Outcome |
| --- | --- | --- |
| subscribes with `delivery_compact = 1` | admits it, ignores the key and delivers over the chain (§6.2) | **works.** A full-route `FWD{WRITE}` was always the delivery an old consumer accepted, and the only one when it did not opt in. |
| subscribes with `delivery_compact = 0`, or without it | the same | **works**, byte for byte as before |
| sends `ADVERTISE` (an old producer) | drops it as an unknown code and binds nothing (§6.1) | the advertise is lost; see §7.2 |
| sends `COMPACT` | drops it as an unknown code and sends no `HANDLE_NACK` | the sample is lost; see §7.2 |
| sends `HANDLE_NACK` | drops it as an unknown code | nothing: a new node never sends `COMPACT`, so it has nothing to re-advertise |
| forwards between two new nodes | sees no compact flow, because neither end asks for or offers one | **works** |

A new node never sets `delivery_compact` and never advertises, so an old producer serving a new
consumer, or an old consumer served by a new producer, always gets full-route or chain deliveries.

### 7.2 The one mixed path that fails

One case fails silently: an **old consumer** opts in, an **old producer** honours it, and the
deliveries cross **a new node**. The producer advertises, the new node drops the `ADVERTISE`, and the
producer then streams `COMPACT` frames that the new node drops. The producer never hears a
`HANDLE_NACK`, so it never re-advertises, and the flow is dead across that hop.

This RFC adds no shim for it (ruling 10 of RFC-0029, and a shim would be the handle table under
another name). The migration rule (§11) avoids it: upgrade every node on a path together, or have
old consumers stop setting `delivery_compact` before the first new node joins the path.

## 8. Cost: what the chain costs against `COMPACT`

> **Placeholders.** [PR #2037](https://github.com/avatarsd-llc/libtracer/pull/2037) (the #1949
> chain-versus-COMPACT bench) is being reworked. Every value marked **⟦#2037⟧** below is a
> placeholder, filled in from #2037's final report when it lands. Until then the analytic figures
> in §8.2 are the only numbers this RFC relies on.
>
> **The headline excludes the reply leg.** A chain delivery requests no reply: its `src` is empty and
> no hop grows it (RFC-0030 §8.5). Until #1946 (the reply is a remote write; AWAIT and REPLY retire)
> lands that rule in the reference core, a forwarder grows the empty `src` and the terminus answers
> every forwarded write. The first #2037 run therefore timed a reply leg into the chain arm (one
> reply frame per hop) and not into the `COMPACT` arm. A figure that includes that reply leg MUST NOT
> be quoted here as the chain's cost. #2037 reports it in separate columns, and §8.1's headline
> columns are the forward leg only.

### 8.1 Measured (#1949, PR #2037)

Host: the bench host, bench CPUs, best of rounds per cell (the standing rule). `ns` is per delivered
frame over the whole chain; `ns/sample` divides by N. `B/hop` is the forward frame on each hop's
link. CAN columns count classic (8 B) data fields per hop. Forward leg only.

| hops | payload | N | chain ns/sample | `COMPACT` ns/sample | ratio | chain B/hop | `COMPACT` B/hop | chain CAN8/hop | `COMPACT` CAN8/hop |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 64 B | 1 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 1 | 64 B | 8 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 1 | 64 B | 32 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 1 | 1 KiB | 1 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 1 | 1 KiB | 8 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 1 | 1 KiB | 32 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 1 | 16 KiB | 1 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 1 | 16 KiB | 8 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 1 | 16 KiB | 32 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 3 | 64 B | 1 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 3 | 64 B | 8 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 3 | 64 B | 32 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 3 | 1 KiB | 1 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 3 | 1 KiB | 8 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 3 | 1 KiB | 32 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 3 | 16 KiB | 1 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 3 | 16 KiB | 8 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |
| 3 | 16 KiB | 32 | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ | ⟦#2037⟧ |

Also from #2037, each a placeholder:

- **Headline sentence:** chain against `COMPACT`, 64 B unbatched, forward leg only: ⟦#2037⟧ at 1
  hop and ⟦#2037⟧ at 3 hops.
- **Reply leg, reported separately and not part of the headline:** ⟦#2037⟧ frames and ⟦#2037⟧ B per
  delivery, present only until #1946 lands §8.5.
- **Cold first frame** (including `COMPACT`'s `ADVERTISE`): ⟦#2037⟧.
- **RSS and warm-path heap allocations** per arm: ⟦#2037⟧.
- **Classic CAN verdict** (does the 3 B per element matter, ruling 4): ⟦#2037⟧.
- **Report reference** (merge commit of #2037 and the bench host's run identifier): ⟦#2037⟧.

### 8.2 Analytic, from the frame layouts

These hold whatever #2037 measures, because they are byte counts from the layouts:

- **Per sample.** `COMPACT` costs **10 B** of framing over the payload TLV: a 4 B header and a 6 B
  label `VALUE`. A chain `FWD{WRITE}` costs **17 B** of fixed framing (the `FWD` header, the `op`
  `VALUE`, and the `dst` and empty `src` headers) plus **11 B per element** left in `dst`
  (RFC-0029 §5.2). The difference on one link is therefore 7 + 11·E B, where E is H + 1 on the first
  link of an H-hop path and falls by one at every hop, as each hop consumes its own element.
  `COMPACT` stays constant. This agrees with RFC-0029 §9.2's "about 10 B against about 30 B".
- **Above 1 KiB.** The difference is a fixed number of bytes per frame, so its share falls as the
  payload grows. At one hop (E = 2, 29 B) it is about 3% of a 1 KiB sample and 0.2% of a 16 KiB
  sample. On the first link of three hops (E = 4, 51 B) it is about 5% and 0.3%. At 16 KiB, copying
  the payload dominates both arms.
- **With BATCH.** A batch of N samples pays the chain's framing once, so the per-sample difference
  divides by N.
- **State.** `COMPACT` holds one ingress binding per flow per hop, plus egress tables at the
  producer. The chain holds nothing at any hop. The edge's learned chain is held once, at the
  producer's edge, which it already holds for RFC-0029 §7.

### 8.3 Across NARROW, MID and WIDE

- **NARROW** (an MCU on CAN or a constrained WebSocket link). It loses the handle store's code and
  its per-link tables, which today draw from the link's source and refuse on exhaustion. It pays
  the per-sample bytes of §8.2, which matter most on classic CAN for small unbatched samples
  (⟦#2037⟧). BATCH (§5.3) and the transport's link-local compression (§5.5, stage 7) are the two
  remedies. Both are local decisions, and neither puts state in a hop.
- **MID** (a gateway forwarding a few streams). The forward path becomes the ordinary per-hop
  algorithm: one element consumed and one gate per frame, with no label swap and no table lookup.
  Latency and throughput against `COMPACT`: ⟦#2037⟧.
- **WIDE** (a host forwarding many flows). Nothing scales with flows × hops. There is no advertise
  path and no exhaustion counter, and no reconnect rule has to scan bindings on other links.

## 9. Normative pages that change

### 9.1 `docs/spec/v1.md`

§3's reference/05 bullet ends its statelessness clause at "a forwarding hop holds no hard state and
no per-request state". The words "RFC-0004 §E.1's `COMPACT` route handle being the single named
exception" are deleted.

### 9.2 `docs/reference/05-protocol-tlvs.md` (normative annex)

- **§Reserved range (`0x0F` – `0x1F`):** the `0x11`–`0x13` bullet reads "retired (were the route-handle frames
  `ADVERTISE`, `COMPACT` and `HANDLE_NACK`; RFC-0032). Not reassigned." The unassigned list is
  unchanged: `0x16`–`0x1F`.
- **§Route-handle frames** is replaced by a short retired-codes section, in the form of §`0x14`,
  that states §6.1's receiver rule. The frame layouts, the clearing rules, the concurrent-refusal
  rule and the `COMPACT` ACL paragraph are removed.
- **§`0x04` `qos_settings`:** the `delivery_compact` line reads `RETIRED (RFC-0032) — carried
  verbatim, read by nothing`. The `delivery_compact` paragraph is replaced by §6.2's rule.
- **§Producer fan-out to remote subscribers:** the fan-out paragraph and its sequence diagram
  describe chain delivery only. The `ADVERTISE` and `COMPACT` steps and the "auto-promoted `COMPACT`"
  clauses are removed, and so is "(full-route or `COMPACT`)" in the latch paragraph.
- **§Reserved range, the `FWD` bullets:** the unacknowledged-request bullet loses "the same drop a denied `COMPACT` delivery
  takes", and its last sentence names a `SUBSCRIBER` as the standing plane without
  `delivery_compact`.
- **§`0x06`:** in §Invalidation, the statelessness paragraph loses its "single named exception"
  sentence. "An `ADVERTISE` route" is removed from the key-context list of §Enforcement of the PATH
  constraints, and from the §Where it appears and §Note on string form vs PATH-TLV form bullets.
- **§`:stats` net-plane seams:** the three nouns of §6.3 are removed, and the deletion note names
  this RFC for them.

### 9.3 `docs/reference/03-addressing.md`

§path syntax, the normative part, does not change. Outside it, the slice-group key table's "Remote,
compacted" row and the `ADVERTISE`-route mention in §One path form are removed. Both are informative.

### 9.4 Informative pages, records and the glossary (#1952)

#1952 (glossary, reference pages and the perf page describe chain delivery) rewrites every
remaining mention, per ruling 6. That covers `CONTEXT.md` (the "Delivery compaction" entry is
removed, and "Advertise + id-match" stays because it is the CAN transport's own map), reference/00,
02, 04, 07–10, 14 (§The ws/UDP generalization is replaced by §5.5's rule), 18, 21 and 22 (label-space
sizing), the module pages under `docs/modules/`, `docs/methodology.md`, the route-label examples, and
the generated perf page. On the perf page, the chain-delivery rows get a methodology rewritten to
describe them, and the retired `compact` series are explained, not silently dropped.

Accepted records are history and are not edited, apart from the status lines of the records this
RFC amends or supersedes (§Amends, §Supersedes), which are updated at acceptance.

### 9.5 Records this RFC contradicts, said explicitly

- **RFC-0029 §9.2 and ruling 6** called `COMPACT` the admissible exception. This RFC withdraws
  both. The admissibility argument ("recoverable, never wrong") was correct about the frames. It is
  overruled on cost: two clearing MUSTs, a second authorization statement and a second lookup path
  (§2.1), against a byte gap that the chain and BATCH have narrowed (§2.2, §8).
- **ADR-0038** ranks the label-compacted plane as the general topology's lean frame. Under this
  RFC the chain is that frame.
- **ADR-0062** (a binding holds the resolved target) refines a mechanism that no longer exists. Its
  insight, resolve once and then dereference, survives as the PAIR element itself, where the owner
  issues the resolution and no hop caches it.

## 10. Breaking surface

Every row is breaking. No compatibility shim is owed.

| # | Surface | Was | Becomes |
| --- | --- | --- | --- |
| B1 | Type codes `0x11`, `0x12`, `0x13` | `ADVERTISE`, `COMPACT`, `HANDLE_NACK` | retired, not reassigned; dropped as unknown codes (§6.1) |
| B2 | `SUBSCRIBER.qos_settings.delivery_compact` | opt-in to label-compacted delivery | retired key: ignored, carried verbatim (§6.2) |
| B3 | Delivery to an opted-in subscriber | `ADVERTISE` once, then `COMPACT{label, payload}` | chain `FWD{WRITE}` like every other delivery (§5.2) |
| B4 | The stateless-forwarder definition | one named exception (RFC-0029 §9.2) | no exception (§5.4) |
| B5 | `:stats` nouns | `labels_exhausted`, `refused_bindings`, `labels_used` | removed (§6.3) |
| B6 | Reference implementation, C++ | `tr::net::route_handle_t` (`route_handle.hpp`); `fwd_router_t::advertise`, `send_compact`, `on_compact_delivery`, `on_stale_label`, `handles()`, `compact_delivery_fn_t`, `stale_label_fn_t`; `router_planes_t::label_src` (the source the handle store draws from); `type_t::ADVERTISE`, `COMPACT`, `HANDLE_NACK`; the subscriber slot's `delivery_compact` field | removed. `fwd_router_t::clear_link` loses its label-table half; whether the member stays is #1951's call. |
| B7 | Reference implementation, TypeScript client | `CompactFlowError`, raised on an inbound `0x11` or `0x12` | removed; those codes are unknown codes like any other |

**The Breaking note.** #1951 lands it in `core/CHANGELOG.md` under **Breaking**, with matching
entries in `bindings/typescript/CHANGELOG.md` (B7) and, if any Rust surface moves,
`bindings/rust/CHANGELOG.md`:

> **`COMPACT` and the per-link handle tables are deleted (RFC-0032, [#1950](https://github.com/avatarsd-llc/libtracer/issues/1950), [#1951](https://github.com/avatarsd-llc/libtracer/issues/1951)).**
> Every stream delivery is now a `FWD{WRITE}` over the subscription's chain, with an empty `src`,
> and no hop keeps state for it. `ADVERTISE` (`0x11`), `COMPACT` (`0x12`) and `HANDLE_NACK` (`0x13`)
> are retired: a node never sends them and drops them as unknown codes. `delivery_compact` in a
> `SUBSCRIBER` is ignored. `route_handle_t`, `fwd_router_t::advertise`, `send_compact`,
> `on_compact_delivery`, `on_stale_label` and `handles()` are removed, together with the
> `ADVERTISE`, `COMPACT` and `HANDLE_NACK` `type_t` enumerators and the `:stats` nouns
> `labels_exhausted`, `refused_bindings` and `labels_used`. To amortize framing on a high-rate
> stream, compose a BATCH value (`tr::wire::compose_batch`) and write it. A path that mixes old and
> new nodes must not carry an old consumer's `delivery_compact` flow (RFC-0032 §7.2).

## 11. Migration

- **Deployments.** Upgrade every node on a path together. Until then, have old consumers stop setting
  `delivery_compact` before the first new node joins a path that an old producer serves (§7.2).
- **C++ embedders.** Remove calls to `advertise`, `send_compact`, `on_compact_delivery` and
  `on_stale_label`, and the `router_planes_t::label_src` source that sized the handle store. A transport that called
  `clear_link` only to reset label state no longer needs to. To amortize framing, compose BATCH
  values in the application (RFC-0025 §4.1.3).
- **TypeScript client.** Remove handlers for `CompactFlowError`. An inbound `0x11` or `0x12` is now
  an unknown code, handled like any other.
- **Rust binding.** No public surface carries the route-handle frames. The `structured.rs` comment
  that names `delivery_compact` as an example key is reworded.
- **Monitors.** Stop reading the three nouns of §6.3. A seam left empty answers
  `tr::schema::not_found`.
- **Transport authors** who want compression on a constrained link implement it under §5.5, inside
  the transport.

## 12. Slices and conformance

### 12.1 Slices

| Ticket | Contents | Gate |
| --- | --- | --- |
| [#1951](https://github.com/avatarsd-llc/libtracer/issues/1951) (delete COMPACT, the handle tables and every remnant) | §5–§6 in the reference core and bindings: the handle store, the router's advertise, compact and NACK arms, the subscriber slot's opt-in, the `:stats` nouns, the TypeScript `CompactFlowError`; the compact tests; the `bench_compact_delivery` and handle-contention benches, their ratchet pins and perf-gate list entries; the §10 Breaking note. **Merges only after this RFC is approved** (the shared brief), and after #1946 lands RFC-0030 §8.5 (§15 Q5). | full core-ci matrix; symbol ratchet shows the deletion; §12.3 tests |
| [#1952](https://github.com/avatarsd-llc/libtracer/issues/1952) (glossary, reference pages and the perf page describe chain delivery) | §9 in full, including the normative-annex edits of §9.1–§9.2 if #1951 does not carry them, and the perf page rewrite of ruling 6 | docs build (`sphinx-build -n -W`); citation gate |
| [#1953](https://github.com/avatarsd-llc/libtracer/issues/1953) (CAN link-local compression, private to the transport) | stage 7 under §5.5 | its own; an amendment only if it touches a normative annex |

### 12.2 Conformance vectors

- **`subscriber/policy-absent` (bytes unchanged).** Its description is reworded: the
  `SETTINGS` child carries only the **retired** `delivery_compact` key, which a receiver ignores. It
  pins both the absent policy and §6.2's pair-consuming read.
- **`subscriber/compact-key-retired` (new).** A `SUBSCRIBER` whose `qos_settings` carries
  `delivery_compact = 1` and no `delivery_policy`. The C++, Rust and TypeScript suites each assert
  that it decodes, that the policy reads as all-zero, and that no reader surfaces a compaction
  opt-in.
- **`tlv-types/retired-route-handle-codes` (new).** One outer frame each of `0x11`, `0x12` and
  `0x13`, in their old layouts. Each suite asserts that it decodes structurally as an unknown
  core-range code and is skipped by its declared length, and that a following frame in the same
  buffer still parses.

The route-handle frames never had vectors (they were transport-plane control), so no vector is
deleted.

### 12.3 Behaviour tests (at the wire, on in-memory links)

- A node receiving `ADVERTISE`, `COMPACT` or `HANDLE_NACK` emits no frame and creates no state.
- A subscribe carrying `delivery_compact = 1` is admitted. Its deliveries are chain `FWD{WRITE}`s
  with an empty `src` at 1 and 3 hops, and no hop answers them.
- A `:subscribers[N]` read returns the subscriber's record with the retired key as written.
- A link reconnect mid-stream loses no delivery that the chain's fallback (RFC-0029 §6.3) does not
  already recover. No re-advertise exists to wait for.
- `:stats.labels.table` and `:stats.link.<child>` publish none of §6.3's nouns.

## 13. Alternatives considered

- **Keep `COMPACT` as the named exception (status quo).** Rejected by ruling 1. The cost is in §2.1.
  The byte saving is real but small against the chain, and BATCH and link-local compression recover
  it where it matters.
- **Keep `COMPACT` for CAN only.** Rejected. CAN's compression is already transport-private
  (ADR-0030) and never used `0x11`–`0x13`. Stage 7 (§5.5) is where CAN gets more of it, without a
  core code.
- **A pair-only 8-byte short form of the chain** (4 + 8·H, dropping the escape header). Deferred
  by ruling 4: it is a follow-up only if #2037's CAN rows show that the 3 B per element matters
  (⟦#2037⟧, §15 Q4). It would be its own amendment to the PAIR element and is not part of this RFC.
- **Library-side batching** (a flush timer or a per-subscriber accumulator). Rejected. It breaks
  the no-timers rule and the no-library-buffers rule, and RFC-0025 Amendment 4 already ruled
  batching user-orchestrated.
- **A compatibility shim** (a new node answers an old `COMPACT` with `HANDLE_NACK`, or translates
  an old `ADVERTISE` into a chain). Rejected. Either one keeps a per-flow table at the new node, which
  is the state this RFC deletes. No compatibility is owed.
- **Reassign `0x11`–`0x13` now.** Rejected for v1 (§15 Q1). A deployed old peer would read a new
  meaning as a route-handle frame.

## 14. What would falsify this RFC

The bench does not falsify it (ruling 4). These would:

1. **A stream that cannot be carried without per-hop state.** That would be a topology where a chain
   delivery fails in a way the canonical fallback (RFC-0029 §6.3) does not recover, so that a hop has
   to remember a flow to deliver it.
2. **Link-local compression that cannot stay invisible.** That would be a transport whose
   compression cannot be expanded to the exact frame before the router sees it (§5.5 clause 3), so
   that compression would leak into the router after all.
3. **An unacknowledged chain delivery that cannot be had.** That would be RFC-0030 §8.5 failing to
   hold across hops, so that every chain delivery drew a reply. §8's headline assumes it holds.

## 15. Questions for the maintainer, each with a recommendation

1. **Retire `0x11`–`0x13` without reassigning them in v1?** *Recommendation: yes.* This is the
   `0x14` precedent (RFC-0029 §5.3). Reassigning them would let a deployed old peer misread a new
   frame as a route-handle frame. `0x16`–`0x1F` stay the unassigned range.
2. **Keep `delivery_compact` as a `RETIRED` line in the reference/05 layout, rather than deleting
   the line?** *Recommendation: keep it,* as the `batch_count` precedent does. It tells a reader of
   an old peer's bytes what the member was. `subscriber/policy-absent` keeps its bytes.
3. **Count a received retired frame (`0x11`–`0x13`)?** *Recommendation: no, drop it uncounted.*
   reference/01 makes unknown codes the forward-extension path, which deployed receivers ignore.
   Counting them as malformed would flag every future extension as an error. The one failure it
   would help diagnose, the mixed path of §7.2, is avoided by the migration rule.
4. **The pair-only short form for classic CAN.** *Recommendation: decide on #2037's final CAN rows
   (⟦#2037⟧).* File a follow-up amendment only if small unbatched classic-CAN samples are a real
   workload and BATCH or stage 7 does not cover it.
5. **Gate #1951's merge on #1946 landing RFC-0030 §8.5 (no hop grows an empty `src`)?**
   *Recommendation: yes.* Without §8.5 in the reference core, deleting `COMPACT` first would make
   every multi-hop stream sample draw a reply leg until #1946 lands. That is the regression the
   headline of §8 excludes.
6. **Remove the three `:stats` nouns (§6.3) in #1951, whatever S3's order?** *Recommendation:
   yes.* They are the handle store's. reference/05 already marks them deleted, but attributes them
   to the RFC-0027 table.

## 16. Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the comment window is waived by default while
the project is solo-maintained, and it is not invoked here. Maintainer approval is recorded on this
RFC's pull request and in the Status row at acceptance. Sustained objections and their resolution are
recorded in this section as they arrive.
