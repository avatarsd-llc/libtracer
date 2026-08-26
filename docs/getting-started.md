# Getting started (C++ reference implementation)

> Build the reference node, register a vertex, wire up pub/sub, then send a value
> between two nodes over a wire — about ten minutes. Every snippet below is the real
> API, and each one is lifted from a program that compiles and self-checks under
> `ctest`: `core/examples/` holds **seven** such programs, catalogued in
> [Examples](examples/index.md).

A libtracer node is a **graph of addressable vertices**. A vertex is addressed by a
**path** (`/sensor/temp`), resolved **once** to a handle, then read/written/awaited on
that handle. A stored value is opaque bytes behind a zero-copy **`view_t`**; the same
bytes are what travel on the wire when two nodes are connected.

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
ctest --test-dir core/build
```

C++23 (GCC 13+ / Clang 16+). The example programs drop out of the same build:

```sh
./core/build/examples/in_process_pubsub      # §3–§4 below
./core/build/examples/two_node_fwd           # §5 below
```

### Build options

The default build is the full node — modularity is opt-out. Four options matter on a
first build; the rest of the module set is enumerated in
[Reference 10 — module catalog](reference/10-module-catalog.md).

| option | default | what it selects |
| --- | --- | --- |
| `BUILD_TESTING` | unset (off) | compiles `core/tests` and registers the example programs as `example_*` smoke tests (`core/CMakeLists.txt:386`) |
| `LIBTRACER_BUILD_EXAMPLES` | on when libtracer is the top-level project | compiles the seven programs under `core/examples/` (`core/CMakeLists.txt:393`) |
| `LIBTRACER_NET_PLANE` | `ON` | the FWD routing plane — `op_resolve`, `route_handle`, `fwd_router_t`, `transport_vertex` (`core/CMakeLists.txt:63`) |
| `LIBTRACER_WITH_QUIC` | `OFF` | configures the separate `libtracer_quic` target (QUIC and WebTransport); needs msquic installed (`core/CMakeLists.txt:290`) |

Two of the seven examples — `two_node_fwd` and `tree_of_ropes` — are built and
registered only under `LIBTRACER_NET_PLANE`, so `ctest -R example_` runs five of seven
when the net plane is off (`core/examples/CMakeLists.txt:87-96`).

Sizes and policy types — the axes that decide a node's static RAM — are a separate
kind of knob from the module set, and they are not visible from the integrator's CMake
line. [The configuration space](design/config/00-configuration-space.md) enumerates all
three axis kinds and what each costs on a given target.

## 2. Use it as a dependency

`core/` installs as a CMake package, so a downstream project links libtracer without
vendoring and gets one namespaced target, `libtracer::libtracer`, however it was pulled
in.

**Installed, via `find_package`:**

```sh
cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release
cmake --build core/build -j
cmake --install core/build --prefix /usr/local   # any prefix on CMAKE_PREFIX_PATH
```

```cmake
find_package(libtracer 0.6 REQUIRED)              # SameMinorVersion: 0.6 ≠ 0.7 (pre-1.0)
target_link_libraries(app PRIVATE libtracer::libtracer)
```

The package version file is written with `COMPATIBILITY SameMinorVersion`
(`core/CMakeLists.txt:375-356`): pre-1.0, a minor bump may break the C++ API, so a
request for one minor never silently accepts another. The repository version is
`0.6.0`; asking for `0.3` against it fails to configure.

The installed static archive is `libtracer.a`, so a non-CMake build links it with the
conventional `-ltracer`.

**In-tree, via `FetchContent` (no install step):**

```cmake
include(FetchContent)
FetchContent_Declare(libtracer
    GIT_REPOSITORY https://github.com/avatarsd-llc/libtracer
    GIT_TAG        main            # pin a commit — protocol v1 is DRAFT
    SOURCE_SUBDIR  core)
FetchContent_MakeAvailable(libtracer)
target_link_libraries(app PRIVATE libtracer::libtracer)   # same target either way
```

**Embedded / platform:** consume a prebuilt integration rather than building the core
directly — the [ESP-IDF managed component](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/esp-idf)
(`REQUIRES libtracer`),
[PlatformIO](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/platformio),
or [Arduino](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/arduino).

## 3. A first node — register, write, read

```cpp
#include <array>
#include <cstddef>
#include <optional>
#include <span>

#include "libtracer/tracer.hpp"
using tr::graph::graph_t, tr::graph::path_t, tr::graph::role_t;

graph_t g;
// Resolve the path ONCE to a vertex_handle_t — the hot-path token, no strings after.
// register_vertex is infallible on a literal: it returns the handle, with no deref.
const tr::graph::vertex_handle_t temp =
    g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

