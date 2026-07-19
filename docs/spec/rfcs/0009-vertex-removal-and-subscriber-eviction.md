<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0009 — Vertex removal and subscriber eviction

| Field | Value |
| ---- | ---- |
| **RFC** | 0009 |
| **Title** | Vertex removal and subscriber eviction |
| **Status** | **accepted** (2026-07-19 — maintainer ruling; the comment window is waived on this solo-maintained spec) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-17 |
| **Comment window closes** | 2026-07-31 |
| **Tracking issue** | #423 |
| **Target spec version** | v1 |

## Summary

A libtracer graph can be built but never unbuilt. There is no vertex-removal
surface at any layer, no way to observe a child disappearing, and no lifecycle for
a `/net/<conn>` connection — so a third party that forms a device-to-device link
**cannot unmake what it made.** This RFC settles the semantics.

It proposes **retirement, not erasure**: the owner calls a local `retire()` — the
mirror of `register_vertex` — which marks the vertex **logically absent while
leaving its allocation intact**, satisfying
[ADR-0057](../../adr/0057-graph-composite-vertex-tree.md)'s pointer-stability
invariant (a bare detach would dangle every outstanding `vertex_handle_t`). It is
the shape the codebase **already ships** for subscriber slots.

**A retired path reads `tr::path::not_found` (`0x0020`) — the same answer as never
written and never existed.** Retirement is deliberately **not observable** as a
distinct state: no new registry code, no delivered marker, no notification. A peer
that cares learns by reading and finding nothing there.

Removal is initiated one of two ways, and neither is a wire remove-verb: the owner
calls `retire()` locally, or a peer **writes a request the device executes** —
`write /net/unexport NAME{<name>}` on a creator endpoint
([ADR-0059](../../adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md)),
which is an ordinary write to an ordinary vertex whose owner's logic chooses to
honour it. §A.1's rule is untouched: **no operation removes a vertex; a device
removes a vertex.**

Subscriber eviction turns out to be **already implemented and already normative**;
this RFC settles the one place the reference and the implementation disagree.

> **Rewritten 2026-07-17** after the maintainer's grill, which struck this RFC's
> original centre: a `STATUS=ERROR` delivered in place of a VALUE, carrying a new
> `tr::path::retired = 0x0023`. Both are gone — see §C, §Alternatives 5, and
> §Discussion 6 for what was cut and why. The first draft rested on a **phantom**:
> it cited [`reference/02:411`](../../reference/02-graph-model.md)'s
> STATUS-in-place-of-a-VALUE as a "ratified, in-use pattern", and it is not in use
> anywhere.

## Motivation

### The concrete need #66 deferred to has landed

