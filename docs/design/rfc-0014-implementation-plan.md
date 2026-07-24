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
| Connection path | flat `/net/<name>` | nested `/net/<transport>/<name>` |
| Creation surface | one global `client`/`listener` catalog, a `:children[]` target on `/net` (`graph.cpp:1521`); `quic` extends it via `register_transport_type` | a **per-transport `conn` endpoint vertex** owning its own catalog |
| Catalog read | none; `:schema` is whole-vertex-only (`graph.cpp:1955`, `size()==1`) | `read /net/<transport>/conn:schema` returns the module's config catalog |
| Removal wire path | none (`retire()` has "no wire path") | `write NAME{name}` → `retire()` |
| Gating | `CREATE` (0x08) enforced on `:children[]`/write-creates (`graph.cpp:1401`,`:475`,`:956`) | `SPEC`→`CREATE` (relocated to endpoint ACL); `NAME`→`WRITE` (0x02) |
| Link state | manual binary `set_link_state(name,bool)` (`transport_vertex.hpp:227`) | 6-state liveness enum as the vertex value, propagating |
| Liveness engine | none — no refcount, dormancy, dial, or self-heal | the full state machine (§4 of the RFC) |
| conn settings | `addr/port/role/keepalive_ms/max_frame/kind` | `+ backoff, connect_timeout` |
| Enumeration hide | none (a registered child always appears in `:children[]`) | a hide seam for `conn` |

`retire()` (RFC-0009) is reused unchanged. The whole liveness engine, the per-transport endpoint,
the per-transport catalog `:schema`, and the enumeration-hide seam are **greenfield**.

## Design spike 0 — reconcile per-transport nesting with flat FWD routing (BLOCKS everything)

Connections move from flat `/net/<name>` to nested `/net/<transport>/<name>`, but the FWD router
resolves a `dst` by descending segments and matching against the **child-link registry**
(`fwd_router.hpp`; `router_.add_child(name, link)` is keyed by bare name today; hardened by #373/#419).
Resolve **before any code**:

- **Recommended:** the connection **vertex** nests under `/net/<transport>/<name>`, while the
  **router child** stays the flat bare `<name>` (the link object already knows its transport). The
  FWD walk descends the structural `net`/`<transport>` segments and matches `<name>` against the flat
  registry — the model CONTEXT.md §84 already describes ("the `/net/<name>` NAME is the router child a
  `dst` routes through"), now one level deeper. **Cost:** connection names must be **globally unique
  across transports** on a node (acceptable — a name is a logical handle, transport is discoverable
  from its vertex path). No router-registry change.
- **Alternative (rejected unless the spike finds a blocker):** transport-qualify the router key
  (`<transport>/<name>`), changing the router's segment resolution. Bigger blast radius on the
  hardened routing path.

Deliverable: a short ADR or spike note confirming the recommended split routes correctly for
single- and multi-hop `dst`, and that `read/await` of the connection **vertex** (`/net/<transport>/<name>`)
still reaches the local vertex (not forwarded). Gate the rest of the plan on it.

## Slices (each an independent, CI-gated PR; order respects dependencies)

**S1 — Liveness value: binary → enum (behavior-neutral).**
Widen the connection vertex's value from the binary up/down to the 6-member enum
(`dormant/dialing/reconnecting/up/listening/bind-failed`) — same 1-byte slot, same VALUE-write +
`write_seq` bump + await-fire, so this is a clean encoding change. Keep it manually set for now
(callers still drive it), and make it **propagate** (assign+propagate under RFC-0008) so subscribers
stream transitions. Add `backoff`/`connect_timeout` to `conn_settings_t`. *No engine yet.* Host-testable.

**S2 — The creator-endpoint vertex + `SPEC`/`NAME` dispatch (control plane).**
Per design spike 0. Each transport module mounts a `conn` child on its transport vertex; move
`register_child_type`/`register_transport_type` to populate the **per-transport** catalog rather than
the single global `:children[]`. Dispatch: `SPEC`⇒create (validate name non-empty/non-reserved +
config vs catalog; `PATH_IN_USE` on existing; atomic create at `/net/<transport>/<name>`), `NAME`⇒remove
(resolve; no-op on absent; reject reserved incl. `conn`; else `retire()`), any-other-payload⇒`type_mismatch`.
Gate `SPEC` on `CREATE` (relocated to the endpoint's ACL), `NAME` on `WRITE`. Host-testable
(create/remove/reserved/bad-payload/gating). **This is the ADR-0059 surface** and retires the
`:children[]` creation spelling.

**S3 — Catalog `:schema` + discovery.**
Make `read /net/<transport>/conn:schema` return the module's config catalog — the endpoint vertex's
**own** schema (ADR-0059 §Decision 2: its structure *is* "what I accept"), which requires teaching the
`:schema` read to serve a catalog for this vertex kind (today it emits settings + the RFC-0010
app-field table). `/net:children[]` enumerates transports; absent endpoint → `PATH_NOT_FOUND` (falls
out of path resolution). Host-testable.

**S4 — Enumeration-hide seam.**
Mark the `conn` endpoint hidden from `/net/<transport>:children[]` (which returns member connection
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
`/net/export` / `:children[]` model (05, 11, 13) to the per-transport `conn` model + the liveness
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

Spike 0 → S1 (parallel with the spike) → S2 → S3 → S4 → S5 → S6 → S7 → S8. S5 is the critical-path
risk (greenfield concurrency); everything downstream of the control plane (S2) can proceed once the
spike unblocks the path shape.
