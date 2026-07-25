<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC-0014 implementation plan — creator endpoint & link liveness

Execution plan for [RFC-0014](../spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)
(accepted 2026-07-24). Descriptive/planning doc — the RFC is the normative source; where they
disagree, the RFC wins.

## Baseline — what exists vs. what is new

Verified against `main` during the RFC's adversarial pass:

| Area | Today | RFC-0014 needs |
| --- | --- | --- |
| Connection path | flat `/net/<name>` | per-module `/net/<module>/<name>` (ADR-0061) |
| Creation surface | one global `client`/`listener` catalog, a `:children[]` target on `/net` (`graph.cpp:1521`); `quic` extends it via `register_transport_type` | a **per-module `conn` endpoint vertex** owning its own catalog |
| Catalog read | none; `:schema` is whole-vertex-only (`graph.cpp:1955`, `size()==1`) | `read /net/<module>/conn:schema` returns the module's config catalog |
| Removal wire path | none (`retire()` has "no wire path") | `write NAME{name}` → `retire()` |
| Gating | `CREATE` (0x08) enforced on `:children[]`/write-creates (`graph.cpp:1401`,`:475`,`:956`) | `SPEC`→`CREATE` (relocated to endpoint ACL); `NAME`→`WRITE` (0x02) |
| Link state | manual binary `set_link_state(name,bool)` (`transport_vertex.hpp:227`) | 6-state liveness enum as the vertex value, propagating |
| Liveness engine | none — no refcount, dormancy, dial, or self-heal | the full state machine (§4 of the RFC) |
| conn settings | `addr/port/role/keepalive_ms/max_frame/kind` | `+ backoff, connect_timeout` |
| Enumeration hide | none (a registered child always appears in `:children[]`) | a hide seam for `conn` |

`retire()` (RFC-0009) is reused unchanged. The whole liveness engine, the per-module endpoint,
the per-module catalog `:schema`, and the enumeration-hide seam are **greenfield**.

## Design spike 0 — RESOLVED → [ADR-0061](../adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)

Spike 0 (reconcile the per-module `/net/<module>/<name>` nesting with the flat FWD child-link
registry) is **resolved**; ADR-0061 (proposed) is the full mechanism and rationale. In brief:

- **No RFC needed for the routing shape.** Routing-address `==` vertex-path is **already the normative
  intent** — the `fwd-routed-multihop` conformance vector encodes the `/net`-mount form and `v1.md`
  pins no MUST on the first-`dst`-segment shape — so aligning the flat bare-name impl is a **#419
  conformance fix**. (Adversarially confirmed against the vectors + `v1.md`.)
- **The mechanism stays entirely in L5.** The router peek recognizes the `/net/<module>` structural
  prefix by offset (**strip-K**), `child_registry_t` is **kept and re-keyed per-module** (its
  cross-bus peer fallback narrowed to a **per-endpoint `resolve_peer`**), and the L4 vertex stays
  transport-blind — **no `graph.find` on the forward path**, hence no `map_mutex` regression. The
  earlier "keep the router flat, nest only the vertex, global name uniqueness" recommendation is
  **superseded**: names are per-module-scoped, and the bus-peer shape is read **structurally** from
  the module (multi-peer module → a peer segment resolved in its own peer table; point-to-point →
  none). The "make the graph the resolver / dissolve the registry" idea was **rejected** (inverts
  L4↔L5 per ADR-0016; black-holes vertex-less bus peers).

