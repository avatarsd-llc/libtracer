<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0014 — Creator endpoint: connection lifecycle and link liveness

| Field | Value |
| ---- | ---- |
| **RFC** | 0014 |
| **Title** | Creator endpoint: connection lifecycle and link liveness |
| **Status** | **accepted** (2026-07-24 — maintainer ruling; comment window waived on this solo-maintained spec, per the RFC-0009 precedent). **Amendment 1 (2026-08-21, §4.1)** replaces §4's literal "refcount → 0 → close socket" steady-state reading with MAY-shape wording: an op-woken socket with no standing binding **MAY** remain up until loss (the reference implementation does) or be closed eagerly, and only three MUSTs bind — no background retry at refcount 0, re-dormant with no retry on loss or a failed wake-dial at refcount 0, and close-plus-re-dormant on the last **standing** release. Ruled on the divergence flagged in [PR #1455](https://github.com/avatarsd-llc/libtracer/pull/1455). **Amendment 2 (2026-08-24, §5.1)** generalizes §5: the demanded right is a **payload-type → required-ACL-right table the vertex declares**, keyed on the written TLV's type only, consulted by the one write gate, which does not move — absent declaration is plain `WRITE`. The creator endpoint declares §5's own mapping and §5 is discharged. **Amendment 3 (2026-08-24, §2.1)** pins the `conn:schema` catalog envelope as the ordinary `POINT{NAME, SETTINGS{…}}` record, with an **empty `SETTINGS`** — never `SCHEMA_NOT_FOUND` — for a module that declares no catalog. **Amendment 4 (2026-08-26, §Compatibility)** EXECUTES the supersession: the `:children[]` `client`/`listener` creation registrations are removed, so the creator endpoint is the only wire creation path and the `role` config key is retired with the catalog type it overrode; and, the conformance vectors having landed, every `proposed pending` byte clause is promoted to **normative**. BREAKING for an orchestrator still using the old spelling. |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-24 |
| **Comment window closes** | waived |
| **Tracking issue** | #492 (implementation); #491 (d2d-origination follow-up) |
| **Target spec version** | v1 |

## Summary

[ADR-0059](../../adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md)
decided the *shape* — creation and removal of a connection are ordinary writes to a
**creator-endpoint vertex** — and deferred every byte-level claim under the clause-kind rule. This
RFC pins those bytes and adds the layer ADR-0059 was silent on: the **link-liveness** model
underneath a connection vertex.

Two independent lifecycles are specified:

1. **The connection *vertex*** — an explicit, **named**, persistent identity. Created by a `SPEC`
   write to a **per-module** creator endpoint — one self-contained module per *(transport, role)*
   (`ws-client`, `ws-server`, `tcp-client`, `tcp-server`, `can`, …) mounted flat under `/net` — and
   removed by a `NAME` write to the same endpoint (the two collapse onto one control, distinguished
   by payload type). The path carries the **name**, never an address; the **role is positional** (it
   *is* the module), not a config field. The routing that makes `/net/<module>/<name>` addressable is
   [ADR-0061](../../adr/0061-per-transport-mount-routing-strip-k-l5-demux.md).
2. **The *link* it names** — the live socket underneath, managed automatically: **refcount-gated**
   (for a `DIAL` link the socket closes when no binding uses it — the vertex stays) and
   **self-healing** (it re-dials on demand or after loss). There is no lazy *vertex* creation; the
   only lazy establishment is reconnection.

