<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0020 — A bus link's connection NAME is not a routable next-hop (reject, never broadcast, on the request plane)

| Field | Value |
| ---- | ---- |
| **RFC** | 0020 |
| **Title** | A bus link's connection NAME is not a routable next-hop (reject, never broadcast, on the request plane) |
| **Status** | **accepted** — 14-day comment window waived by sole maintainer, 2026-08-01 |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-08-01 |
| **Comment window** | waived by the sole maintainer, 2026-08-01 ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"); `docs/implementations.md` still reads `_(none yet)_`, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment / clarification of [RFC-0004](0004-remote-operation-addressing.md) §B.** The forward-resolution step gains a MUST-reject for one hop shape a conforming peer could previously observe being broadcast — the normative surface changes, so this is not an erratum (GOVERNANCE.md excludes by name any change that alters what a conforming implementation does). |
| **Tracking issue** | [#741](https://github.com/avatarsd-llc/libtracer/issues/741) (`rfc`-labelled) |
| **Target spec version** | v1 itself — still `DRAFT` (`docs/spec/v1.md:1`), the immutability clause has never triggered. Same route RFC-0006 / RFC-0018 / RFC-0019 took. |

> **Numbering note.** 0012 was used and withdrawn; 0015 never existed. Neither gap is reusable —
> see [RFC-0016](0016-composed-branch-read.md) §ghost history and RFC-0018's numbering note. 0020
> is the next unused number after [RFC-0019](0019-path-depth-bounded-by-bytes.md).

---

## 1. Summary

This RFC records [ADR-0073 §3](../../adr/0073-naming-authority-the-application-mints-one-predicate-gates.md)
as normative text, amending RFC-0004 §B's forward-resolution step for **multi-peer (bus) links**:

> An inbound `FWD` whose next hop resolves to a bus link's **own connection NAME** — the
> `net/<module>/<name>` mount itself, with a **residual `dst` below it that names no current
> peer** — MUST be **rejected**, never broadcast. The rejection is a single directed
> `FWD{ op=REPLY, kind=ERROR }` carrying `STATUS{ ERROR{ tr::path::invalid (0x0021) } }`
> back along the request's accumulated `src` (RFC-0004 §D shape); a frame with no
> trustworthy return route (malformed at the level that carries `src`, or itself a
> `REPLY`) is dropped by value instead.

Only a bus link's **peer names** are routable next-hops below its mount
(`/net/<module>/<name>/<peer>/...` — universally legal since ADR-0073 §2's node-assigned
`p<slot>` names, #426). Two addressings are explicitly **unchanged**:

- a `dst` naming the mount **exactly** still addresses the connection vertex itself (its
  `:children[]`, `:settings`, liveness value) and terminates locally (ADR-0038 §3a);
- a peer-directed hop (`/net/<module>/<name>/<peer>/...`) still strips `net/<module>/<name>/<peer>`
  and forwards over that peer's directed endpoint (ADR-0061).

The only shape this RFC forbids is the **broadcast shape**: bus NAME + residual, no resolvable
peer segment.

## 2. Motivation

### 2.1 The defect: one directed request drew N replies

RFC-0004 §B's forward step says a hop "re-emits `FWD` over that link". For a point-to-point
link that is well-defined. For a multi-peer link, "that link" is ambiguous — and the reference
implementation resolved the ambiguity in the worst way: the fall-through egressed over the bus
transport's `send()`, which **fans out to every open peer**. One directed request drew N
replies and scrambled FIFO reply correlation — the failure that broke the #409 topology walk
(two replies for one request; nodes bound to wrong identities). The spec text never said
"broadcast"; it simply never said what a multi-peer next hop means, and the gap shipped.

### 2.2 The model: a `dst` is a directed route to one terminus

Everything else in RFC-0004 §B assumes exactly one delivery per route: `dst` shrinks
monotonically to one terminus; `src` accumulates one return route; a REPLY is one frame routed
back along it. A hop that multiplies one frame into N breaks the one-request-one-reply
correlation model *by construction* — there is no spelling of "which of the N replies is mine"
in the frame grammar, and adding one would be request state the stateless forwarder is
forbidden to hold (§A, and the 2026-07-31 erratum's reasoning).

### 2.3 Fan-out is not lost — it lives where it belongs

The subscription plane (RFC-0004 §D/§E, ADR-0026) delivers to N peers because **N peers
subscribed**, each delivery a separate directed `FWD` along a separately accumulated route.
That is the protocol's one sanctioned fan-out, and it is unaffected. What this RFC removes is
only the accidental *addressing-plane* fan-out that no client could correlate.

## 3. Normative change to RFC-0004 §B

Appended to §B's forward-resolution bullet list:

- **Multi-peer next-hop resolution.** When the resolved child is a **multi-peer (bus) link**
  and `dst` continues below its mount, the next segment MUST be resolved in **that link's own
  peer table** (never across buses — ADR-0061). If it names a current peer, the hop consumes
  the peer segment too and re-emits over that peer's **directed** endpoint. If it does not,
  the node MUST **reject** the frame: reply `kind=ERROR` with
  `STATUS{ ERROR{ tr::path::invalid (0x0021) } }` along the accumulated `src` for a request
  with an intact return route; drop by value for a malformed frame or a `REPLY`. A node MUST
  NOT emit the frame over the bus link's shared (fan-out) endpoint, and MUST NOT resolve the
  residual against its local graph (a `WRITE` would materialize a shadow vertex under the
  connection mount). A `dst` naming the mount exactly is unaffected — it addresses the
  connection vertex locally.

**Error code.** The issue left the exact code to this RFC: it is **`tr::path::invalid`
(0x0021)**, not `tr::path::not_found` (0x0020). Below a bus mount the only legal next segment
is a peer name; peer names are session-scoped (ADR-0073 §2 — "a fallback bus peer name
identifies a session, not a device"), so a segment naming a departed session is
indistinguishable from one that never named anything: the `dst` is unroutable *as an address
at this hop*, the same family every unroutable spelling answers (`path_t::parse`,
`subscribe_toward`). `not_found` would misdescribe the failure as a resolvable path whose
target is missing.

**Route-handle plane (§E.1).** An `ADVERTISE` whose route's next hop is a bus NAME with a
residual binds **nothing** — neither a downstream swap (re-advertising over the bus is the
same broadcast) nor a terminus binding (which would absorb every `COMPACT` locally). The
peer's `COMPACT`s then draw the ordinary `HANDLE_NACK` a stale label draws, and the flow
stays on the full-route `FWD` form, where the rejection above answers.

## 4. Rejected alternatives (recorded per #741)

1. **Keep broadcast, document it.** Ratifying the current behavior breaks the
   one-request-one-reply correlation model by construction (§2.2): every client on every bus
   pays a dedup-and-guess protocol that cannot be written correctly, because the frame
   grammar carries no reply-to-request identity beyond FIFO order — the exact thing N replies
   scramble.
2. **Verb-dependent routing** (broadcast for a `WRITE` without reply, reject for
   `READ`/`AWAIT`). This makes *addressability* depend on the *operation* — a `dst` that is a
   legal route for one op and an error for another — which no other hop in the addressing
   model does, and it still leaves the `WRITE` fan-out uncorrelatable (`WRITE` has a reply,
   §D). Fan-out semantics belong to the subscription plane, not to an op-conditional reading
   of `PATH` bytes.

## 5. Reference implementation and conformance

- `core/src/fwd_router.cpp` — `resolve_mount_segs` marks the bus-NAME-plus-residual descent
  `rejected` instead of falling through to the bus child's link; both forward arms (span and
  rope) answer via `reject_bus_name_hop` (assembled-error grammar identical to the terminus
  resolver's); `on_advertise` binds nothing for a rejected route.
- `core/tests/mount_routing_test.cpp` — drives the production wiring: rejection (no fan-out,
  no peer delivery, one directed 0x0021 ERROR reply), peer-originated rejection routed back to
  the sender only, and the two positive controls (peer-directed hop still forwards;
  exact-mount addressing still terminates and answers).
- `tests/conformance/vectors/v1/fwd/fwd-bus-name-reject` — codec-layer vector pinning the
  rejected shape (round-trip-safe bytes; the routing-layer MUST is carried in its note, the
  `fwd-wildcard-reject` precedent).

## 6. Consequences

- The #409 topology walk's `routable: false` special case for bus links becomes removable
  (ADR-0073 §Consequences): every enumerable name below a bus mount is now either routable
  (a peer) or deterministically rejected (anything else).
- Wire-visible change: a frame that used to fan out (and draw N replies) now draws exactly
  one ERROR reply — the protocol is `DRAFT`, so this rides the ordinary amendment route.
- Depends on ADR-0073 §2 / #426 (merged): peers are universally addressable (`p<slot>`)
  before the NAME hop may reject, so no reachable terminus is lost.

## Relates

- [ADR-0073](../../adr/0073-naming-authority-the-application-mints-one-predicate-gates.md) §3 — the grill ruling this RFC records (PR #740).
- [RFC-0004](0004-remote-operation-addressing.md) §B — the amended forward-resolution step.
- [ADR-0061](../../adr/0061-per-transport-mount-routing-strip-k-l5-demux.md) — strip-K descent, per-endpoint peer tables.
- [ADR-0044](../../adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md) — bus peer names as routable next-hop segments.
- #426 (`p<slot>` peer names), #409 (the broadcast failure), #741 (tracking).
