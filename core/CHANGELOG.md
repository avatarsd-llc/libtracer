# Changelog — core (libtracer reference implementation)

All notable changes to the **public API** of the `core/` reference implementation
(the headers under `include/libtracer/`) are recorded here, per
[CONTRIBUTING](../.github/CONTRIBUTING.md) / [CLAUDE.md](../CLAUDE.md). This tracks the
*implementation's* C++ API — which is implementation-defined per
[ADR-0013](../docs/adr/0013-v1-scope-boundaries.md) and versioned independently of
the immutable **protocol-v1** wire format it implements.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The
reference implementation is pre-1.0; the first cut release is `[0.3.0]`, below.

## [Unreleased]

### Added

- **PlatformIO: ESP32 CAN bus driver (best-effort).** The PlatformIO package gains a
  `build.extraScript` hook (`integrations/platformio/pio_esp32_can.py`) that, on
  `espressif32` targets, compiles the ESP-IDF TWAI `can_link_t`
  (`twai_link.cpp`, already CI-built via the ESP-IDF component) so `transport_can` has a
  real on-chip bus driver; a no-op on every other platform. **Not yet verified on a
  physical board or in CI** — see the tracking issue.
- **Per-module build-time modularity — a node compiles only the modules it needs
  (CMake options, no feature macros).** `core/CMakeLists.txt` gains per-module options
  that toggle whether a module's translation unit(s) are **compiled** into `libtracer`
  (they are NOT compile-definitions — a dropped module leaves neither code nor a
  symbol): `LIBTRACER_TRANSPORT_TCP` / `_UDP` / `_WS` / `_CAN` (each adds/omits its
  `transport_*.cpp`, plus shared deps — `posix_endpoint.cpp` when any socket transport
  is on, `socketcan_link*.cpp` when CAN is on) and `LIBTRACER_NET_PLANE` (the FWD routing
  plane: `op_resolve*` / `route_handle` / `fwd_router` / `transport_vertex`). All default
  **ON**, so the default build is byte-for-byte the full node it is today; modularity is
  opt-**out**. The required core — the L2/L3 wire codec, the L0/L1 substrate, `path`, and
  the L4 graph runtime — always compiles, so a pure in-process node (`-DLIBTRACER_NET_PLANE=OFF`)
  or a graph+udp node (`-DLIBTRACER_TRANSPORT_TCP=OFF -DLIBTRACER_TRANSPORT_WS=OFF
  -DLIBTRACER_TRANSPORT_CAN=OFF`) links with no reference to the excluded modules
  (measured: the minimal graph+udp `libtracer.a` drops ~250 KB vs. the full node). The
  built-in `udp`/`tcp`/`ws` transport-factory registrations move out of
  `transport_vertex.cpp` — which hard-referenced the concrete transports — into one glue
  TU each (`builtin_transport_{udp,tcp,ws}.cpp`) behind the new internal
  `builtin_transports.hpp` seam, called through a `register_builtin_transports()`
  dispatcher (the hand-written `builtin_transports.cpp` for a full node; a
  CMake-generated variant naming only the enabled transports for a partial build), so a
  dropped transport leaves no dangling reference. Selection is by which TUs compile — no
  preprocessor `#ifdef` (the project's no-feature-macro doctrine, cf. `socketcan_link.cpp`
  vs. its stub). `LIBTRACER_WITH_CUDA` / `_QUIC`, `LIBTRACER_ACL_FULL`, and
  `LIBTRACER_INSTALL` are unchanged; the ESP-IDF component and the PlatformIO portable set
  still build the full node.

### Changed

- **Opaque `vertex_handle_t` + infallible `register_vertex` retire the raw-pointer graph
  API ([ADR-0056](../docs/adr/0056-vertex-handle-infallible-register.md) — breaking).**
  The caller-held vertex token is now `tr::graph::vertex_handle_t`, a non-null,
  pointer-sized, trivially-copyable opaque wrapper over the internal `vertex_t*` (identical
  pointer-load codegen; graph-only construction, no `operator*`/raw accessor). Every public
  `graph_t` taker — `read` / `write` (both overloads) / `await` / `assign` / `propagate` /
  `set_delivery_mode` / `history` / `read_subscribers` / `subscribe_wire` / field-write —
  now takes a `vertex_handle_t`; `find` returns `std::optional<vertex_handle_t>`;
  `ensure_vertex` / `register_vertex_key` and the `child_factory_t` return
  `result_t<vertex_handle_t>`. `register_vertex(const path_t&, …)` is now **infallible**,
  returning a `vertex_handle_t` directly and aborting on a `PATH_IN_USE` collision (a source
  bug on a literal path, like `path_t(std::string_view)`) — the pervasive
  `*g.register_vertex(...)` unchecked-deref idiom is gone. A fallible
  `try_register_vertex(const path_t&, …) -> result_t<vertex_handle_t>` covers genuine
  runtime-string sites. Added `settings(vertex_handle_t)` to read a vertex's QoS settings
  through the handle. Migration: drop the `*` on `register_vertex` and change held
  `vertex_t*` to `vertex_handle_t` (or `auto`); use `try_register_vertex` where a duplicate
  path is a real runtime outcome. Wire protocol and conformance vectors unaffected.

### Performance

- **Hardware-dispatched CRC-32C (byte-exact).** `tr::crc::crc32c` / `crc32c_state::feed`
  (`include/libtracer/crc.hpp`) now route their runtime path through a CPU-dispatched
  CRC-32C: the SSE4.2 `_mm_crc32_u8/u64` instruction on x86 (selected once via
  `__builtin_cpu_supports("sse4.2")`), the ARMv8 `__crc32cb/__crc32cd` instruction on
  aarch64 (`__ARM_FEATURE_CRC32`), and a portable **slice-by-8** table fallback for CPUs
  with neither. The SSE4.2 intrinsics are confined to a `target("sse4.2")`-attributed
  function, so the translation unit is **not** built for SSE4.2 and still runs on older
  CPUs. Compile-time CRCs keep the `constexpr` Sarwate table loop (guarded by
  `std::is_constant_evaluated`). The hardware, slice-by-8, and Sarwate paths produce
  **byte-identical** CRCs — the frozen conformance vectors and the CRC-16-CCITT path are
  unchanged. Header-only (no new translation unit).
- **Zero-allocation, transparent vertex-key lookup.** `path_key_hash_t` and the new
  `path_key_eq_t` (`include/libtracer/path.hpp`) are now heterogeneous (`is_transparent`),
  so `graph_t::find_ptr` keys the vertex map straight off a `std::span<const std::byte>`
  with **no** owned `path_key_t` copy and **no** FNV re-hash of a fresh vector on every
  internal by-key lookup (fan-out, ancestor bubble-up, ACL walk, FWD resolve). The by-span
  hash is byte-identical to the owned-key hash.
- **Transport receivers snapshotted into a local, re-copied only on change — not per frame.**
  The TCP / UDP / WebSocket RX loops (`src/transport_tcp.cpp`, `transport_udp.cpp`,
  `transport_ws.cpp`) took a mutex and copied the `std::function` receiver on **every**
  inbound frame — and the installed `fwd_router_t` receiver closure exceeds the
  `std::function` small-buffer, so that copy heap-allocated per frame. Each loop now holds a
  local snapshot and re-copies it under the lock **only** when `set_receiver` /
  `set_rope_receiver` set a per-transport `rx_dirty_` flag; the steady-state per-frame cost is
  one relaxed atomic load, no lock, copy, or allocation. Mid-run receiver swaps still take
  effect (they set the flag), and UDP no longer re-copies on idle-timeout wakeups.

## [0.3.0] — 2026-07-07

### Changed

- **Project version derives from the git tag, not a hardcoded number
  (packaging).** `core/CMakeLists.txt` now sets `project(VERSION …)` from
  `git describe --tags` (a `vX.Y.Z` tag), falling back to
  `LIBTRACER_FALLBACK_VERSION` (`0.3.0`) for an untagged checkout — so there is
  no hardcoded C++ version to drift from the tag. This **supersedes** the earlier
  pre-release must-fix bundle's hardcoded `project(libtracer VERSION 0.1.0)`
  (recorded further below) and aligns the reference with the `0.3.0` package
  manifests (`library.json`, `library.properties`, `idf_component.yml`).

- **The structural TLV descent is unified in `grammar::walk` (ADR-0048 §1
  completion — internal).** ADR-0048 unified the header *grammar*
  (`parse_header`); the open-node stack machine that turns headers into a tree
  was still hand-written twice (`frame.cpp decode` and `tlv_arena.cpp
  decode_into`), held equal only by the decode↔decode_into equivalence test. It
  now lives once in `grammar::walk`, driven by a per-decoder sink (owning-tree vs
  pre-order arena). Recursion-free and depth-capped as before; the walk's cursor
  stack draws from a caller-supplied `memory_resource`, so the slab-bound
  terminus decode stays heap-free. Public `decode` / `decode_into` signatures and
  output are byte-identical (verified: the equivalence + reject-parity test over
  every conformance vector, plus the rope-decode fuzzer, pass under ASan/UBSan).

