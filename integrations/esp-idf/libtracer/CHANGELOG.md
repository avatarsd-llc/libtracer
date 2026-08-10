# Changelog — libtracer ESP-IDF component

Notable behavior changes of the `integrations/esp-idf/libtracer` component (the
ESP-IDF packaging of the `core/` reference implementation plus the chip-native
platform links: `httpd_ws_link_t`, `twai_link_t`), per
[CLAUDE.md](../../../CLAUDE.md). The core C++ API itself is tracked in
[core/CHANGELOG.md](../../../core/CHANGELOG.md).

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed

- **A post-101 refusal now spends the reconnect backoff instead of spinning (#1128).**
  `esp_ws_client_link_t`'s backoff was gated on `connect_once()` returning false, but an
  admission refusal can only be expressed *after* `101 Switching Protocols` is on the wire —
  so `esp_transport_connect` succeeds and the refusal looked like a connection that later
  dropped, not a failed dial. The loop re-dialed with **no delay at all**, re-allocating a
  transport pair every cycle; on a single-core target that starved the idle task, tripped the
  task watchdog, and the board panic-rebooted — resuming the dial on the next boot, so one
  misconfiguration could sustain a reboot loop. A connection that goes down having never had
  an inbound message delivered is now treated as a failed attempt and pays the same
  `kReconnectBackoffMs` a failed dial does. **Inbound is the test** — a send into a socket the
  peer has already decided to close still succeeds locally, so only a message *arriving*
  proves the peer admitted the session. A link that has exchanged traffic still retries a
  genuine drop immediately: the backoff is conditional, not blanket.

- **The derived WS send bound now divides by the strike cap as well as the peer cap
  (#840).** `derive_send_timeout_ms` computed `CONFIG_ESP_TASK_WDT_TIMEOUT_S / peers`, which
  makes ONE fan-out round of fully stalled peers fill the entire watchdog window. But a peer
  is not condemned on its first failed send — the brokenness detector wants
  `kMaxConsecutiveTxDrops` consecutive failures and each one costs a full bound — so the
  worst-case stall before the link is rid of a stalled peer set was `strikes` whole windows,
  not one. Downstream HIL measured 4959 ms of a 5000 ms budget consumed (99%), i.e. whether
  the board panics decided by noise. The bound is now
  `watchdog / (peers * kMaxConsecutiveTxDrops)` — 416 ms at the default cap of 4, was
  1250 ms. **Behaviour change:** a peer that cannot absorb a frame within the tighter bound
  accrues a strike sooner, so a genuinely stalled session is torn down faster; a healthy one
  is unaffected, since any completing send resets the streak.

### Changed — BREAKING

- **`httpd_ws_link_t` names accepted sessions `p<slot>`, not `<ip>:<port>`** (#994,
  ADR-0073 §2, #426). The old name could be ENUMERATED and never ADDRESSED:
  `graph::valid_segment` rejects both `.` and `:`, so a peer listed by the link's
  synthesized `:children[]` could not be spelled back in a `dst` path, and every FWD
  descent into one answered `INVALID_PATH (0x0021)`. That broke the enumerable⇒addressable
  invariant RFC-0020 §6 names as the precondition for its NAME-hop rejection — on every
  ESP-IDF node, while core's own bus servers had shipped `p<slot>` since #426.
  **Anything holding an `<ip>:<port>` peer name must re-resolve it**: `peer_link()`,
  `enumerate_peers()`, the departure seam and `peer_stats_t::name` all carry the slot name
  now. The physical address is not lost — it moved to the new
  `peer_stats_t::endpoint_str`, and the strike log prints both.

- **The component now requires ESP-IDF `>=5.5.5`** (`idf_component.yml`, was `>=5.3`) — a
  TX-path correctness floor, not a packaging preference (#949). Below it `httpd_queue_work`
  is a bare non-blocking `sendto` on `esp_http_server`'s loopback control socket, so an
  enqueue past `CONFIG_LWIP_UDP_RECVMBOX_SIZE` is discarded inside lwIP while the call
  still returns `ESP_OK`: the frame is lost with nothing observable anywhere, and the work
  item that would have released its TX pool slot never runs. From 5.5.5 the mbox slot is
  reserved through a counting semaphore before the `sendto` (`httpd_main.c`), so a full
  control queue is an `ESP_FAIL` the caller sees. **Consumers on 5.3.x, 5.4.x and
  5.5.0–5.5.4 are dropped**; a project on those versions must either upgrade or pin the
  previous component release.

- **`httpd_ws_link_t::tx_strands()` is REMOVED**, together with the machinery it reported
  on: the four-state TX slot lifetime, its age stamp, and the exhausted-claim sweep (#949).
  Above the new floor a slot cannot be pinned by an enqueue that silently never runs, so
  there is nothing to reclaim and nothing to count. `tx_slots_busy()` and
  `tx_slot_capacity()` stay. A consumer polling `tx_strands()` should drop the call;
  `enqueue_drops()` is now the whole TX-loss surface.

- **An exhausted TX slot pool drops the frame and counts it, instead of falling back to a
  heap work item** (#949, and #953's part B). The pool is the link's outstanding-send bound
  — `tx_slot_capacity()` sends in flight at once — and exceeding it increments
  `enqueue_drops()` and logs, on the same path a refused enqueue takes. The old arm posted
  an unbounded number of heap-allocated work items into a control mbox drained one message
  per server pass, which is a bounded and observable condition traded for an unbounded and
  invisible one; the exhaustion policy the record already rules is drop-and-count
  (ADR-0039 §4, ADR-0042 §2). **Consumer-visible consequence**: a broadcast to more peers
  than `tx_slot_capacity()` now delivers a pool's worth per pass and counts the rest as
  drops, where it previously heap-queued them. On a device the httpd task drains
  concurrently, so how much of a wide fan-out lands is a scheduling question — but it is no
  longer bounded by the heap, and every loss is on a counter. A host that fans out wider
  than the pool should read `enqueue_drops()`.

### Added

- **`httpd_ws_link_t::peer_named()`** — the `bus_link_t` mode-authority override (#889).
  The same constructed flag `bus()` keys off, published so the base can REFUSE its
  peer-named wiring calls (`set_peer_receiver`, `set_peer_rope_receiver`,
  `set_peer_down_notifier`) on a FLAT link: `bus_link_t` is a public base, so those setters
  are reachable by an upcast past the null `bus()`. This link's delivery and departure
  paths already read the flag directly, so nothing else about it moves.

- **`tr::net::link_counters_t`** (`libtracer_esp/link_stats.hpp`) plus
  **`esp_ws_client_link_t::stats()`** and **`httpd_ws_link_t::enumerate_peer_stats()`** —
  per-link passive traffic counters (#942): rx/tx messages and payload bytes, rx/tx drops,
  and `esp_timer` stamps for the last delivered message and for when the connection came
  up. Counts MESSAGES, not WebSocket fragments, so a client's `tx_frames` is directly
  comparable with the server's `rx_frames` across a link. The client's snapshot also
  carries `reconnects` (completed handshakes — the only externally-observable signal that
  a transient drop happened, since `drop()` deliberately suppresses `notify_down`) and
  `connect_ms` (the last handshake's duration; an upper bound on ~2 round-trips sampled
  only at re-dial, never an RTT). Server-side counters are per SESSION and are handed out
  with the slot's claim generation, so a consumer differencing successive snapshots can
  tell "same connection, N more frames" from "a different peer landed on that slot".

  `stats().c.rx_drops` is not a second tally: it is read from the existing
  `dropped_rx()` below, so the client link keeps exactly one inbound-drop truth.

  Scope is deliberately narrow — `transport_t` grows no `counters()` virtual and
  `fwd_router_t` no per-child accounting, because the only consumers are hosts that
  enumerate the two concrete link types they constructed themselves. A polymorphic hook
  would put a counter bump on core's hot `deliver_remote` path and buy nothing.

- **`httpd_ws_link_t::tx_slots_busy()` / `tx_slot_capacity()`** — TX work slot
  observability (#944): the pool's live occupancy and its size, so a caller can see the
  pool being starved rather than infer it from allocation pressure. (This entry originally
  also introduced `tx_strands()`; that accessor was removed again before release — see the
  BREAKING section above — and neither it nor the sweep it reported on ever shipped in a
  tagged component.)

- **`esp_ws_client_link_t::dropped_rx()`** — inbound messages the receive path refused,
  spelled the way core's transports already spell it (`transport_can::dropped_rx()`).
  It counts the two refusals that were previously logs-only (#953): a message that does
  not fit `rx_bytes`, and a stray WebSocket CONTINUATION arriving with no message open.
  A frame lost to a connection drop is not counted — that is the link going down, not
  the receive path refusing a message.

### Fixed

- **`httpd_ws_link_t::set_admission_cb`'s predicate now runs where the opening GET actually
  arrives — the server's WebSocket PRE-handshake callback — and its refusal stops the
  upgrade (#958).** `esp_http_server` answers the WebSocket handshake internally and returns
  from `httpd_uri()` *before* calling `uri->handler`, so a predicate consulted from the URI
  handler was never handed the opening GET at all: the handler is entered only for data
  frames, on an already-upgraded socket, and the peer is claimed there. The link now
  registers a `ws_pre_handshake_cb` thunk at **both** construction sites (own-server and
  adopted-server) which is called with the parsed opening GET — method, URI and every header
  readable through the ordinary `httpd_req_get_*` accessors — before the 101 is written and
  before the session latches the WS route. Returning `false` refuses the peer: no 101, no
  session, no slot, no per-socket policy, no entry in `enumerate_peers`. Unset (still the
  default) admits every peer, so an unconfigured link is unchanged. In adopted mode the
  predicate is scoped to this link's WS URI — registration is per-URI, so the rest of the
  caller's HTTP surface is untouched.

  **Consumer-visible consequences.** (1) The component's `Kconfig` now
  `select`s `HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT` when `CONFIG_HTTPD_WS_SUPPORT` is on — that
  is where the `httpd_uri_t` member lives, and it is present at this component's ESP-IDF
  floor (`>=5.5.5`), so there is no fallback tier and no second code path. (2) The
  predicate's `httpd_req_t*` is now a *handshake* request rather than a data-frame one,
  which is what makes header inspection meaningful; a hook that read nothing off it is
  unaffected. (3) A predicate is consulted once per CONNECTION, at the handshake, instead of
  once per opening-GET dispatch that never happened.

- **Documentation erratum, shipped in the same change (#958).** The header's threading
  section claimed "the WS URI handler is invoked once at the opening handshake (HTTP GET)
  and again for each subsequent data frame", and `set_admission_cb` / the `max_peers`
  parameter documented a refusal "at the top of every opening handshake". Neither described
  the code: the handler is never invoked for the opening GET, and both the admission
  predicate and the `max_peers` cap were reached only on a peer's FIRST DATA FRAME. The
  admission half is now true because the code moved; the `max_peers` text is corrected to
  say where that check really happens (first frame — this change does not move it). The
  0.7.1 `set_admission_cb` entry below is left as the historical record and is superseded
  by this one.

- **The dead opening-GET handler path is deleted (#958).** `on_handshake` — a second peer
  claim site, a second `max_peers` check and a second `bound_socket` call — was unreachable
  for a real WebSocket peer at the component's ESP-IDF floor, and reachable only by a plain
  non-upgrade GET on the WS URI, which it answered by claiming a peer slot for a socket that
  was not a WebSocket. Such a request now falls through to the frame path, where
  `httpd_ws_recv_frame` answers `ESP_ERR_INVALID_STATE` on a socket with no handshake done
  and the server closes it. `on_data_frame` is now the ONE claim site.

- **A FLAT `httpd_ws_link_t` no longer reports the WHOLE LINK down when one of its several
  sessions closes (#889).** `notify_departed` forked on the constructed mode correctly —
  peer-named evicts just the departed peer — but the FLAT arm fired
  `transport_t::notify_down` on **every** session close, and that hook is
  `fwd_router_t::link_down`: it drops every subscriber edge and label binding registered
  under the link's one NAME. Both defaults that make this reachable are the defaults
  (`peer_named = false`, `max_peers = 0` unbounded), so a two-tab deployment that never
  asked for the bus facet had one tab's hangup evict the other tab's subscriptions. The
  flat arm now waits for the LAST open session to depart (`any_open_session()`); a mid-life
  close notifies nothing and the survivors keep routing. This is the same rule
  `slot_server_t::teardown_slot` gained in core, in the one link that cannot inherit it —
  the issue named only the core servers, so this instance would otherwise have shipped
  unfixed.

- **`httpd_ws_link_t` closes a session whose frame was announced on the wire and then cut
  off, instead of dropping it as a recoverable frame** (#951). `esp_http_server` writes one
  WebSocket frame as TWO calls to the session's send function — the header, then the
  payload — and reports either failure as the same `ESP_FAIL`, so the link could not tell a
  frame that never started from one truncated after its header had gone out. It treated
  both as the #481 whole-frame loss: drop the frame, keep the socket. For the second that
  is unsound. The peer holds a header promising bytes that never arrive and consumes every
  later frame as this one's missing payload, so delivery stops on a socket both ends still
  consider open, and the very next small frame that succeeds resets the failure streak, so
  no teardown ever follows. The send override now brackets each frame and judges the FRAME:
  a failed write with bytes of that frame already on the wire is a stream desynchronisation
  and closes the session at once (the short-write response), while a failure with nothing
  written stays the droppable case it was. Behaviour change for consumers: such a peer now
  gets a close — and therefore an `onclose` and a reconnect — where it previously went
  silent. The desync log line names its cause (`short write` / `frame truncated`) and the
  bytes on each side, and no longer shares wording with the benign drop.

- **Both WS links now detect a peer that vanishes without a FIN, and the client link
  reports its departure** (#957). Two halves of one gap.
  (1) **Detection, both links.** Nothing in either link noticed a peer that stopped
  existing — a Wi-Fi drop, a power cut, a killed browser tab, a NAT rebind. On the server
  the ways a session can end are peer CLOSE/FIN, a handler failure, the TX failure streak,
  the short-write condemn and `httpd_stop`, with no timer among them, and
  `lru_purge_enable = false` (the admission contract that forbids evicting a live peer)
  removes the one reclaim `esp_http_server` has for an idle socket — so such a peer held
  its slot and one unit of `max_peers` for the life of the process. On the client, a poll
  turn that times out is normal and there was no idle deadline, so `ok()` answered `true`
  for a dead peer indefinitely on an idle link. Both links now apply `SO_KEEPALIVE` plus
  the idle/interval/count tunables to the socket — `httpd_ws_link_t::bound_socket` at
  admission, `esp_ws_client_link_t` on every dial — using the values ESP-IDF documents as
  the defaults for `esp_http_server`'s own keepalive, so both ends of a board-to-board
  connection declare a peer dead at the same age. The server link applies them itself
  rather than relying on an adopted server's `keep_alive_enable`, for the same reason it
  applies its own send bound. Best-effort, and only behind the enable: a stack that
  refuses `SO_KEEPALIVE` keeps the previous behaviour for that one peer rather than half a
  policy. **Behaviour change:** a vanished peer's session is now failed by the stack and
  reclaimed (server) or dropped and re-dialed (client), on the order of tens of seconds
  instead of never.
  (2) **Reporting, client link.** `esp_ws_client_link_t` called `notify_down()` from
  nowhere: peer CLOSE, a read error, a poll error and a failed or short write were all
  silent, so the one departure seam a point-to-point link has
  (`transport_t::set_down_notifier`, which `fwd_router_t::add_child` wires to the eviction
  of that child's subscriber edges and label bindings) never fired, and the header's claim
  that this type substitutes for core's `transport_ws_client` "with no other change" was
  false on exactly that seam. The recv loop's not-connected arm — which is downstream of
  every one of those paths — now fires it once per connection that was up, with no
  transport lock held, before it re-dials; a LOCAL teardown still reports nothing, on
  core's `stop_` rule. **Behaviour change:** a reconnect is now a NEW session to the
  routing plane. The old premise (keep the edges so deliveries resume transparently) holds
  only for a blip the far side also survives, and the link cannot tell one from a peer
  that rebooted and forgot every subscription and label it ever issued — which left this
  node producing into a session that no longer existed and resolving compact labels
  against a stranger's label space. Re-establishing after a flap costs edge churn; that is
  the accepted trade.

- **`esp_ws_client_link_t` got a bounded-blocking discipline: a derived write bound, an
  interruptible backoff, and a teardown barrier** (#952). Three defects on one seam —
  `write_m_` and `stop_` were treated as if the operations under them were prompt.
  (1) `send()` held `write_m_` across a 4000 ms write timeout that IDF's `_ws_write`
  spends up to three times over (poll, header, payload), so a peer with a closed TCP
  window parked the *calling* task for up to 12 s against a typical 5 s task watchdog —
  a panic, not a dropped frame — and blocked the recv thread, which serializes its reads
  on the same mutex, from even seeing the CLOSE. Both blocking bounds are now derived
  from `CONFIG_ESP_TASK_WDT_TIMEOUT_S` the way the server sibling's send bound already
  is (#835): a dial gets half a watchdog window and one whole `send()` a quarter of one,
  and `SO_SNDTIMEO` is set on the socket next to the existing `TCP_NODELAY` so the
  blocking write leg is bounded too. **Behaviour change:** a peer that cannot accept a
  small frame within that budget now has its connection torn down and re-dialed instead
  of parking the sender; a dial that does not complete within half a watchdog window is
  retried rather than waited out. (2) The reconnect backoff was a plain `sleep_for`, so
  the destructor's join waited out the full 1.5 s of exactly the unreachable peer a
  re-dial exists for; it is now a condition-variable wait the destructor signals.
  (3) The destructor destroyed the transport handles with `write_m_` held nowhere, on
  the premise that the joined recv thread was the only handle user — which this type's
  own contract contradicts (`send()` may be called from any task), so a sender queued
  behind a stalled write woke up owning a destroyed handle. Teardown now disarms the
  link, destroys the handles under `write_m_`, and waits out every sender that had
  already *announced itself on the in-flight tally* — raised at the top of `send()`,
  before it queues on that mutex. That tally is the covered boundary, and it is the most
  a barrier inside the object can cover: a caller that has not reached it when the
  destructor reads it for the last time is not waited out, whether it is a `send()` that
  *starts* after the destructor returns or one that entered `send()` and is still short
  of the raise. Both stay the embedder's lifetime problem. A sender also reads the
  transport handle only behind the `connected_` acquire gate that pairs with the recv
  thread's release store: the re-dial rebuilds the handles holding no lock, so `write_m_`
  alone does not order that rewrite against a sender's read. No API change; a teardown
  that lands mid-dial still costs one dial bound, since `esp_transport_connect` takes no
  cancellation.

- **`httpd_ws_link_t::send` (the broadcast) no longer allocates, so a fan-out during a
  heap trough drops a frame instead of aborting the node** (#961). The fan-out opened by
  building a `std::vector` of destinations under `peers_m_` — the one container shape the
  rest of that translation unit is written to avoid, because under `-fno-exceptions` its
  throwing allocator turns a failed growth into `abort()` inside libstdc++'s `bad_alloc`
  stub. It sat AHEAD of every `new (std::nothrow)` fallback the TX path has, so the
  drop-on-OOM backpressure the header advertises was void on the one path every
  subscription push takes, and the failure was silent: no counter moves, no OOM log, just
  an unexplained reboot. The destinations are now snapshotted into a fixed on-stack chunk
  (`kFanoutChunk`, sized at the link's own `kDefaultPeerCap` socket budget) with the scan
  resuming where it stopped, so a broadcast to any number of peers takes no heap arm at
  all — there is nothing left to fail, hence no new drop counter and no sizing policy for
  the unbounded-`max_peers` case. A broadcast at or under the chunk still takes exactly
  one `peers_m_` hold; past it, one more uncontended acquisition per chunk. No API change;
  the header's steady-state allocation contract now covers fan-out explicitly.

- **`httpd_ws_link_t` fires the peer-departure eviction notifier with the handler gate
  RELEASED, not held across it** (#960). `reclaim_slot` scoped `peers_m_` to the field
  clears and fired the routing plane's eviction hook outside it — but its only caller held
  the handler gate's mutex for the whole call, so `fwd_router_t::link_down` (a walk of
  every subscribed vertex under the graph's own locks, bounded by nothing this link owns)
  ran under the one mutex every dispatch into the link takes and a destructor blocks on.
  `bus_link_t::notify_peer_down` documents the opposite precondition outright ("with none
  of its internal locks held"), and both core reference servers
  (`transport_ws_server::teardown_slot`, `transport_tcp_server::teardown_slot`) honour it;
  this link was the exception, and it established an undocumented `gate → router → graph`
  ordering edge that nothing recorded. `reclaim_slot` now returns the departed peer's name
  and `on_session_closed` fires the notification after the lock scope ends; the order it
  no longer has is written down on `gate_t`. What does **not** change is the lifetime
  guarantee holding the mutex supplied: the notification registers on the gate's existing
  `depth`/`cv` barrier — the same one a URI-handler frame uses — so `close_gate` still
  cannot return while one is in flight, and the notifier still cannot outlive the link.
  Scope, stated because both are easy to over-read into this: (1) the mutex-order edge is
  gone, so a thread holding a graph lock can always take the gate — but destroying a link
  while holding a lock its in-flight work needs still deadlocks on the barrier, exactly as
  it already did through the URI-handler join and as it does in `transport_ws_server`,
  whose destructor joins its poll thread for the same reason; (2) the eviction still runs
  synchronously on the httpd task from inside `free_ctx`, so a departing peer still costs
  that task the walk — moving it off is a separate design question (#1071). No API change.

- **`twai_link_t`'s TX backpressure window is spent PER FRAME again, and teardown no
  longer queues behind it** (#962). `write_raw` took `write_m_` and only then parked on
  the free-slot semaphore that *is* the FULL policy's backpressure point, so the window
  was per queue rather than per frame: on a controller whose tx-done never fires (a
  bus-off or stalled node), K concurrent writers spent `K * tx_timeout_ms` in series
  rather than one window each, and the destructor — which takes the same lock — inherited
  the whole series, so a link removal during a bus fault blocked the destroying task for
  the same multiple. `tx_dropped()` kept moving throughout, so the symptom read as "we're
  dropping, as designed" right up to the watchdog reboot. The semaphore is now taken
  OUTSIDE the lock, which leaves the lock covering only the submission (the node handle
  and the pool's serialized acquire); a writer parked across teardown re-checks the node
  under the lock and hands its token back rather than submitting to a deleted controller,
  and the destructor releases parked writers and waits for them to leave before deleting
  the semaphore they are waiting on. No API change.

- **`twai_link_config_t::tx_timeout_ms` is clamped to the task-watchdog period**
  (`CONFIG_ESP_TASK_WDT_TIMEOUT_S`, #962). It was taken verbatim, so a config could park
  a writing task longer than a task may go unfed and reboot the board instead of dropping
  the frame — the same class of footgun `derive_send_timeout_ms` exists for on the WS
  server link (#835). The bound is one FRAME's wait; a caller writing a burst on one task
  still sums its own frames' waits. Configs at or under the watchdog period — including
  the 20 ms default — are unaffected.

- **`httpd_ws_link_t`'s TX slot pool no longer dies four dropped datagrams into a boot**
  (#944). `httpd_queue_work` on the default non-blocking path is a bare `sendto` to a
  loopback UDP control socket, so an enqueue past the receiver's mbox is discarded inside
  lwIP while `sendto` — and therefore `httpd_queue_work` — still returns `ESP_OK`. The
  close path was routed around that fact with `shutdown`; the TX path still trusted the
  return value. A binned work item never ran, and the work item was the *only* thing that
  released the pool slot it had claimed: four of them and the pool was gone for the rest
  of the boot, after which every outbound frame took two global-heap allocations on a hot
  publish path. There was no counter for it — `enqueue_drops()` covers the *refused*
  enqueue, which already recycled correctly.

  A TX slot now carries a four-state lifetime (free / claimed / armed / running) instead
  of a single `busy` flag, and an exhausted claim sweeps the pool before giving up:
  a slot armed for longer than `kTxPoolSlots * send_timeout_ms()` — every other slot ahead
  of it, each stalled to this link's full per-socket send bound — is presumed lost and
  recycled. **Safety does not rest on that window.** A pooled work item lives inside its
  slot and so cannot carry an identity a re-claim would not overwrite; it is therefore a
  bare token meaning "send whatever slot *i* has armed", and a token that arrives after a
  reclaim either finds nothing armed or sends the later frame that is — correctly, since a
  payload and the destination it was gathered for are armed together. A window chosen too
  short costs a dropped frame (counted, and droppable by contract), never a torn one.
  Reclaiming happens only on the exhausted-claim path: no timer, no task, and nothing at
  all in the steady state. The **heap-fallback** work item (taken when the pool is
  exhausted) is *not* reclaimable by this or any timeout — the token holds a raw pointer
  to the shell, so freeing it on a guess is a use-after-free rather than a leak; what the
  fix removes is the driver that made those leaks unbounded, since a pool that cannot die
  is no longer permanently bypassed.

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

- **BREAKING: `esp_ws_client_link_t`'s handshake headers moved into the constructor and
  `set_handshake_headers()` is gone** (#959). The setter could not work: the constructor
  spawns the recv thread, the recv thread dials at once, and the setter necessarily ran
  after both. So the *only* ordering the API permitted was the racy one — a `std::string`
  assigned by the wiring task while `connect_once()` read it with `.empty()`/`.c_str()`,
  where a reallocating assignment can hand `esp_transport_ws_set_config` a `cfg.headers`
  that is already freed. Nothing in the API decided which side of that race won, so
  whether the FIRST dial carried the token was **undefined** — and a dial without one is
  refused by a peer whose graph WS gates admission, costing a 1.5 s reconnect backoff
  before the next dial. That is what made first-dial liveness nondeterministic by
  construction. Which side wins in practice is a scheduler property, not an API one: it
  was never measured on chip, and on the host fake a rebuilt pre-fix ordering had the write
  win **20 of 20** measured runs. That reconstruction assigns inside the constructor
  immediately after the thread spawn — a *narrower* write-to-read window than the setter it
  stands in for — so the rate is the arrangement most favourable to the write winning, on
  one host and one scheduler. It says the write won the runs that were made; it does not say
  the opposite interleaving cannot occur. A tokenless first dial is therefore what the old
  surface *permitted*; it is not a behaviour recorded here as observed.

  The headers are now the **fourth** constructor parameter (after `ws_path`, completing
  the "what the handshake requests" group) and the member is `const`: written before the
  thread that reads it exists, re-read on every re-dial, no lock and no snapshot. Empty
  still leaves `cfg.headers` NULL, so a dial without a token is byte-for-byte the
  historical request. Placing it fourth rather than last is deliberate — appending it
  would have forced any caller wanting a token to restate `rx_bytes`/`tx_bytes`/
  `recv_stack` positionally, pinning today's defaults into call sites that never meant to
  choose them.

  **What breaks:** `link.set_handshake_headers(tok)` after construction no longer compiles;
  move `tok` into the construction site. A call that passed buffer sizes positionally
  (`{host, port, "/ws", 4096, 4096, 12288}`) also no longer compiles — `std::string` is not
  constructible from `std::size_t`, so a size in the fourth slot is a hard error.
  **One exception, and it is silent:** a literal `0` in that slot is a null-pointer
  constant, binds to `std::string(const char*)` and yields `std::string(nullptr)`, which is
  UB at run time — `L(host, 8080, "/ws", 0, 0, 0)` compiles clean under
  `g++ -std=c++20 -Wall -Wextra`. Grep call sites for a literal `0` in the old `rx_bytes`
  position before trusting the compiler here.
  `libtracer_esp/esp_ws_client_link.hpp` is the only file involved.

  This is deliberately NOT a `start()` split. Deferring the recv thread would make the
  whole "set X before the first dial" family expressible, but its other motivation (#900's
  discarded `recv_stack`) was already answered by honouring the knob at spawn time, and
  the shape of a deferral on this link — which dials, re-dials and delivers on one thread —
  is the open contract question in #1102, not something to settle as a side effect here.

- **`esp_ws_client_link_t` now LOGS an outbound frame that exceeds `tx_bytes`**, with both
  sizes, alongside the `tx_drops` bump that landed with #942 (#959). `transport_t::send`
  returns void, so the router cannot be told the frame died; `tx_drops` says one did, but
  `send()`'s peer-down and short-write arms bump that same counter, so a bump does not even
  say which drop happened, let alone which knob was too small. A per-frame ceiling nobody
  can see is how a blob-carrying value or a composed reply vanishes with clean logs on both
  ends. The log is the new channel and the only one that names a size; the counter is
  unchanged. The 2048-byte defaults are unchanged — they are documented as modest on
  purpose and raising them costs RAM on every dialed peer; what was wrong was the silence
  around exceeding them, not the number.

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
