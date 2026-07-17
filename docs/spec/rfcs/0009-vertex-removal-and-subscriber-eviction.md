<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0009 — Vertex removal and subscriber eviction

| Field | Value |
| ---- | ---- |
| **RFC** | 0009 |
| **Title** | Vertex removal and subscriber eviction |
| **Status** | **in-comment** (window opened 2026-07-17) |
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
leaving its allocation intact**. That single choice discharges two independent
constraints at once ([ADR-0057](../../adr/0057-graph-composite-vertex-tree.md)'s
pointer-stability invariant and [RFC-0008](0008-vertex-operations-assign-propagate.md)'s
operation-shaped-change doctrine), and it is the shape the codebase **already ships**
for subscriber slots. Retirement is observed the way every other fault is: a
`STATUS=ERROR` delivered in place of a VALUE at the retired path — [#66](https://github.com/avatarsd-llc/libtracer/issues/66)'s
recommended option — but carrying a **new registry code `tr::path::retired`**
rather than reusing `not_found`, because reusing it would make "deliberately
removed", "never written" and "never existed" a single indistinguishable answer.

Subscriber eviction turns out to be **already implemented and already normative**;
this RFC settles the one place the reference and the implementation disagree.

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

So §A.1 below is **not a proposal** — it is already normative by incorporation
through a merged RFC, and this document is partly a repair of a forward reference
that shipped ahead of its target. §A.1 is written to say exactly what RFC-0010
already claims it says.

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
  it can never retire one. A peer manipulating the projection **must go through
  the device's own logic**.
- Reference shape (normative for the reference implementation, informative for
  others): `graph_t::retire(vertex_handle_t)` — the exact counterpart of
  `register_vertex`, callable at any time the owner chooses.

> This mirrors RFC-0010 §A.2 for field declaration, which cites this section as
> its precedent. The two RFCs state one doctrine: **structure is the device's; only
> values cross the wire.**

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
  and delivers nothing (§C fires once, on the transition).
- **B.5** Retirement MUST bump the vertex's **write sequence** — it is an
  `assign`-class operation in RFC-0008's model, so an ordinary `propagate` sweep
  delivers it. **Removal is a thing the owner *does*, never a state the runtime
  infers by comparison**, which is precisely the doctrine RFC-0008 established:
  *"What 'changed' means is decided by which operations the caller performed, never
  by comparing bytes."*

**Two independent constraints, one answer.** ADR-0057 demands a lifetime scheme;
RFC-0008 demands an operation-shaped change. A tombstone is simultaneously the
cheapest lifetime scheme (the node stays allocated, so handles stay valid; only its
observable state changes) and a natural `assign`. They are not competing designs —
**"tombstone" is the storage answer and `STATUS` (§C) is the delivery answer**, and
this RFC needs both. RFC-0010:474 already anticipated *"RFC-0009's tombstone
interim"*.

**This shape already ships.** `vertex_t::clear_edge` retires a *subscriber slot*
exactly this way — `subs_[idx].active = false`, in place, allocation and index
untouched (`core/include/libtracer/vertex.hpp`). §B generalizes a pattern the
codebase already trusts on its hot path; it does not invent one.

**Memory is not reclaimed, and that is the honest trade.** A device that retires
and re-creates in a loop grows without bound. Bounded reclamation needs the real
lifetime scheme ADR-0057 defers (refcount / epoch reclamation) and is **explicitly
out of scope** (§Discussion 3) — but note that retirement is what makes such a
scheme *possible* later: it is the point at which a refcount could begin draining.

### C. Observation — `STATUS=ERROR` at the retired path, with a distinct code

**C.1 — the delivery.** On the `live → retired` transition the vertex MUST deliver
a `STATUS` TLV carrying one `ERROR` child, **in place of a VALUE, at the retired
vertex's own path**, along every subscription edge that observes it — its own
subscribers and, by RFC-0005 vertical bubbling, every ancestor's subscribers. This
is [#66](https://github.com/avatarsd-llc/libtracer/issues/66)'s **option 2**.

It reuses a **ratified, in-use pattern** rather than inventing one
([`reference/02:411`](../../reference/02-graph-model.md), settled by #67):

> **Invalid / fault** ... is a **`STATUS=ERROR(<reason>)` delivered in place of a
> VALUE** — the same pattern the transport plane uses to surface
> `STATUS=ERROR(TRANSPORT_DOWN)`. ... a type-aware consumer distinguishes a
> `STATUS` (type `0x09`) from a `VALUE` (type `0x01`) by its type code and reacts.

It also fits RFC-0005 §A's delivery contract without amendment. That contract is
strict — *"the delivered payload is the **written TLV as-is** — no re-encoding, no
path-tagging envelope"* — and **a `STATUS` is a TLV a producer can write.** A
tombstone *delta record* (option 1) could not ride that edge without inventing a
notification envelope RFC-0005 deliberately does not have.

**C.2 — a new registry code, `tr::path::retired = 0x0023`** (`tr::path::*` occupies
`0x0020`–`0x0022`; `0x0023` is free):

| Code | Path | Severity | Disposition |
| ---- | ---- | ---- | ---- |
| `0x0023` | `tr::path::retired` | warn | permanent |

This is the one place this RFC departs from #66's recommendation, which said
`STATUS=ERROR(NOT_FOUND)` — *"reuses STATUS fault delivery, no new type"* — while
conditioning it on *"unless DELTA subscribers need a distinct tombstone."* They do,
for a reason #66 could not have seen at the time:

**`tr::path::not_found` is already overloaded twice over.** `status.hpp` reads it
as *"Path doesn't resolve / **no last-known-value**"*, and a freshly-registered
never-written vertex returns it (`core/tests/graph_test.cpp`). So on the wire
today, **"registered but never written" and "never existed" are already
indistinguishable** — both `0x0020`. Folding "deliberately retired" into the same
code makes it a **three-way collision**, and the third meaning is the only one a
control plane must act on: a UI that cannot distinguish *"this endpoint was
removed"* from *"this endpoint has not published yet"* will either flap on startup
or never notice a removal. A distinct code costs one registry row; the collision
costs the feature.

Note the disposition subtlety this exposes: `disposition` was designed for *"should
the caller retry **this request**"* (RFC-0002 §D), but a delivery is not a request.
`retired` is `permanent` in the sense `not_found` is — *do not retry this address*
— and unlike `transport::down` (`transient`, "come back later"), a retirement is
not a promise of return. §Discussion 4 records that the registry's disposition
column is being read in a second sense here.

**C.3 — reads and fields.** A `read` / `await` / `:field` operation addressing a
retired vertex MUST reply `kind=ERROR` with `STATUS{ ERROR{ VALUE u16 = 0x0023 } }`
— the ordinary RFC-0004 §D error shape, with `retired` where `not_found` would
otherwise appear. An operation addressing a path that never existed keeps
`0x0020`, unchanged.

**C.4 — enumeration.** A retired child MUST NOT appear in `read(<parent>:children[])`
(§B.2). The read-snapshot-then-subscribe pattern
([`reference/02:421`](../../reference/02-graph-model.md)) therefore stays coherent:
the snapshot omits it and the delta explains it.

**C.5 — no `:status` facet is introduced.** [`reference/05:556`](../../reference/05-protocol-tlvs.md)
mentions an *"asynchronous signal at `<vertex>:status`"*; **no such facet exists in
the implementation** (`graph.cpp` dispatches `subscribers`, `acl`, `children`,
`settings`, `schema` — there is no `status`). Retirement delivery MUST NOT depend
on it. Whether `:status` should exist at all is out of scope here (§Discussion 5).

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
place.

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
| `docs/spec/rfcs/0002-protocol-error-model.md` | §D registry: add `0x0023 tr::path::retired` |
| `docs/reference/02-graph-model.md` | `:422` — replace the open question with §C; note the `DELETE` orphan at `:422` |
| `docs/reference/05-protocol-tlvs.md` | STATUS §Where-it-appears: the eviction sentinel per §D.1; `DELETE` bit reserved-unused per §A.2 |
| `docs/reference/07-host-embedding.md` | connection teardown per §E.2 |
| `docs/spec/rfcs/0005-subtree-subscriptions.md` | §E: mark the deferred child-removal point resolved here |
| `core/include/libtracer/error.hpp` | `err_t::PATH_RETIRED = 0x0023` + the three switches |
| `core/include/libtracer/graph.hpp`, `core/src/graph.cpp` | `retire()`; `find` excludes retired; `:children[]` excludes retired; §D.1 sentinel fix |
| `core/include/libtracer/vertex.hpp` | the retired flag (beside `subs_[].active`) |
| `core/include/libtracer/transport_vertex.hpp`, `core/src/transport_vertex.cpp` | §E.2 teardown |
| `tests/conformance/vectors/v1/` | new vectors (below) |
| `core/CHANGELOG.md` | the public-API additions |

## Compatibility

**Does this break protocol-v1 implementations?** No. Every change is additive:

- A **new error code** is additive by construction — RFC-0002 §D says *"the built-in
  set below is frozen, and additions are RFC-gated"*, which is exactly this
  procedure. A peer that does not know `0x0023` treats it as an unknown registered
  code; RFC-0002's model already requires tolerating that.
- **No new TLV type, no new verb, no frame-layout change.** `STATUS`/`ERROR` are
  unchanged and already carried on this edge.
- An implementation with **no removal surface stays conforming** — it simply never
  emits `0x0023`. Nothing here compels a device to support retirement.
- **§D.1 is a conformance *repair*, not a break.** The reference already specifies
  the empty-STATUS sentinel; the reference implementation diverges from it. A client
  written against the spec is unaffected; one written against the C++ bug (writing
  arbitrary payloads to clear) was already non-conforming. The one visible change —
  an indexed `SUBSCRIBER` write now replacing rather than destroying — is the
  behaviour a reader of `reference/05` would already have predicted.

**New or changed conformance vectors:**

| Vector | Meaning |
| ---- | ---- |
| `errors/error-path-retired` | `STATUS{ ERROR{ VALUE u16 LE = 0x0023 } }` |
| `fwd/fwd-reply-error-retired` | `FWD{ op=REPLY, kind=ERROR, STATUS{ ERROR{ 0x0023 } } }` — a read of a retired path |
| `fwd/fwd-delivery-retired` | `FWD{ op=WRITE, payload = STATUS{ ERROR{ 0x0023 } } }` — the §C delivery, a STATUS where a VALUE would be |
| `subscriber-clear-sentinel` | the 4-byte empty `STATUS` (`09 00 00 00`) as the §D.1 sentinel |

**Migration.** Nothing deployed emits or expects `0x0023`, so there is no migration
for existing devices. Consumers gain a distinction they did not have (retired vs
never-was) and may ignore it — treating `0x0023` as `0x0020` reproduces today's
behaviour exactly, which is the intended graceful degradation.

## Alternatives considered

**Option 1 — a tombstone *delta record*** (#66's first option: a typed marker for
the removed child at its concrete path). Rejected as a *delivery* mechanism: it needs
a notification envelope RFC-0005 §A explicitly refuses (*"the written TLV as-is — no
re-encoding, no path-tagging envelope"*), and it would be a second mechanism for
what `STATUS`-in-place-of-VALUE already does (`reference/02:411`) — the same
duplication argument that killed `:children_changed` in #66. Note the word
"tombstone" survives in §B in its *other* sense (the storage/lifetime scheme), which
is not what option 1 meant.

**Option 3 — snapshot-diff only** (#66's third option: removals observable solely by
comparing successive `:children[]` reads). Rejected: it forces every consumer to poll
to notice a removal, which contradicts the whole point of subscriptions, and it
cannot express *when* a removal happened. It is what implementations are stuck with
today, and it is why #407 exists.

**Reusing `tr::path::not_found` (0x0020)** — #66's literal recommendation. Rejected
on the three-way-collision argument in §C.2, which #66 itself pre-authorized:
*"unless DELTA subscribers need a distinct tombstone."*

**A wire remove-verb** (`FWD{op=DELETE}` or a removal SPEC). Foreclosed by §A.1,
which is already incorporated via merged RFC-0010 — not reopened here.

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

1. **Does §A.1 leave the setup edge unable to do its job?** (§E.2) A web UI can form
   a d2d link remotely but cannot tear one down remotely — arguably a control plane
   with a missing half. Three readings: (a) accept it, and expose a device-side
   surface (a physical button, a local rule, a device-catalog child type whose
   creation means "retire that link" — keeping §A.1's letter while restoring the
   capability); (b) treat `/net/<conn>` as special, since a connection is
   infrastructure rather than device state; (c) revisit §A.1 for a `DELETE`-gated
   remote retire — which would need RFC-0010 §A.2 revisited with it, since it cites
   this doctrine. **The author leans (a)** but this is the decision most worth an
   implementer's objection.
2. **Dangling edges to retired targets** (§D.4). A producer keeps fanning out to a
   retired target forever, wasting a delivery per write. Fixing it needs the
   *target* to tell the *producer*, i.e. a back-channel the protocol does not have
   (delivery is a one-way write). Options: leave it (proposed); let the delivering
   side observe `0x0023` on the return path and self-evict (asymmetric, and only
   works for remote edges); or give SUBSCRIBER a lease/TTL (a much larger change).
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
