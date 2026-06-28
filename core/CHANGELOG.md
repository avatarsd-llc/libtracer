# Changelog — core (libtracer reference implementation)

All notable changes to the **public API** of the `core/` reference implementation
(the headers under `include/libtracer/`) are recorded here, per
[CONTRIBUTING](../CONTRIBUTING.md) / [CLAUDE.md](../CLAUDE.md). This tracks the
*implementation's* C++ API — which is implementation-defined per
[ADR-0013](../docs/adr/0013-v1-scope-boundaries.md) and versioned independently of
the immutable **protocol-v1** wire format it implements.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The
reference implementation is pre-1.0; everything currently lives under
`[Unreleased]`.

## [Unreleased]

### Added

- **`tr::graph::op_resolver_t` — local FWD operation resolution + the zero-copy
  `FWD{REPLY}` builder** ([RFC-0004](../docs/spec/rfcs/0004-remote-operation-addressing.md) /
  [ADR-0035](../docs/adr/0035-implementing-rfc-0004-remote-operation-addressing.md),
  slice 2). `op_resolver_t::resolve(const tr::wire::tlv_t& fwd)` resolves a decoded
  `FWD` against a local vertex (the router's PATH-keyed dispatch), applies
  `READ`/`WRITE`/`AWAIT` plus any `FIELD` `:field` selector, and builds the
  `FWD{REPLY}` as a `tr::view::rope_t`: a small fresh head (`op=REPLY`, `dst`=the
  request's `src`, `src`=the responder endpoint, `kind`) **roped onto refcount-clones
  of the vertex's stored payload view(s)** — never flattened into a fresh buffer
  (ADR-0035 zero-copy reply rule). A `:subscribers[]` read ropes the populated slot
  `SUBSCRIBER` views under a fresh `PL=1` wrapper. New supporting public API:
  `tr::graph::fwd_op_t`, `tr::graph::reply_kind_t`, `tr::graph::kDefaultAwaitTimeout`;
  the field-read-by-handle overload `graph_t::read(vertex_t*, const field_path_t&)`
  and `graph_t::read_subscribers(vertex_t*)`; a `wildcard` flag on `field_step_t`
  and a retained `source_view` on `subscriber_t`. Slice 2 is **local-only**: a
  non-local `dst` replies `ERROR(NOT_FOUND)`; a `[*]` (`index_mode=WILDCARD`) level
  on a non-subscriber path replies `ERROR(INVALID_PATH)` (the `fwd-wildcard-reject`
  conformance vector). Transport/multi-hop forwarding and the route-handle are
  slices 3–4.
- **`FWD` (`0x0F`) and `FIELD` (`0x10`) type codes registered in `tr::wire::type_t`**
  ([RFC-0004](../docs/spec/rfcs/0004-remote-operation-addressing.md) /
  [ADR-0035](../docs/adr/0035-implementing-rfc-0004-remote-operation-addressing.md),
  slice 1). The two remote-operation frames are **structured** (`opt.PL=1`) and
  decode/encode through the existing generic structured-TLV codec — no codec
  change. New cross-core conformance vectors under
  `tests/conformance/vectors/v1/{fwd,field}/` pin the canonical bytes (RFC-0004
  §B/§C) and round-trip byte-for-byte across the C++/TS/Rust cores. Op-resolution,
  forwarding, and `:field` selector validation are later slices (codec only here).

### Changed

- **Substrate hardening — `tr::` namespaces, snake_case `_t` naming, strict docs**
  ([ADR-0016](../docs/adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md),
  [core/STYLE.md](STYLE.md)). A **breaking** rename of the L0/L1 memory substrate;
  the wire format and protocol are unaffected (still **protocol v1**; conformance
  vectors unchanged).
  - **Root namespace `tracer::` → `tr::`.** Sub-namespaces mirror the layer model:
    L0 substrate in **`tr::mem`**, L1 views/ownership in **`tr::view`**.
  - **Types are now snake_case with a `_t` suffix** (std-lib/kernel style):
    `MemBackend`→`tr::mem::mem_backend_t`, `Segment`→`tr::view::segment_t`,
    `SegmentPtr`→`tr::view::segment_ptr_t`, `View`→`tr::view::view_t`,
    `Rope`→`tr::view::rope_t`, `IoDir`→`tr::mem::io_dir_t`, and the concrete
    backends `heap_backend_t` / `pool_t` / `borrowed_backend_t`.
  - **Enums are scoped `SCREAMING_SNAKE`:** `io_dir_t::DEVICE_TO_CPU` /
    `io_dir_t::CPU_TO_DEVICE` (were `IoDir::DeviceToCpu` / `…CpuToDevice`).
  - **Cache hooks renamed** `prepare_for_io`/`finalize_after_io` →
    **`before_io`/`after_io`** (method = timing, `io_dir_t` = direction).
  - **`mem_backend_t::alloc` now takes `alloc_hint_t`** (an opaque, backend-private
    strong typedef; `NONE` default) instead of a raw `std::uint32_t` hint.
  - **Handle-producing helpers moved L0→L1:** `heap_alloc`, `borrow`,
    `borrow_const` are now in **`tr::view`** (they return a `segment_ptr_t`);
    `tr::mem::heap_backend()` (returns `mem_backend_t&`) stays at L0. `segment_t`
    is the one sanctioned L0↔L1 boundary type the backend interface may name.
  - **Doxygen `@brief` discipline, CI-enforced** (`core/Doxyfile`,
    `WARN_AS_ERROR`) and rendered into the Sphinx site as source references via
    Breathe; conventions in `core/STYLE.md`.

- **Phase 2 — snake_case `_t` across the rest of the API (breaking).** The L2/L3
  codec, L4 graph, and transport plane lose their PascalCase, matching the
  substrate convention. Wire format unchanged.
  - **Types:** `Tlv`→`tlv_t`, `Opt`→`opt_t`, `Type`→`type_t`, `Error`→`error_t`,
    `Trailer`/`Crc`/`Timestamp`/`Width`→`*_t` (codec); `Graph`→`graph_t`,
    `Vertex`→`vertex_t`, `Path`→`path_t`, `Status`→`status_t`, `Result`→`result_t`,
    `Settings`→`settings_t`, `Handlers`→`handlers_t`, `Role`→`role_t`,
    `Subscriber`→`subscriber_t`, `FieldPath`→`field_path_t`, `PathKey`→`path_key_t`
    (graph); `Transport`→`transport_t`, `Bridge`→`bridge_t`,
    `RouterMeta`→`router_meta_t`, `PeerId`→`peer_id_t`,
    `UdpTransport`→`udp_transport_t`, `LoopbackChannel`/`LoopbackEndpoint`→`*_t`
    (transport plane).
  - **Enum values are scoped `SCREAMING_SNAKE`:** `type_t::VALUE`/`NAME`/`PATH`/…,
    `status_t::NOT_FOUND`/`INVALID_PATH`/…, `role_t::STORED_VALUE`/`STREAM`/`HANDLER`,
    `error_t::FRAME_INVALID`/`FRAME_TRUNCATED`/…
  - **Layer namespaces.** The codec moves to **`tr::wire`** (L2/L3:
    `tlv_t`/`opt_t`/`type_t`/`error_t`/`decode`/`encode`), the transport plane to
    **`tr::net`** (`transport_t`/`bridge_t`/`router_meta_t`/`udp_transport_t`/…);
    L4 stays `tr::graph`. The full namespace tree now mirrors the six-layer model
    (`tr::mem`→`tr::view`→`tr::wire`→`tr::graph`/`tr::net`).
  - **`view_as_tlv` moved L1→L2.** The TLV-as-cast now lives in `frame.hpp`
    (`tr::wire`), taking a `view::view_t`, so `view.hpp` (L1) no longer depends
    upward on the codec (L2). Two nested CRC enum values also normalized
    (`width_t::CRC32C`/`CRC16_CCITT`).

### Added

- **Differential fuzzer for the RFC 6455 WebSocket frame decoder**
  ([#60](https://github.com/avatarsd-llc/libtracer/issues/60), hardening). The ws
  frame layer (`tr::net::ws::decode_frame`) is network-facing attack surface — it
  parses untrusted bytes (FIN/opcode, the 7/16/64-bit length encodings, the client
  mask, and the overflow-safe 64-bit-over-long path) *before* the TLV layer. A new
  decode harness (`core/tests/ws_fuzz_harness.cpp`, the `ws_fuzz_harness` helper
  binary — like `ws_interop_server`, not an `add_test()`) emits a canonical decode
  result per hex frame; its TS twin
  (`bindings/typescript/packages/transport-ws/fuzz/decode_harness.mjs`) emits the
  byte-identical contract, and `tests/conformance/ws_diff_fuzz.py` feeds thousands
  of seed-derived well-formed + adversarial frames (truncated at every boundary,
  64-bit over-long lengths, missing mask keys, reserved bits, multi-frame buffers)
  to both, asserting the C++ and TS decoders agree and neither crashes. Gated by a
  standalone `ws-diff-fuzz` job in `.github/workflows/ws-interop.yml`. No core API
  change.
- **ESP-IDF managed component — on-silicon build gate** ([#64](https://github.com/avatarsd-llc/libtracer/issues/64)).
  The `integrations/esp-idf/` component now genuinely builds the **P0 in-process
  profile** (L0/L1 substrate, L2/L3 wire codec, L4 graph runtime) as an ESP-IDF
  managed component. A new `inprocess_mirror` example
  (`integrations/esp-idf/examples/inprocess_mirror/`) links the core and exercises
  the in-process mirror surface — `register_vertex` / `write` / `read` / `await`,
  including the `<atomic>` segment-refcount path (`tr::view::segment_ptr_t`). A
  standalone CI workflow (`.github/workflows/esp-idf.yml`) builds it in the
  `espressif/idf:release-v5.3` image for **esp32c6** (required) and **esp32c3** on
  single-core FreeRTOS. The component's `REQUIRES pthread` was corrected to
  `PRIV_REQUIRES pthread` (pthread is a private link dependency of libstdc++
  threading, not a public-header dependency). No core API change.
  - **Host (`linux`) target support** ([#64](https://github.com/avatarsd-llc/libtracer/issues/64)
    follow-up). The component manifest (`idf_component.yml`) now lists the ESP-IDF
    `linux` (POSIX host) target alongside the esp32 family, so a downstream
    **host_test** suite can depend on the real `libtracer` component instead of a
    local wrapper. A new `host_smoke` example
    (`integrations/esp-idf/examples/host_smoke/`) drives the same in-process
    surface (`register_vertex` / `write` / `read`) with **no FreeRTOS and no
    `esp_log`** and is built + run for the `linux` target in CI. The host build
    needs a C++23 `<expected>` compiler: the `espressif/idf:release-v5.3` image's
    default g++-11 lacks it, so the example documents / CI selects **g++-12**. The
    stale `override_path`/`main/idf_component.yml` comment in the component
    `CMakeLists.txt` was corrected to describe the actual `EXTRA_COMPONENT_DIRS`
    wiring. No core API change.

- **CAN transport — SocketCAN binding (increment 2 of [#55](https://github.com/avatarsd-llc/libtracer/issues/55); [ADR-0030](../docs/adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md)).**
  `tr::net::transport_can` (`transport_can.hpp`): a `transport_t` over Linux
  **SocketCAN** that wires the increment-1 framing to a live bus.
  - **Egress** fragments the frame via `view_can_frames_t`, emits an in-band
    `advertise` manifest (exact total length + slice count) on the node's control ID,
    then the lean id-matched data frames — CAN-FD tail windows DLC-padded
    (`can_fd_dlc_round_up`). **Ingress** learns the `id ↔ path` map from advertise
    frames, reassembles data slices via `mem_can_reassembly_t` keyed off the CAN ID
    alone, and trims back to the advertised total (undoing FD padding) → byte-exact.
  - **`can_link_t` seam** decouples the transport from the socket: `socketcan_link_t`
    is the production `PF_CAN`/`SOCK_RAW` impl (Linux-only via `#ifdef __linux__`,
    classic + CAN-FD, `transport_ws`-style concurrency hardening); tests pair two
    transports over an in-memory fake link, so the binding is fully testable with no
    kernel `vcan`.
  - Tested two ways: `core/tests/transport_can_test.cpp` (fake link — multi-frame
    byte-exact round trip classic + FD, advertise learning, DLC padding, lifecycle;
    under ASan/UBSan + TSan) and `core/tests/transport_can_vcan_test.cpp` (real `vcan0`,
    self-skipping; the dedicated `can-vcan-e2e` CI job sets `vcan0` up).

- **CAN transport — pure framing layer (increment 1 of [#55](https://github.com/avatarsd-llc/libtracer/issues/55); [ADR-0022](../docs/adr/0022-transport-framing-modes-elided-full-tlv-advertise.md), [ADR-0030](../docs/adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md)).**
  The host-testable, socket-free part of header-elided CAN. No SocketCAN / `vcan` /
  real socket — the `transport_can : transport_t` binding is a deferred increment.
  - `tr::net::can` (`can.hpp`): the structured **29-bit extended-CAN-ID codec**
    (`[version:4 | node:13 | endpoint:12]`; lower ID = higher bus priority), the
    `slice_can_id` address-shift helper, and the in-band **`advertise`** frame codec
    (`encode_advertise` / `decode_advertise`) — the identity↔path manifest.
  - `tr::view::view_can_frames_t` (`view_can.hpp`): L1 header-elided framing of one
    payload onto classic (≤8B) / CAN-FD (≤64B) data fields — zero-copy subviews,
    `to_rope()` reassembly, plus the `can_fd_dlc_round_up` DLC-lattice helper.
  - `tr::mem::mem_can_reassembly_t` (`mem_can_reassembly.hpp`): L0 multi-frame
    reassembly via `(origin, ts) + index → rope` (address-shift / advertise+id-match,
    **not** ISO-TP) with out-of-order, interior-gap, and totality-opt-in handling.
  - Documented in [docs/reference/14-can-transport.md](../docs/reference/14-can-transport.md);
    tested host-side in `core/tests/can_frames_test.cpp`.

- **`transport_t::send(iov)` — scatter-gather egress (the "rope we put into tx").**
  Ship a rope's `to_iovec()` as one frame with no flatten copy; `udp_transport_t`
  lowers it to a single `sendmsg(iovec)` syscall. Structural batching (the
  composition *is* the batch) rather than a Nagle-style timer: one syscall per
  composite, so network throughput scales with composition size while p50 latency
  stays flat. Measured (`bench/bench_scatter`): 5.1M values/s @ ~3µs (K=8) up to
  46.6M values/s @ ~12µs (K=256) — beating zenoh-c (3.5M/s @ 62µs) on **both**
  throughput and latency. Default impl gathers + calls `send(span)` (other
  transports unchanged). Tested in `udp_test` (`test_scatter_gather`).

- **`mem_cuda` GPU backend ([ADR-0024](../docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md)) — gated, GPU-tested.**
  `tr::mem::cuda_backend()` (DEVICE space; `cudaMalloc`/`cudaFree`) plus
  `tr::view::cuda_alloc` / `cuda_copy_from_host` / `cuda_copy_to_host`. A GPU-backed
  value is a **heterogeneous host(header)+device(payload) rope**. Built only with
  `-DLIBTRACER_WITH_CUDA=ON` (off by default; **never in CI** — no GPU). Built and
  **run on a real GPU locally** via `scripts/test-cuda.sh` (Docker + CDI;
  `cuda_test` passed alloc, H2D/D2H round-trip, and the heterogeneous-rope checks).

- **Memory-space tag (`tr::mem::mem_space_t` HOST/DEVICE) — the L1/L2 groundwork
  for `mem_cuda` and heterogeneous host+device ropes ([ADR-0024](../docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md)).**
  `mem_backend_t::space()` (default `HOST`) is inherited by each `segment_t.space`;
  `view_t::is_host()`/`is_device()` expose it; `rope_t::all_host()` reports whether
  a rope is CPU-walkable; `rope_t::flatten()` now **refuses a heterogeneous rope**
  (returns an empty view rather than CPU-`memcpy`'ing a DEVICE link). A
  `tr::view::borrow_device()` helper tags caller memory `DEVICE` (a CUDA-free
  stand-in for tests / custom bindings). No behaviour change for existing all-HOST
  code. Tested in `substrate_test` (`test_memory_space`).

- **`graph::delivery_mode_t` + per-subscriber delivery policy (first slice of the
  L4/L5 control-surface implementation).** `subscribe(...)` gains a defaulted
  `delivery_mode_t mode` (`EVERY` | `THROTTLED` reserved | `ON_CHANGE`). `ON_CHANGE`
  is enforced **producer-side** in `fan_out` (a subscriber is skipped when the new
  value bytes equal the bytes last delivered to it) — byte-agnostic, exactly the
  `SUBSCRIBER.qos_settings.delivery_mode` of [reference 05](../docs/reference/05-protocol-tlvs.md)
  ([ADR-0021](../docs/adr/0021-colon-field-plane-is-the-vertex-ioctl.md)). The
  `ON_CHANGE` compare/update happens under the vertex mutex (TSan-clean); dispatch
  stays outside it. Numeric filtering (deadband) remains an application filter
  vertex, not a field.

- **Internal — `<libtracer/byteorder.hpp>`:** one `constexpr` little-endian
  (de)serialization primitive (`detail::load_le` / `store_le` / `append_le`). The
  frame codec, router, graph, and path canonicalizer now funnel through it instead
  of each hand-rolling shift/mask loops — byte order lives in exactly one tested
  place. No wire change (conformance vectors unchanged; output byte-identical).

- **Internal — `<libtracer/tlv_emit.hpp>`:** one raw-bytes TLV header emitter
  (`detail::emit_tlv` / `emit_name`, built on `byteorder`). The ROUTER wrap, PATH
  canonicalizer, and `:schema` POINT builder now share it — with named `Type`/`Opt`
  constants instead of magic bytes — instead of each hand-rolling the
  type/opt/length header. No wire change (conformance vectors unchanged).

- **`graph::Graph::write(Vertex*, const FieldPath&, View)`** — a handle-based
  field-write so the control surface (`:settings.*`, `:subscribers[]`) is also
  string-free on the hot path: parse the path once (`Path::parse`), resolve the
  `Vertex*` + `FieldPath` once, then reuse them — no string parse, no map lookup
  per call. The string/`Path` overloads now route through it.

- **M5 — UDP socket transport.** The first transport that crosses the kernel
  network stack; the bridge/router/graph above it are unchanged.
  - `<libtracer/transport_udp.hpp>` — `UdpTransport(bind_port, peer_host,
    peer_port) : Transport` over POSIX UDP. `send` = `sendto`; an internal receive
    thread (`SO_RCVTIMEO` for clean shutdown) drains the socket into the receiver.
    One datagram = one whole frame (no stream reassembly), so it pairs with the
    flat decoder. `ok()`, `local_port()`. Validated raw + end-to-end through the
    full Graph+Bridge+ROUTER stack over localhost UDP (`tests/udp_test.cpp`);
    TSan/ASan/UBSan clean.
  - **`Bridge` perf/correctness:** the mount target is now resolved to a `Vertex*`
    once at `set_mount` (atomic) instead of a per-frame key copy + map lookup —
    faster, and data-race-free against the transport's receive thread (the mount
    vertex must be registered before `set_mount`).

- **M4 — first transport + bridge.** Two nodes talk over a "wire" (P2 bridge
  conformance); no sockets yet — an in-process loopback transport.
  - `<libtracer/transport.hpp>` — the `Transport` seam (`send` + `set_receiver`)
    and `PeerId` (the 16-byte ROUTER `origin_peer_id`).
  - `<libtracer/loopback.hpp>` — `LoopbackChannel`/`LoopbackEndpoint`, a dev/test
    transport over an in-memory channel with per-endpoint receive threads.
  - `<libtracer/router.hpp>` — `RouterMeta`, `router_wrap`/`router_unwrap` for the
    ROUTER envelope (docs/reference/05 §0x0D): NAME-tagged origin/ts/hop_count +
    the wrapped data TLV last; LL-aware emit, zero-copy unwrap.
  - `<libtracer/bridge.hpp>` — `Bridge`: `export_vertex` (egress — subscribe →
    ROUTER-wrap → send), ingress (unwrap → recent-set dedup on `(origin, ts)` →
    `hop_count`/`kMaxHops` termination → write the bare TLV to the mount vertex),
    `set_mount`/`set_recent_set_capacity`/`set_reforward`.
  - `examples/two_node_loopback.cpp` — node A publishes, node B receives over the
    loopback wire (encode→ROUTER→decode roundtrip). `tests/bridge_test.cpp` covers
    golden ROUTER, two-node delivery, dedup, and `hop_count` cycle termination.
    TSan/ASan/UBSan clean.

- **M3b — L4 subscriptions, dispatch, and the in-process P0 node.** Completes the
  in-process graph: pub/sub fan-out + field-write control surface.
  - `graph::Graph::subscribe(src, target)` and `subscribe(src, callback)` — a write
    to `src` fans out (a `SegmentPtr`-clone, no byte copy) to each target vertex
    (spec-faithful SUBSCRIBER re-dispatch) and/or in-process callback.
  - Field-write via `Graph::write(Path, View)` when the path has a field tail:
    `:subscribers[]` (append a SUBSCRIBER TLV target), `:subscribers[N]`
    (unsubscribe), `:settings.<field>` (QoS scalar update). `:schema` read returns
    a `POINT` descriptor.
  - `graph::Subscriber` and `kMaxDispatchDepth` (the in-process cycle bound,
    [ADR-0015](../docs/adr/0015-graph-runtime-concurrency-and-in-process-cycle-cap.md)).
  - `examples/in_process_pubsub.cpp` — the P0 node end to end (callback + target +
    `await` delivery), built and run as a CTest smoke test. TSan/ASan/UBSan clean.

- **M3a — L4 in-process graph runtime (core).** The data API per ADR-0006:
  `read` / `write` / `await`, keyed on canonical PATH-TLV payload bytes.
  - `<libtracer/status.hpp>` — `graph::Status` (the documented protocol error
    codes) and `graph::Result<T> = std::expected<T, Status>`.
  - `<libtracer/path.hpp>` — `graph::Path::parse` (canonical PATH payload bytes +
    `:field.sub[N]` tail, validated/canonicalized per `docs/reference/03`) and the
    `PathKey`/`PathKeyHash` vertex-map key.
  - `<libtracer/vertex.hpp>` — `graph::Vertex` with `Role` {stored-value, stream,
    handler}, `Settings` (core QoS), and the `Handlers` (`on_read`/`on_write`) seam.
  - `<libtracer/graph.hpp>` — `graph::Graph`: `register_vertex`, `read`/`write`/
    `await` (lock-free LKV read/write via an atomic `shared_ptr` swap; per-vertex
    condvar for blocking `await`), and `history` for streams. Validated race-free
    under TSan, leak/UB-free under ASan+UBSan (`tests/graph_test.cpp`). Subscriber
    fan-out + field-write follow in M3b.

- **M2 — L0/L1 memory substrate.** The layer that owns the lifetime of the bytes
  M1's borrowed `Tlv` points at; makes the zero-copy claim safe, not just fast.
  - `<libtracer/backend.hpp>` — `MemBackend`, the small user-implementable
    allocation seam (subclass it to bind any allocator / arena), and `IoDir`.
    Each backend declares its own concurrency/coherency contract (ADR-0012).
  - `<libtracer/segment.hpp>` — `Segment` (refcounted bytes + backend) and the
    intrusive `SegmentPtr` handle. Uses the canonical intrusive_ptr orderings
    required by `docs/reference/02-graph-model.md` (increment `relaxed`,
    decrement `acq_rel`). Build with `-DLIBTRACER_NO_ATOMIC` for single-threaded
    / Cortex-M0 targets (plain integer refcount; no cross-thread sharing).
  - `<libtracer/mem_heap.hpp>` — `mem::heap_backend()`, `mem::heap_alloc()`: the
    owning host allocator backend.
  - `<libtracer/mem_borrowed.hpp>` — `mem::borrow()`, `mem::borrow_const()`: wrap
    caller-owned bytes with a no-op-on-bytes destroy. The transparent
    byte-router / live-raw MVP (ADR-0012).
  - `<libtracer/mem_pool.hpp>` — `mem::Pool`: a bounded fixed-slab backend over a
    caller-owned buffer; `alloc` returns `nullptr` on exhaustion (BACKPRESSURE).
    The free list is threaded through the slab — no auxiliary allocation.
  - `<libtracer/view.hpp>` — `View` (zero-copy `(segment, offset, length)`
    window; copy = clone) and `view_as_tlv()` (the L1→L2 cast: a TLV is a cast
    from a view).
  - `<libtracer/rope.hpp>` — `Rope` (a chain of views spanning segments):
    `concat`, `walk`, `to_iovec` (scatter-gather egress), `flatten` (one-copy
    materialize).

- **M1 — protocol-v1 wire codec.** The L2/L3 borrowed (zero-copy) codec.
  - `<libtracer/tlv.hpp>` — `Type` (the `0x01`–`0x0D` registry) and the `Opt`
    options bitfield (decode/encode, reserved-bit rejection).
  - `<libtracer/crc.hpp>` — `crc::crc32c()` (Castagnoli, default) and
    `crc::crc16_ccitt()`; header-only, `constexpr` tables.
  - `<libtracer/frame.hpp>` — the `Tlv` model (`Tlv`, `Trailer`, `Timestamp`,
    `Crc`), `decode()` (→ `std::expected<Tlv, Error>`, iterative, depth-capped,
    CRC-verified), `encode()`, and `equal()`.
  - `<libtracer/tracer.hpp>` — umbrella header including the whole public API.
