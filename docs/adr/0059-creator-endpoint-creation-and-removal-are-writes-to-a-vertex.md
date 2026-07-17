# ADR-0059 — Creation and removal are writes to a *creator endpoint vertex*, not fields on the parent

Status: accepted (maintainer ratified 2026-07-17 grill).

**Supersedes the *spelling* of [ADR-0017](0017-in-band-vertex-creation-controller-orchestration.md)
§Decision** — the creation *field* (`:children[]` / `:controllers[]`) and its "the creation field's
`:schema`" catalog — while keeping that ADR's load-bearing commitments intact (no new wire primitive;
device-known types only; creation and binding separate; creation stays ACL-gated, on which right see
§Decision 6). ADR-0017 was right that creation must be in-band, ordinary, and bounded by the device's
own catalog; it was wrong only about *where the surface lives*, and that error stayed invisible
because the catalog it specified was never implemented, so nothing ever tried to address it.

**Also supersedes [ADR-0021](0021-colon-field-plane-is-the-vertex-ioctl.md) §Decision**'s "*Creation
is one such optional standard field*" paragraph and its §Decision 3 claim that the standard-field
model "*subsumes the 'device-bounded creation endpoint' alternative without leaving the protocol*" —
ADR-0021 considered this ADR's shape and rejected it; §Considered options answers that rejection on
its own terms. ADR-0021's ioctl framing, its optionality rule (`SCHEMA_NOT_FOUND` = the `ENOTTY` of
an unsupported field), and its standard-vs-device-private two-tier model are **untouched and remain
the reason the full-sysfs option is rejected below**.

