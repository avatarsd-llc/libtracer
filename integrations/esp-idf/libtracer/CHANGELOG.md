# Changelog — libtracer ESP-IDF component

Notable behavior changes of the `integrations/esp-idf/libtracer` component (the
ESP-IDF packaging of the `core/` reference implementation plus the chip-native
platform links: `httpd_ws_link_t`, `twai_link_t`), per
[CLAUDE.md](../../../CLAUDE.md). The core C++ API itself is tracked in
[core/CHANGELOG.md](../../../core/CHANGELOG.md).

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- **`esp_ws_client_link_t::dropped_rx()`** — inbound messages the receive path refused,
  spelled the way core's transports already spell it (`transport_can::dropped_rx()`).
  It counts the two refusals that were previously logs-only (#953): a message that does
  not fit `rx_bytes`, and a stray WebSocket CONTINUATION arriving with no message open.
  A frame lost to a connection drop is not counted — that is the link going down, not
  the receive path refusing a message.

### Fixed

- **`httpd_ws_link_t`'s queued TX no longer delivers one peer's frames to another after a
  descriptor is reused** (#954, partial — the directed-resolve residue is #1013). The
  whole TX path identified its destination by bare fd: `send()`
  snapshotted fds under the peer lock and released it before enqueueing, the queued work
  item stored only `int fd`, and the drain asked `httpd_ws_get_fd_info(handle, fd)` —
  which reports "some websocket lives at this number", never "the session this frame was
  gathered for", because IDF's session lookup is purely fd-keyed with no generation. The
  residency window is wide: `httpd_server` handles ONE control message per `select()`
  pass while close, accept and handshake proceed in that same pass, so a peer can hang up
  and an unrelated client be accepted onto the recycled descriptor while the first peer's
  frames still sit in the queue. Those frames were then written to the new peer — a
  **cross-session data leak** on a peer-named server, where a directed FWD reply or a
  subscription push produced for one authenticated session was delivered to a different
  one — and their failures were charged to the newcomer's strike counter, closing a
  session that had failed nothing. The TX path now carries a `session_ref_t` (the peer
  slot plus a generation stamped at every claim) and re-validates it at each site through
  `live_fd`; a stale reference FAILS and the frame is dropped rather than misdelivered.

  **Scope: the enqueue → drain gap, not every gap.** The generation protects a reference
  from the moment it is MINTED. It does not protect the window BEFORE that: on the directed
  path a caller resolves a peer's endpoint via `peer_link` and sends later in the same
  forward hop, and `peer_endpoint_t::send` mints the reference from the slot's CURRENT
  generation — so whichever session occupies the slot at send time satisfies the downstream
  check. Under preemption that window is unbounded, and the misdelivery is still
  constructible. It is narrower than the window this closes and it predates this change,
  but it is the same consequence, so it is tracked as **#1013** rather than described as
  fixed. Closing it needs a per-resolution identity, which is an API-shape decision the
  shared per-slot endpoint object cannot absorb silently.

  Both halves of the shipped reference are needed: slots are recycled IN PLACE, so the departed peer's slot — and
  therefore the server's session ctx POINTER — is exactly what the next peer is handed,
  which is why the pointer comparison the teardown detach path uses does not close this
  hole on the live path. No public API change.

- **`esp_ws_client_link_t` now HONORS its `recv_stack` argument** (#900). The constructor
  accepted the knob and discarded it, spawning a plain `std::thread` — which on ESP-IDF
  takes the global `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT`, since a pthread's stack can
  only be set by arming `esp_pthread_set_cfg` for the next `pthread_create`. That thread
  runs in-call graph delivery through the `on_write` seam (the deep path the sibling
  *server* link sizes at 12288 bytes), so a caller who sized the stack for its delivery
  path silently got the default and a stack-overflow reboot. The save/set/spawn/restore
  recipe `twai_link_t` already carried is now one shared helper (`tr::esp::spawn_thread`,
  component-private) used by both links; the restore is what keeps the size from leaking
  onto later threads spawned by the same caller. `recv_stack = 0` still means "platform
  default" and arms nothing, so callers passing nothing are unchanged.

- **An over-sized inbound message is now dropped WHOLE** (#901). The receive loop reset
  its accumulator on overflow but kept reading the same message, so the remainder
  re-accumulated from offset zero and was handed to the router as a bogus standalone
  frame — the peer's stream desynchronised and nothing reported it. This never required
  fragmentation: `esp_transport_read` caps each read at the space offered and IDF
  re-reports the frame's `fin` on every one of them, so one over-sized *unfragmented*
  frame split across reads and its tail was delivered. The loop now tracks the current
  frame's unread bytes (from `esp_transport_ws_get_read_payload_len`), so "the message
  ended" is distinguishable from "the buffer ran out", and consumes the remainder of a
  dropped message without delivering it. Two adjacent gaps close with it: a stray
  CONTINUATION arriving with no message open is dropped (the rule `httpd_ws_link_t`
  already applies), and an **exact-fit** message — `off == rx_bytes` with `fin` — is
  delivered instead of being swept into the overflow branch.

### Changed

- **BREAKING (chip targets): the portable `ws` transport is no longer built, and no `ws`
  entry is registered in the built-in transport catalog.** ESP-IDF WebSocket must never
  use POSIX sockets (#947 ruling). Core's `transport_ws_server` / `transport_ws_client`
  are the HOST implementation; on lwIP they are not merely unreachable but *unusable* —
  their scatter-gather egress asks `sendmsg` for `MSG_NOSIGNAL`, `lwip_sendmsg` rejects
  any flag outside `MSG_DONTWAIT|MSG_MORE` with `EOPNOTSUPP`, and `write_all_iov` reads
  that as peer-gone, so every data frame is silently discarded while the opening
  handshake and PING/PONG still answer (#948). They are therefore **absent**, not fixed.
  The sanctioned plane is the IDF-native links this component already ships:
  `httpd_ws_link_t` (on `esp_http_server`, needs `CONFIG_HTTPD_WS_SUPPORT=y`) and
  `esp_ws_client_link_t` (on `esp_transport_ws`), both bound by the application through
  `transport_vertex_t::provide_link`.

  Selection is by **which TU compiles**, not a feature macro — the rule
  `socketcan_link.cpp` vs. `socketcan_link_stub.cpp` already follows. The `linux` (POSIX
  host) target is unchanged and keeps the portable pair; it has glibc's `sendmsg` and no
  `esp_http_server`.

  **What breaks:** a `:children[]` SPEC carrying `kind=ws` with no staged link now
  answers `SCHEMA_NOT_FOUND` on a chip target instead of constructing a portable
  socket server that could not deliver anyway. Nodes already staging an IDF-native link
  with `provide_link` are unaffected. `CONFIG_LIBTRACER_TRANSPORT_WS` keeps its meaning
  ("build the WebSocket plane") — only *which* plane it builds is now target-decided.

  Measured on `examples/full_node`, esp32c6, `-Os`, ESP-IDF v6.0-dev: `nm` on the linked
  ELF went from **46** `transport_ws_server`/`transport_ws_client` symbols to **0**, and
  flash fell 394,146 → 381,098 B (**−13,048 B**; `libtracer.a` −12,769 B, image `.bin`
  −13,040 B). Flash here is tracked, never gated. `tools/check_esp_ws_plane.py` is the
  standing gate.

- **`examples/full_node` now sets `CONFIG_HTTPD_WS_SUPPORT=y`,** so `httpd_ws_link.cpp`
  — the sanctioned chip WebSocket *server* — is compiled by CI. It previously had no
  compile coverage anywhere: the config it is gated on defaults to `n`, and the portable
  server it replaces was silently filling in.

## [0.8.0] — 2026-08-06

### Changed

- No integration-layer changes — the component ships `core` 0.8.0, whose new
  `for_each_vertex` and subscription-observer APIs are thereby available to
  component consumers.

## [0.7.1] — 2026-08-04

### Added

- **`esp_ws_client_link_t::set_handshake_headers(std::string)`: append extra HTTP header
  lines to the opening-handshake request** (each `Name: value\r\n`-terminated,
  `esp_transport_ws` emits them verbatim; empty leaves the handshake byte-for-byte the
  historical one). The client counterpart to `httpd_ws_link_t::set_admission_cb`: a
  board-to-board dial carries no browser session cookie, so a token header here is how a
  dialing node authenticates itself to a peer whose graph WS gates admission. Applied on
  the next dial — set it once at wiring time before the link first connects.
- **`httpd_ws_link_t::set_admission_cb(admission_fn_t, void*)`: an optional predicate
  consulted at the top of every opening handshake, before the peer-cap check and any slot
  allocation.** Returning `false` refuses the peer cleanly (httpd closes the socket — the
  same path as the `max_peers` cap); a null hook (the default) admits every peer, so the
  historical open-graph behavior is unchanged. The seam a host uses to authenticate the
  graph WS the way it gates the rest of its HTTP surface — inspect the handshake request's
  headers (a session cookie, a shared token) and refuse an unauthenticated peer before it
  can read or write a single vertex. Read on the httpd task with no lock; set it once at
  wiring time before the link serves.

### Fixed

- **`httpd_ws_link_t` and `esp_ws_client_link_t`: `TCP_NODELAY` is now set on every
  WebSocket socket, removing the Nagle + delayed-ACK latency floor.** A libtracer WS frame
  is a small, self-contained TLV whose reply the peer is already awaiting — the request-reply
  shape Nagle stalls, adding tens of ms per round-trip with nothing to coalesce that the WS
  framing does not already batch. The server sets it per accepted socket at admission
  (`bound_socket`, alongside the #835 `SO_SNDTIMEO`), so REST sockets — which this link never
  upgrades — are untouched; the client sets it on the freshly connected `esp_transport` fd in
  `connect_once`. Both are best-effort: a peer whose socket refuses the option keeps the
  pre-patch latency for that one link rather than failing. Measured on an ESP32-C6 bench: WS
  request-reply RTT floor drops from ~52 ms toward the airtime bound (HIL-verified per the
  merge gate).

- **`httpd_ws_link_t`: WebSocket sends are now bounded by a derived per-socket send timeout,
  and the brokenness detector now aims at the peer that is actually broken (#835).** Two
  defects on one seam. (1) `esp_http_server` sets `SO_SNDTIMEO` on every accepted socket from
  `config.send_wait_timeout` (5 s by default) and every WS send runs on the single httpd
  task — the task that owns accept/recv for every socket the server has — so one peer with a
  full TCP window blocked that task for up to the whole timeout per frame, serialized under
  delivery fan-out, until the task watchdog fired and the HTTP plane stopped answering
  (observed on an ESP32-C6 under a delivery burst with a throttled browser tab subscribed).
  Every UPGRADED socket now gets a short `SO_SNDTIMEO` of its own at admission, DERIVED as
  the task-watchdog period divided by the peer cap — so one full fan-out round with every
  peer stalled still fits inside one watchdog window. The bound is a DERIVATION, not a
  measurement: the host tests prove the timeout is installed, the short-write handling and
  the strike attribution, while the starvation repro itself is silicon-only — that the task
  watchdog no longer trips is HIL-verified per the #835 merge gate. REST sockets are
  untouched: the server's `send_wait_timeout` still governs HTTP responses, so there is no
  config ripple.
  (2) The three-strikes teardown counted ENQUEUE drops and charged each to the fd of the
  frame that failed to enqueue. The control queue is shared, so when the stalled peer's slow
  sends jammed it the drops — and the session closes — landed on whichever HEALTHY peers
  sent next, while the stalled peer never accrued a strike. Failed SENDS now feed the
  per-session streak (they name their destination by construction) and refused enqueues feed
  a link-level counter and strike nobody. #481's protected behaviour is unchanged: one failed
  send still only drops that frame, and any successful send resets the streak, so the shipped
  "large reply times out between small frames" shape still never closes the session.
  (3) A close the link ASKS FOR is not a close that happens. `httpd_sess_trigger_close` is
  `httpd_queue_work(httpd_sess_close, …)` — the same loopback control socket, drained by the
  same single httpd task that is already serialized behind the stalled fd's queued sends — so
  it is strictly FIFO behind the very backlog it exists to clear, at a full send bound per
  entry; and on the default non-blocking path `httpd_queue_work` is a bare `sendto`, so an
  enqueue past that socket's small UDP mbox is dropped inside lwIP while still returning
  success. An `ESP_OK` from it is therefore no evidence a close was queued at all. The first
  on-silicon run of this fix measured the consequence exactly: the desync was detected and
  logged, and then the same fd kept failing every 2 s for a further two minutes with the peer
  never dropped — the starvation's period had shrunk, not the starvation. So the decision and
  the close are now separated. At the strike cap or on one short write the session is marked
  DEAD in the link's own state, under the existing peer-mutex discipline, at the instant of
  the verdict: `queue_send` then refuses new frames to that fd, `tx_work` skips frames already
  queued to it (the backlog drains at queue speed instead of a send bound apiece, which is
  what frees the httpd task), and the socket is `shutdown`, which needs nothing from the
  control queue — it takes effect on the calling line, makes every later write fail at once,
  and raises the readable-at-EOF event that gets the session reaped through httpd's own select
  arm. It never frees the descriptor, so `esp_http_server` keeps sole ownership of the fd's
  lifetime; the dead mark is cleared where the slot is recycled, on the httpd task, so it
  cannot outlive its session onto a reused descriptor number. `trigger_close` is still called
  as a best-effort second path, but nothing depends on its result. HIL-pending: that the
  stalled peer is now actually dropped and new WS connections stop timing out is a property
  of a real lwIP socket under a real fan-out and is re-verified on the bench, not claimed here.

- **`httpd_ws_link_t`: peers on IPv6 sockets were all named `0.0.0.0` (#835).** With
  `CONFIG_LWIP_IPV6` on — the default on this target — `esp_http_server` binds its listener
  `PF_INET6`, so every accepted WebSocket socket is `AF_INET6` and `getpeername` fills a
  `sockaddr_in6`. The peer namer decoded it as a `sockaddr_in`: the port read correctly (same
  offset) but the address read `sin6_flowinfo`, which is always zero. Every peer name on the
  bus tag, the census and — the case that mattered — the strike log was therefore
  `0.0.0.0:<port>`, so the one line that had to identify a stalled peer identified nothing.
  The family is now read from what `getpeername` actually returned, with v4-mapped addresses
  unwrapped so a dual-stack node keeps naming its IPv4 peers exactly as the census always has.

### Added

- **`httpd_ws_link_t` short-write guard.** Both constructors gained a trailing
  `send_timeout_ms` parameter (default `0` = derive, clamped to the server's
  `send_wait_timeout`) for hosts whose watchdog regime differs from the derivation's inputs,
  plus the accessors `send_timeout_ms()` and `enqueue_drops()`. Every admitted session also
  installs a send override (`httpd_sess_set_send_override`): lwIP returns the PARTIAL count
  when a bounded write expires mid-buffer, and `esp_http_server` treats any non-negative
  return as a delivered frame — which would report a half-written WebSocket frame as success
  and desynchronise the peer's framing for the life of the socket. A short write is now an
  error AND condemns that session immediately, bypassing the streak (a different fault class:
  the stream is broken, not a frame missing) — see (3) above for what "immediately" had to be
  made to mean, since the queued close it originally used could be delayed or silently lost.
  Existing call sites are source-compatible.

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
- **`httpd_ws_link_t` (adopted mode): frames can no longer be dispatched into a
  destroyed link, and the destructor joins the handler before freeing anything (#816).**
  Unregistering the WS URI does not stop inbound frames: `esp_http_server` copies the
  route (`handler` + `user_ctx`) into each session as it answers the handshake
  (`httpd_uri.c`) and dispatches from there (`httpd_parse.c`), clearing it only when that
  session is deleted — which a link cannot force and, for a peer that upgraded without
  ever sending a frame, cannot even observe. The registered `user_ctx` is therefore now a
  small handler GATE rather than the link: after teardown the gate holds no link, so a
  late frame is refused (httpd closes that socket) and a late session callback is inert,
  and while a handler frame IS inside the link the destructor blocks until it leaves.
  The gate is deliberately leaked in adopted mode, since the set of sessions still
  routed at it is unknowable. Also fixed on the same seam: the session being serviced by
  an in-flight handler is now neutralised rather than detached (`httpd_sess_set_ctx`
  edits the request, not the socket table, for that one fd, and `httpd_req_cleanup` runs
  its callback after the destructor has returned); a completed reassembly is moved out of
  its slot before delivery, so an in-call teardown cannot be followed by a write into the
  freed slot; and the queued detach now identifies its sessions by stored context, not by
  socket descriptor alone, so an item draining late cannot run a co-tenant's `free_ctx`
  or force-close its session after the descriptor was recycled on the shared server.

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