**ADR-0061 acceptance (maintainer grill, 2026-07-25).** S2 **stays blocked on ADR-0061 reaching
`accepted`**: a `conn` endpoint that cannot express `/net/ws-client/foo` alongside
`/net/tcp-client/foo` is a half-surface, so per-module scoping is intrinsic to the creator endpoint,
not a follow-on. Of the ADR's four original conditions, **three were circular** — they could only be
cleared by work the ADR itself gates — and are demoted to implementation obligations (bus-peer
`resolve_peer` + black-hole negative test → S2a; vector rewrites → S7; registry teardown → #494).

**The one live gate is a baseline-first forward-hop bench**, landed standalone against *today's*
flat `by_name`, requiring no strip-K code. It extends the `bench_forward_heap` rig (already a full
router + wired transport + `on_frame()` feed; it lacks only timing and a size sweep) and measures
two axes: the **fixed per-hop cost at N=1** — the only term strip-K adds (the registry-size-
independent structural prefix walk) — and the **scan share swept over N links per module**, the term
strip-K *narrows* (per-module scoping shrinks both of `by_name`'s passes). ADR-0061 accepts once the
N=1 cost shows headroom for two extra segment compares; `allocs=0` + scoped-≤-flat become merge
gates on S2a.

**Immediately actionable, in parallel, today:** the #494 `erase` PR and the baseline bench PR.
Neither depends on the other or on acceptance.

## Slices (each an independent, CI-gated PR; order respects dependencies)

**S1 — Liveness value: binary → enum (behavior-neutral).**
Widen the connection vertex's value from the binary up/down to the 6-member enum
(`dormant/dialing/reconnecting/up/listening/bind-failed`) — same 1-byte slot, same VALUE-write +
`write_seq` bump + await-fire, so this is a clean encoding change. Keep it manually set for now
(callers still drive it), and make it **propagate** (assign+propagate under RFC-0008) so subscribers
stream transitions. Add `backoff`/`connect_timeout` to `conn_settings_t`. *No engine yet.* Host-testable.

**S2 — The creator endpoint (control plane), decomposed S2a → S2b → S2c.**
Blocked on ADR-0061 `accepted`. The split is placement+routing / dispatch / gating.

**S2a — Per-module placement + the ADR-0061 routing impl (one atomic edit).**
Move the connection vertex to `/net/<module>/<name>` **and** re-key the router in the same PR: today
`make_connection` registers `router_.add_child(name, *link)` on the *bare leaf*
(`core/src/transport_vertex.cpp:218`) while composing the vertex key separately, so placement and
routing are the same change to `child_registry_t` and the three `fwd_router` demux sites
(`on_frame_impl`, `on_frame_rope`, `on_advertise`) — splitting them would open a window where the
vertex path and the routing key disagree, the very divergence (#419) this closes. Carries: strip-K
peek in `fwd_frame_view.hpp`, the per-module re-key, the **per-endpoint `resolve_peer`**, the
**multi-peer black-hole negative test** (`/net/ws-server/s/alice` reaches ws-`alice`;
`/net/ws-server/s/zzz` → clean `PATH_NOT_FOUND`; `/net/tcp-server/s`'s own `alice` unreachable
through the ws module), the #373 `has_first_level_child` guard's fate, and the scoped-vs-flat bench
delta. **Merge gates:** `allocs=0` on the forward hop; scoped hop ≤ flat hop at equal total links.

**S2b — The `conn` endpoint vertex + `SPEC`/`NAME` dispatch.**
Each transport module mounts a `conn` child on its transport vertex; move
`register_child_type`/`register_transport_type` to populate the **per-module** catalog rather than
the single global `:children[]`. Dispatch: `SPEC`⇒create (validate name non-empty/non-reserved +
config vs catalog; `PATH_IN_USE` on existing; atomic create at `/net/<module>/<name>`), `NAME`⇒remove
(resolve; no-op on absent; reject reserved incl. `conn`; else `retire()`), any-other-payload⇒`type_mismatch`.
**This is the ADR-0059 surface** and retires the `:children[]` creation spelling.

**S2c — Gating + the negative-test matrix.**
Gate `SPEC` on `CREATE` (relocated to the endpoint's ACL), `NAME` on `WRITE` (per RFC-0009 §A.2 —
`WRITE`, not `DELETE`). Land the create/remove/reserved-name/bad-payload/gating host tests.

**S3 — Catalog `:schema` + discovery.**
Make `read /net/<module>/conn:schema` return the module's config catalog — the endpoint vertex's
**own** schema (ADR-0059 §Decision 2: its structure *is* "what I accept"), which requires teaching the
`:schema` read to serve a catalog for this vertex kind (today it emits settings + the RFC-0010
app-field table). `/net:children[]` enumerates transports; absent endpoint → `PATH_NOT_FOUND` (falls
out of path resolution). Host-testable.

**S4 — Enumeration-hide seam.**
Mark the `conn` endpoint hidden from `/net/<module>:children[]` (which returns member connection
vertices). Cheapest path: reuse the ADR-0057 placeholder-exclusion already applied to unfilled
intermediates, or add a per-vertex "hidden" bit. Host-testable (enumerate → members only, no `conn`).

**S5 — The link-liveness engine (the big one; concurrency-sensitive, TSan-gated).**
Refcount per `DIAL` link (standing subs/awaits routed through + transient one-shot holds; per-hop
local). The state machine: `dormant→dialing→up`; `up→reconnecting→up`; refcount→0→`dormant`; self-heal
loop with `backoff` iff refcount>0; auto-wake-on-op (dial, wait one `connect_timeout`, serve or
`link-down`); **transient hold released before self-heal is evaluated**; fail-fast on down; retries
forever while refcount>0. `LISTEN`: `listening`/`bind-failed`, ignores refcount, up-until-retire.
Drives the S1 enum value. **Synergy with #486:** a dormant `DIAL` link has **no recv thread** — the
socket + its thread exist only while up, so #486's per-transport stack sizing applies exactly to the
threads that exist. Build under ASan + **TSan** (refcount vs recv-thread lifecycle); a
retire-vs-liveness-transition concurrency test.

**S6 — Multi-hop reach + `await`-on-up semantics.**
Confirm `conn:schema`, the liveness value, and `SPEC`/`NAME` writes address correctly over multi-hop
FWD (read `/net/<linkToB>/net/can/conn:schema`), each hop gating independently. Make `await` on a
connection vertex resolve specifically when liveness reaches `up` (and return `link-down` on terminal
failure), not a generic await-next-transition.

**S7 — Migration + reference-doc sync + conformance vectors.**
Retire the flat `:children[]` creation spelling. Update the reference docs that describe the old
`/net/export` / `:children[]` model (05, 11, 13) to the per-module `conn` model + the liveness
enum. Land the conformance vectors named in the RFC (`create-via-SPEC`, `remove-via-NAME`,
`remove-nonexistent-noop`, `remove-reserved-rejected`, `spec-name-in-use`, `bad-payload-type`,
`catalog-read`, `absent-endpoint-PATH_NOT_FOUND`, `gate-CREATE`, `gate-WRITE`, the liveness transitions).
Only after the vectors land do the RFC's byte clauses become normative (the clause-kind rule).

**S8 — strawberry web UI (strawberry-fw side).**
Un-stub `network-page.component`'s create/delete (write `SPEC`/`NAME` to a remote board's `conn`
endpoint over the WS link, addressed multi-hop). Render the liveness enum on the status dots
(subscribe the connection vertex, apply the UI's own "unreachable" display threshold to `reconnecting`).

## Out of scope (named follow-up, NOT RFC-0014)

**The d2d-subscription-origination surface.** RFC-0014 delivers the *link*; the cross-device *wire*
is a subscription — "make board A subscribe to board B and depart." Until that surface exists (its own
investigation: is third-party multi-hop `SUBSCRIBER` origination already expressible, or does it need
a companion RFC?), the web UI can create/tear **links** but the browser still relays the **wires**. This
is the remaining gap on the full #407 / browser-relay-retirement story. Tracked as **#491**; S8's web
UI work must know the wire half is not yet available.

## Suggested sequencing

**(#494 `erase` ∥ baseline forward-hop bench ∥ S1)** → ADR-0061 `accepted` → S2a → S2b → S2c → S3 →
S4 → S5 → S6 → S7 → S8.

The three leading items are independent of each other and of acceptance, so they start now; only the
bench feeds the acceptance decision. S2a is the ADR-0061 impl and the routing key it settles is what
S2b/S2c build on. S5 remains the critical-path risk (greenfield concurrency).
