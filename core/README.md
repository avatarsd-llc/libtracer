# core — Reference C++ implementation

The reference implementation of the libtracer protocol. Targets ESP32, STM32, and bare-metal alongside hosted Linux/macOS.

## Status: protocol-v1 rebuild in progress — M1 (codec) + M2 (substrate) + M3 (L4 graph) + M4 (loopback) + M5 (UDP transport) landed

> **2026-06-24.** The original headers under `include/libtracer/` were a pre-spec snapshot extracted from strawberry-fw (see [ADR-0001](../docs/adr/0001-extract-reference-implementation-from-strawberry-fw.md)). They did not compile and encoded the retired v0.0 wire model (in-header XOR-16 CRC, no `PL` bit, fixed `uint32` length, `connect`/`disconnect` API, NUL-terminated NAMEs). Per the protocol-v1 consistency consolidation they were **deleted, to be rebuilt fresh against the spec** rather than patched.

The rebuild targets the protocol-v1 wire format and API as fixed in the ADRs:

- 4/6-byte header, `opt` bits `PL/TS/CR/LL/CW/TF`, trailer-positioned CRC-32C ([ADR-0004](../docs/adr/0004-crc-in-optional-trailer.md)), fixed-width length ([ADR-0005](../docs/adr/0005-fixed-width-length-opt-ll.md)).
- No generic `LIST` ([ADR-0003](../docs/adr/0003-retire-list-type-code-0x05.md)); structured = `opt.PL=1` + a purpose type byte.
- `read`/`write`/`await` + field-write control surface; no `connect`/`disconnect`/`subscribe` ([ADR-0006](../docs/adr/0006-read-write-await-api-no-connect.md)).
- The module ABI is implementation-defined ([ADR-0013](../docs/adr/0013-v1-scope-boundaries.md)); this implementation is modern C++23.

### Milestones

- **M1 — wire codec (landed).** The L2/L3 layer: frame header / `opt` / trailer, CRC-32C and CRC-16-CCITT, the borrowed (zero-copy) `Tlv` model, and `decode`/`encode`. Passes all four seed conformance vectors under [../tests/conformance/vectors/v1/](../tests/conformance/vectors/v1/) via `tests/conformance_runner.cpp`.
- **M2 — L0/L1 substrate (landed).** The refcounted `Segment` (intrusive `SegmentPtr`, the canonical intrusive_ptr orderings from [02-graph-model.md](../docs/reference/02-graph-model.md) §required atomic operations; `LIBTRACER_NO_ATOMIC` for single-threaded/Cortex-M0), the user-implementable `MemBackend` seam with three backends (`mem_heap`, `mem_borrowed` live/transparent-router, `mem_pool` bounded fixed-slab), the zero-copy `View`/`Rope`, and the `view_as_tlv` cast that ties a TLV back to a view ([08-views-and-ownership.md](../docs/reference/08-views-and-ownership.md)). The refcount is what makes M1's borrowed `Tlv` safe to hold past its source buffer. Validated by `tests/substrate_test.cpp` (also built `-DLIBTRACER_NO_ATOMIC`), clean under ASan+UBSan.
- **M3a — L4 graph runtime core (landed).** The vertex map keyed on canonical PATH-TLV payload bytes, the three vertex roles (stored-value, bounded-history stream, user `on_read`/`on_write` handler seam), and the `read`/`write`/`await` data API ([ADR-0006](../docs/adr/0006-read-write-await-api-no-connect.md)). The LKV read/write hot path is lock-free (an atomic `shared_ptr` swap); `await` blocks on a per-vertex condvar. Validated race-free under TSan, leak/UB-free under ASan+UBSan (`tests/graph_test.cpp`).
- **M3b — subscriptions + dispatch (landed).** SUBSCRIBER target-path fan-out on `SegmentPtr` clone + a `subscribe(src, callback)` helper, field-write (`:subscribers[]`, `:settings.*`) + unsubscribe, a dispatch-depth cycle cap ([ADR-0015](../docs/adr/0015-graph-runtime-concurrency-and-in-process-cycle-cap.md)), a minimal `:schema` POINT, and the in-process pub/sub example (`examples/in_process_pubsub.cpp`) — the P0 node, end to end. TSan/ASan/UBSan clean.
- **M4 — first transport (landed; the M4 bridge was later retired).** The `Transport` seam + an in-process loopback transport (dev/test). M4 originally shipped a ROUTER-flood `Bridge`, **retired in [ADR-0040](../docs/adr/0040-net-plane-is-explicit-source-routed-only.md)**: the net plane is now explicit-source-routed `FWD` (RFC-0004 — a forwarding node strips its own path segment and passes the rest; loop-free by construction, no dedup, no `hop_count`). The `Transport` seam stays; `0x0D ROUTER` remains a reserved-but-unimplemented wire code.
- **RFC-0004 remote-operation plane (landed).** `fwd_router_t` (+ `child_registry_t`, `op_resolver_t`, `route_handle_t`): path-addressed `read`/`write`/`await`/`subscribe` over `FWD`, with connections exposed as `/net/<conn>` vertices (`transport_vertex_t`, ADR-0027). The forward hop is heap-free (offset-dispatch + stack header buffers; a CI-gated `bench_forward_heap` proves 0 allocations, ADR-0038/0039).
- **M5 — UDP socket transport (landed).** `udp_transport_t` — a real POSIX UDP socket behind the same `Transport` seam, so two nodes talk over the kernel network stack. One datagram = one frame (no stream reassembly), pairing with the flat decoder. Validated raw and end-to-end through the FWD plane over localhost UDP (`tests/udp_test.cpp`). TSan/ASan/UBSan clean. The two-process **network benchmark** vs Zenoh-over-UDP lives in `bench/`.
- **M6 — a reliable stream transport (next).** TCP/QUIC behind the seam: length-prefix framing + partial-read reassembly, which is the consumer that finally builds **rope-aware (link-walking) zero-copy decode**.

