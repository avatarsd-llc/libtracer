# Getting started (C++ reference implementation)

> Build the reference node, write your first vertex, wire up pub/sub, then send a
> value between two nodes over a wire — in about ten minutes. Every snippet here is
> the real API; the two runnable programs it's drawn from live in
> [`core/examples/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/examples).

A libtracer node is a **graph of addressable vertices**. You address a vertex by a
**path** (`/sensor/temp`), resolve it **once** to a handle, then read/write/await on
that handle. The bytes you store are a zero-copy **`View`**; the same bytes are what
travel on the wire when two nodes are bridged.

```{mermaid}
flowchart LR
    P["publisher<br/>write(v, View)"] --> V(("vertex<br/>/sensor/temp"))
    V --> C["callback subscriber"]
    V --> A["thread in await()"]
    V -. bridged .-> W["ROUTER → transport → peer node"]
```

## 1. Build it

```sh
git clone https://github.com/avatarsd-llc/libtracer
cmake -S core -B core/build -DBUILD_TESTING=ON
cmake --build core/build -j
ctest --test-dir core/build          # all green
```

C++23 (GCC 13+ / Clang 16+). Two runnable examples drop out of the build:

```sh
./core/build/examples/in_process_pubsub      # §2–§3 below
./core/build/examples/two_node_loopback      # §4 below
```

## 2. Your first node — register, write, read

```cpp
#include "libtracer/tracer.hpp"
using tracer::graph::Graph, tracer::graph::Path, tracer::graph::Role;

Graph g;
// Resolve the path ONCE to a Vertex* handle (the hot-path token — no strings after).
tracer::graph::Vertex* temp = *g.register_vertex(*Path::parse("/sensor/temp"),
                                                 Role::StoredValue);

(void)g.write(temp, make_value(23));   // store an opaque value (a View over bytes)
auto got = g.read(temp);               // read the last-known value back (a clone)
```

**The one idea that matters:** `register_vertex` returns a **`Vertex*` handle**, and
the hot path — `write(v, …)` / `read(v)` — takes that handle. No string formatting,
no parse, no map lookup per call (the spec's rule; `reference/10` §path-handle).
`Path::parse` is an init-time step you do once.

A value is just **opaque bytes** wrapped in a `View` (a refcounted, zero-copy handle
over a `Segment`). The simplest constructor:

```cpp
tracer::View make_value(std::uint32_t v) {
    tracer::SegmentPtr seg = tracer::mem::heap_alloc(4);
    for (int i = 0; i < 4; ++i)                        // little-endian, the wire order
        seg->bytes[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    return tracer::View::over(std::move(seg));
}
```

`read` returns a *clone* of the `View` (a refcount bump — no byte copy), keeping the
segment alive for as long as you hold it.

## 3. Pub/sub — subscribe and fan out

A write fans out to every subscriber. Three delivery styles, all on the same vertex:

```cpp
// (1) an in-process callback (fires inline on each write, with a cloned View)
(void)g.subscribe(*Path::parse("/sensor/temp"),
                  [](const tracer::View& v) { /* … use v.bytes() … */ });

// (2) a spec-faithful target vertex (a write re-dispatches the value to /log/temp)
(void)g.subscribe(*Path::parse("/sensor/temp"), *Path::parse("/log/temp"));

// (3) a thread blocking on the next write (the single-shot primitive)
auto r = g.await(temp, std::chrono::seconds{2});

(void)g.write(temp, make_value(23));   // → (1) and (2) fire; (3) wakes
```

Full program: [`in_process_pubsub.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/in_process_pubsub.cpp)
(it also field-writes a QoS setting and reads it back via `:schema`).

## 4. Two nodes over a wire

Two nodes each own a `Graph`; a `Bridge` ROUTER-wraps an exported vertex's value and
sends it across a `Transport`. The receiving bridge sheds the envelope (after dedup +
`hop_count` checks) and writes the bare value into a **mounted** vertex.

```{mermaid}
flowchart LR
    WA["node A: write /sensor/temp"] --> BA["bridge A<br/>ROUTER-wrap"]
    BA --> T(["transport (wire)"])
    T --> BB["bridge B<br/>unwrap · dedup · hop check"]
    BB --> M(("node B: /remote/temp")) --> S["B's subscriber"]
```

```cpp
tracer::LoopbackChannel channel;                       // an in-process dev "wire"
Graph node_a, node_b;
tracer::Bridge bridge_a(node_a, channel.a(), peer_of(0xA1));
tracer::Bridge bridge_b(node_b, channel.b(), peer_of(0xB2));

(void)node_a.register_vertex(*Path::parse("/sensor/temp"), Role::StoredValue);
(void)node_b.register_vertex(*Path::parse("/remote/temp"), Role::StoredValue);
bridge_b.set_mount(*Path::parse("/remote/temp"));      // where inbound values land
(void)node_b.subscribe(*Path::parse("/remote/temp"), /* callback … */);
(void)bridge_a.export_vertex(*Path::parse("/sensor/temp"));

(void)node_a.write(*Path::parse("/sensor/temp"), make_value_tlv(23));
// node B's subscriber receives 23 — the bytes made a full encode → ROUTER → decode trip.
```

```{tip}
Cross-node values are wrapped as a **VALUE TLV** (`tracer::encode`) so the peer can
decode them structurally; in-process you can store any opaque bytes. See
`make_value_tlv` in the example.
```

**Going to a real socket is a one-line swap:** replace `LoopbackChannel` with the
**`UdpTransport`** (M5) — `UdpTransport(bind_port, "127.0.0.1", peer_port)` — and the
bridge/graph above it are unchanged. Full program:
[`two_node_loopback.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/two_node_loopback.cpp);
the two-process UDP version powers the [network benchmark](https://github.com/avatarsd-llc/libtracer/tree/main/bench).

## Where to go next

- **[Module guide](modules/index.md)** — every module, its interface, and how they
  compose (start at the [interface map](modules/interface-map.md)).
- **[Wire walkthrough](modules/wire-format-bits.md)** — the exact bytes, annotated.
- **[Reference](reference/00-overview.md)** — the descriptive six-layer model.
- **[Specification](spec/v1.md)** — the normative v1 wire protocol.