// A value is opaque bytes owned by a refcounted segment. over_bytes copies the input
// once and reports allocation failure as an empty optional — the caller's BACKPRESSURE.
const std::array<std::byte, 4> le23{std::byte{23}, std::byte{0}, std::byte{0}, std::byte{0}};
std::optional<tr::view::view_t> value = tr::view::over_bytes(le23);
if (!value) return;                    // allocation failure

(void)g.write(temp, *value);           // view_t converts implicitly to the stored rope_t

auto got = g.read(temp);               // result_t<value_ref_t>
if (got) {
    std::span<const std::byte> bytes = (*got)->only().bytes();
    // … 23, little-endian, the wire order …
}
```

**The one idea that matters:** `register_vertex` returns a **`vertex_handle_t`**, and
the hot path — `write(v, …)` / `read(v)` — takes that handle. No string formatting, no
parse, no map lookup per call ([Reference 03 — addressing](reference/03-addressing.md)
§static path handles). The `path_t("…")` constructor parses the literal once
([ADR-0054](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0054-path-t-parse-once-constructor.md));
a runtime string uses the fallible `path_t::parse`. The infallible-register rule is
[ADR-0056](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0056-vertex-handle-infallible-register.md);
a path whose collision is a genuine runtime outcome uses `try_register_vertex` instead.

`tr::view::over_bytes` (`core/include/libtracer/mem_heap.hpp:340`) is the one audited
place that turns a byte span into an owned `view_t`. A hand-rolled
`heap_alloc` + `memcpy` + `view_t::over` triplet is the pattern it replaces, and it
loses the allocation-failure signal that `std::optional` carries.

**`read` returns a reference, not a copy.** `graph_t::read` and `graph_t::await` return
`result_t<value_ref_t>` (`core/include/libtracer/graph.hpp:1384,1590`), so `(*got)` is a
`value_ref_t` and `(*got)->…` reaches the referenced `rope_t`. The rule: *a read of a
published value returns a reference to it; a read that composes a new value returns the
value* — which is why `read_children_folded` and its siblings still return a `rope_t`.
Under an injected `std::pmr::memory_resource` an outstanding `value_ref_t` **pins** the
value it names (`core/include/libtracer/vertex.hpp:237-240`), so a long-lived reference
holds the graph's memory; take the bytes and drop it.

```{note}
`rope_t::only()` has a precondition — `link_count() == 1`, debug-asserted
(`core/include/libtracer/rope.hpp:195-204`). It is the consumer's explicit "this value
is one segment", correct for a scalar written as above. A consumer that cannot promise
contiguity calls `materialize()` instead, which returns the single link when there is
one and pays a single flatten copy otherwise.
```

## 4. Pub/sub — subscribe and fan out

A write fans out to every subscriber. Three delivery styles, all on the same vertex:

```cpp
// (1) an in-process callback, fired inline on each write with the delivered rope_t.
//     The callback is bound BY ADDRESS: it must be a named lvalue, and it and any
//     state it captures must outlive every delivery.
auto on_temp = [](const tr::view::rope_t& v) { /* … use v.only().bytes() … */ };
(void)g.subscribe(path_t("/sensor/temp"), on_temp);

// (2) a spec-faithful target vertex (a write re-dispatches the value to /log/temp)
(void)g.subscribe(path_t("/sensor/temp"), path_t("/log/temp"));

// (3) a thread blocking on the next write (the single-shot primitive)
auto r = g.await(temp, std::chrono::seconds{2});

(void)g.write(temp, *value);           // → (1) and (2) fire; (3) wakes
```

The callback form is sugar over the primitive
`subscribe(const path_t&, subscriber_fn_t fn, void* ctx)` with
`subscriber_fn_t = void (*)(void*, const rope_t&)`
(`core/include/libtracer/subscriber.hpp:157`). The sugar takes the callable as `F&`
(`core/include/libtracer/graph.hpp:1798-1801`), so a temporary lambda written inline at
the call site does not compile — and would dangle if it did. **Lifetime obligation:**
the bound callable is the `ctx`, and `ctx` must outlive every possible delivery;
`unsubscribe` only deactivates the edge slot, and a delivery already in flight
completes against it.

Full program: [`in_process_pubsub.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/in_process_pubsub.cpp)
(walked through in [in-process pub/sub](examples/in-process-pubsub.md)); it also
declares the STREAM ring depth owner-side with `set_history_depth` and reads the vertex
shape back through `:schema`.

## 5. Two nodes over a wire

