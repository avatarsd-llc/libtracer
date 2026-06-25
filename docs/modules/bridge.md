# bridge — cross-node forwarding (L4)

```{admonition} In one paragraph
:class: tip
A **`Bridge`** connects a local graph to a transport. On **egress** it subscribes a
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
class Bridge {
    Bridge(graph::Graph&, Transport&, PeerId);
    Result<void> export_vertex(const graph::Path& src);   // egress: subscribe → wrap → send
    void set_mount(const graph::Path&);                   // where ingested data lands
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
    S -- no --> C["copy data → heap View"]
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

See: [transport](transport.md), [router](router.md), [graph](graph.md).