[#66](https://github.com/avatarsd-llc/libtracer/issues/66) closed on 2026-07-03
with the removal question explicitly parked:

> Child-*removal* observation remains future work and is noted in RFC-0005;
> **reopen or file fresh if a concrete need lands.**

[#407](https://github.com/avatarsd-llc/libtracer/issues/407) is that need. The
d2d-hardening milestone makes a **web UI the setup edge** that forms links on
devices and departs ([reference/13](../../reference/13-network-formation.md) §2, §5);
an edge that can only ever *add* is not a control plane.

The mesh testbed ([#408](https://github.com/avatarsd-llc/libtracer/issues/408))
turned the abstract gap into three executable observations, each now a documented
xfail in `tests/testbed/README.md`:

- **Link state never falls.** `set_link_state(name, true)` is called exactly once,
  at creation (`core/src/transport_vertex.cpp`). Kill a node and its peers'
  `/net/<conn>` still reads **up**, forever.
- **There is no reconnect anywhere** — `core/include/libtracer/transport_tcp.hpp`
  states it outright: *"Reconnect is out of scope."*
- **There is no child removal, so a failed link cannot even be recreated under its
  own name.** `make_connection` rejects a duplicate NAME with `PATH_IN_USE`
  (`core/src/transport_vertex.cpp`), and nothing can release it. Recovery requires
  inventing a **new name** — which means a node's addresses drift with its failure
  history. That is the sharpest argument here: **without removal, stable-identity
  reconnection is impossible**, and identity ([#406](https://github.com/avatarsd-llc/libtracer/issues/406))
  is the milestone's keystone.

### The number, the filename, and §A.1 are already committed

RFC-0009 does not exist as a file, yet **the merged RFC-0010 cites it five times**
— including a **dangling link** and a section heading naming *"the RFC-0009 §A.1
doctrine"*:

| Where | Committed claim |
| ---- | ---- |
| [`0010:33`](0010-owner-app-fields-and-schema.md) | the filename `0009-vertex-removal-and-subscriber-eviction.md` |
| `0010:164` | heading: *"A.2 Declaration is owner-initiated and local — the RFC-0009 §A.1 doctrine"* |
| `0010:167` | *"the mirror of `register_vertex`, exactly as **RFC-0009 §A.1 makes removal owner-facing**"* |
| `0010:465` | *"the RFC-0009 §A.1 doctrine: **the graph is a projection of device state**"* |
| `0010:474` | *"same pattern as **RFC-0009's tombstone interim**"* |

[`reference/02:397`](../../reference/02-graph-model.md) corroborates: *"the
owner-initiated doctrine of **the draft vertex-removal RFC-0009**"*.

So §A.1 below is **not a novel proposal** — it is a dangling forward reference
that a sibling document already leans on, and this RFC is partly the repair of a
citation that shipped ahead of its target. §A.1 is written to say exactly what
RFC-0010 already claims it says.

> **Corrected 2026-07-17.** This section previously said §A.1 "is already
> **normative by incorporation** through a merged RFC." **That was false, twice
> over**, and it is the error this RFC most needs to not make:
>
> 1. **RFC-0010's *file* is merged; its *Status* is `in-comment`, not `accepted`.**
>    Merging a file is not accepting an RFC. **Nothing in RFC-0010 is normative**,
>    so it cannot confer normativity on anything else.
> 2. **"Normative by incorporation" is not a general-purpose phrase.** It is
>    [ADR-0007](../../adr/0007-normative-wire-format-by-incorporation.md)'s term
>    for one specific mechanism: how the **accepted spec** ([v1.md](../v1.md) §3)
>    incorporates `reference/01` and `reference/05`. One draft citing another is
>    not that.
>
> The claim also **closed a citation loop** — RFC-0009 §A.1 was justified by
> RFC-0010:164, which was justified by RFC-0009 §A.1 — laundering a dangling
> forward reference into authority neither document had. The **real** basis for
> banning a wire remove-verb is [v1.md](../v1.md) §1 and
> [ADR-0006](../../adr/0006-read-write-await-api-no-connect.md) (the
> read/write/await API admits no third verb), which is what §A.1 now rests on.
> Five citations from a sibling draft are **evidence that the doctrine is
> load-bearing and relied upon** — a good reason to write it down — but they are
> not authority. That distinction is the whole point of the comment window.

### The slot in the reference

[`reference/02:422`](../../reference/02-graph-model.md), verbatim — the question
this RFC closes:

> **Open — child removal.** Appearance and value-change are covered above, but how
> a child *removal* is delivered to a composite subscriber (a tombstone delta? a
> `STATUS=ERROR(NOT_FOUND)` at the child's path? snapshot-diff only?) is **not yet
> specified**. Because it is wire-observable behavior it is an RFC/ADR-level
> decision, tracked in #66; the `DELETE` access-mask bit gates the *right* to
> remove but does not define the *notification*.

### Why removal is genuinely harder than creation

Creation needs no notification because it is a **side effect of the data plane**:
under write-creates ([RFC-0005](0005-subtree-subscriptions.md) §D) a write to a
nonexistent path creates it, so **the write that creates *is* the notification**
([`reference/02:420`](../../reference/02-graph-model.md): *"appearance **is** the
first write"*). Removal has no such carrier. **There is no operation whose natural
byproduct is "gone."** That asymmetry — not a missing verb — is the whole problem.

## Proposed change

### A. Removal is owner-initiated and local

#### A.1 The doctrine (already incorporated via RFC-0010)

- Removal of a vertex is a **local, owner-facing host API** — the mirror of
  `register_vertex`. **The graph is a projection of device state**, and which
  vertices a device has *is* device state.
- **There is no wire operation that removes a vertex.** No `DELETE` verb is added
  to `FWD`, no removal SPEC, no `:children[N]` clear. A remote peer can write
  vertices the owner declared (RFC-0005) and create them where it holds `CREATE`;
  it can never retire one **by addressing it**. A peer manipulating the projection
  **must go through the device's own logic**.
- Reference shape (normative for the reference implementation, informative for
  others): `graph_t::retire(vertex_handle_t)` — the exact counterpart of
  `register_vertex`, callable at any time the owner chooses.

> This mirrors RFC-0010 §A.2 for field declaration, which cites this section as
> its precedent. The two RFCs state one doctrine: **structure is the device's; only
> values cross the wire.**

**A.1.1 — a peer may *ask*; the distinction is load-bearing.** The rule above bans
a **removal operation**, not a **removal outcome**. A peer writes
`/net/unexport` with `NAME{<name>}`
([ADR-0059](../../adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md)),
and the device's own logic may respond by calling `retire()`. This is **not** a
loophole, and the difference is not a word game:

| a wire remove-verb (banned) | a write the device executes (allowed) |
| ---- | ---- |
| addresses the **victim** (`retire /a/b`) | addresses a **creator endpoint** (`write /net/unexport`) |
| the runtime removes, because the frame said so | the **owner's code** removes, if it agrees |
| every vertex acquires a remove surface | only a vertex the owner *built to accept requests* has one |
| the ACL gates the runtime's obedience | the device's logic decides, ACL-gated like any write |

The test is *"who decides"*, not *"did bytes arrive"*. Under §A.1 the answer is
always **the device**. A creator endpoint is a vertex whose declared purpose is to
receive such requests — the device chose to have one, chose which names it will
honour, and can refuse any of them. A device with no creator endpoint is
untouched: every removal stays purely local, and `/net/unexport` is
`PATH_NOT_FOUND` like any other unbuilt path.

This is the shape §Discussion 1(a) of the first draft floated ("the device
executes a written request") and the maintainer ratified in the 2026-07-17 grill.
**The bytes are not settled here** — ADR-0059 deliberately specifies none, and the
`unexport` payload, its gating right, and its reply belong to the forthcoming
creator-endpoint RFC. What this section settles is only that such a write is
**consistent with §A.1**, not an exception to it.

#### A.2 The `DELETE` access bit is orphaned — and this RFC says so rather than inventing a use

`DELETE = 0x10` is allocated in the access mask
([`reference/05:596`](../../reference/05-protocol-tlvs.md)) and
[`reference/02:422`](../../reference/02-graph-model.md) says it *"gates the right
to remove"*. Under §A.1 **there is no remote removal for it to gate.** Subscriber
eviction — the one removal a peer *can* perform (§D) — has always been gated on
`WRITE`, not `DELETE`.

This RFC therefore:

- **MUST NOT** repurpose `DELETE` to gate subscriber eviction. That would be a
  silent tightening of a shipped, ACL-gated wire path: every peer currently able to
  unsubscribe holds `WRITE`, and many will not hold `DELETE`.
- **Records `DELETE` as reserved-and-unused for protocol v1**, with its meaning
  deliberately left open. It is the natural gate for a future remote-retire surface
  should §A.1 ever be revisited (§Discussion 1).

Stating the orphan explicitly is the point: an allocated-but-ungated right is a
trap for implementers, who reasonably read `reference/02:422` as promising that
setting `DELETE` does something.

### B. Retire, not erase — the tombstone is the lifetime scheme

**Erasure is not implementable today, and this RFC does not pretend otherwise.**
[ADR-0057](../../adr/0057-graph-composite-vertex-tree.md) is unambiguous:

> Vertices are **still never erased**: the insert-only invariant is what makes the
> raw `vertex_t*` inside `vertex_handle_t` — and every pointer held past a lock —
> sound for the graph's lifetime. ... **Erasure remains future work gated on a real
> lifetime scheme (refcount / epoch reclamation / tombstone) — a bare
> detach-from-parent would dangle every outstanding handle**, the same class of bug
> as the route_handle `clear_link` dangling ref fixed in #220.

Normative rules:

- **B.1** `retire(v)` MUST mark `v` **logically absent** without deallocating it or
  detaching it from its parent. Every outstanding `vertex_handle_t` to `v` remains
  **safe to dereference**; the insert-only invariant is preserved exactly.
- **B.2** A retired vertex MUST NOT be resolved by `find` (`read` / `await` /
  `:field` operations on it behave as §C describes), and MUST NOT appear in its
  parent's `:children[]` enumeration.
- **B.3** Retiring a vertex MUST retire its whole subtree. A retired vertex's
  descendants are unreachable by construction (they are addressed through it), so
  leaving them live would be a lie the enumeration could not tell.
- **B.4** `retire` MUST be idempotent: retiring an already-retired vertex succeeds
  and delivers nothing.
- **B.5** Retirement **delivers nothing and wakes nothing.** It MUST NOT deliver
  along subscription edges, MUST NOT wake `await`, and MUST NOT bump the vertex's
  write sequence. Retirement is not an `assign`: it publishes no value. A
  subscriber learns of a retirement only by addressing the path and receiving
  `tr::path::not_found` (§C).

**One constraint, one answer.** ADR-0057 demands a lifetime scheme, and retirement
is the cheapest one available: the node stays allocated, so handles stay valid,
and only its resolvability changes.

> **Changed 2026-07-17.** B.5 previously required retirement to bump the write
> sequence as an `assign`-class operation so an ordinary `propagate` sweep would
> deliver it, and this section argued that "tombstone is the storage answer and
> `STATUS` (§C) is the delivery answer, and this RFC needs both." With the §C
> delivery struck, there is nothing to propagate, and an `assign` that assigns
> nothing is a contradiction. The storage half — the only half ADR-0057 actually
> demands — stands unchanged. Note RFC-0010:474's reference to *"RFC-0009's
> tombstone interim"* now points at the storage mechanism only, not an observable
> marker.

**This shape already ships.** `vertex_t::clear_edge` retires a *subscriber slot*
exactly this way — `subs_[idx].active = false`, in place, allocation and index
untouched (`core/include/libtracer/vertex.hpp`). §B generalizes a pattern the
codebase already trusts on its hot path; it does not invent one.

**Memory is not reclaimed, and that is the honest trade.** A device that retires
and re-creates in a loop grows without bound. Bounded reclamation needs the real
lifetime scheme ADR-0057 defers (refcount / epoch reclamation) and is **explicitly
out of scope** (§Discussion 3) — but note that retirement is what makes such a
scheme *possible* later: it is the point at which a refcount could begin draining.

#### B.6 Retirement re-virginizes: it restores the invariant `unregistered ⇒ carries no state`

This is the subtle rule, and getting it wrong is a **security defect**, not a
cosmetic one. Marking `registered_ = false` is **not sufficient**. In the reference
implementation, registration (`fill` / `adopt_identity`) sets a vertex's role,
settings, and handlers but **leaves almost everything else in place**; a naive
"retire = flip the flag" therefore leaves a fully-armed vertex that is invisible to
`find` yet **still live to the ACL gate, still holds the previous owner's handlers,
and still serves the previous owner's last value**. The codebase already banks on
the opposite — a comment on the effective-ACL walk states that "*placeholder
intermediates hold empty ACE lists, so merging them is the no-op the old walk's skip
was*", and the fan-out walk relies on "*a placeholder ancestor holds no edges, so
its fan_out is the no-op*". Retirement-without-reset mints the first counterexample
to both.

**B.6.1** — `retire(v)` MUST restore `v` to the state an **unregistered placeholder**
carries: as if `fill` had never run. It MUST clear, at retire time:

- **the vertex's own ACEs** (and invalidate the cached effective-ACL merge for the
  whole subtree). This is the concrete meaning of §C's "indistinguishable from
  never-built" and §E.1's "fresh": a never-built path has no own ACEs and **inherits
  its nearest bearing ancestor's policy**. After retire, the revived path inherits
  exactly that — the parent's policy, never the retired owner's. *(Maintainer ruling
  2026-07-17: an ACL does not survive churn; the revived path inherits the parent.
  §Discussion 7.)*
- **the value seam** — `on_read` / `on_write` / `on_children` — so a revived vertex
  runs none of the previous owner's logic. Because a reader may dereference the
  handler pointer without a lock (the value seam is read on the hot path), an
  implementation MUST NOT free the old handler block under a concurrent reader; the
  ADR-0057-consistent discipline is to swap the pointer and **leak** the old block
  (bounded by retire count, the same trade §B already books).
- **the stored last-known-value and history**, so a `read` of the revived path is
  `not_found` until the new owner writes (§C.2), not the retired owner's last value.
- **the owner app-field descriptor table and its apply seam**, so the retired
  owner's declared field names are no longer a writable surface on the revived
  vertex.
- **the subscriber edge list**, with the ancestor-listener bookkeeping the ordinary
  unsubscribe path performs, so no dangling fan-out target and no inflated
  listener counts survive (this generalizes §D.3).
- **the delivery-mode membership**, so the revived vertex does not inherit the
  retired one's sweep participation.

**B.6.2** — Exactly one piece of per-vertex state MUST **survive**: the **write
sequence** (`write_seq_`). It is monotonic per address for the graph's lifetime;
resetting it would break the readiness cursors that assume it never regresses. It is
**not** wire-observable (it feeds only the local `await` predicate), so keeping it
does not re-distinguish a retired path from a never-built one on the wire — §C.4's
collapse is preserved. The vertex's allocation, its extension block, its name, and
its parent/child links also survive, by §B.1 / ADR-0057 (the block is emptied, never
freed).

**B.6.3** — The reset happens **at retire, not at revive.** Revival is an ordinary
`register_vertex` that finds an unregistered placeholder and fills it; it inherits
nothing because retire already left nothing. Resetting at retire is fail-closed: the
retired window (invisible to `find`, live to the gate) is never left holding stale
authority, and no revival path can forget to clean up.

### C. Observation — the retired path is `not_found`, indistinguishable from never-built

**C.1 — no delivery, no marker, no new code.** Retirement is **not observable as a
distinct state**. On the `live → retired` transition the vertex delivers
**nothing**: no `STATUS`, no tombstone record, no notification of any kind along
any subscription edge.

**C.2 — reads and fields.** A `read` / `await` / `:field` operation addressing a
retired vertex MUST reply exactly as one addressing a path that never existed:
`kind=ERROR` with `STATUS{ ERROR{ VALUE u16 = 0x0020 } }` — `tr::path::not_found`,
the ordinary [RFC-0004](0004-remote-operation-addressing.md) §D error shape. **No
new registry code is minted.** `0x0023` stays free.

**C.3 — enumeration.** A retired child MUST NOT appear in
`read(<parent>:children[])` (§B.2) — again exactly as a child that was never
created.

**C.4 — the collapse, stated plainly.** After this RFC, `tr::path::not_found`
carries **three** meanings that a peer cannot tell apart:

1. this path never existed;
2. this path exists but was never written (no last-known-value);
3. this path existed and was deliberately retired.

**This is the accepted cost, ruled by the maintainer on 2026-07-17.** A control
plane cannot distinguish *"this endpoint was removed"* from *"this endpoint has
not published yet"*. A UI that must tell them apart has to carry its own
expectation of what should exist — which, per
[ADR-0044](../../adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)
pt 3, is where such state already lives: the "real graph" is client logic keyed by
an identity the client chooses, and the core never dedups or remembers on the
client's behalf.

**Why the collapse won.** The first draft minted `tr::path::retired = 0x0023` and
delivered it, arguing that folding retirement into `not_found` "makes it a
three-way collision, and the third meaning is the only one a control plane must act
on." The grill rejected that on evidence:

- **The delivery rested on a phantom.** §C.1 of the first draft called
  `STATUS=ERROR` delivered in place of a VALUE a *"ratified, in-use pattern"*,
  citing [`reference/02:411`](../../reference/02-graph-model.md) — *"the same
  pattern the transport plane uses to surface `STATUS=ERROR(TRANSPORT_DOWN)`"*.
  **It is not in use.** The transport plane emits a **`VALUE`**
  (`transport_vertex.cpp` `link_state_value`); `TRANSPORT_DOWN` appears only in
  `error.hpp`'s enum and switches and **is never emitted**; and the sole
  `type_t::STATUS` emit in `core/` (`op_resolve_walk.hpp`) is the FWD error-**reply**
  wrapper, not a delivery. The pattern the draft generalised from does not exist —
  `reference/02:411` is itself a phantom, and building a new normative surface on
  it would have made a second one.
- **A distinct code is not free.** It obliges every implementation to distinguish
  states the reference cannot yet produce, and pins an error identity — which, per
  the clause-kind rule, MUST be code-pinned before acceptance. Nothing pins it.
- **Nothing has proven it load-bearing.** The collision (2) vs (3) is real but
  hypothetical: no shipped consumer distinguishes them, because no consumer can
  retire anything yet.

**Re-adding is cheap; un-adding is not.** `0x0023` stays free, and a future RFC may
mint it **gated on landed code and a real consumer that demonstrates the need**.
Shipping a code no implementation emits, into a wire format aiming at immutability,
is the expensive direction. This is the RFC-0005 posture applied to ourselves: ship
the smallest true thing.

**C.5 — no `:status` facet is introduced.** [`reference/05:556`](../../reference/05-protocol-tlvs.md)
mentions an *"asynchronous signal at `<vertex>:status`"*; **no such facet exists in
the implementation** (`graph.cpp` dispatches `subscribers`, `acl`, `children`,
`settings`, `schema`, `identity` — there is no `status`). It is another entry on
the phantom ledger, recorded here and repaired nowhere. Retirement delivery does
not depend on it — there is no retirement delivery. Whether `:status` should exist
at all is out of scope (§Discussion 5).

### D. Subscriber eviction — settle what already ships

**Eviction is not missing.** An indexed write to `:subscribers[N]` already clears
the slot (`core/src/graph.cpp` → `vertex_t::clear_edge`), gated on `WRITE`, with
RFC-0005 listener bookkeeping maintained. `reference/05` already documents the
surface. This RFC settles the two things that are genuinely unsettled.

**D.1 — the sentinel is normative; the implementation is wrong.**
[`reference/05:556`](../../reference/05-protocol-tlvs.md) says the sentinel is a
specific TLV:

> Sentinel TLV used to clear subscriber slots (**write empty STATUS** to
> `:subscribers[N]`).

**The implementation ignores the payload entirely** — *any* indexed write to
`:subscribers[N]` clears slot N. So a peer writing a `SUBSCRIBER` TLV to slot N,
plainly intending to **replace** that edge, silently **destroys** it instead. This
RFC confirms the reference and fixes the implementation:

- An indexed `:subscribers[N]` write of an **empty `STATUS`** (`09 00 00 00`, the
  4-byte smallest valid TLV) MUST clear slot N.
- An indexed write of a **`SUBSCRIBER`** MUST **replace** slot N's edge, admitted
  through the same door as an append (ADR-0049), and MUST fail
  `tr::path::not_found` if slot N is not active.
- Any other payload MUST be rejected `tr::schema::type_mismatch`.

This is **not a breaking change**: a conforming client already writes the empty
STATUS the reference specifies, and `reference/05` is normative by incorporation
([ADR-0007](../../adr/0007-normative-wire-format-by-incorporation.md)). The
implementation is the thing out of conformance. Fixing it converts a silent
data-losing surprise into a useful operation.

**D.2 — slot indices are stable across eviction.** Clearing slot N MUST NOT
renumber any other slot. `clear_edge` already guarantees this (`active = false` in
place). This is now normative rather than incidental: a peer holding index N from a
prior read must not have it silently come to mean a different edge — the classic
index-invalidation race. A cleared slot MAY be reused by a later append.

**D.3 — retirement evicts.** Retiring a vertex (§B) MUST clear every edge it holds,
after the §C delivery. Deliveries already in flight are unaffected — the
snapshot-under-lock / dispatch-outside discipline is unchanged.

**D.4 — a retired *target* does not evict its producer's edge.** An edge whose
delivery target has been retired remains registered and MUST NOT be silently
dropped: the producer has no way to learn the target's fate (delivery is a write,
and a write to a retired path is not an error the producer observes — §E.1). This
is a real dangling-edge cost, called out honestly in §Discussion 2 rather than
papered over.

### E. Interactions

**E.1 — write-creates revives a retired vertex, and this is deliberate.** RFC-0005
§D: a data write (no `:field`) to a nonexistent vertex creates it, `CREATE`-gated on
the nearest existing ancestor. A retired vertex is not resolvable (§B.2), so a data
write to its path **revives it** — a fresh, valueless vertex at the same address,
subject to the same `CREATE` gate that would have permitted creating it in the first
place. "Fresh" is load-bearing and is **exactly** what §B.6 guarantees: because
retire re-virginized the vertex, the revived one carries no ACEs, no handlers, no
value, and no subscribers of the retired owner — it inherits its parent's ACL policy
and is byte-for-byte a never-built path. Without §B.6 this sentence would be false
(the reference `fill` does not clear those), which is the class of defect —
normative text outrunning the code — this RFC exists partly to stop.

This is the coherent reading: **retirement removes the current projection; it is not
a permanent claim on the name.** A peer that could have created the vertex can
create it again — nothing is being circumvented. The owner-initiated doctrine (§A.1)
is untouched: a peer still cannot *retire*; it can only *create*, which it always
could.

The alternative — retirement as a durable tombstone that rejects revival — is
recorded in §Alternatives. It is **not** proposed, because it would make write-creates
conditional on invisible history (the same address behaving differently depending on
whether something once lived there), and because §A.1's premise cuts the other way:
if the graph is a projection of device state and the device is publishing to that
address again, the projection should say so.

**E.2 — `/net/<conn>` teardown ([#407](https://github.com/avatarsd-llc/libtracer/issues/407)).**
Retiring a connection vertex MUST, in order: retire the vertex (§B), deliver §C,
unhook the router's NAME→link entry, then close and destroy the owned transport
(joining its recv thread). The order matters: the link must stop being *routable*
before it stops being *alive*, or in-flight frames arrive at a destroyed transport.
The freed NAME becomes available for a later `:children[]` SPEC — **which is the
whole point**: stable-identity reconnection needs the name back.

Note this is the one place §A.1 bites in practice. A web UI cannot retire a
connection remotely; it can only ask the device to, through a device-provided
surface. Whether the transport plane should expose such a surface — a
device-catalog child type whose *creation* means "tear that link down", keeping the
letter of §A.1 while restoring the setup edge's ability to unmake what it makes —
is the sharpest open question here (§Discussion 1).

**E.3 — `vertex_handle_t` is unaffected.** Handles to retired vertices stay valid
and dereferenceable (§B.1). Operations through them behave per §C.

### Files an accepted RFC edits

| Path | Change |
| ---- | ---- |
| `docs/reference/02-graph-model.md` | `:422` — replace the open question with §C (answer: *snapshot-diff only; a retired path is `not_found`*); note the `DELETE` orphan at `:422`; **`:411` — strike the STATUS-in-place-of-a-VALUE claim, which nothing implements (§C.4)** |
| `docs/reference/05-protocol-tlvs.md` | STATUS §Where-it-appears: the eviction sentinel per §D.1; `DELETE` bit reserved-unused per §A.2 |
| `docs/reference/07-host-embedding.md` | connection teardown per §E.2 |
| `docs/spec/rfcs/0005-subtree-subscriptions.md` | §E: mark the deferred child-removal point resolved here |
| `core/include/libtracer/graph.hpp`, `core/src/graph.cpp` | `retire()`; `find` excludes retired; `:children[]` excludes retired; §D.1 sentinel fix |
| `core/include/libtracer/vertex.hpp` | the retired flag (beside `subs_[].active`) |
| `core/include/libtracer/transport_vertex.hpp`, `core/src/transport_vertex.cpp` | §E.2 teardown |
| `tests/conformance/vectors/v1/` | new vectors (below) |
| `core/CHANGELOG.md` | the public-API additions |

## Compatibility

**Does this break protocol-v1 implementations?** No. Every change is additive:

- **No new error code.** The first draft minted `tr::path::retired = 0x0023`; §C
  strikes it. `0x0023` stays free. A retired path answers `0x0020`
  `tr::path::not_found` — a code every peer already knows and already handles.
- **No new TLV type, no new verb, no frame-layout change, and no new delivery.**
  Retirement is observed by a read returning what an unbuilt path returns.
- An implementation with **no removal surface stays conforming** — it simply never
  retires anything, and no peer can tell the difference between a device that
  cannot retire and one that has not. Nothing here compels a device to support
  retirement.
- **The wire is byte-for-byte unchanged.** This RFC's entire normative footprint is
  *when* `0x0020` is returned — never *what* is on the wire.
- **§D.1 is a conformance *repair*, not a break.** The reference already specifies
  the empty-STATUS sentinel; the reference implementation diverges from it. A client
  written against the spec is unaffected; one written against the C++ bug (writing
  arbitrary payloads to clear) was already non-conforming. The one visible change —
  an indexed `SUBSCRIBER` write now replacing rather than destroying — is the
  behaviour a reader of `reference/05` would already have predicted.

**New or changed conformance vectors:**

| Vector | Meaning |
| ---- | ---- |
| `subscriber-clear-sentinel` | the 4-byte empty `STATUS` (`09 00 00 00`) as the §D.1 sentinel |

The three `retired` vectors of the first draft (`errors/error-path-retired`,
`fwd/fwd-reply-error-retired`, `fwd/fwd-delivery-retired`) are **withdrawn with the
code they pinned**. Retirement needs no new vector: a read of a retired path is
byte-identical to a read of a path that never existed, which existing vectors
already cover. **That a feature needs no new conformance vector is the strongest
evidence it adds nothing to the wire** — and the clearest statement of what §C
chose.

**Migration.** None, for anyone. Nothing on the wire changes. A consumer gains no
distinction it must learn, and loses none it had: today a removed vertex is
unobservable because removal does not exist; after this RFC it is unobservable
because retirement is `not_found`. The *device-facing* API gains `retire()`; the
*wire* gains nothing.

## Alternatives considered

**Option 1 — a tombstone *delta record*** (#66's first option: a typed marker for
the removed child at its concrete path). Rejected as a *delivery* mechanism: it
needs a notification envelope RFC-0005 §A explicitly refuses (*"the written TLV
as-is — no re-encoding, no path-tagging envelope"*). **The first draft also
rejected it as "a second mechanism for what `STATUS`-in-place-of-VALUE already
does (`reference/02:411`)" — that half of the argument is withdrawn**, because
nothing does that (§C.4): there was no first mechanism for it to duplicate. The
RFC-0005 envelope argument stands on its own and is sufficient. Note "tombstone"
survives in §B in its *other* sense — the storage/lifetime scheme — which is not
what option 1 meant.

**Option 2 — `STATUS=ERROR` in place of a VALUE, with a distinct
`tr::path::retired = 0x0023`.** **This was the first draft's proposal, and it is
now rejected** — see §C.4. Two reasons, either sufficient: the pattern it
generalised from is a phantom (the transport plane emits a `VALUE`, not a
`STATUS`; `TRANSPORT_DOWN` is never emitted), and a minted error identity must be
code-pinned before acceptance under the clause-kind rule, with nothing to pin it
to. Kept here, as the option that lost, because the reasoning is the most
instructive thing this RFC contains: **a draft that cites a doc claim without
grepping for its implementation will manufacture a phantom of its own.**

**Option 3 — snapshot-diff only** (#66's third option: removals observable solely by
comparing successive `:children[]` reads). Rejected as a *specified mechanism*: it
forces every consumer to poll to notice a removal, which contradicts the point of
subscriptions, and it cannot express *when* a removal happened. **In practice it is
what §C leaves consumers with** — the difference is that this RFC does not dress it
up as a notification. It is what implementations are stuck with today, and it is
why #407 exists; §C.4 records that as the accepted cost rather than pretending
otherwise.

**Reusing `tr::path::not_found` (0x0020) — #66's literal recommendation. ADOPTED
(§C.2).** The first draft rejected this on a three-way-collision argument, reading
#66's *"unless DELTA subscribers need a distinct tombstone"* as pre-authorisation
to mint one. The grill ruled the collision real but **not yet load-bearing**: no
shipped consumer distinguishes the three meanings, because no consumer can retire
anything yet. `0x0023` stays free and may be minted later, **gated on landed code
and a consumer that demonstrates the need**. Shipping an error identity no
implementation emits, into a wire format aiming at immutability, is the expensive
direction.

**A wire remove-verb** (`FWD{op=DELETE}` or a removal SPEC). Foreclosed by §A.1 —
whose basis is [v1.md](../v1.md) §1 and
[ADR-0006](../../adr/0006-read-write-await-api-no-connect.md) (the read/write/await
API admits no third verb), **not** RFC-0010, which is `in-comment` and confers no
normativity (see the correction under §Motivation). Not reopened here. Note §A.1.1:
a peer may *write a request* the device executes, which is not a remove-verb.

**Real erasure with refcounting or epoch reclamation.** Deferred by
[ADR-0057](../../adr/0057-graph-composite-vertex-tree.md) and still deferred: it is a
substantial memory-model change (`vertex_handle_t` holds a raw `vertex_t*`) and
orthogonal to the *semantics* this RFC settles. Retirement is a prerequisite for it,
not a competitor: erasure needs a point at which a vertex stops being reachable, and
that point is `retire()`.

**Repurposing `DELETE` to gate eviction.** Rejected in §A.2 — a silent tightening of
a shipped ACL-gated path.

## Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the tracking issue (#423) stays
open at least 14 days for implementer feedback before this document is merged.
Record sustained objections and their resolution here.

Open points the author wants comment on:

1. **Does §A.1 leave the setup edge unable to do its job?** (§E.2) ✅ **RESOLVED
   2026-07-17 — the maintainer rules (a), and it is now §A.1.1.** A web UI can form
   a d2d link remotely but could not tear one down remotely — a control plane with
   a missing half. Three readings were offered: (a) accept §A.1, and expose a
   device-side surface — *"a device-catalog child type whose creation means 'retire
   that link'"* — keeping §A.1's letter while restoring the capability; (b) treat
   `/net/<conn>` as special, since a connection is infrastructure rather than
   device state; (c) revisit §A.1 for a `DELETE`-gated remote retire.

   **(a) is ratified**, and [ADR-0059](../../adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md)
   is its concrete form: the device-side surface is a **creator endpoint vertex**,
   and the request is `write /net/unexport NAME{<name>}` — the setup edge gets its
   missing half, and §A.1 keeps not just its letter but its meaning, because the
   **device still decides** (§A.1.1's table). (b) was rejected as a special case
   with no principle behind it — `/net/<conn>` is device state like any other, per
   [ADR-0027](../../adr/0027-transport-and-connections-are-vertices.md). (c) was
   rejected because it would have forced RFC-0010 §A.2 to be revisited with it, and
   because it inverts *who decides* — the thing §A.1 exists to protect.

2. **Dangling edges to retired targets** (§D.4). A producer keeps fanning out to a
   retired target forever, wasting a delivery per write. Fixing it needs the
   *target* to tell the *producer*, i.e. a back-channel the protocol does not have
   (delivery is a one-way write). Options: leave it (proposed); let the delivering
   side observe `not_found` on the return path and self-evict; or give SUBSCRIBER a
   lease/TTL (a much larger change). **The self-evict option got worse under §C**
   and this discussion point should be re-read in that light: it was already
   asymmetric and remote-only, but with the §C.4 collapse a `not_found` on the
   return path no longer means "the target was retired" — it also means "the target
   exists and has never been written." Self-evicting on it would tear down live
   edges to not-yet-published targets. **Leave it** now looks less like the lazy
   option and more like the only correct one short of a lease.
3. **Unbounded growth under retire/create churn** (§B). Accepted for v1; a device
   that churns addresses will grow. Is that acceptable for the MCU class, or does
   retirement need reclamation *before* it is useful there?
4. **`disposition` is being read in a second sense** (§C.2). The registry column
   means "retry this request"; a delivery is not a request. Should RFC-0002 grow a
   delivery-disposition concept, or is the reuse fine?
5. **Should `:status` exist?** (§C.5) `reference/05:556` promises a facet that does
   not exist. This RFC routes around it; someone should either implement it or delete
   the claim. Tracked separately.
6. **`retire` on a placeholder.** ADR-0057 has structural placeholder parents (a
   vertex registered only because a descendant was — cf. `has_first_level_child`,
   #373). Retiring a placeholder is meaningless on its own but §B.3 makes retiring
   its parent retire it. Should retiring the *last live descendant* of a placeholder
   also retire the placeholder? Proposed: **no** — placeholders are structure, not
   state, and reaping them is a reclamation question (point 3).
7. **Does an ACL survive retire/revive?** (§B.6, the sharp case of
   [#407](https://github.com/avatarsd-llc/libtracer/issues/407)) ✅ **RESOLVED
   2026-07-17 — the maintainer rules it does NOT: the revived path inherits its
   parent's policy.** A design panel confirmed against the code that the two readings
   are genuinely lossy in opposite directions, and that the code cannot tell them
   apart (ACEs carry no provenance; there is no path-keyed policy store):
   - **Clear own ACEs (ruled):** the revived vertex has no own ACEs, so the
     effective-ACL bearer walk climbs to the nearest live ancestor — it inherits the
     *parent's* policy, exactly as a never-built path does. This is what §C's
     "indistinguishable from never-built" and §E.1's "fresh" already imply; §B.6 now
     states it. **Cost, recorded:** an operator who set a leaf ACL intending it as
     durable *path* policy (whatever occupies this address is technician-serviceable)
     loses it on churn, because clearing is the maximally-permissive reset (no own
     ACE ⇒ fall back to the ancestor).
   - **Rejected:** ACL-survives-churn. It contradicts §C, and it would need a **new
     mechanism outside the vertex and outside this RFC** — a path-keyed policy store,
     and ACE provenance to distinguish an operator's address-policy from a delegate's
     self-grant. Neither exists. If a fleet genuinely needs durable path policy, that
     is its own future RFC, not an inference from this one.

   The confused-deputy this avoids: without the reset, retiring peer X's `/net/b`
   and letting peer Y re-create it would leave Y's connection governed by X's stale
   ACEs — Y's write authorized against `/net`'s policy but enforced under X's. §B.6
   closes it.