- **`field_write` is the single SUBSCRIBER admission door (ADR-0049 — breaking).**
  `graph_t::add_remote_subscriber` is retired; the FWD resolver's wire append now
  enters `graph_t::subscribe_wire(v, source_view, return_route, link)`, which parses
  the SUBSCRIBER TLV ONCE (the resolver's parallel `delivery_compact` parse is gone)
  and lands in the same internal admission step as every other door. Deliberate
  behavior alignment, uniform across sugar / field-write / wire subscriptions:
  the SUBSCRIBE ACL gate and the transient-local (`durability == 1`) LKV latch now
  apply at every door — a LOCAL callback/target subscriber receives the latched
  value at subscribe exactly as a remote one always did. The `subscribe(src, target)`
  sugar now encodes a `SUBSCRIBER{PATH}` TLV through the field-write door, so its
  edge reads back from `:subscribers[]` byte-identically to a wire-made one
  (previously sugar edges were invisible to `:subscribers[]` reads).

### Added

- **CMake install/export — `find_package(libtracer)` now works (release
  packaging).** `core/` gains `install(TARGETS … EXPORT)`, header installation,
  and a generated package config (`libtracerConfig.cmake` +
  `libtracerConfigVersion.cmake`, `SameMinorVersion` compatibility for the pre-1.0
  API), so a downstream consumes the core with
  `find_package(libtracer 0.3 REQUIRED)` and
  `target_link_libraries(app PRIVATE libtracer::libtracer)` instead of
  source-vendoring the tree. Gated on `LIBTRACER_INSTALL` (default: on only when
  libtracer is the top-level project), so an embedding parent that
  `add_subdirectory()`s core never inherits install rules. The optional
  `libtracer_quic` module is intentionally not part of the installed package yet
  (its imported-`msquic` dependency can't be faithfully re-exported — a follow-up).
  The static-library artifact now ships as **`libtracer.a`** (target `OUTPUT_NAME
  tracer`), not the double-prefixed `liblibtracer.a`, so a non-CMake consumer links
  `-ltracer`; CMake consumers use the `libtracer::libtracer` imported target and
  are unaffected by the file name. A namespaced `libtracer::libtracer` **alias** is
  also defined in the build tree, so an `add_subdirectory()` / `FetchContent`
  consumer links the exact same target name as a `find_package` consumer.

- **`posix_endpoint.hpp` — the shared POSIX recv-thread/endpoint scaffold
  (internal).** `posix_endpoint_t` is a protected base owning the `stop_` flag +
  receive `thread_` (with `start()` / `stop_and_join()`) and the 100 ms
  shutdown-responsive socket idioms (`set_rcv_timeout` / `poll_readable` /
  `poll_accept`) that `tcp`/`udp`/`ws` (server + client) each open-coded. The
  teardown invariants (stop-and-join before releasing thread-touched resources;
  reset the fd under the write mutex before `::close`) are documented once on the
  base. No public transport API or wire behavior changes; the transports' own
  fds, receivers, counters, and write mutexes stay where they were. Also
  concentrates the thrice-cloned DIAL/LISTEN factory boilerplate in
  `transport_vertex.cpp` into `make_checked` / `dial_or_listen` helpers.

- **`security_acl.hpp` — the pure ACL policy seam (ADR-0050).** ACE evaluation
  leaves `graph.cpp`'s anonymous namespace for a pure per-target policy:
  `allow_only_policy_t` (the ALLOW-only MCU profile, the default) and
  `full_acl_policy_t` (ordered first-match-per-bit with DENY), selected at build
  time via the new CMake option `LIBTRACER_ACL_FULL` (ADR-0047 §1 — a
  target-configuration change, never an edit to `graph.cpp`). The typed ACE
  surface lands with it: `parse_acl<Policy>()` (strictness follows the policy —
  ALLOW-only rejects DENY at write time) and `encode_acl()` (replaces the
  hand-rolled ACE byte builders in `acl_test`/`subtree_test`). `ace_t` gains a
  leading `type` field (`ace_type_t::ALLOW`/`DENY`). Effective-ACL semantics
  (ADR-0020) are unchanged; the graph still owns the ancestor walk — the
  ADR-0050 cached effective-ACE merge is the follow-up behind the same
  interface.

- **`length_prefix_framer` exposes the shared framing-rule kernel.** New public
  statics `effective_cap(backend, max_frame)` (the RX cap =
  `min(max_frame, backend.max_segment_size())`) and
  `on_prefix(backend, cap, len)` → `prefix_decision_t`
  (`EMPTY` / `MALFORMED` / `DROP` / `ACCEPT{seg}`), plus a public `kPrefixBytes`.
  These are the per-prefix rules `feed()` already applied, now callable by
  pull-mode readers: `tcp_transport_t` consumes them directly instead of
  open-coding the identical logic, while keeping its direct-into-segment body
  read (ADR-0042 §2/§4 — chunk-feeding `feed()` there would add a copy).
  Behavior on the wire is unchanged.

- **`path_t(std::string_view)` — a parse-once constructor for literal paths (ADR-0054).**
  `path_t p("/sensor/temp");` parses the string ONCE; hold `p` and reuse it across
  operations (the graph API takes `const path_t&`, so a held path never re-parses on the
  hot path). `explicit`, and **infallible by hard-abort** — a malformed *literal* is a
  source bug, so it `std::abort()`s rather than returning a `result_t` the caller would
  `*`-deref unchecked (no exceptions; usable under `-fno-exceptions`). Use the fallible
  `path_t::parse` for a genuine RUNTIME string. This retires the repo-wide
  `*path_t::parse("…")` unchecked-deref idiom at 168 literal call sites (the two
  runtime-string sites keep `parse` + a check). `path_t() = default` is restored.

### Changed

- **`fwd_router_t::on_reply` is now rope-native (ADR-0055 — breaking).** The reply sink
  changes from `std::function<void(const wire::tlv_t&)>` to
  `std::function<void(const view::rope_t&)>`. The router no longer decodes or flattens a
  terminating `FWD{REPLY}` on the consumer's behalf — a rope-delivered reply reaches the
  sink zero-copy, and the sink materializes on demand: `reply.materialize()` yields the
  contiguous bytes (a single-link reply — the common case — is returned zero-copy, no
  alloc; only a multi-link reply pays one flatten), which feed `wire::decode` for the
  eager tree (the ADR-0052 escape hatch, now at the consumer). Migration for an existing
  `[](const tlv_t& r){ … wire::encode(r) … }` sink: take
  `[](const view::rope_t& r){ const view::view_t m = r.materialize(); const auto b =
  m.bytes(); … }` — hold `m` while reading its span; this also drops the old
  decode-then-re-encode round-trip. Deletes the `on_frame_rope` whole-frame flatten for
  replies; `tlv_t` and the ADR-0041 §2 span-arena contract are untouched. A follow-on
  (ADR-0055 §2/§3) makes the **control-frame** egress (`ADVERTISE` / `COMPACT` /
  `HANDLE_NACK`) rope-native too — the label is read off the rope and only the child
  sub-rope a handler needs contiguous (the route to re-encode, the payload to store) is
  materialized — which **deletes the `on_frame_rope` whole-frame flatten entirely**,
  completing the ADR-0053 ⑥ flatten sweep for the net plane. The only flattens left on the
  receive path are the legitimate span-tier decodes on the contiguous `on_frame` path and
  the on-demand payload/route sub-rope materializes at the control egress/store boundary.

- **Vertex value operations split into `assign` + `propagate`; `write` retained as
  their composition (RFC-0008 — breaking).** `graph_t` now exposes the two irreducible
  operations a `write` was hiding: `assign(v, value)` performs only the state transition
  (swap the last-known-value, append the stream ring, bump the write sequence, mark the
  vertex for the next sweep) and delivers **nothing**; `propagate(v)` performs only the
  edge transition — it delivers `v` (always — the argument is the explicit target) and
  sweeps its subtree, flushing the descendants assigned since the last covering sweep in
  *O((pending + unconditional)-in-subtree)*. `write(v, value)` stays as the eager
  convenience (assign, then a targeted delivery of the written vertex), which is exactly
  a `FWD{WRITE}` terminus, so existing callers are unaffected — but nothing fans out from
  `assign` itself.
- **`delivery_mode` redefined value-agnostic and moved per-vertex (RFC-0008 — breaking).**
  No longer a per-subscriber value filter — now a per-vertex policy governing whether an
  *ancestor's* `propagate` sweep includes the vertex: `IF_NEWER` (default — included only
  if assigned since the last covering sweep, the structural coalescing flush),
  `UNCONDITIONAL` (always — a sweep-driven keepalive), `EXPLICIT` (never by an ancestor;
  deliverable only by a direct `propagate` on the vertex). Set via the new
  `graph_t::set_delivery_mode`. The `delivery_mode_t mode` parameter is gone from
  `subscribe` / `add_remote_subscriber` (a subscription no longer carries a delivery
  policy), and `delivery_mode_t`'s values are now `IF_NEWER` / `UNCONDITIONAL` /
  `EXPLICIT`.

### Removed

- **Value-based delivery filtering (`delivery_mode_t::ON_CHANGE` byte-diff) and the
  throttle (`THROTTLED` / `min_interval_ns` / `keepalive_ns`) (RFC-0008).** The runtime no
  longer compares stored bytes (or wall-clock) to decide delivery — a vertex never parses
  its value (ADR-0053 §1), so what counts as "changed" is an application judgement, not the
  graph's. Selective delivery is now structural (the write-sequence sweep above). The
  `subscriber_t::mode` / `last_delivered` fields and the `rope_bytes_equal` /
  `rope_snapshot_bytes` helpers are gone. On the wire, `SUBSCRIBER.qos_settings` loses
  `delivery_mode` / `min_interval_ns` / `keepalive_ns` (reference/05); `delivery_compact`
  (label compaction) is orthogonal and retained.

### Added

- **`tr::view::rope_t` small-buffer inline storage + `only()` / `materialize()`
  (`rope.hpp`, ADR-0053 §6).** A `rope_t` now keeps its first two links in inline
  storage, so a single-link value (or a two-link head+payload) allocates nothing for
  the chain — the third link spills it to the heap. This is the trivial-case cost
  guard that lets the L4 graph store rope values without a per-write regression: a
  scalar write is still one allocation (`make_shared<rope_t>`), what the old `view_t`
  slot cost. Two consumer-facing accessors make the consumption form explicit:
  `only()` returns the sole link (asserts single-link, zero copy — for callers that
  know the value is contiguous), and `materialize()` returns the rope as one
  contiguous `view_t` (zero copy when single-link, one flatten copy otherwise —
  distinct from `flatten()`, which always copies). Locked by `tests/rope_test.cpp`,
  which structurally asserts 1–2 link ropes stay inline and the 3rd spills.
- **`tr::wire::tlv_view_t` (`tlv_view.hpp`) — the lazy rope-backed decode view
  (ADR-0053 §1).** What a rope-delivered frame becomes on the decode side:
  `tlv_view_t::over(rope)` anchors the bounds (root header, CRC **deferred**, exact
  `total == rope size`) and everything after is on-demand — `children().next()`
  parses exactly one child header per step, `body()`/`wire()` hand out refcounted
  subropes (zero copy, links keep their segments alive past the transport read
  loop — the owning tier, a scoped ADR-0041 §2 revision that leaves the span arena
  untouched), `verify()` checks *this* TLV's CRC trailer at access time (fully lazy
  validation, ADR-0053 §4; endpoints needing atomicity use verify-all-then-apply),
  `timestamp()` reads the trailer, and `materialize()` is the single explicit copy
  point (flatten + eager `decode` → `{flat, tlv_t}`). Own TU (`tlv_view.cpp`);
  a span-only target never instantiates the lazy tier. Gated by a lazy-vs-eager
  differential test and a full-lazy-walk mode in the rope fuzzer.
