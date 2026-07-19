# The net plane is explicit-source-routed only: `bridge_t` and the ROUTER-flood mechanism are retired; `0x0D ROUTER` stays a reserved-but-unimplemented wire code

Status: accepted. **Supersedes the `bridge_t`-relocation framing of [ADR-0037](0037-net-side-channels-dissolve-into-vertex-tree-compositor.md)/[ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md)** (which imagined the ROUTER guard being *absorbed* into the connection-vertex). It is not absorbed — it is **retired**. The Brick-3b decision of the #83 Stage-2 flip.

## Context

The C++ core carries two cross-node models that predate and postdate [RFC-0004](../spec/rfcs/0004-remote-operation-addressing.md):

- **`bridge_t`** (M4) — a *flooder with a dedup safety net*. `export_vertex` subscribes a local vertex, wraps every write's **bare data TLV** in a `ROUTER` (`0x0D`) envelope (adding `origin_peer_id`, `origin_timestamp`, `hop_count`), and sends it on one transport. Ingress unwraps → dedups on the `(origin, ts)` recent-set → checks `hop_count`/`MAX_HOPS` → republishes the bare data at a fixed `mount` path. Flood/mirror; the ROUTER envelope + recent-set are the loop-safety net.
- **`fwd_router_t`** (RFC-0004) — a *source-router that cannot loop*. A `FWD` frame carries its own route (`dst` shrinks per hop, `src` grows); a hop strips its segment and forwards the rest. A `dst` revisiting a node is malformed (`ERROR=INVALID_PATH`), so it is **loop-free by construction** — no dedup, no `hop_count`, no state.

ADR-0037/0038 framed Stage-2 as "the ROUTER guard is absorbed per-connection." Grilling the topology showed that framing is wrong, because the **only** case that guard exists for cannot occur under this project's addressing model.

**ROUTER's sole job is `(origin, ts)` dedup for "the same value arrived two ways and I must drop the duplicate."** That requires the two ways to be *auto-selected* — the protocol floods or picks a path, so the receiver cannot tell the arrivals apart and must dedup. libtracer does the opposite: **every remote endpoint is addressed by an explicit source route** ([ADR-0027](0027-transport-and-connections-are-vertices.md) path-as-route). Two links to the same peer are two explicit addresses:

```
/net/ws/<peer1>/ow/<dallas_temp>/temp     ← route #1 (over WebSocket)
/net/can/<peer1>/ow/<dallas_temp>/temp    ← route #2 (over CAN), to the same sensor
```

These are **different addresses ⇒ different subscriptions ⇒ different local vertices**. A consumer that subscribes both *wants both deliveries* — that is redundancy / failover (watch both links; an `await`-timeout detects a dead one). ROUTER dedup on `(origin, ts)` would **silently delete the second arrival — destroying the failover signal the dual link exists to provide.** So the "fold" ROUTER guards against **cannot occur here**: there is no un-asked-for second arrival; every arrival is a route the consumer deliberately named. RFC-0004 §D says it itself — *"strict source-routed deliveries need no `ROUTER`; `ROUTER` earns its keep only where the topology folds."* **No topology in scope folds** (trees with explicit redundant links, never auto-multipath meshes).

There is also a code-vs-spec gap: `bridge_t` wraps a **bare data TLV**, whereas RFC-0004 §D specifies wrapping the **delivery `FWD`**. So `bridge_t` is not even the ROUTER model RFC-0004 keeps — it implements a *superseded, pre-RFC-0004* one. Keeping `bridge_t` is not "keeping the surviving ROUTER capability"; that capability was never built.

## Decision

**The net plane is `FWD` explicit-source-routed only. Retire `bridge_t` and the entire ROUTER-flood *mechanism*; keep `0x0D ROUTER` as a reserved wire codepoint.**