Two nodes each own a `graph_t`; a **`fwd_router_t`** on each node routes **`FWD`**
frames between its local graph and a set of *named transport links*. A `FWD` frame
carries its own route: `dst` is the explicit source route to the target (one NAME per
hop), and each hop **strips the segment it consumed** and **prepends its name for the
inbound link to `src`** — so when the frame lands, the accumulated `src` is the exact
way back for the reply. No per-request state lives on any hop
([ADR-0040](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0040-net-plane-is-explicit-source-routed-only.md),
[RFC-0004 §D](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md)).

```{mermaid}
flowchart LR
    CA["node A: FWD{WRITE dst=/b/sensor/temp}"] --> RA["router A<br/>peek first dst seg → link b"]
    RA -->|"strip b · grow src"| T(["transport (wire)"])
    T --> RB["router B<br/>dst=/sensor/temp → local terminus"]
    RB --> M(("node B: /sensor/temp")) --> S["B's subscriber"]
```

```cpp
// Declaration order is load-bearing: the channel is declared LAST so it destructs
// FIRST — its receive threads join before the routers they call into are gone.
graph_t node_a, node_b;
tr::net::fwd_router_t router_a(node_a);
tr::net::fwd_router_t router_b(node_b);
tr::net::loopback_channel_t channel;               // an in-process dev "wire"

// B owns the target vertex and a subscriber; A knows its link to B as "b".
(void)node_b.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
router_a.add_child("b", channel.a());   // a dst starting with "b" routes over the wire
router_b.add_child("a", channel.b());   // B's name for the inbound link (the way back)

auto on_temp = [](const tr::view::rope_t& v) { /* … v.only().bytes() … */ };
(void)node_b.subscribe(path_t("/sensor/temp"), on_temp);   // named lvalue, as in §4

// A client hands A's router FWD{ op=WRITE, dst=/b/sensor/temp, payload=VALUE(23) }:
// A strips "b" and forwards /sensor/temp across the wire; B's terminus writes it.
router_a.on_frame("client", fwd_write({"b", "sensor", "temp"}, value_tlv_23));
// node B's subscriber receives 23 — the bytes made a full trip over the wire.
```

```{tip}
Cross-node values travel as a **VALUE TLV** so the peer can decode them structurally;
in-process, any opaque bytes may be stored. `fwd_write` is not library API — it is the
frame the *client* builds, from the `tr::wire::emit_tlv` / `emit_name` primitives in
`tlv_emit.hpp`: one VALUE TLV holding the op byte, a `dst` PATH TLV of NAME segments, an
empty `src` PATH TLV that grows per hop, then the payload, all wrapped in one FWD TLV.
The emitter contract is in [frame-codec](modules/frame-codec.md); a working copy is the
`fwd_write` helper in
[`two_node_fwd.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/two_node_fwd.cpp).
```

**Going to a real socket is a one-line swap:** replace the loopback endpoints with two
**`udp_transport_t`** instances — `tr::net::udp_transport_t(bind_port, "127.0.0.1",
peer_port)` — and the routers and graphs above are unchanged. That exact swap is
[`udp_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/udp_test.cpp)'s
two-node test, and the two-*process* UDP version powers the
[network benchmark](https://github.com/avatarsd-llc/libtracer/tree/main/bench). Measured
hop and end-to-end figures are on the generated
[performance page](performance.md).

Two properties of this net plane are worth knowing from the first frame:

- **A forward hop of a contiguous frame allocates nothing of its own.** The router reads
  three headers by offset, builds the shrunk-`dst` and grown-`src` heads on the stack, and
  scatter-gathers them with untouched views of the inbound frame. `bench_forward_heap`
  CI-gates *that arm* at zero allocations — but the gate covers the hop's own work against a
  **stub link**, not the shipping transport's `iovec` table, which spills to the heap above
  16 spans (`bench_transport_iov` measures the spill at 17 caller spans / ~288 B). And a
  **multi-link rope** frame draws its scatter-gather table from the injected receive source,
  because the sub-span count is the sender's choice and is known only at run time: nothrow,
  not allocation-free. See [04 §multi-hop FWD forwarding](reference/04-communication-flows.md).
- **Routes cannot loop.** `dst` only ever shrinks — by a whole mount run per hop, never by
  nothing — so a route is
  finite and a physical cycle is harmless per-op: there is no revisit check and none is
  needed. No dedup tables, no hop counters — loop-freedom is by construction.

## Where to go next

- **[Examples](examples/index.md)** — the seven compile-tested programs, each with a
  walkthrough.
- **[Module guide](modules/index.md)** — every module, its interface, and how they
  compose (start at the [interface map](modules/interface-map.md)).
- **[Wire walkthrough](modules/wire-format-bits.md)** — the exact bytes, annotated.
- **[Reference](reference/00-overview.md)** — the descriptive six-layer model.
- **[Specification](spec/v1.md)** — the normative v1 wire protocol.