**And supersedes the `:children[]` *spelling* of
[ADR-0027](0027-transport-and-connections-are-vertices.md) §Decision** (its
`write /B/net/quic:children[] += SPEC{type=client, …}` example and "*its catalog is
`{client, listener}`*"). ADR-0027's *substance* — transports and connections are `/` vertices;
distinct identity ⇒ `/` — is what §Decision 5 leans on and is untouched.

## Context

ADR-0017 decided creation is an in-band field-write of a controller-spec `{type, path, config}` into
a **creation field** on the parent, and located the type catalog at *"the creation field's
**schema** (a POINT enumerating accepted `type`s and each one's config schema)"*. Its Consequences
also promised a *"wire-driven path (create/**destroy**)"*.

Three things falsified that shape, none of which was visible from the text:

1. **The catalog was never served, and could not be addressed.** `reference/05:750` and
   [CONTEXT.md](../../CONTEXT.md) §Module set promise a runtime-queryable catalog; nothing implements
   it. `read <vertex>:children.schema` parses, but `graph.cpp` dispatches `:schema` **only** as a
   whole-vertex read (`field.steps.size() == 1`), so the address resolves to `SCHEMA_NOT_FOUND`.
   RFC-0013 ([#417](https://github.com/avatarsd-llc/libtracer/pull/417), in-comment) proposed to
   specify it and could not close its central clause — because it was faithfully specifying an
   address that does not exist.

2. **`:schema` is a *vertex* facet, not a *field* facet.** A vertex's `:schema` describes **that
   vertex's own structure** — one per vertex, never its children's (see [CONTEXT.md](../../CONTEXT.md)
   §Schema, added by the same grill and landing in this ADR's PR). "The creation field's schema" is
   therefore a **category error**: it invents a schema hanging off a field. ADR-0017 minted that
   concept and ADR-0021 §Decision entrenched it, and it propagated into `reference/05:750` and
   RFC-0013 unchallenged **because `CONTEXT.md` had no `schema` entry at all** — the vocabulary was
   never pinned, so nothing could contradict the drift.

3. **Destroy never arrived, and a field had no spelling for it.**
   [#66](https://github.com/avatarsd-llc/libtracer/issues/66) (closed) left child-removal semantics
   unsettled; [#407](https://github.com/avatarsd-llc/libtracer/issues/407) is the concrete need — a
   web UI that forms device-to-device links must be able to unmake what it makes. A `:children[N]`
   clear collides with the read-members/write-SPEC asymmetry, and a wire *remove verb* is forbidden
   outright by [docs/spec/v1.md](../spec/v1.md) §1 (*"there are no `connect`/`disconnect`/`subscribe`
   wire operations"*) and [ADR-0006](0006-read-write-await-api-no-connect.md). RFC-0009
   ([#424](https://github.com/avatarsd-llc/libtracer/pull/424), in-comment until 2026-07-31) proposes
   to restate that ban as an owner-initiated doctrine for removal specifically; **this ADR does not
   depend on that RFC's outcome — it needs only the ban already in the accepted spec.**

The maintainer's framing that opened this: creating and deleting endpoints is a **high-level
feature**, and it should work like a device driver's setup surface — a finite structure where
configuration happens through ordinary reads and writes, sysfs-style, rather than through a
special-cased field grammar.

## Decision

**A creatable parent exposes a *creator endpoint* — an ordinary vertex — and creation and removal
are ordinary writes to it.**

```
write /net/export    SPEC{ type, name, config }   ; create  -> /net/<name>, atomically
write /net/unexport  NAME{ <name> }               ; retire  -> the device removes /net/<name>
read  /net/export:schema                          ; THE CATALOG — a vertex's own schema
read  /net:children[]                             ; enumerate members
```

1. **Creation is a write to a vertex, not a field on its parent.** This stays squarely inside the
   read/write/await API, so ADR-0017's load-bearing "**no new wire primitive**" is preserved. It also
   keeps creation **atomic**: one write yields a fully-configured child (see §Considered options on
   why that is the differentiator). The spec carries `name`, not ADR-0017's `path` — the creator's
   own location already fixes the parent.

2. **The catalog is the creator endpoint's own `:schema`.** No field-level schema, no category
   error, no second **schema** addressing concept — the catalog is exactly what "this vertex
   describes its own structure" already means, applied to a vertex whose structure *is* "what I
   accept". ADR-0017's substance survives verbatim: the orchestrator **selects a type the device
   already knows**, and roles stay invisible; only the *catalog of types* is published.

3. **Removal is a write, executed by the device.** `unexport` carries a **request**; the device
   performs the retirement through its own logic. No wire remove-verb is added, so `v1.md` §1 and
   ADR-0006 hold — while the setup edge regains the ability to unmake what it makes.

4. **Retirement collapses into absence.** A retired path reads `tr::path::not_found` (`0x0020`),
   **indistinguishable from never-written** — no distinct code, no tombstone, no
   `STATUS`-in-place-of-`VALUE` carrier (see §Considered options; that carrier does not exist).
   Re-adding the distinction later requires a landed implementation first. This is the maintainer's
   **position into** RFC-0009's window (#424, closing 2026-07-31), not that window's outcome.

5. **The creator endpoint is a distinct identity, so `/` is correct.** [CONTEXT.md](../../CONTEXT.md)
   §Field-write's rule is *control facet ⇒ `:`, distinct identity ⇒ `/`*, and its long form asks for
   "*a genuinely distinct subsystem with its own lifecycle/stats/ACL*". A creator has **its own ACL,
   its own schema, and its own name**, but — unlike a connection — no lifecycle, no stats, and
   nothing to `await`. It qualifies on the ground that actually matters here: it **holds none of the
   parent's state**, so making it a `/` vertex dissolves nothing (ADR-0021's anti-dissolution
   concern). It is a *thing you write to*, not a facet of what `/net` **is**. It costs **one vertex
   per creatable parent**, never one per knob.

6. **ACL-gating relocates, and the right's identity is deferred.** ADR-0017 gated creation on the
   parent's `CREATE` right (`vertex.hpp:260`; enforced on `:children[]` in `graph.cpp`). A creator
   endpoint carries its own ACL, so the create right becomes delegable **without granting any right
   on the parent at all** — a *different* cut than `CREATE`-vs-`WRITE` on one mask, not a finer one
   (those are already distinct bits: `WRITE = 0x02`, `CREATE = 0x08`). Whether the creator's gate is
   `WRITE` — which would leave `CREATE` allocated-but-ungated for `:children[]`, a trap that must
   then be retired rather than left standing (write-creates gates on `CREATE` too and is unaffected)
   — or a `CREATE` right on the creator itself, is a **right/byte claim** deferred to the RFC under
   the clause-kind rule in §Consequences.

**What is superseded:** the creation *field* as the creation surface, and "the creation field's
`:schema`" as the catalog location, in ADR-0017 §Decision, ADR-0021 §Decision, and ADR-0027's worked
example. **What stands:** no new wire primitive; device-known catalog only; **creation and binding
remain separate steps** (a created controller still subscribes to nothing — SUBSCRIBER edges are a
distinct act); and creation remains ACL-gated.

## Considered options

- **Keep ADR-0017/0021 as written — `:children[]` creation plus a `:children.schema` catalog.**
  Rejected: the catalog address is a **category error** (§Context 2) and has never dispatched.
  Accepting it makes `schema` mean two things — a vertex facet *and* a field facet — which is the
  ambiguity RFC-0013 drifted through. It also leaves removal with no spelling.

- **Full sysfs: settings-as-virtual-files, a vertex per knob** (the maintainer's opening proposal,
  and the strongest rejected option). Rejected on three grounds:
  - **It inverts a ratified rule.** [ADR-0021](0021-colon-field-plane-is-the-vertex-ioctl.md) is
    titled "*the `:` field plane is the vertex's `ioctl`*" and rejects "*control surfaces as
    sub-vertices*"; `CONTEXT.md` §Field-write lists that under `_Avoid_`; `v1.md` §1 is normative
    that there are no `connect`/`disconnect`/`subscribe` operations. Sysfs **is** the
    control-facets-are-files model. This is the decisive objection, and it is why the adopted
    version is **narrow**: `/net/export` is not a facet of `/net`'s value.
  - **libtracer cannot collect sysfs's payoff.** Sysfs bought discoverability from a shell — `echo 17
    > export`, `cat direction` — no client library, human-inspectable, scriptable. libtracer has no
    shell and no `echo`; every access goes through a TLV codec regardless. It would pay sysfs's costs
    to buy an advantage it structurally cannot have.
  - **It breaks the 16 KB story.** `core/tests/vertex_size_test.cpp` `static_assert`s
    `sizeof(vertex_t)` against a hard ceiling; a vertex carries name, settings, subscribers, ACL. A
    file-per-setting design multiplies vertex count on exactly the target class the facet plane
    exists to serve.

  **On the GPIO precedent the proposal cited:** Linux did move GPIO from sysfs to a chardev
  (`/sys/class/gpio` is deprecated; `Documentation/ABI/obsolete/sysfs-gpio`), and the sharpest reason
  transfers directly — **atomicity**. `GPIO_V2_GET_LINE_IOCTL` configures a line in one call, where
  sysfs `export`-then-configure leaves a window in which the line is live and misconfigured;
  `write /net/export SPEC{type,name,config}` is likewise one atomic write, and that property is worth
  protecting. The rest of the analogy does **not** transfer and is not relied on here: GPIO's other
  fix was that the ioctl **returns a new fd**, and *that fd is the ownership handle* — whereas
  ADR-0021 defines the `:` plane by the opposite property ("*exactly as `ioctl` does not spawn a new
  fd*"). libtracer has no ownership handle either, and `/net/<name>` persists until someone writes
  `unexport` — which is sysfs's lifetime model, under sysfs's own spelling. The narrow option is
  adopted for the reasons above, not because it inherits GPIO's ownership fix.

- **Put the catalog inside the parent's own `<vertex>:schema`** as a `child_types` member. Rejected:
  it satisfies the vertex-only schema rule, but conflates two unrelated self-descriptions — "what I
  am" and "what I can make" — so every reader of a sensor's `:schema` parses a member that exists
  only for creatable parents, and removal still has no home.

- **A separate bare `:child_types` facet on the parent.** Rejected: it spends a protocol-minted field
  name to describe a capability a creator vertex describes for free through the schema surface that
  already exists — and again leaves removal unspelled.

- **A distinct `tr::path::retired` error code + a `STATUS`-in-place-of-`VALUE` delivery** (RFC-0009
  §C as currently proposed, [#424](https://github.com/avatarsd-llc/libtracer/pull/424), in-comment
  until 2026-07-31). Rejected: `reference/02:411` cites that delivery pattern as *"the same pattern
  the transport plane uses to surface `STATUS=ERROR(TRANSPORT_DOWN)`"* — **and it has never
  executed**. The transport plane emits a `VALUE` (`transport_vertex.cpp`'s `link_state_value`), and
  `TRANSPORT_DOWN` appears only in `error.hpp`'s enum and switches, never emitted. Choosing it would
  make vertex removal the **first real user of a doctrine nobody had implemented** — a fifth entry in
  the phantom ledger beside [#419](https://github.com/avatarsd-llc/libtracer/issues/419),
  [#420](https://github.com/avatarsd-llc/libtracer/issues/420), `reference/02:411`, and
  `reference/05:750`.

## Consequences

- **RFC-0013 is answered, and its window still decides.** Its capability — "what can I add here" — is
  served by `read <creator>:schema` with no new addressing concept, so the maintainer's position
  going into the close of [#417](https://github.com/avatarsd-llc/libtracer/pull/417) (2026-07-31) is
  *withdraw*. RFC disposition is the spec domain; this ADR records a position, not a closure. On
  withdrawal, `reference/05:750`'s "*(the `:children` field's `:schema`)*" parenthetical and
  `CONTEXT.md` §Module set's "runtime-queryable catalog" phrasing are corrected to name the creator.

- **[#407](https://github.com/avatarsd-llc/libtracer/issues/407) gets its answer.** The freed NAME
  becomes reusable, which is what stable-identity reconnection needs — today a failed link cannot be
  recreated under its own name (`PATH_IN_USE`), so a node's addresses drift with its failure history.

- **The graph substrate still never erases.** [ADR-0057](0057-graph-composite-vertex-tree.md)'s
  insert-only invariant is what makes `vertex_handle_t`'s raw `vertex_t*` sound; it states plainly
  that "*a bare detach-from-parent would dangle every outstanding handle*" and that reclamation is
  future work gated on a real lifetime scheme. Retirement therefore **cannot detach**. The shape
  proposed here — un-registering a node **back into** the unregistered/placeholder state ADR-0057
  defines for unfilled intermediates, inverting its fill-in-place step — is **new**, is *not*
  something ADR-0057 contemplates, and is a design claim for the RFC: it must demonstrate that a
  re-`register_vertex` over an un-registered node succeeds rather than returning `PATH_IN_USE` before
  the NAME-reuse promise above can be relied on.

- **A control plane cannot distinguish "removed" from "never published"** (§Decision 4). Accepted
  knowingly: a UI may flap on startup or miss a removal. Re-adding the distinction later is a
  breaking change to error identity — merged concepts are harder to split than to keep apart — so the
  cost was weighed against publishing bytes no code emits.

- **`export`/`unexport` become children of their parent**, so `read /net:children[]` returns creation
  machinery alongside members unless they are hidden from enumeration (as ADR-0057's placeholders
  already are). Which — plus the well-known names themselves, which reserve protocol-owned names
  inside the `/` tree that write-creates and the #373 shadowing guard both touch — is for the RFC.

- **This ADR decides the *shape*, not the bytes.** Under the grill's **clause-kind rule** — a clause
  that declares, forbids or reserves may lead the code; a clause asserting **bytes, error identities
  or gate order** must be pinned by code plus a conformance vector before it is accepted as normative
  text — the SPEC layout, the catalog's reply bytes, the `unexport` payload, the creator's gating
  right, and the creator's value semantics (it must be write-only and valueless: the write is
  *executed*, not *assigned*, so it neither stores nor propagates under RFC-0008) are all **byte
  claims** and are deferred. This ADR records a doctrine and supersedes three others; it deliberately
  specifies no wire layout, and no reference or spec document should assert one until it exists.
