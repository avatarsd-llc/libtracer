# Getting started (C++ reference implementation)

> Build the reference node, write your first vertex, wire up pub/sub, then send a
> value between two nodes over a wire — in about ten minutes. Every snippet here is
> the real API; the runnable in-process program lives in
> [`core/examples/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/examples),
> and the two-node flow is the same one exercised end-to-end by
> [`core/tests/udp_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/udp_test.cpp).

A libtracer node is a **graph of addressable vertices**. You address a vertex by a
**path** (`/sensor/temp`), resolve it **once** to a handle, then read/write/await on
that handle. The bytes you store are a zero-copy **`view_t`**; the same bytes are what
travel on the wire when two nodes are connected.

```{mermaid}
flowchart LR
    P["publisher<br/>write(v, view_t)"] --> V(("vertex<br/>/sensor/temp"))
    V --> C["callback subscriber"]
    V --> A["thread in await()"]
    V -. remote subscriber .-> W["FWD → transport → peer node"]
```

## 1. Build it

```sh
git clone https://github.com/avatarsd-llc/libtracer
cmake -S core -B core/build -DBUILD_TESTING=ON
cmake --build core/build -j
ctest --test-dir core/build          # all green
```

C++23 (GCC 13+ / Clang 16+). A runnable example drops out of the build:

```sh
./core/build/examples/in_process_pubsub      # §2–§3 below
```

## 1b. Use it as a dependency

`core/` installs as a CMake package, so a downstream project links libtracer the
modern way — no vendoring — and gets one namespaced target, `libtracer::libtracer`,
regardless of how it was pulled in.

**Installed, via `find_package`:**

```sh
cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release
cmake --build core/build -j
cmake --install core/build --prefix /usr/local   # any prefix on CMAKE_PREFIX_PATH
```

```cmake
find_package(libtracer 0.3 REQUIRED)              # SameMinorVersion: 0.3 ≠ 0.4 (pre-1.0)
target_link_libraries(app PRIVATE libtracer::libtracer)
```

The installed static archive is `libtracer.a`, so a non-CMake build links it with
the conventional `-ltracer`.

**In-tree, via `FetchContent` (no install step):**

```cmake
include(FetchContent)
FetchContent_Declare(libtracer
    GIT_REPOSITORY https://github.com/avatarsd-llc/libtracer
    GIT_TAG        main            # pin a commit — the wire format is not yet frozen
    SOURCE_SUBDIR  core)
FetchContent_MakeAvailable(libtracer)
target_link_libraries(app PRIVATE libtracer::libtracer)   # same target either way
```

**Embedded / platform:** don't build the core yourself — consume a prebuilt
integration: the [ESP-IDF managed component](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/esp-idf)
(`REQUIRES libtracer`),
[PlatformIO](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/platformio),
or [Arduino](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/arduino).

## 2. Your first node — register, write, read

```cpp
#include "libtracer/tracer.hpp"
using tr::graph::graph_t, tr::graph::path_t, tr::graph::role_t;

graph_t g;
// Resolve the path ONCE to a vertex_handle_t (the hot-path token — no strings after).
// register_vertex is infallible on a literal (ADR-0056): it returns the handle, no *-deref.
const tr::graph::vertex_handle_t temp =
    g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

(void)g.write(temp, make_value(23));   // store an opaque value (a view_t over bytes)
auto got = g.read(temp);               // read the last-known value back (a rope clone)
```

