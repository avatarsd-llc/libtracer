# Changelog — libtracer ESP-IDF component

Notable behavior changes of the `integrations/esp-idf/libtracer` component (the
ESP-IDF packaging of the `core/` reference implementation plus the chip-native
platform links: `httpd_ws_link_t`, `twai_link_t`), per
[CLAUDE.md](../../../CLAUDE.md). The core C++ API itself is tracked in
[core/CHANGELOG.md](../../../core/CHANGELOG.md).

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed

- **`httpd_ws_link_t` (adopted mode): the destructor now retires every session's close
  callback before the link dies (#816).** Each admitted peer is registered with
  `httpd_sess_set_ctx(handle, fd, slot, on_session_closed)`, so a link that adopted an
  external server used to leave that `free_ctx` pointing into freed memory: the next
  peer disconnect — or the adopting server's own `httpd_stop`, arbitrarily later — called
  it. The destructor now queues a self-contained detach onto the server's control queue
  (`httpd_queue_work`), which is the only context allowed to touch the session table, and
  clears each session's ctx/free_ctx there (`httpd_sess_set_ctx(.., nullptr, nullptr)` —
  which runs the outgoing callback inline, while the link is still alive) before closing
  the sessions; it returns only once that has happened. A destructor running ON the
  adopting server's own task detaches inline instead, because queued work there could
  only run after the destructor returns (the same self-deadlock #815 fixed for the TX
  pool). If the detach cannot run at all — the queue refuses it, or the server task is
  wedged past the teardown drain bound — the still-armed session slots are neutralised
  and LEAKED with a warning rather than freed under a live callback, so the late
  callback lands on valid, inert memory. No public API change; owning mode is unaffected
  (`httpd_stop` already closes every session synchronously). Teardown-only: on-device
  nodes leak the link by design, so the exposure was host/testbench teardown and any
  future dynamic-reconfiguration path.

## [0.7.0] — 2026-08-02

### Added

