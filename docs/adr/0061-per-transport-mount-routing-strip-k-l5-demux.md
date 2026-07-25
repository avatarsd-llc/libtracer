# Per-module mount routing: routing-address `==` vertex-path, realized as a strip-K structural descent in the L5 demux over a per-module-scoped registry — refining ADR-0037/0038

Status: proposed. **Refines [ADR-0037](0037-net-side-channels-dissolve-into-vertex-tree-compositor.md)** (dissolve the side-channel) and **[ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) §3b** (the `child_registry_t` brick) — it is their concrete strip-K + scoped-registry realization, not a supersession. **Upholds [ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md)** (an L4 `vertex_t` carries no `transport_t*`; `graph.find` stays mount-unaware) and **ADR-0038 §3a** (the intra-device path pays nothing). The wire-visible byte change is governed by the already-accepted **[RFC-0014](../spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)** ([#492](https://github.com/avatarsd-llc/libtracer/issues/492)) — **no new RFC or amendment**. Grounded by a `/grill-with-docs` session, a deep-dive, and a three-lens spike whose load-bearing claims were adversarially verified against the code and the conformance vectors. **One condition remains before `accepted`: the forward-hop baseline bench** (see Consequences). A maintainer grill on 2026-07-25 triaged the four original conditions — three were circular (they could only be cleared by implementation this ADR itself gates) and are demoted to implementation obligations; the mechanism is settled, its hot-path affordability is not.

## Context

[RFC-0014](../spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md) makes connections runtime-dynamic and nests them in per-*(transport, role)* modules (`/net/<module>/<name>`), so `/net/ws-client/foo` and `/net/tcp-client/foo` must coexist. Today's FWD demux keys the connection by its **bare leaf NAME** at the `dst` root, which is the direct cause of the [#373](https://github.com/avatarsd-llc/libtracer/issues/373) first-level shadow-guard, the [#419](https://github.com/avatarsd-llc/libtracer/issues/419) doc-vs-impl divergence, and the impossibility of that coexistence.

A grill framed this as "routing is path polymorphism; the runtime routing table is a duplicate of the runtime graph — make the graph the resolver." That intuition is **directionally right about the addressing** (routing-address should equal the vertex-path) but **wrong about the mechanism**, for three reasons a deep-dive and an adversarial verify pass pinned against the code:

- **The forward decision cannot move onto the L4 graph.** A `vertex_t` may not carry a `transport_t*` ([ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md)); `graph.find(child) → transport` was already litigated-and-rejected in [ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) §3b.1. Putting an "is-mount" marker on the vertex and injecting an L4→L5 forward sink re-courts exactly that inversion.
- **Bus peers have no vertex at all.** A multi-peer `ws`/`tcp`/`can` peer is synthesized on read; `transport_vertex.cpp:178-180` — "NO vertex is ever created for a peer." Pure graph-descent black-holes every peer. The registry's `by_name`/`peer_link` fallback (`child_registry.hpp:52-67`) is the only thing that reaches them, and `by_name` is used pervasively on the reply/advertise paths (`fwd_router.cpp:151,163,356,383,481,497,508,529`) with no graph substitute.
- **So the registry is not a removable duplicate.** It holds the two things the graph genuinely cannot: the `transport_t*` binding (L5-only) and the vertex-less bus-peer table. What it *duplicates* is only the mount **namespace** — and it is merely **mis-keyed** (bare `<name>` at root instead of the mount-path). It is add-only besides: no `erase`, and `link_down` never touches it, so a removed link leaves a dangling `name → transport_t*` — a live use-after-free once RFC-0014's remove-half ([#407](https://github.com/avatarsd-llc/libtracer/issues/407)) lands.

**Spec status (adversarially confirmed against the artifacts):** `v1.md` pins **no MUST** on the first-`dst`-segment shape (it incorporates only the reference/03 path-syntax constraints + reference/05); `reference/03:216` marks `/net/<conn>/…` a *convention*, not normative. The normative conformance vector `fwd-routed-multihop/expected.json` **already encodes the mount form** (`dst=/net/board/can0/ow/sensor`, `net` as `segment[0]`), decaying segment-by-segment in `fwd-src-accumulated`. So the flat bare-name impl is the divergence, and aligning it to the mount form is a **conformance fix (#419), not an RFC**. The per-transport two-level nesting is governed by the **already-accepted RFC-0014** ([#492](https://github.com/avatarsd-llc/libtracer/issues/492)); its only spec cost is rewriting the vector *bytes* under #492's draft-spec authority.

*(An earlier draft of this ADR proposed the graph-resolver / L4-mount-marker model above. The adversarial pass rejected it; this revision records the L5-only mechanism that survives contact with the layering law and the bus-peer case.)*

## Decision

**Routing-address `==` vertex-path (the mount form), realized as a generalized strip-K demux that stays entirely in L5.**

1. **Addressing is compositional.** A remote operation is addressed by its vertex-path, walking *through* the flat per-*(transport, role)* module `/net/<module>` (`ws-client`, `ws-server`, `can`, … — [RFC-0014](../spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)). This is the grill's goal and the already-normative intent (the vectors + ADR-0037/0038). Flat-module nesting gives `/net/ws-client/foo` ≠ `/net/tcp-client/foo` for free, because the routing key *is* the path.

2. **The mechanism stays in L5.** The FWD demux generalizes in three lockstep pieces:
   - **peek** — instead of peeking exactly the first `dst` segment (`fwd_frame_view.hpp:105-123`), recognize the **fixed structural prefix** by offset: `segment[0]==net` (literal), then `<module>` (one of the link-time-closed module set — `ws-client`, `ws-server`, `can`, …) are **descended but not forwarded**, then resolve the `<name>` segment against the **per-module-scoped** registry. Because the module declares its **shape**, the demux knows *structurally* whether a peer segment follows: a **multi-peer** module (`ws-server`, `can`) resolves the next segment as a peer in that module's own peer table (strip K+1); a **point-to-point** module (`ws-client`) forwards the residual directly (strip K). No runtime `bus()` probe — the module segment carries the shape.
   - **strip-K** — `rebuild_fwd_forward` strips the K-segment local run at the mount hop (K = structural-depth-to-mount) rather than exactly one segment; `gather()` stays offset-only zero-copy and `src` still grows by **only** the single inbound-link `NAME` — the local mount prefix never rides the return route.
   - **scoped registry** — `child_registry_t` is **kept**, re-keyed from bare `<name>` to `<module>/<name>`, and its global cross-bus `peer_link` fallback narrows to a **per-endpoint** `resolve_peer` (scoped to the resolved multi-peer module — which is also what makes two servers' same-named peers distinct). `by_name`/`by_segment` mechanics are otherwise unchanged. All three demux sites move together: `on_frame_impl` (`fwd_router.cpp:249`), `on_frame_rope` (`:194`), `on_advertise` (`:440`).

   The L4 `vertex_t` stays **transport-blind** and `graph.find_ptr` stays **mount-unaware**; there is no is-mount marker and **no `graph.find` on the forward path** — hence no `map_mutex` per-hop regression. The mount binding is the opaque L5 registry link, never a raw pointer on a vertex.

3. **The registry stays; it cannot dissolve.** Bus peers have no vertex and the reply/advertise paths need `by_name`. Only the forward-demux *key* widens to per-module scope, and the peer fallback narrows from global to per-endpoint.

4. **Layering and intra-device invariants are upheld.** `transport_t*` stays in L5 (ADR-0016; ADR-0038 §3b.1). A local `dst` still descends to a local vertex and terminates — the forward arm is reached only for a `dst` that names a transport mount (ADR-0038 §3a, a hard invariant).

## Considered options

- **L4 mount-marker on the vertex + an injected L4→L5 forward sink** (this ADR's own first draft, mirroring `set_remote_delivery_sink`). **Rejected:** adds an L4 seam that re-courts the ADR-0016 inversion, and graph descent *still* cannot reach bus peers, so the registry stays anyway — pure cost, no payoff.
- **Dissolve the registry into graph descent** (the grill's "make the graph the resolver," claimed O(3·log N) < O(N)). **Rejected:** black-holes bus peers (no vertex) and loses the reply-path `by_name`; the "faster" claim assumed a mechanism that does not hold.
- **Keep the flat bare-name-at-`dst`-root demux.** **Rejected:** contradicts ADR-0037/0038 and the already-vectored intent, and cannot express per-transport scoping.
- **Strip segment-by-segment** (`net`, then `<transport>`, then `<name>` as separate hops). **Rejected:** multiplies head rebuilds; strip-K-at-once matches NFS `follow_mount` and keeps one head-rebuild per hop.
- **Compile-time dispatch over the closed `{udp,tcp,ws,can}` set.** **Rejected by [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) §4:** net-plane control structures stay runtime-dispatched because the connection kind arrives as runtime `:children[]` data.

## Consequences

**The one condition to clear before this moves from `proposed` to `accepted`:**

- **UNCONFIRMED — hot-path affordability. A baseline bench answers it *without* strip-K code.** The
  per-hop fixed-depth structural walk + per-module-scoped linear scan over the immutable `children_`
  vector is bounded and zero-heap, but the net plane is latency-critical and
  [#386](https://github.com/avatarsd-llc/libtracer/issues/386) showed wide-fanout regression
  sensitivity. Today's benches do **not** cover this: `bench_forward_heap` measures per-hop *heap*
  (0 allocs — a self-policing CI gate the scan must preserve) and `bench_fanout_clone_storm`
  measures *refcount* contention — **neither times the forward-demux scan against registry size**.

  The original phrasing ("a new micro-bench … landed with the strip-K PR") was **circular**: it
  gated acceptance on code that acceptance gates. It is replaced by a **baseline-first bench**,
  landed standalone against **today's flat `by_name`**, extending the existing `bench_forward_heap`
  rig (which already stands up router + wired transport and feeds a frame straight into
  `on_frame()` — it lacks only a timing window and a size sweep). It measures **two axes**, because
  the two terms move in opposite directions:

  1. **Fixed cost at N=1 link** — isolates the constant term. This is the *only* term strip-K
     **adds**: the structural prefix walk (`segment[0]=="net"` literal, then a module match against
     the link-time-closed module set), which is registry-size-independent.
  2. **Scan share swept over N links per module** (1/2/4/8/16/32) — characterizes the term strip-K
     **narrows**. Today's `by_name` (`child_registry.hpp:52-62`) is a linear scan over *all* links
     plus a second pass asking *every* bus child to `peer_link`; per-module scoping and the
     per-endpoint `resolve_peer` shrink both scan sets. A sweep alone would have measured only the
     shrinking term and missed the growing one.

  **Acceptance rule:** the ADR moves to `accepted` when the baseline shows the N=1 per-hop cost has
  headroom for two additional segment compares. **Merge gate on S2a** (not on acceptance): `allocs=0`
  still holds on the forward hop, and the scoped hop is ≤ the flat hop at equal total link count.
  Two existing signals bound the risk meanwhile: the `allocs=0` forward gate, and the intra-device
  dispatch numbers (`inproc`/`inproc-borrow`/`inproc-path`) which per §3a must stay unchanged — a
  shift there means a local `dst` wrongly entered the forward path.

**Demoted to implementation obligations** (each was unclearable as an acceptance gate, since only
work this ADR blocks could clear it):

- **DESIGNED — bus-peer resolution → S2a.** The grill resolved it structurally: because each module declares its **shape** (`ws-server`/`can` multi-peer vs. `ws-client` point-to-point), the demux reads the module segment and *structurally* knows whether a peer segment follows — no runtime `bus()` probe. A multi-peer module resolves `<peer>` in **its own** peer table (a **per-endpoint** `resolve_peer`, replacing today's global cross-bus `peer_link` scan), which also makes two servers' same-named peers distinct. The adversarial pass had refuted the naive "strip K+1 via the existing global `by_name`" because that scan is single-segment and bus-blind; the fix is the per-endpoint scoping — genuinely new code, not a re-key. **Remaining impl work (not a design unknown):** the scoped `resolve_peer` + a **multi-peer black-hole negative test** — `/net/ws-server/s/alice` reaches ws-`alice`, `/net/ws-server/s/zzz` → clean `PATH_NOT_FOUND`, and `/net/tcp-server/s`'s own `alice` is never reachable through the ws module.
- **Vector byte rewrite is RFC-0014-governed → S7.** Strictly circular as a gate: S7 is four slices downstream of the S2 this ADR blocks. The wire-visible `dst` becomes `/net/<module>/<name>[/<peer>]/residual`; rewrite `fwd-routed-multihop` + `fwd-src-accumulated` under RFC-0014/[#492](https://github.com/avatarsd-llc/libtracer/issues/492)'s "proposed pending" clause-kind authority on the DRAFT spec — **not silently as a #419 tooling change** (`v1.md` §conformance: adding a vector is free, changing existing bytes is a spec change). **Keep the two gaps separate:** mount-vs-bare-name (pure impl + doc + vector conformance, no RFC) and one-level→two-level nesting depth (RFC-0014-governed).
- **Teardown → [#494](https://github.com/avatarsd-llc/libtracer/issues/494), landing *first* and standalone.** `retire()`/[#66](https://github.com/avatarsd-llc/libtracer/issues/66) currently cannot evict the registry entry or tear the socket — add a registry `erase` + wire `retire`/`link_down` → evict. This is a real latent use-after-free **today**, independent of strip-K, so it is fixed against the flat registry on its own schedule rather than riding the re-key; it also keeps it out of S2a's ~250-450 LOC blast radius. RFC-0014's remove-half turns the latent leak live.

**Standing consequences:**

- Refines ADR-0038 §3b: keeps §3b.1's L5-registry layering realization; corrects only the **key** (bare `<name>` → `<module>/<name>`, plus per-endpoint peer scoping) and adds teardown. §3's "FWD demux lock-free" holds (still no `graph.find` on the forward path); invariants #1 (never full-decode), #2 (zero-heap forward), and §3a (intra-device pays nothing) stand. The registry now mutates at runtime (create/remove), so its "immutable after setup" premise erodes on the *write* side — the concurrency of runtime mutation vs. lock-free forward reads is TSan-gated (the plan's S5).
- Closes #419 at the impl level (bare-name → mount form is the already-normative intent); the wire-visible closure lands with the #492 vector rewrite.
- Retires the #373 `has_first_level_child` shadow-guard (`transport_vertex.cpp:145-147`) once connection names are no longer top-level — **only after verifying no other first-level shadowing** depends on it.
- **Blast radius (~250-450 LOC core + docs + vectors):** `fwd_frame_view.hpp` (peek → structural-prefix descent; strip-1 → strip-K), `fwd_router.cpp` (the three demux sites in lockstep), `child_registry.hpp` (re-key + `erase`), `transport_vertex.cpp` (register at `/net/<module>/<name>`, per-endpoint `resolve_peer`, the `/net/<module>/conn` endpoint, #373-guard fate), `docs/reference/02-graph-model.md` + `03` + `07` (one-level → two-level erratum, folding into the #86 sweep), the two `fwd/*` conformance vectors, and the `fwd_*`/`route_handle`/`transport_vertex` host tests. **Riskiest:** the bus-peer/L5-layering pair above.