**The one idea that matters:** `register_vertex` returns a **`vertex_handle_t`**, and
the hot path — `write(v, …)` / `read(v)` — takes that handle. No string formatting,
no parse, no map lookup per call (the spec's rule; `reference/10` §path-handle).
The `path_t("…")` constructor parses the literal once (ADR-0054); a runtime string
uses the fallible `path_t::parse`.

A value is just **opaque bytes** wrapped in a `view_t` (a refcounted, zero-copy handle
over a `segment_t`) — which the graph stores as a single-link `rope_t`. The simplest
constructor:

```cpp
tr::view::view_t make_value(std::uint32_t v) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(4);
    for (int i = 0; i < 4; ++i)                        // little-endian, the wire order
        seg->bytes[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    return tr::view::view_t::over(std::move(seg));
}
```

`read` returns the value as a *`rope_t`* (a refcount-clone — no byte copy; a scalar is a
single-link rope, so `got->only().bytes()` gives you the contiguous bytes), keeping the
segment alive for as long as you hold it.

## 3. Pub/sub — subscribe and fan out

A write fans out to every subscriber. Three delivery styles, all on the same vertex:

```cpp
// (1) an in-process callback (fires inline on each write, with a cloned rope_t)
(void)g.subscribe(path_t("/sensor/temp"),
                  [](const tr::view::rope_t& v) { /* … use v.only().bytes() … */ });

// (2) a spec-faithful target vertex (a write re-dispatches the value to /log/temp)
(void)g.subscribe(path_t("/sensor/temp"), path_t("/log/temp"));

// (3) a thread blocking on the next write (the single-shot primitive)
auto r = g.await(temp, std::chrono::seconds{2});

(void)g.write(temp, make_value(23));   // → (1) and (2) fire; (3) wakes
```

Full program: [`in_process_pubsub.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/in_process_pubsub.cpp)
(it also field-writes a QoS setting and reads it back via `:schema`).

## 4. Two nodes over a wire

Two nodes each own a `graph_t`; a **`fwd_router_t`** on each node routes **`FWD`**
frames between its local graph and a set of *named transport links*. A `FWD` frame
carries its own route: `dst` is the explicit source route to the target (one NAME per
hop), and each hop **strips the segment it consumed** and **prepends its name for the
inbound link to `src`** — so when the frame lands, the accumulated `src` is the exact
way back for the reply. No per-request state lives on any hop.

```{mermaid}
flowchart LR
    CA["node A: FWD{WRITE dst=/b/sensor/temp}"] --> RA["router A<br/>peek first dst seg → link b"]
    RA -->|"strip b · grow src"| T(["transport (wire)"])
    T --> RB["router B<br/>dst=/sensor/temp → local terminus"]
    RB --> M(("node B: /sensor/temp")) --> S["B's subscriber"]
```

```cpp
tr::net::loopback_channel_t channel;               // an in-process dev "wire"
graph_t node_a, node_b;
tr::net::fwd_router_t router_a(node_a);
tr::net::fwd_router_t router_b(node_b);

// B owns the target vertex and a subscriber; A knows its link to B as "b".
(void)node_b.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
router_a.add_child("b", channel.a());   // A routes a dst starting with "b" over the wire
router_b.add_child("a", channel.b());   // B's name for the inbound link (the way back)
(void)node_b.subscribe(path_t("/sensor/temp"), /* callback … */);

// A client hands A's router FWD{ op=WRITE, dst=/b/sensor/temp, payload=VALUE(23) }:
// A strips "b" and forwards /sensor/temp across the wire; B's terminus writes it.
router_a.on_frame("client", fwd_write({"b", "sensor", "temp"}, value_tlv_23));
// node B's subscriber receives 23 — the bytes made a full trip over the wire.
```

```{tip}
Cross-node values travel as a **VALUE TLV** so the peer can decode them structurally;
in-process you can store any opaque bytes. `fwd_write` builds the FWD frame with the
`tr::wire::emit_tlv`/`emit_name` helpers from `tlv_emit.hpp` — see the
`fwd_write` helper in
[`udp_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/udp_test.cpp)
for the exact fifteen lines.
```

**Going to a real socket is a one-line swap:** replace the loopback endpoints with two
**`udp_transport_t`** instances — `tr::net::udp_transport_t(bind_port, "127.0.0.1",
peer_port)` — and the routers/graphs above are unchanged. That exact swap is
[`udp_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/udp_test.cpp)'s
two-node test, and the two-*process* UDP version powers the
[network benchmark](https://github.com/avatarsd-llc/libtracer/tree/main/bench).

Two properties of this net plane worth knowing from day one:

- **A forward hop never touches the heap.** The router reads three headers by offset,
  builds the shrunk-`dst`/grown-`src` heads on the stack, and scatter-gathers them with
  untouched views of the inbound frame — zero allocations, CI-gated.
- **Routes cannot loop.** `dst` only ever shrinks, one segment per hop, so a route is
  finite and a physical cycle is harmless per-op (not rejected — there is no revisit
  check). No dedup tables, no hop counters — loop-freedom is by construction.

## Where to go next

- **[Module guide](modules/index.md)** — every module, its interface, and how they
  compose (start at the [interface map](modules/interface-map.md)).
- **[Wire walkthrough](modules/wire-format-bits.md)** — the exact bytes, annotated.
- **[Reference](reference/00-overview.md)** — the descriptive six-layer model.
- **[Specification](spec/v1.md)** — the normative v1 wire protocol.
