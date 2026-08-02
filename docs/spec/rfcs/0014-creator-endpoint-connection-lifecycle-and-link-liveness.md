<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0014 — Creator endpoint: connection lifecycle and link liveness

| Field | Value |
| ---- | ---- |
| **RFC** | 0014 |
| **Title** | Creator endpoint: connection lifecycle and link liveness |
| **Status** | **accepted** (2026-07-24 — maintainer ruling; comment window waived on this solo-maintained spec, per the RFC-0009 precedent) |
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
`:children[]` creation spelling still live in `transport_vertex.cpp`.

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
- **The current creation path is the superseded spelling.** `transport_vertex_t` registers ONE
  global `client`/`listener` catalog through `register_child_type` as a `:children[]` target on
  `/net` (`graph.cpp:1558`), and the `quic` module extends the *same* catalog via
  `register_transport_type` with a `kind` selector (open/closed). RFC-0014 keeps that module
  ownership but **replaces the single-global-catalog mechanism** with a per-*(transport, role)*
  module endpoint (§1); the per-module structure, the positional role, and `conn:schema`-as-catalog
  are all **new code**, not a preservation of the current seam.

## Proposed change

> **Implementation status.** Except where noted, this describes **new mechanism**. Today connections
> register flat at `/net/<name>` (not `/net/<module>/<name>`), the catalog is a single global
> `:children[]` target, `:schema` is whole-vertex-only (`graph.cpp:1879`, `size()==1`), `set_link_state`
> is a manual binary up/down bool, and no refcount / dormancy / self-heal exists. Per the clause-kind
> rule (see Discussion) the byte-level clauses here are **proposed pending** code + conformance
> vectors.

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
  `ERROR{tr::schema::not_found}` / `ERROR{tr::schema::type_mismatch}`; an empty or reserved name →
  `ERROR{tr::schema::type_mismatch}`. `SPEC` naming a **name that already exists** (live or retired
  but re-registered) → `ERROR{tr::path::in_use}` — reconfiguration is via the connection's `:settings`,
  never re-SPEC. (A retrying orchestrator treats `PATH_IN_USE` as "already exists," so the reject is
  idempotent-safe.) On success the child `/net/<module>/<name>` exists **atomically** (one write
  yields a fully-configured connection vertex — the GPIO-atomicity ADR-0059 protects; no "live but
  misconfigured" window).
- **`NAME` ⇒ remove.** The endpoint resolves the name; an **unresolvable** name (never created, or
  already retired) is a **no-op success** at the endpoint layer (`retire()`'s own idempotence only
  covers an already-resolved-but-unregistered handle). A resolvable member is passed to
  `graph_t::retire()`. `NAME{conn}` (or any reserved/protocol-owned name) is **rejected**
  (`ERROR{tr::acl::permission_denied}`) and never routes to `retire()` — the endpoint cannot
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

### 3. The connection vertex (explicit identity) and the reserved name

`/net/<module>/<name>` is a first-class `/` vertex ([ADR-0027](../../adr/0027-transport-and-connections-are-vertices.md)):

- **Name, not address; role is the module.** The path segment is a **logical name** chosen at
  creation; the address (`addr`/`port`), keepalive, reconnect `backoff`, and `connect_timeout` live
  in `:settings` and are re-configurable. The **`role` (`DIAL`/`LISTEN`) is *not* a `:settings`
  field** — it is positional, fixed by which module the connection was created under (`ws-client` =
  DIAL, `ws-server` = LISTEN, `can` = a multi-peer bus). B's IP changing is a `:settings` edit; every
  route under the connection is untouched.
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

### 4. Link liveness (the socket, managed automatically)

The connection *vertex* and the *link* it names are two lifecycles. The vertex is explicit (§3); the
link is a state machine.

**Refcount governs `DIAL` links only.** A *binding* is anything that needs the peer reachable — a
standing subscription or `await` routed through the link, plus a **transient** hold for the duration
of a one-shot `read`/`write`/`FWD`. It is **per-hop and local**: a multi-hop route
`/net/ws-client/b/net/ws-client/c/sensor` holds a ref on *this* node's link `b`; c's node independently refs its own link
`c`. For a `DIAL` link the **steady-state target is: socket up while refcount > 0** (the transient
states `dialing`/`healing` have refcount > 0 with the socket not yet or no longer up). A `LISTEN`
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
  background retry with `backoff`. refcount → 0 → close socket, go **dormant**, stop retrying.
- **Ops on a down/`reconnecting` link fail fast** with `link-down` — never block forever on a dead
  peer.
- **A permanently-unreachable `DIAL` peer with a standing binding retries indefinitely** while
  refcount > 0 (the no-synthetic-limits consequence — no max-attempt constant, **no give-up bound,
  and no terminal-failure state**: a declared link retries until it connects or is `NAME`-removed).
  `backoff` and `connect_timeout` are `:settings` config, never hardcoded (RFC-0006/0007 / ADR-0051).
  A consumer distinguishes "transiently retrying" from "unreachable" by applying its **own** display
  threshold to the propagated `reconnecting` state — the wire stays honestly `reconnecting`.
- **Liveness enum** (the vertex value; supersedes the binary `set_link_state`):

  | state | role | meaning |
  | --- | --- | --- |
  | `dormant` | DIAL | vertex exists; no socket (refcount 0) |
  | `dialing` | DIAL | a connect attempt is in flight (first-ever or resumed) |
  | `reconnecting` | DIAL | retrying toward `up` between backoff waits (whether or not previously up) |
  | `up` | DIAL | socket connected, bidirectional |
  | `listening` | LISTEN | listen socket bound and accepting (0 or more peers — see below) |
  | `bind-failed` | LISTEN | the listen socket could not bind (e.g. port in use) |

  A `LISTEN` vertex's liveness reports **listen-socket reachability**, not per-accepted-peer
  connectivity; accepted-peer count/identity is exposed via `:children[]`/`:settings`, not the
  liveness enum. Once up, a link is **bidirectional** regardless of who dialed; `role` is only *who
  initiates*.

### 5. Gating

The control reuses **already-enforced** access-mask bits ([05 §ACL](../../reference/05-protocol-tlvs.md)):

- **`SPEC` (create) gates on `CREATE` (0x08).** `CREATE` already gates `:children[]` creation and
  write-creates (`graph.cpp:1375`, `:460`, `:930`). This RFC **relocates** that gate onto the
  creator endpoint's own ACL, so the create right is delegable **without any right on the parent
  transport**. (ADR-0059 §Decision 6's "allocated-but-ungated trap" described the *rejected*
  WRITE-for-create alternative; choosing `CREATE` here means the trap never arises.)
- **`NAME` (remove) gates on `WRITE` (0x02).** RFC-0009 §A.1.1 pins the creator-endpoint removal
  write as "ACL-gated like any write," §A.2 records `DELETE` (0x10) as **reserved-and-unused for
  protocol v1**, and §Discussion 1 explicitly *rejected* a `DELETE`-gated remote retire. This RFC
  therefore gates `NAME` on `WRITE`, consistent with the accepted RFC-0009 — **not** on `DELETE`.
  (An earlier draft proposed `DELETE`; that collided with RFC-0009 §A.2 and is withdrawn.)

A peer can thus hold create-but-not-remove (`CREATE` without `WRITE` on the endpoint) or the reverse.

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
  `retire()` unchanged** (RFC-0009).
- **New conformance vectors** (required before the byte clauses are normative): `create-via-SPEC`,
  `remove-via-NAME`, `remove-nonexistent-noop`, `remove-reserved-rejected`, `spec-name-in-use`,
  `bad-payload-type`, `catalog-read`, `absent-endpoint-PATH_NOT_FOUND`, `gate-CREATE`, `gate-WRITE`,
  and the liveness transitions (`dormant→dialing→up`, `up→reconnecting→up`, `refcount-0→dormant`,
  `listen→listening`).
- **Migration.** Additive; deployed devices keep working. That consumer's web UI's deferred stub
  (`#82`) gains its *link* half; the browser-relay wires need the origination dependency above before
  they become owner-mediated.

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
  `CREATE`/`WRITE` gate order, the error identities in §2/§Compatibility) are **proposed pending**.
- **Amends ADR-0059** on three points (per-transport rather than one global endpoint; `SPEC`/`NAME`
  collapsed onto one control; the gating **resolved** as `CREATE`-for-SPEC / `WRITE`-for-NAME) and
  **extends** it with the link-liveness layer. ADR-0059's load-bearing decisions are unchanged.
- **Consistent with RFC-0009** — removal is the §A.1.1 device-mediated write, `WRITE`-gated per §A.2,
  reusing `retire()`; the write-creates exception for connection vertices (§3) explicitly narrows
  RFC-0009 §E.1 for the transport subtree.
- **Rulings (maintainer, window waived).** (i) `SPEC` on an existing name **rejects** with
  `PATH_IN_USE` (idempotent-safe for a retrying orchestrator); reconfiguration is `:settings`, and a
  declarative reconciler diffs live-vs-desired and SPECs only absent names. (ii) A `DIAL` link
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