This unblocks the transport-link half of
[#407](https://github.com/avatarsd-llc/libtracer/issues/407) and retires the superseded
`:children[]` creation spelling that was live in `transport_vertex.cpp` when this RFC was written
(retirement executed at S7 — Amendment 4).

## Motivation

- **ADR-0059 deferred the bytes.** Its §Consequences names them: *"the SPEC layout, the catalog's
  reply bytes, the `unexport` payload, the creator's gating right, and the creator's value semantics
  … are all byte claims and are deferred"* to "the forthcoming creator-endpoint RFC." This is it.
- **The removal wire path does not exist.** `graph_t::retire()`
  ([RFC-0009](0009-vertex-removal-and-subscriber-eviction.md), merged) re-virginizes a vertex, but
  its own contract says *"there is **no wire operation** that reaches here — a peer goes through the
  creator endpoint."* That path is undefined until this RFC.
- **The consumer is blocked on it.** A downstream consumer's web UI `network-page.component` ships a
  **deferred create/delete stub** (its `endpointStubTitle` reads *"Creating/deleting an endpoint
  needs the libtracer `:children[]` op (issue #82) — deferred"*) and forms cross-device wires by
  **browser-relaying** between boards (`relay.service.ts`) rather than owner-mediated links. RFC-0014
  supplies the *link* half of un-stubbing that (see §Scope boundary for the *wire* half it does
  **not** supply). Un-stubbing tracks **#82** (the code's cited issue) and **#407**.
- **The creation path at drafting time was the superseded spelling.** `transport_vertex_t`
  registered ONE global `client`/`listener` catalog through `register_child_type` as a
  `:children[]` target on `/net`, and the `quic` module extended the *same* catalog via
  `register_transport_type` with a `kind` selector (open/closed). RFC-0014 keeps that module
  ownership but **replaces the single-global-catalog mechanism** with a per-*(transport, role)*
  module endpoint (§1); the per-module structure, the positional role, and `conn:schema`-as-catalog
  are all **new code**, not a preservation of the old seam. *(Historical as of Amendment 4: the
  global catalog registration no longer exists.)*

## Proposed change

> **Implementation status (superseded by Amendment 4 — kept as the drafting-time record).** Except
> where noted, this described **new mechanism**: at drafting, connections registered flat at
> `/net/<name>` (not `/net/<module>/<name>`), the catalog was a single global `:children[]` target,
> `:schema` was whole-vertex-only, `set_link_state` was a manual binary up/down bool, and no
> refcount / dormancy / self-heal existed. Per the clause-kind rule (see Discussion) the byte-level
> clauses here were **proposed pending** code + conformance vectors.
>
> **As of Amendment 4 (S7) all of it is SHIPPED and every byte clause is normative.** The
> `:children[]` creation target is gone, the per-module endpoint is the only creation door, and the
> clause-by-clause instrument table is in Amendment 4.

### 1. A per-module creator endpoint, designated by the transport module

> **Erratum (2026-08-01),
> [ADR-0073](../../adr/0073-naming-authority-the-application-mints-one-predicate-gates.md) §4 /
> [#621](https://github.com/avatarsd-llc/libtracer/issues/621):** this section (and the lifecycle
> summary above) says modules are "mounted flat under `/net`" and lists `ws-client`, `ws-server`, …
> as if the root and the module names were library rules. They are not, and never were normative
> here: **`/net` is the recommended root convention** (a constructor default, overridable per
> node — the normative spec never mandates it), and **every module name is application-declared**
> through `register_module` — the library derives and auto-registers none; the listed spellings are
> the built-ins' *suggested* names. The divergence was introduced by the implementation's derived
> `"<kind>-client"`/`"<kind>-server"` fallback and its builtin self-registration, both removed by
> ADR-0073 §4. Wording only: the endpoint shape, gating, and byte-level clauses of this RFC are
> unchanged, and none of them promised a derived name or a fixed root.

A creatable *(transport, role)* pair is a **self-contained module** mounted flat under `/net` —
`ws-client`, `ws-server`, `can`, … (a transport with a single shape, like `can`, is one module; a
transport with both a dial and a listen shape, like `ws`, is two). Each module exposes a
**creator-endpoint child vertex**; by convention the child name is **`conn`**:

```
/net/<module>/conn                                     ; the creator endpoint (a / vertex, not a : field)
   write SPEC{ name, config }   → create /net/<module>/<name>, atomically       (gated CREATE 0x08)
   write NAME{ <name> }         → retire /net/<module>/<name>                    (gated WRITE  0x02)
   read  :schema                → THE CATALOG: the module's accepted config      (module-defined)
/net:children[]                                        ; enumerate the modules (discovery, §6)
/net/<module>:children[]                               ; enumerate the member connection VERTICES
                                                       ; module ∈ { ws-client, ws-server, can, … }
```

- **Per-module, role- and transport-positional.** Both the transport *and* the role are fixed by the
  module in the path (`/net/ws-client/…`, `/net/ws-server/…`, `/net/can/…`), so `SPEC` carries
  `{ name, config }` with **no `type` and no `role` field** — the module already says both — and each
  module's `:schema` is its own config catalog (a client's dial-target vs. a server's bind-port are
  *different* catalogs, which is the reason to split them). This refines ADR-0059's single global
  `/net/export` and this RFC's own earlier per-*transport* spelling with a config `role`.
- **The module designates the control.** The endpoint, its `:schema` catalog, and the `config`
  semantics are owned by the transport module (the `register_transport_type` seam), not a central
  switch — `transport_vertex.cpp` never learns a concrete transport (open/closed, ADR-0043). Adding a
  module adds its creator endpoint and catalog; dropping one drops both. **This replaces
  the current single-global catalog**, and requires per-module `:schema`-as-catalog, which
  `graph.cpp`'s whole-vertex-only `:schema` does not yet serve.
- **It is a vertex, not a field.** `/net/<module>/conn` is a distinct-identity `/` vertex written
  to, **not** a `:conn` control field — a creation *field* is exactly what ADR-0059 / ADR-0021 moved
  creation off of.

### 2. Create and remove collapse by payload type

One control vertex serves both; the **TLV type of the written value** selects which:

- **`SPEC` ⇒ create.** The device validates the `name` (non-empty, non-reserved — see §3) and the
  `config` against its `conn:schema` catalog. An unknown/malformed config →
  `ERROR{tr::schema::not_found}` / `ERROR{tr::schema::type_mismatch}`; an **empty** name (or any
  other schema-shape violation of the SPEC envelope) → `ERROR{tr::schema::type_mismatch}`, while the
  **reserved** name → `ERROR{tr::path::in_use}` — the path is literally occupied by the endpoint
  vertex, so this is what the `register_vertex_key` seam produces anyway (see §3 create-side).
  `SPEC` naming a **name that already exists** (live or retired
  but re-registered) → `ERROR{tr::path::in_use}` — reconfiguration is via the connection's `:settings`,
  never re-SPEC. (A retrying orchestrator treats `PATH_IN_USE` as "already exists," so the reject is
  idempotent-safe.) On success the child `/net/<module>/<name>` exists **atomically** (one write
  yields a fully-configured connection vertex — the GPIO-atomicity ADR-0059 protects; no "live but
  misconfigured" window).
- **`NAME` ⇒ remove.** The endpoint resolves the name; an **unresolvable** name (never created, or
  already retired) is a **no-op success** at the endpoint layer (`retire()`'s own idempotence only
  covers an already-resolved-but-unregistered handle). A resolvable member is passed to
  `graph_t::retire()`. `NAME{conn}` (or any reserved/protocol-owned name) is **rejected**
  (`ERROR{tr::access::denied}` — erratum 2026-08-21; this read `tr::acl::permission_denied`, which
  is not a registered identity) and never routes to `retire()` — the endpoint cannot
  self-destruct.
- **Any other payload** (empty, or a TLV type that is neither `SPEC` nor `NAME`) →
  `ERROR{tr::schema::type_mismatch}`. The endpoint never falls through to an ordinary assign.
- **`SPEC`/`NAME` are control only on the creator endpoint.** Written to any *other* vertex
  (including a connection vertex `/net/<module>/<name>`) they are ordinary value writes, not
  control.

The creator endpoint is **write-only and valueless**: the write is *executed*, not *assigned*, so it
neither stores a value nor propagates to subscribers under
[RFC-0008](0008-vertex-operations-assign-propagate.md) (RFC-0008 §D maps assign+propagate for a
`FWD{WRITE}` carrying a `VALUE`; `SPEC`/`NAME` are outside that scope — the verbatim semantics
ADR-0059 §Consequences pre-declared). Its only readable facet is `:schema` (the catalog).

#### 2.1 Amendment 3 (2026-08-24, ruled) — the catalog envelope is the ordinary `:schema` record, and an empty catalog is an empty `SETTINGS`

**What was open.** §1 and §2 name `conn:schema` as *"the module's config catalog"* and §Compatibility
lists `catalog-read` among the vectors, but no clause says what the reply *is* — so the S3 seam
(`transport_vertex.cpp`, the kind-private-config refusal) had no envelope to validate against, and an
endpoint with nothing to advertise had no defined answer.

**The envelope.** The catalog rides the **existing** `:schema` record and adds no shape of its own:

```
POINT { NAME <endpoint vertex name>, SETTINGS { …module-defined catalog… } }
```

That is exactly what `graph_t::read_schema` already emits for any vertex
([RFC-0010](0010-owner-app-fields-and-schema.md) §B.2's synthesized protocol part, plus the
owner part when a descriptor table is installed). The module-defined catalog is the **content of that
`SETTINGS`** — the module owns what goes inside it, the graph owns the frame around it.

**A module with no catalog answers an EMPTY `SETTINGS`, never `SCHEMA_NOT_FOUND`.** The empty answer
is the *conforming degenerate case*, not a stub: today's shipped `read_schema` behaviour already
satisfies this clause. `SCHEMA_NOT_FOUND` keeps the meaning §Compatibility gives it — *"endpoint
present, config type unknown"* — which is an answer to a `SPEC` **write**, never to the catalog read.
An implementation that answered `SCHEMA_NOT_FOUND` to a catalog read of a catalog-less module would
make the §6 creatability probe ambiguous: the probe asks whether the endpoint EXISTS, and the two
answers to that question are the `POINT` and `PATH_NOT_FOUND`.

**§6 probe, fully specified.** `read /net/<module>/conn:schema` ⇒ `PATH_NOT_FOUND` means *not
creatable*; **any** `POINT` means *creatable*, and its `SETTINGS` — possibly empty — is the catalog.

**Instrument: amendment** ([GOVERNANCE.md](../../../.github/GOVERNANCE.md)), window waived. It pins a
byte clause that was `proposed pending` (see §Discussion) rather than correcting one, and it moves no
bytes: the reply is what the reference implementation has always emitted.

### 3. The connection vertex (explicit identity) and the reserved name

`/net/<module>/<name>` is a first-class `/` vertex ([ADR-0027](../../adr/0027-transport-and-connections-are-vertices.md)):

- **Name, not address; role is the module.** The path segment is a **logical name** chosen at
  creation; the address (`addr`/`port`), keepalive, reconnect `backoff`, and `connect_timeout` live
  in `:settings` and are re-configurable. The **`role` (`DIAL`/`LISTEN`) is *not* a `:settings`
  field** — it is positional, fixed by which module the connection was created under (`ws-client` =
  DIAL, `ws-server` = LISTEN, `can` = a multi-peer bus). B's IP changing is a `:settings` edit; every
  route under the connection is untouched.

  > **Erratum (2026-08-13), [#1070](https://github.com/avatarsd-llc/libtracer/issues/1070) —
  > see §Erratum at the end of this document.** "Live in `:settings` and are re-configurable"
  > and "B's IP changing is a `:settings` edit" are **corrected**: these keys are
  > transport-private creation-time config, not a vertex `:settings` facet, and no
  > post-creation reconfiguration door exists. The `role`-is-positional clause and the
  > name-not-address principle stand unchanged.
- **Explicit lifecycle only.** Created solely by a `SPEC` write (or an owner-local registration),
  removed solely by a `NAME` write (or owner-local `retire`). Connection vertices are a stated
  **exception** to RFC-0005 §D write-creates and RFC-0009 §E.1 revive-by-data-write: a plain data
  write to an absent or retired `/net/<module>/<name>` does **not** create/revive it (a
  config-less connection would violate the SPEC-atomicity §2 protects). Re-creation is `SPEC`-only.
- **Its value is the link-liveness state** (§4), readable, `await`-able, and **subscribable** — a
  liveness transition assigns-and-propagates under RFC-0008 (so `subscribe /net/<module>/<name>`
  streams state changes without polling), distinguishing this propagating vertex from the write-only,
  non-propagating creator endpoint.

**`conn` is a reserved, protocol-owned name** per transport:

- **Create-side:** a connection may not be named `conn` — the endpoint vertex already occupies
  `/net/<module>/conn`, so a same-named `SPEC` fails `ERROR{tr::path::in_use}` at
  `register_vertex_key`. (This is *not* the `#373` first-level-shadowing guard, which inspects only
  root-level `/name` children against the FWD child-link registry and never sees a `conn` nested
  under a transport.)
- **Remove-side:** `NAME{conn}` is rejected (§2).
- **Enumeration:** `conn` is **hidden** from `/net/<module>:children[]` (which returns the member
  connection vertices). **No mechanism to hide a registered child from enumeration exists today** —
  this is a new seam the implementation must add (or the endpoint must be a placeholder-like node
  ADR-0057 already excludes).

  > **Clarified 2026-08-24 (ruled;
  > [RFC-0016](0016-composed-branch-read.md) Amendment 1).** The hide is a property of **every
  > enumeration-shaped surface**, not of the `:children[]` spelling: the composed branch read of
  > `/net/<module>` — which answers the same question, "what is under here?" — omits `conn` and its
  > subtree as well. **Direct addressing always sees it**, which is precisely why §6's
  > `read <module>/conn:schema` probe is the sanctioned way to discover a deliberately unlisted
  > endpoint.

### 4. Link liveness (the socket, managed automatically)

The connection *vertex* and the *link* it names are two lifecycles. The vertex is explicit (§3); the
link is a state machine.

**Refcount governs `DIAL` links only.** A *binding* is anything that needs the peer reachable — a
standing subscription or `await` routed through the link, plus a **transient** hold for the duration
of a one-shot `read`/`write`/`FWD`. It is **per-hop and local**: a multi-hop route
`/net/ws-client/b/net/ws-client/c/sensor` holds a ref on *this* node's link `b`; c's node independently refs its own link
`c`. For a `DIAL` link the **steady-state target is: socket up while refcount > 0** (the transient
states `dialing`/`reconnecting` have refcount > 0 with the socket not yet or no longer up — erratum
2026-08-21: this pair read `dialing`/`healing`, and there is no `healing` state). A `LISTEN`
link **ignores refcount** — its listen socket stays bound and accepting until the vertex is retired
(reachability is its purpose); bindings routed through its accepted peers do not gate it.

- **Any op auto-wakes a dormant `DIAL` link.** The op triggers a dial, waits for **one connect
  attempt** (bounded by `connect_timeout`), then serves or returns `link-down`. A `write` may
  therefore stall on a dial; `await /net/<module>/<name>` (which resolves specifically when
  liveness reaches `up`, and returns `link-down` on terminal failure — not a generic
  await-next-transition) is the explicit "bring it up and wait" verb for callers that will not
  tolerate a data op blocking.
- **A one-shot op's transient hold is released BEFORE self-heal is evaluated**, so a lone one-shot
  whose dial fails drops refcount to 0 and the link re-dormants with **no** background retry; only a
  standing binding (refcount > 0 after the op returns) triggers self-heal.
- **Self-heal is the retry loop toward the target, and runs iff refcount > 0.** Loss while in use →
  background retry with `backoff`. A link **MUST NOT** background-retry at refcount 0; loss — or a
  failed wake-dial — at refcount 0 re-dormants with **no** retry; and the **last standing release
  MUST close the socket and re-dormant**. Whether an *op-woken* socket that never acquired a
  standing binding is closed eagerly when its transient hold releases, or left **up until loss**, is
  an implementation choice — see **Amendment 1 (§4.1)**, which replaces this bullet's original
  "refcount → 0 → close socket" reading.
- **Ops on a down/`reconnecting` link fail fast** with `link-down` — never block forever on a dead
  peer.
- **A permanently-unreachable `DIAL` peer with a standing binding retries indefinitely** while
  refcount > 0 (the no-synthetic-limits consequence — no max-attempt constant, **no give-up bound,
  and no terminal-failure state**: a declared link retries until it connects or is `NAME`-removed).
  `backoff` and `connect_timeout` are `:settings` config, never hardcoded (RFC-0006/0007 / ADR-0051).
  A consumer distinguishes "transiently retrying" from "unreachable" by applying its **own** display
  threshold to the propagated `reconnecting` state — the wire stays honestly `reconnecting`.

  > **Erratum (2026-08-13), [#1070](https://github.com/avatarsd-llc/libtracer/issues/1070) —
  > see §Erratum at the end of this document.** Read "config, never hardcoded" as scoped to
  > **this engine's own defaults** for a creator-endpoint `DIAL` connection, and read
  > "`:settings`" as the transport's config door (the erratum on §3). It does **not** oblige a
  > *provided* link to accept a wire-set dial deadline: where a platform bound exists — an ESP
  > task-watchdog window — `connect_timeout` is a **derived safety bound**, and config may only
  > tighten within it, never raise how long a task may block.
- **Liveness enum** (the vertex value; supersedes the binary `set_link_state`):

  > **Erratum (2026-08-21), Amendment 1 (§4.1) — see §Erratum: the `dormant` row is not a
  > biconditional.** The row below originally read *"vertex exists; no socket (refcount 0)"*,
  > which reads as *refcount 0 ⇒ `dormant`*. Under §4.1 an op-woken socket with no standing
  > binding **MAY** remain `up` at refcount 0, so refcount 0 is necessary for `dormant` and not
  > sufficient. The state set, its meanings and its encoding are unchanged.

  | state | role | meaning |
  | --- | --- | --- |
  | `dormant` | DIAL | vertex exists; no socket. Refcount is 0 here, but refcount 0 does **not** imply this state — see §4.1 |
  | `dialing` | DIAL | a connect attempt is in flight (first-ever or resumed) |
  | `reconnecting` | DIAL | retrying toward `up` between backoff waits (whether or not previously up) |
  | `up` | DIAL | socket connected, bidirectional |
  | `listening` | LISTEN | listen socket bound and accepting (0 or more peers — see below) |
  | `bind-failed` | LISTEN | the listen socket could not bind (e.g. port in use) |

  A `LISTEN` vertex's liveness reports **listen-socket reachability**, not per-accepted-peer
  connectivity; accepted-peer count/identity is exposed via `:children[]`/`:settings`, not the
  liveness enum. Once up, a link is **bidirectional** regardless of who dialed; `role` is only *who
  initiates*.

#### 4.1 Amendment 1 (2026-08-21, ruled) — keep-up after an op-woken dial is a MAY, and only three MUSTs bind

**Instrument.** Amendment, not erratum: §4's steady-state bullet as written was read literally to
require a socket close the instant refcount hits 0 — *including* the moment a one-shot's transient
hold releases — and this replaces that requirement with a MAY. It withdraws a normative
obligation rather than clarifying one, so it is an amendment even though no wire byte, frame shape,
type code or error identity moves: what changes is only *when a local socket is closed*, observable
to a peer as nothing and to a caller as op latency. Ruled by the maintainer on the divergence
flagged in [PR #1455](https://github.com/avatarsd-llc/libtracer/pull/1455) (the S5 link-liveness
engine, [#492](https://github.com/avatarsd-llc/libtracer/issues/492)). The comment window is
**waived by default** under [GOVERNANCE.md](../../../.github/GOVERNANCE.md)'s solo-maintainer clause
and is not invoked.

**The steady-state target stands, for standing bindings.** "Socket up while refcount > 0" is still
what a `DIAL` link aims at, and the transient states `dialing`/`reconnecting` are still refcount > 0 with
the socket not yet or no longer up. What the amendment touches is the *other* direction — what an
implementation owes the moment refcount reaches 0.

**The MAY.** An **op-woken socket with no standing binding MAY remain `up` until loss.** The
reference implementation does exactly this: a one-shot's transient hold is invisible at the
fire-and-forget send seam, and tearing the socket down per-op would put a dial — a hidden handshake
— inside the next `write`, which is precisely the latency §Alternatives rejects in refusing lazy
create-from-address. An implementation **MAY** instead close eagerly as soon as the transient hold
releases; a constrained node with no socket budget to spare is entitled to that trade. Both are
conforming. The choice is **locally observable only as op latency** — a peer cannot tell the two
apart except by when the next connection arrives.

**The MUSTs.** Three clauses bind, and they are the peer-observable surface:

1. A link **MUST NOT** background-retry at refcount 0. No timer, no backoff loop, no attempt a
   binding did not ask for.
2. Loss — **or a failed wake-dial** — at refcount 0 **MUST** re-dormant with **no** retry. This is
   §4's transient-hold rule stated for both of its arms: the lone one-shot whose dial fails, and the
   kept-up socket that later dies with nothing holding it.
3. The **last standing release MUST close the socket and re-dormant.** Releasing the final standing
   binding is an explicit "I no longer need this peer reachable"; it is not the same event as a
   transient hold evaporating, and the MAY above does not reach it.

> **Conformance-vector note.** The S7 vectors pin **only these three MUSTs**. A vector that a
> conformant eagerly-closing implementation would fail is, by construction, not a conformance
> vector — keep-up is a reference-implementation property and is pinned in the reference
> implementation's own test suite (`core/tests/link_liveness_test.cpp`), not on the conformance
> surface. §Discussion's `refcount-0→dormant` vector is to be read as the MUST-2/MUST-3
> re-dormant, never as an assertion about *when* an op-woken socket closes.

### 5. Gating

The control reuses **already-enforced** access-mask bits ([05 §ACL](../../reference/05-protocol-tlvs.md)):

- **`SPEC` (create) gates on `CREATE` (0x08).** `CREATE` already gates `:children[]` creation and
  write-creates (`graph.cpp:1400`, `:485`, `:955`). This RFC **relocates** that gate onto the
  creator endpoint's own ACL, so the create right is delegable **without any right on the parent
  transport**. (ADR-0059 §Decision 6's "allocated-but-ungated trap" described the *rejected*
  WRITE-for-create alternative; choosing `CREATE` here means the trap never arises.)
- **`NAME` (remove) gates on `WRITE` (0x02).** RFC-0009 §A.1.1 pins the creator-endpoint removal
  write as "ACL-gated like any write," §A.2 records `DELETE` (0x10) as **reserved-and-unused for
  protocol v1**, and §Discussion 1 explicitly *rejected* a `DELETE`-gated remote retire. This RFC
  therefore gates `NAME` on `WRITE`, consistent with the accepted RFC-0009 — **not** on `DELETE`.
  (An earlier draft proposed `DELETE`; that collided with RFC-0009 §A.2 and is withdrawn.)

A peer can thus hold create-but-not-remove (`CREATE` without `WRITE` on the endpoint) or the reverse.

#### 5.1 Amendment 2 (2026-08-24, ruled) — the demanded right is a payload-type table the VERTEX declares; the gate does not learn a transport

**What §5 left unsaid.** §5 says which right each control payload gates on. It does not say **where
that mapping lives** — and the reference implementation has exactly ONE value-write gate
(`graph_t::write_impl`), which every plane enters through and which hardcoded `WRITE` for every
vertex. Spelling §5 into that gate directly would put a transport concept ("this vertex is a creator
endpoint, so a `SPEC` means create") inside the graph's narrowest, most-travelled frame.

**The general contract.** A **handler vertex MAY declare a payload-type → required-ACL-right table**
alongside its handlers. Normatively:

1. **Absent declaration ⇒ plain `WRITE`.** Today's behaviour, and today's cost: a vertex that
   declares nothing is gated exactly as before, and pays nothing — not a byte of storage and not a
   taken branch — for a contract it does not use. (In the reference implementation the rows live on
   the graph, and the vertex carries one bit saying they exist; that is an implementation choice the
   clause does not mandate, but the *cost* it protects is the clause's point.)
2. **The selector is the written TLV's TYPE, and only its type.** Never payload content. This is the
   load-bearing half: content-keyed selection would mean running the vertex's own parsing ahead of
   the ACL check, i.e. letting an unauthorized caller reach user code. A value with no readable
   leading type byte (empty, or a device-memory head) is not a declared payload and takes `WRITE`.
3. **One gate, one counter.** The declaration changes **which** right is demanded, never **where**
   the demand is made: the single gate consults it, and a refusal counts into the same single-sited
   `denied` drop counter as any other write refusal — a counter whose value depended on which door a
   refusal came through could not be summed.
4. **The right is demanded on the written vertex's OWN effective ACL.** A declaration re-labels the
   demand; it never relocates it. This is what keeps §5's delegability claim true: `CREATE` on the
   endpoint's own ACL, with no right on the parent transport.
5. **The table is the vertex's, not the graph's.** The graph learns no transport concept — it learns
   that a vertex may say which right a written type costs.

**§5 discharged.** The creator endpoint is the first user of the contract and declares exactly §5's
mapping: `SPEC` ⇒ `CREATE`, `NAME` ⇒ `WRITE`. Anything else written to the endpoint takes the default
`WRITE` and is then refused on its **shape** by §2 (`TYPE_MISMATCH`) — a right-less payload cannot
slip past the gate by being undeclared, because undeclared means `WRITE`, not "ungated".

**Deliberately general.** The contract is written for the creator endpoint but is not about
transports: an application subtree-owner that implements create-on-write (the
[RFC-0003](0003-bridged-wildcard-delivery-path.md)/RFC-0005 §D direction) can demand `CREATE` for the
payload type that creates and `WRITE` for the one that mutates, on its own vertex, with no core
change. That generality is the reason the mapping is a declaration rather than two branches in the
gate.

**Instrument: amendment** ([GOVERNANCE.md](../../../.github/GOVERNANCE.md)), window waived. The
normative surface grows — a new declaration and a new right-selection rule — while the **wire surface
does not move at all**: no new TLV type, no new error identity, no change to any reply's bytes. A
peer that never had `CREATE` on an endpoint now sees `tr::access::denied` where the reference
implementation previously accepted a `SPEC` under `WRITE` alone; that is §5 arriving, not a new
codepoint.

**How it is pinned.** Under the clause-kind rule (§Discussion) the gate-order clause is pinned by
code plus tests: `core/tests/payload_right_table_test.cpp` covers a `CREATE`-only peer creating via
`SPEC` and being refused `NAME`, a `WRITE`-only peer removing via `NAME` and being refused `SPEC`,
the fallback for an undeclared type, and the single `denied` counter. There is no codec vector
because there are no new bytes to bank — the conformance instrument for a clause with no
wire-observable bytes is the bound host test (`tests/conformance/HARNESS.md`).

### 6. Discovery and multi-hop reach (the orchestrator contract)

An orchestrator that manages a **remote** board — including one reachable only *through* a peer (the
d2d premise: A bridges B) — must discover and drive the endpoint over the ordinary FWD route:

- **Enumerate modules:** `read /net:children[]` returns the modules (`ws-client`, `ws-server`, `can`, …).
- **Creatability probe:** `read /net/<module>/conn:schema` returns the config catalog; a
  `PATH_NOT_FOUND` reply means "no creator endpoint → not creatable" (the ENOTTY optionality pattern
  applied to a missing `/` vertex). This is the sanctioned way to learn a creator endpoint exists,
  since `conn` is hidden from enumeration.
- **Reach through peers:** `conn:schema`, the connection-vertex liveness value, and `SPEC`/`NAME`
  writes are addressable via ordinary multi-hop FWD — e.g. read
  `/net/ws-client/<toB>/net/can/conn:schema` (route through A's `ws-client` link `<toB>`, then read
  `/net/can/conn:schema` on B) to configure a CAN link *on B* from an orchestrator connected only to
  A. **Each hop gates
  independently** (the intermediate link's ACL *and* the far endpoint's `CREATE`/`WRITE`).

## Scope boundary (what this RFC does NOT do)

RFC-0014 specifies the transport **link** and its lifecycle. A cross-device **wire** (the thing the
web UI's `relay.service` provides today by browser-relaying) is a **subscription**, not a link: to
retire the relay, board A must become the *subscriber* on B's producer — a multi-hop `SUBSCRIBER`
append whose accumulated `src` is A-rooted, originated by the departing orchestrator. RFC-0014
supplies the link that such a subscription rides and refcounts the link by "a standing subscription
routed through it," but it does **not** originate that subscription. Retiring browser-relay in favour
of owner-mediated d2d therefore has a **hard dependency** on a remote-owned-subscription-origination
surface (existing multi-hop `SUBSCRIBER` semantics, or a companion RFC). This RFC's #407 claim is
scoped to the link plane accordingly.

**Teardown, soft vs hard.** *Soft* = drop the last binding → the `DIAL` link goes dormant, the vertex
persists, self-heals on next use (the orchestrator-facing act is removing the routed subscription —
see the dependency above). *Hard* = `NAME` → `retire()` → the vertex is gone. On a hard retire, any
subscriptions still routed through the link are **cascade-evicted** per RFC-0009 §D (subscriber
eviction) rather than left dangling; a far-side producer's edge back through the retired link
fails-fast `link-down` and is reaped.

## Compatibility

- **v1-compatible — no new wire primitive.** `SPEC` and `NAME` are existing TLV types; `read` /
  `write` / `await` are unchanged. "No connect operation" holds **in letter**: v1.md §1 and ADR-0006
  ban connect/disconnect/subscribe as *wire verbs*, and RFC-0014 adds none — the dial is a
  transport-internal effect of an existing verb, the already-ratified model in CONTEXT.md §Transport/
  connection vertex and ADR-0027 §Decision 2.
- **Absent creator endpoint → `PATH_NOT_FOUND` (`tr::path::not_found`, 0x0020)** — the missing-`/`-
  vertex answer, matching RFC-0009 §A.1.1 (*"`/net/unexport` is `PATH_NOT_FOUND` like any other
  unbuilt path"*). `SCHEMA_NOT_FOUND` is reserved for the distinct "endpoint present, config type
  unknown" case (§2). (The ADR-0021 `SCHEMA_NOT_FOUND`/ENOTTY pattern is for an unsupported `:field`,
  not a missing `/` vertex.)
- **Supersedes the `:children[]` creation spelling** (as ADR-0059 already ruled); **reuses
  `retire()` unchanged** (RFC-0009). *(Executed at S7 — see Amendment 4 below. The supersession is
  no longer a statement of intent: the `client`/`listener` catalog types are gone.)*
- **New conformance vectors** (required before the byte clauses are normative): `create-via-SPEC`,
  `remove-via-NAME`, `remove-nonexistent-noop`, `remove-reserved-rejected`, `spec-name-in-use`,
  `bad-payload-type`, `catalog-read`, `absent-endpoint-PATH_NOT_FOUND`, `gate-CREATE`, `gate-WRITE`,
  and the liveness transitions (`dormant→dialing→up`, `up→reconnecting→up`, `refcount-0→dormant`,
  `listen→listening`). *(Amendment 1, §4.1: `refcount-0→dormant` pins the three MUSTs only — it
  must not assert **when** an op-woken socket with no standing binding closes, which is a MAY.)*
  *(All landed — see Amendment 4 for the final disposition of each, including the three that carry
  no wire-observable bytes and are therefore bound by a host test alone.)*
- **Migration.** ~~Additive; deployed devices keep working.~~ **BREAKING as of S7** (Amendment 4): a
  deployed orchestrator that creates connections through `write /net:children[] += SPEC{type, name,
  config}` stops working and must move to the creator endpoint. That consumer's web UI's deferred
  stub (`#82`) gains its *link* half; the browser-relay wires need the origination dependency above
  before they become owner-mediated.

### Amendment 4 (2026-08-26, ruled) — the supersession is EXECUTED, and the byte clauses are normative

**Instrument: amendment**, window waived by default under
[GOVERNANCE.md](../../../.github/GOVERNANCE.md)'s solo-maintainer clause and not invoked. Two things
move the normative surface at once, and both are the RFC's own machinery arriving rather than a new
design:

1. **A wire door is REMOVED.** Not deprecated, not discouraged — removed. This RFC and
   [ADR-0059](../../adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md) both
   ruled the `:children[]` creation spelling superseded, but a superseded door that still answers is
   still a door, and two doors onto one creation semantics is exactly the drift the split body was
   built to prevent. S7 executes the ruling: the reference implementation no longer registers the
   `client` and `listener` child types, so
   `write /net:children[] += SPEC{ type = "client"|"listener", name, config }` now answers
   `SCHEMA_NOT_FOUND` — the ordinary unregistered-catalog-type answer, not a new identity. The
   creator endpoint of §2 is the **only** wire path that creates a connection vertex.
2. **The `role` config key is retired with it.** `role` existed to override the catalog type's
   default direction, and the catalog type is what died. §1/§3 already made the role **positional**;
   the endpoint always overwrote a written `role` with its module's declared one, so the key was
   inert before it was removed. It is now an ordinary unknown pair: **ignored**, per the config
   walk's forward-compatibility rule, never obeyed and never an error.

**What is NOT retired — stated because conflating the two is the likely misreading.** `:children[]`
remains, in both of its other capacities. As an **enumeration** it is untouched: `/net:children[]`
lists this plane's modules, `/net/<module>:children[]` lists that module's member connections (with
`conn` hidden per §3), and a bus connection's `:children[]` still serves its live peers. As a
**generic creation** surface ([RFC-0013](0013-creatable-child-type-catalog.md)) it is untouched too:
`stored_value` and any type an application registers still create through it. What died is two
registrations, not a mechanism.

**The byte clauses are now normative.** §Discussion's clause-kind rule made every byte / error-identity
/ gate-order clause in this RFC *proposed pending* code plus a conformance vector. That condition is
discharged, and the promotion is what makes this an amendment rather than a code change: clauses that
did not bind now bind. Their instruments, in full:

| clause | instrument |
| --- | --- |
| the `SPEC`/`NAME`/`config` layout (§2) | `conn/create-via-spec`, `conn/remove-via-name` |
| the §2 error identities | `conn/remove-nonexistent-noop`, `conn/remove-reserved-rejected` (`0x0050`), `conn/spec-name-in-use` (`0x0022`), `conn/bad-payload-type` (`0x0030`) |
| the §Compatibility absent-endpoint identity | `conn/absent-endpoint-not-found` (`0x0020`) |
| the liveness-enum encoding (§4) | `conn/liveness-enum`, plus the transitions in `link_liveness_test.cpp` |
| the §4.1 three MUSTs | `conn/refcount-0-dormant` — **host-test-only, no vector directory**, because none of the three has a wire surface of its own. The §4.1 MAY is deliberately **not** pinned. |
| the `conn:schema` catalog reply bytes (§2.1) | Amendment 3 plus `graph_t::read_schema` and its host test |
| the `CREATE`/`WRITE` gate order (§5) | Amendment 2 plus `core/tests/payload_right_table_test.cpp` |

Three of those clauses have **no wire-observable bytes** and so carry **no codec vector** — the gate
order, the three MUSTs, and the `conn:schema` shape (an existing banked record). Per
`tests/conformance/HARNESS.md` the conformance instrument for such a clause is the bound host test
alone, and that is not a weaker pin: a vector for a clause whose subject is the *absence* of a dial
would claim carriage of bytes the clause is not about.

**One vector is retired with the door.** `spec/conn-client-ws` was the `:children[]` spelling of the
same connection and carried the `type` and `role` pairs. It is deleted. The one claim it held that
outlives the door — that a config's values are typed **per key** and the two forms are not
interchangeable (`addr` a textual `NAME`, `port` an opaque `VALUE`) — moved to `conn/create-via-spec`,
whose `config` carries the same mix.

**Reference-implementation surface.** `tr::net::conn_spec_t`'s two-argument (`type`, `name`)
constructor, its `role()` setter, and the `conn_spec(type, name, role, port, …)` free function are
removed; the TypeScript binding's `encodeConnSpec` loses `type` and `role` for the same reason. That
is API, not wire, and is recorded in the respective `CHANGELOG.md` files — but it is named here
because a spec that removes a door owes the reader the fact that the encoder for it went too.

## Alternatives considered

- **Lazy create-from-address** (a data write to `/net/<transport>/<addr>` implicitly creates+dials).
  Rejected: hides a dial/handshake inside a `write` (against the latency-first design center),
  unbounded implicit vertex accumulation, no home for non-address config (the atomicity the GPIO
  precedent in ADR-0059 protects), and no way to express a `LISTEN` (a server has no destination
  address). The useful kernel — *lazy reconnection* — is retained as self-heal (§4), which creates no
  vertex.
- **Full sysfs / a vertex per control knob.** Rejected in ADR-0059 §Considered options (inverts
  ADR-0021, no shell payoff, breaks the 16 KB `vertex_size_test` story). Connection controls stay
  `:settings`.
- **One global `/net/export` with `type` in the SPEC** (ADR-0059's literal spelling). Rejected here
  for per-transport: type-positional payloads, transport-specific catalogs and ACLs, module-designated.
- **A `:conn` creation *field*.** Rejected: creation is a distinct-identity `/` write, not a `:`
  facet (ADR-0059 / ADR-0021).
- **`DELETE`-gated removal.** Rejected (RFC-0009 §A.2 / §Discussion 1): `DELETE` is reserved-unused
  for v1; the removal write is `WRITE`-gated (§5).
- **A distinct `tr::path::retired` error + STATUS-in-place-of-VALUE delivery.** Rejected (ADR-0059
  §Decision 4 / RFC-0009 §C): retirement collapses into `not_found`.

## Discussion

- **Clause-kind rule.** Per ADR-0059 §Consequences, *declaring / forbidding / reserving* clauses may
  lead the code; *bytes / error identities / gate order* must be pinned by code **plus a conformance
  vector** before they are normative. The leading clauses (a per-module creator endpoint exists;
  creation is explicit and named; `conn` reserved and hidden; removal routes through `retire()`;
  refcount governs DIAL only; liveness target rule) stand on acceptance. The byte clauses (the
  `SPEC`/`NAME`/`config` layout, the `conn:schema` reply bytes, the liveness-enum encoding, the
  `CREATE`/`WRITE` gate order, the error identities in §2/§Compatibility) were **proposed pending**.
  Two of them were pinned first: the `conn:schema` reply bytes by Amendment 3 (§2.1) plus
  `graph_t::read_schema` and its host test, and the `CREATE`/`WRITE` gate order by Amendment 2 (§5.1)
  plus `core/tests/payload_right_table_test.cpp`. Neither pin has a codec vector, and neither needs
  one: the schema reply is an existing banked shape and the gate order has no wire-observable bytes
  of its own — for such a clause the conformance instrument is the bound host test
  (`tests/conformance/HARNESS.md`). **The rest were discharged at S7: no byte clause in this RFC is
  `proposed pending` any more — see Amendment 4's instrument table.**
- **Amends ADR-0059** on three points (per-transport rather than one global endpoint; `SPEC`/`NAME`
  collapsed onto one control; the gating **resolved** as `CREATE`-for-SPEC / `WRITE`-for-NAME) and
  **extends** it with the link-liveness layer. ADR-0059's load-bearing decisions are unchanged.
- **Consistent with RFC-0009** — removal is the §A.1.1 device-mediated write, `WRITE`-gated per §A.2,
  reusing `retire()`; the write-creates exception for connection vertices (§3) explicitly narrows
  RFC-0009 §E.1 for the transport subtree.
- **Rulings (maintainer, window waived).** (i) `SPEC` on an existing name **rejects** with
  `PATH_IN_USE` (idempotent-safe for a retrying orchestrator); reconfiguration is `:settings`, and a
  declarative reconciler diffs live-vs-desired and SPECs only absent names. *(Erratum 2026-08-13,
  [#1070](https://github.com/avatarsd-llc/libtracer/issues/1070): "reconfiguration is `:settings`"
  is corrected — see §Erratum. The `PATH_IN_USE` rejection and the reconciler guidance stand; a
  reconciler that must change a key retires and re-creates.)* (ii) A `DIAL` link
  **retries forever** while refcount > 0 — **no** give-up bound and **no** terminal state (a give-up
  count would be a synthetic limit); presentation thresholds are the consumer's. (iii) The
  remote-owned-subscription-origination surface named in §Scope boundary is a **separate follow-up**
  (its own investigation / RFC), not part of RFC-0014 — RFC-0014 is the link plane only. (iv) The
  creator endpoint is a **per-*(transport, role)* flat module** (`/net/ws-client/…`,
  `/net/ws-server/…`, `/net/can/…`), not a per-*transport* endpoint with a config `role`: a client and
  a server take different config (dial-target vs. bind-port), so they are separate modules with
  separate catalogs, and the **role is positional**, not a `:settings` field. Uniform
  `net → module → name → [peer]` depth; a multi-peer module (`ws-server`, `can`) carries a **peer
  segment** resolved in that module's own peer table, a point-to-point module (`ws-client`) does not
  ([ADR-0061](../../adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)). This refines the RFC's
  own §1/§3; the exhaustive path/byte rewrite across §§2–6 and the conformance vectors lands **with
  the code, under #492's draft-spec authority** (the clause-kind rule — leading clauses here, byte
  clauses pending).
- Drafted from a maintainer grill session (2026-07-24) with Claude Code, then revised against an
  adversarial verification pass (ADR/spec consistency, code-pinnability, internal coherence, consumer
  fit); the design decisions are the maintainer's. Accepted the same day (solo-maintained spec, window
  waived — the RFC-0009 precedent).

## Erratum (2026-08-13) — connection config is a transport-private creation-time record, not a vertex `:settings` facet; `connect_timeout` may be a derived safety bound ([#1070](https://github.com/avatarsd-llc/libtracer/issues/1070))

**What the text said.** §3's first bullet: the address (`addr`/`port`), `keepalive`, `backoff`
and `connect_timeout` *"live in `:settings` and are re-configurable"*, and *"B's IP changing is
a `:settings` edit; every route under the connection is untouched"* — restated in
§Compatibility ruling (i) as *"reconfiguration is `:settings`"*, and copied into
[reference/13](../../reference/13-network-formation.md) §Creator endpoint. Separately, §4:
*"`backoff` and `connect_timeout` are `:settings` config, never hardcoded"*.

**What was wrong.** Three distinct claims, none of which holds:

1. **Residence.** These keys never reach a vertex `:settings` namespace. They travel in the
   `SPEC`'s `config` SETTINGS and are parsed into `tr::net::conn_settings_t`, whose own
   declaration states it is *"not part of any vertex's protocol `:settings` surface"* — a
   device-private facet ([ADR-0021](../../adr/0021-colon-field-plane-is-the-vertex-ioctl.md)
   §Decision 3) reached through the transport's own config door. The route the text names was
   not merely unimplemented: the vertex `:settings` core namespace was emptied outright by
   [RFC-0022](0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.B, so there
   is no shared per-vertex policy record for these to live in.
2. **Reconfigurability.** There is no post-creation write door for them. The only accessor,
   `transport_vertex_t::settings_of`, returns a `const conn_settings_t*`. Changing a peer's
   address therefore means `NAME`-retire plus `SPEC`-re-create — and because
   `remove_connection` un-routes and retires the identity vertex, the routes under the
   connection are **not** untouched, which is the one property the sentence promised.
3. **Consumption.** `keepalive` has no consumer anywhere in the tree; `backoff` and
   `connect_timeout` are parsed and dormant pending this RFC's own §4 engine (#492). The
   [connection-config module page](../../modules/connection-config.md) already documented all
   three honestly; only the RFC and the reference page over-promised.

**The correction.**

- Connection config is **creation-time, transport-private** config. §3's residence and
  re-configurability clauses are withdrawn; the *name-not-address* principle and the
  positional-`role` clause they were attached to are untouched and still operative.
- §4's "never hardcoded" is **scoped**: it governs the S5 liveness engine's own defaults for
  creator-endpoint `DIAL` connections. It does not reach a *provided* link (`provide_link`, an
  embedder-constructed socket), where a platform constraint may impose a ceiling.
  `connect_timeout` on such a link is a **derived safety bound** — a peer's config may tighten
  within it but must never raise how long a local task may block. The shipped ESP WS client
  derives its dial deadline from the task-watchdog window for exactly this reason, and is
  conforming.
- A **re-configuration door, if one is ever wanted, is new design** — it belongs to the S5
  liveness engine work under [#492](https://github.com/avatarsd-llc/libtracer/issues/492), not
  to this erratum. Until then the honest statement is: set at creation, changed by
  retire-and-re-create.

**Instrument: erratum, not amendment** ([GOVERNANCE.md](../../../.github/GOVERNANCE.md)). No
wire surface moves: no grammar, frame shape, type code or error identity changes, and the
`config` SETTINGS keys and their parse are exactly as shipped. The corrected clauses are
*declaring* clauses describing where config lives and whether a door exists — and applying the
correction changes what no conforming implementation does, because no implementation ever
offered the withdrawn door. It aligns the text with RFC-0022 §3.B and with the shipped
`conn_settings_t` contract. Maintainer ruling in
[#1070](https://github.com/avatarsd-llc/libtracer/issues/1070) (grilled 2026-08-12).

**Text corrected alongside:** [reference/13](../../reference/13-network-formation.md) §Creator
endpoint — the copied sentence, plus an explicit statement of which keys are consumed today.

## Erratum (2026-08-21) — the `dormant` row states a biconditional §4.1 withdrew

**What the text said.** §4's liveness-enum table, `dormant` row: *"vertex exists; no socket
(refcount 0)"*. The parenthetical was written when §4's steady-state bullet was read literally as
"refcount → 0 → close socket", so the row's reading — **refcount 0 ⇒ `dormant`** — was the whole
truth at the time. `reference/13`'s §Link liveness carried the same reading in prose (*"when
refcount reaches 0 the socket closes and the link goes dormant"*) and in the same table row.

**What the behaviour is.** **Amendment 1 (§4.1, ruled 2026-08-21, on the divergence flagged in
[PR #1455](https://github.com/avatarsd-llc/libtracer/pull/1455))** replaced that literal reading
with a **MAY**: an op-woken socket with no standing binding **MAY** remain `up` until loss, and
the reference implementation takes exactly that arm — `self_heal_link_t` states it in its own
contract (*"an op-woken socket with no standing binding stays up until loss rather than being torn
down per-op"*), and its machine leaves `UP` for `DORMANT` only on **socket loss at refcount 0** or
on the **last standing release**
(`core/include/libtracer/self_heal_link.hpp`, landed in
[PR #1455](https://github.com/avatarsd-llc/libtracer/pull/1455)). So a link at refcount 0 may be
`up`, and the row as written contradicts the amendment that sits four paragraphs below it.

**The correction.** `dormant` means *the vertex exists and there is no socket*. Refcount is 0 in
that state — the condition is **necessary and not sufficient**, and the row now says so and points
at §4.1. The three MUSTs of §4.1 are what bind at refcount 0; the row describes a state, not a
transition rule, and it never did.

**Instrument: erratum, not amendment** ([GOVERNANCE.md](../../../.github/GOVERNANCE.md)). No wire
surface moves: the state set is unchanged, the reference encoding of `link_state_t` is unchanged
(`DORMANT = 0` …), no error identity, frame shape or type code moves, and no conforming
implementation does anything different — the clause corrected is a *describing* clause about when a
state holds, and the normative rule it appeared to state was already withdrawn by §4.1. This
erratum only stops the table from re-asserting the withdrawn reading.

**Text corrected alongside:** [reference/13](../../reference/13-network-formation.md) §Link
liveness — the "when refcount reaches 0 the socket closes" bullet, the same table row, and the
one-shot pitfall whose title generalized a failed-dial case into "a one-shot op never leaves a link
up".

## Erratum (2026-08-21) — §2 spells the reserved-name refusal with an unregistered identity

**What the text said.** §2, the `NAME` ⇒ remove bullet: `NAME{conn}` (or any reserved name) is
rejected *"(`ERROR{tr::acl::permission_denied}`)"*.

**What was wrong.** `tr::acl::permission_denied` is **not a registered error identity**, and could
not be: [RFC-0002](0002-protocol-error-model.md) §A draws `<concept>` from a **frozen** set —
*frame · tlv · path · schema · flow · access · transport · version* — in which `acl` does not
appear, and §D's registry has no such row. The registered identity for this refusal is
**`tr::access::denied` (`0x0050`)**, whose reserved status byte is `0x02 PERMISSION_DENIED` — which
is exactly what the reference implementation answers: `status_t::PERMISSION_DENIED` maps to
`wire::err_t::ACCESS_DENIED` in `core/src/fwd_reply.cpp`, and `ACCESS_DENIED = 0x0050` is spelled
`tr::access::denied` at its declaration (`core/include/libtracer/error.hpp`). The RFC named the
*status byte's* spelling under a concept word the registry does not have; the bytes on the wire
were never in doubt.

It is also a house naming-rule violation in its own right (`CLAUDE.md`: a namespace *"never reuses
an error-concept word"*) — `acl` sitting beside the registered `access` is precisely that
collision, and the access-mask bits §5 gates on are what invited it.

**The correction.** §2 reads `ERROR{tr::access::denied}`. Nothing else in §2 or §5 changes; the
`WRITE`-gating of `NAME` and the reserved-name rule are untouched.

**Instrument: erratum, not amendment** ([GOVERNANCE.md](../../../.github/GOVERNANCE.md)). **No wire
surface moves** — this is the strongest case of that in the batch: the corrected token was never a
wire value, the error code (`0x0050`) and the status byte (`0x02`) are unchanged and unchangeable
here, and a conforming implementation could not have implemented the spelling that was written
because no such identity exists to emit.

**Provenance.** Surfaced by the **#492 S7-A** car while building the control-plane conformance
vectors; its `conn/remove-reserved-rejected` vector pins `0x0050` off a real reply, so the shipped
side is nailed down by a vector rather than by inspection. Recorded on
[#492](https://github.com/avatarsd-llc/libtracer/issues/492).

## Erratum (2026-08-21) — there is no `healing` state; the transient state is `reconnecting`

**What the text said.** §4's steady-state sentence and §4.1's restatement of it both name *"the
transient states `dialing`/`healing`"*.

**What the behaviour is.** There is no `healing` state and there never was one in this RFC. §4's own
liveness enum — the normative state list, four lines below the first occurrence — enumerates
`dormant`, `dialing`, `reconnecting`, `up`, `listening`, `bind-failed`, and the shipped engine's
machine is exactly that set (`link_state_t`, `core/include/libtracer/transport_vertex.hpp`;
`self_heal_link_t`'s `DORMANT → DIALING → UP / RECONNECTING` transitions, landed in
[PR #1455](https://github.com/avatarsd-llc/libtracer/pull/1455)). The state the sentence means is
**`reconnecting`** — *retrying toward `up` between backoff waits* — which is precisely "refcount > 0
with the socket no longer up". `self_heal_link_t`'s contract deliberately no longer describes its
own behaviour in words the state model does not carry, and the RFC should not either: an
implementer reading "`healing`" looks for a seventh state, finds none, and has to guess which of the
six was meant.

**The correction.** Both occurrences read `dialing`/`reconnecting`. Nothing else in either sentence
changes: the pair still means "refcount > 0 with the socket not yet (`dialing`) or no longer
(`reconnecting`) up", which is what §4 and §4.1 both assert about it.

**On correcting a dated instrument.** The second occurrence sits inside **Amendment 1 (§4.1)**, and
this document does not rewrite dated instruments to match later rulings. This is not that: no
ruling of §4.1 is touched, no clause changes meaning, and the amendment's three MUSTs and its MAY
are untouched. What is corrected is a **state name that names nothing** — the same class as a
mis-spelled enumerator, in text that landed the same day as this erratum.

**Instrument: erratum, not amendment** ([GOVERNANCE.md](../../../.github/GOVERNANCE.md)). No wire
surface moves: no state is added or removed, the reference encoding is untouched, and a conforming
implementation cannot have implemented `healing` because no clause ever defined one.