1. **Delete `bridge_t`** (`bridge.hpp`/`bridge.cpp`) and its ROUTER-flood machinery: the `(origin, ts)` recent-set (mutex + set + deque), the per-origin HLC `origin_timestamp` clock, `export_vertex`/`set_mount` egress/ingress, and mount-republish. This removes exactly the mutable, allocating, locked state ADR-0038 wanted gone — it was `bridge_t`'s, not the FWD plane's.
2. **Delete the ROUTER *codec* helpers** (`router.hpp`/`router.cpp`: `router_wrap`/`router_unwrap`/`router_meta_t`). They exist **only** to serve `bridge_t` — nothing else references them.
3. **Keep `0x0D ROUTER` reserved in the wire registry.** Retiring a *wire code* is a spec change with no benefit; reserving an unimplemented codepoint costs nothing and leaves the door open for a future *flooding profile* if a genuinely auto-multipath deployment ever appears. The frame codec still decodes `0x0D` generically (the `router-wrapped` conformance vector stays — it tests the **codec**, not `bridge_t`). We retire the **mechanism**, not the **codepoint**.
4. **No loss of loop safety.** FWD's `dst`-monotonicity + `INVALID_PATH`-on-revisit is the loop bound for the source-routed plane, already in the code; deleting `bridge_t` removes no protection FWD relies on.
5. **No loss of provenance.** RFC-0003's wildcard-provenance need is already served by RFC-0004 §B: when `dst` empties, the accumulated `src` *is* the complete reverse route, so the consumer receives the full source route inherently — no ROUTER PATH-child needed.

This is a fresh-project foundational commitment: **explicit-source-routed-only, no flooding, no auto-multipath.** Every deployment in scope (the originating production firmware — an ESP32-C6 smart-agriculture node: web-UI ↔ robot ↔ CAN, with explicit redundant links) is served by it, and the net plane becomes stateless and dedup-free.

## Considered options

- **Absorb the ROUTER `(origin, ts)`/`hop_count` guard into the connection-vertex** (ADR-0037/0038 as written). Rejected: the guard's only purpose is dedup of auto-arrived duplicates, which explicit addressing makes impossible — and would corrupt failover (delete the redundant-link signal). Absorbing a guard for a case that cannot occur is dead, harmful state.
- **Build the real RFC-0004 ROUTER-wrap** (wrap the delivery `FWD` on cyclic regions; the guard lives per-connection). Rejected *for now*: no topology in scope folds, so this is speculative work for a deployment class the project is deliberately designing out. Deferred — if an auto-multipath mesh ever ships, it wraps the delivery `FWD` per §D with the guard on the connection-vertex, and `0x0D` is reserved and waiting.
- **Keep `bridge_t` "just in case."** Rejected: it is a superseded model (wraps bare data, not FWD), already annotated trajectory-superseded in the reference sweep (#155), and carries a mutex + clock + dedup set forever for a capability nothing uses. The reference-vs-code divergence #86 is *resolved by deletion*, not by keeping two models.
- **Retire the `0x0D` codepoint too.** Rejected: a wire-registry change (RFC territory) for zero benefit; reserving it is free and forward-compatible.

## Consequences

- **`bridge_t`, `router.hpp`/`router.cpp` are deleted;** their tests (`bridge_test`) and the ROUTER-flood example (`two_node_loopback`) go with them. `udp_test` — the real UDP two-node coverage — is **migrated to the FWD plane** (a `FWD{WRITE}` delivery over UDP through `fwd_router_t`), not deleted, so UDP end-to-end stays covered.
- **Public API removal:** `bridge_t`, `export_vertex`, `set_mount`, `set_status_path`, `router_wrap`/`router_unwrap`/`router_meta_t`. `core/CHANGELOG.md` note. `peer_id_t` (the ROUTER origin identity) stays — it is the node identity, still meaningful.
- **The net plane is now uniformly stateless and lock-free-eligible** — there is no ROUTER recent-set left to make lock-free (ADR-0038 §3's "no dedup state on the connection" is satisfied *by construction*, because there is no dedup state anywhere).
- **`0x0D ROUTER` remains a reserved, decodable, unimplemented wire code.** reference/05 §0x0D and reference/04 §Bridge-republish get a final "retired mechanism; reserved codepoint" annotation (folding into the #86/#155 sweep).
- **ADR-0037/0038's "the two side-channels dissolve" resolves cleanly:** `fwd_router_t::children_` dissolved into `child_registry_t` (Brick 3a, #163); `bridge_t` is *retired*, not relocated (this ADR). Both side-channels are gone.
- **The `#77` bridge hop-limit status** (`STATUS=ERROR(NESTING_TOO_DEEP)` on `hop_count` cap) is retired with `bridge_t` — it guarded the ROUTER flood, which no longer exists. If a future flooding profile returns, it returns with its cap and its status.
