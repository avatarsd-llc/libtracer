<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0031 — Bus-session anchors are child vertices under their door: one walk and one gate reach a session, send-through is directed

<!-- status: proposed -->

| Field | Value |
| ---- | ---- |
| **RFC** | 0031 |
| **Title** | Bus-session anchors are child vertices under their door: one walk and one gate reach a session, send-through is directed |
| **Status** | **proposed** (2026-10-07). The direction of §§5–6 was **ruled** on 2026-10-07 in [#1938](https://github.com/avatarsd-llc/libtracer/issues/1938) (§"Addendum: stages 5 and 6", "all rec"). This document turns that ruling into normative text, and §14 lists the choices the ruling left open, each with a recommendation. |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-10-07 |
| **Comment window** | Waived by default while the project is solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"). Invoke it explicitly if outside input is wanted. At drafting, `docs/implementations.md` still lists no registered implementation, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** It lifts a normative refusal (a PAIR MUST NOT egress through a shared mount, RFC-0029 §10 and v1 §3), narrows a normative MUST (RFC-0020 §3: "MUST NOT resolve the residual against its local graph"), and changes which vertex a frame addressed below a bus door is authorized at. GOVERNANCE.md reserves each of these for an amendment. **No backward compatibility is owed**: the project takes none (the RFC-0028 ruling, restated as RFC-0029 ruling 10). |
| **Tracking issue** | [#1947](https://github.com/avatarsd-llc/libtracer/issues/1947); parent spec [#1938](https://github.com/avatarsd-llc/libtracer/issues/1938) |
| **Target spec version** | v1 itself. `docs/spec/v1.md` still reads "(DRAFT)". RFC-0018, RFC-0023, RFC-0024, RFC-0027, RFC-0029 and RFC-0030 took the same route. |
| **Scope** | Stage 5 of #1938. The implementation ticket [#1948](https://github.com/avatarsd-llc/libtracer/issues/1948) is blocked on this RFC and on stages 1 and 2 ([#1939](https://github.com/avatarsd-llc/libtracer/issues/1939), [#1940](https://github.com/avatarsd-llc/libtracer/issues/1940)). Sequenced after [RFC-0030](0030-host-api-walks-the-graph-reply-is-a-remote-write.md), because both amend RFC-0029. |
| **Amends** | [RFC-0029](0029-one-path-primitive.md) §4.2 (the shared-mount example), §6 step 3 (the shared-mount arm), §8.1 (the session-anchor guard) and §10 in full, which this RFC replaces, with §13.2 S8; [RFC-0020](0020-bus-name-not-a-routable-next-hop.md) §3 (the local-graph MUST, narrowed). |
| **Confirms, not amends** | [ADR-0044](../../adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md) and its 2026-08-13 amendment (an accepted session MAY be an ordinary vertex; announce-census peers create none); [ADR-0073](../../adr/0073-naming-authority-the-application-mints-one-predicate-gates.md) §2 (`p<slot>` names a session, not a device) and §3 (a bus link's NAME is not a routable next hop); [ADR-0082](../../adr/0082-auth-subject-and-peer-named-are-decoupled-claims-default-stays-false.md) (`peer_named` is an addressing knob and its default stays `false`). |
| **Discharges** | RFC-0030 §9.1's forward reference: "the session is the accepted session's anchor where one exists (RFC-0029 §10, stage 5 of #1938)". |

> **Numbering note.** Numbering gaps and why they are not reused are recorded in the
> [ADR and RFC index](../../adr-rfc-index.md#numbering-gaps).

---

## 1. Summary

A WebSocket or TCP listener built with `peer_named = true` gives each accepted session a **session
anchor**: a vertex with an owner-issued `(index, generation)` pair, retired when the session ends,
so that a route naming a dead session fails the generation check (ADR-0044 §Amendment, #1223).
Today that anchor hangs off a **private root** that no walk can reach. It is keyed by a string that
no path can spell (`:<mount>/<peer>`), the router parses that string back to find the session, and
a frame that names the session by NAME is authorized at a different vertex from a frame that names
it by PAIR. A PAIR cannot egress through it at all: RFC-0029 §10 refuses a PAIR to a shared mount
until this stage lands, so every chain across a bus stays NAMEs.

This RFC makes the anchor an ordinary child of its bus connection vertex (its **door**, RFC-0030 §4):

1. **The anchor is a child of the door,** named by the peer name the link already uses (`p<slot>`,
   ADR-0073 §2). `<door>/<peer>` and the anchor's PAIR reach **the same vertex through the same
   walk**: stage 1's descent to the deepest connection vertex finds it with no new branch.
2. **It is the one gate.** The anchor is the session's connection vertex. A frame through the session
   is authorized once, at the anchor, whose ACL is the door's ACL as the door evaluates it, plus the
   anchor's own `:acl` if one was written (§6).
3. **It lives for its session.** Every new session revives it with a new generation, including a
   returning peer that gets the same name, so a cached pair to a dead session is refused and the path
   object falls back to the name (RFC-0030 §5.2).
4. **Send-through is directed.** A walk that reaches an anchor with a non-empty remainder sends it
   to exactly that session, over the link's directed per-peer endpoint. It never broadcasts.
5. **Announce-census peers stay vertex-less.** On CAN (and any other census bus) the walk gates at
   the bus door and the link's own peer list resolves the remainder. Census peers get no per-peer ACL,
   and the documentation says so.

The private anchor root, the `:<mount>/<peer>` key and its parser are deleted. A FLAT listener
(`peer_named = false`, the default) stays exactly as free as it is today, and every graph saves one
`vertex_t` (§7).

## 2. Motivation

### 2.1 One session, two vertices, two names

A `peer_named` session is visible in two places that do not know about each other:

- **the link's peer list** (`bus_link_t::peer_link`, `bus_link_t::enumerate_peers`), which the NAME
  spelling `<door>/<peer>/…` reaches through the router's mount descent and
  `child_registry_t::resolve_peer` (ADR-0061);
- **the anchor** (`graph_t::register_session_anchor`, `core/src/graph.cpp`), which hangs off the
  private root `graph_t::anchor_root` (`core/include/libtracer/graph.hpp`) and is keyed by the
  unspellable record `:<mount>/<peer>` (`fwd_router_t::session_anchor_id`, `core/src/fwd_router.cpp`).

The graph's design note for the anchor says so on purpose: an anchor "exists to be REFERENCED and
never to be ADDRESSED", so that `find`, every listing and RFC-0020 §3's premise stay unchanged. That
was the right call for #1223 step 2, which needed a generation before it needed an address. Under the
2026-10-07 model (a connection vertex is a door, and there is one walk) it leaves a session as the
one thing on a node that a walk cannot reach.

### 2.2 Two spellings, two gates

The NAME spelling `<door>/<peer>/…` is authorized at the door. The PAIR spelling, on the delivery
arm that already accepts an anchor (the bound-delivery path in `fwd_router_t`, #1223 step 3), is
authorized twice: once at the anchor, which "lives OUTSIDE the path tree … and cannot see the
mount's" ACL, and once more at the mount's connection vertex, looked up by name. RFC-0029 §6.4 asks
for a verdict that is a function of the vertex, the caller and the right alone, reached by one
`allows` site. Two vertices for one session cannot satisfy that by construction.

### 2.3 No send-through, so the chain stops at every bus

RFC-0029 §10 refuses a PAIR that dereferences to a shared mount, because a bus link's `send()`
broadcasts (RFC-0020). It names the fix, an egress **through** the anchor keyed by its slot, and
leaves it to slice S8. Until then the origin's chain toward anything behind a bus is "PAIRs up to the
bus, NAMEs across it", which is RFC-0029 §2.2's second essential divergence. RFC-0030 §9.1 also
leans on the anchor: the pending record's egress names "the next-hop session … the accepted
session's anchor where one exists".

### 2.4 A second key namespace

The private root needs its own key rule (one record whose bytes contain `:` and `/`), its own parse
(`graph_t::session_anchor_route`, which splits the record back into mount and peer with `rfind`), its
own lookup (`find_session_anchor`), its own census (`session_anchor_slots`) and a per-delivery
name lookup of the mount (`child_registry_t::entry_by_name`). Each exists only because the anchor has
no parent in the tree. A parent deletes all of them.

## 3. The rulings this document encodes

**From #1938 (2026-10-07), the stage-5 addendum, ruled "all rec":**

1. An accepted session's anchor becomes a real child vertex under its bus connection vertex, named
   by the peer name the link already uses.
2. The NAME `<door>/<peer>` and the anchor's PAIR reach the same vertex through the same walk.
3. The anchor inherits the door's `:acl` (nearest bearer) and may carry its own `:acl` override.
4. An anchor lives for its session. Every new session bumps the generation, including a returning
   peer of the same name, so cached pairs to a dead session are refused and fall back to the name.
5. Send-through an anchor is directed to exactly that session and never broadcasts.
6. Announce-census peers (CAN) stay vertex-less. The walk gates at the CAN door, and the link's own
   peer list resolves the remainder. They get no per-peer ACL, and this is documented.

**From #1938, the stage 1–4 rulings this one builds on:** the router is stateless and the connection
vertex is the only attachment point; one walk descends to the deepest connection vertex; one
`allows` call is evaluated there for every spelling; a link cannot exist without its connection
vertex; the maintainer's model, "a connection vertex is a door to another map" (RFC-0030 ruling 9).

**Standing constraints** (`CLAUDE.md` §Design rules): no timers or clock reads in libtracer; no
library-internal buffers; the receiver pays; compile-time policy by default; complexity is lowered
by deleting branches, never by splitting helpers; lookups stay population-independent in vertex
count. §8 checks each one.

## 4. Vocabulary

Proposed for `CONTEXT.md` at acceptance (§9.4). **Door** is RFC-0030 §4's.

- **Bus door.** The connection vertex of a multi-peer link (a listener carrying the `bus_link_t`
  facet, or a census bus). A bus door is never an egress by itself: its link's `send()` broadcasts
  (RFC-0020).
- **Session anchor.** The child vertex of a bus door that stands for one accepted session, named by
  the session's peer name. It is the session's door: a walk that reaches it with a remainder sends the
  remainder to that session alone.
- **Census peer.** A peer a bus learns of from announce or heartbeat traffic (ADR-0030, ADR-0044),
  rather than one this node's accept policy admitted. A census peer has no vertex.

## 5. The normative change

### 5.1 The anchor is a child of its door

**Normative.** When a bus link whose sessions are anchored (an accepting listener with
`peer_named = true`; §5.5 for the others) admits a session, the host MUST register the session's
anchor as a **child of the link's connection vertex**, with the session's peer name as its one NAME
segment. The name is the one the link already stamps (`p<slot>` for a creatorless session, ADR-0073
§2), so it passes `graph::valid_segment` by construction and is the same segment the NAME spelling
already uses.

- The anchor is **registered by the host's net plane, never by the application or a peer.** It is
  the one kind of vertex that may exist below a door. RFC-0030 §6.2 ("registering a local vertex
  below a door is refused") stands for every other vertex.
- The anchor is **revived in place.** A given `(door, peer name)` pair is allocated once and refilled
  on every later session that receives that name, exactly as today's `register_session_anchor` does.
  Anchor count is therefore bounded by the listener's `max_peers` and not by churn (ADR-0044
  §Amendment's measurement, unchanged).
- **The private root, the `:<mount>/<peer>` key and its parser are deleted** (§11.1 slice 5a).

### 5.2 One walk reaches it, in every spelling

**Normative; amends RFC-0029 §6 step 3 and RFC-0020 §3.**

- **NAME.** A `dst` whose run names `<door>/<peer>/…` is resolved by stage 1's descent to the
  **deepest existing connection vertex**. When `<peer>` names a live anchor, the anchor is that
  vertex. No new branch is needed: an anchor is a connection vertex one level below another.
- **PAIR.** A PAIR that dereferences to a live anchor is treated exactly as a PAIR that dereferences to
  a point-to-point connection vertex (RFC-0029 §6 step 3, first arm). With a non-empty residual it
  egresses (§5.4). As the last element it addresses the anchor's own `:` facets (RFC-0004 §A's dual
  nature).
- **The bus door itself** keeps RFC-0029's shared-mount arm. A PAIR that dereferences to a bus door
  with a non-empty residual is refused `tr::path::not_found`, because a bus door is not an egress
  (RFC-0020). As the last element it addresses the door's own facets, as today.
- **Below an anchored bus door, a segment that names no live anchor** is rejected
  `tr::path::invalid`, exactly as RFC-0020 §3 rejects a residual naming no current peer. On an
  anchored door an accepted session is reachable **only** as its anchor (§14 Q2). A host MUST NOT fall
  back to the link's peer list there, and MUST NOT emit on the bus link's shared endpoint.

RFC-0020 §3's sentence "MUST NOT resolve the residual against its local graph (a `WRITE` would
materialize a shadow vertex under the connection mount)" is **narrowed**: the first segment of the
residual MAY resolve to an anchor, which is the session's door and not a shadow, and nothing else
below a bus door is ever resolved locally. RFC-0030 §6.2 already forbids the shadow it was guarding
against, and RFC-0030 §7 refuses the creation that would have made one.

### 5.3 It lives for its session, and every session is a new generation

**Normative; amends RFC-0029 §8.1.**

1. **Departure retires the anchor, before anything else.** When a session ends, the host MUST retire
   its anchor before the link may hand the session's slot, and therefore its name, to another session.
   Retirement bumps the saturating generation (RFC-0029 §8.1). This is today's
   `fwd_router_t::bus_peer_down` order, made normative.
2. **Arrival revives it.** A session that receives a name whose anchor was retired revives that anchor
   at the bumped generation. A returning peer that receives the same name is a new session and gets a
   new generation, so a pair cached against its previous session fails the generation check, answers
   `tr::path::not_found`, and the path object falls back to the canonical string (RFC-0030 §5.2).
3. **An arrival for a live anchor is a session boundary.** If the link reports an arrival for a name
   whose anchor is still live (a missed departure, or a duplicated notification), the host MUST treat
   it as a departure followed by an arrival: retire, then revive. It MUST NOT keep the old generation
   for a session that may be new. Today the arrival is refused `PATH_IN_USE` and the old generation is
   kept (§14 Q4).
4. **The door going away takes its anchors with it.** When the door's tenancy changes or its link
   goes down (RFC-0029 §8.2 rules 1 and 2), every live anchor below it MUST be retired before the door
   is restamped. Accepted sessions report their own boundaries, so RFC-0029 §8.2 rule 4's per-boot
   epoch is not needed for them.
5. **Retirement clears the anchor.** A retired anchor keeps no `:acl`, no value and no edges into the
   next session (`vertex_t::revert_to_placeholder`). A per-session ACL override therefore ends with its
   session (§6.3). This follows ADR-0073 §2: a `p<slot>` name identifies a session, not a device.

### 5.4 Send-through is directed

**Normative; replaces RFC-0029 §10.** A walk that reaches a live anchor with a non-empty remainder
hands the remainder to **exactly that session**: the anchor's parent is the bus door, the door's link
resolves the anchor's name to its directed per-peer endpoint (`bus_link_t::peer_link`), and the frame
leaves on that endpoint alone. The `dst` shrinks by the consumed element and `src` grows by the
door's NAME run plus the peer segment (RFC-0030 §8.4).

- A host **MUST NOT** emit the frame on the bus link's shared endpoint, on any arm, including when
  the session departed between the dereference and the send. In that window the egress finds no
  directed endpoint and the frame is refused like any other stale address: `tr::path::not_found` to
  a non-empty `src`, a drop otherwise.
- **No vertex holds a transport pointer** (ADR-0038 §3b.1). The anchor-to-session binding is the
  anchor's parent and the anchor's name, resolved by the link's own peer table. That table is the
  link's state, which stage 3 moves onto the transport object. The router adds nothing.

### 5.5 Census peers stay vertex-less

**Normative.** A bus whose peers are announce-census peers (ADR-0044 §Decision 1; the CAN advertise
map) creates **no anchor**. A walk that reaches such a bus door with a remainder:

1. is authorized once, at the bus door (§6.1);
2. hands the remainder to the link, whose own peer list resolves its first segment (`n<node-id>` on
   CAN) to a directed per-peer endpoint, or rejects it `tr::path::invalid` (RFC-0020 §3, unchanged).

A census peer has **no per-peer ACL**: every census peer behind one bus door is governed by that
door's `:acl`. A census peer has no PAIR, so a chain stays NAMEs across a census bus (RFC-0029 §10's
last sentence, kept). The census rejection in ADR-0044 (an 8191-wide node-id space would cost
~960 KB of anchors on rv32) is the reason, and it is unchanged. Per-writer authorization on a census
bus is the subject's job (ADR-0082, ADR-0086), not an anchor's.

FLAT listeners (`peer_named = false`) have no bus facet. Their door is an ordinary door with one
routing identity, and none of this section or §5.1–§5.4 applies to them.

### 5.6 What an anchor is not

- **It holds no value.** An anchor is registered value-less (§14 Q6). A data operation that ends at
  an anchor stores nothing below a door. Its `:` facets (`:acl`, `:schema`, `:children[]`) are its
  surface.
- **Nothing can be created below it.** A walk that reaches an anchor with a remainder hands the
  remainder to the session first (RFC-0030 §6.2), so no creation hook below an anchor is ever
  consulted.
- **It is not an identity claim.** An anchor names where a session appears in the graph, never who
  is speaking. That is ADR-0082 §Decision 1, unchanged: the subject is the authorization answer.

## 6. Request/reply and ACL consequences: the connection vertex is the one gate

### 6.1 One gate, at the session's door

**Normative; applies RFC-0029 §6.4 to bus sessions.** A frame or a host-API call whose walk reaches a
session anchor is authorized by **one** `allows` call, at the anchor, for the caller's subject and
the operation's right. That holds for the NAME spelling, the PAIR spelling and a delivery along a
subscriber's chain alike. The bus door is **not** gated separately on the way through: one walk, one
gate, at the deepest door. The double gate of today's bound-delivery arm (anchor, then the mount
looked up by name) becomes that one call.

On a census bus the gate is the bus door, for every peer behind it (§5.5).

### 6.2 The anchor's ACL is the door's

**Normative.** The ACL an anchor is evaluated against is the door's ACL **as the door evaluates it**:
the door's own ACEs, then the door's inherited ancestor ACEs, in the door's evaluation order. That
is the "nearest bearer" of ruling 3, with the anchor counted as part of its door. An anchor with no
own ACEs therefore reaches exactly the verdict the door would reach. That is also exactly the verdict
the NAME spelling reaches today, so no deployment's grant set moves when its sessions gain anchors.

The general rule of reference/05 §`0x0A` is that a bare child evaluates only the ancestor ACEs that
carry `INHERIT`. This RFC does **not** apply it to anchors (§14 Q1). Under that rule a door ACE without
`INHERIT` would stop applying to the door's sessions, and a door bearing only such ACEs would leave
them with an empty effective list, which reference/05 defines as unrestricted. The reference
implementation realizes the anchor rule as one predicate in the existing bearer walk
(`graph_t::acl_allows`): an anchor counts as its door's "self".

### 6.3 The anchor's own `:acl` override

**Normative.** An anchor MAY carry its own `:acl`, written through the anchor's `:acl` facet by a
caller holding `WRITE_ACL` there (that is, on the door, by §6.2). Its own ACEs evaluate **first**,
then the door's list as in §6.2. The override lives for the session (§5.3 rule 5).

What an override can express depends on the build's ACL model, and this RFC adds nothing to either:

- On the **core subset** (ALLOW-only, reference/05 §`0x0A`), an override can only **add** grants for
  one session. Granting a session less than the door grants needs a door that grants less, plus
  per-session ALLOWs.
- On the **full model** (`security_acl`, ordered DENY), an override can also narrow one session,
  because its DENY evaluates before the door's ALLOW.

A durable per-device policy belongs on the **subject** (ADR-0082, ADR-0086), which survives the
session and the slot. The anchor override is for a decision about one live session.

### 6.4 Request/reply: the anchor is the egress record's session

**Normative; discharges RFC-0030 §9.1.** On an anchored bus, the pending record's "next-hop
session" is **the anchor's PAIR**. RFC-0030 §9.2 condition 2 compares it with the session the reply
arrived from, which the receiving node stamps (RFC-0030 §8.4). Because a returning peer revives the
anchor at a new generation (§5.3), a reply from a successor session in the same slot, with the same
name, fails condition 2 even when it presents a live token. That is a guard RFC-0030 §9.2 could only
offer by name before this RFC.

On a census bus, the session is the peer's `n<node-id>` segment, as RFC-0030 §9.1 already says. CAN
names are derived from node identity and are never inherited by another peer (ADR-0044 §Amendment),
so the name comparison is exact there.

Nothing here adds state at a hop or at the terminus. The anchor is the session's own vertex, bounded
by the accept policy. It is not per-request state (RFC-0029 §9.1).

### 6.5 Stamping across a bus

**Normative; amends RFC-0029 §4.2 and §6.2 for buses.** A node receiving a frame **from an
anchored session** stamps (RFC-0030 §8.4):

- into a non-empty `src`, the door's NAME run plus the peer segment, unchanged;
- into a present `0x15` child, **the anchor's PAIR**. Today `fwd_router_t::reverse_hop_ref` already
  stamps it; it becomes the rule for every frame that carries the child.

So a reply's `0x15` chain, and the chain a subscriber learns, is PAIRs across an anchored bus too.
RFC-0029 §2.2's second essential divergence closes for accepted sessions. It stays open, by design,
across census buses (§5.5).

### 6.6 Subscriptions

A delivery to a subscriber behind an anchored session walks the edge's chain like any frame. Its
head element is the anchor's PAIR, and it is gated once at the anchor (§6.1). When the session ends,
the anchor's retirement makes every such chain stale. The first delivery then answers
`tr::path::not_found`, and the producer reclaims the edge (#1258), as it does today on the bound
delivery arm. The name-keyed eviction on departure (`fwd_router_t::link_down` of the peer name) is
unchanged.

### 6.7 A refusal at an anchor answers the same in every spelling

**Normative.** A refusal at a session anchor has one shape for every spelling. That covers a
generation mismatch, a departed session, and a denial by the anchor's gate (§6.1). The `src` decides
whether anything is sent; the spelling does not:

- **A non-empty `src`:** the host answers the refusal to `src` with the identity the NAME spelling
  answers, `tr::path::not_found`. This holds whether the frame named the anchor by NAME or by PAIR,
  and whether the anchor was the last element or a hop.
- **An empty `src`:** nothing is sent (RFC-0030 §8.5). Every subscriber delivery carries an empty
  `src` (RFC-0004 Amendment 2), so a refused delivery is a silent drop.

The delivery leg therefore needs no carve-out. Its silence follows from the empty-`src` rule that
every other frame follows. Today the two spellings differ: on the last-element PAIR arm, which
the bound-delivery path in `fwd_router_t` serves, a refusal at the mount is dropped silently even
when the frame is a `WRITE` with a non-empty `src`. The NAME spelling answers that same `WRITE` with
`tr::path::not_found`. Slice 5c makes the PAIR arm answer like the NAME arm (§14 Q8).

## 7. Cost: throughput, latency and RAM across NARROW, MID and WIDE

### 7.1 Per-peer RAM of an anchor, against today

An anchor already exists today for every `peer_named` session. This RFC moves it; it does not add
one. Per session, against today:

| Item, per anchored session | Today (under the private root) | This RFC (under its door) |
| --- | --- | --- |
| `vertex_t` | 64 B rv32 / 88 B host (`kMaxVertexBytes32`, `kMaxVertexBytes64`, `core/include/libtracer/config.hpp`) | same |
| Key record | `3 + len(mount) + len(peer)` B (`:<mount>/<peer>` plus the length byte) | `1 + len(peer)` B |
| Vertex-index slot | 4 B rv32 / 8 B host | same |
| Parent's child-list entry | one pointer in the private root's list | one pointer in the door's list |
| Value | an empty `STORED_VALUE` slot | none (value-less, §14 Q6) |
| `:acl` | never written: no walk reaches it | allocated only when an override is written (§6.3) |

With the door `net/ws` and the peer `p0`, the key record goes from 11 B to 3 B. A plain anchor stays
at the measured cost of a plain vertex: about **111 B on a 64-bit host** (`handler_vertex_heap_test`,
[#1959](https://github.com/avatarsd-llc/libtracer/pull/1959)) and **about 110–120 B on rv32**
(ADR-0044 §Amendment, `bench_forward_heap`), less `len(mount) + 2` B of key. §13 clause 4 holds
the claim to "never more than today".

Per node and per door:

- **Every graph, every profile: one `vertex_t` less.** The private anchor root (`graph_t::roots_t::anchor`)
  is deleted: −64 B on rv32, −88 B on a host. This includes FLAT and bus-closed builds, which carry it
  today without using it.
- **Each anchored door: one child list,** allocated with its first anchor. That is the same
  `children_t` the private root carries today, so a node with one anchored door breaks even on it.

| Profile | Typical shape | Per-peer RAM today | Per-peer RAM after |
| --- | --- | --- | --- |
| **NARROW** (single-upstream MCU) | FLAT listener or a dialer; or `kBusLinks` closed | 0 B | 0 B, and −64 B per graph |
| **MID** (board serving a few browser tabs) | `peer_named = true`, `max_peers` ≈ 4 | ≈ 0.5 KB at 4 sessions | the same, less `4 × (len(mount) + 2)` B |
| **WIDE** (gateway) | `peer_named = true`, hundreds of sessions | ≈ 111 B × sessions on a host | the same, less `(len(mount) + 2)` B × sessions |

### 7.2 `peer_named = false` keeps FLAT links free

ADR-0082's default is untouched, and this RFC MUST NOT make it cost anything. A FLAT listener has no
bus facet: it registers no anchor, grows no child list, allocates no slot and parses nothing per
frame. The anchor arm of the walk (§5.2) is compiled out where `kBusLinks` closes the bus tier
(ADR-0082 §Consequences, ADR-0047 §1). The only change a FLAT node sees is the deleted private root
(§7.1).

### 7.3 Hot path

- **NAME through an anchored session.** Today: the mount descent (25 ns per pass,
  `child_registry.hpp`) plus `resolve_peer`. After stage 1: the tree descent to the door, one more
  child lookup in the door's sorted list (a binary search over at most `max_peers` anchors), then the
  same `peer_link`. The added step is bounded by the accept policy, not by the vertex count. The
  `bench_forward_demux` gate decides (§11.1).
- **PAIR through an anchored session** is new. It costs `deref_vertex_slot` (11 ns flat), one parent
  load and the same `peer_link`, against a descent it replaces. A bound hop measured 261.7 ns
  against 304.7 ns for a canonical hop (RFC-0024 §3.4).
- **A delivery along a chain** loses one `allows` call under enforcement (§6.1), the per-delivery
  `session_anchor_route` parse and the per-delivery `entry_by_name` lookup of the mount.
- **Payload size.** Every change here is per frame and independent of payload size, so the rows above
  1 KiB (1 KiB, 4 KiB, 16 KiB and up) move only by the same constant as the 64 B rows.

### 7.4 Bytes on the wire

Across an anchored bus, one 11 B PAIR replaces the NAME run `net/<module>/<name>/<peer>`. For the
short `net/ws/p0` (three records, 10 B) that is +1 B. Every longer mount name saves bytes. Bytes are
not the point: the win is one spelling and no descent. On a census bus, nothing changes.

## 8. The constraints, checked

| Constraint | How this RFC meets it |
| --- | --- |
| **No timers, no clock reads** | None added. Session boundaries are the link's accept and close events (§5.3). |
| **No library-internal buffers** | None added. The anchor comes from the graph's injected table source, as today. Its child-list entry replaces one in the private root. |
| **The receiver pays** | An anchor is drawn from the accepting node's own graph and bounded by its own `max_peers` accept policy. A peer cannot grow it by churn (§5.1). |
| **Compile-time by default** | The anchor arm compiles out with `kBusLinks` (§7.2). `peer_named` stays a wiring-time knob with default `false` (ADR-0082). No new runtime knob. |
| **Delete, do not split** | Deleted: the private root, the `:<mount>/<peer>` key, `session_anchor_id` and `session_anchor_id_t`, `session_anchor_route`, `find_session_anchor`, `session_anchor_slots`, the PAIR shared-mount refusal for anchors, the second `allows` and the `entry_by_name` lookup in the bound-delivery arm. Added: one predicate in the ACL bearer walk (§6.2), and the arrival rule of §5.3 rule 3. The per-file CCN totals are the judge (#1790). |
| **Population-independent lookups** | The anchor lookup is bounded by `max_peers`, not by vertex count. A PAIR to an anchor is a slot dereference. |

## 9. Normative pages that change

### 9.1 `docs/spec/v1.md`

**§3, the `05-protocol-tlvs.md` incorporation bullet, its `0x06` routing semantics.** "A PAIR MUST NOT
egress through a shared (bus) mount" becomes: a PAIR MUST NOT egress through a **bus door**. A PAIR
that dereferences to a **session anchor** egresses to exactly that session (§5.4). A walk below an
anchored bus door reaches only anchors (§5.2). The verdict for a session is evaluated once, at its
anchor, against the door's ACL plus the anchor's own (§6.1–§6.3).

### 9.2 `docs/reference/05-protocol-tlvs.md` (normative annex)

- **§`0x06` §path element PAIR, the per-hop algorithm, step 3.** The shared-mount arm splits into
  two lines. A bus door with a non-empty residual is refused, as today. A session anchor is the
  point-to-point arm (§5.2). The admonition "until RFC-0029 §10's session-anchor egress (slice S8)
  lands" goes.
- **§`0x06`, "One element per node's whole local part".** The example "a shared (bus) mount" becomes
  "a census bus".
- **§`0x0A` ACL.** One sentence: a session anchor evaluates its door's ACL as the door does, then its
  own (§6.2–§6.3). Census peers have no per-peer ACL (§5.5).

### 9.3 `docs/reference/03-addressing.md`

The normative annex part (§path syntax) is unchanged: an anchor's name is an ordinary segment. In the
informative routed-scope text, the sentence on nodes that "cannot issue a PAIR — a shared (bus)
mount" becomes "a census bus".

### 9.4 Informative pages, records and the glossary

- `docs/reference/19-transports-are-vertices.md` §What is a vertex: a row for the session anchor
  (position `<door>/<peer>`, value-less, retired with its session).
- `docs/reference/16-websocket-session-auth.md`: the anchor's place (under the door, named by the
  peer) and the override, beside the subject guidance.
- `docs/reference/14-can-transport.md`: census peers have no vertex and no per-peer ACL; the CAN door's
  `:acl` governs every peer behind it.
- `docs/reference/04-communication-flows.md`: a chain crosses an anchored bus as PAIRs.
- `docs/modules/fwd-router.md`: the deleted anchor-id machinery and the directed send-through.
- `CONTEXT.md`: new entries for **Bus door**, **Session anchor** and **Census peer** (§4). **Transport
  vertex / connection vertex** gains "a bus door's sessions are its child anchors".
- **Status rows at acceptance:** RFC-0029 (§4.2, §6 step 3, §8.1, §10 and §13.2 S8 amended by
  RFC-0031); RFC-0020 (§3 amended by RFC-0031).

### 9.5 Records this RFC contradicts, said explicitly

Under `CLAUDE.md`'s precedence rule, each of these is named rather than overridden silently:

- **The design note on `graph_t::register_session_anchor` and `graph_t::anchor_root`**
  (`core/include/libtracer/graph.hpp`): "an anchor is not part of the addressable tree,
  deliberately … invisible to all of them". This RFC reverses it. The note's two reasons are answered
  here: `enumerate_peers` stops being the second source of a bus door's members (§14 Q5), and RFC-0020
  §3's premise is narrowed in §5.2.
- **RFC-0029 §10**: "Normative, until §13.2 S8 lands." This RFC is that landing, and its text
  replaces §10.
- **ADR-0044 §Implementation status**, "`child_registry_t::by_name` falls back to asking each bus
  child's `bus_link_t::peer_link(name)`": on an anchored bus the anchor is the route, and the
  peer-list fallback is kept for census buses only (§5.2, §5.5). ADR-0044's decisions are unchanged.
- **ADR-0082 §Reason 2**, which describes the anchor as a "routing-table entry": after this RFC it is
  a child vertex of the door. ADR-0082's decision and its default are unchanged.

## 10. Breaking surface

Every row is breaking. No compatibility shim is owed (ruling 10 of RFC-0029).

| # | Surface | Was | Becomes |
| --- | --- | --- | --- |
| B1 | PAIR to a session anchor, non-empty residual | the anchor is not a door; refused | directed egress to that session (§5.4) |
| B2 | PAIR to a session anchor, last element | no addressable surface | the anchor's `:` facets (§5.2) |
| B3 | `0x15` stamp on arrival from an anchored session | the anchor's PAIR on the subscribe leg only | the anchor's PAIR on every frame that carries `0x15` (§6.5) |
| B4 | Authorization of a frame through a session | NAME: at the door; bound delivery: at the anchor and at the door | once, at the anchor, against the door's list plus the anchor's own (§6.1–§6.3) |
| B5 | Below an anchored bus door, a name with no live anchor | resolved by the link's peer list | `tr::path::invalid` (§5.2, §14 Q2) |
| B6 | Arrival for a live anchor | refused `PATH_IN_USE`, generation kept | retire and revive, generation bumped (§5.3 rule 3, §14 Q4) |
| B7 | `:children[]` of an anchored bus door | synthesized from `enumerate_peers` | the door's child anchors (§14 Q5) |
| B8 | Anchor role | an empty `STORED_VALUE` | value-less (§14 Q6) |
| B9a | Refusal at an anchor, PAIR spelling, non-empty `src` | silent drop on the last-element arm | `tr::path::not_found` to `src`, as the NAME spelling answers (§6.7) |
| B9 | Host API (reference implementation) | `graph_t::register_session_anchor(id)`, `find_session_anchor`, `session_anchor_route`, `session_anchor_slots`; `fwd_router_t::session_anchor_id`; `session_anchor_id_t` | an anchor registered under its door by `(door, peer name)`; the id type, the route parse and the private census are deleted |

Each implementation slice carries its own `CHANGELOG.md` entry under **Breaking** (`core/`, and
`bindings/typescript/` if its client spells a bus route) for the rows it lands. This RFC is not
itself a public API change.

## 11. Slices and conformance

### 11.1 Slices (the stage-5 ticket of #1938)

All four land under [#1948](https://github.com/avatarsd-llc/libtracer/issues/1948), which is blocked
by #1939 (one walk, one gate), #1940 (the connection vertex is mandatory) and this RFC.

| Slice | Contents | Gate |
| --- | --- | --- |
| **5a anchors under the door** | §5.1–§5.3: register the anchor as a child of the door by `(door, peer name)`; delete the private root, the id type, the route parse, `find_session_anchor` and `session_anchor_slots`; the stage-1 descent finds anchors; the anchored-door rejection of §5.2; the arrival rule (§5.3 rule 3); the door takes its anchors with it (§5.3 rule 4); value-less anchors; `:children[]` from the real children. | `session_anchor_test` rewritten against the tree; the ws-server multi-peer test; `vertex_size_test` unchanged; the per-graph −1 `vertex_t` visible in `bench_forward_heap` |
| **5b directed send-through** | §5.4 and §6.5: the PAIR arm admits an anchor (`fwd_router_t::bound_egress`); egress through the parent door's link to `peer_link(name)`; the `0x15` stamp on every frame. | `bench_forward_demux` at 1, 4 and `max_peers` sessions, 64 B to 16 KiB, inside the A/A band; never a byte on the shared endpoint (asserted on the in-memory links) |
| **5c one gate** | §6.1–§6.3: one `allows` at the anchor on every arm; the anchor-as-door's-self predicate in `graph_t::acl_allows`; the override; delete the second gate and the `entry_by_name` lookup on the bound-delivery arm; the refusal shape of §6.7 on both spellings. | the ACL tests of §11.3; `acl-inherit-d4` on the perf gate |
| **5d pages** | §9 text, the glossary and the status rows. | doc gates (`check_doc_citations.py`, the strict Sphinx build) |

5a lands first. 5b and 5c each depend on 5a only.

### 11.2 Conformance vectors

**Kept, rescoped:** `fwd/bus-mount-refuses-pair` names a bus door with a residual (§5.2).
`fwd-bus-name-reject` (RFC-0020 §5) keeps its bytes.

**New** (names indicative):

- `fwd/pair-anchor-egress-directed`: a PAIR to an anchor with a residual egresses with `dst` shrunk
  by the element and `src` grown by the door run plus the peer segment;
- `fwd/pair-anchor-last-element-is-facet`;
- `fwd/pair-anchor-stale-generation-not-found`: the pair of a departed session, after a new session
  took the same name;
- `fwd/name-below-anchored-door-no-anchor-invalid`;
- `fwd/reverse-0x15-stamps-anchor-pair`: a frame arriving from an anchored session carries the
  anchor's PAIR at the head of `0x15`;
- `fwd/census-peer-stays-names`: across a census bus, `0x15` carries the NAME run;
- `fwd/anchor-refusal-spelling-independent`: a `WRITE` with a non-empty `src` refused at an anchor
  draws the same `tr::path::not_found` reply bytes whether `dst` names the anchor by NAME or by PAIR.
  The same refusal with an empty `src` (a delivery) draws no frame.

### 11.3 Behaviour tests (the S set, on the wire and at the host API)

Each is driven through the node's external surface (#1938 §Testing), and each fix-shaped test is
shown to fail with its change ablated:

- NAME and PAIR to one session reach the same vertex and the same verdict, with enforcement on and
  off;
- the door's ACL governs a session with no override, including a door ACE without `INHERIT` (§6.2);
- an anchor override adds a grant (core subset) and, on the full model, narrows one session;
- the override is gone after the session ends and a new session takes the name;
- a reconnect under the same name gets a new generation, the old pair is refused, and the name still
  works;
- an arrival for a live anchor bumps the generation (§5.3 rule 3);
- send-through reaches exactly one session, and a session departing mid-send leaves nothing on the
  shared endpoint;
- a census peer resolves through the link's peer list, is gated at the door, and has no vertex;
- a FLAT listener allocates no anchor and no child list across 1,000 accept and close cycles;
- §6.7: a gate refusal at an anchor answers `tr::path::not_found` to a non-empty `src` on both
  spellings, and stays silent on a delivery;
- RFC-0030 §9.2: a reply from a successor session in the same slot fails condition 2.

## 12. Alternatives considered

- **Keep the private root and do S8 as RFC-0029 §10 wrote it** (a directed send keyed on the anchor's
  slot, the anchor still unreachable by name). It closes the send-through gap and nothing else. NAME
  and PAIR keep reaching different vertices with different gates, the `:<mount>/<peer>` key and its
  parser stay, and the anchor remains the one thing on a node that a walk cannot reach. Rejected by
  ruling 1.
- **Anchors for census peers too.** The ADR-0044 arithmetic still holds: ~960 KB at CAN's full
  node-id space on rv32, and a census is driven by another node's liveness, not by this node's accept
  policy. Rejected by ruling 6.
- **An incarnation suffix on the peer name (`p0-7`)** instead of a generation. Rejected by the
  ADR-0044 amendment already: it is irreversibly wire-visible, grows every `src`, and contradicts
  ADR-0073 §2.
- **Bare-child inheritance for anchors** (only `INHERIT` door ACEs reach a session). It is the general
  rule with no special case, but it changes the verdict for every deployment whose door ACEs lack
  `INHERIT`, and it can leave sessions with an empty, hence unrestricted, effective list. Rejected in
  favour of §6.2; see §14 Q1.
- **Fall back to the link's peer list when an accepted session has no anchor.** One fewer branch,
  but the same session would then be reachable as two vertices with two gates, which is the state
  this RFC removes. Rejected; see §14 Q2.
- **Keep a per-session override across sessions, keyed by name.** A `p<slot>` name identifies a
  session, not a device (ADR-0073 §2), so a policy keyed by it would pass to whoever takes the slot
  next. Durable policy belongs on the subject. Rejected.
- **Give the anchor a transport pointer for a faster egress.** ADR-0038 §3b.1 forbids a vertex that
  holds a transport pointer. The parent door and the link's own peer table already give the egress.
  Rejected.
- **Make every listener `peer_named` so that every session has an anchor.** This is ADR-0082's
  rejected flip: per-peer state everywhere for an addressing capability most nodes never use.
  Rejected.

## 13. What would falsify this RFC

1. **The NAME arm through an anchored session regresses** on `bench_forward_demux` beyond the A/A
   band at any session count up to `max_peers`, at any payload size including above 1 KiB, against
   today's `resolve_peer` path. Then the child lookup of §7.3 is the wrong structure, not the model.
2. **NAME and PAIR to one session can reach different verdicts, or different replies to one
   refusal.** §6.1 claims one gate and §6.7 claims one refusal shape. A reachable counterexample
   means a second gate or a second refusal arm survived somewhere.
3. **A reachable sequence lets a stale anchor pair validate** for a successor session other than
   through a saturated generation. §5.3's rules are the whole guard.
4. **An anchored session costs more RAM than today** on rv32, or a FLAT listener pays anything per
   peer. §7.1 and §7.2 claim neither happens.
5. **A transport cannot pair its arrival and departure reports**, so that §5.3 rule 3 bumps live
   sessions often enough to cost measurable re-learn traffic. That would be a measurement on a real
   listener (the ESP httpd link is the first to check), and the answer would be a fix to the
   transport's reporting, not a refusal in the graph.

## 14. Questions for the maintainer, each with a recommendation

1. **Does an anchor evaluate the door's ACL as the door does (§6.2), rather than as a bare child
   (only `INHERIT` ACEs)?** *Recommendation: yes.* It keeps every existing door verdict for sessions
   exactly as the NAME spelling reaches it today, it can never leave a session with an empty effective
   list while its door has ACEs, and it costs one predicate in the bearer walk.
2. **On an anchored bus door, is a session reachable only as its anchor (§5.2)?**
   *Recommendation: yes.* A name with no live anchor answers `tr::path::invalid`, as RFC-0020 §3
   already answers a name with no peer. A fallback to the link's peer list would let one session be
   two vertices with two gates.
3. **If an anchor cannot be registered at admission (the table source refuses its block), is the
   session refused?** *Recommendation: yes.* The link closes it and counts the refusal (`core/STYLE.md`
   §Introspection). Under Q2 a session without an anchor is unreachable and ungateable, so admitting it
   buys nothing. The receiver pays: admission fails on the receiver's own budget.
4. **Is an arrival for a live anchor a session boundary (§5.3 rule 3)?** *Recommendation: yes,*
   retire and then revive. A spurious bump costs one re-learn. A missed bump lets a cached pair reach
   a session that may be new, which is the class #1223 closed. Today's `PATH_IN_USE` keeps the old
   generation.
5. **Does `:children[]` of an anchored bus door list its real child anchors, with the link's
   synthesis kept only for census buses?** *Recommendation: yes.* There is one source per bus kind,
   and the synthesis (`handlers_t::on_children` wired to `bus_link_t::enumerate_peers`) stops being a
   second answer for the same members.
6. **Is an anchor value-less (§5.6)?** *Recommendation: yes.* An anchor is a door, not a store. Today
   it is an empty `STORED_VALUE`, so a data write that ended at one could leave a value below a door,
   which RFC-0030 §6.2 forbids for every other vertex. The conformance test pins the answer a data
   operation at an anchor receives.
7. **Is the session's liveness published on the anchor** (the way a connection vertex publishes its
   link state, reference/19)? *Recommendation: no, not in this RFC.* An anchor exists only while its
   session is up, so its existence is the liveness, and a departure is already observable as a
   `:children[]` change on the door. A value can be added by a later amendment if a consumer needs one.

8. **Does a refusal at an anchor answer `tr::path::not_found` to a non-empty `src` on both spellings
   (§6.7), with the delivery leg's silence following from its empty `src`?** *Recommendation: yes.*
   The alternative keeps today's split: answer on the NAME spelling, drop on the PAIR spelling, with
   the delivery leg written down as a carve-out. That makes the reply depend on the spelling, which
   RFC-0029 §6.4 rules out, and it needs a branch that the empty-`src` rule already covers. The
   identity is the one the NAME spelling answers today, so no existing NAME reply changes.

## 15. Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the comment window is waived by default while the
project is solo-maintained, and it is not invoked here. Sustained objections and their resolution are
recorded in this section as they arrive.
