# Naming authority: the application mints, one predicate gates every boundary

Status: **accepted; not yet implemented** (tracking: [#426](https://github.com/avatarsd-llc/libtracer/issues/426), [#688](https://github.com/avatarsd-llc/libtracer/issues/688), [#621](https://github.com/avatarsd-llc/libtracer/issues/621), [#491](https://github.com/avatarsd-llc/libtracer/issues/491); declined: [#622](https://github.com/avatarsd-llc/libtracer/issues/622)). Grill ruling of 2026-08-01 over the five-issue naming cluster.

## Context

Five open issues turned out to be one question — *who may mint a name in the graph, and must every minted name be expressible in the addressing grammar?* — answered inconsistently at every boundary:

| boundary | today's minting | defect |
| --- | --- | --- |
| bus transports (ws, tcp) naming accepted peers | `<ip>:<port>` — contains two reserved characters | enumerable but unaddressable; **taints the accumulated return route**, so a reply cannot walk back (#426) |
| wire `create_child` accepting a peer-supplied name | any non-empty bytes | a peer mints `a/b` or `x:y`; the vertex registers, lists, and can never be addressed by a conforming client (#688) |
| `module_for` deriving `<kind>-client` for unregistered kinds | core invents path text for transports it has never heard of | the application does not own its own path shape (#621) |

`reference/03` already states the rule as a MUST — reserved characters (`/ : . * ?`; brackets deferred to address-index parsing) must not appear inside segment bytes — but only `path_t::parse`, the *local string* tier, enforces it. The recurring defect is the drift between tiers (same shape as #681), not any single missing check.

## Decision

### §1 — The invariant: a name that enters the graph MUST be expressible in the addressing grammar

Enumerable implies addressable — a vertex the graph will list is a vertex a conforming `dst` can reach. Enforcement is **total at every minting boundary** through **one shared predicate** (the segment-validity check `path_t::parse` already applies: reserved set, `kMaxSegmentBytes`, and segment count where composition is possible), never per-site copies. The predicate is the single place the reserved set ever changes. A wire-supplied name that fails it answers `INVALID_PATH`, the same status the local tier answers for the same bytes.

The rejection is wire-visible for #688 — a create that used to "succeed" now errors. That is the point, not a cost: what it created was unreachable, and the protocol is DRAFT.

### §2 — Session naming is two-tier: creator-chosen, else node-assigned `p<slot>`

- A connection created through the control vertex (the RFC-0014 creator endpoint) carries its **creator-chosen name**, exactly as today — preserving retried-create idempotency (`PATH_IN_USE`, not a duplicate connection).
- A session with **no creator** — a bus listener accepting an inbound peer — gets the node-assigned fallback **`p<slot>`** (the slot index is already the identity the transport uses; legal by construction; shortest possible `src` bytes on a string that rides every bus-ingress return route).
- The `<ip>:<port>` string leaves the graph: transport diagnostics only. If [#584](https://github.com/avatarsd-llc/libtracer/issues/584) rules per-peer facets in, the endpoint string is their first tenant — this ADR deliberately does not mint the first facet as a rename side effect.
- Documented semantics: *a fallback bus peer name identifies a session, not a device.* A peer that reconnects may land in a different slot. Device-stable identity is a named link — which was already true of the ephemeral-port name this replaces.

Rejected: a sanitized `ip_port` name (longer wire segments, needs a canonical collision-free encoding, and it lies about stability across slot recycling).

### §3 — A bus link's NAME is not a routable next-hop

Routing a `dst` through a bus link's own connection NAME today **broadcasts** (`transport_ws_server::send` fans out to every open peer), so one directed request draws N replies and scrambles FIFO reply correlation — the failure that broke the #409 topology walk. Ruled: an inbound FWD whose next hop is a bus link's NAME is **rejected**; only the link's *peer names* route. A `dst` is a directed route to one terminus; fan-out belongs to the subscription plane, where N deliveries exist because N subscribers asked. Rejected alternatives: keep-and-document (breaks one-request-one-reply by construction) and verb-dependent routing (write broadcasts, read rejects — makes addressability depend on the operation, which no other hop does).

This is spec-touching (RFC-0004 §B territory) and lands through the RFC instrument, not a silent patch.

### §4 — Modules are declared-only, by the application; `/net` is a recommendation

1. **`module_for`'s derived `<kind>-client` / `<kind>-server` fallback is removed.** Creation with an unregistered kind fails explicitly with `SCHEMA_NOT_FOUND` (the unknown-SPEC-`type` convention).
2. **No library-side auto-registration.** Linking a built-in transport registers nothing; the application explicitly registers each module under a name it chooses (built-ins keep exporting factories and may ship a suggested-name constant, but the `register_module` call is always application code). Registration is a minting boundary, so §1's predicate runs there.
3. **`/net` is demoted to a documented naming recommendation** — a convention for interop legibility, never a library rule. It is already only a constructor default; wording that implies it is structural (RFC-0014 §1 "mounted flat under `/net`") gets an erratum-grade correction.

**BREAKING twice** (derived names gone; auto-registration gone). The consumer fix is one explicit `register_module` call per module — which is the point: every module segment in the graph then traces to an application decision. Rides the v0.7.0 cut.

### §5 — Node-assigned connection names for the creator path: declined (#622)

The amendment's only gain — node-assigned naming — now exists where it is coherent (§2's creatorless fallback), while the creator path keeps the property the amendment would have destroyed: a retried create over a lossy link answers `PATH_IN_USE` instead of silently minting a second connection. An application that wants nameless-append semantics names its connections `"0"`, `"1"`, `"2"` today — zero protocol change, nothing imposed. Revival condition recorded in `.out-of-scope/node-assigned-connection-names.md`: a demonstrated need for node-assigned names *with* retry safety, whose correct shape is a client-supplied idempotency token separate from the name — strictly more machinery, which is why the status quo wins.

## Consequences

- The third-party orchestration recipe (#491) becomes fully computable **offline**: an orchestrator C creates the B→A link through B's creator endpoint under a name *C chose* (§2's creator tier — this is what §5 preserved), then writes the subscription with the B-rooted target it can compose from that name, then departs. The topology walk demotes from prerequisite to discovery aid, needed only for links the orchestrator did not create. ADR-0026 already makes the subscribe-write identical for consumer, firmware, and orchestrator — no new surface, no companion RFC.

  > **Erratum, 2026-08-01 (#491) — the last sentence of the bullet above is WRONG.** A conformance test built to this recipe (`test/491-orchestrate-and-depart`) refutes it at the subscription step, and the refutation was verified against `main`:
  >
  > 1. The wire `:subscribers[]` door executes **`s.target_key.reset()`** (`core/src/graph.cpp:1448`) — *"a PATH child names the consumer at ITS origin — never a local re-dispatch target"* — so the B-rooted target the orchestrator composed is **discarded**, and the edge binds to `(caller = the inbound session, return_route = the accumulated src)`. The producer's updates are delivered to **the orchestrator**, not to A.
  > 2. On the orchestrator's departure, `fwd_router_t::link_down` → `graph_t::evict_link_edges` (`core/src/fwd_router.cpp:482`) removes that edge, so nothing survives at all.
  > 3. The dual that *does* resolve a mount-path target, `fwd_router_t::subscribe_toward` (#739), is **host-local only** — not reachable from the wire.
  >
  > What survives: the *creation* half (step 1 — a remote creator write under a creator-chosen name) works end to end, and offline computability of the target **string** is real. What fails is the inference that an identical write yields an identical *binding* — the binding is derived from the **arrival session**, which differs for a third party. ADR-0026 is not contradicted; the conclusion drawn from it here was.
  >
  > **A companion RFC IS needed** (RFC-0004 §D territory): a ruled behaviour for a wire SUBSCRIBER whose target routes through a mount — presumably resolving it through the ADR-0061 descent and storing the mount link, so the edge outlives the writer. Tracked on #491, which is back in `needs-triage`. `reference/13` §Boundaries already flagged this gap; its open-question wording was the accurate one.
- The #409 walk's `routable: false` special case for bus links becomes removable once §2 and §3 land.
- One predicate to maintain; a future minting boundary (e.g. #603's label tables, if labels ever surface as names) adopts it rather than re-deriving the rule.
