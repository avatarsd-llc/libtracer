# The auth subject and `peer_named` are two different claims — who wrote this, versus where the peer appears in the graph — and the `peer_named` default stays `false`

Status: **accepted** (maintainer-ratified 2026-08-16, ruling on [#1278](https://github.com/avatarsd-llc/libtracer/issues/1278), the policy residual split out of [#375](https://github.com/avatarsd-llc/libtracer/issues/375)). Closes the deliverable [#375](https://github.com/avatarsd-llc/libtracer/issues/375) listed as *"an ADR recording the decouple + the addressing-only role of `peer_named`"*. Composes with [ADR-0018](0018-access-control-authorization-pluggable-subject-token.md) (authorization over a pluggable subject token), [ADR-0044](0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md) (peer enumeration, and its 2026-08-13 amendment admitting an accepted session as an ordinary vertex) and [ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md) (the identity roadmap that eventually mints the subject). Feeds the ACL **subject table** the `write_ctx_t` doc comment names as its integration point (`core/include/libtracer/vertex.hpp:275`).

**This ADR changes no code.** It records why the shipped default is the right one and does not flip, and gives integrators the reasoning the reference docs did not carry.

## Context

`peer_named` is a wiring-time flag on the stream server links. Constructed `true`, a listener exposes the `bus_link_t` facet and each accepted session becomes an individually addressable peer; constructed `false` — the shipped default — the listener is FLAT: one routing identity for every peer it carries. The default is spelled in two places, both verified against `main` = `218ab50f`:

- `core/include/libtracer/transport_ws.hpp:198` — the `transport_ws_server` constructor's `bool peer_named = false`.
- `core/src/builtin_transport_ws.cpp:67` — the SPEC-created path's `cfg.flag("peer_named").value_or(false)`, so an omitted config key means FLAT.

(The line numbers quoted on [#1278](https://github.com/avatarsd-llc/libtracer/issues/1278) — `builtin_transport_ws.cpp:55`, `transport_ws.hpp:184` — have since drifted; the values they cited are unchanged.)

The question this ADR answers was opened when `peer_named` was the *only* way a handler could tell one writer from another. At `peer_named=false` a per-peer authorization decision was unreachable, so "flip the default to `true`" was a live proposal: pay per-peer addressing state everywhere, in order to get per-peer identity anywhere.

**Two things happened since, and between them they remove that motivation.**

1. **[#375](https://github.com/avatarsd-llc/libtracer/issues/375) Part 1 landed** (PR [#1291](https://github.com/avatarsd-llc/libtracer/pull/1291)). `on_write` is now a single signature carrying a `write_ctx_t` (`core/include/libtracer/vertex.hpp:286`) whose `subject` (`:306`) is the writer's resolved subject token — the identical value the vertex's ACL gate was evaluated against one frame up — with `is_local_owner()` (`:310`) discriminating the local-owner door by the empty token rather than by a spellable sentinel. A handler now has a first-class place to read *who wrote this* that does not go through the peer's name at all.
2. **Part 2's design is ruled** (2026-08-16, Option A, fused with [#1266](https://github.com/avatarsd-llc/libtracer/issues/1266)). The opaque `peer_handle_t` minted at accept (`core/include/libtracer/transport.hpp:69`, an 8-byte POD held to that size by the `static_assert` at `:88`) stops being converted back to a name string at `fwd_router_t::resolve_peer_name` (`core/src/fwd_router.cpp:1517`) and is carried through the resolver into the ACL caller context; the subject is derived from the handle at the terminus. When that car lands, a per-writer subject exists on every ingress frame **regardless of how the link is wired**.

So the axis that once made the two features look like one feature is gone. What remains is the honest question: are they *the same claim*, or two?

## Decision

**1. They are two claims, and they stay decoupled.**

> The **subject** answers *"who wrote this"*. **`peer_named`** answers *"where in the graph this peer appears"*.

They **compose**; neither implies the other. A named peer is not thereby authenticated — the name is an addressing fact minted by the transport at accept, not a credential anyone presented. An authenticated subject does not thereby become routable — a subject is an identity the ACL evaluates, and nothing in it says the graph must be able to send *to* its holder. All four combinations are legitimate deployments and each is reachable:

| | `peer_named=false` (FLAT) | `peer_named=true` (bus facet) |
| --- | --- | --- |
| **no subject** | the default point-to-point link: one identity, ACL open or link-scoped | a browser-tabs server that must reply to a specific tab, with no per-tab identity |
| **subject bound** | per-writer authorization with **zero** per-peer graph state — the target shape once Part 2 lands | per-peer ACL *and* per-peer addressing: a gateway that both authorizes and routes to individuals |

**2. The `peer_named` default stays `false`.** Not by inertia — by the two reasons below. No code changes in this ADR; `transport_ws.hpp:198` and `builtin_transport_ws.cpp:67` are correct as they stand.

**3. `peer_named` is an ADDRESSING knob and is documented as one.** Its cost and its benefit are both about reaching an individual peer, never about trusting one.

### Why the default does not flip

**Reason 1 — the payoff is gone.** The only argument that ever made a flip attractive was "auth needs per-peer naming". With Part 1 landed and Part 2 ruled, it does not. Flipping now buys nothing that the subject does not already buy more cheaply.

**Reason 2 — the NARROW lens says it is the wrong default.** `peer_named=true` is not free per peer. It is exactly the switch that turns peer churn into graph state:

- `bus()` returns the facet iff the flag is set (`core/include/libtracer/posix_endpoint.hpp:675`), and only then does a live session announce itself (`core/src/posix_endpoint.cpp:636`; a FLAT server announces nothing — `posix_endpoint.hpp:933`).
- That announcement lands as `fwd_router_t::bus_peer_up`, which registers a **session-anchor vertex** per peer (`core/src/fwd_router.cpp:1006`), retired on departure by `bus_peer_down`.
- Retirement **parks a value seam** the graph cannot free on its own; only an explicit `graph_t::collect()` frees it (see CONTEXT.md, *value-seam park*). A bus node with peer churn that never collects grows the park forever; a FLAT server parks nothing on teardown and needs no collect point at all.

On a constrained node — the single-upstream MCU that is the common NARROW deployment — that is per-connection RAM, a per-connection routing-table entry, and a collect obligation, bought for an addressing capability the node never uses because it never routes *to* an individual peer. Defaults are for the common case, and the common case is FLAT.

**Reason 3 — a shipped public-header default is an interop commitment.** `peer_named=false` is baked into every deployment built against a released header and into every SPEC-created listener whose config omits the key. Flipping it would silently change a stock listener's graph shape and its peer-census behaviour on upgrade — a breaking interop change — in exchange for a payoff Reason 1 has already removed. There is nothing to weigh against the cost.

## Guidance for integrators

The reference docs described both mechanisms but never said how to reason about holding both. The rule:

- **Do you need to address, ACL, or tear down an individual peer by name?** (Reply to *this* browser tab; enumerate peers per [ADR-0044](0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md); write an ACE naming one peer; `bus_link_t::close_peer` a specific session.) → **`peer_named=true`**, and budget the per-peer anchor vertex and the `collect()` point.
- **Do you need to know who wrote something, in order to decide whether to allow it or to record it?** → **the subject**. Read `write_ctx_t::subject` in the handler. This is the authorization answer, and once Part 2 lands it works at either setting of `peer_named` — do not turn the flag on to obtain it.
- **Both?** Set both. They are independent; there is no interaction to reason about beyond the cost of the first.
- **Neither, i.e. a point-to-point link?** The default is already right: FLAT, no per-peer state, and the link's own registered child name is the hop name.

The failure mode this guidance exists to prevent is treating a peer *name* as an identity claim. A name is assigned by the local transport at accept; it attests to nothing about the far end. Only a subject bound by an authentication check ([ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md), and the WebSocket session-auth frame) carries a claim worth authorizing against.

## Considered options

- **Flip the default to `true`.** Rejected on all three reasons above: no remaining payoff, wrong for NARROW, breaking for a shipped surface.
- **Collapse `peer_named` into the subject machinery — derive the flag from whether a subject seam is installed.** Rejected: it re-creates the exact conflation this ADR names. A node can authenticate every writer and still have no reason to make any of them individually routable, and a node can need to reply to a specific tab that presented nothing. Deriving one from the other forces a deployment to buy the cost of whichever it did not want.
- **Make `peer_named` role-derived (`true` for listeners, `false` for dialers).** Rejected: it is still a default flip for exactly the deployments Reason 2 covers — the MCU that listens for one upstream is a listener, and would start paying per-peer state.
- **Make it a required SPEC parameter with no default.** Rejected: it breaks every existing config that omits the key, to force a choice most deployments correctly do not care about.
- **Retire the flag and always expose the bus facet.** Rejected as the maximal form of the flip: it makes the per-peer anchor and its collect obligation unconditional, which [ADR-0044](0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)'s statelessness commitment and the NARROW target both refuse.

## Consequences

- **[#1278](https://github.com/avatarsd-llc/libtracer/issues/1278) closes with no code change.** The default is ratified where it stands; a future PR proposing a flip cites this ADR or supersedes it.
- **[#375](https://github.com/avatarsd-llc/libtracer/issues/375)'s deliverable 4 is discharged, and deliverable 3's CONSUMER half has since landed.** The prediction here held: `tr::graph::default_config_t::kBusLinks` closes the peer-named tier out at build time per [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) §1, bound by an [ADR-0068](0068-build-configuration-is-plain-cpp-config-header.md) override fragment, and a default-`false` flag on a NARROW target is exactly the module set it closes — measured **−2,078 B of flash, ±0 B of `.bss`** on rv32. What that PR did *not* take, and what remains open on the issue, is the PROVIDER half (hoisting `slot_server_t`'s bus facet into a derived type so a flat listener carries neither the base subobject nor its vtable slots) and the [ADR-0079](0079-allocation-store-composition-defaults-to-per-plane-mid.md) injected per-peer bound that must pair with `peer_named = true` where the tier IS present.
- **[ADR-0044](0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md) is confirmed, not amended.** Its 2026-08-13 amendment already scoped "no vertices for peers" to announce-census peers and admitted an accepted session as an ordinary vertex; this ADR adds only that the switch admitting them is an *addressing* switch, and that its cost is why it stays opt-in. The conflation [#375](https://github.com/avatarsd-llc/libtracer/issues/375) flagged — "peer names double as routable segments" — is resolved by naming the two claims separately rather than by rewriting ADR-0044's routing model.
- **The Part 2 car ([#375](https://github.com/avatarsd-llc/libtracer/issues/375) Part 2 fused with [#1266](https://github.com/avatarsd-llc/libtracer/issues/1266)) inherits an explicit requirement**: the subject it derives from `peer_handle_t` must be available at `peer_named=false`. A realization that only produces a subject on a bus link would re-couple the two claims and contradicts this ADR.
- **The ACL subject table keys off the subject alone.** It never needs to consult `peer_named`, and a subject-table entry is not a routing fact.
- **Reference docs carry the two-claims sentence** so an integrator meets it where they meet the session subject — see [`docs/reference/16-websocket-session-auth.md`](../reference/16-websocket-session-auth.md) §The subject.
