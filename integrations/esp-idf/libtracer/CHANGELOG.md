# Changelog — libtracer ESP-IDF component

Notable behavior changes of the `integrations/esp-idf/libtracer` component (the
ESP-IDF packaging of the `core/` reference implementation plus the chip-native
platform links: `httpd_ws_link_t`, `twai_link_t`), per
[CLAUDE.md](../../../CLAUDE.md). The core C++ API itself is tracked in
[core/CHANGELOG.md](../../../core/CHANGELOG.md).

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

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