## Layout

```text
core/
├── include/libtracer/    Public headers
│   ├── tlv.hpp crc.hpp frame.hpp     M1 — wire codec (L2/L3)
│   ├── backend.hpp segment.hpp       M2 — L0 seam + refcounted segment
│   ├── mem_heap.hpp mem_borrowed.hpp mem_pool.hpp   M2 — L0 backends
│   ├── view.hpp rope.hpp             M2 — L1 zero-copy view/rope + cast
│   ├── status.hpp path.hpp vertex.hpp graph.hpp     M3 — L4 graph runtime
│   ├── transport.hpp loopback.hpp    M4 — transport seam + loopback
│   ├── fwd_router.hpp child_registry.hpp op_resolve.hpp route_handle.hpp   FWD source-routing (RFC-0004)
│   ├── transport_vertex.hpp          connection as a /net/<conn> vertex (ADR-0027)
│   ├── transport_udp.hpp transport_ws.hpp transport_can.hpp   socket / bus transports
│   └── tracer.hpp                    umbrella include
├── src/                  codec, substrate, graph, FWD router, transports
├── tests/                conformance_runner + graph_test + fwd_* + transport_vertex_test + udp/ws/can + CMake
├── examples/             in_process_pubsub.cpp
├── CHANGELOG.md          public-API change log
└── CMakeLists.txt
```

The directory name `include/libtracer/` is part of the public API: include paths in user code (`#include <libtracer/tracer.hpp>`) depend on it. Do not rename.

## Building

Requires a C++23 compiler (GCC 13+ / Clang 16+, for `std::expected`).

```sh
cmake -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

`ctest` runs the conformance suite (`conformance_runner`) against the seed vectors: each is validated by roundtrip (`encode(decode(input.bin)) == input.bin`) plus a programmatic golden encode/decode — no JSON parser, since the bytes are self-describing.