- **`tr::view::rope_t::subrope(off, len)` (`rope.hpp`)** — the `[off, off+len)`
  sub-range as its own rope: covering links trimmed via `view_t::subview`, so the
  result refcounts exactly the segments its window touches (chaining, no copy).
  The region primitive of the lazy decode tier.
- **`tr::wire::grammar::crc_check_t` (`grammar.hpp`)** — CRC-trailer policy for
  `parse_header`: `VERIFY` (the default — every pre-existing caller unchanged) or
  `DEFER` (sizing/bounds validated, payload never walked) for the lazy tier, so
  stepping over a sibling costs O(header), not O(payload).

- **`tr::wire::emit_header` (`tlv_emit.hpp`)** — the single home of the TLV header
  byte layout (`<type> <opt> <length>`, ADR-0048 §3). `emit_tlv` now delegates to it
  (still auto-widening `LL` for an oversize body), and `frame.cpp`'s `encode` routes
  its formerly hand-rolled header push through it (respecting the `tlv_t`'s existing
  `opt.ll`) — byte-identical output (conformance-gated), retiring the last hand-rolled
  header emission.

- **Rope-aware grammar: `tr::wire::validate_rope` + the rope byte-source cursor
  (ADR-0048 §1).** `validate_rope(const view::rope_t&) -> std::expected<void, err_t>`
  (`rope_decode.hpp`) runs the one grammar core (`grammar::parse_header`) over a
  scatter-gather rope through a new `grammar::rope_cursor`, so CAN reassembly / WS
  fragments can be **validated without flattening** — a header or trailer straddling
  a link boundary is stitched byte-by-byte, and a CRC-covered payload is fed
  link-by-link. It reaches the byte-for-byte identical verdict (and `err_t`) as
  `decode(flatten(rope))` for every adversarial split, gated by a differential test.
  Every link must be HOST (a device link is rejected `FRAME_INVALID`). The rope
  cursor lives in its **own TU** (`rope_decode.cpp`) so a span-only target never
  instantiates it. **Structure + CRC only** — materializing a rope into a `tlv_t` /
  arena node is the ratification-gated sink-type follow-on (both sink node types
  hold a borrowed contiguous `std::span` that cannot name a straddling payload,
  ADR-0041 §2).
- **`tr::crc::crc32c_state` / `tr::crc::crc16_ccitt_state` (`crc.hpp`)** — running
  CRC accumulators (feed contiguous chunks, then read `value()`) that are now the
  single home of the CRC init/final-xor constants; the existing single-/two-span
  `crc32c`/`crc16_ccitt` overloads delegate to them (byte-identical), and the
  grammar's incremental CRC over rope links uses them instead of open-coding the
  constants a third time.

