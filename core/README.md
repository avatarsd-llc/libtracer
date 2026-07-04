# core — Reference C++ implementation

The reference implementation of the libtracer protocol. Targets ESP32, STM32, and bare-metal alongside hosted Linux/macOS.

## Status: protocol-v1 rebuild in progress — M1 (codec) + M2 (substrate) + M3 (L4 graph) + M4 (loopback) + M5 (UDP transport) + M6 (TCP stream transport) landed

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
- **M4 — first transport (landed).** The `Transport` seam + an in-process loopback transport (dev/test). The net plane is explicit-source-routed `FWD` ([ADR-0040](../docs/adr/0040-net-plane-is-explicit-source-routed-only.md), RFC-0004 — a forwarding node strips its own path segment and passes the rest; loop-free by construction, no dedup, no `hop_count`); `0x0D ROUTER` stays a reserved-but-unimplemented wire code.
- **RFC-0004 remote-operation plane (landed).** `fwd_router_t` (+ `child_registry_t`, `op_resolver_t`, `route_handle_t`): path-addressed `read`/`write`/`await`/`subscribe` over `FWD`, with connections exposed as `/net/<conn>` vertices (`transport_vertex_t`, ADR-0027). The forward hop is heap-free (offset-dispatch + stack header buffers; a CI-gated `bench_forward_heap` proves 0 allocations, ADR-0038/0039). The terminus reads the ADR-0041 arena: `wire::decode_into`/`tlv_arena_t` (flat pre-order span-nodes over the inbound frame), a span-aliased `path_key` (a canonical PATH body *is* the vertex-map key — zero materialization), trailer-sliced stores (stored WRITE values are header+body only), and a direct-emitted `FWD{REPLY}` head in one exactly-sized segment. A remote subscriber's `return_route` is copied once at subscribe into a refcounted segment and refcount-cloned per delivery. The arena draws directly from the `fwd_router_t` constructor's `std::pmr::memory_resource*` (defaulted to the standard heap) — the library holds no internal buffer; a host injects a pool resource over its own slab for a zero-global-heap terminus.
- **M5 — UDP socket transport (landed).** `udp_transport_t` — a real POSIX UDP socket behind the same `Transport` seam, so two nodes talk over the kernel network stack. One datagram = one frame (no stream reassembly), pairing with the flat decoder. Validated raw and end-to-end through the FWD plane over localhost UDP (`tests/udp_test.cpp`). TSan/ASan/UBSan clean. The two-process **network benchmark** vs Zenoh-over-UDP lives in `bench/`.
- **M6 — TCP stream transport (landed).** `tcp_transport_t` — a reliable byte stream behind the same seam. Each frame rides behind a 4-byte u32-LE **length prefix** (transport framing, not part of the TLV); the receive thread reassembles partial reads and honors record boundaries on coalesced writes, reading each frame straight into ONE refcounted segment from the injected `mem_backend_t` (ADR-0042 owning delivery; backend exhaustion drains the frame off the stream — framing sync survives). Prefixes above 16 MiB are malformed: counted, connection torn down. Dial (synchronous connect) and listen (one inbound peer) modes; `tcp` is a transport-factory builtin beside `udp`/`ws`. Validated raw and end-to-end through the FWD plane over localhost TCP (`tests/tcp_test.cpp`); TSan/ASan/UBSan clean. The rope-aware (link-walking) zero-copy decode — the stream consumer that would let a frame span segments (ADR-0042 §4) — is not implemented.
- **ADR-0043 Phase A — QUIC transport, a separate module (landed).** `quic_transport_t` — msquic behind the same seam, in its own library target **`libtracer_quic`**: a host that talks QUIC links the module and registers `quic_transport_factory()` via the `register_transport_type` extension seam; a host that doesn't never compiles these sources — the core has no msquic reference, no feature macro, no `quic` builtin (open/closed), and the 16 KB MCU story is untouched. One connection, ONE bidirectional stream carrying the SAME length-prefix framing as `tcp_transport_t` — plus what QUIC adds: TLS 1.3 (the first real link-security story), connection migration, no TCP head-of-line blocking. Dial (synchronous handshake; CA bundle or a DEV-ONLY no-verify flag for self-signed certs) and listen (PEM `cert`/`key` paths, one inbound peer; `tools/gen-dev-cert.sh` emits a dev pair); the factory consumes the new `cert`/`key` config keys, parsing them MODULE-SIDE from the raw config TLV — the shared `conn_settings_t` stays lean with only the universal keys (the ADR-0043 §5 leanness ruling). ADR-0042 owning delivery + backpressure drain, as tcp. Validated raw and end-to-end over localhost QUIC (`tests/quic_test.cpp`, the dedicated `quic` CI workflow); TSan/ASan/UBSan clean. Per-flow streams, RFC 9221 datagram mode, and WebTransport (Phase B, #92) are staged follow-ons.

## Layout

```text
core/
├── include/libtracer/    Public headers
│   ├── tlv.hpp crc.hpp frame.hpp tlv_arena.hpp   M1 — wire codec (L2/L3) + terminus arena decoder
│   ├── backend.hpp segment.hpp       M2 — L0 seam + refcounted segment
│   ├── mem_heap.hpp mem_borrowed.hpp mem_pool.hpp   M2 — L0 backends
│   ├── view.hpp rope.hpp             M2 — L1 zero-copy view/rope + cast
│   ├── status.hpp path.hpp vertex.hpp graph.hpp     M3 — L4 graph runtime
│   ├── transport.hpp loopback.hpp    M4 — transport seam + loopback
│   ├── fwd_router.hpp child_registry.hpp op_resolve.hpp route_handle.hpp   FWD source-routing (RFC-0004)
│   ├── transport_vertex.hpp          connection as a /net/<conn> vertex (ADR-0027)
│   ├── transport_tcp.hpp transport_udp.hpp transport_ws.hpp transport_can.hpp   socket / bus transports
│   ├── transport_quic.hpp            msquic QUIC transport — the separate libtracer_quic module (ADR-0043)
│   └── tracer.hpp                    umbrella include
├── src/                  codec, substrate, graph, FWD router, transports
├── tests/                conformance_runner + graph_test + fwd_* + transport_vertex_test + tcp/udp/ws/can + CMake
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

Separate modules (a host links a module only if it uses it; the default build has zero external dependencies and never compiles module sources):

- **`libtracer_quic`** — the msquic QUIC transport (`-DLIBTRACER_WITH_QUIC=ON` configures the target; `quic_test` joins the suite). Needs [msquic](https://github.com/microsoft/msquic); point `CMAKE_PREFIX_PATH` at its install prefix if it isn't system-installed. Wire it in with `net.register_transport_type("quic", tr::net::quic_transport_factory())`.
- **`mem_cuda`** — the GPU backend (`-DLIBTRACER_WITH_CUDA=ON`). Needs the CUDA toolkit; tested locally via `tools/test-cuda.sh` (no GPU in CI).