- **`tr::esp::critical_pool_t` — a thread-safe, slab-bounded `mem_backend_t` for a
  single-core ESP32 (#770).** `include/libtracer_esp/critical_pool.hpp`. `portmux_sync_t`
  implements core's `tr::mem::pool_sync_policy` with `portENTER_CRITICAL_SAFE` /
  `portEXIT_CRITICAL_SAFE`, and `critical_pool_t` is `tr::mem::synchronized_pool_t` specialised
  on it — the interrupt-disable variant
  [ADR-0060](../../../docs/adr/0060-lkv-copy-store-injected-value-backend.md) §2 names for a
  priority-preemptive target, where core's spinlock `sync_pool_t` would let a high-priority task
  spin on a lock its lower-priority holder cannot release. `is_isr_safe` is `true` (the section
  disables interrupts, and the `_SAFE` macros pick the ISR-context primitive). Inject it wherever
  a shared byte seam must live in the application's own slab — `transport_vertex_t`'s
  `rx_backend`, `graph_t`'s `value_backend`, `fwd_router_t`'s `flat`; a bare `tr::mem::pool_t` is
  unsynchronised and wrong at all three. **Opt-in construction — nothing defaults to it**, and an
  app that never names it links nothing (the compile-gate TU `critical_pool.cpp` instantiates the
  specialisation on chip targets so the adapter is type-checked by every build; `--gc-sections`
  drops it). The bundled `full_node` example wires it behind its per-target platform seam.

- **`esp_ws_client_link_t` — an ESP-IDF `esp_transport_ws`-backed WebSocket *client*
  `transport_t` (dial-out).** The embedded-native counterpart to core's portable
  `transport_ws_client` and the dial mirror of `httpd_ws_link_t`: the RFC 6455 opening
  handshake, masking/framing, and PING/PONG/CLOSE control frames run inside ESP-IDF's
  tested `esp_transport_ws` (over core `tcp_transport`), instead of the raw-socket client's
  hand-rolled handshake that does not reliably complete on lwIP silicon. One inbound BINARY
  message is one libtracer TLV, read directly into a reusable buffer (server→client frames
  are unmasked, so RX is a zero-copy fill) and delivered borrowed in-call to the router;
  `send()` emits one masked BINARY frame, copied once into a reusable scratch (client
  masking is in-place, so a shared frame must not be mutated). Point-to-point: `bus()` is
  nullptr, `delivers_ropes()` is false. A single recv thread owns the read side and re-dials
  with backoff (a transient drop keeps subscriber edges, resuming on reconnect); all handle
  access is serialized by one syscall-brief mutex. Chip-only, gated on
  `CONFIG_LIBTRACER_TRANSPORT_WS` (default `y`); adds `tcp_transport` (core IDF) to the
  unconditional `PRIV_REQUIRES`. The portable `transport_ws_client` still compiles; a node
  that binds this link leaves it unreferenced for `--gc-sections` to drop, the same
  coexistence as `httpd_ws_link_t` vs the raw server.

- **`CONFIG_LIBTRACER_TRANSPORT_{UDP,TCP,WS,CAN}` — per-transport Kconfig knobs (#393).**
  The component now gates each built-in transport's translation units on its own
  menuconfig bool, mirroring the core CMake options `LIBTRACER_TRANSPORT_{UDP,TCP,WS,CAN}`.
  All four **default `y`**, so a stock node compiles the full transport set and
  `libtracer.a` is byte-identical to before. Turning one **off** sheds that transport's
  factory glue + socket TU(s) from the archive — e.g. UDP drops `builtin_transport_udp` +
  `transport_udp`; WS additionally drops the chip-native `httpd_ws_link_t`; CAN drops
  `transport_can` + the TWAI `twai_link_t` — and a `SPEC` of that trimmed `kind` then
  resolves to `SCHEMA_NOT_FOUND` (the factory is simply absent). This is the RAM-audit
  "compose the C6 for its role" lever (shed unused transports), with **no in-source
  feature macros**: selection is which TUs the build compiles, and the
  `register_builtin_transports` dispatcher is CMake-generated to call only the enabled
  factories (the hand-written full-node form is used when udp+tcp+ws are all on, so a
  stock build is unchanged). The transport `PRIV_REQUIRES` (`esp_driver_twai`,
  `esp_driver_gpio`, `esp_http_server`, `lwip`) stay **unconditional** — a Kconfig-gated
  REQUIRES never propagates in IDF's early requirement-expansion pass — and are free when
  their TU is absent (`--gc-sections` drops the link dep).

### Changed

- **`CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES` now rides the generated `libtracer/config.hpp`**
  ([ADR-0068](../../../docs/adr/0068-build-configuration-is-plain-cpp-config-header.md))
  instead of a PUBLIC compile definition: the component `configure_file`s the header from the
  Kconfig value and lists it before `core/include`, so every dependent TU reads the same
  constexpr `tr::graph::kVertexLockStripes` — the ODR hazard the PUBLIC `-D` existed to manage
  is gone by construction. menuconfig behavior is unchanged.

- **`httpd_ws_link_t` steady-state RX/TX no longer allocates per frame (#814).** Two
  once-per-link buffers replace the per-frame heap on the hot paths, bringing the server
  link toward `esp_ws_client_link_t`'s allocation discipline. **RX:** a frame that fits the
  2 KB reusable scratch (all graph control TLVs) is read into it and delivered borrowed —
  the exact-size `new (std::nothrow)` remains only as the fallback for larger frames (up to
  the 32 KB abuse cap), trading one allocation for not pinning 32 KB permanently. **TX:** a
  send claims one of 4 pre-allocated work slots **lock-free** (a CAS scan; senders on any
  task, released by the httpd task as the send drains) and gathers the frame straight into
  the slot's ~1.5 KB inline payload — no allocation. A frame past the inline capacity keeps
  the pooled shell and takes a nothrow heap payload; a momentarily exhausted pool falls back
  to the previous fully-heap work item. Every fallback stays nothrow with the same
  drop-on-OOM backpressure (`note_tx_result` streak accounting unchanged) — never an abort.
  No API change; ~8.5 KB of heap moves from per-frame churn to one construction-time
  allocation per link.

## [0.6.0] — 2026-07-23

## [0.5.0] — 2026-07-21

### Added

- **`CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES` — Kconfig knob for the vertex
  lock-stripe count.** libtracer's one process-global mutable buffer is the
  vertex lock-stripe table (`core/include/libtracer/vertex.hpp`, #361 §2): it
  costs N lazily-allocated FreeRTOS mutexes (~90 B of heap each) plus N
  condition variables once a `graph_t` exists, independent of how many vertices
  are registered. The core header already honored a
  `-DLIBTRACER_VERTEX_LOCK_STRIPES` override; this exposes it through
  menuconfig (default **16**, unchanged) and propagates it as a **PUBLIC**
  compile definition so every translation unit that includes
  `<libtracer/vertex.hpp>` agrees on the `inline constinit` table size (a
  mismatch would be an ODR violation). A single-core chip node can drop it
  (e.g. 4–8) to reclaim RAM; the lock-free LKV read/write hot path is
  unaffected — only control-plane lock contention (ring trim, edge/ACL
  mutation, `await` wake) rises. Doctrine-pure per RFC-0006 (bounds are
  per-target config, never a synthetic magic number).

### Changed

- **`httpd_ws_link_t` heap exhaustion no longer aborts the node (OOM soft-fail).**
  With `-fno-exceptions`, the throwing allocator turned heap exhaustion on the
  in-call WS service path into `abort()` (decoded on-device: 3/3 browser-session
  crashes on the httpd task while serving a ~12.7 KB composed-read reply). All
  buffers on that path are now nothrow with graceful degradation: (1) TX — the
  reply is gathered **once** from the caller's iovec into the queued work item
  via `new (std::nothrow)` (the link and its `peer_endpoint_t` now override
  `transport_t`'s iovec `send()`, eliminating the base default's throwing gather
  temporary and the frame's double-buffering); a gather-OOM lands in the existing
  `note_tx_result` drop/close ladder as a counted drop. (2) RX — the pass-2
  payload buffer is nothrow (OOM closes that session), and fragment reassembly
  moved to a nothrow exact-size regrow buffer (OOM drops the in-flight message,
  keeps the peer). No wire-visible change: peers observe the pre-existing
  drop/close backpressure behavior instead of a device reboot.

- **`httpd_ws_link_t` no longer goes silently deaf on a failed WebSocket send.**
  Previously the TX work callback ignored the `httpd_ws_send_frame_async` result,
  so a send failure (e.g. a large fragmented frame timing out the socket's 5 s
  `SO_SNDTIMEO`) left the session open while the peer silently missed frames.
  Now a failed send logs a warning and **triggers the session's close**
  (`httpd_sess_trigger_close`, flowing through the normal `free_ctx` teardown),
  so the client's `onclose` fires and it can reconnect into clean state.
  Likewise, TX enqueue drops (`httpd_queue_work` refused / work-item OOM) — which
  were silently discarded — are now logged and counted per session; a session
  accumulating `kMaxConsecutiveTxDrops` (3) consecutive drops with no successful
  enqueue in between is treated as broken and closed the same way. Clients that
  held a connection during TX failure will now observe a disconnect instead of a
  half-dead session.
