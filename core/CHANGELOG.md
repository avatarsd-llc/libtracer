# Changelog — core (libtracer reference implementation)

All notable changes to the **public API** of the `core/` reference implementation
(the headers under `include/libtracer/`) are recorded here, per
[CONTRIBUTING](../.github/CONTRIBUTING.md) / [CLAUDE.md](../CLAUDE.md). This tracks the
*implementation's* C++ API — which is implementation-defined per
[ADR-0013](../docs/adr/0013-v1-scope-boundaries.md) and versioned independently of the
**protocol-v1** wire format it implements. (That format is still `DRAFT` — see
[`docs/spec/v1.md`](../docs/spec/v1.md) — and becomes immutable on release, not before; the
two surfaces differ in which instrument a change needs, not in whether one is permitted.)

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The
reference implementation is pre-1.0; the first cut release is `[0.3.0]`, below.

## [Unreleased]

### Added

- **`transport_can` exposes the sibling drop counters, and its RX state is bounded and
  aged (#912).** The CAN ingress buffers grew on the receive thread with nothing expiring
  and nothing counting a drop, unlike every sibling transport. Three parts:
  - **The injected-bound seam is reachable at last.** `can_reassembly_t` was
    *default-constructed* inside the transport, so `max_groups` was `0` and its
    evict-oldest could never fire however a deployment was configured. It is now
    constructed from new `transport_can_config_t` fields — `reasm_mr` (a
    `std::pmr::memory_resource*`, default the process heap), `max_groups`, `max_pending`
    and `rx_ttl` — and the pending-slice queue draws from the same injected resource, so
    the RX thread no longer reaches the global heap. The `can` factory parses
    `max_groups`, `max_pending` and `rx_ttl_ms` from the connection's config SETTINGS
    TLV (`0` = unbounded, host-bounded per RFC-0006, matching `max_peers`), and
    `can_transport_factory` takes an optional `std::pmr::memory_resource*` — a resource
    is a pointer, not a wire value, so it is injected at registration time.
  - **`pending_` is bounded and aged.** A data frame with no matching binding was parked
    forever: the only drain is `learn_advertise`'s covered-range re-drive, so a peer that
    never advertises (or a bus that dropped the advertise — the exact failure CAN
    produces) grew it without limit. It is now capped by `max_pending` (evict-oldest and
    count) and swept of entries older than `rx_ttl`.
  - **An incomplete reassembly group no longer pins its slices forever.** `erase` is
    reached only after `is_complete`, so any lost data slice left a group buffered for the
    transport's life. `can_reassembly_t` gains `set_now(std::uint64_t)` and
    `sweep_stale(std::uint64_t max_age)` — the buffer stays clock-free, the caller stamps
    it — and `transport_can` sweeps on every inbound advertise.

  New public accessors on `transport_can`: `dropped_rx()` (inbound slices reclaimed by the
  cap or the age-out), `dropped_tx()` (a send that never reached the bus: allocation
  failure, an empty split, or an unencodable manifest), `dropped_groups()` (reassembly
  groups reclaimed before delivery — the accessor the buffer's own counter never had) and
  `pending_slices()`. `rx_ttl` left at `0` tracks the configured `peer_ttl` rather than
  introducing a second window: a peer already considered gone cannot complete RX state.
  Unlike the opt-in count caps the age-out is **always live**, so it is the bound that
  holds under the shipped default config. That "always" is now literal: a `peer_ttl` of
  `0` derives an `rx_ttl` of `0`, which the peer enumeration and the reassembly sweep both
  read as *instantly expired* — the pending age-out used to read the same `0` as *sweep
  disabled* and return early, so one degenerate config value silently re-opened the
  unbounded growth this entry closes. Zero now means the same thing to all three, and a
  negative window (previously cast to `std::uint64_t` and compared against ~1.8e19, so it
  reclaimed nothing at all) is normalized to zero at construction. No wire change.

### Fixed

- **`stream_endpoint_t::write_all` no longer truncates a frame when a signal interrupts the
  write (#903).** The two sibling full-write helpers disagreed on interrupted syscalls:
  `write_all_iov` retried EINTR, while `write_all` treated any `n <= 0` — EINTR included — as
  peer-gone and abandoned the rest of the buffer. EINTR is reachable (the stream sockets are
  blocking; `MSG_NOSIGNAL` suppresses SIGPIPE, not EINTR), and every `write_all` call site
  carries a COMPLETE pre-encoded frame on a persistent framed stream (tcp, and the ws control
  + data sends), so an interrupt after `off > 0` left a partial frame on a still-live
  connection and desynced the peer's framing permanently — every later byte parsing under the
  wrong length. Both helpers now share ONE interrupted-write policy (`retry_interrupted_write`
  in `posix_endpoint.cpp`): EINTR resumes the write where it stopped; every other `n <= 0`
  (including the `n == 0` that previously spun `write_all_iov`) is peer-gone and drops the
  rest silently. Behavior only — no signature change.

- **`wire::encode` no longer truncates the length field for a body over 65535 bytes (#924).**
  `encode` called `emit_header` directly, which writes the length at the width `opt.ll` names —
  so a `tlv_t` built programmatically with a default `opt` (`ll = false`) over an oversize
  payload or child list serialized a length silently truncated to `size & 0xFFFF`, a frame a
  peer mis-frames. `encode` now goes through `wire::emit_tlv`, the single home of the
  length-width policy, which widens to the u32 `LL` form when the body exceeds `0xFFFF`. No
  signature change and no wire-grammar change (`LL` was always permitted). Callers see one
  behaviour difference: `decode(encode(t))` on such a tree now returns a tree with `opt.ll`
  set, where before it returned a decode error or a mis-framed tree. Bodies at or under
  `0xFFFF` are byte-identical, and `opt.ll` is never cleared. A body over `0xFFFFFFFF` still
  truncates modulo 2^32 — the wire grammar has no length form wider than u32, so that residual
  is a grammar limit, not an `encode` bug, and it is unchanged here.

- **`tr::detail::try_reserve` / `try_push_back` / `try_assign` no longer probe-then-commit
  (#923, #850).** They performed a nothrow `operator new`/`delete` probe and then ran the
  THROWING `std::vector::reserve` behind it, on the argument that "the just-freed probe block
  satisfies it". That inference is single-threaded, and this library's own concurrency model
  is not: a segment self-routes its reclaim on whatever thread drops the last ref, and
  transport receive threads run concurrent with writers. A racer taking the block inside the
  window made `reserve` throw `bad_alloc` out of a `noexcept` function — `std::terminate`,
  measured (`exit=134`). A single-core FreeRTOS context switch between the `operator delete`
  and the `reserve` opens the same window without SMP.

  The growth now runs through a new `tr::detail::try_grow(bytes, grow)`: on any profile that
  has exceptions the container's OWN allocation is the one whose failure is reported (caught
  and returned as `false`), so there is no second allocation and no window. Under
  `-fno-exceptions` a `bad_alloc` has no representation at all — libstdc++ turns it into a
  bare `abort()` inside `reserve` that no wrapper can intercept — so the probe is retained
  there unchanged; a path on that profile that must genuinely survive exhaustion migrates to
  the ADR-0065 failable seam (`block_source_t` / `block_array_t`), as
  `transport_t::send(iov)` and `ws::try_encode_client_frame` already did.

  Behaviour is unchanged for callers (`false` still means "nothing changed"), the
  `probe_fail_hook` OOM-injection seam still gates every one of these paths, and the hosted
  profile gets **one fewer allocator round trip per growth** (measured on
  `bench_failable_census`: `try_encode_advertise_guarded` heap blocks/call 2.01 → 1.00;
  `probe` mode `try_reserve` median 18.90 → 7.52 ns/growth against an unchanged `try_alloc`
  control arm at 13.42 → 10.81). `try_push_back` now `static_assert`s that `T` is
  nothrow-move-constructible.

- **`LIBTRACER_BACKEND_SET_POOL_ONLY` `destroy_dispatch` honours the segment's `backend_tag`
  (#922).** The single-member (MCU) fold of the ADR-0047 §2 dispatch reinterpreted every
  segment's backend as `pool_t*` with no tag check — unlike the `transfer` beside it. A
  `synchronized_pool_t` is a `mem_backend_t` holding a `pool_t` **member** and re-points its
  segments to itself with an `UNKNOWN` tag precisely so reclaim takes the locked virtual
  `destroy`; reinterpreting it read `slab_`/`stride_` from the wrong offsets and skipped the
  critical section (measured on the new POOL_ONLY target: 1 lock acquisition instead of 2, and
  the inner pool's slot count overwritten with a wild index). The same cast fired for every
  `tr::view::borrow()`ed segment and for any user backend. The tag is a fast path again, never
  a correctness dependency.

### Changed

- **BREAKING — `subject_resolver_t` gains a DENY channel; an unresolvable caller is no
  longer trusted (#905).** The type in `graph.hpp` changes from
  `std::function<std::optional<subject_token_t>(std::string_view)>` to
  `std::function<std::expected<subject_token_t, wire::err_t>(std::string_view)>`. The
  error arm means **deny**: the operation fails `status_t::PERMISSION_DENIED`
  (`tr::access::denied`, 0x0050 on the wire).

  Before this, the resolver had exactly one non-token answer — `nullopt` — and the graph
  read it as *fully trusted*, skipping every ACE check. The natural reading of that value
  ("I cannot name this caller") therefore granted **every** right on the vertex, including
  `WRITE_ACL` and `CREATE`, at every gate: READ, WRITE, SUBSCRIBE, CREATE, WRITE_ACL,
  READ_ACL, and remote-edge fan-in delivery. A resolver bug, a revoked peer, or an unknown
  remote identity bypassed all ACLs on protected vertices. There was no way for a resolver
  to say *deny*.

  **The trusted channel moved out of the resolver.** `acl_allows` now settles the EMPTY
  caller context — the local-API convention — as trusted **before** invoking the resolver,
  so the resolver is never called with an empty caller and a remote op (which always
  carries its inbound link NAME) cannot reach the trusted arm.

  *Migration:* a resolver of the form
  `if (caller.empty()) return std::nullopt; return token;` becomes
  `return token;` — the empty case is now handled by the graph. Any *other* former
  `nullopt` return was silently granting everything and should become
  `return std::unexpected(wire::err_t::ACCESS_DENIED);`. The signature change is
  deliberately recompile-visible: keeping `std::optional` and inverting `nullopt`'s meaning
  would have flipped the semantics of every existing resolver in silence.

  No change to the enforcement-disabled path: the `!subject_resolver_` early-out remains
  the only check when no resolver is installed.

## [0.8.0] — 2026-08-06

### Added

- **`graph_t::set_subscription_observer(sub_observer_t)` + `sub_event_t` — the EXTERNAL
  `:subscribers[]` mutation stream ([ADR-0076](../docs/adr/0076-external-subscription-mutations-are-observable-at-the-admission-door.md)).**
  The edge-triggered counterpart of `read_subscribers`: an app-installed callback fired when
  a **peer** subscribes to or unsubscribes from one of this node's vertices, so a producer can
  start a source on demand or project the fan-out graph without polling. The event carries
  `{kind (ADDED/REMOVED), producer, target, link, slot}`; `producer` and `target` are
  **canonical keys** (`wire::key_view_t`, borrowed for the call), `target` decoded from the
  `SUBSCRIBER`'s `PATH` child and EMPTY when the record carries none (the bare
  remote-subscriber case). Fired from the one ADR-0049 admission door and from the RFC-0009
  §D.1 `:subscribers[N]` clear, so an append, a `[N]` replace (`REMOVED` then `ADDED`) and a
  clear all report through one site.

  **"External" is exactly a non-empty ADR-0018 caller context** — the inbound link NAME the
  FWD resolver drives the op under. Both `subscribe()` sugars, `unsubscribe()`, and a
  `:subscribers[]` field-write from host code are SILENT by design, as is a resolver op that
  carries no inbound link. So is **`evict_link_edges`**: link teardown drops *k* edges in a
  batch with no caller context, so an app maintaining a live subscription inventory must treat
  its own link-down signal as the removal for every edge of that link. The observer runs
  **synchronously on the resolver's thread**, outside every graph lock but inside the operation
  it reports — it must be cheap, non-blocking, and must not re-enter the graph; deferral is the
  app's job. Set once at wiring time (the `set_remote_delivery_sink` contract). Null by default
  ⇒ one null check on the subscribe path and no other change.

- **`graph_t::for_each_vertex(Fn)` — the graph's enumeration surface.** Visits every
  **registered** vertex once as `fn(wire::key_view_t key, vertex_handle_t vh)`, in ascending
  canonical-key **byte** order. Placeholders (the unregistered intermediates a deep
  `register_vertex` creates) are skipped, exactly as `find` refuses them. Sorted-only, with no
  unsorted twin: a consumer paginating this surface needs the order to be the same across calls
  while the graph is unchanged, and every visit renders `key` anyway (ADR-0057 stores one
  segment per node), so ordering is a comparison pass on top of work an unsorted form would
  already have done. It is the same order the RFC-0008 sweep sets use, and it earns its keep the
  same way: a parent's key is a byte-prefix of every descendant's, so a parent precedes its
  subtree and that subtree is a contiguous run. Note it is byte order over the KEY, so siblings
  sort by name LENGTH first (`/zone` before `/sensor` before `/actuator`) — alphabetical
  *display* order is the consumer's own sort. **Control-plane only:** one owned key per
  registered vertex plus the snapshot vector, then a sort. The `{key, vertex}` snapshot is taken
  under one shared `map_mutex_` hold and `fn` runs OUTSIDE it (the `evict_link_edges` two-phase
  discipline), so `fn` may re-enter the graph — in exchange for a snapshot: every vertex
  registered before the hold is visited, a later arrival may not be.

## [0.7.1] — 2026-08-04

### Added

- **`graph_t::has_subscribers(vertex_handle_t)` — the demand-driven producer's DELIVERY gate**
  ([#852](https://github.com/avatarsd-llc/libtracer/issues/852)). True iff a delivery here
  would reach a subscriber: this vertex's own **or** a subtree subscriber on a strict ancestor
  (RFC-0005). It joins the two gates `deliver_vertex` applies — `fan_out`'s own self-gate on
  the own count, then the `listeners_above` gate over `bubble_up` — so a producer that skips
  a `deliver_vertex` on `false` skips exactly what that call would have found no receiver
  for. (A decomposing branch write is not one `deliver_vertex`.) Two
  limits are documented on the declaration and are load-bearing: **`read` pollers and `await`
  waiters are not counted** (subscription is a field-write to `:subscribers[]`, not one of
  ADR-0006's three verbs), so a producer that skips the value STORE rather than just the
  delivery starves them; and a **skipped publish is not recovered by ADR-0049's durability
  latch** — that argument belongs to the fan-out skip, which stores the LKV *before* loading
  the count, and the latch is opt-in besides (RFC-0022 §3.A bit 5; a default `policy = {}`
  latches nothing). The `seq_cst` own half buys only that a landed subscribe is ordered before
  the producer's next read, so at most one round is skipped; the ancestor half is relaxed and
  not even that.

- **`graph_t::own_subs(vertex_handle_t)` — the owner-side subscriber-slot count.** Exposes
  the `own_subs` counter (#635) through the graph as a sizing/observability read: how many
  slots a delivery here would feed. Inline, `noexcept`, relaxed load. **It must not be used
  to gate a publish** — it omits subtree subscribers entirely (they are counted by
  `listeners_above`, not here), and it is the relaxed load `vertex_t::own_subs_ordered`
  documents as unfit for a skip decision. Use `has_subscribers` for that.

- **`tr::net::iov_table_t` (`libtracer/iov_table.hpp`) and the nothrow transport egress
  ([#848](https://github.com/avatarsd-llc/libtracer/issues/848)).** The scatter-gather entry
  table the three socket transports that build an `::iovec` table — WS, TCP, UDP — gather
  their egress through: the caller's stack array while the entries fit (`kMaxInlineIov`, 16),
  else ONE block from a `tr::mem::block_source_t`
  (ADR-0065) whose exhaustion answer is `nullptr`. It replaces five copies of a *throwing*
  `std::vector` resize/reserve in `transport_ws.cpp`, `transport_tcp.cpp` and
  `transport_udp.cpp`. (Not every POSIX transport: `quic` and `webtransport` override
  `send(iov)` and gather into one owned msquic send buffer, so they build no `::iovec` table
  and never reach this store.) **Behaviour change:** those growths sized an entry count the SENDING
  peer chooses (a rope's link count × its region count, uncapped by `ws_assembler_t::on_data`),
  so exhaustion was a peer-reachable `abort()` under the `-fno-exceptions` MCU profile;
  a `send` that cannot obtain the table now DROPS the frame — it never truncates one.

- **`transport_t::send(iov)`'s default gather draws from the failable seam.** The base
  class's scatter-gather fallback — what `transport_can` and any embedder's transport that
  does not override `send(iov)` lands on, reachable from `route_fwd_forward` — assembled its
  contiguous temporary with `tr::detail::try_reserve`, i.e. a nothrow probe in front of a
  THROWING `std::vector::reserve` behind a `noexcept`. It now uses a
  `tr::mem::block_array_t<std::byte>` and honours the `probe_fail_hook` injection seam
  explicitly (as `iov_table_t::acquire` does). No signature change; exhaustion still DROPS
  the frame, but it can no longer `std::terminate` on the way there.

- **RFC 6455 control-frame surface in `tr::net::ws`:** `kMaxControlPayload` (125),
  `is_control_opcode`, `kMaxServerControlFrame` / `kMaxClientControlFrame`,
  `encode_server_control` / `encode_client_control` (stack buffers, returning the byte count),
  and `decode_status_t` / `decode_result_t` / `decode_one` / `decode_frame_checked`.
  **Behaviour change:** an oversized or non-final control frame now FAILS the connection
  (§5.5/§7.1.7) instead of being echoed, so the PONG reply is built entirely on the stack and
  a heap blip can no longer cost a link that an unanswered PING entitles the peer to drop.

- **`tr::net::can::encode_advertise_header(std::array&, const advertise_t&, std::string_view)`.**
  The one field-encoding locus for a CAN advertise, filling a caller-owned `std::array` and
  returning `false` past `kAdvertiseMaxPathLen`. The path is a separate parameter because the
  header is a function of its LENGTH, not of who owns its bytes: `transport_can::emit_advertise`
  now slices the 18-byte stack header and then `cfg_.path` IN PLACE into 8-byte CAN windows, so
  the advertise on **every** send costs zero allocations — no contiguous buffer, and no
  `adv.path = cfg_.path` copy either (an advertise this node emits always announces its own
  path, so `advertise_t::path` is left empty on egress). The contiguous `encode_advertise` twin
  is retained for callers that want the whole frame in one buffer (`bench_conn_ram`,
  `transport_can_peers_test`), and now delegates its header to `encode_advertise_header`.
  **Behaviour change in that twin:** a path longer than `kAdvertiseMaxPathLen` now yields an
  empty vector — nothing to emit — where it previously cast the length to `std::uint16_t`
  unchecked, encoding a frame every decoder rejects (`decode_advertise` bounds `path_len` at
  `kAdvertiseMaxPathLen`, `core/include/libtracer/can.hpp:361`) — or, past 65535, one whose
  length field silently truncates. Its bytes are unchanged for every path within the bound.
  **Scope:** this removes the advertise's allocations only. A CAN `send` still allocates for
  the owning payload block (`view::over_bytes`, which soft-fails and DROPS) and for
  `view_can_frames_t::split`'s window vector (still a THROWING `push_back` — the remaining
  `-fno-exceptions` abort risk on this path, not addressed here).

- **`ws::try_encode_client_frame(mem::block_array_t<std::byte>&, …)`.** The nothrow twin of
  `encode_client_frame` — a client frame MUST be masked (§5.1), so its wire bytes are not the
  caller's and it is the one WS egress path that still needs a buffer. Returns the frame's
  byte count, `0` on refusal. It takes a `tr::mem::block_array_t` rather than a
  `std::vector` + `tr::detail::try_reserve`: that helper is `noexcept` but only its PROBE is
  nothrow — it frees the probe block and then runs the THROWING `std::vector::reserve`, so
  refusing that second allocation crosses a `noexcept` boundary into `std::terminate` (and a
  bare `abort()` under `-fno-exceptions`). Drawing from the failable seam leaves exactly one
  refusable allocation and no unguardable second step.

- **`tr::net::detail::ws_peer_published_hook` (`libtracer/transport_ws.hpp`) — a TEST-ONLY
  seam in a public header.** A null function pointer `transport_ws_server::service_peer`
  calls at the exact instant a peer's `101 Switching Protocols` response is on the wire and
  its slot is published open — immediately after (not inside) the `write_m_` critical
  section that makes those two transitions one step, so a sender racing the hook can still
  take that lock. A test installs it to HOLD that instant open and send into it;
  production leaves it null and pays one predictable null-check per accepted peer, on the
  cold handshake path. Same shape and same rules as `tr::detail::probe_fail_hook` (recorded
  under #477, below): install it before the peer that should trip it connects, and clear it
  before the test returns.

- **`tr::graph::kEdgePinSlots` and the edge-pin domain (`libtracer/edge_pin.hpp`,
  [#635](https://github.com/avatarsd-llc/libtracer/issues/635),
  [ADR-0075](../docs/adr/0075-a-vertexs-edges-are-published-and-read-under-an-edge-pin.md)).**
  A vertex now PUBLISHES its edges as an immutable array that `fan_out` copies out under a
  bounded per-participant pin instead of the shared stripe mutex — ×18.58 on the same-stripe
  fan-1 write at twenty-four threads, and the negative scaling past four threads is gone.
  `kEdgePinSlots` sizes the announcement registry (CMake `-DLIBTRACER_EDGE_PIN_SLOTS=`);
  correctness never depends on it, only scaling does. `sizeof(vertex_t)` drops 112 -> 96 B
  on x86-64 and 80 -> 72 B on rv32.

- **`vertex_t::kNoSlot`.** The answer `vertex_t::add_edge` gives when the edge could not be
  admitted because the injected resource is exhausted. **Behaviour change:** `add_edge`
  could not previously fail, and a caller that ignores `kNoSlot` would count a listener that
  does not exist. `graph_t::subscribe` reports this as `status_t::BACKPRESSURE`, the
  injected-resource status; it used to be unreachable there.

- **`wire::type_t::PATH_REF` (`0x14`) and the bound-path element codec
  ([RFC-0024](../docs/spec/rfcs/0024-bound-paths-node-scoped-vertex-ref-source-routing.md) §4).**
  New header `core/include/libtracer/path_ref.hpp`: `path_ref_element_t`
  (`{u32 index, u32 generation}`), the constants `kPathRefElementBytes` (8),
  `kMaxPathRefElements` (255) and `kMaxPathRefBodyBytes` (2040), plus
  `path_ref_element_count`, `path_ref_element_at`, `path_ref_store_element` and the structural
  predicate `path_ref_body_valid`. `path_ref_element_at` / `path_ref_store_element` are
  precondition-only and debug-assert their bound: a caller that cannot prove the index — anything
  walking a foreign frame's elements — gates on `path_ref_element_count` first. `tlv_emit.hpp` gains `emit_path_ref(out, elements)`, which
  refuses (returns `false`, emitting nothing) past the 255-element bound rather than truncating.
  Purely additive — no existing declaration changes spelling or semantics.
- **`grammar::parse_header` now enforces the `PATH_REF` body shape.** The one per-type rule in
  the wire grammar: for `0x14`, `opt.PL` and `opt.LL` MUST be 0, `length` MUST be a multiple of
  8, and the element count MUST be ≤ 255. A violation is `err_t::FRAME_INVALID`, the same
  answer a set reserved bit gets. Because the rule lives in the shared header grammar it fires
  at every nesting depth and through every decoder (the owning `tlv_t` tree, the terminus arena,
  the lazy `tlv_view_t`). **Behaviour change, and the only one:** bytes that previously decoded
  as an opaque unknown-type TLV with type `0x14` now reject. No frame any libtracer version has
  ever emitted is affected — `0x14` was unassigned.

- **Bound-path routing and minting at the terminus
  ([RFC-0024](../docs/spec/rfcs/0024-bound-paths-node-scoped-vertex-ref-source-routing.md)
  §5-§7).** `graph_t` gains three methods for the node-scoped vertex index (§6.4):
  `vertex_slot_count()`, `vertex_slot(vertex_handle_t)` — the **mint** side, which declines
  for a vertex whose generation has saturated — and `deref_vertex_slot(index, generation)`,
  which is the whole of the §5.1 check (bounds, saturation refusal, generation compare) and
  authorizes nothing. `vertex_slot` returns `graph::vertex_slot_t` (`{u32 index, u32
  generation}`), both fields read under **one** lock hold: read as two calls they can straddle
  a `retire`, and the pair would then name the successor tenant's vertex while the caller
  believes it bound the one its operation reached. `vertex_slot` is a control-plane call and
  is priced as one: it scans, because a per-vertex index field costs 4 bytes on rv32 where
  `sizeof(vertex_t)` sits at its ceiling with zero headroom. Purely additive.
- **`graph::kGenerationSaturated` and `graph::saturating_next_generation`.** The retirement
  generation now **saturates rather than wraps** (§4.4 rule 3, normative in §9.3). **Behaviour
  change:** a vertex retired 2^32 times stops advancing its generation and becomes permanently
  unbindable, instead of aliasing a generation a stale bound-path element still carries. No
  reachable deployment is near the ceiling; the rule closes the failure class by construction.
- **`graph::bound_generation_matches(slot_gen, element_gen)`.** The deref's generation rule as
  one total function, and the reason it is one: it refuses `kGenerationSaturated` **outright**
  rather than comparing it. Below the ceiling "generations only move forward" is the whole
  guard; at the ceiling the counter stops, so a saturated element would keep matching its slot
  through every subsequent retire and revive, with staleness detection permanently dead for
  that slot. Enforcing this only at the mint would leave the guard to the side that does not
  choose the number — an element is peer-supplied. Exercised at the ceiling by `static_assert`,
  which 2^32 retirements cannot be.
- **`wire::emit_path_ref_into(span, elements)` and `wire::path_ref_wire_bytes(n)`.** The
  allocation-free `PATH_REF` writer, and the size function that lets a caller stack-size its
  buffer. `emit_path_ref(vector, …)` now delegates to it, so the two spellings cannot drift.
  The reply path uses the span form: a `PATH_REF`'s size is a pure function of its element
  count, and a growing container on a path that has already succeeded would abort the node
  under `-fno-exceptions` on a fragmented heap instead of degrading to the plain reply.
- **`graph::kFwdOpcodeMask` (`0x3F`) and `graph::kFwdOpFlagMintRequest` (`0x80`)** in
  `op_resolve.hpp`. The `FWD` `op` byte's opcode is now `op & 0x3F` and bits 7-6 are flags.
  **Behaviour change:** `net::peek_fwd_op` and the terminus resolver mask before switching, so
  an `op` byte carrying an unrecognised flag routes as its plain opcode rather than rejecting
  as an unknown one. Frames libtracer emitted before this change all have zero flag bits and
  are unaffected.
- **A `FWD` `dst` may be a `PATH_REF`.** The terminus dereferences the single remaining element
  and applies the operation at that vertex, through the **same** `graph_t` call the canonical
  spelling makes — so the per-operation ACL check happens at the dereferenced vertex by
  construction, and the two spellings' outcomes agree without a second policy. Any validation
  failure is a **drop**: no forward, no apply, no repair (§5.3). (A residual longer than one
  element is a forwarder's, and is routed as such — see the forwarder-hop entry below.)
- **A mint answers in the reply.** An operation whose `op` byte sets bit 7 gets a one-element
  `PATH_REF` appended as the reply's **last** child, on **success only**. A denied or failed
  operation mints nothing, which is the anti-enumeration property of §6.1.
- **`graph::path_binding_t` and `path_t::binding()` / `bind()` / `clear_binding()`.** The
  §7.4 origin-side slot: `path_t` stays a value type and carries the bound form beside its
  canonical bytes, which are never discarded — they are the key the binding was minted from
  and the fallback a failed one drops back to. `bind()` refuses past the element cap rather
  than truncating. Purely additive.

- **The bound-path FORWARDER hop, and the origin-side bind — RFC-0024 §3.4/§5/§7, car 3.**
  A `PATH_REF` `dst` whose residual is longer than one element now routes: the hop consumes
  element 0 (bounds, generation, then `acl_allows` at the dereferenced vertex for the
  operation's own right), egresses over the link that vertex names, and forwards the remainder
  with `src` grown canonically — so the return route of a bound request is **byte-identical**
  to the canonical spelling's and every hop on the way back may be a peer that does not speak
  the bound form. No mount descent runs on the hop: `resolve_mount_*` is not entered. Any
  validation failure is a **drop** (§5.3) — never a fall-through to the local terminus. New
  public API on `net::fwd_router_t`:
  - `connection_ref(link_name)` — this node's own element for a child's connection vertex,
    which is element 0 of any route leaving through it and the one element no peer can supply;
  - `bound_egress(element, caller, right)` — the §5.1 check plus the element→link join, shared
    by the forwarder's hop and the origin's own;
  - `adopt_binding(path, link_name, reply)` — the §7.4 origin side: takes the accumulated
    `PATH_REF` off a mint reply, stacks this node's own element under it, and records the whole
    stack on the `path_t` (the first production caller of `path_t::bind`);
  - `bound_dispatch(path, right)` → `{link, dst}` — what the next operation over a binding is
    sent as: the origin consumes element 0 exactly as a forwarder does, and puts the residual
    on the wire.
- **`graph_t::vertex_slot_at(index)` and `graph_t::allows(vertex, caller, right)`.** Two
  primitives the hop needs and nothing else could give it: the O(1) index→generation read a
  forwarder's mint uses (`vertex_slot` scans, which is right for the terminus and wrong here),
  and the ACL predicate published for the one caller that reaches a vertex without performing
  a data op on it. Both purely additive. `vertex_slot_at` refuses a **placeholder** — a
  retired-but-not-yet-revived slot, or a never-registered structural intermediate — exactly as
  `deref_vertex_slot` does: `retire` bumps the generation and clears `registered_`, so an
  element minted in that window already carries the number the SUCCESSOR tenancy validates
  under, and the validate-on-use stamp has to hold on the issuing side as well as the honouring
  one.
- **A hop that forwards a mint reply now either contributes its element or STRIPS the answer**
  (RFC-0024 §7.1 **erratum 1**, landed with this car). **Behaviour change** for a node that
  cannot mint for the link a reply arrived on: it removes the trailing `PATH_REF` instead of
  relaying it. A list that skips a hop is not a shorter route but a wrong one — the origin
  would consume its own element and the non-contributing hop would find one element left,
  believe itself the terminus, and dereference another host's element against its own vertex
  map. Stripping closes that mis-route class; the origin simply stays canonical.
  **Every** cannot-contribute case strips, the erratum's full-list arm included: a reply whose
  trailing `PATH_REF` already holds 255 elements cannot be extended, so it is removed rather
  than relayed. `peek_reply_mint` reports that as `reply_mint_t::can_contribute == false`
  rather than as "no answer found", because "not found" is the one verdict that would take the
  forbidden relay branch.
- **`net::peek_fwd_dst_any` + `net::fwd_dst_kind_t`, `net::peek_fwd_dst_ref`,
  `net::read_path_ref_element`, `net::peek_reply_mint` + `net::reply_mint_t`,
  `net::no_mint_t` and `stack_writer::header_bare`** in `fwd_frame_view.hpp`; `fwd_pre_t`
  gains `dst_ref`, `fwd_rebuild_t` gains the mint accumulation fields. **`kFwdMaxIov` stays
  9**: the mint's two regions and the mount run's three are mutually exclusive by `is_reply`,
  so the counted maximum is 9 for a request and 8 for a reply. It was briefly raised to 11 by
  adding the two sets together — a bound no frame can reach — and that alone moved code
  placement enough to cost `bench_forward_rope` a disjoint +13% at fan 2 in branch mispredicts.
  The constant is counted from `gather`'s emit sequence, and it is measured.
  `peek_fwd_dst_any` is the routing gate both forms now share: it classifies a frame's `dst`
  as canonical `PATH`, bound `PATH_REF` or neither in ONE read of the three leading headers.
  `peek_fwd_dst` and `peek_fwd_dst_ref` keep their spellings as its two arms, for callers with
  only one of the questions to ask. **The router asks once** — running the two gates in
  sequence put a whole second header walk on every bound frame and measured a bound terminus
  slower than the canonical terminus it is meant to beat.
- **The mint accumulation is an out-of-line call (`net::rebuild_reply_mint`).**
  `rebuild_fwd_forward` carries `flatten`, which pulled `peek_reply_mint`'s header loop into
  its body on a branch a REQUEST hop never takes; the front end that bought cost the rope
  forward hop a disjoint +13% at fan 2. `noinline` on the helper puts one not-taken branch on
  the request path instead.
- **`rebuild_fwd_forward` takes a mint SUPPLIER, not a mint element** — a callable defaulting
  to `no_mint_t`, invoked at most once and only on a forwarded REPLY that carries an
  extendable mint answer. Eager evaluation meant reading the frame's op byte a second time on
  **every** forwarded frame, request hops included, and on a multi-link rope that read is a
  cursor walk rather than a load. Callers that pass no supplier are unaffected.
- **`rebuild_fwd_forward` accepts a `PATH_REF` `src` on a REPLY.** A reply to a bound request
  echoes the request's `dst` in `src`, so refusing it dropped every such reply at the first
  forwarder. On a REQUEST the same shape is still refused: this hop grows `src` by its inbound
  mount, and a NAME prepended into a fixed-stride record array is not a longer route but a
  corrupt one.

Still not implemented, and named rather than implied: the §5.3 NACK carrying the failing hop
index, whose spelling RFC-0024 §9.2 leaves open. A drop is already the conformant answer
without it.

### Fixed

- **The WS server publishes an accepted peer as OPEN under the write lock** (#848).
  `transport_ws_server::service_peer` wrote the `101 Switching Protocols` response, released
  `write_m_`, and only then stored `open = true`. In between, the peer has already read the
  response — and so believes the connection is up — while every `send` still skips the slot
  as not-open, so a frame sent in that instant is silently lost rather than queued or
  refused. The store now sits inside the same `write_m_` critical section as the response
  write: a sender can only reach the fd through that lock, so anyone who could observe the
  response also observes the slot as open. (`open` deliberately does NOT move ahead of the
  response write instead — a concurrent `send` would then be free to put a BINARY frame on
  the wire in front of the handshake reply.) The window predates this branch: it spans the
  gap between the `write_m_` scope closing and the `open` store, so a test cannot reliably
  be inside it by racing for it. `ws_transport_test`'s guard therefore PARKS the server's
  poll thread at that exact point (`detail::ws_peer_published_hook`) and makes its whole
  `send` in the parked window. No signature or API change.

- **The owning receiver seam holds no library reference *during* the callback** (#845). The
  UDP, TCP and QUIC/WebTransport receive paths built the delivered frame as
  `view_t::over(std::move(seg)).subview(0, len)`. `subview` is `const` and **copies** the
  segment handle, so the discarded whole-segment temporary stayed alive for the rest of the
  full-expression — which is the `deliver(...)` call, hence the whole receiver callback. The
  frame is now built by aggregate init (`view_t{std::move(seg), 0, len}`): one fewer
  refcount round-trip per received frame, and a receiver that inspects
  `view.owner.use_count()` inside (or straight after) its callback now sees the `1` that the
  ownership transfer in [ADR-0042](../docs/adr/0042-refcounted-receiver-seam-view-delivery.md)
  implies, rather than racing the temporary's destruction. Observable only through
  the refcount; no signature, ownership or lifetime rule changes.

- **BOTH folded READs' POINT headers no longer come from the global heap** (#831).
  `graph_t::read_subtree_folded`'s pass-3 emit frames one exactly-sized OWNED POINT header per
  included subtree node, and `graph_t::read_children_folded` — where the wire `":children"` READ
  routes — frames one per registered child plus the outer listing header; each was built by
  `view::heap_alloc` / `view::over_bytes`, hard-wired to
  `mem::heap_backend()`, so an
  [ADR-0067](../docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md)-class node with
  `mr`, `ctl`, `value_backend`, `flat`, `egress` and its transport backend all pointed at one slab
  still leaked this framing to `malloc` — at a count a **peer** chooses (it picks which composed
  root to READ, and thus how many nodes fold; or whose `":children"` to list, and thus how many
  members frame). They now draw from the graph's existing injected
  `value_backend` ([ADR-0060](../docs/adr/0060-lkv-copy-store-injected-value-backend.md)): these
  are *payload* framing bytes — each header's length field wraps the stored TLV and the
  name records below it — as distinct from the route-byte-sized reply-egress seam of
  [ADR-0074](../docs/adr/0074-terminus-reply-egress-is-its-own-injected-backend.md), and the seam
  already carries the required cross-thread self-routed reclaim (§2), which these segments need
  because they escape inside the reply rope. **No signature change and no new injection** — one
  existing seam widened in scope, so no deployment gains a constructor parameter; an injector
  sizing a bounded slab now budgets these 4/6-byte headers too. Exhaustion is unchanged and still
  answered by value (`BACKPRESSURE`, never a throw), and the default `&mem::heap_backend()` makes
  the shipped shape allocate byte-identically. Gated by `core/tests/folded_read_backend_test.cpp`.

- **A reconnect landing *inside* an in-flight `ADVERTISE` no longer binds a stale forwarding
  swap** (#827). `route_handle_t::clear_link(L)`'s cross-link sweep (#716) can only erase
  bindings that already exist. An `on_advertise` running on another link's receive thread
  mints its out-label and retains its egress route against `L`'s pre-clear tables and binds
  the swap *afterwards* — after the sweep has already scanned its inbound link — so the
  binding lands aimed at exactly the out-label the sweep existed to invalidate. That is
  #716's permanent silent drop, recreated through a microsecond window: the downstream
  `HANDLE_NACK`s a label `on_nack` can no longer answer and the upstream never learns.
  **Two additions to `route_handle_t`, both cold-path:** `link_epoch(link)` returns an opaque
  clear-epoch token for a link, and `bind_ingress_forward(in_link, label, binding,
  down_epoch)` binds a *forwarding* swap only if that token still names the downstream tables
  the label was minted against, refusing otherwise. `clear_link` advances the epoch, and the
  epoch read and the bind are one critical section against it, so there is no interleaving in
  which a binding outlives the tables it names. `bind_ingress` is unchanged and remains the
  call for terminus bindings, which have no downstream half. A refusal is **not** counted in
  `refused_bindings` (which means "at the injected bound") and is not silent: the peer's next
  `COMPACT` misses and fires the stale-label observer, prompting a re-advertise — the
  recovery cascade and the wire surface are the ones #716 already specified. The counter
  saturates rather than wraps, per RFC-0024 §4.4; at the ceiling forwarding swaps stop being
  bound and flows degrade to the full-route `FWD` form, the same degrade an exhausted label
  space takes. The per-delivery path is untouched — `resolved` / `cache_resolution` /
  `on_compact` disassemble to the same instruction streams.

- **The terminus reply head + mint no longer draw from the global heap.** The `FWD{REPLY}`
  egress-construction segments — the head (peer-driven size: the swapped route bytes plus the
  inline tail) and, on an RFC-0024 mint, the trailing 12-byte `PATH_REF` — were built by
  `view::heap_alloc`, hard-wired to `mem::heap_backend()`, on *every* reply. It was the last
  *reply-egress* byte source an [ADR-0067](../docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md)-class
  bounded node could not bound (peer-drivable, and reachable pre-authorization); the
  folded READs' POINT-header framing (composed-root *and* `":children"`) was a separate
  value-seam residual (#831), since closed on `graph_t`'s `value_backend`. Both sites now
  draw from a **new, dedicated** `mem::mem_backend_t* egress` injection, deliberately kept
  separate from `flat` (the *flatten* seam, sized against payload bytes) so a slab sized for
  flattens is not silently re-scoped by egress heads sized against route bytes
  ([ADR-0074](../docs/adr/0074-terminus-reply-egress-is-its-own-injected-backend.md)). **Public
  constructor signature change:** `graph::op_resolver_t` gains an `egress` parameter (after
  `flat`) and `net::fwd_router_t` gains one (after `max_label_bindings_per_link`), both
  defaulted to `&mem::heap_backend()` — every existing call site is source- and byte-unchanged;
  only a bounded node that points `egress` at its slab gets the bound. No wire surface changes;
  the reply bytes are byte-for-byte identical. Exhaustion still degrades by value through the
  existing empty-rope → `or_backpressure` → addressed `STATUS{BACKPRESSURE}` path — never an
  abort, never a silent drop.
- **A mid-chain reconnect no longer kills a compacted flow permanently and silently.**
  `route_handle_t::clear_link(L)` (and therefore `fwd_router_t::clear_link` / `link_down`) now
  also drops every **ingress** binding the node holds, **on any link**, whose downstream half
  crossed `L`. A forwarding binding is stored under the link its label *arrives* on while
  `handle_binding_t::down_link` names the link the swapped label *leaves* by, so clearing only
  `L`'s own tables left a forwarder still aimed at an out-label that died with them. The
  upstream never observed the reconnect and so never re-advertised: it kept streaming
  `COMPACT`s, the forwarder kept swapping onto the dead out-label, the downstream kept
  returning `HANDLE_NACK`s, and `on_nack` answered them out of the table `clear_link` had just
  erased — a silent return, forever, with the origin never told. The sweep hands recovery back
  to shipped machinery: the upstream's next `COMPACT` misses, draws the ordinary stale-label
  `HANDLE_NACK`, and re-advertises. **No wire surface changes** — every frame in the cascade is
  an already-specified frame in an already-specified situation (normative in
  [reference/05](../docs/reference/05-protocol-tlvs.md) §route-handle and RFC-0004 §E.1;
  corrects [ADR-0062](../docs/adr/0062-resolve-once-label-bindings-hold-resolutions-not-names.md)'s
  erratum, which named the asymmetry but judged it harmless from the link-*departure* case
  alone). **Behaviour change:** a `clear_link` now invalidates strictly more bindings than
  before — terminus bindings, which have no downstream half, are untouched. Cost is
  O(links × bindings) on the cold (re)connect path only; nothing on the per-delivery path
  moves.
- **A `PATH_REF`-addressed `FWD` delivered as a MULTI-LINK rope is no longer silently
  dropped.** `fwd_router_t`'s rope arm gated its routing on `peek_fwd_dst`, which asks "does
  this frame carry an address this node can *descend*" and therefore requires a canonical
  `PATH` whose first child is a `NAME`. A bound `dst` has neither, so the frame fell through
  to the control arm, where `peek_control` refuses a `FWD` — the operation vanished with no
  reply and no drop anyone could name, on every transport that scatter-delivers (ADR-0053
  ④b), while the same operation spelled canonically and split the same way answered normally.
  It was **not** the §5.3 validation drop: the element was never validated. A structured
  `FWD` the descent cannot gate on now reaches the terminus arm, which is the conclusion the
  contiguous path always came to — restoring the router's own invariant that fragmenting a
  frame must not change whether it is applied.
- **A bound hop no longer walks the receiver-context table without synchronization.** The
  RFC-0024 element→egress lookup iterated the router's owning `std::deque` of per-child
  receiver contexts from a transport RECEIVE thread while `add_child` appended to it from
  whichever thread a CREATE arrived on — genuinely concurrent on any multi-transport node, and
  a data race on the deque's own chunk map (the container-level twin of ADR-0063 erratum 3).
  The contexts are now published through an append-only atomic chain, the same
  release/acquire shape `child_registry_t` uses for its chunks; the deque still owns them and
  is never walked by a frame path. `net_control_plane_race_test` drives a bound frame down
  that chain against the create/remove churn, and reports the race under `-fsanitize=thread`
  when the chain is ablated back to the deque walk.
- **An unknown `FWD` opcode arriving on a bound `dst` is dropped, not charged `READ`.** The
  right an operation carries is what §6.2 evaluates the ACL for; an opcode this build cannot
  name has no known right, so forwarding it after a `READ` check was a guess that a future
  write-like opcode would have crossed a READ-only gate on. It joins `REPLY` among the shapes
  a bound hop refuses rather than guesses at.
- **The bus-NAME hop rejection masks the `op` byte** (RFC-0024 §9.3). It compared the **raw**
  byte against `REPLY`, so a `REPLY` carrying a flag bit (`0x83`) was not recognised as one
  and the node answered it with an addressed error reply — the reply-to-a-reply the guard
  exists to prevent.

## [0.7.0] — 2026-08-02

### Added

- **`view::over_bytes(bytes, mem::mem_backend_t&)` and `view::segment_alloc(backend, size)` —
  an ownership copy can name the seam it draws from (#793).**
  `core/include/libtracer/mem_heap.hpp`. Purely additive: the existing
  `over_bytes(std::span<const std::byte>)` and `heap_alloc(std::size_t)` are unchanged in
  spelling, semantics and generated code, so every call site outside this change is untouched
  (verified by object-file `cmp` — see below). The new overload exists for the last rope-tier
  heap site #766 left outside the router's injected `flat`: `view_node::own_wire`'s
  **single-link** branch, the ADR-0041 §2 ownership copy of a contiguous payload, which
  flattened through the injection when the peer's fragmentation split the payload and copied
  through the **global heap** when it did not. A refused copy answers `std::nullopt` → the empty
  view the resolve walk's existing empty-value guards already read as `BACKPRESSURE`, and the
  refusal is recorded on the per-resolve sticky `spans_intact()` flag so a later span read on the
  same walk is not believed either. Measured with a counting global `operator new`: a two-link
  `FWD{WRITE}` whose payload TLV is contiguous consulted an injected `flat` **zero** times before
  and once after, global-heap `new` 16 → 15. Covered by
  `core/tests/terminus_flatten_backend_test.cpp` (exact-size seam instrument, refusing-backend
  case, a mutation-aware refusal sweep); reverting the site reddens 7 checks and drops the seam
  count to zero. A separate overload rather than a defaulted parameter deliberately: defaulting
  `mem::mem_backend_t& = mem::heap_backend()` moves that call into every existing call site and
  changed 8 library object files, and the pre-#793 arm has to be provably untouched.

- **`mem::synchronized_pool_t<Sync>` — the pool's critical section is now a compile-time
  policy (#770, [ADR-0060](../docs/adr/0060-lkv-copy-store-injected-value-backend.md) §2
  erratum 2).** `core/include/libtracer/mem_pool.hpp`. New: the class template
  `synchronized_pool_t<Sync>`, the constraint `concept mem::pool_sync_policy` (a policy supplies
  `lock()`/`unlock()`, `is_isr_safe`, `name`) and the shipped host policy `mem::spin_sync_t`.
  The pool forwards its policy's `is_isr_safe` as its own ADR-0047 §2 module-set trait.
  **`mem::sync_pool_t` is unchanged in behavior and spelling** — it is now the alias
  `synchronized_pool_t<spin_sync_t>`, so every existing use compiles and runs as before. The
  point of the seam is the *other* pairing: a single-core, priority-preemptive MCU cannot use a
  spinlock (a high-priority task spins while the lower-priority holder cannot run), and gets the
  interrupt-disable policy `tr::esp::portmux_sync_t` / `tr::esp::critical_pool_t` from the
  ESP-IDF component instead (it needs FreeRTOS headers). Compile-time per
  [ADR-0068](../docs/adr/0068-build-configuration-is-plain-cpp-config-header.md): the target
  knows its concurrency model at build time, so there is no branch or vtable on a ~120 ns op.
  **Nothing defaults to it** — `heap_backend()` remains the default at `value_backend`, `flat`
  and `transport_vertex_t`'s `rx_backend`; injecting a pool is opt-in construction, and a *bare*
  `mem::pool_t` is still wrong at all three (unsynchronised free list). Covered by
  `core/tests/mem_sync_policy_test.cpp` (policy engaged on every alloc/destroy, no double
  hand-out under contention, free list intact) — sanitizer-independent, and RED when the
  critical section is ablated.

- **`graph_t::collect()` and `graph_t::parked_seam_count()` (#576, ADR-0072 §Supersession).**
  `retire()` detaches a vertex's value seam and parks it — the seam is read lock-free, so the
  retiring thread cannot free a block a reader may still hold. The park is keyed on **handler
  presence, not role**: `adopt_identity` allocates the seam iff `on_read`, `on_write` or
  `on_children` was installed, so a `role_t::STORED_VALUE` vertex with an `on_children` parks
  one and a `role_t::HANDLER` with an empty `handlers_t` parks none. Until now the park had one
  append site and **zero** release sites, and `transport_vertex_t::remove_connection` retires the
  `/net/<module>/<name>` identity vertex — which is `role_t::STORED_VALUE` and bears a seam only
  when its link exposes a bus facet (`link->bus() != nullptr`: CAN, or a tcp/ws server wired
  `peer_named = true`). So **a bus node leaked ~96 B of `std::function` per connection teardown**,
  permanently — peer-driven growth, not operator action — while a point-to-point deployment
  (dial links, UDP, loopback, a default-wired server) parks nothing and needs no collect point at
  all. `collect()` is the park's other end — it swaps the park into a local under the map lock and lets the local
  destruct **after** the lock is released, so the free runs on the **caller's thread, outside
  every graph lock** (a seam callback's destructor may re-enter the graph). `parked_seam_count()`
  makes an uncollected park observable rather than silent. **Caller obligation:** `collect()`
  MUST be called from a point where no lock-free reader holds a value seam — the library cannot
  know that moment, so it is published rather than hidden. That includes a reader that entered
  while the vertex was still LIVE: `read` / `write` / `:children[]` load the seam pointer once
  and hold it across the user callback, so a concurrent retire hands the park a block a live
  call is still using. An embedder that never calls it keeps
  today's behaviour. Nothing was added to the read or write path; no existing call site changes.
  This supersedes the ADR-0072 reclamation domain for this site — PR #750's hazard-domain design
  is not merged (three rounds, three blocking defects, +19/+29 % on the read path), and #635
  keeps the open reclamation question.

- **`tr::graph::delivery_policy_t` — the per-subscription delivery policy (RFC-0022 §3.A).** A
  packed `u16`: bits 0–1 `reliability`, 2–4 `priority`, 5 `durability_request`, 6–15 reserved
  (written 0, **ignored** on read — never rejected, and carried verbatim so they read back
  unchanged). It rides the `SUBSCRIBER` TLV's **already-existing `SETTINGS` child** under the key
  `delivery_policy` — the same child `delivery_compact` uses — so no new wire structure exists.
  Absent ⇒ all-zero ⇒ today's behaviour, byte-identically. Both `subscribe` sugars take it as a
  defaulted trailing argument (`subscribe(src, target, policy)`, `subscribe(src, fn, ctx, policy)`,
  `subscribe(src, callable, policy)`), so every existing call site compiles and behaves unchanged.
  Only `durability_request` is consumed today; `reliability` and `priority` are stored and read
  back, awaiting the transport work that honours them.

- **`graph_t::set_history_depth(vertex_handle_t, std::uint32_t)` — the STREAM ring depth, declared
  owner-side (RFC-0022 §3.C).** Shaped like `set_delivery_mode` / `set_app_fields`: a declaration
  the owner makes host-side after registration, with **no wire surface at all** — no peer can read
  it and none can write it. The depth is what the *application* wants retained, which no peer and
  no injected resource can supply, so it is not protocol QoS and does not belong on a remote write
  surface. Costs a STREAM vertex zero extra bytes (a STREAM identity already allocates the cold
  block). Default 1, as before.

- **`graph_t::set_store_ref_min_bytes(vertex_handle_t, std::uint32_t)` and
  `graph_t::store_ref_min_bytes(vertex_handle_t)`.** The ADR-0042 §3 store-by-reference threshold,
  rehomed the same way and for the same reason — it is a deployment copy/pin trade, not a
  quality-of-service property. Semantics are **unchanged**: an absolute byte threshold, `0` (the
  default) disables referencing, read on every view-delivered write as one inline load. RFC-0022
  §3.D replaces that predicate with an amplification ratio whose constant §6 gates on a
  dual-target measurement; that measurement has not been run, so the predicate is untouched here.

- **`vertex_t::has_extension_block()`.** The RAM-census observable that replaced comparing
  `settings()`'s returned address against `kDefaultSettings` — both of which RFC-0022 deleted.
  Used by `bench_qos_census` and the vertex-size gate; not a data-plane predicate.

- **`fwd_router_t::subscribe_toward(producer, target)` (#739).** Binds a local producer's
  subscription toward ONE ordinary mount-path target (`/net/<module>/<name>/<consumer...>`,
  arbitrarily nested), resolved through the SAME ADR-0061 strip-K cached descent the forward
  path uses — no caller-side `(link, return-route)` hand-split, no single-hop assumption, no
  `net/<module>/<name>` string knowledge in the application. Bind-time resolution,
  link-lifetime durability (teardown evicts the binding; re-binding is the application's job —
  re-establishment is #716). A target whose first hop is a bus PEER answers `INVALID_PATH`
  (no directed registry entry to store yet — the #741 work's territory).

- **`child_registry_t` slots carry a precomputed name digest** — the forward demux's mount scan
  no longer touches a candidate's string or its atomic link unless the digest matches. Measured
  on `bench/bench_forward_demux` (8 alternating rounds, two builds), the scan's marginal cost
  over a fixed-position hop drops **35 ns to ~0 at 8 links, 86 to 2 at 16, and 333 to 17 at 64
  (95% removed)**, with the fixed-position hop itself unchanged within +/-2 ns. The digest is a
  filter, never a decision: `live()` and the full compare still gate every answer. New public:
  `digest_name`, `digest_segments`, `fold_segment` — public only so a test can pin the first two
  in agreement, which a lookup cannot do.


- **`tr::graph::kCacheLineBytes` — false-sharing padding becomes a per-target knob.** New
  constant in `libtracer/config.hpp` (CMake: `-DLIBTRACER_CACHE_LINE_BYTES`, default 64), and
  **0 means "this target has no second core to false-share with"**. It governs every shared
  table libtracer pads: the vertex lock stripes and the hazard domain's cells and retire
  lists. Measured on rv32 (`-Os -fno-exceptions -fno-rtti`, GCC 15.2, real `core/src/graph.cpp`),
  16 stripes cost **1,024 B of `.bss` at 64 and 128 B at 0** — a 896 B static-RAM saving on a
  single-core node, with the hazard domain worth a further 6.5 KB when bound. The ESP-IDF
  component *derives* the value from `CONFIG_FREERTOS_UNICORE` rather than adding a Kconfig
  option: a unicore build has no second core by construction. Purely an optimization axis —
  the wrong value costs control-plane throughput and changes nothing observable. The host
  default is unchanged, byte for byte. New: `tr::graph::kStripeAlign`,
  `tr::graph::detail_hp::kDomainAlign` (both derived, both `static_assert`ed to have actually
  taken effect), and a `sizeof(vertex_stripe_t)` gate in `vertex_size_test`.

- **`tr::graph::hazard_slot_t` — the host LKV slot, reclaimed with hazard pointers** (#604,
  [ADR-0069](../docs/adr/0069-lkv-slot-is-a-compile-time-policy-hazard-reclamation.md) §2/§5/§6).
  A lock-free `std::atomic<node*>` over a 24-byte indirection node that owns the rope's
  `shared_ptr`; a read pins the node, copies the handle out, and unpins, so `read_stored()`
  keeps its owning signature and nothing in `graph.cpp` changed. Bind it per target with
  `-DLIBTRACER_LKV_SLOT=hazard_slot_t`; the new `tr::graph::kHazardReaderSlots` knob
  (`-DLIBTRACER_HAZARD_READER_SLOTS`, default 64) sizes the process-wide domain at
  `(N + 1) * 128` bytes — **zero** unless the slot is bound, since the default binding never
  references the registry. Measured against the default slot through `graph_t::read` on one
  shared LKV: **4.2× aggregate and 4.4× p50 at 24 readers** (1.7 → 7.4 M ops/s), decisive from
  8 readers up, and indistinguishable at one — the gain is a concurrency gain only. Three
  differences from `sp_atomic_slot_t` that are not performance and are documented in the
  header: reclamation is deferred (so an injected `pmr` resource must outlive the threads that
  wrote through it, not just the graph); a publish can fail under memory exhaustion, which it
  **reports** — `vertex_t::store` turns it into the same `nullptr` → `BACKPRESSURE` soft-fail an
  LKV allocation failure already produces, and the node comes from the global heap rather than
  from a graph's injected resource; and an undersized `kHazardReaderSlots` costs throughput
  rather than correctness.

- **`tr::graph::lkv_slot_t` — the LKV slot is now a per-target binding** (#604,
  [ADR-0069](../docs/adr/0069-lkv-slot-is-a-compile-time-policy-hazard-reclamation.md) §1). The
  slot type `vertex_t` holds is selected in `config.hpp` through the same forward-declare-plus-alias
  mechanism `acl_policy_t` already uses, set by the CMake cache variable `LIBTRACER_LKV_SLOT`
  (default `sp_atomic_slot_t`, so a stock build and the ESP-IDF component are unchanged). No
  templating is involved: `vertex_t` names the alias, and binding a different slot is a
  target-configuration change rather than an edit to any header. Verified that the generated
  header substitutes the requested type, that the configure-time drift gate is **not** fooled by a
  non-default cache value, and that `graph.cpp.o` is byte-for-byte the size it was before the
  alias was introduced — the indirection costs nothing.

- **`libtracer/lkv_slot.hpp` — the LKV slot is a named policy** (#604,
  [ADR-0069](../docs/adr/0069-lkv-slot-is-a-compile-time-policy-hazard-reclamation.md) §1). The
  new public header defines `tr::graph::sp_atomic_slot_t`, which is exactly the
  `std::atomic<std::shared_ptr<const rope_t>>` `vertex_t` has always held, behind the three
  operations the vertex actually performs — `[[nodiscard]] bool store(value_ptr_t, order)`,
  `void clear(order)`, `value_ptr_t load() const`. `store` reports rather than returns void
  because a slot that reclaims lazily must allocate to publish, and a policy that cannot say
  "I did not take this" would force the one thing [#477](https://github.com/avatarsd-llc/libtracer/issues/477)
  exists to prevent: a dropped write reported as a successful one. `sp_atomic_slot_t` allocates
  nothing and always returns `true`. **No behaviour change and no new configuration**: this
  slice only names the seam so a later one can bind a different reclamation strategy per target.
  The header also states the contract a replacement must satisfy — chiefly that `load()` returns
  an *owning* handle, because the composed branch read holds one per node across three passes.
  Verified neutral: `graph.cpp.o`'s symbol table is unchanged (423 symbols, none added or
  removed, so the policy inlines away), the `sizeof(vertex_t)` gate is unmoved, and
  `bench_compact_delivery`'s forward and terminus hops sit within ±1.1% p50 over eight
  alternating runs per side with no consistent sign.

- **`graph_t::delivery_drops()` — a dropped delivery is now countable** (#629). A path-target
  subscription edge (the form a wire `SUBSCRIBER` produces) delivers by re-dispatching into its
  target, and three conditions make that impossible: the target PATH resolves to no live vertex,
  the target's `:acl` denies the edge's stored caller (the #81 fan-in gate), or the nothrow
  delivery clone cannot be allocated (#477). All three are specified to drop that ONE leg while
  the write itself succeeds — correct, and previously **invisible**: a node whose target had been
  retired dropped every delivery for the rest of its life with nothing anywhere to say so. Note
  that an edge may name a target that does not exist and still be admitted, so this is reachable
  without anything being retired.

  Returns `delivery_drops_t{no_target, denied, out_of_memory}` — per cause, because "something
  dropped" is not actionable. Counted, never enforced: nothing in the library reads them, so a
  deployment decides whether to alarm. Relaxed monotonic and incremented only ON a drop, so the
  delivering path is unchanged while nothing drops — the same near-free-when-idle shape as
  `ancestor_walks()`. The three loads are not one coherent snapshot, deliberately: a lock on the
  drop path to serve a diagnostic would cost more than it tells.

- **`tr::mem::pool_source_t<Sync>` — a bounded, RECYCLING `block_source_t`** (#597,
  [ADR-0067](../docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md)). Closes the
  gap between `heap_source_t` (recycles, unbounded) and `bump_source_t` (bounded, never
  recycles): a **long-lived** seam could not be bounded at all, so an 8 KiB bump source wired
  as a router's `rx` decoded six frames and rejected the next 194. Segregated free lists keyed
  on the exact `(bytes, align)` pair, with **no per-block header** — the seam's sized `release`
  makes one unnecessary. Both bounds are injected per RFC-0006: the caller supplies the slab and
  the `size_class_t` span, so neither the byte ceiling nor the class count is a constant in this
  library. `classes_used()` / `overflowed()` report what to size the span against.

  Shape chosen on a recorded trace of the seam (70,937 events): **12 distinct sizes, three
  covering 99.8 %**, so exact classes cost zero internal fragmentation and need 26,176 B where
  first-fit-with-coalescing needs 27,448 B and TLSF 28,440 B. Costs **322 B** of text on
  `riscv32-esp-elf-g++ -Os -fno-exceptions -fno-rtti`, plus a 24 B vtable.

  **Read ADR-0067 §3 before sharing one.** A `pool_source_t` is structurally the object
  ADR-0060 erratum 1 measured collapsing to ~1/15 of its single-thread rate on a 12-core host
  while the platform heap scaled. It is owned by one thread wherever it sits on a per-frame
  path; sharing behind a lock is admissible only at wiring frequency.

- **`tr::mem::sync_none_t`** (in `mem_source.hpp`) and **`tr::mem::sync_mutex_t`** (in the new
  `mem_source_sync.hpp`) — the synchronization policies for the above. The default is empty and
  compiles to nothing under `[[no_unique_address]]`; the hosted one is in its own header so
  `mem_source.hpp` stays freestanding-clean for the footprint sentinel. A policy is anything
  with `lock()`/`unlock()`, so a single-core target supplies its own interrupt-disable section.

- **`fwd_router_t::add_child` takes an optional per-child failable source**:
  `add_child(std::string name, transport_t& link, mem::block_source_t* rx = nullptr)`, stored on
  the child's `child_rx_ctx_t`. **Source-compatible** — the parameter is appended with a default,
  and a child that supplies none draws from the router's as before.

  This is what makes a bounded source *usable* on the RX path at all. The router previously held
  one `rx_` while its own class comment documented `on_frame` as firing on several transports'
  receive threads concurrently with no per-request locking — so a shared pool there would have
  put a contended lock on a deliberately lock-free path, at the 15x cost ADR-0060 erratum 1
  measured. Each transport has its own receive thread, so a source parked on the child is
  touched by exactly one. It also makes the bound **per-peer**: one noisy link can no longer
  starve another link's decode.

  Resolution is `rx_for(ctx)` — a branch on a pointer already in a register, with no lookup by
  name on the frame path.

- **`vertex_t::replace_edge(idx, subscriber_t, edge_latch_t*)` and `vertex_t::edge_replace_t`** —
  the §D.1 replace primitive. Takes the durability latch under the same single lock hold as
  `add_edge`, so a concurrent `clear_edge` cannot slip between the write and the snapshot, and
  reclaims the displaced edge's segment pin in place. It never grows `subs_`: an out-of-range
  index is refused rather than back-filled, because the index arrives off the wire.

- **BREAKING (routing addresses): a bus PEER is addressed `<mount>/<peer>/<residual>`, not
  `<peer>/<residual>` — RFC-0014 S2a (`9d038b9`).** Per-module mount routing made
  routing-address equal vertex-path, and a multi-peer child now resolves the next segment in
  **its own** peer table, eating one more segment. A dst that used to reach a CAN peer as
  `n21/a/b` must now say `can0/n21/a/b` (or the full `net/can/can0/n21/a/b` for a connection
  created through `/net`). Nothing rejects the old form — it simply stops resolving, so a peer
  goes quietly unreachable and a READ times out rather than erroring.

  This slice landed without a CHANGELOG entry, which is how it reached a downstream firmware as
  a mystery timeout (`"READ over vcan never resolved"`) and cost a 188-commit bisect to
  attribute. The commit message documented it thoroughly; the file an integrator actually reads
  did not.

- **BREAKING: `transport_vertex_t::provide_link` gains a leading `module` argument** — RFC-0014
  S2a. A staged link bypasses the connection factory, so there is no `kind` to derive the mount
  from and it must be named: `provide_link("can", "can0", tcan)`. Modules are **declared**, not
  derived (`register_module(module, kind, role)`) — a transport with both a dial and a listen
  shape is two modules (`ws-client` / `ws-server`), while a bus like CAN is one for both roles;
  an undeclared kind falls back to `<kind>-client` / `<kind>-server`, so an externally
  registered transport keeps working.

- **`graph_t::set_app_fields_static` / `vertex_t::set_app_fields_static` take a
  `tr::graph::borrowed_fields_t`** instead of a `std::span<const app_field_static_t>`
  (ADR-0058 erratum 2). **Source-compatible with every array-shaped caller**: the new type
  converts implicitly from `const app_field_static_t (&)[N]` and from `std::array`, which is
  what a `static`/`constexpr` table already is. It deliberately does **not** convert from a
  `std::vector` or a bare `std::span`.

  This closes a silent break. The previous release tightened this install's lifetime contract
  to cover the ARRAY, not just the bytes it points at — but a `std::span` binds implicitly to a
  `std::vector`, so a caller that built its table into a function-local vector kept compiling
  and began viewing freed memory. That reached a downstream firmware consumer and cost a
  188-commit bisect to find, because there was no compiler diagnostic anywhere.

  **If this stops compiling for you, you had the bug.** Give the table storage that outlives the
  vertex. A genuinely runtime-sized table — a language binding mapping a foreign POD array into
  slots, typically — opts out explicitly with `borrowed_fields_t::unchecked(span)`, which asserts
  the lifetime by hand. Note the guard rejects the container/temporary mistake, not every one: a
  block-scope array still binds, since static storage duration is not expressible as a constraint.

- **`tr::mem::block_array_t<T>::data()`** (both const and non-const), returning the contiguous
  block or `nullptr` when empty. For handing the array to a pointer/length API — the egress
  `iov` table below is the first caller. Invalidated by any growth, like every other
  contiguous container.

- **`tr::mem::bump_source_t`, `null_source()`, and `block_array_t<T>`** — the companions the
  migrated call sites need. `bump_source_t` is the nothrow twin of
  `std::pmr::monotonic_buffer_resource` over a caller buffer, with an upstream so it stays
  capability-preserving; `null_source()` is the upstream that makes the buffer a hard bound.
  `block_array_t<T>` is a nothrow growable array of trivially-copyable `T` whose growth
  returns `false`.

  `block_array_t::push_slot()` claims one uninitialized slot to fill **in place**. That is
  load-bearing, not sugar: `push_back(T{...})` on a 48-byte element materializes the
  aggregate on the stack field-by-field and reads it back as wide loads, and the
  store-forwarding stall cost **~45 % of a terminus decode while executing FEWER
  instructions** (IPC 5.0 → 2.6). It was the entire regression this change first showed, and
  five other hypotheses — modulo vs mask alignment, growth inlining, index vs pointer
  cursor, the sink's latch stores, `is_canonical_name` inlining — were each measured and
  refuted before the profile pointed here.


- **A nothrow failable-block seam: `tr::mem::block_source_t` (#551, slice 1).** New header
  `libtracer/mem_source.hpp` with `block_source_t` (`try_alloc` / `release`, both `noexcept`,
  `nullptr` on exhaustion), the default `heap_source_t`, and `tr::mem::heap_source()`.
  `graph_t` gained a THIRD, **appended** constructor parameter
  (`mem::block_source_t* ctl = &mem::heap_source()`) and a `control_source()` accessor.
  Appended, so every existing `graph_t{...}` call site — including the shipping
  `graph_t graph{&mr};` — compiles unchanged.

  Why a new type rather than reusing `std::pmr::memory_resource` with a documented
  may-return-null contract: `memory_resource::allocate` is annotated `returns_nonnull`, so
  the caller's null check is undefined behaviour and **is deleted at `-Os`/`-Oz`** —
  measured on `riscv32-esp-elf-g++ 15.2.0`, where the soft-fail branch survives at
  `-O0`/`-O1`/`-O2`/`-O3` and vanishes at the two size-optimized levels. `-Os` is what the
  reference ESP-IDF node ships, and no job executes an allocation-failure path at it (the
  one `-Os` binary CI runs, `full-node-host`, drives only the happy path).

  **As of slice 1 no call site drew from the seam**; the #588 entry above then made the
  branch-write decode its first consumer. The registration
  allocations that motivated #551 migrate next.

- **The LKV publish takes no lock when nobody is awaiting (ADR-0064 §1, #555).** A non-`STREAM`
  write now bumps `write_seq_` with one `fetch_add(seq_cst)`, reads the stripe's `waiters`
  count, and returns **without acquiring the stripe mutex** when it is zero. `#370` already
  skipped the condvar *call* on this path; the mutex itself was what remained, and measured it
  is not free — it sits immediately downstream of the `lkv_` atomic publish, so the two
  serializing regions land back-to-back on the critical dependency chain (~38 of the write's
  ~335 cycles). `inproc` measures **89.2 → 82.6 ns/op**, faster in 5 of 5 pinned interleaved
  rounds. `current_seq()` becomes lock-free as a consequence. `STREAM` roles are unchanged —
  their ring append is real state mutation and keeps the lock.

  **`write_seq_` and `vertex_stripe_t::waiters` are now atomic and part of a documented
  ordering contract**: the writer bumps the sequence *before* reading `waiters`, the waiter
  publishes `++waiters` *before* reading the sequence, both `seq_cst`. Reversing either order,
  or relaxing either access, silently reintroduces a lost-wakeup window. The argument is in
  ADR-0064 §1 and beside the code.

  Also corrects the "lock-free" language on `lkv_`: `std::atomic<std::shared_ptr<T>>` is
  lock-free *by contract* and **spin-locked in libstdc++** (`is_lock_free()` returns 0), which
  is ~77 of the remaining ~316 cycles and now the largest single term on the write path.
  ADR-0064 §2 records what a genuinely lock-free slot would take and why none was chosen yet.

- **Borrowed app-field installs now really cost zero declaration RAM (ADR-0058 erratum 1).**
  `set_app_fields_static` promised the slots would view caller flash, but the runtime still
  copied the caller's array into an owned `std::vector` — and `app_field_static_t` /
  `app_field_slot_t` were field-for-field identical, so the copy converted between a type and
  itself. Measured host-side, that vector was the **largest single resident block** on the
  borrowed path: 200 B of the 456 B a 5-field table added. **`app_field_static_t` is now an
  alias of `app_field_slot_t`**, and `app_field_table_t::slots` is a `std::span` the borrowed
  install points straight at the caller's array. Per-vertex live bytes for a 5-field borrowed
  table drop **592 → 392 B (11 → 10 allocations)**; the owning install is unchanged, and the
  struct does not grow (the owning copy moves to `unique_ptr<slot[]>`, which with the span is
  the size the vector was on host and rv32 alike). Gated by the `vertex_app5_static` row.

  **⚠ Stronger caller contract.** The runtime now views the **array**, not merely the
  `name`/`descriptor` bytes it points at — so a `set_app_fields_static` caller must keep the
  array itself alive for the vertex's lifetime. Pass a `static`/`constexpr` array; a stack
  array that previously worked by accident will now dangle. Wire-invariant (`:schema` serves
  identical bytes), so no RFC.

- **Control-plane serialization — `fwd_router_t` and `transport_vertex_t` each gain an internal
  mutex (ADR-0063 §3).** `transport_vertex_t` had **no synchronization at all**, yet the graph
  invokes its connection factory *outside* `map_mutex_`, on whichever transport's receive thread
  delivered the CREATE. With two transports that is two threads, so concurrent `make_connection`
  calls raced `conns_` / `pending_links_` (and the unlocked `settings_of` / `link_of` readers
  raced the insert's rebalance), while `fwd_router_t::add_child` raced on the registry's
  scan-then-append — two writers could be handed the **same empty slot**, silently losing a
  child — and on the `child_rx_` deque spine. Now serialized. **The forward and delivery paths
  take no lock and are unchanged** (ADR-0038 §3). Lock order, where more than one is held:
  `transport_vertex_t` → `fwd_router_t` → `graph_t::map_mutex_` → the vertex stripe. Cost is
  ~4 B static per lock plus a FreeRTOS mutex allocated on first lock — one-time, not per
  connection. ADR-0063 originally specified an arch-selected sync *trait* for this; its
  Erratum 1 records why that was withdrawn (it was never built, it cannot wrap a section that
  blocks on sockets and `map_mutex_`, and a spinlock there risks unbounded priority inversion
  on single-core FreeRTOS).

- **`child_registry_t::child_t::multi_peer` is now `std::atomic<bool>`.** `add` **rebinds** an
  existing slot on the tombstone-reuse path that create/remove churn takes constantly, and that
  rebind wrote this field while the lock-free mount descent read it — a genuine reader-vs-writer
  data race that the atomic `link` did not cover. Confirmed under TSan, independently of the
  locks above: with the locks in place and this field left plain, the race still reports.

- **Connection teardown — `child_registry_t::erase`, `fwd_router_t::remove_child`,
  `transport_vertex_t::remove_connection` (#494).** The FWD child registry was add-only, so a
  retired connection left a dangling `name → transport_t*` — latent while removal had no wire
  path, a use-after-free once RFC-0014's remove-half drives create/remove churn. `erase`
  **tombstones in place** (nulls the slot's link, keeps its NAME) rather than erasing from the
  vector, so a concurrent lock-free reader's iteration stays valid; a later `add` of the same
  NAME reuses the tombstone, so churn on a stable name set does not grow the table.
  `remove_connection` sequences un-route → `retire()` the vertex → destroy the owned socket. The
  eviction hook is **removal, not departure**: `link_down` alone remains correct for a link that
  merely dropped, since a DIAL connection's vertex outlives its socket and self-heals.
  `remove_connection` is owner-internal — the RFC-0014 `NAME`-write dispatch (S2b) is what will
  expose it to the wire. `child_registry_t::size()` now counts slots including tombstones;
  `live_size()` counts those that still resolve.

- **Link-liveness enum — RFC-0014 S1 (`tr::net::link_state_t`, #492).** A connection
  vertex's stored value is now the six-state liveness enum
  (`DORMANT`/`DIALING`/`RECONNECTING`/`UP`/`LISTENING`/`BIND_FAILED`, RFC-0014 §4) rather
  than a binary up/down byte. `conn_settings_t` gains `backoff_ms` and
  `connect_timeout_ms` (config keys `backoff` / `connect_timeout`), parsed now but
  consumed by the S5 liveness engine that lands later. The state remains manually driven
  in S1: a config-constructed socket self-reports `UP` (DIAL) / `LISTENING` (LISTEN) at
  creation; the engine becomes the sole writer of the DIAL transitions in S5. The 1-byte
  wire encoding (table order, `DORMANT = 0x00` preserving the old "down") is the reference
  encoding until the S7 conformance vectors make it normative (the RFC defers the byte
  clauses to #492).

- **`route_handle_t::link_count()` (#488).** A diagnostic accessor returning the number of
  live per-link table shells in the compaction registry — the observable that `clear_link`
  now returns to steady state (see Fixed).

### Changed

- **Conformance vector `fwd/fwd-routed-multihop` is renamed `fwd/fwd-routed-mount-residual`;
  a genuine two-mount vector `fwd/fwd-routed-two-mount` is added (#419).** No public C++ API
  moves, but the conformance corpus is normative-by-incorporation (ADR-0007), so the rename is
  recorded here. **The renamed vector's bytes are byte-for-byte unchanged** — the maintainer
  ruling (2026-08-02) is that they were always correct under strip-K mount routing
  (RFC-0014 §S2a / ADR-0061): `dst=/net/board/can0/ow/sensor` parses as ONE hop — the mount key
  `net/board/can0` consumed as a whole run, residual `ow/sensor`. Only the name and the
  `description.md` prose were stale artifacts of the pre-S2a two-hop reading. The new
  `fwd-routed-two-mount` carries `dst=/net/uplink/b/net/uplink/c/sensor/temp` — a `dst` crossing
  TWO mounts — and is bound behaviourally by the new `core/tests/fwd_two_mount_test.cpp`
  (`ctest -R fwd_two_mount`), the first FWD test in the tree with more than one forwarder: three
  in-process `fwd_router_t` nodes chained by loopback link pairs, wired through the production
  `transport_vertex_t` `:children[]` SPEC door, asserting `strip_k = 3` at each hop and the
  terminus delivery. Corpus: 50 → 51 vectors, all three cores green.

- **Conformance vector `fwd/fwd-src-accumulated`'s BYTES are rewritten (#419) — the first time
  any vector's bytes have changed.** The corpus is normative-by-incorporation
  (ADR-0007) and `v1.md` §conformance is explicit that *adding* a vector is free while
  *changing existing bytes* is a spec change, so this landed only under a second, explicit
  maintainer ruling (2026-08-02) on the still-`DRAFT` protocol — recorded here because any
  implementation pinned to the old bytes will now fail the harness. Old:
  `FWD{ op=READ, dst=/can0/ow/sensor, src=/via_board/via_net/reply-ep }`, 77 B, `0f4049…`.
  New: `FWD{ op=READ, dst=/net/uplink/d/sensor/temp,
  src=/net/downlink/a/net/downlink/cli/reply-ep }`, 119 B, `0f4073…`. The old frame was
  **pre-S2a and did not compose**: under the shipped mount routing a forwarder consumes and
  prepends a whole three-segment `net/<module>/<name>` run, so a `src` grown by two hops is six
  segments, never the two bare names `via_board`/`via_net` — neither of which is a mount key at
  all. The name is kept: the vector still shows exactly what it always claimed, `src`
  accumulating mid-route. Now bound behaviourally by `core/tests/fwd_two_mount_test.cpp`'s hop-2
  probe, which asserts that same `src` byte-exactly out of the production router. Corpus stays
  at 51 vectors; cpp/ts/rust green.

- **Conformance vectors `fwd/fwd-reply-result` and `fwd/fwd-reply-error`'s BYTES are rewritten
  (#419) — maintainer ruling (c), 2026-08-02, the same instrument and the same defect as the
  entry above.** These two were the residue that ruling (b) left: they carried
  `dst=/via_board/via_net/reply-ep`, the identical pre-S2a accumulated route, in the two vectors
  that show a REPLY retracing one. Recorded here because any implementation pinned to the old
  bytes will now fail the harness.
  - `fwd/fwd-reply-result`: old `FWD{ op=REPLY, dst=/via_board/via_net/reply-ep, src=/sensor,
    kind=RESULT, VALUE u32=1234 }`, 76 B, `0f4048…` → new
    `FWD{ op=REPLY, dst=/net/downlink/a/net/downlink/cli/reply-ep, src=/sensor/temp,
    kind=RESULT, VALUE u32=1234 }`, 110 B, `0f406a…`.
  - `fwd/fwd-reply-error`: old same route with `kind=ERROR, STATUS{ERROR{VALUE u16=0x0020}}`,
    82 B, `0f404e…` → new same corrected route, 116 B, `0f4070…`.

  Both now depict **one explicit point**: the reply exactly as the terminus C of
  `fwd/fwd-routed-two-mount`'s route emits it, before any forwarder has consumed a mount run —
  RESULT and ERROR being the two sides of the same frame. That is adjudicated from the code, not
  chosen: `op_resolve_walk.hpp`'s `assemble`/`assemble_error` splice the request's two routes
  **swapped and verbatim** (`reply dst = req.src`, `reply src = req.dst`), so the reply `dst` is
  necessarily the accumulated `src` `fwd/fwd-src-accumulated` carries, in whole
  `net/<module>/<name>` mount runs — which is exactly what lets `route_fwd_forward` walk it home
  by the same op-agnostic strip-K descent. `kind`, the VALUE payload and the RFC-0002 §C ERROR
  identity are untouched: only the route was wrong. Bound behaviourally by
  `core/tests/fwd_two_mount_test.cpp` (the hop-2 probe pins the request `src` these vectors'
  `dst` mirrors; the terminus leg asserts the REPLY arrives with `dst` consumed to `/reply-ep`)
  and `core/tests/op_resolve_test.cpp` (the swap and the `0x0020 tr::path::not_found` identity).
  Corpus stays at 51 vectors; cpp/ts/rust green. **With rulings (b) and (c) both executed, no
  route-bearing conformance vector carries the pre-S2a form any more.**

- **`op_resolver_t`'s `flat` now bounds the SPAN (arena) tier too (#801).**
  `core/include/libtracer/op_resolve.hpp`, `core/include/libtracer/fwd_router.hpp` — **no
  signature changes**; what changed is which allocation the already-public `flat` parameter
  covers, which is why it is documented here. `arena_node::own_wire` — the ADR-0041 §2 ownership
  copy of a borrowed arena span, and the *only* site the span tier allocates at — drew from
  `view::over_bytes`'s global heap. It now takes the `over_bytes(bytes, backend)` overload #793
  added, carried by the same per-resolve seam the rope tier uses. This was the tier that mattered
  most in practice: a synchronous CAN/UART child forwards a **contiguous span** inline on its
  receive (ADR-0038), so the MCU terminus took the unbounded copy on its *ordinary* WRITE, not on
  an exotic fragmented one — and which tier runs is decided by the delivering transport, so a
  stored value's provenance depended on the link it arrived over. A refusal answers
  `std::nullopt` → the empty view the walk's existing empty-value guards already read as
  `BACKPRESSURE`; `arena_node::spans_intact()` deliberately stays a constant `true`, because an
  arena span is borrowed from the frame and a refused allocation cannot shorten one. The backend
  travels as a `resolve_node` argument rather than as a node member, so `arena_node` — copied by
  value throughout the walk — stays two words wide. **Default unchanged** (the global heap), and
  25 of the library's 27 object files `cmp` byte-identical to `origin/main` — only
  `op_resolve.cpp.o` and `op_resolve_view.cpp.o`, the two TUs that instantiate the walk, differ.
  Covered by
  `core/tests/terminus_flatten_backend_test.cpp`: an exact-size seam instrument, a
  slab-containment *provenance* assertion on the stored value, a READ control that must draw
  nothing, a refusing-backend case and a mutation-aware sweep. Reverting the site reddens **8** of
  the new checks and takes the seam count to zero.

- **`op_resolver_t` takes the flatten backend, so the TERMINUS rope flattens are bounded too
  (#766).** `core/include/libtracer/op_resolve.hpp`.
  `explicit op_resolver_t(graph_t&)` gains a second, defaulted parameter:
  **`explicit op_resolver_t(graph_t& graph, mem::mem_backend_t* flat = &mem::heap_backend())`**.
  Every existing call site compiles unchanged and the default is the global heap, so an
  un-injected node is byte-identical to before. (At the time of #766 the span (arena) tier drew
  from this parameter not at all; #801, above, put its one allocating site on it.)
  `fwd_router_t` now passes its
  own `flat` down, which closes the #730 gap this issue was split out for: the resolver's rope-tier
  flattens — `view_node::ensure_cache` (the per-node contiguous span every `wire()`/`body()` read
  of a multi-link TLV materializes) and `view_node::own_wire` (the ADR-0053 ⑤ ownership flatten) —
  took `rope_t::materialize`'s default backend, so a bounded node that had injected `mr`, `rx`,
  `flat`, `ctl`, `value_backend` and the transport-receive backend at one slab **still allocated on
  the global heap** the moment a peer sent a fragmented terminus request. Peer-drivable, and on
  `-fno-exceptions` the `abort()` the seam programme exists to remove. The honest sentence is now
  **all rope flattens on the forward AND terminus paths draw from the injected seam**; `flat` still
  does not cover what is not a rope flatten (the arena is `rx`'s, the reply head segment is
  neither's).

  A refused terminus flatten is answered **by value**, never by reading a short span: the shared
  resolve walk carries a sticky per-call `spans_intact()` on its node-reader concept (a constant
  `true` on the span tier, so the checks fold away on the MCU terminus) and answers an addressed
  `kind=ERROR STATUS{BACKPRESSURE}` reply — or, when the refusal hit the reply's OWN route bytes
  and no trustworthy address remains, a `BACKPRESSURE` status the router drops. Never a `RESULT`
  reply built on an empty span, never a truncated route, never a stored value. Covered by
  `core/tests/terminus_flatten_backend_test.cpp`: the seam is proved consulted (`served() > 0` —
  zero before this change), the RAM claim is measured with a counting global `operator new`
  (18 vs 24 calls for the same 4-link `FWD{READ}` with `flat` on a static-slab pool vs the heap),
  and a sweep moves the refusal point across every flatten one request makes and requires each
  outcome to be a drop or an addressed `BACKPRESSURE`. Ablated both ways: re-pointing the two sites
  at the default heap reddens 8 checks, and deleting the walk's guards reddens the sweep with a
  `kind=RESULT` answer built on a short span.

- **The owner-declared pin knob is renamed to what its value became (#774).**
  `core/include/libtracer/graph.hpp`, `vertex.hpp`.
  `graph_t::set_store_ref_min_bytes(vertex_handle_t, std::uint32_t)` /
  `graph_t::store_ref_min_bytes(vertex_handle_t)` become
  **`graph_t::set_pin_payload_ratio(vertex_handle_t, std::uint32_t k)`** /
  **`graph_t::pin_payload_ratio(vertex_handle_t)`**; the extension-block member
  `vertex_ext_t::store_ref_min_bytes` becomes `pin_payload_ratio`. **The old names cease to
  exist** — no alias, no deprecation shim, so a stale call site is a compile error rather than
  a silent misconfiguration. The u32 layout, the storage class, the owner-side home, the
  absence of any wire surface, the `0` default and the non-inheritance (RFC-0022 §3.F) are all
  **unchanged**.

  The rename is not cosmetic: since §6's bench landed (#758, PR #771) this u32 has held the
  RFC-0022 §3.D amplification ratio `K` — the store pins iff `payload_bytes * K >=
  segment_bytes` — while the name still said *minimum bytes*, the absolute-threshold predicate
  the bench refuted. An owner writing `4096` expecting "pin only payloads ≥ 4 KB" was instead
  asking for "pin any payload whose 4096x amplification clears the segment", which at large
  segments is close to the opposite. Per RFC-0022 Amendment 2 the shipped default on both
  targets remains the sentinel `tr::graph::kPinNever` (0), so nothing pins unless a deployment
  declares a non-sentinel `K`; the per-vertex value overrides `config_t::kPinPayloadRatio` and
  exists so §6-style measurement arms rotate inside one process.

- **The mount WIDTH bound is lifted — one registry pass, any width (#523, #765).**
  `core/include/libtracer/child_registry.hpp`, `fwd_frame_view.hpp`, `fwd_router.hpp`,
  `route_handle.hpp`. A mount key of **any** number of segments now registers and resolves; the
  1..3 bound is gone and `tr::net::kMountPeekMax` is **deleted**, not raised. The descent is
  INVERTED to a single pass: `child_registry_t::longest_prefix` walks the table ONCE, matching
  each slot against the prefix of that slot's own `seg_count` (a new `child_t` field), so it is
  O(N) in the table and independent of width. The `k = W..1` retry loop and
  `child_registry_t::by_segments` are gone with it.

  **API changes, all source-compatible except where noted:**
  - `fwd_router_t::add_child` returns **`bool`** instead of `void` — `false` ⇔ the name is
    unaddressable and NOTHING was registered. Deliberately not `[[nodiscard]]`, so existing
    call sites compile and behave unchanged. The check is ALWAYS ON, replacing a debug-only
    `assert` that compiled out under `NDEBUG` and therefore left every release build with no
    bound at all. What it refuses is only what no address could ever name: an empty name, a
    name with an empty segment, or one wider than `graph::kMaxSegments` — the path-depth
    budget being the ONLY real bound on mount width. `false` ALSO covers the registry refusing
    to grow: `child_registry_t::add` now reports its soft allocation failure instead of
    swallowing it, and `add_child` refuses BEFORE it wires the link's receiver — so a node
    that cannot allocate a registry chunk gets no child rather than a GHOST one, audible on
    its transport and resolvable by no `dst` (`core/tests/mount_add_oom_test.cpp`).
  - `child_registry_t::add` returns **`bool`** instead of `void` — `false` ⇔ no slot could be
    appended and NOTHING was registered. Also not `[[nodiscard]]`, so control-plane call sites
    that ignore it are unchanged.
  - `tr::net::kMountPeekMax` — **removed.**
  - `tr::net::peek_fwd_dst_segs` — **removed**, replaced by `tr::net::peek_fwd_dst` (opens the
    `dst` window, materializes nothing) plus `tr::net::dst_seg_walk_t` (reads leading segments
    on demand). Nothing on the forward path is sized by mount width any more: the walk's state
    is a cache of `tr::net::kDstSegCacheSlots` offsets — one cache line, from the existing
    `kCacheLineBytes` config — and re-reads beyond it, so a 33-segment mount costs the router
    no more stack than a 1-segment one.
  - `child_registry_t::by_segments` — **removed** (it existed to serve the width loop). Use
    `longest_prefix`, which answers the routing question directly.
  - `handle_binding_t` / `resolved_binding_t` gain `mount_gen`; `child_registry_t` gains
    `mount_generation()`.

- **Label bindings are stamped with the mount shape (#765).** A binding records where an
  address SPLIT into "local mount" and "remote residual". Registering a deeper mount moves that
  split, and nothing noticed: a full `FWD` resolved against the new mount while a `COMPACT`
  riding the old label still used the old one — the two planes disagreeing about the same
  address, silently. Until this change that was unreachable only because NEITHER plane could
  reach a deeper mount, which the width lift ends. `child_registry_t::mount_generation()` is
  bumped on every registration and teardown; a binding carries the value it was resolved
  against and is validated on use, and a mismatch takes the SAME RFC-0004 §E.1 self-heal an
  unknown label takes (drop, fire `on_stale_label`, `HANDLE_NACK`, re-advertise). Third
  validate-on-use stamp, beside `graph_t::retire_generation` and the registry tombstone — no
  second invalidation mechanism, no new error code.

- **`kMaxSegments` repriced 32 → 255 (RFC-0023, accepted; #767).** `core/include/libtracer/path.hpp`
  — a **monotone widening** of `path_t::parse`: every path that parsed before still parses, and a
  33–255-segment address that used to answer `INVALID_PATH` is now legal. The 32 was inherited
  from the extraction (ADR-0001) as a string-parser guard and was never priced; 255 is chosen so
  every per-segment quantity (count, slot index, table dimension) stays `u8`-representable.
  **Nothing in `core/` is dimensioned by the constant** — `segments_` is already a `std::size_t`,
  the parse-time reserve is keyed to `kMaxPathBytes`, and the one cap-sized scratch
  (`core/src/fwd_router.cpp`) is keyed to `kMaxSegmentBytes` and a fixed slot count — so the
  static-RAM delta is **zero by construction** (RFC-0023 §4.4, re-verified on this tree).
  (RFC-0023 as accepted named that scratch `kMaxSegmentBytes × kMountPeekMax`, which was its
  shape at the time; the #523 lift in this same release deletes `kMountPeekMax` and shrinks the
  scratch to `kMaxSegmentBytes × 2` — two stitch slots, a transient and a retained. The RFC's
  own text is left as accepted; the conclusion it reaches — nothing is dimensioned by
  `kMaxSegments` — is only more true afterwards.) Under the current
  NAME-TLV body encoding each segment costs `4 + len`, so the **1024-byte `kMaxPathBytes` cap
  binds first at 204 segments** and the count clause cannot fire; it becomes binding only under
  RFC-0018's packed body. The observable widening today is therefore 32 → 204.
  `kMaxSegmentBytes`, `kMaxPathBytes` and `kMaxFieldDepth` are unchanged.

- **BREAKING: `fwd_router_t` takes a fourth injection — the `flat` byte backend every rope
  flatten it performs draws from (#730).** The constructor is now
  `fwd_router_t(graph, mr = get_default_resource(), rx = &heap_source(), flat = &heap_backend(),
  max_label_bindings_per_link = 0)`; `flat` is inserted BEFORE `max_label_bindings_per_link`, so a
  call site that passed that ceiling positionally must move it one place right (a type error, not a
  silent misbind). Every other call site is unchanged, and the default reproduces the previous
  global-heap behaviour byte for byte. The router's four `materialize()` sites — the ingress
  `ADVERTISE` route and `COMPACT` payload sub-rope flattens, the cold bus-name rejection flatten,
  and the per-delivery egress `COMPACT` flatten — all took `rope_t::materialize`'s DEFAULT
  heap backend, so a bounded node's memory bound did not cover any of them however much it
  injected.

  **The bound was the smaller half.** Two of those sites did not check their result, and an empty
  flatten is not a visibly failed one: `view::over_bytes` maps an empty span to an ENGAGED-empty
  optional by design, and `graph_t::write` stores it and reports success. So a heap exhaustion
  during the ingress `COMPACT` flatten REPLACED the subscriber's last-known value with nothing,
  fired the delivery callback, and reported success — silent corruption, indistinguishable
  downstream from a legitimate empty write. Both unguarded sites now answer a refused flatten by
  value: the `COMPACT` drops the delivery (the vertex keeps its last-known value) and the
  `ADVERTISE` binds no label (the peer's subsequent `COMPACT`s draw the ordinary `HANDLE_NACK`
  self-heal). The guard could not have shipped without the seam — with every site on the global
  heap there was no way to make a flatten fail, and `core/tests/fwd_flatten_backend_test.cpp` is
  what the seam bought: an injected backend that refuses on command, and an ablation proving each
  guard fails without it.

  **Contract corrections, filed against the merged change by an adversarial verify pass.** The code
  is unchanged; four claims written around it were wrong and are now narrowed.

  - **`flat` MUST be thread-safe** — the same obligation `graph_t`'s `value_backend` carries
    (ADR-0060 §2), and it was missing from `@param flat`, which said only "must outlive the router".
    Three of the four sites run on a transport child's receive thread (several children receive
    concurrently) and the fourth on the writer thread, and a `segment` self-routes its reclaim on
    whichever thread drops the last reference. A bare `mem::pool_t` is **not** thread-safe — plain
    `std::size_t` free-list head and count, no lock, no atomic — and the ESP32 recipe in
    `docs/interop/esp32-production-node.md` shipped exactly that. It now injects `heap_backend()`
    for both byte-buffer seams and says why: `sync_pool_t` is the only synchronised pool in the
    tree and it is a spinlock, wrong for a single-core target, and the interrupt-disable variant
    ADR-0060 §2 names is unbuilt.
  - **`flat` bounded the router's OWN four flattens, not "every flatten the router performs"** — the
    terminus resolver's rope-tier flattens never saw it and still drew from the global heap.
    Measured: with `flat` armed to refuse everything, a 4-link `FWD{READ}` consulted it zero times
    and made 20 global-heap `new` calls. Tracked as **#766** and **since closed** (see the
    `op_resolver_t` entry under Changed, above): the router hands `flat` to its resolver, and the
    header, the source comment, `docs/design/allocation-and-backpressure.md`, the fwd-router module
    page and the ESP32 recipe now say all rope flattens on the forward and terminus paths.
  - **The bus-name rejection flatten is now exercised.** It was listed as covered by
    `fwd_flatten_backend_test.cpp` and was not — reverting both halves of the site left the suite at
    72/72. It has its own case, which asserts `refusals() > 0` first, and reverting the site's seam
    fails it.
  - **The `ADVERTISE` `empty()` early-out is no longer claimed as a proven guard.** It is redundant
    with the `wire::decode` on the next line and its removal is unobservable — honestly reported at
    the time, then pinned as a citation anchor and cited as the line producing the drop. The anchors
    now pin the three flatten SEAMS (which the test does prove) plus the one guard that is
    independently observable, and both redundant early-outs say plainly in code that they are
    redundant early-outs.

- **BREAKING (RFC-0022 §3.B): `tr::graph::settings_t` and `kDefaultSettings` are DELETED, and
  `register_vertex` / `try_register_vertex` / `register_vertex_key` lose their fourth parameter.**
  The type is removed, not shrunk. Four of its seven knobs (`reliability`, `priority`,
  `deadline_ns`, `queue_max_bytes`) were inert — writable, readable, consumed by no code;
  `durability` describes a *delivery relationship* and moved to the subscription
  (`delivery_policy_t::kDurabilityRequest`); the two survivors are construction parameters, not
  QoS, and became owner-side declarations (above). Nothing is inherited (§3.F): there is no
  ancestor walk, no cached ancestor reference, and no propagation question when a parent's
  configuration changes after its children exist.

  `graph_t::settings(vertex_handle_t)` and `vertex_t::settings()` are removed with it. Code that
  passed a `settings_t` no longer compiles; that is deliberate — a silent behaviour change would
  be worse, since four of the fields never functioned and the other three moved.

  **A vertex-RAM win falls out of it.** `adopt_identity`'s extension-block gate loses its
  `settings == kDefaultSettings` term, so **strictly more vertices stay extension-less than
  before**: REGISTRATION can no longer force the cold block. The caller's `settings_t` was copied
  onto the new vertex at registration time and never consulted afterwards — so removing the
  parameter removes the whole mechanism. Where a non-default
  `settings_t` passed to `register_vertex` used to materialise a whole `vertex_ext_t` (120 B on
  x86-64) on the new vertex, a declaration now reaches exactly the vertex it names and costs
  nothing anywhere else.

- **BREAKING (RFC-0022 §4): the whole `:settings.<knob>` write surface is REMOVED.** All seven
  historical names — `reliability`, `priority`, `durability`, `deadline_ns`, `queue_max_bytes`,
  `history_keep_last`, `store_ref_min_bytes` — answer `SCHEMA_NOT_FOUND`, the honest answer and
  the one an unsupported field already gives. The answer is **caller-independent**: an unknown
  core-namespace NAME resolves to nothing before any ACL gate, so a denied caller sees
  `tr::schema::not_found` and never `tr::access::denied`. **There is no deprecation window:** the
  protocol is DRAFT, and of the seven only three ever drove behaviour — none of them as remotely
  writable QoS ([#756](https://github.com/avatarsd-llc/libtracer/issues/756)). `settings.app.*`
  writes (RFC-0010 §A) are untouched.

- **BREAKING (RFC-0022 §4): the `:schema` and bare-`:settings` reads keep their SHAPE and lose
  their KNOBS.** The bare `:settings` read becomes `SETTINGS{ [NAME "app" SETTINGS{…}] }` — the
  reserved `app` subkey and RFC-0010 §A.4's single-traversal renderer contract both survive, and a
  vertex with no declared app fields reads an **empty** `SETTINGS{}`, which is honest rather than
  absent. `:schema`'s synthesized protocol part becomes `SETTINGS{}` — an empty enumeration, and
  therefore for the first time a **complete** one. `:schema` previously advertised `deadline_ns`
  (u64), which nothing consumed, and omitted `store_ref_min_bytes`, which the write path reads on
  every write, so the reported set was not even a subset of the working set: that dissolves
  [#706](https://github.com/avatarsd-llc/libtracer/issues/706) rather than fixing it.
  `:settings.app` and `:settings.app.<name…>` are unchanged. The `tlv-types/point-schema-app` and
  `field/field-nested` conformance vectors were regenerated (the latter now spells the two-level
  field with the surviving `app` subkey); six new vectors were added under `subscriber/`,
  `settings/` and `stream/`.

- **BREAKING (RFC-0022): the transient-local durability latch is now the SUBSCRIBER's request.**
  It fires when *that* subscription set `durability_request`, not when the producer carries a
  flag. This changes which subscribers are latched: a producer's `durability = 1` used to replay
  its last value to **every** subscriber of that vertex, including ones that never asked, and
  there was no way to decline. A producer that relied on the flag must now let its consumers ask
  (they are the ones who know whether they want the replay). ADR-0049's one-door decision is
  unaffected — the latch still fires at the single admission step, for every door.

- **TypeScript client: `encodeSubscriber` / `LibtracerClient.subscribe` take an optional
  `deliveryPolicy`** (`SubscriberOptions.deliveryPolicy`, with the `DELIVERY_DURABILITY_REQUEST`
  constant exported). Omitted or `0` emits no `SETTINGS` child at all, so existing callers produce
  byte-identical frames.


- **BREAKING: the derived `"<kind>-client"` / `"<kind>-server"` module name is gone (#621,
  ADR-0073 §4).** `transport_vertex_t::module_for` no longer invents a module for an undeclared
  *(kind, role)* pair — it returns `result_t<std::string>` and answers `SCHEMA_NOT_FOUND`
  (previously `std::string`, always synthesizing), and creation with an unregistered kind now
  fails with `SCHEMA_NOT_FOUND` (the unknown-SPEC-`type` convention) instead of silently
  mounting under a path segment core picked. `register_module` now returns `result_t<void>` and
  gates the name with the shared ADR-0073 §1 segment predicate (`tr::graph::valid_segment`) —
  a reserved-character, empty, or oversized module name answers `INVALID_PATH`. The consumer
  fix is one explicit `register_module` call per module.

- **BREAKING: linking a built-in transport no longer registers its module names (#621,
  ADR-0073 §4).** `register_udp_transport` / `register_tcp_transport` / `register_ws_transport`
  (and therefore the default `transport_vertex_t` ctor) register only the transport *factory* —
  never a module name. The application declares each module under a name IT chooses via
  `register_module`; the built-ins now export *suggested*-name constants
  (`kUdpClientSuggestedModule`, `kUdpServerSuggestedModule`, `kTcpClientSuggestedModule`,
  `kTcpServerSuggestedModule`, `kWsClientSuggestedModule`, `kWsServerSuggestedModule` in their
  transport headers) an application may pass. `/net` remains only the constructor-default
  root — a documented recommendation, not a library rule.

- **A bus link's connection NAME is no longer a routable next-hop (#741, ADR-0073 §3,
  RFC-0020).** An inbound FWD whose `dst` routed THROUGH a multi-peer link's own
  `net/<module>/<name>` mount with a residual naming no current peer fell through to the bus
  transport's `send()` — a broadcast to every open peer, drawing N replies for one directed
  request and scrambling FIFO reply correlation (the #409 failure). The forward path now
  REJECTS that hop: a single directed `FWD{REPLY, kind=ERROR}` with
  `STATUS{ERROR{tr::path::invalid (0x0021)}}` back along the accumulated `src` (drop by value
  for a malformed frame or a REPLY, matching the terminus resolver's split), and `on_advertise`
  binds nothing for such a route (the peer's COMPACTs draw the ordinary HANDLE_NACK).
  Wire-visible behavior change; a `dst` naming the mount exactly (the connection vertex) and
  peer-directed hops (`/net/<module>/<name>/<peer>/...`) are unchanged. Fan-out remains the
  subscription plane's.

- **Bus-accepted peers are named `p<slot>` (#426, ADR-0073 §2).** The ws and tcp multi-peer
  listeners named an accepted session `<ip>:<port>` — two reserved characters, so a peer was
  enumerable in the synthesized `:children[]` but addressable by no conforming client, and the
  delivery tag tainted every accumulated return route. The routable NAME is now the slot index
  (`p0`, `p1`, …) — a pure function of the slot, stable across recycling; the `<ip>:<port>`
  string is transport-diagnostic only and no longer appears anywhere in the graph. Observable
  name change; `enumerate_peers` / `peer_link` / the peer-named delivery tag all carry the new
  spelling. A bus peer name identifies a session, not a device.

- **A rebind of a live connection name no longer touches the published mount run (#684, ADR-0072
  §4).** `child_registry_t::add`'s rebind branch move-assigned `mount_tlv` while the forward
  path may read it as a span on a transport receive thread — a use-after-free whose symptom is
  a corrupted mount prefix on an emitted frame. The bytes were identical anyway (the encoding is
  a pure function of the slot's key); the member is now documented immutable-after-publish, the
  rebind updates only `multi_peer`/`link`, and a debug assert pins the purity invariant. No
  signature change; the reconnect path drops one alloc+free.

- **A peer-supplied child name must pass the segment predicate (#688, ADR-0073 §1).** The wire
  creation path (`graph_t::create_child`) now rejects a SPEC `name` containing a reserved
  character (`/ : . * ?`) or exceeding `kMaxSegmentBytes` with `INVALID_PATH` — the same answer
  `path_t::parse` gives the same bytes. Previously the name was checked only for non-emptiness,
  so a peer could register a vertex that is enumerable but addressable by no conforming client.
  **New public `tr::graph::valid_segment(std::string_view)`** (path.hpp) is THE shared segment
  predicate; both tiers call it, so they cannot drift. Wire-visible: a create that used to
  "succeed" with an unaddressable name now errors.

- **A write to a vertex nobody subscribes to no longer takes the vertex lock stripe (#635).**
  `graph_t::fan_out` gated `snapshot_edges` on nothing, so every write acquired the stripe mutex
  whether or not an edge existed — and a stripe is shared by `kVertexLockStripes` vertices, so two
  unrelated vertices serialised against each other on a hash collision alone. RFC-0005's
  near-free-when-idle promise was already kept this way by `mark_pending` / `clear_pending`;
  `fan_out` was the write-path verb that did not keep it. **New `vertex_t::own_subs_ordered()`** —
  the same count as `own_subs()` under `seq_cst`, and the only read that may be used to SKIP a
  fan-out. **`vertex_t::bump_own_subs` is now `seq_cst`** rather than relaxed; it is the subscriber
  half of a Dekker pair with that read, and subscribe is control-plane-cold. Behaviour is
  unchanged: the count now rises *before* the slot it stands for, which is what keeps a write that
  races a subscribe from being lost by the new gate. Measured at **×10.5** aggregate write
  throughput at 24 threads on distinct vertices sharing a stripe (9.30 → 97.73 M writes/s,
  median of 11 interleaved pairs), with the fan-1 arm flat at ×0.99–1.14 as the control.

- **`wire::path_key` returns `std::optional<std::vector<std::byte>>`** and rejects a `PATH` whose
  children are not all `NAME`. Two callers updated. Returning `nullopt` rather than an empty vector
  is deliberate: `graph_t::find_ptr` walks segments from the root, so an **empty key resolves the
  ROOT vertex** — an empty-key rejection would have turned #681 into a misroute to `/`.

- **`tr::net::kFwdMaxIov` is now a structural `9`, counted from `fwd_rebuild_t::gather`'s emit
  sequence, rather than `6 + 2 * kMountPeekMax` (= 14).** The `2 * kMountPeekMax` term described a
  header-and-bytes pair per prepended mount segment; **that emission has not happened since #508**,
  which made the mount run one precomputed span. The constant was therefore over-provisioned by 5
  and — the part that mattered — tied to a mount width the region count had long been independent
  of. Callers reading it as a buffer size get a smaller, exactly-fitting bound; the contiguous
  forward hop's stack array shrinks by 5 spans (80 B on a 64-bit host). No behaviour change: the
  emitted region count is unchanged.

- **`route_handle_t` and `fwd_router_t` take an optional per-link binding bound (#603).**
  `route_handle_t(mr, max_bindings_per_link = 0)` and
  `fwd_router_t(graph, mr, rx, max_label_bindings_per_link = 0)`; `0` keeps the prior
  unbounded behaviour, so every existing call site is unchanged. `route_handle_t::bind_ingress`
  and `record_egress` now return `[[nodiscard]] bool` — `false` means the link's table was at
  its bound and nothing was recorded. New `route_handle_t::refused_bindings()` counts them.

- **`route_handle_t::alloc_label` and `ensure_egress` now report label-space exhaustion**
  instead of wrapping. `alloc_label` returns `0` (the reserved "none") once a link has issued
  1..65535; `ensure_egress` returns `{0, false}` and records nothing. Callers **must** treat
  `0` as "cannot compact" and never stamp it on a frame — `fwd_router_t::advertise` already
  documented `0` as its failure return, and now uses it for this case too.

- **Configuration is one named type: `tr::graph::default_config_t`, bound by `using config_t`**
  ([ADR-0070](../docs/adr/0070-configuration-is-a-named-traits-type.md)). Every compile-time knob
  is a member; the loose names (`kVertexLockStripes`, `lkv_slot_t`, …) remain and are **derived**
  from it, so no call site moved. An application declares its own by inheriting
  `default_config_t` and overriding what differs. **Verified byte-identical codegen** against the
  previous shape — same disassembly and same section sizes for `core/src/graph.cpp` on host
  x86-64 `-O2` and rv32 `-Os`.

  It is bound **once**, not threaded as a template parameter. ADR-0070 supersedes ADR-0068 §2's
  `basic_graph_t<config_t>` plan on measurement: a parameter is latency-neutral by construction,
  its one unique capability (two configs per binary) forks the process-global stripe and hazard
  tables and so costs RAM, and an app-declared traits type cannot reach the library's
  out-of-line TUs.

- **The `sizeof(vertex_t)` RAM-diet gate moved from `vertex_size_test.cpp` into `vertex.hpp`**,
  keyed on `config_t::kMaxVertexBytes{64,32}`. A `static_assert` in the header is evaluated by
  every TU that includes it, so **every target and every configuration now checks its own
  binding** — including the 32-bit arm, which was never checked before (no CI leg cross-compiled
  that test, while the ESP-IDF legs compile `vertex_t` on every PR). Note that the 32-bit ceiling
  has **zero headroom**: rv32 sits exactly on 80 B, so the next inlined 32-bit member is a build
  failure by design.

- **BREAKING: `graph_t::read` and `graph_t::await` return `result_t<value_ref_t>`** — a
  REFERENCE to the vertex's published value — instead of `result_t<rope_t>`, a copy of it. The
  value was already refcounted (`shared_ptr<const rope_t>` is the LKV slot's value type); the
  read then copied the rope out of it, cloning one `segment_ptr_t` per link, and each clone is
  a contended refcount RMW on the line every reader of that vertex shares. Measured on the real
  path with both binaries built once and alternated over 6 rounds: **median 1.48x aggregate,
  95/102 paired samples**, up to **3.07x** on distinct vertices at 8 readers, with p50 falling
  90 -> 60 ns on the uncontended shapes. New type: `tr::graph::value_ref_t` (dereferences to the
  rope; `value_ref_t::composed` wraps a freshly built one).

  **Migration:** `r->only()` becomes `(*r)->only()`, and `*r` becomes `**r` where a `rope_t` is
  wanted. The rule the surface now follows: **a read of a PUBLISHED value returns a reference to
  it; a read that COMPOSES a new value returns the value** — so `read(handle, field)`,
  `read_children_folded`, `read_children_materialized` and `read_subtree_folded` are unchanged
  and still rope-valued. The composed BRANCH read measured **1.00x** (30 paired samples), so the
  shape that cannot benefit does not pay for the change either.

- **BREAKING: build configuration is plain C++ — the config macros are gone**
  ([ADR-0068](../docs/adr/0068-build-configuration-is-plain-cpp-config-header.md)). The new
  public header `libtracer/config.hpp` carries every per-target compile-time knob as ordinary
  C++; a CMake build generates it from `config.hpp.in` (shadowing the checked-in default, with
  a configure-time drift gate keeping the two identical). Consequences:
  `LIBTRACER_VERTEX_LOCK_STRIPES` is now the constexpr `tr::graph::kVertexLockStripes`, set
  via the CMake cache variable (or ESP-IDF menuconfig) — a bare `-D` compile definition **no
  longer does anything**; `LIBTRACER_ACL_FULL` remains the CMake option name but no longer
  exists as a preprocessor symbol (it rebinds the `acl_policy_t` alias in the generated
  header). Pre-production, so no deprecation shims.

- **BREAKING: `fwd_router_t`'s five observability/terminus sinks are fn-ptr + context**
  (`on_reply` / `on_inbound` / `on_raw` / `on_compact_delivery` / `on_stale_label`), no longer
  `std::function`. They fire on the per-frame RX path — the seam
  [ADR-0047](../docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md) already
  called "the largest avoidable embedded liability on the delivery path" when it rejected
  `std::function` receivers — and now take `(fn, ctx)` in the same shape as
  `graph_t::subscriber_fn_t`. Wiring-frequency `std::function`s elsewhere
  (`posix_endpoint_t::start`, `peer_visitor_t`) are deliberately kept. Pre-production, no
  deprecation shims ([ADR-0068](../docs/adr/0068-build-configuration-is-plain-cpp-config-header.md)
  records the survey).

- **`graph_t::read` no longer takes the graph map lock**
  ([#652](https://github.com/avatarsd-llc/libtracer/issues/652)). The leaf/branch fork — "does
  this vertex have a registered child?" — was computed by walking the child list under
  `map_mutex_` shared, on **every** read. Being process-wide, that lock capped the whole
  process at roughly 20 M reads/s no matter how many cores were reading or how disjoint the
  vertices, and a blocking lock plateaus rather than collapsing, so a flat aggregate read like
  "scales fine" until you notice flat across a 24× thread range means each thread is 24×
  slower. The predicate is now a bit on the vertex, set when a child is filled and recomputed
  when one is retired, both under the unique lock the graph already held. No API change.
  Measured, twenty-four readers on distinct vertices: **19.7 → 163.5 M ops/s (8.28×)**, which
  is 99% of what short-circuiting the call entirely achieves; 3.19× at eight readers.
  Shared-vertex reads and every write shape land inside their run-to-run spread, as expected —
  that lock was never what bound them. `sizeof(vertex_t)` is unchanged at 112 B: the bit
  shares a byte with the existing `OWN_ACES` flag rather than taking a word of its own.

- **`vertex_ext_t` no longer caches a second, projected ACE list.** `eff_aces_inherit` — a
  materialized copy of `eff_aces`'s `kAceInherit`-flagged elements — is removed;
  `effective_acl_t::allows` gained a `required_flags` parameter and filters the single cached
  merge in place. `vertex_ext_t` drops **104 → 96 B on rv32**, and each ACL-bearing vertex loses a
  heap block plus N x 32 B of `ace_t` copies rebuilt on every merge invalidation.

  The evaluator could already do this: both ACL policies' `allows` take `required_flags` ("ACEs
  lacking these flag bits are skipped"), so the projection duplicated a filter that existed.
  Filtering in place is **order-identical** to projecting — skipping elements cannot reorder the
  ones that remain — which matters because the full policy is first-match-per-bit in stored
  order. (Partitioning the vector by the flag, considered first, is **not** sound: it would move
  an own INHERIT-flagged ACE past ancestor ones.)

  **Only affects you if you call `vertex_t::with_effective_aces` directly**: the `eval` callable
  now takes one list, not `(merged, inherited)`. Select the inheritable subsequence by passing
  `kAceInherit` as `effective_acl_t::allows`'s `required_flags`.

- **`subscriber_t::target_key` and `edge_view_t::target_key` are now
  `std::shared_ptr<const std::vector<std::byte>>`, not `std::vector<std::byte>`** — the key is
  immutable and refcount-shared instead of deep-copied per delivery. A null pointer means "no
  local re-dispatch target", replacing the old `.empty()` test.

  **Why:** `snapshot_edges` copied the key into every `edge_view_t` so dispatch could run outside
  the vertex lock, and that snapshot is a fresh stack object per publish — so every edge with a
  local target allocated on the fan-out path. It cost **two** allocations, not one, because
  `try_reserve` is probe-then-commit (probe allocates and frees, then the real reserve allocates).
  Measured with a global operator-new counter: a delivery to a local target went from **2.00
  allocations to 0.00**, with the store's own LKV allocation unchanged at 1.00 on both sides as a
  control.

  Sizes on rv32 (`riscv32-esp-elf -Os`): `subscriber_t` 40 → 36 B, `edge_view_t` 84 → 80 B — so
  the 8-deep inline dispatch snapshot drops from 672 to 640 B of stack per publish.

  **Only affects you if you construct `subscriber_t` directly** (the in-process
  `graph_t::subscribe` sugar and every wire path are unchanged). Build the key with
  `tr::graph::try_make_target_key(std::move(bytes))`, which soft-fails to null on OOM rather than
  aborting under `-fno-exceptions`.

- **`settings_t`'s member order changed (widest-first), shrinking it 32 → 24 B.** Layout only —
  no field was added, removed, renamed or retyped, and the wire form is unaffected (it emits
  **named** children, so it never depended on declaration order). `vertex_ext_t` drops
  112 → 104 B on rv32 and 168 → 160 B on x86-64, so every ext-bearing vertex (ACL, handlers,
  app fields, STREAM ring, or non-default QoS) gets 8 bytes back on every target.

  `settings_t` is all fixed-width scalars, so it is the one runtime type that does **not** shrink
  with the pointer — it was 32 B on a 32-bit MCU exactly as on a 64-bit host, making it the
  largest non-ACL member of `vertex_ext_t` on device.

  **Only affects you if you positionally aggregate-initialize it** (`settings_t{1, 0, 8, …}`),
  which would now bind values to different fields. Use designated initializers
  (`settings_t{.deadline_ns = …}`) or assign members; every construction site in this tree
  value-initializes with `settings_t{}` and needed no change.

- **BREAKING (wire behaviour): an indexed `:subscribers[N]` write is now payload-discriminating,
  per RFC-0009 §D.1 — and `:subscribers[*]` on a write is rejected (#598, #579).** The
  implementation cleared slot `N` payload-blind, so a `SUBSCRIBER`, a junk `VALUE` and the
  empty-`STATUS` sentinel all produced the identical clear and the identical `RESULT`. Now:
  an empty `STATUS` (`09 00 00 00`) clears; a `SUBSCRIBER` **replaces** slot `N`'s edge; anything
  else is `TYPE_MISMATCH`; an index no slot answers to is `INVALID_PATH`.

  Three consequences an integrator must read:

  1. **A replace passes the `SUBSCRIBE` gate, not merely `WRITE`.** It enters the same admission
     door as an append (ADR-0049), so a caller holding `WRITE` but not `SUBSCRIBE` can still
     clear a slot but can no longer install one.
  2. **Writes that used to succeed now fail.** A client clearing a slot with an arbitrary payload
     was already non-conforming (§D.1 calls this a conformance *repair*), but it was succeeding;
     it now gets `TYPE_MISMATCH`. Clear with the sentinel.
  3. **`:subscribers[*]` was silent data loss.** The wildcard sets `indexed` and leaves `index`
     at 0, so a wildcard write cleared **slot 0** and answered `RESULT`. It is now
     `INVALID_PATH`; the `WRITE` grammar has no wildcard axis.

- **BREAKING: `transport_vertex_t::set_link_state(std::string_view, bool)` →
  `set_link_state(std::string_view, tr::net::link_state_t)` (#492).** The manual
  link-state seam now takes the liveness enum instead of a bool. Migrate `set_link_state(n,
  true)` → `set_link_state(n, tr::net::link_state_t::UP)` (or `LISTENING` for a listen
  socket) and `set_link_state(n, false)` → `…::DORMANT`.

- **Per-transport recv-thread stack sizing (#486).** Every socket transport constructor
  (`udp_transport_t`, `tcp_transport_t` dial+listen, `transport_tcp_server`,
  `transport_ws_server`, `transport_ws_client`) and `socketcan_link_t` gain a trailing
  `recv_stack` parameter, and `twai_link_config_t` gains a `stack_size` field — all
  defaulting to `0` (the platform default, i.e. the prior behavior). A non-zero value
  right-sizes that one thread's stack instead of forcing an integrator to inflate the
  **global** `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT` (which pays for every pthread in the
  system) to cover the stack-heaviest recv loop. Socket transports apply it via
  `pthread_attr_setstacksize` (honored by glibc and the ESP-IDF pthread layer alike); the
  ESP TWAI link applies it via `esp_pthread_set_cfg`. Size to the measured high-water mark.

- **The shared recv-thread scaffold (`posix_endpoint_t::start`) and `socketcan_link_t` now
  spawn via `pthread_create`, not `std::thread`.** `std::thread`'s constructor *throws* on a
  spawn failure, which under the MCU profile's `-fno-exceptions` becomes `std::abort` — a
  thread-spawn OOM on a starved node would bring the whole process down. `pthread_create`
  returns an error code; a failed spawn now leaves the endpoint simply not receiving (a soft
  fail in keeping with the 0.6.0 OOM→`BACKPRESSURE` hardening), never an abort. At the
  default stack size the observable behavior is otherwise unchanged.

### Fixed

- **An `AWAIT` carrying a `:field` selector is rejected instead of silently awaiting the whole
  vertex (#585).** ⚠️ **Wire-visible behaviour change**: the reply status for such a frame moves
  from `tr::flow::timeout` (`0x0041`, after the full await deadline) to `tr::schema::not_found`
  (`0x0031`, immediately). The selector was decoded and validated and then **discarded**, so
  `await <v>:<anything>` was byte-identical to `await <v>` — a peer asking to be woken on one
  facet was woken on the whole vertex, or told `timeout`, which it cannot tell from a quiet link.
  RFC-0010 §C settles the direction (*"`await` on a single field is deliberately unsupported"*),
  and `reference/02` already carried the MUST NOT (*"Consumers … MUST NOT expect per-field
  wakeups"*) — this enforces it. It applies to **every** selector, including `:subscribers` and
  `:acl`, which read and write normally: the field is real, the await surface is not.
  `graph_t::await` takes no field parameter, so the local API is unaffected.

- **The per-link label allocator no longer wraps onto live labels (#603).** `next_label` was a
  bare `std::uint16_t` incremented unchecked, so allocation 65536 handed out the **reserved 0**
  and 65537 handed out **1** — while label 1's egress entry still aliased the first flow's route.
  A COMPACT on the reused label resolved the **wrong route**: a silent misroute, not a drop. The
  allocator now saturates. On exhaustion `deliver_remote` falls through to the full-route
  `FWD{WRITE}` form, which carries its own route and needs no label — the degrade
  [ADR-0038](../docs/adr/0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md)
  §3 already specified (*"exhaustion falls back to full-route `FWD{WRITE}`"*) and nothing
  implemented. Labels are still not reclaimed individually; `clear_link` (the (re)connect
  self-heal) restores a link's whole space.

- **The per-link label tables are no longer unbounded (#603).** `ingress` was push_back-only and
  peer-driven, bounded only by the 16-bit label space — 65535 `handle_binding_t` (each carrying a
  `std::string` and a `std::vector`) is megabytes per link on a node whose whole budget is 16 KB.
  Both tables now honour an **injected** ceiling, and a full table **refuses** rather than evicts:
  evict-oldest is right for `can_reassembly_t`, whose groups are short-lived so oldest ≈ stalest,
  but a label binding exists precisely *because* a flow is long-running, so evicting the oldest
  would preferentially kill the longest-lived stream and make it re-advertise forever. True LRU
  would need a write on `resolved()` — the per-delivery hot path — to solve a flow-setup problem.
  A refused flow delivers over the full-route `FWD{WRITE}` form; established flows are untouched.
  The remaining half of #603 — the throwing pmr allocation under `-fno-exceptions` — is unfixed.

- **An ADVERTISE route with a non-`NAME` child no longer binds a label (#681).** `wire::path_key`
  re-emitted **every** child's payload through `wire::emit_name` with no type check, so a peer's
  `PATH{VALUE "sensor"}` composed byte-identical key bytes to the legal `PATH{NAME "sensor"}` and
  resolved `/sensor` on the wire-facing ADVERTISE path — while the arena tier
  (`path_lookup_key`), given the same bytes, answered `INVALID_PATH`. The two tiers disagreed:
  #436's shape, one layer up, and the fix landed only in the arena tier at the time. Gated by a
  new `compact_cache_test` case that drives the malformed route through `on_frame`, i.e. the
  production wiring — the RFC-0014 lesson was that two silent misroutes shipped because no test
  used it.

- **The contiguous forward arm now DROPS an over-wide gather instead of truncating it.** Its iov
  guard filled what fitted and sent the partial span list, putting a **truncated frame on the
  wire** — the exact outcome the rope arm's own comment two branches down calls *"worse than
  none"*. Two arms of one hop disagreeing on a drop policy is the shape of the CRC divergence
  fixed above and of #516. Unreachable today (`gather` emits at most `kFwdMaxIov` regions for a
  contiguous source by construction); it is a guard against a future region being added without
  the count moving, and there a counted drop is recoverable where a corrupt frame is not.

- **A fragmented control frame now gets the same CRC check as a contiguous one.**
  `fwd_router_t::on_control_rope` called `peek_control` without a `crc_check_t`, so it took the
  `DEFER` default: a multi-link `ADVERTISE` / `COMPACT` / `HANDLE_NACK` carrying the `CR` bit had
  its trailer read as structure and never verified, while the **byte-identical unfragmented
  frame** was dropped by the span arm's explicit `VERIFY`. Whether a control frame mutated
  routing state therefore depended on how a peer chose to fragment it.

  Not a regression: before the `crc_check_t` parameter existed, `read_fwd_header` hard-coded
  `DEFER`, so `peek_control` never verified on either arm. The commit that introduced the
  parameter added `VERIFY` to the span arm and left the rope arm on the default — a gap that was
  half-closed, not one that opened. ADR-0041 §1's 2026-07-26 amendment already states the intended
  rule ("`crc_check_t::VERIFY` passed explicitly so verify-before-apply is preserved"); the code
  now matches it on both arms, so the ADR needs no erratum.

  The `DEFER` default is deliberate and stays: a forward hop relays bytes it never interprets, and
  making it verify would put a CRC pass on the hot path. The policy travels with the explicit
  argument at the two call sites that apply a frame, not with the default.

  Regression guard added in `fwd_rope_forward_test` — it corrupts a body byte under a valid
  trailer and splits the frame so the corruption and the trailer land in **different links**, the
  shape a contiguous arm cannot produce. Verified to fail before the change and pass after.

- **The rope FORWARD hop can no longer abort (#596).** `fwd_router_t::route_fwd_forward`'s
  multi-link instantiation gathered its scatter-gather entries into a
  `std::pmr::vector<std::span<const std::byte>>` drawn from the injected `mr_`. The entry
  count is `~6 + link_count` — **chosen by the sending peer**, on the **forward** path, which
  sits behind no ACL and is not even the terminus. Growth went through `std::pmr`, so a
  fragmented heap threw `std::bad_alloc`, which on `-fno-exceptions` ESP-IDF is the
  link-wrapped `abort()` stub: the same peer-reachable reboot #588 removed from the decode
  path, still live on egress. The reply path immediately below it had already been guarded
  (`rope_t::try_to_iovec`), so this was an asymmetry, not an oversight of the whole file.
  The table is now a `mem::block_array_t` over the router's injected `rx_`
  ([ADR-0065](../docs/adr/0065-failable-allocation-gets-its-own-seam-block-source.md)) and
  exhaustion **drops the hop**. Dropping rather than sending the entries that fit is the
  point: a partial iov is a *truncated FWD on the wire*, and FWD is not delivery-guaranteed,
  so the sender retries. No API change for callers that do not inject a source — `rx_`
  already defaulted to `mem::heap_source()`.
  Pinned by `core/tests/fwd_rope_forward_test.cpp`: one maximally fragmented rope through
  three seams — `null_source()` emits **nothing** (asserted as nothing, not as something
  short), a bounded `bump_source_t` and the default heap source both emit bytes identical to
  the contiguous oracle. Reverting the fix fails the first two.
- **The wire RX decode path can no longer abort (#588).** `wire::decode_into` — the terminus
  arena decoder, reached from a peer's frame **behind no ACL** — made three unguarded
  `std::pmr` allocations: the node array's `reserve`/growth, the sink's open-node stack, and
  `walk_stack_t::grow`'s spill past its 8 inline slots (the only direct `->allocate(` in
  `core/`). A peer picks the nesting depth and the node count, so on a fragmented heap any
  of the three threw `std::bad_alloc`, which on `-fno-exceptions` ESP-IDF is the
  link-wrapped `__cxa_throw` → `abort()` stub. Measured on the host, same 24-deep frame and
  a 64 B budget: **exit 134 (SIGABRT) before, `TLV_NESTING_TOO_DEEP` after.**

  All three now draw from a `tr::mem::block_source_t`. No new status was invented —
  `TLV_NESTING_TOO_DEEP` is what RFC-0006 already specifies for "exceeds this receiver's
  decode resources", and `walk_stack_t::push` already returned `false` for the no-spill
  case; only the allocation was dishonest.

  **Also closes the `#477` residual** at the branch-write decode (`graph.cpp`), which used a
  `monotonic_buffer_resource` whose overflow leg drew from the *throwing* default upstream.
  A `bump_source_t` over the same stack buffer falls back to the graph's injected
  `ctl` block source instead (which defaults to the nothrow heap source), so capability
  is unchanged, exhaustion is a value, and a bounded node's seam covers that leg too. The node-counting pre-pass
  that residual called for turned out to be unnecessary.

  Signature change: `decode_into(input, std::pmr::memory_resource&)` →
  `decode_into(input, tr::mem::block_source_t&)`; `tlv_arena_t`'s constructor likewise.
  `fwd_router_t` gained a third **appended** parameter (`rx`, defaulted) for the arena
  source, so existing call sites are unchanged. `grammar::walk_stack_t`'s constructor
  also changed its spill parameter from `std::pmr::memory_resource*` to
  `mem::block_source_t*` — source-breaking for anyone driving the grammar walk directly.

  **Faster, not slower**: the terminus decode measures **236 ns vs 241–251 ns** on `main`
  (interleaved, best of 3), and **97 ns vs 114 ns** on an isolated decode loop. Getting
  there required `push_slot()` — see below.

- **A PATH whose child is not a NAME answers `INVALID_PATH` instead of being silently
  rewritten (#436).** `op_resolve`'s lookup-key builder accepted any child TLV and emitted
  it as if it were a NAME, so a malformed address resolved to *some* vertex rather than
  being rejected — and a WRITE could `mkdir -p` a path the spec does not permit. The
  rejection now happens before the RFC-0005 write-creates branch. Conformance vector
  `path/path-value-children-illegal` pins it.

- **`:subscribers` and `:children` are addressed WHOLE — a trailing step no longer acts
  (#580, #581).** `field_write` gated both branches on the FIRST step alone, so every
  deeper selector fell through to the branch's action and answered `RESULT`:
  - `:subscribers[0].liveness.last_seen_ns` (or any tail, any depth) reached the
    unconditional `clear_edge` and **destroyed the slot** — a caller aiming at a member
    unbound a live subscriber and was told it succeeded, with a reply byte-identical to a
    legitimate `[0]` clear.
  - `:children[].bogus` created the child exactly as the sanctioned `:children[]` does,
    silently discarding the tail. On `/net` that spelling built a live connection vertex
    and wired it into the router.

  Both now answer `SCHEMA_NOT_FOUND`, resolved before the ACL gate exactly as `:acl`
  already was. This makes WRITE agree with READ, which already required
  `steps.size() == 1` on both surfaces — no new rule is invented. The legal shapes are
  untouched: `:subscribers[]`, `:subscribers[N]` and `:children[]` behave exactly as
  before (the guard deliberately does NOT use `plain_step`, since an append is
  `append == true` and a clear is `indexed == true`).

- **FWD FIELD selector: a malformed `index_mode` byte now rejects with `INVALID_PATH` (#437).**
  `selector_to_field` (`op_resolve_walk.hpp`) switched on the decoded `index_mode` (RFC-0004 §C:
  `SCALAR`/`ELEMENT`/`WILDCARD`) with no `default` case, so a wire byte outside `{0,1,2}` fell
  through and **silently dropped the decoded index** (the step resolved as a plain non-indexed
  scalar) instead of erroring. It now returns `INVALID_PATH`, matching the sibling `kMaxFieldDepth`
  guard and the wildcard-reject path. (Bracket handling in `path::has_reserved_char` — the other
  half of #437 — is intentionally deferred, coupled to address-index parsing; left as-is.)

- **`route_handle_t::clear_link` reclaims a departed link's table shell (#488).** The
  per-link compaction registry (`links_`) was insert-only: `clear_link` emptied a link's
  tables but retained the empty `link_tables_t` shell for the object's lifetime, so a
  `delivery_compact` workload cycling through many distinct link names (e.g. a bus link that
  names peers `<ip>:<port>` and never reuses a name) accumulated one shell per distinct name.
  The registry now owns each link's tables through a `std::shared_ptr`, so the accessors hand
  out a pinning copy and `clear_link` can *erase* the map entry — the node is destroyed only
  once the last outstanding reference drops, preserving the reference-stability invariant that
  made the map insert-only (no use-after-free of a table a concurrent `ensure_egress`/
  `bind_ingress` is mid-write on) while bounding `links_` to live link names. The public label
  API is behavior-unchanged; a churn-of-distinct-names case plus a concurrent
  writer×`clear_link` case (TSan) were added.

## [0.6.0] — 2026-07-23

### Fixed

- **A SUCCESS reply whose assembly OOMs now replies an addressed `BACKPRESSURE`, never a
  silent drop.** A composed-root READ (`read_subtree_folded`, RFC-0005 §C) serves a folded
  hundreds-of-links snapshot; the fold itself is nothrow-guarded, but it can SUCCEED and
  then the reply builder's own link-table reserve — a second large contiguous block, one
  link larger — fail on a fragmented heap. `op_resolver_t::resolve`'s success sites
  previously returned that empty rope, which `fwd_router_t`'s `link_count() == 0` guard
  dropped with NO reply at all; a WebSocket client read the missing reply as a dead session
  and churned teardown+redial (each redial re-primed the same failing snapshot, so the page
  stayed wedged — and the churn fed the dead-peer accounting). The reply builder now (a)
  returns an empty rope on a head-segment alloc failure instead of a headerless/malformed
  frame, and (b) wraps every READ/WRITE/AWAIT/`:subscribers[]` success reply in
  `or_backpressure`, turning an empty (OOM) reply into an addressed
  `STATUS{ ERROR{ VALUE tr::flow::backpressure } }` the client falls back on over the same
  link. The `BACKPRESSURE` reply's only allocation is a 14-byte head segment, so it succeeds
  on exactly the heap that could not reserve the snapshot. Non-OOM replies are byte-identical.
  `graph_oom_softfail_test` gains a composed-read case that fails without the wrap.

- **The residual writer-thread store/delivery allocations are nothrow soft-fail (#477) —
  drop or `BACKPRESSURE`, never `abort()`.** The 2026-07-22 storm HIL reproduced the
  #453/#454 failure class on the ENGINE task: a throwing `operator new` reachable from the
  local write path (`store_value` → fan-out → dispatch) aborts the node under the MCU
  profile's `-fno-exceptions` on a heap-exhausted allocation. Per the d352998 discipline
  (store legs report status, delivery legs drop): `vertex_t::store`'s LKV
  control-block+rope allocation soft-fails (`store_value` maps it to
  `status_t::BACKPRESSURE`; the handler-role null-`shared_ptr` "consumed" sentinel is
  unaffected); a STREAM's ring append is shed on OOM (bounded-lossy history — the LKV
  still publishes); `drain_unflushed` DEFERS its batch (cursor kept — redelivered next
  sweep) instead of a throwing snapshot; the wide fan-out edge snapshot degrades to the
  inline prefix and skips an uncloneable edge; a spilled (>2-link) value's target-edge /
  handler-notify delivery clone drops that leg; the sweep-set legs
  (`mark_pending`/`clear_pending`/`propagate` and the branch-write plan) render keys and
  grow nothrow (branch-write OOM ⇒ `BACKPRESSURE`); and `fwd_router_t::deliver_remote`
  drops a delivery whose iov table, flatten, or COMPACT/ADVERTISE frame cannot allocate.
  Success paths are byte-identical.

### Added

- **`tr::net::try_encode_advertise` / `try_encode_compact` — nothrow forms of the
  route-handle frame encoders (#477).** Build the frame into a caller vector and return
  `false` on OOM (the writer-thread delivery egress drops instead of aborting); the
  throwing forms stay for setup-time callers. New `tr::detail` nothrow growth primitives
  beside `try_reserve`/`try_push_back`: `try_assign(std::vector<std::byte>&, span)` and
  `try_assign(std::string&, string_view)` (soft-fail copy-assign), plus the test-only
  OOM-injection seam `tr::detail::probe_fail_hook` consulted by `probe_bytes` — the
  global-heap twin of the failing-`mem_backend_t` injection `graph_value_backend_test`
  uses; `graph_oom_softfail_test` exercises every converted class through it.

- **`tr::net::transport_tcp_server` — the multi-peer raw-TCP listener (the board↔board
  default).** The transport_ws_server slot/poll machinery (#362) over the shared u32-LE
  length-prefix stream framing: ONE poll thread accepts and serves N concurrent peers
  (slots recycled on departure; `max_peers` is the RFC-0006 injected admission cap), each
  frame reassembled per-slot by a chunk-fed `length_prefix_framer` into one owning segment
  from the injected `mem_backend_t` (ADR-0042). With `peer_named` the ADR-0044 `bus_link_t`
  facet tags inbound frames per peer (`<ip>:<port>`), resolves directed per-peer links
  (span and zero-copy gathered sends), and evicts a departed peer's edges
  (`notify_peer_down`, RFC-0009 §D.5); FLAT mode keeps point-to-point semantics. The
  built-in `kind=tcp` LISTEN factory now constructs this server (a single-client
  deployment behaves exactly as the one-peer listener always did) and parses the two
  ws-mirrored kind-private keys `peer_named` / `max_peers` (ADR-0043 §5). No HTTP
  upgrade, no frame masking — leaner than WS packaging for device↔device links.
  `tcp_transport_t`'s DIAL and one-peer LISTEN modes are unchanged.

- **`graph_t::subscribe(...)` now returns a `subscription_t` handle, and `graph_t::unsubscribe(handle)`
  removes one in-process subscription.** The callback-form `subscribe` overloads return an opaque
  `subscription_t` (`{vertex, slot}`) instead of `void`; `unsubscribe` is the host-SDK-sugar
  counterpart of the wire `:subscribers[N]` clear (ADR-0049) — it deactivates the edge slot and
  unwinds the RFC-0005 listener bookkeeping via the same `note_subscriber_removed` path the wire door
  uses, so both leave identical counters. Only DEACTIVATES: an in-flight delivery already snapshotted
  the edge (ADR-0041 §2) and completes, so the callback `ctx` must outlive it. Fills the "the substrate
  has no local edge removal" gap (link-eviction was remote-only; retire is vertex-wide) that blocked
  per-subscriber unbinding for host consumers. Source-compatible: existing callers use `(void)` or
  `.has_value()`, both valid on the new return type.

- **`mem::sync_pool_t` — a thread-safe `pool_t` for the graph's `value_backend_` on a
  multi-core host (ADR-0060 §2).** `value_backend_` must be thread-safe: a `segment`
  self-routes its reclaim on whatever thread drops the last ref (a reader/subscriber),
  concurrent with a writer's `alloc`, and `pool_t` is single-threaded-reclamation.
  `sync_pool_t` guards the O(1) free-list with a spinlock — the multi-core-host variant
  (`is_isr_safe == false`), avoiding the ~2 µs OS-mutex round-trip that would dominate
  the ~120 ns op. It composes over `pool_t` and re-points each fresh segment to itself
  with an `UNKNOWN` tag, so reclaim routes through the locked virtual `destroy` rather
  than the devirtualized POOL fast path. The single-core interrupt-disable crit-section
  and the many-core lock-free index+tag CAS variants are the recorded ADR-0060 §2
  follow-ups. New bench series `syncpool-mt{1,2,4,8}` / `heap-mt*` track the contended
  path; a `mem_sync_pool_test` proves it TSan-clean under contended + cross-thread reclaim.

## [0.5.0] — 2026-07-21

### Added

- **`graph_t` gains a second injected memory seam — the ADR-0060 write-path value
  byte-buffer backend.** The constructor takes an optional
  `mem::mem_backend_t* value_backend` (defaulted to `mem::heap_backend()`, so the
  full signature is now `graph_t(std::pmr::memory_resource* mr =
  get_default_resource(), mem::mem_backend_t* value_backend = &heap_backend())`).
  The write-path copy-store — the single `materialize()` flatten of a branch or
  field write (`graph.cpp` sites 825/1017), the last unpoolable allocation on the
  write hot path — now draws its owned value `segment` from `value_backend` instead
  of the default heap. A bounded host points it (and the ADR-0042 transport-receive
  backend) at one `pool_t` slab for deterministic, fragmentation-free value memory;
  passing nothing keeps the standard heap and behaviour is **byte-identical**. On
  pool exhaustion / oversize the write rejects with `status_t::BACKPRESSURE` rather
  than silently falling back to the heap (ADR-0060 §3). An injected `value_backend`
  **must be thread-safe** — a value `segment` self-routes its reclaim on whatever
  thread drops the last ref (§2); the default heap already is. New coverage:
  `graph_value_backend_test` (routing / read-back / backpressure / default-idle) and
  the `bench/` `lkv-alloc-*` / `lkv-store-*` series gated by `perf_gate.py`.

### Changed

- **A plain `read` of a vertex with ≥ 1 registered child now serves the COMPOSED
  BRANCH READ** (RFC-0005 §C follow-on) — the folded POINT tree of the target's
  registered subtree, a view over the live last-known-value ropes (not a copy):
  `composed(target) = POINT{ [stored TLV of target]?, child_node* }`,
  `child_node(c) = POINT{ NAME(c), [stored TLV of c]?, child_node(grandchild)* }`. Each
  node's value is that vertex's stored TLV **verbatim** (the landed LKV bytes, opaque —
  a non-VALUE TLV such as a STATUS composes as-is; descendant HANDLER `on_read` seams
  are **not** invoked). A vertex the caller may not READ **prunes** its whole subtree
  (siblings unaffected); unregistered placeholders are skipped as in `:children[]`;
  synthesized `on_children` transport listings are not graph children and are absent; a
  value-free branch serves a names-only topology tree instead of `NOT_FOUND`. Leaf reads
  and HANDLER-target reads (`on_read` precedence, `NOT_FOUND` without one) are
  **byte-identical to before**; field reads (`read(v, field, caller)`) are unchanged.
  The new public composer `graph_t::read_subtree_folded(vertex_handle_t,
  string_view)` builds the reply as a scatter-gather rope — per-node owned POINT
  headers, borrowed NAME records, refcount-cloned LKV links, zero flatten
  (`subtree_read_test` gates the differential, round-trip, prune, regression, and
  link-accounting batteries).

- **The composed-reply path is now NOTHROW / soft-fail — a heap-exhausted reply is
  DROPPED, never an `abort()`.** Assembling a large composed `read` reply (the folded
  POINT tree of RFC-0005 §C) on a fragmented heap used to grow several throwing
  `std::vector` / rope transients whose `std::bad_alloc` is an `abort()` under the MCU
  profile's `-fno-exceptions` — observed as an ESP32-C6 reboot when the ~12.7 KB
  composed `/` reply was built low on memory. Every residual throwing allocation on the
  reply path is converted to a nothrow soft-fail that drops the reply as
  `status_t::BACKPRESSURE` (a dropped reply is already valid — the client retries), with
  **no wire-behavior change** on the success path (byte-identical, `subtree_read_test` /
  `folded_children_test` stay green): the `read_subtree_folded` pre-order `nodes` / work
  `stack` growth (nothrow `try_push_back`), its per-node POINT header (emitted by cursor
  into a nothrow `heap_alloc`, retiring the throwing `std::vector<std::byte>` +
  `emit_header`), the reply rope's `heap_` chain spill (pre-`try_reserve`'d to the exact
  final link count), the FWD{REPLY} `assemble` head+payload chain (pre-`try_reserve`'d;
  an OOM yields an empty rope the router drops), and the `resolve_terminus` /
  `resolve_terminus_rope` egress span table (`try_to_iovec`). This finishes the residual
  half of the production-node OOM crash begun by the `assemble_result_rope` link-table fix.

### Added

- **Zero-copy scatter-gather WebSocket SERVER egress (`tr::net::transport_ws_server`).**
  The WS server path now makes libtracer's "zero-copy everywhere" claim true on egress:
  server→client frames are UNMASKED (RFC 6455 §5.1), so the frame header and the payload
  spans go to the wire in one gathered `sendmsg` with no flatten and no re-copy. Previously
  `send(iov)` fell back to the base `transport_t` default (flatten the rope into one buffer,
  then `encode_frame` copied again to prepend the header = 2 allocs + 2 copies per send).
  New public API:
  - `transport_ws_server::send(std::span<const std::span<const std::byte>>)` — overrides
    the base scatter-gather default; encodes the frame header once and fans
    `[header, span0, span1, …]` to every open peer via one gathered write per peer (each
    peer writes from a fresh copy of the iovec array, since the write consumes it).
  - `transport_ws_server::peer_endpoint_t::send(std::span<const std::span<const std::byte>>)`
    — the directed single-peer twin (one consumer, no per-peer copy).
  - `tr::net::ws::encode_frame_header(std::array<std::byte, kMaxServerFrameHeader>&,
    opcode_t, std::size_t, bool = true)` plus the `kMaxServerFrameHeader` (10) bound —
    the extracted header-only length encoder now shared by `encode_frame` and the
    scatter-gather egress (one length-encoding implementation). The MASKED client path
    (`transport_ws_client`) is unchanged (it stays on the base flatten default — a client
    XORs every payload byte, so its bytes cannot ride uncopied). The shared stream
    scatter-gather writer moved to `stream_endpoint_t::write_all_iov` (tcp now uses it too).

- **`rope_t` nothrow soft-fail growth API (`tr::view::rope_t`).** For assembling a
  scatter-gather reply on a constrained/fragmented heap without an `-fno-exceptions`
  `abort()`:
  - `[[nodiscard]] bool rope_t::try_reserve(std::size_t links) noexcept` — reserve the
    heap chain for `links` more `append`/`concat` links; after success those appends are
    guaranteed non-reallocating (hence nothrow). Returns `false` (rope untouched) on OOM
    or an impossible count.
  - `[[nodiscard]] bool rope_t::try_to_iovec(std::vector<std::span<const std::byte>>&)
    const noexcept` — the nothrow form of `to_iovec` (one zero-copy span per link);
    returns `false` (output left empty) when the span table cannot be grown. The throwing
    `to_iovec()` is unchanged for existing callers.
  - `tr::detail::try_reserve(std::vector<T>&, std::size_t)` /
    `tr::detail::try_push_back(std::vector<T>&, T&&)` (in `mem_heap.hpp`) — the generic
    nothrow `std::vector` growth primitives both rope methods and the composed-read
    collection stacks build on (probe with `operator new(..., std::nothrow)`, then the
    throwing grow only once the allocation is known to succeed).

- **Subscriber-edge eviction on peer departure (RFC-0009 §D, extended to link
  teardown) + edge-slot reuse.** Nothing evicted a departed link's subscriber edges:
  every browser session left its remote edges ACTIVE in every write fan-out (~27 KB
  and ~70 orphaned edges per session measured on the C6), a redialed peer never
  matched its old edges (new ephemeral port in the name), and `add_edge` never
  reused a freed slot. New public API:
  - `graph_t::evict_link_edges(std::string_view link_name)` — deactivates **and
    reclaims** (releases route/link/caller state in place, slot shell kept) every
    subscriber edge whose stored link is `link_name`, unwinding the RFC-0005
    listener counters; returns the evicted count. Slot indices of survivors never
    renumber (§D.2); local edges and other links are untouched.
  - `vertex_t::add_edge` now **reuses the first inactive slot** before growing
    `subs_` (a §D.2 "cleared slot MAY be reused" append); in-flight deliveries are
    unaffected — their `edge_view_t` snapshots own copies/refcount clones.
  - `fwd_router_t::link_down(std::string_view)` — the link-departure hook: graph
    eviction + `clear_link` label-state drop, in that order.
  - `transport_t::set_down_notifier` / `bus_link_t::set_peer_down_notifier` — the
    teardown seam `fwd_router_t::add_child` now installs on every child;
    `transport_ws_server` (peer-named and flat), `transport_ws_client`,
    `tcp_transport_t` (both roles) and the ESP-IDF `httpd_ws_link_t` fire it when
    a session dies. Connectionless kinds (UDP) and announce-census buses (CAN)
    have no closure event and never fire it.

- **`graph_t::retire(vertex_handle_t)` — owner-facing vertex retirement (#407,
  RFC-0009 §B/§C/§E.6)** — the mirror of `register_vertex`. Marks a vertex and its
  whole subtree **logically absent**: invisible to `find` / `read` / `:children[]`,
  reading `tr::path::not_found` exactly like a never-built path. The allocation is
  **not freed** and every `vertex_handle_t` stays dereferenceable (ADR-0057
  insert-only — the vertex is *emptied*, never erased). Retirement **re-virginizes**
  each vertex: it clears the previous owner's `:acl`, value seam, stored value,
  history, app-field table, subscribers, settings, and delivery mode, so a later
  write-creates revive of the same address inherits **nothing** — in particular the
  revived path inherits its live ancestor's ACL policy, never the retired owner's
  (an ACL does not survive churn). `write_seq_` survives (monotonic per address).
  Retirement delivers nothing and wakes no `await`; it is idempotent; the root cannot
  be retired. There is **no wire operation** that reaches it — a peer goes through the
  device's own logic (RFC-0009 §A.1).
  Two defects a naive `registered_ = false` would have shipped are closed by
  construction, and pinned by `retire_test` (including a ThreadSanitizer read-vs-retire
  race): a **confused-deputy** (the retired owner's ACEs stayed live to the gate — the
  effective-ACL walk climbs on `has_own_aces()`, not `registered()`) and a
  **use-after-free** on the value seam (read lock-free, so retirement swaps the pointer
  atomically and **parks** the old block rather than freeing it under a concurrent
  reader).
- **`vertex_t` value-seam pointer is now atomic (`std::atomic<value_handlers_t*>`)** —
  the `on_read`/`on_write`/`on_children` group was a `unique_ptr` read lock-free under a
  "set once, never changes" contract. Retirement needs to clear it, so it became an
  atomic published with `store(release)` and swapped with `exchange(acq_rel)`; a
  swapped-out block is parked in `vertex_ext_t::retired_handlers` and reclaimed only by
  the destructor. The `handlers()` accessor is unchanged (still returns a stable
  `const value_handlers_t&`); no call site changed. Cold-block (`vertex_ext_t`) growth
  only — `sizeof(vertex_t)` is unaffected.
- **Node identity facet: `graph_t::set_identity` / `clear_identity`, serving
  `read <vertex>:identity` (#406, RFC-0011, ADR-0045 decision 3)** — a node-scoped
  identity record, synthesized on read at **every** vertex (the `read_schema`
  pattern: zero stored per-vertex bytes). `set_identity(kind, key)` installs the
  RFC-0011 §B record — `SETTINGS{ NAME "kind" VALUE u8, NAME "key" VALUE <key> }`,
  60 bytes for `kind=0x01` (ed25519) — pre-serialized at install so every vertex
  returns **byte-identical** bytes by construction; that invariant is what makes the
  record a valid **cross-path key**, letting a client prove `/b` and `/c/a/b` are one
  device (ADR-0044 pt 3: the core never dedups; the client does, keyed by an identity
  it chooses). A kind outside the registry, or a key length contradicting the kind,
  is rejected `TYPE_MISMATCH`; `clear_identity()` returns the node to the keyless
  surface.
  **The read is pre-auth** (RFC-0011 §C.2) — a narrow, named exemption from the READ
  gate applying to `:identity` alone, because the public key is precisely what an
  unauthenticated peer must obtain to TOFU-pin and to verify the ADR-0045 challenge;
  gating it would deadlock first contact against the closed default ACL. There is
  **no write surface** — the identity is the owner's, installed locally.
  **The record is served whole** (RFC-0011 §C.4): the entire `identity` field namespace
  resolves above the READ gate, so `:identity.key` (member addressing) and
  `:identity[0]` (indexed addressing) are `SCHEMA_NOT_FOUND` for **every** caller —
  sub-addressing buys nothing and costs a resolver branch on MCU-class nodes. Resolving
  the whole namespace, not just the bare spelling, is what makes that refusal
  caller-independent as §C.4 requires; it discloses nothing, since the record itself is
  world-readable by design.
  **No crypto is involved:** the facet stores and serves a *claim*; proving a node
  holds the key is authentication and lives elsewhere. A node without a keypair keeps
  today's `SCHEMA_NOT_FOUND` byte-for-byte (the surface is absent, not empty).
- **ws-private `peer_named` / `max_peers` connection-config keys (#408, ADR-0043 §5,
  ADR-0044)** — the built-in `ws` factory now parses two LISTEN-side kind-private keys
  from the SPEC's raw config TLV (as `quic` does for `cert`/`key` and `can` for
  `ifname`/`node`); neither lands on the shared `conn_settings_t`, and both are ignored
  on a DIAL. `peer_named` (`VALUE` u8, nonzero = true; default **false**, so existing
  behaviour is unchanged) constructs the server with the ADR-0044 `bus_link_t` facet, so
  each inbound peer gets its own return-route identity and `make_connection` installs the
  synthesized `:children[]` peer listing on the connection vertex. `max_peers` (`VALUE`
  u32; default 0 = unbounded) is the concurrent-peer admission cap. Previously a
  SPEC-created ws listener was *always* constructed `peer_named=false`, so its `bus()` was
  null and ADR-0044's peer enumeration was reachable only by direct construction +
  `provide_link` — i.e. not at all to the in-band creator (a web UI forming a link on a
  remote device) that ADR-0027 exists for. Non-wire (a kind-private config key, not a
  protocol change), no RFC.
- **First-level-child existence test: `graph_t::has_first_level_child`
  (#373)** — a placeholder-inclusive predicate (`std::span<const std::byte>` NAME
  record → `bool`) reporting whether the graph root has a top-level child by that
  name, whether it is a registered vertex or a mere structural placeholder (unlike
  `find`, which excludes placeholders). The transport plane uses it to reject a
  child-link name that would shadow a first-level subtree: a FWD's first `dst`
  segment resolves against the child-link registry before the local graph, so a
  link named e.g. `system` otherwise black-holes every `/system/...` read onto the
  transport. Non-wire, no RFC.
- **Borrowed (static-storage) app-field install: `graph_t::set_app_fields_static`
  (#388, ADR-0058)** — a non-owning overload beside the owning
  `set_app_fields(std::vector<app_field_t>)`, taking
  `std::span<const app_field_static_t>` whose `name`/`descriptor` are
  `std::string_view`/`std::span` **views into caller storage that must outlive the
  vertex** (pass pointers into flash/`.rodata`). An MCU owner whose field table is
  `constexpr` in flash now installs it at **zero declaration RAM** — the runtime
  stores views, never a heap copy of `.rodata`. The owning overload stays the safe
  default for runtime-formed tables. Wire-invariant (`:schema` serves the same
  verbatim bytes either way), so no RFC.
- **Slim `transport_vertex_t` ctor: the `slim_net` tag overload** — a fifth
  constructor parameter (`slim_net_t`, via the `slim_net` inline tag) selects a
  `transport_vertex_t` that registers the `client`/`listener` catalog types under
  `/net` but omits the built-in udp/tcp/ws factory auto-registration. Because the
  slim ctor's translation unit never names `register_builtin_transports`, a node
  that binds its links directly (`provide_link`) sheds the unbound socket
  transports under `--gc-sections` — a flash trim for slim device/VB nodes — and
  re-adds exactly the factories it wants via `register_transport_type`.
  Non-breaking: the default full-node ctor is unchanged; only a node opting in
  with the tag sheds the builtins. Non-wire, no RFC.

### Fixed

- **Departed QUIC / WebTransport peers now evict their subscriber edges (RFC-0009 §D.5,
  closing the [#453](https://github.com/avatarsd-llc/libtracer/issues/453) eviction
  gap — [#455](https://github.com/avatarsd-llc/libtracer/issues/455)).** The
  connection-oriented ws/tcp/esp transports fire `transport_t::notify_down` on peer
  departure, but the `quic` and `webtransport` modules never did — so a departed
  QUIC/WebTransport peer's remote subscriber edges leaked (stayed ACTIVE in every write
  fan-out). The shared `msquic_endpoint_t` base now fires the outer transport's link-down
  notifier from the msquic connection-shutdown callback (and the one-peer replacement
  harvest that displaces an old peer), guarded by a `stopping` latch so our OWN teardown
  never fires it and deduped by a `link_established` latch so it fires at most once per
  established connection (a link that never came up, and the trailing `SHUTDOWN_COMPLETE`,
  do not fire). Both kinds are point-to-point (one peer at a time), so a departure is the
  whole link down (`notify_down`, not the multi-peer bus facet's `notify_peer_down`).
  No public-API change (the notifier seam is #453's) and no wire change; module-only —
  exercised by the `quic` CI job (msquic).

- **A read reply's link table is no longer heap-copied per reply (OOM-abort hardening,
  on-device crash)** — `assemble_result_rope` staged the stored payload rope's links
  through a fresh `std::vector<view_t>` before handing them to `assemble`, a per-reply
  transient that scaled with the value's link count (~9 KB for a composed-root read) and,
  on an `-fno-exceptions` MCU where the throwing `std::allocator` has no failure path,
  turned heap exhaustion into `abort()` mid-request (decoded on-device: 3/3 browser-session
  crashes on the httpd task). `assemble` now takes
  `std::span<const view_t>` and borrows the payload rope's own link span — zero staging
  allocation. Internal helper (`core/src/`), no public-API change. The companion
  `integrations/esp-idf` fix makes the WS link's TX/RX buffers nothrow end-to-end
  (gather-once into the queued work item, drop-on-OOM backpressure per the existing
  `note_tx_result` contract) instead of aborting. Known residual throwing transients
  on the same reply path (follow-up): `rope_t`'s heap-spill link vector,
  `rope_t::to_iovec()`'s span vector at the router boundary (`fwd_router.cpp`
  `resolve_terminus`), and `assemble`'s headerless-degrade when the nothrow header
  alloc fails.

- **Gated reads no longer fail-open while an `:acl` rewrite is in flight**
  — the ADR-0050 effective-ACE cache cleared its dirty flag at the *entry* of the lazy
  rebuild (`vertex_t::with_effective_aces` did an `exchange(false)` before the fresh merge
  was published), so a second reader arriving during the unlocked rebuild window saw
  `dirty == false` and evaluated a not-yet-populated (empty ⇒ open-by-default) merge —
  momentarily **allowing a caller that should have been denied**, on every gated op. The
  rebuild is now **generation-gated**: a new per-vertex `acl_gen` counter is bumped — ahead
  of the dirty flag — by every invalidator (`set_acl`, `mark_acl_cache_dirty` for the
  ancestor-subtree fan-out, and the placeholder revert); the rebuild snapshots it before the
  unlocked ancestor walk and, back under the stripe lock, **publishes AND clears only when
  the generation is unchanged**, otherwise discarding the (stale or torn) merge and retrying.
  So `dirty == false` under the lock now always means `eff_aces` holds a *current* published
  merge, a slow rebuilder can never clobber a fresh merge a faster one published (a
  *persistent* fail-open), and no `:acl` mark is ever lost (no stale-forever cache).
  Reproduced at ~10% on a 2-core ASAN build; a new `effective_acl` subtest races
  different-valued ancestor rewrites (which the prior same-grant storm could not surface).
  0 failures after across ASAN/TSAN and the full suite (#425).
- **`write <vertex>:acl.<anything>` and `write <vertex>:acl[N]` no longer REPLACE the
  vertex's entire ACL** — the `:acl` write branch matched on the name alone, with no
  step bound and no selector check, so every unresolved shape fell through to
  `set_acl()`, which replaces wholesale. A caller holding `WRITE_ACL` who typoed the
  field name, or emitted a stray selector, **silently swapped the vertex's whole
  access-control list for the payload** — and if the new list did not grant them,
  locked themselves out. Now `SCHEMA_NOT_FOUND`, resolved before the gate. The ACL is
  addressed whole; an ACE is not separately writable.
  **Not a privilege escalation** — the `WRITE_ACL` gate always ran, so only a caller
  already entitled to replace the ACL could trip this. It is an integrity and
  typo-safety fix. Reads are bounded to match: `:acl[N]` and `:schema[N]` served the
  *entire* record under an OK status and are now `SCHEMA_NOT_FOUND` (the schema is one
  synthesized POINT, not an array).
  Found by a systematic sweep for the defect class behind the `:settings` and
  `:identity` fixes below: *a field branch that matches on NAME but not SHAPE.*

### Changed

- **A `write <vertex>:settings.<knob>` now resolves the knob NAME *before* the `:acl`
  WRITE right, and rejects a knob path that is not exactly two plain steps (#373
  follow-up; `docs/reference/05` §`0x0B` validation).** The §`0x0B` rule — "unknown
  NAMEs MUST be ... rejected with `ERROR{tr::schema::not_found}` if in the core
  namespace" — carries **no caller qualifier**, but the flat-knob branch gated on the
  ACL first, so a caller lacking `WRITE` received `PERMISSION_DENIED` for a knob that
  does not exist. The `settings.app.` branch already resolved names first (an
  undeclared field is ENOTTY before the ACL right) and the terminal branch has no gate
  at all; all three now agree.
  **Two interop-visible behavior changes**, both narrowing what is accepted:
  1. `write :settings.<unknown>` is now `SCHEMA_NOT_FOUND` for **every** caller
     (was `PERMISSION_DENIED` for a caller without the `WRITE` right).
  2. `write :settings.<knob>.<tail>` and `write :settings.<knob>[N]` are now
     `SCHEMA_NOT_FOUND`. **They previously succeeded**, silently ignoring the tail or
     selector and writing `<knob>` — accepting a shape the read surface has always
     rejected. No knob has a nested or indexed surface.
  **Accepted cost:** a caller without the `WRITE` right can enumerate flat knob NAMES
  by probing. Knob names are a fixed, published constant of the protocol, not a
  per-node secret, and app fields already leak their names the same way.
- **In-process SUBSCRIBER delivery now terminates at its target (ADR-0051 /
  RFC-0007); the dispatch-depth cap `kMaxDispatchDepth` is deleted with no
  replacement.** A delivery into a target vertex applies exactly the target-local
  effects of a write — store (LKV/history per role), `await` wake, and the target's
  own handler reaction — gated by the target's `WRITE` `:acl`, and **never**
  re-dispatches to the target's own `:subscribers[]`. Propagation past a target is
  exclusively the target's own logic (a controller re-emits on its execution; a
  handler re-emits when it chooses), so a dispatch-level subscription cycle is
  impossible **by construction** — no cap, no dedup, no drain queue, and the
  internal `depth` parameter threads out of the graph runtime entirely.
  **Behavior change (interop-visible):** chained plain-vertex subscriptions
  (`A→B`, `B→C`) no longer relay `A`'s writes onward to `C` — the write is still
  *stored* at `B`, but `B`'s subscribers are not notified. Migration: subscribe the
  final consumer directly to the source (subtree subscriptions make this cheap), or
  make the intermediary a `HANDLER` that re-emits. Suspicious topologies (cycles,
  dead relay chains) are a design-time analyzer/reconciler concern, never runtime
  enforcement.
- **`vertex_ext_t` value-seam handlers and app-field table are now lazily-allocated
  groups (#388, ADR-0058 Step 2)** — the value seam (`on_read`/`on_write`/
  `on_children`) moved behind a `std::unique_ptr<value_handlers_t>` (HANDLER-role
  only) and the RFC-0010 table + its `on_app_field_write` apply seam behind a
  `std::unique_ptr<app_field_group_t>`. A plain STORED_VALUE or app-field-only
  vertex no longer pays the ~96 B of value-seam `std::function`s the extension
  block carried inline for every role. `on_app_field_write` moved out of the
  stored handler block into the app-field group (it co-occurs with the table, not
  the value seam); the public `handlers_t` install input is unchanged (still its
  four seams — `graph_t::register_vertex` splits them). `acl`, `settings`, and
  `history`/`last_flushed_seq` stay inline (the effective-ACL cache is stored per
  gated vertex, so the ACL group is not sheddable — see ADR-0058). Behaviour and
  wire output unchanged.

- **RFC-0010 app-field storage is now view-slots + a lazy value store (#388,
  ADR-0058)** — the resident table dropped from an owning `std::vector<app_field_t>`
  (each ≈ 88 B, plus a per-field descriptor allocation) to a `std::vector` of
  view-shaped slots (`{string_view name; app_access_t; span descriptor}`). The
  owning `set_app_fields` copies the runtime table's declaration bytes into **one
  backing buffer** (one allocation for the whole table, not N); field **values**
  moved to a separately, lazily-allocated index-keyed store that stays null until
  the first field write — a declared-but-never-written table costs zero value RAM.
  The `:schema`/settings read surface (`app_fields_snapshot()` and the emitted
  bytes) is unchanged.

- **STREAM history ring is lazily allocated (#388)** — `vertex_ext_t::history`
  is now a pointer-to-deque, allocated on the first STREAM append: an empty
  libstdc++ `std::deque` allocates its ~512 B map node at construction, which
  every extension-bearing vertex (handlers, RFC-0010 app fields, `:acl`,
  non-default settings) paid despite only the STREAM role using the ring.
  Measured: a leaf with a 5-field app table drops 1664 → 1008 live B (64-bit
  host); the new `vertex_app5` heap-probe row tracks this economy in CI.

- **`path_key_t` is now a small-buffer type (#380 §2)** — records ≤ 16 B (names up
  to 12 characters) live inline; a named vertex no longer allocates a ~32 B heap
  block for its NAME record (per-leaf live heap 160 → 136 B in the steady-heap
  probe). The `bytes` member became a `bytes()` span accessor; construction from a
  span or byte vector copies. Move-only spill ownership; moved-from keys read empty.

- **Subscription-edge wire state split to a lazily-allocated cold half (#380 §3)** —
  `subscriber_t` is now 80 B (was 160) and **move-only**: `return_route`, `link`,
  `caller`, and `delivery_compact` moved to `subscriber_remote_t` behind a
  `unique_ptr` that stays null for plain in-process edges (callback or local
  target under the empty caller context) — the common MCU wiring shape halves
  its per-edge RAM. Wire subscribers and caller-gated edges allocate the cold
  half at admission time (never on a dispatch path); `edge_view_t` (the
  dispatch snapshot) is unchanged.

- **Vertex child storage collapsed to one lazily-allocated sorted list (#380 §1)** —
  a leaf (the common MCU vertex) now pays exactly one null pointer for child
  storage instead of 40 B of inline slots + spill vector; `sizeof(vertex_t)`
  112 B (144 post-#377). `vertex_t::kInlineChildren` is removed; child
  enumeration (`for_each_child`, `:children[]` listings) is now always in
  sorted name-record order (previously insertion order for ≤2 children).

- **Stripe-lock hot path recovered to pre-stripe latency (#370)** — the #361 §2
  striped locks had cost ~10 ns on the 1:1 write. The stripe table is now
  `constinit` (guard-free lookup: `vertex_stripes` / `vertex_stripe_index`), the
  condvars moved to a separate cold table (`vertex_stripe_cv`), stripes are
  `alignas(64)` (no false sharing between adjacent stripes), and a **waiterless
  publish skips the condvar call entirely** (per-stripe waiter count, mutated
  only under the stripe mutex). Measured interleaved vs the pre-stripe build:
  parity on p50, better mean/throughput. `vertex_stripe_t` lost its `cv` member
  (use `vertex_stripe_cv(vertex_stripe_index(v))` — the stripe machinery is an
  implementation detail exposed in the header, not a supported API).

### Added

- **`can_tx_pool_t` — owned in-flight TX frame storage for asynchronous
  `can_link_t` backends (#383).** New header `can_tx_pool.hpp`: a
  fixed-capacity slot pool (non-blocking acquire, lock-free ISR-safe release)
  in which a link whose driver transmits asynchronously — queues the frame
  POINTER and formats it later, like ESP-IDF's `esp_driver_twai` — owns each
  frame until the driver's tx-done callback returns it. The
  `can_link_t::write_raw` contract now states the lifetime rule explicitly:
  `frame` is borrowed only for the duration of the call; an async
  implementation must copy into link-owned storage (the ESP component's
  `twai_link_t` was handing the driver `write_raw`'s stack — a use-after-free
  formatted from the tx-done ISR). The pool is mechanism-only; the FULL
  policy (bounded backpressure, then a counted — never silent — drop) lives
  in the owning link.

- **RFC-0010 owner app fields — the field descriptor table, `:settings.app.*`, and
  the two-part `:schema`.** New types `app_access_t` / `app_field_t` and the owner
  declaration API `graph_t::set_app_fields(vertex_handle_t, std::vector<app_field_t>)`
  (mirrored by `vertex_t::set_app_fields` and the `app_field_access` /
  `app_field_store` / `app_field_get` / `app_fields_snapshot` verbs on the #338 seam).
  `field_write` admits declared `:settings.app.<name…>` writes (owner always; remote
  callers per the declared `ro`/`rw`/`wo` access, then the vertex WRITE right — the
  RFC's gate order); reads serve stored bytes verbatim (`wo` has no read surface;
  declared-but-unset reads `NOT_FOUND`); undeclared names keep `SCHEMA_NOT_FOUND`.
  New read surfaces: bare `:settings` (protocol knobs + nested `app` record) and
  `:settings.app` (the app container). `read :schema` appends the owner part —
  `NAME "app" SETTINGS{…}` with the runtime-projected `access` member leading each
  field's descriptor, owner bytes verbatim. `handlers_t` gained the owner apply seam
  `on_app_field_write(name, value)`, fired outside the vertex lock after the store.
  Field writes still never wake `await` and never propagate (the RFC-0010 §C
  announce-write convention). Storage rides the lazy cold block: `sizeof(vertex_t)`
  unchanged (168 B x86-64, gate 192/128 holds); `vertex_ext_t` +56 B, and a leaf
  with no app fields (and no other ext trigger) still allocates nothing.

- **`graph_t` now takes an ADR-0039 §1 injected `std::pmr::memory_resource*`
  (#361 §5)** — `graph_t(mr)`, defaulting to `std::pmr::get_default_resource()`
  (zero churn for existing callers). Every `assign`'s LKV allocation (control
  block + rope, the per-write heap churn) draws from it; an MCU node installs a
  pool/slab resource over a static arena and per-write allocations stop
  fragmenting the global heap. `vertex_t::store` gained the corresponding
  defaulted `mr` parameter. Contract: the resource outlives the graph and every
  value handle obtained from it.

### Changed

- **Per-vertex `std::mutex` + `std::condition_variable` replaced by a process-wide
  striped lock table (#361 §2)** — `vertex_stripe_of(vertex*)` hashes the pinned
  vertex address into `LIBTRACER_VERTEX_LOCK_STRIPES` (default 16, a per-target
  compile definition) shared `{mutex, condvar}` stripes. `sizeof(vertex_t)` drops
  248 → 160 B on x86-64; on ESP-IDF this also removes the lazily-allocated
  per-vertex FreeRTOS mutex/condvar (~150–200 B heap per touched vertex) — the
  largest single on-device win of the diet. `await` waits on the stripe condvar
  with the per-vertex `write_seq_` predicate (a stripe collision costs a spurious
  wake + re-check, never a semantic change). `with_effective_aces` now snapshots
  the vertex's own ACEs and runs the ancestor-merge rebuild UNLOCKED (one stripe
  at a time, never nested) — required because an ancestor may share the stripe —
  with the same dirty-flag convergence guarantee.

- **ACL state evaluates at the nearest BEARING ancestor (#361 §3)** — a vertex
  with no own ACEs no longer builds or caches an effective merge (previously
  every gated descendant allocated its extension block and duplicated the
  merged ancestor list). `acl_allows` walks the immutable parent chain
  lock-free (new `vertex_t::has_own_aces()` atomic) to the nearest vertex with
  own ACEs and evaluates its cached merge through the `kAceInherit` projection
  (new `vertex_ext_t::eff_aces_inherit`, rebuilt with the merge) — which is
  exactly the descendant's effective list. Verdicts are unchanged; RAM stops
  scaling as ancestors × descendants. `with_effective_aces`'s `eval` now
  receives `(merged, inherited)`.

### Changed

- **`transport_ws_server` serves MANY concurrent inbound peers (#362)** — the
  `listen(fd, 1)` single-client-per-boot limit is gone. One poll-based thread
  multiplexes the listen socket and every peer (no per-peer thread). New
  constructor parameters `max_peers` (admission cap, 0 = unbounded) and
  `peer_named` (both defaulted — source-compatible). `send()` now fans out to
  every open peer. With `peer_named = true` the server exposes the ADR-0044
  `bus_link_t` facet: inbound frames are tagged per peer (`<ip>:<port>`), so
  each browser tab gets its own return-route identity, `peer_link(name)` gives
  a directed per-peer sender, and the connection vertex's `:children[]` lists
  the live peers. Default (point-to-point naming) behavior is unchanged.
  Handshake fix: bytes pipelined after the HTTP Upgrade header are now carried
  into the frame stream instead of dropped.

- **`vertex_t` hot/cold split (#361 §1)**: the cold, conditionally-needed members —
  `handlers_t`, the STREAM history ring, the `:acl` state + ADR-0050 effective-merge
  cache, non-default QoS `settings_t`, and the stream drain cursor — moved behind one
  lazily-allocated `vertex_ext_t` block (`new` `vertex_ext_t`, public in `vertex.hpp`).
  A default STORED_VALUE leaf allocates no block; `sizeof(vertex_t)` drops 536 → 248 B
  on x86-64. Behavior-preserving: `settings()` / `settings_snapshot()` return the shared
  `kDefaultSettings` (new public constant) when no block exists, and the first `:acl` /
  `:settings` write allocates transparently. `settings_t` gained defaulted `operator==`.
  A new `vertex_size_test` gates `sizeof(vertex_t)` at compile time per ABI.

## [0.4.0] — 2026-07-09

### Changed

- **`vertex_t::store` now returns the published `std::shared_ptr<const rope_t>`**
  (previously `void`) — the exact LKV pointer a concurrent `read_stored` observes.
  The eager write path delivers `*sp` instead of a pre-store rope clone (RFC-0008
  §D "deliver exactly what was stored"), removing one rope clone+destroy per write
  (`inproc 64B fan1` ~101→~92 ns/op). Migration: callers that ignored the old
  `void` result need no change; the return value may be discarded.

- **`vertex_t::snapshot_edges` now fills a `graph::edge_snapshot_t` (new type)
  instead of a `std::span<edge_view_t>` inline buffer.** The snapshot buffer is
  raw stack storage that placement-constructs ONLY the views actually
  snapshotted; default-constructing a `std::array<edge_view_t, 8>` per publish
  zeroed ~900 bytes of dead stack (GCC lowers it to eight `rep stos` blocks),
  a fixed ~18 ns/op on the fan-out hot path — the post-v0.3.0 `inproc 64B fan1`
  delivery regression (126→144 ns/op; back to ~105 ns/op with the fix).
  `vertex_t::kInlineFanout` is now an alias of `edge_snapshot_t::kCapacity`
  (same value, 8). Migration: declare `edge_snapshot_t buf;` where you declared
  the `std::array`; indexing and the overflow-vector contract are unchanged.

### Removed

- **`wire::view_as_tlv` — folded into a `wire::decode(const view::view_t&)`
  overload (frame.hpp).** The function was a pure alias (`return
  decode(v.bytes());`) that failed the deletion test. The overload keeps the
  identical contract (the L1↔L2 cast lives at L2 because it produces a `tlv_t`;
  the returned tree borrows the view's bytes, so keep the view — and its
  segment — alive) under the codec's own name. Migration: replace
  `wire::view_as_tlv(v)` with `wire::decode(v)`.

### Added

- **`length_prefix_framer::kDefaultMaxFrame` — one home for the 16 MiB default
  receive cap.** The default that applies when `:settings max_frame` is unset,
  previously duplicated as a literal `static constexpr kMaxFrame` in
  `transport_tcp.hpp`, `transport_quic.hpp`, and `transport_webtransport.hpp`.
  The three per-class `kMaxFrame` constants remain (tests and callers keep
  their spelling) but are now aliases of the shared default — same value, no
  behavior change.

- **`fwd_frame_view.hpp` — the FWD forward-plane offset-dispatch cluster as a
  public, unit-testable header (`tr::net`).** `fwd_hdr_t` / `read_fwd_header`
  (absolute-offset header reads over the one grammar, CRC deferred),
  `peek_fwd_first_dst_seg` / `peek_fwd_op` / `peek_control` (the forward-vs-
  terminus and control-frame dispatch peeks), `stack_writer<N>` (the zero-heap
  clamp-to-empty head builder, ADR-0038 inv. #2), and `fwd_rebuild_t` /
  `rebuild_fwd_forward` (the shrunk-dst / grown-src head rebuild +
  cursor-seam `gather`). Extracted from `fwd_router.cpp`, which now delegates
  mechanically — forwarded frames are byte-identical. Cursor-templated
  (ADR-0053 ④b), so span and rope sources share the identical logic; covered
  directly by `fwd_frame_view_test` (the `length_prefix_framer` precedent).
- **`graph::effective_acl_t` (`security_acl.hpp`) + the ADR-0050 cached
  effective-ACE merge.** The effective-ACL semantics that lived inline in
  `graph_t::acl_allows` — own ACEs before ancestors, nearest-first, ancestor
  ACEs filtered to `kAceInherit`, open-by-default over an empty merge,
  any-present-ACE-closes (even expired), verdicts via the ADR-0050 policy — are
  now a pure, graph-free class (`append_own` / `append_ancestor` / `merged` /
  `release` / `allows`). On top, the graph caches the merged list per vertex
  (`vertex_t::with_effective_aces` + `mark_acl_cache_dirty`; `set_acl` now marks
  its own cache stale), so a gated read/write/await evaluates ONE pre-merged
  list under the target's own lock — the per-operation ancestor mutex-walk
  leaves the data plane. Invalidation is subtree-precise via the ADR-0057 child
  links: a `:acl` write re-marks the written vertex's subtree dirty
  (wiring-frequency); the rebuild is lazy on the next check. Verdicts are
  bit-identical — the cache is a pure optimization (~3.5× aggregate throughput
  and ~10× p99 latency on the new contended `acl-inherit-d4-mt4` bench row;
  single-threaded parity).

- **`wire::check_frame(const view::rope_t&)` — the cheap ingress check
  (CONTEXT.md §Validation timing).** Top-level `parse_header` + the
  `total == size` anchor + the whole-frame trailer CRC when `opt.CR` (a linear
  link-by-link scan) — and nothing more. No tree walk at ingress: a malformed
  child TLV surfaces its error where that level is CONSUMED (per-TLV
  verify-at-access, ADR-0053). `wire::validate_rope` keeps its strict whole-tree
  semantics as the opt-in eager validator; the rope terminus now verifies only
  the root (`tlv_view_t::over` + `verify()`), its recursive whole-tree
  `verify_view_tree` pre-pass deleted.

### Changed

- **BREAKING — the graph is a Composite vertex tree; `vertex_t::key()` is replaced by
  `vertex_t::name()`** ([ADR-0057](../docs/adr/0057-graph-composite-vertex-tree.md)).
  `graph_t`'s flat full-key `unordered_map` is replaced by parent/children links on
  `vertex_t`: each vertex stores its **own canonical NAME record** (`name()`) plus a
  parent pointer and a children container (`parent()` / `registered()` / `fill()` /
  `add_child()` / `child_by_record()` / `for_each_child()`, and the `kInlineChildren`
  inline-first width); the full key is rendered on demand by the graph. Vertical
  bubbling and the ACL inheritance walk follow parent pointers lock-free instead of one
  shared-lock map hop per ancestor level. `graph_t`'s public data API — handles,
  register / find / read / write / await / subscribe / propagate — is unchanged, and
  vertex lifetime stays insert-only (no erasure; handles remain stable for the graph's
  lifetime). Callers never dereferenced `vertex_t` (opaque behind `vertex_handle_t`,
  ADR-0056), so the impact is limited to code constructing bare vertices.

- **BREAKING — `wire::kMaxDepth` is deleted; nesting depth is
  receiver-resource-bounded (RFC-0006).** `grammar::walk` no longer takes
  `(std::pmr::memory_resource&, std::size_t max_depth)`: it takes a
  `grammar::walk_stack_t<Cursor>&` — inline caller-provided
  `walk_frame_t<Cursor>` slots (a tuning knob, not a limit) that spill into
  geometrically grown blocks drawn from a `std::pmr::memory_resource*`. A null
  spill makes the inline span the receiver's whole decode budget: exhaustion is
  a clean non-throwing reject with `TLV_NESTING_TOO_DEEP`, whose meaning is
  amended to "exceeds this receiver's decode resources". `decode` /
  `validate_rope` spill to the default (heap) resource; `decode_into` spills to
  the caller's arena, so the arena IS the depth bound. `graph_t`'s branch-write
  decomposition walk is now iterative (an explicit open-node stack) — no
  recursion over wire-derived structure remains behind the removed cap.
- **BREAKING — the in-process subscription callback is `{fn-ptr, ctx}`-based, and
  `vertex_t` grows a verb interface.** `subscriber_t::callback` is no longer a
  `std::function<void(const rope_t&)>` but a plain `{subscriber_fn_t, void* ctx}` pair
  (the same ADR-0047 hot-path shape as the transport `receiver_slot_t` seam), so the
  per-publish edge snapshot is a trivial copy — no `std::function` clone (which
  heap-allocates once captures exceed the SBO). `graph_t::subscribe(path, callback)`
  becomes `subscribe(path, subscriber_fn_t, void* ctx)` plus a template lvalue-callable
  sugar mirroring `transport_t::set_receiver`: the callable is bound by address and
  must outlive every delivery — **temporaries no longer compile**
  (`auto cb = [&](const rope_t&){...}; g.subscribe(path, cb);`). `vertex_t` now exposes
  its state behind public verbs — `store` / `note_write` / `read_stored` /
  `wait_for_change` / `current_seq` / `mark_flushed` / `drain_unflushed` /
  `history_snapshot`, edge verbs `add_edge` / `clear_edge` / `snapshot_edges` (with the
  new `edge_view_t` dispatch view and `edge_latch_t` durability latch) / `edge_source` /
  `edge_sources`, ACL verbs `set_acl` / `acl_bytes` / `with_aces`, settings accessors
  `settings_snapshot` / `update_settings`, `delivery_mode` / `set_delivery_mode`,
  `handlers`, and the RFC-0005 counter accessors — and the `friend class graph_t` grant
  is deleted; `graph_t` goes through the verbs only. Behavior-preserving: store order
  (LKV publish before the lock), ring keep-last trim, await predicate, the
  snapshot-under-lock/dispatch-outside discipline, and the `kInlineFanout` no-heap
  small-fan-out path are unchanged. The duplicated three-leg delivery (per-write
  fan-out vs. the admission durability latch) now shares ONE `dispatch_edge` helper.

- **BREAKING — the transport receiver seam is `{fn-ptr, ctx}`-based and lives in the
  base (`receiver_slot_t`).** `transport_t::set_receiver` / `set_rope_receiver` and
  `bus_link_t::set_peer_receiver` / `set_peer_rope_receiver` are now NON-virtual base
  functions storing plain `{function pointer, void* context}` pairs in a shared
  `tr::net::receiver_slot_t` (new header `libtracer/receiver_slot.hpp`) — the one home
  of the owning-rope-vs-borrowed-span tier select every adapter used to re-implement.
  The `std::function` aliases `transport_t::receiver_t` / `rope_receiver_t` /
  `bus_link_t::peer_receiver_t` / `peer_rope_receiver_t` are **removed**; new fn-ptr
  aliases are `receiver_fn_t` / `rope_receiver_fn_t` / `peer_receiver_fn_t` /
  `peer_rope_receiver_fn_t`. A template lvalue-callable sugar keeps call sites terse
  (`auto rx = [&](...){...}; t.set_receiver(rx);`) — the callable is bound by address
  and must outlive delivery (temporaries no longer compile). Adapter authors: dispatch
  inbound frames via the protected `rx_` / `peer_rx_` slot (`deliver` /
  `deliver_rope` / `deliver_borrowed`, strategy query `has_rope()`); the per-adapter
  receiver members, setter overrides, and `rx_dirty_` snapshot dances are gone.

## [0.3.0] — 2026-07-08

<!-- The following subsections are the work landed after the initial
     0.3.0 cut but before the tag; all of it ships in 0.3.0. -->

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
