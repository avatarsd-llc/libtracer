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
