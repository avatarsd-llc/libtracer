# bridge — cross-node forwarding (L4)

```{admonition} Trajectory — this class dissolves (ADR-0037)
:class: warning
`bridge_t` is a **pre-[ADR-0027](../adr/0027-transport-and-connections-are-vertices.md)
side-channel** for exactly the routing the vertex tree now owns, so it is **slated for
Stage-2 dissolution** per
[ADR-0037](../adr/0037-net-side-channels-dissolve-into-vertex-tree-compositor.md):
its **egress** (`export_vertex` + ROUTER-wrap) is **deleted** — source-routing has no
"subscribe-and-mirror-to-one-transport"; its **ingress** guard (dedup `(origin, ts)` +
`hop_count`/`MAX_HOPS`) is **absorbed** as a per-connection defense-in-depth layer. The
routing model that survives is `fwd_router_t`'s FWD source-routing
([ADR-0035](../adr/0035-implementing-rfc-0004-remote-operation-addressing.md)), carried
by the connection-vertex. The body below describes the **current** class — still the
way two nodes bridge today — until Stage-2 lands (gated on the 16KB-RAM zero-heap
bench). Read ADR-0037 for the operative target.
```

```{admonition} In one paragraph
:class: tip
A **`bridge_t`** connects a local graph to a transport. On **egress** it subscribes a
local vertex and forwards each write ROUTER-wrapped. On **ingress** it unwraps,
**dedups** (recent-set on `(origin, ts)`), enforces **`hop_count`** termination,
then writes the bare TLV to a *mount* vertex — where local subscribers receive it
as if it were local. This is the piece that makes two nodes talk.
```

## What it does

`export_vertex(src)` reuses [graph](graph.md) `subscribe` so every write to `src`
fires a callback that wraps the value (origin = this peer, `ts = now`, `hop = 0`)
and `transport.send`s it. The ingress receiver runs the safety pipeline:

1. `router_unwrap` the frame ([router](router.md));
2. if `hop_count ≥ MAX_HOPS` (32) → **drop** (the [ADR-0014] termination guarantee);
3. if `(origin, ts)` seen in the bounded recent-set → **drop** (dedup);
4. else materialize the data TLV into a fresh heap segment and `graph.write` it to
   the mount vertex — one copy at the bridge boundary, then zero-copy locally.

Knobs: `set_mount`, `set_recent_set_capacity(0)` to disable dedup (proving
`hop_count` alone terminates), `set_reforward` to build a cycle for tests.

## Interface

```cpp
class bridge_t {
    bridge_t(graph::graph_t&, transport_t&, peer_id_t);
    result_t<void> export_vertex(const graph::path_t& src);   // egress: subscribe → wrap → send
    void set_mount(const graph::path_t&);                 // where ingested data lands
    void set_recent_set_capacity(std::size_t);            // 0 = dedup off
    void set_reforward(bool);
    std::uint64_t delivered() const, deduped() const, hop_dropped() const;   // counters
};
```

## Ingress pipeline

```{mermaid}
flowchart LR
    F["frame in"] --> U["router_unwrap"]
    U --> H{"hop ≥ 32?"}
    H -- yes --> D1["drop (termination)"]:::x
    H -- no --> S{"(origin,ts) seen?"}
    S -- yes --> D2["drop (dedup)"]:::x
    S -- no --> C["copy data → heap view_t"]
    C --> W["graph.write(mount)"] --> SUB["local subscribers (zero-copy)"]
    classDef x fill:#fee2e2,stroke:#991b1b;
```

## Benefits

- **Local/remote symmetry** — after the bridge strips ROUTER, a remote value is
  just a local `write`; subscribers don't know or care it came over a wire.
- **Loops can't hang** — `hop_count` guarantees termination independent of dedup
  memory, so an embedded bridge can size its recent-set to zero.
- **One copy per hop, not per fan-out** — materialize once at ingress; every local
  subscriber after that is zero-copy.

See: [transport](transport.md), [router](router.md), [graph](graph.md),
[ADR-0037](../adr/0037-net-side-channels-dissolve-into-vertex-tree-compositor.md)
(the dissolution trajectory).
