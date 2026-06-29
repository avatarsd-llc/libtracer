# The `bridge_t` class dissolves into the connection-vertex — it is not kept as a separate user-facing abstraction

[reference/04 §bridge](../reference/04-communication-flows.md) already specifies that *"a bridge is a vertex that ingests TLVs from one transport and republishes them into the local graph under a mount point"* — bridged data is *"indistinguishable from local-source data."* [ADR-0027](0027-transport-and-connections-are-vertices.md) then makes every transport and each connection a first-class `/` vertex, so that a node is **one path tree** — data endpoints, controllers, *and* transports — uniformly. The C++ `bridge_t` (landed in M4) predates both: it is a standalone class holding one `transport_t&` with a single `set_mount` and an `export_vertex` egress hook. As a non-vertex side-channel for exactly the routing the path tree now owns, it is the thing out of step with the spec — not the spec with the code.

## Decision

**Dissolve `bridge_t` into the connection-vertex.** Its two halves relocate *verbatim* (the TSan-clean M4 logic moves, it is not rewritten):

- **Egress** (`export_vertex` → ROUTER-wrap → `transport.send`) becomes the connection-vertex's **forward** behavior: hop-by-hop source-routing per [CONTEXT.md §addressing](../../CONTEXT.md) — the connection-vertex strips its own path segment and forwards the unresolved suffix + payload across the link.
- **Ingress** (transport receiver → unwrap → dedup `(origin, ts)` → `hop_count`/`MAX_HOPS` → `graph.write(mount)`) becomes the connection-vertex's **receive** behavior: the proxy vertex at the per-connection mount point `/net/<conn>/<remote path>` ([CONTEXT.md](../../CONTEXT.md): a transport-vertex mounts the peer's graph under itself; the receive-side prefix equals the send-side suffix).

The user-facing surface becomes ordinary graph writes — subscribe an input endpoint to `/net/<conn>/<source>`; the connection-vertex carries the routing. No `bridge_t`, no `export_vertex`/`set_mount` in the public API.

**Staged** to keep the tested data path live throughout:

1. **Stage 1 — shell over the live path.** Transports/connections appear as `/` vertices (`:settings`, `await` up/down, `:children[]`-created per [ADR-0017](0017-in-band-vertex-creation-controller-orchestration.md)) while `bridge_t` still carries the bytes. Proves the vertex model with **zero regression risk** to the TSan-clean path. (Prerequisite: the `:children[]` SPEC creation sub-issue [#82](https://github.com/avatarsd-llc/libtracer/issues/82).)
2. **Stage 2 — the flip.** Relocate egress into `forward` and ingress into `receive`; retire `bridge_t`'s public surface. Public API change → `core/CHANGELOG.md` note.

## Considered options

- **Keep `bridge_t` as a user-facing class, re-pointing its `transport_t&` at a connection-vertex's transport.** Rejected: it remains a non-vertex side-channel for routing the path tree owns — a standing contradiction of ADR-0027's "one path tree, uniformly" and a seam the orchestrator cannot reach with ordinary writes.
- **Per-subscriber egress proxy vertex** (each remote subscriber becomes its own `/` vertex whose `on_write` wraps+sends). Rejected: [ADR-0021](0021-colon-field-plane-is-the-vertex-ioctl.md) explicitly forbids turning a vertex's `:subscribers` facet into `/` sub-vertices (it dissolves one-identity atomicity), and it would spawn a vertex per remote subscriber. The route is the **path** (source-routed through the connection-vertex), not a proxy vertex or a subscriber-field routing entry.
- **Rewrite the ROUTER/dedup/hop logic fresh inside the connection-vertex.** Rejected: discards the M4 logic already proven under TSan/ASan/UBSan. Relocate it verbatim; optimize later only if a bench demands it.

## Consequences

- The dissolved bridge is **uniform** with the rest of the graph — a remote source is just `/net/<conn>/<path>`, addressed and subscribed to like any local vertex. This is what makes third-party orchestration generic ([ADR-0027](0027-transport-and-connections-are-vertices.md)).
- **No shared global namespace.** Each node has its own root; same-named remote vertices disambiguate by the connection in the path (`/net/connB/sensor/temp` vs `/net/connC/sensor/temp`). Two peers publishing `/sensor/temp` never collide — the key difference from Zenoh's global key-space, and the reason direct 1:1 input→source subscription is unambiguous.
- **Per-connection mount, one pub/sub, not two.** Inbound data lands at the proxy (`/net/<conn>/<path>`) and fan-outs to the input subscribed to it — a single write→fan-out, identical to local publish. No extra broker hop.
- Public API removal (`export_vertex`/`set_mount`); tracked as the Stage-2 flip of [#83](https://github.com/avatarsd-llc/libtracer/issues/83) (sub-issue of [#59](https://github.com/avatarsd-llc/libtracer/issues/59)). The `tr::net` namespace stays; `bridge_t` the class goes.