- **Per-connection receive frame cap via `:settings max_frame` (kMaxFrame→:settings,
  tcp first).** `tr::net::conn_settings_t` gains a `max_frame` field (parsed from a
  `max_frame` SPEC `:settings` key), and `tcp_transport_t`'s constructors gain a
  trailing `std::size_t max_frame = 0` (`0` = the protocol default `kMaxFrame`,
  16 MiB). The receive cap is `min(max_frame, backend.max_segment_size())` — a
  connection can **tighten** its accepted frame size below the protocol ceiling
  (e.g. a heap-backed host connection wanting a hard cap without a bounded pool),
  but never raise it. Behavior-preserving default (unset ⇒ `kMaxFrame`, exactly
  the #216 behavior). The send-side `kMaxFrame` check is unchanged (it is the
  peer's limit, not the local receive cap). **`quic` and `webtransport` now honor
  the same `:settings max_frame`** — a trailing `std::size_t max_frame = 0` on their
  constructors flows through the pImpl to the shared `length_prefix_framer`, and
  their factories pass `settings.max_frame` — so all three length-prefix transports
  cap uniformly.

- **`tr::net::length_prefix_framer` (`length_prefix_framer.hpp`)** — the
  u32-LE length-prefix stream reassembler extracted from the **byte-for-byte
  identical** RX state machines `transport_quic` and `transport_webtransport` each
  open-coded (review finding #4, self-labeled "verbatim"). A chunk-fed state
  machine: each complete frame is reassembled into ONE exactly-sized refcounted
  segment from the caller's backend (ADR-0042 §2/§4), an `alloc` failure is
  backpressure (drain + a per-chunk `dropped` count), and an oversize prefix is
  `malformed` (the caller shuts the peer down). It carries **no msquic type, no
  atomic, no connection handle** — the transport keeps its counters/shutdown and
  drives them from the per-chunk `result_t` — so it is unit-tested directly
  (`length_prefix_framer_test`, 11 cases incl. split prefix/body, multi-frame,
  empty records, oversize, backpressure resync, reset) in the default build
  without a live QUIC connection. Behavior-preserving for both transports.

- **`tr::wire::opt_t::without_trailer()`** — returns the `opt` with the trailer
  bits (TS/CR/CW/TF) cleared, keeping only the structural bits (PL/LL). It
  replaces the raw `opt & 0x48` mask (`kStructOptMask`) that `op_resolve.cpp`'s
  ADR-0041 §4 trailer-sliced copy open-coded — the third representation of the
  `opt` bitfield (review finding #7) is gone; there is now one typed representation.
  Byte-identical to the retired mask for any validated opt byte (compile-time
  asserted). Behavior-preserving (byte-exact under conformance + the FWD suites).

- **One wire-grammar core behind a chunk-cursor seam (ADR-0048 §1, first
  increment).** The TLV header/trailer grammar — `type == 0x00` reject,
  reserved-bit reject, `LL` length width, trailer sizing, the two-span CRC verify
  — now lives **once** in `grammar.hpp`'s `tr::wire::grammar::parse_header`, read
  through a `span_cursor` byte-source seam. Both materializing decoders delegate
  to it: the owning `tlv_t` tree (`frame.cpp` `decode`) and the terminus arena
  (`tlv_arena.cpp` `decode_into`). Previously the grammar was **forked**
  (`parse_one` vs the arena's `parse_header`, ~40 lines held byte-for-byte equal
  only by the `decode`↔`decode_into` equivalence test — every future rule a
  two-file edit; review finding #7). **Behavior-preserving** (byte-exact under the
  conformance vectors + the equivalence test); the two decoders' iterative walks
  stay distinct (sinks differ). The cursor seam is where the ADR-0048 rope cursor
  (rope-aware decode) plugs in next. Net Cortex-M0 footprint: **−296 B** (the two
  forked parse functions fold to one instantiation).

- **Build-time-closed backend set + tag dispatch for segment release (ADR-0047
  §2, first increment).** `segment_t` now carries a `tr::mem::backend_tag`
  (inherited from its backend like `space`), and `segment_ptr_t::reset` reclaims
  through `tr::mem::destroy_dispatch` — a `switch` to a devirtualized (`final`-
  class, qualified) direct call per linked backend, with the backend's virtual
  `destroy` as the fallback for any unlisted tag (so dispatch is identical to the
  prior `backend->destroy` for every backend; **behavior-preserving**, validated
  under ASan/TSan + the full suite). `mem_backend_t` gains a `virtual tag()`;
  `heap_backend_t` is now a public type in `mem_heap.hpp` (was TU-local) so the
  dispatch can see it. A target that links only `mem_pool` defines
  `-DLIBTRACER_BACKEND_SET_POOL_ONLY` and the dispatch **folds to a single direct
  call** (the MCU single-member set). The follow-up that inlines the dispatch
  into `reset` (removing the small out-of-line seam — currently ~+20 B on the
  Cortex-M0 sentinel) needs the `mem_pool`↔`segment` header decouple, tracked for
  the next increment.

- **L0 module-set `constexpr` traits + `tr::mem::transfer` (ADR-0047 §2, second
  increment).** Each concrete backend now carries compile-time contracts as
  `static constexpr` members in place of prose: `needs_cache_ops` (does a transfer
  need the DMA cache hooks), `is_isr_safe` (`alloc`/`destroy` callable from an ISR
  — `mem_pool` yes, `mem_heap`/`mem_borrowed` no), and `owns_bytes` (bytes
  backend-owned and thus durably storable — false for a borrow). New
  **`tr::mem::transfer(seg, host, io_dir_t)`** is the tag-dispatched host↔device
  byte-mover: a host backend `memcpy`s (bracketed by `before_io`/`after_io` only
  when `needs_cache_ops`, so the bracket folds away at compile time on cacheless
  cores — the traits' and the I/O hooks' first in-tree consumer, review finding
  #8); a `DEVICE` backend (`mem_cuda`) routes to `cudaMemcpy` + the `after_io`
  stream barrier. Single-member (`-DLIBTRACER_BACKEND_SET_POOL_ONLY`) builds fold
  the transfer dispatch to one direct call. No Cortex-M0 footprint delta (the
  sentinel doesn't call `transfer`; `--gc-sections` drops it, traits are
  compile-time).

### Removed

- **`tr::view::cuda_copy_from_host` / `cuda_copy_to_host`** (public API, CUDA-only
  build): retired in favor of the general `tr::mem::transfer(seg, host, io_dir_t)`
  which subsumes both directions (`CPU_TO_DEVICE` / `DEVICE_TO_CPU`) and every
  host backend (ADR-0047 §2). `tr::view::cuda_alloc` is unchanged.

- **`tr::wire::key_view_t` — canonical-key NAME navigation (`key_view.hpp`).** One
  locus for walking a vertex-map key (the concatenated NAME-TLV encodings):
  `last_segment` / `parent` / `is_ancestor_of` / `child_record_under` /
  `split_levels`. The L4 graph previously open-coded this walking across ~seven
  sites in `graph.cpp` (last-segment, parent, ancestor/child, level split); those
  now funnel through the module. Behavior-preserving — no wire or existing-API
  change beyond the new header; contract pinned by `key_view_test.cpp`.

- **Stateless transport-peer enumeration + transparent per-peer FWD (ADR-0044,
  Brick C).** New kind-neutral bus capability on the transport seam:
  `tr::net::bus_link_t` (`enumerate_peers` / `peer_link` / `set_peer_receiver`)
  and `transport_t::bus()` (default `nullptr`). `child_registry_t::by_name` now
  falls back to asking each bus child to resolve an unknown segment as a live
  peer, and `fwd_router_t::add_child` installs a peer-named receiver on a bus
  link — so an announced bus peer's name is a routable next-hop segment and
  replies route back per-peer, with zero stored routing state. `transport_can`
  implements the capability: an insert-only last-heard peer table (one entry per
  distinct node id; `peer_ttl` silence expiry — new
  `transport_can_config_t::peer_ttl`, default `kCanDefaultPeerTtl`), peers named
  `n<node-id>`, a join-time **hello** advertise, and **directed** groups (the
  module-internal advertise framing is now format `0x02`: an 18-byte header with
  an explicit `target_node`; `advertise_t` gained `target` and
  `kCanBroadcastNode`). New `can_transport_factory()` registers a `kind = "can"`
  connection type (CAN-private config — `ifname`/`node`/`version`/`fd`/`path`/
  `peer_ttl_ms` — parsed by the factory from the raw config TLV, per the
  ADR-0043 §5 leanness ruling). On the graph side, `handlers_t` gained
  `on_children` (a synthesized `:children[]` listing hook honored for any role)
  and `graph_t` now serves the **`:children[]` field READ** (member enumeration:
  a `POINT` of `POINT{NAME}` members — the read dual of the SPEC-creating
  append), locally and through the FWD terminus; `transport_vertex_t` wires
  `on_children` for any bus-capable connection so `/net/<conn>:children[]`
  lists the currently-audible peers without creating any vertex.

- **Subtree subscriptions, branch-write decomposition, write-creates
  (RFC-0005).** Every subscription now observes writes to its vertex AND to any
  descendant: a write fans out to subscribers at the vertex and at each ancestor
  ("vertical bubbling"), delivering the written TLV as-is (local view clone /
  remote return-route FWD — unchanged machinery). The idle write path stays
  near-free — per-vertex listener counters gate the ancestor walk on one relaxed
  atomic load; the new `graph_t::ancestor_walks()` accessor exposes the walk
  count for tests/benches. A write whose payload is a `POINT` (`0x07`) tree
  rooted at the target vertex **decomposes**: each value-carrying node lands at
  the corresponding descendant vertex as a refcount SUBVIEW of the written frame
  (zero copy), missing vertices are created on the way, and each covered
  subscription point is notified once with its slice. New
  `graph_t::ensure_vertex(key, caller)` implements **write-creates** (`mkdir -p`,
  CREATE-ACL-gated on the nearest existing ancestor).

### Changed

- **The L4 graph value type is `tr::view::rope_t` (ADR-0053 §6 / step ④a) —
  breaking.** A vertex's stored value, its stream history, and subscriber fan-out
  now hold ropes, so a chunked stream (e.g. RTSP fragments reassembled by a
  transport) is stored and drained link-by-link with no per-chunk copy; a scalar is
  the single-link trivial case (unchanged cost, guarded by the new inline storage).
  The data API migrates rename-and-migrate (the #230 rope-receiver precedent,
  pre-1.0): `graph_t::read` / `await` / `read(v, field)` / `read(path)` /
  `await(path)` now return `result_t<rope_t>`; `history` returns
  `result_t<std::vector<rope_t>>`; `write` / the field-write overload / `write(path)`
  take `rope_t` (an existing `view_t` caller compiles unchanged via the implicit
  `view_t → rope_t`); `handlers_t::on_read` returns `result_t<rope_t>` and `on_write`
  takes `const rope_t&`; `subscribe(callback)` / `subscriber_t::callback` take
  `const rope_t&`; and `set_remote_delivery_sink`'s value is `const rope_t&`. A
  consumer needing contiguous bytes calls `rope_t::only()` (single-link, zero copy)
  or `materialize()` — the consumption form is now legible in the type; there is no
  silent-flattening parallel view API. The [ADR-0042](../docs/adr/0042-refcounted-receiver-seam-view-delivery.md)
  §3 referenced store generalizes to *subrope into the slot*. ON_CHANGE compares the
  new value against the last-delivered snapshot across links without flattening. One
  documented interim: `deliver_remote` materializes a multi-link value to contiguous
  bytes until step ⑤ makes the emission path scatter-gather.
- **WS supports RFC 6455 fragmented messages and delivers ropes (ADR-0053 §5 /
  step ②).** Both `transport_ws_server` and `transport_ws_client` now reassemble
  fragmented BINARY messages — previously a non-final fragment was delivered as if
  it were a whole frame and CONT frames were dropped — and join the owning tier:
  `set_rope_receiver` / `delivers_ropes() == true`, each completed message a rope
  with **one owning link per fragment** (reassembly is chaining, never a flat
  memcpy; the per-fragment copy out of the reused connection buffer is the single
  legitimate substrate-boundary copy). Control frames (PING) interleave mid-message
  per RFC 6455 §5.4; a BINARY mid-assembly (protocol error) restarts the assembly;
  a stray CONT is dropped. The span tier is unchanged for unfragmented messages
  (borrowed, zero-copy) and pays a single flatten for fragmented ones.
  `ws::encode_frame` / `ws::encode_client_frame` gain a trailing `bool fin = true`
  (existing callers byte-identical).

- **CAN reassembly delivers ropes (ADR-0053 §5 / step ③).** `bus_link_t` gains the
  owning peer-named seam — `peer_rope_receiver_t` / `set_peer_rope_receiver`
  (default no-op, same honesty rule as `set_rope_receiver`) and
  `bus_link_t::delivers_ropes()` — and `transport_can` implements it: a completed
  group crosses the seam as the rope its reassembly already built (one refcounted
  owning link per slice), with CAN-FD DLC padding trimmed by **shortening the tail
  link** (`rope_t::subrope`), never by flattening. The span-tier sinks
  (`set_peer_receiver` / `set_receiver`) still receive contiguous bytes and now pay
  their single flatten inside `deliver()` — the legitimate bridge-boundary copy —
  instead of every delivery paying it. `fwd_router_t::add_child` prefers the owning
  bus seam when available. Byte-exact delivery unchanged (existing round-trip /
  DLC-padding tests untouched and passing).

- **The owning delivery seam is rope-typed (ADR-0053 §5 / ADR-0042 generalized).**
  `transport_t::view_receiver_t` / `set_view_receiver` / `delivers_views()` are now
  `rope_receiver_t` (`std::function<void(view::rope_t)>`) / `set_rope_receiver` /
  `delivers_ropes()` — "delivers views" and "delivers ropes" are ONE capability
  (CONTEXT.md §ingress rope delivery), a contiguous frame being the trivial
  single-link rope. All four owning transports (tcp/udp/quic/webtransport) deliver
  exactly the single-link ropes their old views were (behavior-preserving);
  `fwd_router_t` routes a single-link rope over the identical zero-heap span path
  as before, and flattens a multi-link rope ONCE (the documented ADR-0053 interim
  recipe, removed when partial-path routing lands). This unblocks CAN/WS
  reassembly delivering their scatter-gather frames as the ropes they already are.

- **The stream transports' receive frame cap is now the injected backend's real
  capacity** — `min(kMaxFrame, backend.max_segment_size())` (kMaxFrame→pool-bound,
  first slice). A length prefix claiming more than the rx backend could ever
  allocate (e.g. a bounded `mem_pool`'s slot) is now rejected up front as
  malformed, so an undeliverable frame is never drained (the no-synthetic-limits
  doctrine: the bound is the injected resource, not just a magic constant). The
  cap lives in `length_prefix_framer::feed` (so `transport_quic`/`transport_webtransport`
  get it with no change) and inline in `transport_tcp`'s receive loop. **No
  regression for heap-backed connections** (`max_segment_size()` ≈ unbounded ⇒ the
  cap stays `kMaxFrame`). Full removal of `kMaxFrame` (a per-connection `:settings`
  override) is a follow-up.

- **`tr::view::over_bytes` returns `std::optional<view_t>`** (was `view_t`,
  review finding #9 / L1 contracts). It no longer conflates the two outcomes an
  unowned `view_t` used to share: **`std::nullopt`** is an allocation failure the
  caller maps to BACKPRESSURE; an **engaged, empty** view is a legitimately-empty
  input span. The call sites (graph read_schema/read_acl/read_children, the FWD
  resolver's `own_tlv`, `fwd_router` local delivery, `transport_vertex`,
  `transport_can`, and the two heap benchmarks) branch on the optional instead of
  hand-disambiguating an empty view. Behavior-preserving (each site's prior
  failure semantics kept).

- **CAN reassembly rehomed `tr::mem::mem_can_reassembly_t` →
  `tr::net::can_reassembly_t` (ADR-0048 round 2).** The header moves
  `mem_can_reassembly.hpp` → **`can_reassembly.hpp`** and the type moves to
  **`tr::net`**, beside `transport_can`. This resolves a self-admitted layer
  inversion — an L0 (`tr::mem`) type that referenced the L1 `rope_t` it assembles;
  the reassembly is a transport-plane concern that composes L1 views, like any
  transport. The two internal `std::map`s become `std::pmr::map`s over an
  **injected `std::pmr::memory_resource`** with a **config-bounded live-group
  count** (evict-oldest + a `dropped_groups()` counter), so a constrained node
  degrades by a bounded drop, never OOM (the no-synthetic-limits doctrine). The
  defaults — the process heap, unbounded — preserve the prior behavior
  (verified by the existing `can_frames` / `transport_can` suites); `reassembly_key_t`
  and `can_origin_id_t` move to `tr::net` with the same shape. Public API change
  for anyone naming the type directly (transports use it internally).

- **The wire decoders return `tr::wire::err_t` (error.hpp), not the deleted
  decode-local `error_t` (ADR-0048).** `decode`, `decode_into`, and `view_as_tlv`
  now yield the RFC-0002 registry code directly — so `err_path` / `err_severity` /
  `err_disposition` come for free and there is no parallel decode-only error
  vocabulary. The four decode outcomes keep their names (`FRAME_TRUNCATED` /
  `FRAME_INVALID` / `FRAME_CRC_FAIL` / `TLV_NESTING_TOO_DEEP`) and the conformance
  `ERR:<name>` strings are unchanged; a `std::expected<…, wire::error_t>` in a
  caller becomes `wire::err_t` (value-compatible, same names). Behavior-preserving.

- **`view_t::subview` / `view_t::bytes()` now assert their bounds preconditions
  in debug builds** (`sub_offset + sub_length <= length`; `offset + length <=
  segment size`). Zero release cost — `assert` compiles out under `NDEBUG`, so the
  Cortex-M0 footprint is unchanged (the sentinel build now passes `-DNDEBUG`, a
  release profile) — while the fuzz + sanitizer CI catches an out-of-bounds view
  at its source instead of as a silent over-read. Hardens the L1 window invariant
  the review flagged as prose-only.

- **The raw-TLV byte emitters moved from `tr::detail` to `tr::wire`
  (`tlv_emit.hpp`): `emit_tlv` / `emit_name` are now `tr::wire::emit_tlv` /
  `tr::wire::emit_name`.** They produce wire bytes from wire types (`type_t`,
  `opt_t`), so they are a codec (L2/L3) concern, not a layer-free `tr::detail`
  primitive — removing the `tr::detail`-reaches-up-into-`tr::wire` anomaly the
  architecture review flagged. Pure relocation: identical bytes (conformance
  vectors unchanged); the LE byte helper `detail::append_le` (byteorder.hpp)
  stays in `tr::detail`. Update callers `detail::emit_*` → `wire::emit_*`.

- **`transport_can` ingress now filters by protocol-version prefix and ignores
  self-echoed frames** (frames whose CAN-ID `version` differs from the
  transport's, or whose `node` equals its own, are dropped before any map/table
  processing) — the ADR-0030 discovery-layer-versioning band made explicit.
  `transport_can::send` is unchanged for callers; broadcast sends stamp
  `target_node = kCanBroadcastNode`. The CAN advertise frame layout changed
  incompatibly (format `0x01` → `0x02`); it is transport-internal framing
  (ADR-0030), not the L2 wire spec — all nodes of one bus run one binding
  version.

- **A data write to a nonexistent path now creates it (RFC-0005 write-creates)**
  instead of returning `NOT_FOUND` — both the local `graph_t::write(path, …)`
  and the remote `FWD{WRITE}` terminus (`op_resolver_t`). `:field` writes,
  `read`, and `await` on a nonexistent vertex keep `NOT_FOUND`. A `POINT`
  payload written to a stored-value/stream vertex is now a branch write
  (decomposed) rather than an opaque store; handler-role vertices still receive
  it as-is via `on_write`.
- **`socketcan_link_t` moved to its own translation units — the last in-source
  platform `#ifdef` is gone** (#183). `core/src/transport_can.cpp` is now 100%
  portable (it talks only to the `can_link_t` seam); the Linux `PF_CAN`
  implementation lives in `core/src/socketcan_link.cpp` and the always-off
  stub in `core/src/socketcan_link_stub.cpp`, selected by the build system
  (`CMAKE_SYSTEM_NAME`), per the no-feature-macro ruling. No API change; the
  `transport_can.hpp` doc comment now states the build-system selection, and
  platform ports (the ESP-IDF component's `twai_link_t`) implement the same
  seam in their own tree.
- **`udp_transport_t` RX is now bounded by the injected backend and keeps its
  scratch off the recv-thread stack** (#183). RX segments are sized
  `min(kMaxDatagram, backend->max_segment_size())`, so a `pool_t` over an
  MCU-sized static slab (slot payload ≪ 64 KiB) receives datagrams instead of
  dropping every one (ADR-0042 backpressure by injection); the legacy
  borrowed-span path's 64 KiB scratch is allocated lazily on the heap on first
  use instead of living in the recv thread's stack frame (which would overflow
  FreeRTOS/pthread stacks on-target). Heap-backed hosts see identical behavior.

### Fixed

- **`route_handle_t::clear_link` data race / use-after-free (leak).** `tables()` and
  `find_tables` release the registry `shared_mutex` before the caller locks the
  per-link mutex, so the `link_tables_t&` they return could dangle: `clear_link`
  `erase`d that registry entry, and a concurrent `ensure_egress`/`bind_ingress`
  mid-write then operated on a destroyed `link_tables_t` — a use-after-free that
  orphaned the egress buffer (a leak LeakSanitizer intermittently caught in the
  `fwd_fanout` writer-vs-clear stress). `clear_link` now **empties the entry in
  place** under the per-link mutex instead of erasing it, leaving `links_`
  insert-only so every handed-out table reference is stable for the object's
  lifetime (`std::map` nodes never move). Semantically identical (cleared bindings,
  allocator restart at label 1, counts) — regression-covered by `route_handle_test`
  and the now-deterministic `fwd_fanout` concurrency test (100× ASan / 40× TSan clean).

- **v0.1 release must-fix bundle (pre-release hardening).** The CMake project now
  declares its version (`project(libtracer VERSION 0.1.0 CXX)`), aligning the C++
  reference with the `0.1.0` TypeScript packages. Fixed the two `-Wunused-result`
  warnings in `socketcan_link_t::write_raw` (`core/src/transport_can.cpp`): the
  `::write` return is now checked and a failed/short write drops the frame
  best-effort, mirroring the RX side's skip-and-continue policy. A full build is
  warning-free again.

### Added

- **ADR-0043 Phase B — `webtransport_transport_t`, the WebTransport-over-HTTP/3
  endpoint in the `libtracer_quic` module** (new public header
  `transport_webtransport.hpp`; NOT in the `tracer.hpp` umbrella; same module
  target — one msquic investment serves QUIC and WebTransport, and the core
  library still has **zero** msquic/H3 references). LISTEN mode serves browsers:
  H3 SETTINGS advertising extended CONNECT (RFC 9220), H3 datagrams (RFC 9297)
  and ENABLE_WEBTRANSPORT/WT_MAX_SESSIONS; the extended CONNECT
  (`:method=CONNECT, :protocol=webtransport`) answered with 200; then ONE
  WebTransport bidirectional stream (the browser's
  `createBidirectionalStream()`) carrying the SAME 4-byte u32-LE length-prefix
  framing as `tcp_transport_t`/`quic_transport_t`, with ADR-0042 owning view
  delivery and backpressure. DIAL mode implements the client half of the same
  handshake (self-contained C++ e2e; native clients). The H3/QPACK layer is a
  deliberately minimal, module-private codec (`src/wt_h3.hpp`: SETTINGS+HEADERS
  frames, zero-dynamic-table QPACK with the static-table subset, Huffman
  decode-only — no ls-qpack, nothing vendored; the subset's sufficiency is
  documented in the header and pinned against RFC vectors). Registered via
  `webtransport_transport_factory()` as kind `webtransport`, with the
  kind-PRIVATE `cert`/`key` config keys (the ADR-0043 §5 leanness ruling —
  `conn_settings_t` unchanged). New `session_up()` observer reports the
  extended-CONNECT state. Companion TS package:
  `@avatarsd-llc/libtracer-webtransport` (browser client, identical framing).

- **ADR-0043 Phase A — `quic_transport_t`, the msquic QUIC transport as a SEPARATE
  MODULE** (new library target `libtracer_quic` + public header
  `transport_quic.hpp`; NOT in the `tracer.hpp` umbrella). A host that talks QUIC
  links the module and registers its factory —
  `net.register_transport_type("quic", quic_transport_factory())` — through the
  existing catalog extension seam; a host that doesn't simply never compiles these
  sources: the core library has **no msquic reference, no feature macro, no `quic`
  builtin** (open/closed — `transport_vertex` is extended, not modified). The
  `-DLIBTRACER_WITH_QUIC=ON` CMake option only controls whether the module target
  is configured (msquic must be installed; OFF by default). One QUIC connection
  carries ONE bidirectional stream with the SAME 4-byte u32-LE length-prefix
  framing as `tcp_transport_t` (16 MiB cap; an oversize prefix is malformed —
  `malformed_rx()` ticks and the connection is shut down). DIAL
  (`quic_transport_t(host, port, quic_dial_tls_t)`, synchronous handshake; trust
  via a CA bundle or the DEV-ONLY `insecure_no_verify` flag for self-signed certs)
  and LISTEN (`quic_transport_t(port, cert_pem, key_pem)`, one inbound peer at a
  time; ephemeral `0` resolved via `local_port()`). RX reassembles msquic RECEIVE
  chunks into ONE exactly-sized refcounted segment from the injected
  `mem::mem_backend_t*` — ADR-0042 owning delivery (`delivers_views() == true`);
  backend exhaustion is backpressure (frame skipped, `dropped_rx()` ticks,
  framing sync survives). TX copies each frame ONCE into a heap buffer msquic
  owns until SEND_COMPLETE (the msquic buffer-lifetime contract — the only
  library-held buffer); `send(iov)` makes that same single gather copy (msquic's
  multi-buffer send cannot borrow the seam's call-scoped spans). Link state via
  the QUIC connection events (`link_up()`). The factory consumes `addr` + `port`
  (DIAL, dev-grade no-verify TLS) or `port` + the new **`cert`/`key` config keys**
  (LISTEN). `cert`/`key` are quic-PRIVATE config keys the factory parses ITSELF
  from the raw config SETTINGS TLV — the shared `conn_settings_t` stays lean with
  only the universal keys (the ADR-0043 §5 leanness ruling); accordingly
  `transport_vertex_t::transport_factory_t` now receives
  `(const conn_settings_t&, const wire::tlv_t* raw_config)` (`raw_config` = the
  SPEC's config SETTINGS TLV, may be null; the built-ins ignore it). A new
  `tools/gen-dev-cert.sh` emits a self-signed dev certificate pair. Per-flow
  streams, RFC 9221 datagrams, and WebTransport are staged follow-ons
  (ADR-0043 Phase B). CI: a dedicated `quic` workflow builds msquic and runs the
  full suite + ASan/UBSan + TSan with the module on; default jobs are unchanged.

- **M6 — `tcp_transport_t`, the reliable stream transport** (new public header
  `transport_tcp.hpp`, included by the `tracer.hpp` umbrella). A TCP `transport_t`
  with **4-byte u32-LE length-prefix framing** — the prefix is transport framing, NOT
  part of the TLV; a prefix announcing more than `kMaxFrame` (16 MiB) is malformed:
  counted via the new `malformed_rx()` and the connection is torn down. Two modes,
  one class: DIAL (`tcp_transport_t(host, port)`, synchronous connect) and LISTEN
  (`tcp_transport_t(port)`, one inbound peer at a time — the `transport_ws_server`
  model; an ephemeral `0` resolved via `local_port()`). The receive thread
  reassembles partial reads and honors record boundaries on coalesced writes, reading
  each frame straight into ONE refcounted segment from the injected
  `mem::mem_backend_t*` (default heap) — ADR-0042 owning delivery
  (`delivers_views() == true`); without a view receiver the span receiver borrows the
  same segment bytes. Backend exhaustion is backpressure: the frame is drained off
  the stream (framing sync survives) and `dropped_rx()` ticks. `send(iov)` puts the
  prefix as the first iovec entry ahead of the rope's spans — one gathered `sendmsg`,
  no flatten copy. Reconnect is out of scope (#66 owns link lifecycle). The transport
  factory gains a **`tcp` builtin** beside `udp`/`ws` (DIAL: `addr` + `port`; LISTEN:
  `port`), threading the `rx_backend` seam like `udp`.

- **The refcounted receiver seam — transports MAY hand up owning frames, and big WRITE
  payloads may store zero-copy as frame subviews**
  ([#173](https://github.com/avatarsd-llc/libtracer/issues/173),
  [ADR-0042](../docs/adr/0042-refcounted-receiver-seam-view-delivery.md)). New public
  API, all additive and defaulted:
  - `transport.hpp`: `transport_t::set_view_receiver(view_receiver_t)` — the optional
    OWNING inbound sink (each frame a `view::view_t` over a refcounted segment; base
    impl is a documented no-op) — and `transport_t::delivers_views()` (default `false`),
    the capability a view-delivering transport overrides. No adapter wraps a borrowed
    span into a lying view; span-only transports keep span semantics.
  - `transport_udp.hpp`: `udp_transport_t` gains a `mem::mem_backend_t* backend`
    constructor parameter (default `&mem::heap_backend()`; listener mode included) —
    with a view receiver installed, each datagram is `recvfrom`'d straight into a fresh
    `kMaxDatagram` segment from that backend and handed up owning (one datagram = one
    frame = one segment); backend exhaustion is backpressure (drop + the new
    `dropped_rx()` counter), never an OOM. Without a view receiver the span path is
    byte-identical to before. `delivers_views()` returns `true`.
  - `transport_vertex.hpp`: `transport_vertex_t` gains an optional `rx_backend`
    constructor parameter (default heap) threaded into the built-in `udp` factory, so
    config-constructed sockets participate in owning delivery with the host's memory
    policy.
  - `vertex.hpp` / graph `:settings`: `settings_t::store_ref_min_bytes` (u32, default
    0 = disabled; writable via `:settings.store_ref_min_bytes`) — on a view-delivered
    terminus frame, a WRITE whose trailer-less payload TLV is ≥ the threshold stores a
    **subview of the frame** (refcount pin, zero copy) instead of the ADR-0041 one-copy
    `own_tlv`; smaller/trailered payloads and span-delivered frames keep the copy, and
    the remote-subscriber return route keeps its subscription-scoped one-copy behavior.
  - `op_resolve.hpp`: `op_resolver_t::resolve` gains an optional
    `const view::view_t* frame_view = nullptr` parameter (the owning frame, threaded by
    `fwd_router_t` from a view-delivering link); existing callers are unchanged.
  `fwd_router_t::add_child` now installs the receiver matching the link's capability;
  the forward hop stays span-based zero-heap (`ZEROHEAP_MAX=0` unaffected) — the
  refcount rides only to the terminus. The big-payload WRITE path thus copies its bytes
  **zero** times between the socket and the LKV.

- **Config-constructed socket transports — the `:children[]` SPEC now builds the real
  socket** ([#83](https://github.com/avatarsd-llc/libtracer/issues/83) final piece;
  [ADR-0027](../docs/adr/0027-transport-and-connections-are-vertices.md)). New public API
  in `transport_vertex.hpp`: `transport_vertex_t::register_transport_type(kind, factory)`
  — a transport-factory catalog mirroring the graph's child-type catalog — with
  `transport_factory_t` returning an owning `std::unique_ptr<transport_t>` from the
  parsed `conn_settings_t`; `conn_settings_t` gains `kind` (the config's transport
  selector, parsed from a `NAME "kind" NAME <kind>` pair). Built-ins registered by the
  constructor: **`udp`** (DIAL: bind ephemeral, peer = `addr:port`; LISTEN: bind `port`,
  peer learned from inbound datagrams — `udp_transport_t` constructed peer-less now
  adopts each datagram's source, so a listener replies to a dialing client's ephemeral
  port) and **`ws`** (DIAL: `transport_ws_client(addr, port)`, a synchronous
  connect+handshake at creation; LISTEN: `transport_ws_server(port)`, one inbound peer).
  When a connection SPEC names a `kind` and no `provide_link` was staged, the connection
  vertex **constructs and owns** the transport, wires it into `fwd_router_t` exactly as a
  provided link, and writes its link state up; `provide_link` remains the test/manual
  seam and **takes precedence** when staged. Errors are clean statuses: unknown `kind` ⇒
  `SCHEMA_NOT_FOUND`, config missing the fields the kind requires ⇒ `TYPE_MISMATCH`,
  bind/dial failure ⇒ `NOT_FOUND` (no vertex is created on any failure). Lifecycle
  (honest): with no child-removal model yet (#66) an owned transport lives as long as its
  `transport_vertex_t`, whose destructor joins the recv threads — declare it after the
  router it feeds. Verified: `transport_vertex_test` grows a two-node
  config-created-UDP end-to-end (FWD{READ} out A's SPEC-built socket to B's terminus and
  the REPLY back over B's learned peer), provide-link-precedence, and creation-error
  cases; all sanitizers clean; forward-hop zero-heap gate unaffected (setup-time only).

- **ACL enforcement, core subset** (#81; [ADR-0018](../docs/adr/0018-access-control-authorization-pluggable-subject-token.md)/[0020](../docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md)/[0026](../docs/adr/0026-consumer-initiated-subscription-client-write.md)).
  New public API in `graph.hpp`/`vertex.hpp`: `subject_token_t`, `subject_resolver_t`,
  `graph_t::set_subject_resolver` (the pluggable subject-token seam — **no resolver ⇒
  enforcement disabled**, today's behavior, one null check on the hot path), `acl_right_t`
  (the §0x0A access-mask bits), `ace_t` + `kAceInherit` (the parsed ALLOW-only ACE), and a
  defaulted `std::string_view caller` on `graph_t` `read`/`write`/`await`/
  `read_subscribers` (source-compatible; the FWD terminus passes its `inbound_link`
  through as the caller context, local calls default to trusted). A `:acl` write now
  **parses** the ACEs (rejecting a DENY ACE or flag bits beyond the single `INHERIT` with
  `TYPE_MISMATCH` so subset evaluation never silently weakens stored semantics) and, with
  a resolver installed, gates: READ/AWAIT by `READ`, writes by `WRITE`, `:subscribers[]`
  append by the producer's `SUBSCRIBE` and fan-out re-dispatch by the *target's* `WRITE`
  under the edge's stored caller (the ADR-0026 two-ACL pair; `subscriber_t` gains
  `caller`), `:children[]` by `CREATE`, `:acl` read/write by `READ_ACL`/`WRITE_ACL`.
  Effective ACL = own ACEs + `INHERIT`-flagged ancestor ACEs (walked at check time; empty
  ⇒ open). Denial returns `status_t::PERMISSION_DENIED` (`tr::access::denied` `0x0050` on
  the wire). New conformance vector `acl/acl-aces`.

### Removed

- **`op_resolver_t::resolve(const wire::tlv_t&, …)` — the `tlv_t` resolver overloads are
  deleted** ([ADR-0041](../docs/adr/0041-terminus-arena-decode-span-contract.md) §5; Brick 5
  part 2). `op_resolver_t::resolve` is rewritten over the **terminus arena**:
  `resolve(const wire::tlv_arena_t&, std::string_view inbound_link = {})`. Callers migrate
  to `wire::decode_into` (the terminus never builds a `tlv_t` anymore). Behavior fixes
  riding the rewrite: **trailer-sliced stores** (§4 — a CRC/TS-carrying WRITE stores
  header+body only, with the copied opt byte's trailer bits cleared; fixes the
  trailer-less-at-rest violation where `encode()` re-emitted arriving trailers into stored
  values), **span-aliased vertex lookup** (§3 — a canonical PATH body IS the vertex-map
  key: zero key materialization; non-canonical PATHs fall back to a re-emit), and the
  **direct-emitted reply head** (one exactly-sized segment replaces the 4-stage
  encode→children→head→segment staging: route bytes copied once). The remote-subscriber
  `return_route` is likewise a single trailer-sliced copy of the `src` span.

### Changed

- **`kind=ERROR` replies emit the RFC-0002 structured ERROR and a new `error.hpp`
  registry header ships** ([RFC-0002](../docs/spec/rfcs/0002-protocol-error-model.md),
  ADR-0009/0010). New header-only `include/libtracer/error.hpp` (`tr::wire`): `err_t`
  (the frozen u16 registered codes of the `tr::<concept>::<error>` namespace),
  `err_severity_t` / `err_disposition_t`, and constexpr `err_path` / `err_severity` /
  `err_disposition` lookups. `op_resolve.cpp` now emits
  `STATUS{ ERROR{ VALUE u16 LE code } }` (ERROR always structured, first child = the
  identity) instead of the withdrawn flat `STATUS{ ERROR u8 }` byte codes — breaking
  for pre-freeze draft consumers only (no released v1). The
  `fwd/fwd-reply-error` conformance vector is regenerated to the new layout and three
  `errors/` vectors are seeded (`error-registered-code`, `error-registered-detail`,
  `error-string-form`); the TypeScript client's `FWD_ERROR` / `replyErrorCode` follow
  the registry, and a `replyErrorPath` accessor is added for the string-form identity.

- **`route_handle_t` label state is per-connection and pmr-backed** (Brick 4 of the #83
  Stage-2 flip; [ADR-0038](../docs/adr/0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md)
  §3 / [ADR-0039](../docs/adr/0039-pmr-memory-model-host-aligned-allocation.md)). The
  node-global `std::mutex` + four node-global `std::map`s are gone: each link owns its
  own flat pmr entry tables (ingress bindings, egress routes — which double as the
  `route → label` index — and the label allocator) guarded by **its own mutex**, so
  label traffic on one connection never contends with another; the only cross-link lock
  is a `shared_mutex` over the link registry, taken exclusively at create/clear
  (setup/reconnect frequency). New ctor
  `route_handle_t(std::pmr::memory_resource* = get_default_resource())`; `fwd_router_t`
  passes its injected resource through, so a bounded node's label state lives entirely
  in the host slab (proven by the new `route_handle_test`, which runs the whole
  lifecycle over a slab resource with a `null_memory_resource` upstream). Public
  accessor API unchanged.

- **The remote-subscriber `return_route` is a refcounted segment view**
  ([ADR-0041](../docs/adr/0041-terminus-arena-decode-span-contract.md) §2; Brick 5
  part 3). `graph_t::add_remote_subscriber` takes `view_t return_route` (was
  `std::vector<std::byte>`), `subscriber_t::return_route` and
  `remote_delivery_t::return_route` are `view_t` (were vector/span). The route is
  copied **once** at subscribe (trailer-sliced); every later delivery snapshot is a
  refcount clone — **O(deliveries) route copies → O(1)** — and an in-flight delivery
  keeps the route alive across a concurrent unsubscribe. The full-route producer
  delivery is now **scatter-gathered** (fresh stack heads + the roped stored route and
  value views — a delivery copies no payload/route byte; `build_delivery`'s
  per-delivery frame materialization is gone); a transport without native
  scatter-gather gathers once in the seam's default `send(iov)`.

- **`fwd_router_t` gains a defaulted `std::pmr::memory_resource*` constructor parameter**
  ([ADR-0039](../docs/adr/0039-pmr-memory-model-host-aligned-allocation.md) §1 /
  [ADR-0041](../docs/adr/0041-terminus-arena-decode-span-contract.md) §5):
  `fwd_router_t(graph, mr = std::pmr::get_default_resource())`. The terminus arena draws
  from it **directly — the library holds no internal buffer**: a bounded node injects a
  pool resource over its static slab (one slab, whole stack) and the terminus then
  allocates nothing from the global heap; the default is the standard heap (a terminus may
  allocate). The FWD plane no longer builds a `tlv_t` anywhere: forward hops offset-dispatch
  (unchanged, zero-heap, CI-gated), terminus requests arena-decode, and only the originator
  `on_reply` sink and the ADVERTISE/COMPACT/NACK control frames keep the owning
  `wire::decode`. `bench_forward_heap` gains a report-only **terminus mode** (armed window
  around one local READ resolve) making the terminus allocation count visible; the
  `ZEROHEAP_MAX=0` forward gate is unchanged and still passes.

- **`bridge_t` and the ROUTER-flood mechanism are retired**
  ([ADR-0040](../docs/adr/0040-net-plane-is-explicit-source-routed-only.md); Brick 3b of the
  #83 Stage-2 flip). The net plane is now **`FWD` explicit-source-routed only** — every remote
  endpoint is addressed by an explicit path (`/net/ws/<peer>/…` vs `/net/can/<peer>/…` are
  *different* addresses, so parallel links are deliberate redundancy, never auto-multipath that
  needs `(origin, ts)` dedup). Removed public API: `bridge_t` (with `export_vertex`, `set_mount`,
  `set_status_path` and its recent-set/`hop_count`/HLC-clock machinery) and the ROUTER codec
  helpers `router_wrap`/`router_unwrap`/`router_meta_t` (`router.hpp`/`router.cpp`) — they served
  only `bridge_t`. The `0x0D ROUTER` **wire codepoint stays reserved and decodable** (the
  `router-wrapped` conformance vector is unchanged; retiring a codepoint would be a needless spec
  change) for a possible future flooding profile. FWD is loop-free by construction
  (`dst`-monotonicity + `INVALID_PATH` on revisit), so no loop safety is lost; provenance is the
  accumulated `src` route (RFC-0004 §B). `peer_id_t` stays (the node identity). Two-node delivery
  over a real transport is now covered by `udp_test`'s FWD path. This resolves ADR-0037/0038's
  "the two side-channels dissolve": `fwd_router_t::children_` → `child_registry_t` (Brick 3a),
  `bridge_t` → *retired*, not relocated.

### Added

- **`wire::decode_into` + `tlv_arena_t`/`arena_tlv_t` — the terminus arena decoder**
  ([ADR-0041](../docs/adr/0041-terminus-arena-decode-span-contract.md), implementing
  [ADR-0038](../docs/adr/0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md)
  invariant #5 / [ADR-0039](../docs/adr/0039-pmr-memory-model-host-aligned-allocation.md) §3;
  Brick 5 of the #83 Stage-2 flip, part 1). New header `tlv_arena.hpp`:
  `wire::decode_into(span, std::pmr::memory_resource&) → std::expected<tlv_arena_t, error_t>`
  parses a frame into a **flat, pre-order arena of `arena_tlv_t` nodes** drawn from the
  injected resource — each node `{type, opt, wire (header+body span, trailer excluded),
  body, end (one-past-last-descendant), canonical_path}`, every span zero-copy into the
  input. Identical validation to `decode` (bounds, reserved bits, type `0x00`, `kMaxDepth`,
  two-span trailer CRC, trailing-byte rejection), iterative, no recursion. `canonical_path`
  marks a PATH whose body is byte-identical to its `path_key` form, enabling the ADR-0041 §3
  span-aliased vertex lookup. `frame.hpp` (`tlv_t`/`decode`/`encode`) is byte-for-byte
  untouched — the arena is a distinct terminus-local representation, not a codec change.
  Verified by the new `tlv_arena_test`: `decode` ↔ `decode_into` equivalence over **every**
  conformance vector, all four trailer shapes trailer-sliced, pre-order/`end`/sibling
  iteration, canonical + all non-canonical PATH fallbacks, the depth cap, 11 rejection
  branches error-for-error, and a zero-spill decode inside a 4 KiB stack
  `monotonic_buffer_resource` with a `null_memory_resource` upstream. The resolver rewrite
  over the arena (deleting the `resolve(const tlv_t&)` overloads) is part 2.
- **`tr::net::child_registry_t`** — the connection demux table (`NAME → transport
  link`, `by_name`/`by_segment`), extracted from `fwd_router_t`'s private `children_`
  field into one named, shareable owner (Brick 3a of the #83 Stage-2 flip;
  [ADR-0037](../docs/adr/0037-net-side-channels-dissolve-into-vertex-tree-compositor.md)
  compositor demux, [ADR-0038](../docs/adr/0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md)
  §3b). The `NAME → link` table is no longer duplicated between `fwd_router_t` and
  `transport_vertex_t` — the router owns the single registry (exposed read-only via
  `fwd_router_t::registry()`), and `transport_vertex_t` populates it. Layering-safe:
  the registry is `tr::net` (L5) and holds `transport_t*`, *not* a `graph.find` against
  an L4 vertex (which must never know a transport). Pure dedup — byte-identical routing,
  zero-heap forward gate still PASSES, no behavior change. `fwd_router_t::add_child` is
  unchanged; the private `child_by_segment`/`link_by_name` are gone.
- **Transport / connection as a `/` vertex — Stage-1 shell** ([ADR-0027](../docs/adr/0027-transport-and-connections-are-vertices.md)
  / [ADR-0037](../docs/adr/0037-net-side-channels-dissolve-into-vertex-tree-compositor.md)
  Stage-1; [#83](https://github.com/avatarsd-llc/libtracer/issues/83)). New
  `tr::net::transport_vertex_t` registers `client`/`listener` child types on a `graph_t`
  (via the #82 `register_child_type` seam), so an in-band `write /net:children[] +=
  SPEC{type, name, config{addr,port,role,keepalive}}` instantiates a connection at
  `/net/<name>` — a first-class `/` vertex that carries its transport-private
  `:settings`, is `await`-able for link up/down (`set_link_state`), and — Stage-1 — wires
  its pre-supplied `transport_t&` (`provide_link`) into `fwd_router_t` so **bytes still
  flow the tested FWD path unchanged**. This is the (A) shell over the live path
  (ADR-0037 Stage-1): the vertex/compositor model is proven with zero regression; the
  dissolution of `fwd_router_t::children_` into `graph.find` is the Stage-2 flip, and
  real per-transport socket construction from the config replaces `provide_link` as a
  follow-on that plugs into the same catalog seam. New public API:
  `transport_vertex_t`, `conn_role_t`, `conn_settings_t`. Verified: new
  `transport_vertex_test` (in-band create + resolve, `:settings` parse, `await` link
  up/down, FWD-still-routes zero-regression, and the intra-device-path-untouched
  invariant); 24/24 ctest, ASan/UBSan/TSan clean, perf-gate PASS — the local
  write→subscriber path is unchanged (ADR-0038 §3a: a same-device edge is a direct
  call + deref, the net plane off its hot path by construction).
- **In-band `:children[]` SPEC vertex creation** ([ADR-0017](../docs/adr/0017-in-band-vertex-creation-controller-orchestration.md)
  / [ADR-0021](../docs/adr/0021-colon-field-plane-is-the-vertex-ioctl.md); [#82](https://github.com/avatarsd-llc/libtracer/issues/82),
  the ADR-0037/0038 Stage-1 prerequisite). A `write` of a `SPEC{ type, name, config? }`
  (`0x0E`) into a parent's `:children[]` field now instantiates a child vertex of a
  **device-catalog type** — the graph composes the child's canonical key (parent key +
  the SPEC `name` NAME) and dispatches on `type`; unknown type ⇒ `SCHEMA_NOT_FOUND`
  (the ENOTTY of creation), duplicate name ⇒ `PATH_IN_USE`, non-SPEC value ⇒
  `TYPE_MISMATCH`. New public API: `graph_t::register_child_type(type, factory)` (the
  device populates its creation catalog; the built-in `stored_value` type is registered
  by the constructor) and `graph_t::register_vertex_key(key, role, …)` (register by a
  pre-composed key, the in-band creation dual of the string-parsed `register_vertex`).
  The `graph_t::child_factory_t` seam is where #83 plugs transport-connection types.
  Verified: new `children_test` (create+resolve / built-in / unknown / duplicate /
  non-SPEC / custom factory); 23/23 ctest, ASan/UBSan/TSan clean, perf-gate PASS (the
  catalog is off the read/write hot path).
- **Bridge hop-limit local error** ([ADR-0014](../docs/adr/0014-router-cycle-termination-hop-count.md)
  "MUST emit a local error", [#77](https://github.com/avatarsd-llc/libtracer/issues/77)).
  A `hop_count >= MAX_HOPS` drop now emits `STATUS=ERROR(NESTING_TOO_DEEP)` (wire code
  `0x0D`) to the subscribers of the bridge's status path — new public API
  `bridge_t::set_status_path(const path_t&)` (mirrors `set_mount`: resolves the vertex
  once; the receive thread emits through it with no per-frame lookup). Unset status
  path ⇒ silent drop (counter-only), as before. The spec reuses `NESTING_TOO_DEEP` for
  hop exhaustion; a distinct `HOP_LIMIT` code would be a spec change (RFC), not done
  here. Verified: new `bridge_test` hop-limit case asserts the emission (not just the
  `hop_dropped()` counter); 22/22 ctest, ASan/UBSan/TSan clean, perf-gate PASS.
- **Two consolidated byte-idiom helpers** (one audited locus each, used across the
  codec/router/graph). `view::over_bytes(span) → view_t` collapses the repeated
  `heap_alloc` + `memcpy` + `view_t::over` triplet (graph `read_schema`/`read_acl`,
  the FWD resolver's reply-head and WRITE-payload, `fwd_router`'s local delivery, the
  bridge's ingress materialize, the CAN reassembly slice) into one place — and skips
  the allocation entirely for an empty span. `detail::as_string_view(span) →
  std::string_view` is the byte↔char-string counterpart, replacing the
  `reinterpret_cast<const char*>` idiom repeated across the codec/router (NAME
  payloads, link names). Pure refactor — no behavior change on the hot path (verified:
  22/22 ctest, perf-gate PASS, ASan/UBSan/TSan clean).
- **Producer remote fan-out + `delivery_compact` auto-promotion**
  ([RFC-0004](../docs/spec/rfcs/0004-remote-operation-addressing.md) §D/§E.1 /
  [ADR-0035](../docs/adr/0035-implementing-rfc-0004-remote-operation-addressing.md)
  slice-4 completion, [#136](https://github.com/avatarsd-llc/libtracer/issues/136)). A
  write to a vertex that has a remote subscriber now
  fans out a delivery back over the subscriber's link with no explicit
  advertise/send. New public API: `graph_t::set_remote_delivery_sink(...)` and
  `graph_t::add_remote_subscriber(v, source_view, return_route, link, delivery_compact,
  mode)`; the `graph::remote_delivery_t` sink contract; `subscriber_t` gains
  `return_route` + `link`; `op_resolver_t::resolve(fwd, inbound_link)` (an overload —
  the no-arg form is unchanged); and `route_handle_t::ensure_egress(link, route) →
  {label, fresh}` (the lazy advertise-once primitive). `fwd_router_t` registers the
  graph sink in its constructor and emits a full-route `FWD{WRITE}` by default, or —
  for a `delivery_compact` subscriber — auto-advertises a label once then streams
  `COMPACT` (re-advertising after `clear_link`). A **transient-local** producer
  (`durability == 1`) latches its current value to a fresh subscriber on subscribe.
  No wire-format change (the codec and all conformance vectors are unaffected). Tested
  in `fwd_fanout_test.cpp` (incl. a TSan writer × `clear_link` race) and end-to-end
  against the TS client over a live socket (`fwd_node_server` no longer hand-rolls the
  delivery).

- **`tr::net::route_handle_t` + `fwd_router_t` route-handle — ws delivery-compaction**
  ([RFC-0004](../docs/spec/rfcs/0004-remote-operation-addressing.md) §E.1 /
  [ADR-0035](../docs/adr/0035-implementing-rfc-0004-remote-operation-addressing.md),
  slice 4). A new `route_handle.hpp` (`tr::net::route_handle_t`) holds per-link
  `label ↔ route` tables; `fwd_router_t` gains the producer-side `advertise(link,
  route) → u16 label` and `send_compact(link, label, payload)`, the inbound
  `ADVERTISE`/`COMPACT`/`HANDLE_NACK` handlers, `clear_link(link)` (the reconnect
  self-heal hook), `handles()` (introspection), and the `on_raw` / `on_compact_delivery`
  / `on_stale_label` observers. An established, `delivery_compact`-flagged delivery
  flow is compacted to a per-link **u16 label** (swapped each hop, MPLS-style)
  advertised in-band; lean `COMPACT` frames then carry only the label + value instead
  of a full-route `FWD{WRITE}`. One-shot / cold / non-compact flows allocate **no**
  label state (the slice-3 stateless property holds). New transport-plane type codes
  `ADVERTISE=0x11`, `COMPACT=0x12`, `HANDLE_NACK=0x13` (`tr::wire::type_t`) — these
  ride a link alongside `FWD`, are not core conformance TLVs, and carry no vectors.
- **`SUBSCRIBER.qos_settings.delivery_compact`** (`graph::subscriber_t::delivery_compact`)
  — the consumer's opt-in to label-compacted deliveries, decoded from the SUBSCRIBER's
  `qos_settings` SETTINGS (`NAME "delivery_compact" VALUE u8`). Optional / NAME-tagged
  ⇒ back-compatible; absent leaves the full-route delivery path unchanged.
- **`tr::net::fwd_router_t` — stateless multi-hop `FWD` forwarding + zero-copy
  `src` accumulation across transports** ([RFC-0004](../docs/spec/rfcs/0004-remote-operation-addressing.md)
  §A/§B / [ADR-0035](../docs/adr/0035-implementing-rfc-0004-remote-operation-addressing.md),
  slice 3). Wires a local `graph::graph_t` (terminus op resolution via the slice-2
  `op_resolver_t`) to a set of NAMED transport children (ADR-0027). On an inbound
  `FWD` (`add_child(name, link)` installs the receiver): if the first `dst` segment
  names a local non-transport vertex, the op is applied and the `FWD{REPLY}` is sent
  back over the link the request arrived on; if it names a transport child, the
  segment is **stripped from `dst`** and the inbound-link `NAME` is **prepended to
  `src`** as a rope head-insert (the original accumulated route and the payload ride
  on as zero-copy views — no byte of the route or payload is moved) before the
  shortened `FWD` is sent onward. A `FWD{op=REPLY}` routes by the same step but does
  **not** accumulate `src`; when its `dst` is fully consumed it is delivered to the
  `on_reply` sink. Forwarders are **stateless** — the forward route (`dst`) and the
  return route (`src`) live in the frame, so there is no per-request table and a hop
  may reboot mid-operation. New public API: `fwd_router_t` with `add_child`,
  `on_reply`, `on_inbound` (an observability/ACL-seam hook), and `on_frame`. Proven
  over live `transport_ws` by the `fwd_multihop` integration test (byte-exact
  `dst`-shrink / `src`-grow + round-tripped value; ThreadSanitizer-clean). The
  route-handle (per-link label compaction) is slice 4.
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
  **run on a real GPU locally** via `tools/test-cuda.sh` (Docker + CDI;
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
  (`wire::emit_tlv` / `emit_name`, built on `byteorder`). The ROUTER wrap, PATH
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
