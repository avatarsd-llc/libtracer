<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0030 — The host API walks the graph: a graph-owned path object, creation refused by default, the reply as a remote write, `AWAIT` and `REPLY` retired

<!-- status: proposed -->

| Field | Value |
| ---- | ---- |
| **RFC** | 0030 |
| **Title** | The host API walks the graph: a graph-owned path object, creation refused by default, the reply as a remote write, `AWAIT` and `REPLY` retired |
| **Status** | **proposed** (2026-10-07). The maintainer approves it. The direction of §§5–10 was **ruled** on 2026-10-07 in [#1938](https://github.com/avatarsd-llc/libtracer/issues/1938) (stage 4 of "one walk"). This document turns that ruling into normative text, and §18 lists the choices the ruling left open, each with a recommendation. |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-10-07 |
| **Comment window** | Waived by default while the project is solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"). Invoke it explicitly if outside input is wanted. At drafting, `docs/implementations.md` still lists no registered implementation, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** It retires two `FWD` opcodes and two `FWD` children, changes what a reply frame is, changes what every receiving node stamps into a frame, assigns a reserved `delivery_policy` bit, and turns a `MUST create` into a `MUST NOT create`. GOVERNANCE.md reserves each of these for an amendment. **No backward compatibility is owed**: the project takes none (the RFC-0028 ruling, restated as RFC-0029 ruling 10). |
| **Tracking issue** | [#1942](https://github.com/avatarsd-llc/libtracer/issues/1942); parent spec [#1938](https://github.com/avatarsd-llc/libtracer/issues/1938) |
| **Folds in** | [#1631](https://github.com/avatarsd-llc/libtracer/issues/1631) (one path primitive): its 2026-09-29 rulings carry over, except ruling 4. [#1502](https://github.com/avatarsd-llc/libtracer/issues/1502) (an empty `src` means no reply is requested). Optionally [#1932](https://github.com/avatarsd-llc/libtracer/issues/1932) (subscriber append returns its entry identity), in §9.7, at the maintainer's choice (§18 Q4). |
| **Target spec version** | v1 itself. `docs/spec/v1.md` still reads "(DRAFT)". RFC-0018, RFC-0023, RFC-0024, RFC-0027 and RFC-0029 took the same route. |
| **Scope** | v0.20.0, with stages 1–3 of #1938. The stage-4 implementation tickets ([#1943](https://github.com/avatarsd-llc/libtracer/issues/1943), [#1944](https://github.com/avatarsd-llc/libtracer/issues/1944), [#1945](https://github.com/avatarsd-llc/libtracer/issues/1945), [#1946](https://github.com/avatarsd-llc/libtracer/issues/1946)) are blocked on this RFC (§15). |
| **Amends** | [RFC-0004](0004-remote-operation-addressing.md) §B, §C, §D, §H, Amendment 2 and Amendment 3; [RFC-0005](0005-subtree-subscriptions.md) §D and its Amendment 1; [RFC-0029](0029-one-path-primitive.md) §4.3, §6.1, §6.2, §7.1 and §7.2; [RFC-0022](0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.A (one reserved bit). |
| **Supersedes, on acceptance** | [ADR-0084](../../adr/0084-remote-await-completes-from-a-receiver-side-waiter.md) (the receiver-side `AWAIT` waiter) in full; [ADR-0006](../../adr/0006-read-write-await-api-no-connect.md) in part (`await` leaves the wire verb set and becomes a host-API form). |

> **Numbering note.** Numbering gaps and why they are not reused are recorded in the
> [ADR and RFC index](../../adr-rfc-index.md#numbering-gaps).

---

## 1. Summary

Today a libtracer node has one model of the graph for the wire and a different one for the
application. A frame walks the graph and, at a connection vertex, hands the rest of its path to the
link. A host-API call does not: a local `write` to a path below a connection vertex creates a local
**shadow vertex** and never reaches the remote device. Reaching the remote needs a separate door,
`fwd_router_t::originate`. Its answer comes back through a dedicated `REPLY` opcode and is matched by
`src` suffix with an oldest-first fallback. A remote `AWAIT` parks a waiter at the receiver
(ADR-0084), and the local `graph_t::await` reads a clock to time out.

This RFC makes the host API walk the same graph as the wire, and makes the answer an ordinary
operation:

1. **The path object.** The application addresses a vertex through a **graph-owned path object**,
   built from a canonical string. It holds that string as the durable truth, plus a soft cache of the
   mixed NAME/PAIR chain of RFC-0029 §4.2, which it learns from matched replies. A miss or
   `NOT_FOUND` falls back to the string. The path object replaces the pointer-style vertex handle at
   the host API. It is sized by compile-time policy and drawn from the injected allocation seam.
2. **The host API walks into doors.** A read or write whose walk reaches a connection vertex (a
   **door**) hands the remainder to that link, exactly as a forwarding hop does. There is no shadow
   vertex. `originate` retires.
3. **Creation is refused by default everywhere.** A write to a missing vertex answers
   `tr::path::not_found`, locally and remotely. A parent vertex opts in to creating children by
   carrying **app-supplied creation logic**. This amends RFC-0005 §D (write-creates).
4. **The reply is a remote write** to the requester's **reply path**, which the request carries in
   `src`. An empty `src` means no reply is requested, at every hop. The requester admits a reply only
   if it matches a request it is waiting on. That record is requester-side state: **hops hold
   nothing**. §9 states the matching rule in full.
5. **`AWAIT` and `REPLY` retire.** An asynchronous call returns a **pending handle**. A synchronous
   call waits on that handle with a deadline from the application's own clock. An await is realised
   as a one-shot subscription. There is no library timer and no clock read.

## 2. Motivation

### 2.1 Two models of "what is attached here"

[#1938](https://github.com/avatarsd-llc/libtracer/issues/1938) §Problem states the structural half:
the graph holds every connection vertex, while the router keeps its own mount registry as a second
name index over the same tree. Stages 1–3 of that spec make the walk the only index. This RFC is the
half the application sees, and it is the half that needs normative text.

### 2.2 A local write below a door does not reach the door

`graph_t::write(const path_t&, view::rope_t)` (`core/src/graph.cpp`) looks a path up with
`find_ptr`. On a miss it write-creates through `graph_t::ensure_vertex_ptr`, `mkdir -p` style
(RFC-0005 §D, whose Amendment 1 kept the **local** arm on purpose). So
`write("/net/ws/board/led", on)` on a node whose `/net/ws/board` is a connection vertex creates a
local vertex `/net/ws/board/led`, stores `on` in it, and returns success. The board never receives
the write, and a later local read returns a value the remote never had. Reaching the board takes
`fwd_router_t::originate` (`core/include/libtracer/fwd_router.hpp`), a second entry point with its own
record type (`fwd_router_t::origin_t`), its own reply sink and its own cancel.

### 2.3 The answer is a separate machine, matched by suffix

A request's answer is `FWD{op=REPLY}`, a fourth opcode with a `kind` child and a
"terminus-reply asymmetry" (RFC-0004 §B) that only replies take. RFC-0004 §D says there is no
end-to-end correlation id and leaves matching to "the transport". In practice, every requester
matches by `src` suffix:

- `reference/04` §Invariants: the oldest outstanding request whose `dst` ends with the reply's
  `src`, then arrival order among requests the suffix cannot tell apart;
- `originate` mints a fresh one-segment route per request and falls back to oldest-first pairing
  for a caller-supplied route;
- the TypeScript client keeps a FIFO of pending requests keyed by `dst`.

RFC-0004 Amendment 3 clause 3 records what that costs: replies name no request op, so a requester
SHOULD NOT have an `AWAIT` and a `READ` to the same vertex outstanding on one link, because their
RESULTs are indistinguishable. A matching rule that needs a usage restriction is not a rule.

### 2.4 `AWAIT` keeps state at the receiver and a clock in the library

A remote `AWAIT` parks a one-shot waiter at the terminus, charged to the receiving link
(ADR-0084, `fwd_router_t::pending_await_t`). That is per-request state at the node that answers,
held until the vertex changes or the link drops. The local `graph_t::await(v, timeout, …)` waits on
a condition variable for a `std::chrono` duration, which is a clock read inside the library. That
contradicts the standing rule in `CLAUDE.md`: no timers or clock reads in libtracer. The write-sequence
and stripe-waiter machinery that exists to wake an await is sized in
[#1792](https://github.com/avatarsd-llc/libtracer/issues/1792) (findings on dropping await).

### 2.5 Creation happens on a miss

Write-creates makes a typo a vertex. On the local arm, nothing but the CREATE bit on the nearest
existing ancestor stands between a mistyped path and a new `STORED_VALUE` subtree, and with no ACL
present creation is open. RFC-0005 Amendment 1 removed the remote arm for this reason. Once a local
write below a door is forwarded (point 2), the local arm cannot keep a different rule without
reintroducing the shadow. Creation becomes an explicit decision of the parent's owner (§7).

## 3. The rulings this document encodes

**From #1631 (2026-09-29), carried:**

1. The primitive is the owner-issued `(index, generation)` pair, carried per hop. It is minted, not
   hashed.
2. The full string path is the durable truth. The chain is a learned cache, re-learned after a
   reboot, a departure, a retirement or a refusal.
3. No per-request state at a hop, ever.

**From #1631, superseded:** ruling 4, "a per-hop label table is acceptable because it is a pure
cache". Under the 2026-10-07 ruling hops hold no table at all. COMPACT and the per-link handle
tables are deleted in stage 6 ([#1951](https://github.com/avatarsd-llc/libtracer/issues/1951)).

**From #1502 (2026-08-22), carried:** a reply is something the requester asks for by wiring a
return route, never something the library imposes. An empty `src` means "no reply requested".

**From #1938 (2026-10-07), the stage-4 rulings:**

4. The host API takes a **graph-owned path object**, built from a string, that carries both NAME and
   PAIR elements and learns pairs from replies.
5. A write below a door goes to the remote, with no shadow copy. `originate` folds into the walk.
6. Creating a missing vertex is **refused by default everywhere**. It is opt-in app logic on the
   parent vertex.
7. A reply is an ordinary **remote write** to the requester's reply path. It is admitted only if it
   matches a request the requester is waiting on. That is requester-side state, which RFC-0029 §9.1
   permits; hops hold nothing.
8. `AWAIT` and `REPLY` retire. Async means a pending handle. Sync means the app waits on that handle
   with a deadline from its own clock. An await can be a one-shot subscription. No library timer.
9. The maintainer's model: **a connection vertex is a door to another map.** The path after it is
   the other world's path from its own root, and the way back is stamped into the message by the side
   that receives it.

**Standing constraints** (`CLAUDE.md` §Design rules): no timers or clock reads in libtracer; no
library-internal buffers; the receiver pays; compile-time policy by default; complexity is lowered
by deleting branches, never by splitting helpers; lookups stay population-independent in vertex
count. §11 checks each one.

## 4. Vocabulary

These terms are proposed for `CONTEXT.md` at acceptance (§12.4).

- **Door.** A connection vertex, seen from a walk. A walk that reaches a door with a non-empty
  remainder hands the remainder to the door's link. A walk that *ends* at a door addresses the door's
  own `:` facets (RFC-0004 §A's dual nature, unchanged).
- **Path object.** The graph-owned object an application addresses a vertex through (§5).
- **Reply endpoint.** One node-local vertex per node that receives reply writes (§8.1). It is the
  RFC-0029 §7.2 learn endpoint, unified.
- **Token.** One NAME segment, minted by the requester, that names one pending request at the
  requester's reply endpoint (§9.1).
- **Reply path.** The reply endpoint's route followed by a token. A request carries it as the seed of
  its `src`.
- **Pending handle.** The requester-side record of one outstanding request, and the application's
  handle to its outcome (§9.4).
- **Creation hook.** App-supplied logic on a parent vertex that decides whether a write to a missing
  child creates it (§7.2).

## 5. The path object

### 5.1 What it is

**Normative.** A conforming implementation's host API MUST address a vertex through a **path
object** that:

1. is **created from a canonical path**, either a string that the implementation parses and
   validates once, or a pre-encoded `PATH` TLV in read-only memory (v1 §3.1.2), which it borrows and
   does not copy;
2. **holds the canonical bytes for its whole life**. They are the durable truth (ruling 2) and the
   only form an application may persist. The canonical bytes never carry an escape record (v1 §3.1.1);
3. holds a **chain cache**: at most one mixed NAME/PAIR chain (RFC-0029 §4.2) that spells the same
   address. The cache is **soft**: its absence, loss or refusal never changes an answer (RFC-0029
   §9.1);
4. is **owned by the graph**. The graph allocates it, updates its chain cache when a matched reply
   teaches one (§8.7), and frees it when the application releases it and no pending handle refers to
   it.

A path object is a v1 §3.1 **path handle** that also carries a soft chain. Everything §3.1.1 requires
of a path handle still holds of its canonical bytes, and the chain is never a key (RFC-0029 §5.1).

### 5.2 The chain cache

**Normative.**

- **Content.** The chain is the address as the walk would spell it head-first. Element 0 is local:
  the PAIR of the target vertex when the target is on this node (a chain of length one), or the PAIR
  of the first door. The remaining elements belong to the nodes beyond that door, one element per
  node's whole local part (RFC-0027 §5.3.3 amendment 6, carried by RFC-0029 §12.3), as PAIRs where that node issued
  one and as NAMEs where it did not.
- **First use.** Before anything is learned, the walk spells element 0 from its own resolution and
  the remainder as the canonical NAMEs. No reply is needed to cache element 0.
- **Learning.** The chain beyond the first door is learned **only from a matched reply** (§8.7,
  §9.2). A reply that matches no pending request teaches nothing.
- **Fallback.** A chain-spelled request answered `tr::path::not_found` MUST clear the chain. When the
  request was chain-spelled, the requester re-sends it **once**, in canonical form, under the same
  pending handle. A `NOT_FOUND` to a PAIR is answered before the operation is applied
  (RFC-0029 §6 step 2), so the resend cannot apply a write twice. A `NOT_FOUND` to the canonical
  form is the answer. A stale local element 0 (the target or door was retired, so
  `deref_vertex_slot` refuses) falls back to the canonical bytes in the same way, with no frame.
- **No withdraw, no lease, no TTL** (RFC-0029 §6.3, carried). Clearing the cache is the whole
  recovery.

### 5.3 It replaces the pointer-style handle

**Normative for the reference implementation; amends RFC-0029 §4.3.** Every host-API entry point
that takes `graph::vertex_handle_t` today (`read`, `write`, the `:` field doors, `subscribe`,
`retire`) takes the path object instead. RFC-0029 §4.3 made `vertex_handle_t` the pair by value.
That pair survives as the path object's element 0, and it remains the internal currency of the walk
and of `deref_vertex_slot`. The application no longer holds a bare pair, because a bare pair has no
string to fall back to. RFC-0029 §15 clause 3 (the 11 ns priced cost of a pair dereference) carries
over as this RFC's falsifier §17 clause 1. The ISR fast door that RFC-0029 S7 leaves to the bench
stays a compile-time option on the path object.

### 5.4 Sizing and allocation

**Normative.**

- The chain cache has a **fixed capacity**, chosen by **compile-time policy** (a byte bound on the
  packed chain body). A chain that does not fit is not cached, and the canonical form is used. No
  runtime member selects the capacity.
- The path object and its chain cache are drawn from the graph's **injected allocation seam**
  (`block_source_t`, `core/include/libtracer/mem_source.hpp`; ADR-0083). Exhaustion is reported by
  value at creation (`tr::flow::backpressure`), and nothing is created.
- The chain cache is stored **inline** in the path object, so learning never allocates. A learn
  rewrites the inline chain under a sequence counter. A reader that races a learn either retries or
  uses the canonical bytes. No chain block is ever freed while a reader may hold it, so the cache
  adds no reclamation problem.

### 5.5 What the path object does not do

It holds no route-table entry, registers nothing at any hop and never touches the vertex map's keys.
A path object to a vertex on another node costs that node nothing. The pair a remote node issued
costs it nothing either (RFC-0029 §8.3: the vertex index is the table).

## 6. The host API walks into doors

### 6.1 One walk

**Normative.** A host-API `read`, `write` or `:` field operation through a path object walks the
graph from this node's root, by the same step a `FWD` arriving on a link takes (RFC-0029 §6, and
stage 1 of #1938):

1. **The walk ends at a local vertex.** Apply the operation there, with the one `allows` gate
   (RFC-0029 §6.4). The result is immediate. The asynchronous form returns a pending handle that has
   **already ended** (§10.2), and no pending record is armed.
2. **The walk reaches a door with a non-empty remainder.** Hand the remainder to the door's link as a
   `FWD{op, dst = remainder, FIELD?, src, payload?}`, with `src` either the reply path of a fresh
   pending handle (§9.1) or empty when the caller asked for no reply (§8.5). The door's `:acl` is
   evaluated by the same `allows` call as for a frame passing through.
3. **The walk misses.** `tr::path::not_found`. Nothing is created unless a creation hook on the
   parent admits it (§7).

### 6.2 No shadow vertex

**Normative.** An operation whose walk reaches a door MUST NOT create, write or read any local
vertex below that door. A local vertex that already exists below a door is unreachable by a walk
(the door hands the remainder over first). Registering one is refused (stage 2 of #1938 makes the
connection vertex the only attachment point).

### 6.3 `originate` retires

`fwd_router_t::originate`, `fwd_router_t::origin_t` and `fwd_router_t::cancel(origin_t&)`
(`core/include/libtracer/fwd_router.hpp`, `core/src/fwd_originate.cpp`) are replaced by the host API
of §6.1 and the pending handle of §9. Their behaviour is kept where it was right: the request is
built into one exactly-sized segment from the injected egress source, the router keeps no buffer,
and the caller owns the record. The `~o<hex>` one-segment route they mint becomes the token of §9.1.
The `on_reply` sink for unmatched replies retires: an unmatched reply is dropped and counted (§9.3).

### 6.4 Subscribing across a door

A host-API subscribe whose source path crosses a door is a remote `:subscribers[]` append. It is an
ordinary request, and its pending handle resolves to the entry's identity (§9.7). A subscribe whose
*target* crosses a door (`fwd_router_t::subscribe_toward`) keeps its RFC-0029 §7.2 first-fire learn.
That learn is a request too, and §8.1 gives it the same endpoint and the same rule.

## 7. Creation is refused by default

### 7.1 The rule

**Normative; amends RFC-0005 §D and its Amendment 1.** A data write whose walk misses (its target, or
an intermediate level, does not exist) MUST answer `tr::path::not_found` and MUST NOT create anything,
**whatever its origin**: the local host API, a `FWD{WRITE}` terminating here, or a branch write's
landing site (RFC-0005 §B). The `mkdir -p` behaviour is withdrawn. RFC-0005 Amendment 1's
local/remote asymmetry is withdrawn with it: both arms now give the same answer.

### 7.2 Opting in: the creation hook

**Normative.** A parent vertex MAY carry a **creation hook**, which the application installs through
the local host API only (as with application fields, it is never declarable from the wire). When a
walk misses at a child of a vertex that carries a hook:

1. the ordinary `allows` gate is evaluated at the **parent** for the `CREATE` right and the caller's
   subject. A denial is `tr::access::denied`, and the hook does not run;
2. the hook is called with the parent, the missing child's name, the caller's subject and the write's
   payload. It either creates exactly **one** child, typed and configured as the application decides,
   or refuses. A refusal is `tr::path::not_found`;
3. when the hook created the child, the walk continues from it. A deeper miss is decided by the new
   child's own hook, if it has one. So `mkdir -p` is still expressible, but only where every level
   opted in.

Whether vertices can carry a creation hook at all is a **compile-time policy**. With it off, which is
the lean default, a vertex has no hook slot and every miss refuses. The hook draws whatever it creates
from the graph's injected seam. Any bound on how many children a peer may cause is the hook's
decision. The library adds no synthetic limit (`CONTEXT.md` §Resource bound).

### 7.3 What stands

- **Owner registration.** The application creating its own vertices through the host API's
  registration calls is not write-creates and is unchanged.
- **The creator endpoint** ([ADR-0059](../../adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md)).
  A `SPEC` write to an existing creator endpoint is a write to a vertex that exists. It is unchanged,
  and it remains the typed, catalogued channel for creation from a peer. A creation hook is the
  general form of the same idea, on any parent.
- **§A bubbling.** A child a hook creates appears as its first write, bubbling to the parent's
  subscribers exactly as before (RFC-0005 §1).

### 7.4 Note on RFC-0003

The ticket names "RFC-0003/RFC-0005 write-creates". RFC-0003 (concrete-path delivery for bridged
wildcard subscriptions) is superseded and holds no write-creates text. The write-creates semantics
live in RFC-0005 §D, which is what this section amends. Nothing in RFC-0003 changes.

## 8. The reply is a remote write

### 8.1 The reply endpoint

**Normative.** A node that issues requests wanting replies MUST have exactly **one reply endpoint**:
a node-local vertex whose route the node chooses. Only the node itself reads that route. A write
whose walk ends at the reply endpoint with one further segment, the token, is a **reply write**, and
§9 decides what happens to it. The reply endpoint never emits a frame in response to any write.

The RFC-0029 §7.2 learn endpoint **is** this endpoint. The first-fire learn's tail names the edge, and
under this RFC that tail is a token like any other (§9.1). The learn is one more request, matched by
the one rule.

### 8.2 The request

A request is `FWD{op ∈ {READ, WRITE}, dst, FIELD?, src, payload?, PATH_REF_REVERSE?}` (RFC-0004 §B,
RFC-0029 §7.1), with **`src` seeded by the requester** as either:

- **its reply path** (its reply endpoint's route followed by one token), when it wants a reply; or
- the **empty `PATH`**, when it does not (§8.5).

The seed is spelled in NAMEs (RFC-0029 §6.1 stands: a return route is never spelled in PAIRs alone).

### 8.3 The reply

**Normative.** A terminus that has applied, or refused, a request whose `src` arrived non-empty MUST
answer with **one** reply write:

```
FWD (0x0F, PL=1) {
  VALUE  op = WRITE (1)
  PATH   dst              ; the request's src, as stamped on arrival here (§8.4)
  PATH   src = <empty>    ; a reply requests no reply
  <payload TLV>           ; the outcome (§8.6)
  PATH_REF_REVERSE (0x15) ; the chain back to the responder, seeded by the responder (§8.7)
}
```

The terminus sends it by **walking `dst` from its own root**, the same walk every write takes
(§6.1). The head of `dst` is the door the request arrived through (§8.4), so the walk egresses there
with no special case. RFC-0004 §B's **terminus-reply asymmetry is retired**: the terminus no longer
sends "unmodified over the link the request arrived on"; it walks an ordinary address.

There is no `kind` child and no `REPLY` opcode (§10.1). A reply write is opcode-identical to any
other `WRITE`. Only the requester can tell that it is a reply, because only the requester's reply
endpoint receives it.

### 8.4 The receiving side stamps the way back

**Normative; amends RFC-0004 §B and RFC-0029 §6.1, §6.2 and §7.1.** Every node that **receives** a
`FWD` over a link, whether a forwarding hop or the terminus, stamps the door it arrived through:

1. **`src`.** If the `src` arrived **non-empty**, the node prepends the door's canonical NAME run
   (RFC-0004 §B, RFC-0014 S2a mount run). If it arrived **empty**, the node leaves it empty (§8.5).
2. **`PATH_REF_REVERSE` (`0x15`).** If the frame carries this child, the node prepends its element
   for the door: the door's PAIR when it can issue one, the door's NAME run when it cannot, **never
   nothing** (RFC-0029 §7.1's rule, now for every frame that carries the child).

The rule is **opcode-agnostic**: no node reads `op` to decide what to stamp, and the terminus stamps
like any other receiver. It is RFC-0029 §7.1 generalised. On a subscribe request the `0x15` chain is
the way back to the subscriber, as today. On a reply write it is the way back to the responder, which
is the forward chain the requester wants to learn (§8.7). One rule produces both.

*On the wire, the reply's bytes do not grow.* The terminus's own door run is consumed by its own walk
before egress, so the `dst` on every link equals today's `REPLY` `dst` on that link.

### 8.5 An empty `src` means no reply, at every hop

**Normative; folds in #1502 and amends RFC-0004 Amendment 2.** A `FWD` whose `src` is the empty
`PATH` requests no reply, and:

- **a forwarding hop MUST NOT grow an empty `src`** (§8.4 rule 1), so the marker survives any number
  of hops;
- the terminus MUST NOT emit any frame in response. Amendment 2's clauses 2–5 (silent apply, silent
  drop of a refused write, `READ` with an empty `src` is malformed, an empty-`src` subscribe is
  malformed) stand, now at every hop count.

Amendment 2's §"Scope boundary" is resolved by this clause. It kept the marker single-hop because an
empty seed `src` was also the spelling for "name me by the link I arrive on". That spelling retires:
a reply now needs a token to match (§9.2), so a reply addressed to a bare link could never be
admitted anyway. A requester that wants a reply seeds its reply path, and a requester that does not
seeds nothing (§13, Breaking).

### 8.6 The reply's payload: the outcome

**Normative.** The payload of a reply write is exactly one TLV:

| Outcome | Payload |
| --- | --- |
| **Error** | an `ERROR` (`0x08`) TLV, top level, carrying the `tr::` identity (RFC-0002). It is not wrapped in `STATUS` (reference/05 §`0x08` already admits the unwrapped form). |
| **Result of a `READ`** | the value TLV, as `kind=RESULT` carried it before (a `POINT` for an array-whole read, RFC-0004 §D) |
| **Result of a `WRITE`** | an empty `STATUS` (4 bytes, the implicit OK of reference/05 §`0x09`), or for a `:subscribers[]` append the entry identity of §9.7 |

The requester tells the cases apart by **the payload's top-level type and the op its own pending
record holds** (§9.1). An `ERROR` at top level is an error, and anything else is the result of the
recorded op. For that to be unambiguous, an `ERROR` TLV must never be a value:

> **Normative (new).** A data write whose top-level payload TLV is `ERROR` (`0x08`) MUST be refused
> with `tr::schema::type_mismatch`, except when its walk ends at the reply endpoint.

This is ADR-0010's closed error boundary ("applications never emit a protocol error") made a gate.
It constrains no legitimate value. `STATUS` stays writable as data (RFC-0004 §D's keepalive and
liveness writes), and that is safe because a `READ`'s result is never confused with a `WRITE`'s ack
when the requester knows its own op.

### 8.7 Learning rides the reply's `0x15` child

**Normative; amends RFC-0029 §6.2.** A responder issuing a reply write MUST attach a `0x15` child
seeded with its element for the vertex it applied the request to: its PAIR, or the residual NAMEs it
resolved when it cannot issue one. Each node on the way back prepends its element for its arrival door
(§8.4 rule 2). The requester stamps its own element for the door the reply arrived through, which is
the door the request left by. At the reply endpoint the child therefore holds the **complete forward
chain, head-first, from the requester's root**, and a **matched** reply (§9.2) installs it in the
path object's chain cache (§5.2).

RFC-0029 §6.2 put the same chain in the reply's `src`. Moving it into `0x15` is what lets every node
stamp without knowing whether a `WRITE` is a reply (§8.4). The bytes are equal, apart from the 4-byte
`0x15` header: §6.2's reply `src` and this `0x15` child hold the same elements.

## 9. The request/reply matching rule

This section is the rule the maintainer asked to see stated precisely. All of its state lives at the
requester. No forwarding hop and no terminus holds anything for a `READ` or a `WRITE` (§9.6).

### 9.1 What identifies a pending request

**Normative.** A pending request is identified, **on the requester and nowhere else**, by its
**token**: the last segment of the reply path the request carried as its `src` seed (§8.2).

- The requester mints the token. It is one valid NAME segment (reference/03 §Reserved characters).
  Its bytes are opaque to every other node, and no other node may derive meaning from them.
- A token MUST name **at most one live pending request**, and once that request ends (§9.4) the token
  MUST NOT match again. The reference implementation spells it as the `(slot, generation)` of a slot
  in the requester's **pending index**, hex-encoded behind a fixed prefix (today's `~o<hex>`
  spelling, `core/src/fwd_originate.cpp`). Like a PAIR, it is minted, never hashed (ruling 1), and
  matching it is a bounds check, a generation compare and a state test, at a cost independent of how
  many requests are pending. A slot's generation advances every time its request ends, and it
  saturates and never wraps. A saturated slot is retired.
- The pending record stores, at minimum:
  - the **token**;
  - the **op** (`READ` or `WRITE`, and whether the `WRITE` is a `:subscribers[]` append);
  - the **door** the request left by, as the door's PAIR, or "local" for a request that never left
    the node;
  - the **path object**, so a matched reply can teach it its chain;
  - the **state**: pending, or ended with its outcome.

### 9.2 What a reply must carry to match

**Normative.** A write is a **matched reply** if and only if all of the following hold:

1. **Its walk ends at the requester's reply endpoint with exactly one further segment**, and that
   segment is a token naming a **pending** record (§9.1). The reply's `dst`, which the responder spelled
   from the request's `src`, is consumed hop by hop and arrives at the requester as the reply path the
   requester seeded.
2. **It arrived through the door the record holds.** The requester compares the door it stamped on
   arrival (§8.4) with the record's door. A reply to a request that never left the node is delivered in
   process and matches on the token alone.
3. **Its payload is well-formed for the recorded op** (§8.6): an `ERROR`, or a result of the recorded
   op's shape.
4. **Its `src` is empty.** A frame that asks the reply endpoint for a reply is not a reply.

A matched reply **ends** the record with outcome *answered* (§9.4) and hands the payload to the
handle. If the reply carries a `0x15` chain, the requester installs it in the path object's cache
(§8.7). The reply endpoint emits nothing.

Nothing else is checked, and nothing else needs to be. In particular, the requester does not
compare the reply's provenance with the request's `dst`. The token already names exactly one
request, and the door names the link it can come back by.

### 9.3 Unmatched, duplicate and late replies

**Normative.** A write that reaches the reply endpoint and fails any condition of §9.2 is
**dropped**:

- it is not applied, it changes no record and it teaches no chain;
- no frame is emitted in response (§8.1), because the reply endpoint never answers;
- it is **counted** in the node's introspection, under the `core/STYLE.md` §Introspection counting
  doctrine (the row's name is #1946's).

The cases this covers, each by construction rather than by a special branch:

| Case | Why it fails §9.2 |
| --- | --- |
| **Unmatched**: a token no record ever held, or one malformed as a segment | condition 1: no pending record |
| **Duplicate**: a second reply to an answered request, for example a retransmission on a link that duplicates | condition 1: the slot's generation advanced when the first reply ended it |
| **Late**: a reply after cancel, deadline, link down or teardown | condition 1: the record already ended, and its generation advanced |
| **Wrong door**: the right token, through a different link | condition 2 |
| **Wrong shape**: a `READ`-shaped answer to an append, or the reverse | condition 3 |

A late reply is never an error to anybody. The responder applied the operation, and the requester
had already stopped waiting. For a `WRITE` the requester therefore cannot know whether it was applied
(the classic lost-ack ambiguity). Making an operation idempotent is the application's design, and §9.7
does it for the one operation where the protocol owns the state.

### 9.4 How a pending handle ends

**Normative.** A pending record ends **exactly once**, with the first of these to occur:

| End | Who triggers it | Outcome the handle reports |
| --- | --- | --- |
| **Answer** | a matched reply (§9.2) | the result, or the `ERROR` it carried |
| **Cancel** | the application | *cancelled* |
| **App deadline** | the application, from its own clock: either its waiter's deadline passes (§10.3), or it cancels at a time it measured | *expired*. The library never decides this; it only records what the app reported. |
| **Link down** | the requester's own link to the record's door goes down (`link_down`, `remove_child` through it), or the door's generation moves (RFC-0029 §8.2 rules 1, 2 and 4) | `tr::transport::down` |
| **Teardown** | the graph or router that holds the pending index is destroyed | *torn down*. Every pending record is ended before the index's memory is released. |

A record whose request never left the node (§6.1 step 1) ends inside the call that created it and
never occupies a slot.

**Exactly once.** Ending is a single atomic state transition on the record (pending → ended), and
the slot's generation advances in the same step. Whichever trigger performs the transition owns the
outcome. A trigger that finds the record already ended does nothing. A cancel that loses to an answer
reports that it lost, as `fwd_router_t::cancel(origin_t&)` does today, so the application knows the
outcome was delivered.

**No end is a frame.** None of the five ends sends anything. Cancel does not tell the responder, and
neither does a deadline. A cancelled await's one-shot edge is cleared by the application's ordinary
write (§10.4) if it wants the edge gone before it fires.

### 9.5 Ordering

A requester MAY have any number of requests outstanding, to any mix of vertices and ops, on one door.
Replies carry no ordering promise (reference/04 §Invariants), and none is needed: each reply names its
own token. RFC-0004 Amendment 3 clause 3's SHOULD NOT (no `AWAIT` and `READ` to one vertex
outstanding together) is withdrawn, because the condition it guarded against cannot arise.

### 9.6 Hops hold nothing

**Normative, restating RFC-0029 §9.1 for this plane.** A forwarding hop holds no record of any
request, any reply or any token. A hop that reboots mid-request still routes the reply, because the
reply's route is in the frame (§8.4). A terminus answering a `READ` or `WRITE` holds nothing after
it has emitted the reply write. The pending index is the **requester's** state for the **requester's**
own requests. Losing it (a requester reboot) ends every handle, and every later reply is unmatched
(§9.3). That changes no answer at any other node, so it is soft state in RFC-0029 §9.1's sense from
every node's point of view but the requester's, and the requester is the party that chose to wait.

The one-shot edge of §10.4 is not hop state. It is an ordinary `:subscribers[]` entry on the
producer's own vertex, installed by an ordinary request and charged like every other edge (§11).

### 9.7 A subscriber append resolves to its entry's identity (#1932)

**Normative, at the maintainer's choice (§18 Q4).** This applies the matching rule to the one
request whose idempotence the protocol, rather than the application, owns:

1. **The append's result carries the entry's identity.** A `WRITE` to `:subscribers[]` that installs
   an edge answers with a result payload of one `VALUE` holding the entry's `u32` slot index then its
   `u32` generation, little-endian (8 bytes, the same shape as a PAIR). The pending handle resolves to
   it.
2. **A duplicate append is idempotent.** An append whose `SUBSCRIBER` names the same target and the
   same `delivery_policy` as a live entry MUST NOT install a second edge. It answers that entry's
   identity. A retry after a lost reply therefore creates no second edge. The duplicate check is the
   producer's own subscriber list, so hops still hold nothing.
3. **Removal by identity.** A `FIELD` level with `index_mode = ELEMENT` MAY carry a second `VALUE`
   after `index`: a `u32` generation. A write of the empty `STATUS` to `:subscribers[N]` that carries
   a generation clears the slot only if its entry's generation matches. Otherwise it answers
   `tr::path::not_found`, so a removal can never clear an entry that reused the slot. This amends
   RFC-0004 §C's `FIELD` grammar by one optional child.

## 10. `AWAIT` and `REPLY` retire

### 10.1 On the wire

**Normative; amends RFC-0004 §B, §D and Amendment 3.**

- `FWD` `op` keeps `READ = 0` and `WRITE = 1`. **`AWAIT = 2` and `REPLY = 3` are retired**. Bits 7–6
  stay reserved and MUST be zero (RFC-0029 §5.3).
- A terminus receiving `op = 2` with a non-empty `src` answers a reply write carrying
  `ERROR{tr::schema::type_mismatch}`, which tells an old requester it is speaking to a new node. With
  an empty `src` it drops the frame. A node receiving `op = 3` drops it and counts it as malformed.
  The old `REPLY` asks for nothing, and an old requester's deadline ends its wait.
- The **`kind`** child (`REPLY` only) and the **`await_timeout`** child (`AWAIT` only) are retired with
  their opcodes. The `kDefaultAwaitTimeout` constant (`core/include/libtracer/op_resolve.hpp`)
  retires with them.

### 10.2 Async is a pending handle

**Normative for the host API.** Each operation of §6.1 has an asynchronous form that returns a
**pending handle**:

- through a door, the handle is pending until §9.4 ends it;
- on a local vertex, the handle has already ended when the call returns (§6.1 step 1). The reference
  implementation returns the result directly on that path, so the local hot path arms nothing and
  takes no slot;
- the application may ask for **no reply**. The request then goes out with an empty `src` (§8.5), and
  the handle ends at once as *sent*.

The handle's storage is either the **caller's own** (as `fwd_router_t::origin_t` is today) or drawn
from the injected seam. When the pending index has no free slot, the call answers
`tr::flow::backpressure` **by value**, before any frame is built, and nothing is sent. The index's
capacity and growth are compile-time policy (§11). On a WIDE build it grows by append-only chunks with
stable slots, which is the ADR-0063 pattern, and lock-free for the receive threads that match against
it.

### 10.3 Sync is the application waiting with its own clock

**Normative.** A synchronous form is the asynchronous form followed by a **wait on the handle**, with a
deadline the application measures on **its own clock**:

- the wait goes through an **app-supplied waiter**, a compile-time policy type with two operations:
  `block_until(deadline)`, which the application implements with its own clock and blocking primitive,
  and `wake()`, which the library calls when the handle ends;
- the library holds no timer, reads no clock and starts no thread. It never compares a deadline; the
  waiter does, and reports *expired*, which ends the record (§9.4);
- the waiter is not wired into the library's receive path. A record ends whether or not anyone waits
  on it.

ADR-0084 rejected an app-driven `expire_awaits(now)` seam because it would have made every embedder
call it, for a deadline the receiver did not need. That objection does not reach this seam. Here the
deadline belongs to the requester, the only party that waits, and an embedder that never calls a
synchronous form never supplies a waiter.

### 10.4 An await is a one-shot subscription

**Normative; amends RFC-0022 §3.A.** `delivery_policy` **bit 8** is assigned: **`one_shot`**. A
producer MUST remove an edge whose `one_shot` bit is set immediately after it emits the **first**
delivery on that edge. That delivery is an ordinary `FWD{WRITE, src = <empty>}` to the edge's target
(RFC-0004 §D). Bits 9–15 stay reserved.

**Informative: how the host API realises await.**

- **Locally:** a one-shot local subscription whose first delivery ends the pending handle.
- **Remotely:** the host API arms one pending record and sends a `:subscribers[]` append whose
  `SUBSCRIBER` targets the reply path of that record, with `one_shot` set. The append itself needs no
  reply, but it may ask for one with a second token to learn the entry identity of §9.7, which a
  cancel uses to clear the edge. The first delivery is a write to the reply path, and §9.2 matches it.
  Its payload is the value, so the recorded op for an await record is "a `READ`-shaped answer".
- **Cancel or deadline before the first change:** the record ends at the requester. If the
  application cleared the edge, nothing more happens. If it did not, the edge's eventual delivery is a
  late reply and is dropped (§9.3). Either way the producer's edge goes when it fires once, when the
  application clears it, or when its link goes down (`evict_link_edges`), as for any edge.

The waiter no longer lives at the receiver (ADR-0084), and the vertex no longer carries a
write-sequence wake for await (#1792). A remote waiter is an edge, and a local waiter is a callback.
Both machineries are deleted (§12.5).

A requester that relied on a remote `ERROR(TIMEOUT)` already got silence after RFC-0004 Amendment 3.
It now gets the same outcome from its own waiter.

## 11. The constraints, checked

| Constraint | How this RFC meets it |
| --- | --- |
| **No timers, no clock reads** | No library timer anywhere. `graph_t::await`'s timed wait is deleted. The receiver-side waiter (ADR-0084) is deleted. Deadlines are measured by the app's waiter (§10.3). The only "time" in this RFC is the app's. |
| **No library-internal buffers** | Request and reply frames come from the injected egress source and the receiving link's own source (`fwd_router_t::rx_for`), as today. The path object and the pending index come from the injected seam (§5.4, §10.2), or the pending record is the caller's own storage. Nothing is queued: a full index refuses by value. |
| **The receiver pays** | A terminus answering a `READ` or `WRITE` holds nothing afterwards. A one-shot edge installed by a remote append is charged to the link it arrived on and evicted with that link, which is ADR-0084's decision 2 carried into an ordinary edge. Creation from a peer draws from the graph's seam under the parent owner's hook, which owns its bound. |
| **Compile-time by default** | Chain capacity (§5.4), pending-index capacity and growth (§10.2), creation-hook availability (§7.2), the waiter type (§10.3) and the ISR fast door (§5.3) are compile-time policies. No new runtime knob. |
| **Delete, do not split** | Deleted: the `REPLY` arm and its terminus asymmetry, the `AWAIT` arm, the receiver waiter, `originate`/`origin_t`, suffix pairing, the `on_reply` sink, the write-create walk, and the reply-`src` rewrite at hops (one stamping rule replaces two). Added: the pending index and the hook slot. The per-file CCN totals are the judge (#1790, the CCN ratchet). |
| **Population-independent lookups** | A reply matches in O(1) (token = slot and generation). A path object's local element is a pair dereference. The door walk is stage 1's. |

## 12. Normative pages that change

### 12.1 `docs/spec/v1.md`

- **§1 Scope.** "The data API semantics (`read` / `write` / `await` …)" becomes `read` / `write` plus
  the `:` field surface. `await` is a host-API form over a one-shot subscription (§10.4), not a wire
  operation.
- **§2 Terminology.** The **path handle** entry gains: "A path object (RFC-0030 §5) is a path handle
  that also carries a soft chain cache; the chain is never the handle's bytes."
- **§3, the `05-protocol-tlvs.md` incorporation bullet, its `0x06` routing semantics** (RFC-0029):
  - "a host relaying or issuing a `REPLY` prepends to its `src` …" is replaced by §8.4: every
    receiving node, terminus included, prepends its door's NAME run to a non-empty `src` and its PAIR
    or NAME run to a present `0x15` child, and never grows an empty `src`;
  - the first-fire learn's endpoint becomes the node's reply endpoint (§8.1).
- **§3, the `0x0F` incorporation** (RFC-0004): `op ∈ {READ, WRITE}`; the reply is a `WRITE` to the
  reply path (§8.3); an empty `src` requests no reply at every hop (§8.5); the matching rule of §9 is
  incorporated as normative requester behaviour; a data write of a top-level `ERROR` is refused
  (§8.6).
- **§3.1.4.** "The corresponding read and await entry points" becomes "the corresponding read entry
  point and the asynchronous forms (RFC-0030 §10.2), for an operation that does not cross a door". An
  operation that crosses a door draws its frame from the injected egress source, which is dispatch.

### 12.2 `docs/reference/05-protocol-tlvs.md` (normative annex)

- **§`0x04` SUBSCRIBER:** `delivery_policy` bit 8 `one_shot` (§10.4). The append's result carries the
  entry identity, and appends are idempotent (§9.7, if adopted).
- **§`0x06` §path element PAIR, routing semantics:** the per-hop algorithm's `REPLY` sentence goes,
  because a reply is a `WRITE`. §learning moves from the reply `src` to the reply's `0x15` child (§8.7),
  and the stamping rule is §8.4.
- **§`0x08` ERROR:** "never a value", with the write gate of §8.6.
- **§`0x0F` FWD:** the child table loses `kind` and `await_timeout`, `op` loses `AWAIT` and `REPLY`, the
  reply frame is §8.3, the empty-`src` marker is multi-hop (§8.5), and the reply-correlation paragraph
  ("per-hop multiplexing of a reply to a specific request remains the transport's concern") becomes
  §9's token rule.
- **§`0x10` FIELD:** the optional generation `VALUE` on an `ELEMENT` level (§9.7, if adopted).
- **§`0x15` PATH_REF_REVERSE:** "the chain back to the sender, stamped by every receiving node".
  It appears on subscribe requests (RFC-0029 §7.1) and on every reply write (§8.7).

### 12.3 `docs/reference/03-addressing.md` §path syntax (normative annex)

Unchanged in grammar. A token is an ordinary segment and needs no reserved character. One
informative sentence names the reply path as an ordinary path.

### 12.4 Informative pages, records and the glossary

- `docs/reference/04-communication-flows.md`: §Invariants' suffix-correlation paragraph is replaced
  by §9, and the delivery section's first-fire learn names the reply endpoint.
- `docs/reference/02-graph-model.md` §subtree subscriptions, branch writes and write-creates: refused
  by default, with the creation hook (§7).
- `docs/reference/18-composition-over-the-network.md` §failure modes: a lost reply is a pending handle
  ended by the app's deadline.
- `docs/modules/graph.md`, `docs/modules/fwd-router.md`: the path object, the pending index, and the
  walk into doors. `originate` and await are removed.
- `CONTEXT.md`:
  - **read / write / await** becomes **read / write**, with await as a host-API form over a one-shot
    subscription;
  - **Path-as-route**'s _Avoid_ "a reply correlation-id" is kept, with the clarification that the
    token is a path segment, not a field;
  - **Write-creates** becomes **Creation (refused by default, opt-in hook)**;
  - **Transport vertex / connection vertex** gains "a door";
  - new entries for **Path object**, **Pending handle**, **Reply path / reply endpoint** and
    **Creation hook** (§4).
- **Status rows at acceptance:** RFC-0004 (amended by RFC-0030: §B, §C, §D, §H, Amendments 2–3);
  RFC-0005 (§D amended); RFC-0029 (§4.3, §6.1, §6.2, §7.1, §7.2 amended); RFC-0022 (bit 8 assigned);
  ADR-0084 superseded; ADR-0006 superseded in part.

### 12.5 Records this RFC contradicts, said explicitly

Under `CLAUDE.md`'s precedence rule, each of these is named rather than overridden silently:

- **RFC-0004 §D**: "there is no end-to-end correlation-id … matching it to a specific outstanding
  request at that endpoint is the transport's concern". After this RFC there is still no correlation
  **field**, but matching is the **requester's** concern, done by a path segment it minted. A transport
  stream tag is no longer needed.
- **ADR-0084**: the receiver-side waiter, its rx-source block and its `on_await_defer` sink
  (`op_resolver_t::on_await_defer`, `core/include/libtracer/op_resolve.hpp`) are superseded by
  §10.4's edge. Its decision 1a (the requester owns the deadline) is carried. It was the step toward
  this RFC.
- **ADR-0006**: `await` leaves the verb set on the wire. The "no connect/subscribe; radical
  minimalism" thesis is strengthened: the wire now has two ops.
- **RFC-0005 Amendment 1**: its stated asymmetry ("the local host API keeps write-creating … the
  asymmetry is the ruling") is reversed by ruling 6.
- **RFC-0013 §7**: its quoted split ("a plain data write still creates stored-value vertices")
  no longer holds. RFC-0013 is already superseded.
- **reference/04 §Invariants**: suffix correlation with oldest-first fallback is replaced.

## 13. Breaking surface

Every row is breaking. No compatibility shim is owed (ruling 10 of RFC-0029).

| # | Surface | Was | Becomes |
| --- | --- | --- | --- |
| B1 | `FWD` `op` | `READ`, `WRITE`, `AWAIT`, `REPLY` | `READ`, `WRITE`. Ops 2 and 3 are refused or dropped (§10.1). |
| B2 | Reply frame | `FWD{REPLY, dst, src = responder, kind, payload}` | `FWD{WRITE, dst = reply path, src = <empty>, payload, 0x15}` (§8.3) |
| B3 | Error payload | `kind = ERROR` + `STATUS{ERROR}` | a top-level `ERROR` (§8.6) |
| B4 | `await_timeout` child | requester hint | removed |
| B5 | Stamping | hops grow `src`; the terminus does not; replies rewrite `src` at hops | every receiving node stamps its door into a non-empty `src` and a present `0x15` (§8.4) |
| B6 | Empty `src` across a hop | grown into "name me by the link" | preserved: no reply, at every hop (§8.5) |
| B7 | Learning carrier | the reply's `src` (RFC-0029 §6.2) | the reply's `0x15` child (§8.7) |
| B8 | `ERROR` as a data value | accepted | refused `tr::schema::type_mismatch` (§8.6) |
| B9 | `delivery_policy` bit 8 | reserved, ignored | `one_shot` (§10.4) |
| B10 | Write to a missing vertex | local creates `mkdir -p`; remote `not_found` | `not_found` everywhere unless the parent's hook creates (§7) |
| B11 | Append result (if §9.7 adopted) | empty | entry identity; duplicate appends idempotent; `FIELD` generation child |
| B12 | Host API (reference implementation) | `vertex_handle_t` arguments; `graph_t::await`; `originate`/`origin_t`/`cancel(origin_t&)`/`on_reply`; `path_t` label and binding slots | path object arguments; async forms returning a pending handle; sync forms taking a waiter; creation-hook installer |

Each implementation slice carries its own `CHANGELOG.md` entry under **Breaking** (`core/`,
`bindings/rust/`, `bindings/typescript/`) for the rows it lands. This RFC is not itself a public API
change.

## 14. Migration for bindings and embedders

- **C++ embedders.**
  - Replace `vertex_handle_t` lookups with path objects created once at init (v1 §3.1.3 registration
    still applies).
  - Replace `graph_t::await(v, timeout)` with the async await and either a callback or a sync wait
    with your own waiter.
  - Replace `fwd_router_t::originate` with an ordinary `read`/`write` through a path object below the
    door.
  - Code that relied on a local write creating its target must register the vertex first, or install
    a creation hook on the parent.
- **Rust binding** (`bindings/rust/src/fwd.rs`): delete the `AWAIT` and `REPLY` op constants, the
  `fwd_kind` discriminant and `await_timeout_ns`. Add the reply-write builder (§8.3) and the `0x15`
  child on replies. Correlate by token rather than by suffix.
- **TypeScript client** (`bindings/typescript/packages/client`):
  - `LibtracerClient` keeps its `replyEndpoint` option. Each request now seeds
    `replyEndpoint + token` rather than the bare endpoint, and the FIFO `Pending` list keyed by `dst`
    becomes a token map.
  - `await_` becomes a one-shot subscribe returning the same `Promise<Tlv>`.
  - The deadline comes from the caller as an `AbortSignal`, which is the application's clock. The
    client's own `requestTimeoutMs` timer is removed, so the binding holds no timer. A caller that wants
    the old behaviour passes `AbortSignal.timeout(ms)`.
- **Examples** that relied on the empty-seed "name me by the link" reply (RFC-0004 Amendment 2 §Scope
  boundary) seed a reply path instead.
- **Conformance runner and vectors**: §15.2.

## 15. Slices and conformance

### 15.1 Slices (the stage-4 tickets of #1938)

| Ticket | Contents | Gate |
| --- | --- | --- |
| [#1943](https://github.com/avatarsd-llc/libtracer/issues/1943) **path object** | §5: graph-owned object, canonical bytes plus inline chain, seam-drawn, compile-time capacity; host API entry points take it; `path_t`'s label and binding slots fold in. Blocked by #1941 (stage 3) and this RFC. | local read/write A/B against today's handle: §17 clause 1 |
| [#1944](https://github.com/avatarsd-llc/libtracer/issues/1944) **host API into doors** | §6: the walk hands the remainder to the door; no shadow vertex; `originate` deleted. | host-API seam tests of #1938 (write below a door reaches the remote, no shadow) |
| [#1945](https://github.com/avatarsd-llc/libtracer/issues/1945) **creation refused by default** | §7: delete the write-create walk (`graph_t::ensure_vertex_ptr`'s create arm), add the compile-time hook policy and installer, branch-write admission refuses a miss. Independent of #1943. | refused creation and opt-in creation tests; `graph_seam_refusal_test` extended |
| [#1946](https://github.com/avatarsd-llc/libtracer/issues/1946) **reply as a remote write, AWAIT/REPLY retired** | §8–§10: reply endpoint, tokens, pending index, matching, stamping rule, `0x15` learning, empty-`src` multi-hop, op retirement, `one_shot`, waiter seam; delete the ADR-0084 machinery and the await wake; bindings (§14); §9.7 if adopted. | §15.2 vectors; the matching tests below; the `reply-spread` bench row inside the A/A band |

#1946's matching tests are each asserted on the wire and at the handle:

- answer;
- unmatched token;
- duplicate reply;
- late reply after cancel, after the app deadline and after link down;
- wrong door;
- wrong shape;
- exactly-once under a cancel/answer race;
- pending-index exhaustion answered by value;
- a requester reboot leaves every hop and the terminus unaffected.

Each fix-shaped test is shown to fail with its change ablated (#1938 §Testing).

### 15.2 Conformance vectors

**Retired:** `fwd-await-timeout`, `fwd-reply-result` and `fwd-reply-error` in their `op = REPLY` form,
and empty-`src` vector (d) (`AWAIT`) of RFC-0004 Amendment 2.

**Re-authored:** `fwd-reply-result`, `fwd-reply-error` as reply writes (§8.3). Their `dst` bytes are
unchanged (§8.4), `src` is empty, the payload is the value or a top-level `ERROR`, and the `0x15` child
is added.

**New** (names indicative):

- `fwd/op-await-retired`, `fwd/op-reply-retired`;
- `fwd/empty-src-preserved-across-hop`;
- `fwd/terminus-stamps-arrival-door`;
- `fwd/reply-0x15-forward-chain`;
- `fwd/write-error-value-refused`;
- `subscriber/policy-one-shot`;
- with §9.7: `subscriber/append-returns-identity`, `subscriber/append-idempotent`,
  `field/element-generation`.

The matching rule itself is requester behaviour with no wire bytes of its own. It is tested in the S
set, not as a byte vector.

## 16. Alternatives considered

- **Keep `REPLY` and add a correlation-id child.** This is the RPC envelope RFC-0004 rejected, and it
  keeps a fourth op, its asymmetry and a matching field that every node must carry. A token in the
  reply path is a correlation id that only the requester reads, spelled in the addressing the frame
  already has. Rejected.
- **Make the reply a `WRITE` but mark it with a reserved `op` bit** (so hops could keep RFC-0029
  §6.2's reply-`src` rewrite). That is `REPLY` by another name, and every hop still branches on it.
  Moving learning into `0x15` (§8.7) removes the branch instead. Rejected.
- **Match by suffix and op** (RFC-0004 Amendment 3's implied fix). It needs the op on the wire,
  still falls back to arrival order between identical requests, and costs a scan. Rejected for the
  O(1) token.
- **Match on the token alone, without the door check.** Simpler by one compare. But the door is
  already known at both ends and costs nothing to compare, and a reply that arrives through a link its
  request never left by is wrong by construction. Rejected.
- **Keep the terminus-reply asymmetry** (the terminus does not stamp). The terminus would then be the
  one receiver that does not stamp, the reply would not be an ordinary walk, and the maintainer's model
  (ruling 9) would have an exception at the last node. Rejected; see §18 Q1.
- **Keep `STATUS{ERROR}` as the error payload and forbid `STATUS` as a value.** This collides with
  RFC-0004 §D's keepalive and liveness `STATUS` writes. Forbidding `ERROR` as a value is ADR-0010
  already. Rejected.
- **A library-owned deadline with an app-driven `expire(now)` seam.** No clock read, but it makes
  every embedder call it, and it puts the library in the business of deciding *expired*. The waiter
  (§10.3) leaves the deadline wholly with the app. Rejected.
- **Keep local write-creates and refuse only below doors.** This keeps two creation rules on one
  API, and a typo still creates. Ruling 6 chose refuse-by-default everywhere.
- **The path object as a caller value type** (today's `path_t`). A reply arriving on a receive thread
  could not update the caller's copy, so learning would need a lookup from token to caller memory
  anyway. Graph-owned it is (ruling 4).

## 17. What would falsify this RFC

1. **The path object costs the local hot path more than the pair it wraps.** The local read/write
   through a path object must stay within RFC-0029 §15 clause 3's priced 11 ns over the pointer
   handle, at every payload size, including above 1 KiB, and must not break an ISR-context write
   (reference/00 claim 6). If it does, §5.3 re-opens.
2. **A reachable sequence lets a stale token match** other than through a saturated slot. The
   generation advance in the ending transition (§9.4) is the whole guard. If a second match survives
   it, the token needs another stamp.
3. **The pending index is not population-independent** in practice (match cost grows with pending
   count on the receive thread). Then §10.2's structure is wrong, not the rule.
4. **A reply's bytes grow on the wire** beyond the 4-byte `0x15` header over today's `REPLY` on the
   `reply-spread` rows. §8.4 claims they do not.
5. **The one-shot edge cannot replace remote await** on a narrow target, because the edge's cost per
   await (an edge record plus the append's frame) exceeds the waiter block it replaces by more than
   the deleted write-sequence machinery saves. That would be a measurement, and the answer would be a
   leaner edge, not a receiver waiter.

## 18. Questions for the maintainer, each with a recommendation

1. **Does the terminus stamp its own arrival door (§8.4)?** *Recommendation: yes.* It is ruling 9's
   model at the last node, it makes the reply an ordinary walk, and it deletes the terminus-reply
   asymmetry. The `dst` bytes on every link are unchanged.
2. **Does learning move from the reply's `src` to its `0x15` child (§8.7)?** *Recommendation: yes.*
   It is the only way to keep hops opcode-agnostic once `REPLY` is gone, and it reuses the one
   accumulation rule RFC-0029 §7.1 already has.
3. **An `ERROR` TLV is never a value (§8.6)?** *Recommendation: yes.* It turns ADR-0010 into a gate,
   and it is what makes the reply's outcome readable without a `kind` child.
4. **Fold #1932 into §9.7?** *Recommendation: fold items 1 and 2* (identity in the append result,
   idempotent append), which are applications of this rule. Fold item 3 (the `FIELD` generation child)
   too: it is the symmetric removal, and it touches one optional child. Strike §9.7 if #1932 should
   stay separate.
5. **Creation hook availability: compiled out by default?** *Recommendation: yes, on every profile.*
   The application turns the policy on where it wants opt-in creation (compile-time by default, lean
   default).
6. **One transparent resend on a chain `NOT_FOUND` (§5.2)?** *Recommendation: yes, exactly one,* under
   the same handle, so the path object honours "a stale cache never changes an answer" without the app
   seeing it.
7. **An old `AWAIT` with a non-empty `src`: answer `type_mismatch`, or drop?** *Recommendation: answer,*
   so an old requester learns why. `REPLY` is always dropped (§10.1).

## 19. Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the comment window is waived by default while the
project is solo-maintained, and it is not invoked here. Sustained objections and their resolution are
recorded in this section as they arrive.
