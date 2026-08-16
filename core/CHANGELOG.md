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

## [0.13.0] — 2026-08-16

### Added

- **`wire::path_label_t` and `net::path_label_table_t` — the RFC-0027 path label and its mint
  table** ([#1325](https://github.com/avatarsd-llc/libtracer/issues/1325)). The first car of
  RFC-0027's acceptance train: the 32-bit per-host, per-path-element alias — `(u16 slot index,
  u16 generation)` as one little-endian u32, host-assigned and never a content hash — plus the
  per-host table that mints it. `path_label.hpp` (`tr::wire`, beside `path_ref.hpp`) owns the
  value and its byte order; `path_label_table.hpp` (`tr::net`, keyed by the `peer_handle_t` the
  receiver seam already carries) owns the state. The table draws every slot from an **injected**
  `memory_resource` (ADR-0079's per-plane axis), carries a **per-peer ceiling**, and on
  exhaustion **refuses a new mint** rather than evicting a live one — a refusal, a retirement and
  a non-minting hop are all one benign case, the string path, which is what every host does
  today. Generations **saturate and retire their slot permanently** (RFC-0027 §4.3.1's
  acceptance ruling), which preserves RFC-0024 §4.4 rule 3 verbatim across both
  `(slot, generation)` fields at zero wire cost and closes #603's mis-delivery class by
  construction. Default-constructed, the table mints nothing, so a host that does not opt in is
  unaffected. (Car 1 froze no wire surface; the element that carries the value is car 2, below.)

- **The RFC-0027 label ELEMENT — `wire::emit_path_label`, `wire::path_label_at`,
  `wire::path_label_record_valid`, `wire::kPathLabelRecordBytes`, and the kind-agnostic
  `wire::emit_path_escape` / `wire::packed_escape_kind` / `wire::packed_escape_payload`**
  ([#1325](https://github.com/avatarsd-llc/libtracer/issues/1325) car 2). RFC-0027 §5.3 is
  **closed** by its amendments 4–6 and this is the spelling they ruled: a label element is
  RFC-0018 §8's escape record, **`00 <u8 kind = 0x16> <u8 len = 4> <u32 LE label>` — 7 bytes**,
  inside the packed `PATH` body. Three consequences the API shape carries:
  - **No `PATH_LABEL` TLV type code exists.** §5.3's candidate 8-byte child spelling is *never
    built* (amendment 5), so `tlv.hpp` is untouched, `0x16`–`0x1F` stays the unassigned type
    range, and the element has no option byte — which is why
    **`wire::path_label_body_valid(pl, ll, len)` is REMOVED** and replaced by
    `path_label_record_valid(kind, payload_len)`. Its `PL`/`LL` clauses are unrepresentable
    under the ruled spelling; the two clauses that survive are the kind and the length, and
    they are checked (and vectored) separately.
  - **A label covers a hop's whole local part**, not one segment (amendment 6): the record is
    7 bytes whether the mount run it stands for is one segment or five, so nothing in the
    element encodes the run's width and no core may infer it.
  - **A labelled `PATH` is not a `path_lookup_key`** (amendment 5, §5.3 sub-question 3). That
    refusal needed no new code — `packed_path_valid_key` already rejects every escape record —
    but it now has a test, which is what the ruling asked for.

  `packed_path.hpp` stays **kind-agnostic** (it frames, spans and skips a record without knowing
  what a kind means — the property a non-implementing hop relies on) and `path_label.hpp` owns
  `0x16`. Emitting the record is **not** minting one: RFC-0027 §6.2's trigger, the reply-leg
  rewrite and the deref are later cars. **Conformance:** a new `path-label/` category —
  `label-roundtrip`, `label-mixed`, `label-multi-segment`, `label-wrong-length`,
  `label-foreign-kind` — pinned byte-exact against this emitter in `path_label_test.cpp`. No
  existing vector changes: a `PATH` with no label element is byte-identical to today's, and the
  escape record it uses was already admissible in a frame path since RFC-0018 landed in this
  same unreleased cycle.

- **`graph_t::set_vertex_ceiling()` / `vertex_ceiling()` / `vertex_ceiling_refusals()` — vertex
  creation is now CHARGED against the vertex-slot census**
  ([#1314](https://github.com/avatarsd-llc/libtracer/issues/1314)). RFC-0005 §D branch-write
  decomposition created its landing vertices with no count bound: each site was *governed* (it
  passed its CREATE and WRITE gates) but *uncounted*, so an authenticated writer grew the node's
  vertex population by writing more and writing wider, with no refusal short of allocation
  failure. That is the peer/writer-multiplied allocation class
  [ADR-0079](../docs/adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md) fences
  elsewhere — a per-call bound a caller can multiply is not a node bound. The census
  (`vertex_slot_count()`, RFC-0024 §6.4) already counted every `vertex_t` allocation; it now also
  *charges* one, in the single descent every creation door funnels through — local registration,
  the write-create `mkdir -p`, and every branch-write landing site. Past the ceiling a creation
  answers `BACKPRESSURE` (the injected-store exhaustion status, so no new refusal vocabulary) and
  the refusal is tallied — #838's count-then-act shape rather than a second bespoke counter.
  **Policy stays with the deployer** per ADR-0079 §Decision 4: the default is
  `graph_t::kNoVertexCeiling`, so an un-sized node behaves exactly as before and the library fixes
  no synthetic limit. The ceiling bounds ALLOCATIONS, not live occupancy — the census is
  append-only because retirement revives in place — and session identity anchors are not charged,
  being already bounded by the listener's `max_peers` accept policy.

- **`net::route_handle_t::copy_local_route()` — the warm COMPACT delivery observation no longer
  re-pays the owning lookup** ([#917](https://github.com/avatarsd-llc/libtracer/issues/917)).
  A COMPACT that resolves to a local terminus took the allocation-free
  `resolved()` view, wrote the payload, and then — whenever an
  `fwd_router_t::on_compact_delivery` observer was installed — called `lookup_ingress()` again
  purely to name the bound route for it. That is the whole two-allocation owning copy (a
  `std::string` plus a `std::vector`) `resolved()` exists to remove, re-paid on every observed
  frame. The new accessor copies just the terminus route bytes out under the link's own mutex
  into caller storage: the steady-state observed delivery now allocates **nothing**, and an
  observer that merely watches can no longer turn a delivery into an allocation-failure drop.
  It returns the route's full size (`0` ⇒ no binding, or a forwarding swap, which has no local
  route); a return greater than the destination's size means nothing was written and the
  caller falls back to `lookup_ingress()`. No behaviour change for hosts that install no
  observer, and none in what an observer is handed.

- **The per-module `conn` CREATOR ENDPOINT — `SPEC`/`NAME` writes to `/net/<module>/conn` create
  and remove connections (RFC-0014 S2b,
  [#1302](https://github.com/avatarsd-llc/libtracer/issues/1302))**. RFC-0014's write-driven
  dispatch seam, the one ADR-0059 decided the shape of and `transport_vertex.hpp` has carried an
  "accepted, unimplemented" note about since. `net::transport_vertex_t::register_module` now
  MINTS two vertices for the module it declares: the `<net_root>/<module>` grouping vertex
  (previously created lazily on the first connection) and, below it, a `role_t::HANDLER`
  endpoint at the new `net::kConnEndpointName` (`"conn"`). A write to that endpoint is
  *executed*, not assigned, and the written TLV's TYPE selects which operation:
  `SPEC{ name, config }` ⇒ create `<net_root>/<module>/<name>` atomically, `NAME{ <name> }` ⇒
  remove it, any other payload ⇒ `TYPE_MISMATCH` (the endpoint never falls through to an
  ordinary assign). Both the transport and the ROLE are positional — they *are* the module — so
  the endpoint SPEC carries neither a `type` nor a `role`, and a config `role` pair written
  there is ignored rather than honoured. The SPEC's `name` is REQUIRED and stays required
  ([ADR-0073](../docs/adr/0073-naming-authority-the-application-mints-one-predicate-gates.md)
  §5): a creator-chosen name is what makes a retried create idempotent (`PATH_IN_USE`, "already
  exists") instead of appending a second connection. Refusals: empty name ⇒ `TYPE_MISMATCH`, a
  name failing the shared segment predicate ⇒ `INVALID_PATH`, the reserved name `conn` ⇒
  `PATH_IN_USE` on create and `PERMISSION_DENIED` on remove (the endpoint cannot self-destruct),
  a kind the module does not declare ⇒ `SCHEMA_NOT_FOUND`; a `NAME` naming no connection is a
  **no-op success**, so a retried teardown is safe too. Encoder side: `net::conn_spec_t` gains a
  one-argument (name-only) constructor for the endpoint spelling and `net::conn_remove(name)`
  builds the removal `NAME`. Nothing is added to the shared `conn_settings_t` or to
  `transport_vertex`'s config surface — kind-private config is still parsed by the kind's own
  factory from the raw config TLV (the standing lean-transport rule). The superseded
  `/net:children[]` creation spelling is UNCHANGED and still works; RFC-0014 S7 retires it.

- **`net::transport_t::egress_source()` / `set_egress_source()` — the egress gather draws from
  an INJECTED `mem::block_source_t`, not the process default**
  ([#1287](https://github.com/avatarsd-llc/libtracer/issues/1287), family 1 of
  [#873](https://github.com/avatarsd-llc/libtracer/issues/873);
  [ADR-0079](../docs/adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md)). Six
  hot egress sites drew their per-send gather block from the process-wide `mem::heap_source()`:
  the base `transport_t::send(iov)` gather — the path **every non-overriding transport** lands
  on, `transport_can` and any out-of-tree embedder's included — and the `net::iov_table_t`
  overflow of the three socket transports that build an `::iovec` table (tcp's one-peer,
  broadcast and directed-facade senders, udp's datagram gather, ws's broadcast and
  directed-facade gathers). Both the entry count and the byte count are the **sending peer's**
  choice (a rope's link count × its region count), so a deployer who wired a bounded source at
  every seam the API offered still had these allocations escaping to the process heap — the gap
  ADR-0079 §Decision 4 exists to close. Each link now carries its own store; sizing it bounds
  this node's egress allocation, and exhaustion answers exactly as before (the frame is
  **dropped and counted**, never truncated, never `abort()`). Wired by the transport factory:
  `register_builtin_transports` / `register_udp_transport` / `register_tcp_transport` /
  `register_ws_transport` take a new trailing `mem::block_source_t* egress_src`, fed by a new
  trailing `egress_src` argument on the full `net::transport_vertex_t` constructor and applied
  through the new `net::with_egress_source` helper; `transport_vertex_t::egress_source()`
  exposes the plane's store to a factory registered later through `register_transport_type`
  (`quic`, `can`, an embedder's own). Nothing is added to the shared `conn_settings_t` or to
  `transport_vertex`'s config surface — the standing lean-transport rule. **Every new argument
  defaults to `&mem::heap_source()`, so a build that wires nothing is unchanged bit for bit**;
  the SLIM `transport_vertex_t` ctor is untouched. None of the six sites allocates while
  holding a transport lock, so an injected source carrying its own `Sync` policy introduces no
  lock-ordering obligation ([#1049](https://github.com/avatarsd-llc/libtracer/issues/1049)).
- **`net::transport_can::dropped_stale_binding()` — the CAN weld, made observable and then
  refused** ([#1011](https://github.com/avatarsd-llc/libtracer/issues/1011)). A learned binding
  whose endpoint run no later advertise *overlapped* survived indefinitely, because the
  retire-on-re-issue rule ([#909](https://github.com/avatarsd-llc/libtracer/issues/909)) fires
  on overlap and skips disjoint runs. Data slices whose own advertise was lost on the bus then
  resolved first-match to that survivor, completed its stale group, and two unrelated payloads
  were delivered upstream as one frame — trimmed to the length the *stale* manifest promised,
  so byte-corrupt at exactly the size the receiver expected. The receiver now decides "no
  advertise in this lap" from state it already holds: `alloc_base` issues strictly ascending
  bases and wraps to `kCanFirstDataEndpoint`, so an advertise whose base does not exceed the
  last one seen from that node proves the producer's allocator came round, and every binding of
  that node is marked as belonging to a **prior lap**. A slice resolving to a prior-lap binding
  is refused instead of welded: the group is discarded (ticking `dropped_groups()`, as every
  other pre-delivery reclamation does) and the slice ticks the new counter. Counts **slices**,
  like `dropped_rx()`; never folded into it. **No wire change** — ADR-0077's option 1 (binding
  group identity into the data frames) stays declined, since the 29 bits are fully consumed by
  `version|node|endpoint`. RAM: **0 added bytes per binding** (the flag lands in `binding_t`'s
  existing tail padding) and 8 per remote node heard. Common-path cost: one already-loaded
  `bool` test per data slice — no lookup added.
- **`graph::default_config_t::kWeaklyOrdered` — the target's memory model, as a configuration
  member, so an ordering precondition can be a `static_assert`**
  ([#1143](https://github.com/avatarsd-llc/libtracer/issues/1143)). Public config surface, with
  the derived spelling `graph::kWeaklyOrdered` beside the other loose names. **Defaults to
  `true`** — assume the target reorders unless its fragment says otherwise — and it never
  selects a weaker access, so no shipped build changes an instruction. The knob's first
  consumer is the new `graph::kDeliverySkipOrder` (`vertex.hpp`), the single spelling of the
  delivery-skip Dekker pair's order (`vertex_t::own_subs_ordered` and `bump_own_subs`, #635 /
  #1140): on a weakly-ordered target the build now REFUSES a `kDeliverySkipOrder` weaker than
  `seq_cst`, where before that pairing's argument lived only in prose and its ablation left the
  whole suite green on a TSO host. The evidence half of the same question — an
  `ubuntu-24.04-arm` CI leg that can actually exhibit the anomaly — landed earlier under #1140.

- **The peer LIVENESS WINDOW: `liveness_window_ms` on all four host stream transports, and the
  `liveness_window` config key on `tcp` and `ws`**
  ([#838](https://github.com/avatarsd-llc/libtracer/issues/838)). The #838 bound's provenance is
  an **app-provided liveness window**, injected exactly as `connect_timeout`, CAN's `peer_ttl`
  (ADR-0044) and `httpd_ws_link_t::send_timeout_ms` are — not a core literal, and not derived,
  because a host has no task watchdog to derive from the way the MCU link does. `tcp_transport_t`
  (both constructors), `transport_tcp_server`, `transport_ws_client` and `transport_ws_server`
  each gained a trailing `liveness_window_ms` parameter (default `0` = the conservative
  `kDefaultLivenessWindowMs` **10 s** clamp — "safe unless you opt out", since today's unbounded
  block is the bug); the `tcp`/`ws` factories parse the same number from the kind-private
  `liveness_window` key (VALUE u32, ms). Existing call sites are source-compatible. New
  accessors `liveness_window_ms()` and `stalled_tx()` on all four. The per-record bound is
  `derive_send_bound_ms(window, peers-in-this-round)`, floored at `kBoundedWaitMs` (100 ms —
  a bound that rounds to 0 means *block forever*, the #956 lesson), so one whole fan-out round
  with EVERY peer stalled still releases both locks inside one window. `stream_endpoint_t`'s two
  full-write helpers now RETURN a `write_result_t` (outcome + whether the record half-landed) and
  take an optional per-record bound; `slot_server_t::broadcast_iov` returns how many peers it
  shed for. Framed as the liveness window rather than a fifth independent timeout knob so it
  converges into RFC-0014 §S5's single liveness contract later; S5's engine is NOT built here —
  a keepalive clock cannot unblock a send already stuck inside `write_all_iov`, which is why the
  per-send bound is what makes that clock enforceable at all.

- **#838's RAM cost, priced and EXPLICITLY ACCEPTED: +16 B per link, +14–16 B per connection**
  ([#838](https://github.com/avatarsd-llc/libtracer/issues/838)). Recorded here because it never
  was — #838 merged as `aee923d5` without an entry pricing what the stalled-peer write bound
  costs, and an unpriced cost is indistinguishable from a regression the next time someone reads
  the census. It is neither: it is the accepted price of the bound, following the
  [#1160](https://github.com/avatarsd-llc/libtracer/issues/1160) precedent, where a +226 B cost
  was priced and waived on the record rather than silently absorbed. **What it bought:** a host
  stream send can no longer block indefinitely on a stalled-not-dead peer, and no longer holds
  `write_m_` while it does. **What it costs,** measured `aee923d5` against its parent
  `2dcdbcdd` with `bench_conn_ram --no-can --reps=5` on the same host, same toolchain:

  | metric | `2dcdbcdd` | `aee923d5` | Δ |
  | --- | ---: | ---: | ---: |
  | `sizeof tcp_client` | 264 | 280 | **+16 B** |
  | `sizeof tcp_server` | 440 | 456 | **+16 B** |
  | `sizeof ws_server` | 440 | 456 | **+16 B** |
  | `sizeof ws_client` | 328 | 344 | **+16 B** |
  | `tcp-server metric=link_base` | 464 | 480 | **+16 B** |
  | `ws-server metric=link_base` | 464 | 480 | **+16 B** |
  | `tcp-server metric=per_conn` (median) | 282 | 296 | **+14 B** |
  | `ws-server metric=per_conn` | 376 | 392 | **+16 B** |

  The 16 B is the two new per-link members — the `stalled_tx_` counter (8 B) and
  `liveness_window_ms_` (4 B), padded to the alignment already there. **`udp` and `can` are
  unchanged** (`sizeof udp` 216, `can` 800; every udp arm's `link_base` and `per_conn` byte-for-byte
  identical), which is the scope ruling showing up in the numbers: #838 touched the host stream
  transports only. **No RAM baseline is re-pinned by this entry** — it is accounting, not a gate
  move. Note the per-connection census is emitted by `bench_conn_ram` into the `perf-local` run
  log (`# sizeof:` and `RESULT … metric=per_conn`/`metric=link_base` rows) and is **not** carried
  into `data.js`, so it does not appear on the gh-pages charts; reproducing it means running the
  bench at both revisions, which is what was done here.

### Changed

- **A `PATH` (`0x06`) body is now packed `[u8 len][utf8]` segment records, not `NAME` children
  ([RFC-0018](../docs/spec/rfcs/0018-packed-path-segments.md),
  [#680](https://github.com/avatarsd-llc/libtracer/issues/680)) — BREAKING on the wire and on
  the C++ API.** `opt.PL` is `0`; the body is a self-delimiting run of records and the walk is
  `p += 1 + body[p]`. `/sensor/temp` goes 18 bytes to 12, a four-hop `dst` 146 B to 104 B, and
  the resolve leg 34 ns to ~20 ns (`bench_forward_demux` axis 3 grew a `fwd-demux-resolve-literal`
  arm so the two encodings are timed in ONE binary — RFC-0018 falsifier 1).
  - **The vertex-map key IS the `PATH` body**, so it moves with the encoding:
    `path_t::key()`, `graph_t::find`, `key_view_t` and every stored `path_key_t` are packed
    bytes now. A `key_view_t` record is a one-byte length prefix; `record_end` /
    `record_from` / `record_cursor_t` / `child_record_under` answer in the new offsets.
  - **`arena_tlv_t::canonical_path` is REMOVED**, and with it `is_canonical_name` and the
    per-child bookkeeping the terminus decode ran for it. The flag asked whether a `PATH`'s
    children were all bare `NAME`s, because a legal peer could spell one address several
    ways (`opt.LL`, a per-segment trailer) and a byte key would then miss. A packed record has
    no option byte and no type byte, so there is exactly ONE spelling per address: the body is
    the key unconditionally, and the span-alias (ADR-0041 §3) is guaranteed rather than tested.
    That also closes the second, still-open locus of
    [#436](https://github.com/avatarsd-llc/libtracer/issues/436) — `wire::path_key` could
    mistype a child; a packed body has none. `path_lookup_key` lost its `fallback` parameter
    for the same reason.
  - **`wire::path_key(const tlv_t&)`** now returns the packed body and refuses a structured
    (`opt.PL = 1`) `PATH`, ragged framing, or an escape record, where it used to refuse a
    non-`NAME` child.
  - **New header `libtracer/packed_path.hpp`** (`tr::wire`) owns the grammar in one place:
    `emit_path_segment`, `packed_record_span` (frame-path context — steps OVER the RFC-0018
    §5.4 `len == 0` escape), `packed_path_valid_key` (canonical/key context — REJECTS it), and
    the escape constants. Nothing mints an escape; `kind = 0x16` is reserved for RFC-0027's
    label element, and building the SKIP path now is what keeps that RFC from reopening this
    code.
  - **`net::stack_writer::name` becomes `path_seg`**, and a new `header_path` writes the
    `opt.PL = 0` `PATH` header the rebuild emits. `encode_mount_tlv` /
    `child_registry_t`'s mount run emit packed records; `fwd_rebuild_t::extra_hdr` is one byte.
  - **Unmoved on purpose:** `NAME` (`0x02`) and `wire::emit_name` are untouched — they still
    spell SETTINGS keys, `:schema` labels and `:children[]` members. RFC-0018 removes `NAME`
    from `PATH` bodies only, so `read_children` / `read_children_folded` /
    `read_subtree_folded` keep emitting `POINT{NAME …}` byte-identically; they write the
    `NAME` header beside the `POINT` one rather than borrowing the key record, which keeps
    both folds at the link count their reservations are sized against.
  - **The RFC-0023 caps trade places.** A one-byte segment costs 2 bytes packed instead of 5,
    so `kMaxPathBytes` (1024) admits 512 records and `kMaxSegments` (255) is what binds —
    where before the byte cap fired at 204 segments and the count clause could never trigger.
    Pinned by the new `path/path-deep-255-packed` vector.
  - **Conformance:** 27 vectors carrying a `PATH` were re-blessed from this reference
    (ADR-0028). `path/path-value-children-illegal` is **retired** — it is unrepresentable —
    and replaced by `path/path-escape-in-key-context` and `path/path-record-overruns-body`,
    which pin what remains of its rule: an illegally-spelled address answers
    `ERROR{tr::path::invalid}` rather than resolving to something.

- **BREAKING — `net::fwd_rebuild_t`'s trailer-TS window is one `std::uint32_t ts_window`
  (offset + form bit) read through `ts_off()` / `ts_bytes()`; `ts_off`/`ts_len` as fields are
  gone, and the struct is size-ratcheted at 256 B**
  ([#1235](https://github.com/avatarsd-llc/libtracer/issues/1235)). The two `std::size_t`
  fields #1109 added pushed this per-hop STACK object from 256 to 272 bytes, and that alone
  cost `fwd-demux-fixed 79B/fan1/1ep` **p50 +9.6% / throughput −9.3%** on the pinned host —
  a step that held for 16 samples because it walked under every PR-gate threshold. It is the
  SIZE, not the work: an ablation that added the same 16 bytes and NEVER READ them reproduced
  the regression exactly, while a 264-byte intermediate (two `std::uint32_t`s) was still
  +9.5%. The window now occupies the alignment hole after `extra_hdr` as a single word —
  offset in bits 0-30, the producer's `opt.TF` form in bit 31 — which restores the row to the
  pre-#1198 band. A `static_assert(sizeof(fwd_rebuild_t) <= 256)` makes the next such growth
  a compile error rather than a bench-series reading. **Wire behaviour is unchanged** —
  #1109's stamp preservation, its TF=0 wide / TF=1 narrow relay (the width stays the
  producer's choice; the forwarder never picks one) and the dropped inbound CRC all still
  hold — with one new degenerate case stated rather than left implicit: a frame whose body
  ends past 2 GiB cannot express its window, so the stamp and its header bits are dropped
  TOGETHER instead of a head declaring a trailer the gather cannot emit.

- **BREAKING — `net::can_link_t` is now a TWO-PHASE seam: construction opens the link,
  `start()` begins reading**
  ([#1186](https://github.com/avatarsd-llc/libtracer/issues/1186)). The seam documented
  "set before frames flow" for `on_receive`, but `socketcan_link_t`'s constructor spawned
  the receive thread — so there was no ordering a caller could adopt that registered a sink
  before the link could read. The requirement is now compile-checked instead of written down:
  `can_link_t` gains a pure-virtual `start()`, `socketcan_link_t` and the ESP component's
  `twai_link_t` split the thread spawn out of their constructors, and the owner registers
  first and starts second. `start()` is idempotent and silent on a link that never opened
  (the seam has no error channel; `ok()` already answers that). **No frames were being lost
  and none are now**: the kernel socket buffer — and, on the TWAI port, the driver RX queue
  the rx-done ISR keeps filling — holds what arrives between open and the first read.
  **Migration**: an out-of-tree `can_link_t` implementation must add `start()` (spawn its
  receive machinery there, not in the constructor); a caller that owns a link DIRECTLY must
  call `link->start()` after `link->on_receive(...)`. A caller that hands its link to
  `transport_can` needs no change at all — the transport drives both phases itself, and it
  now expects the link it is given to be open but NOT started. `write_raw` is unaffected:
  egress is live as soon as the link is open.

- **BREAKING: the `net::bus_link_t` peer-receiver seam carries an opaque per-peer HANDLE, not a
  name string** ([#1294](https://github.com/avatarsd-llc/libtracer/issues/1294)). The seam
  re-supplied a `std::string_view` peer NAME on every inbound frame, so every consumer that
  wanted a per-peer identity had to re-derive one from that string per frame — a hash and a map
  find on the remote-subscribe path, and nothing at all to hang a per-peer auth subject off. It
  now carries `net::peer_handle_t`, an 8-byte `(index, generation)` POD minted once when a peer
  becomes audible and valid until it departs — the same node-local-index-plus-validate-on-use
  stamp the in-tree edge binding and the ESP link's session ref already use. Concretely:

  - `bus_link_t::peer_receiver_fn_t` / `peer_rope_receiver_fn_t` (and the callable-taking
    `set_peer_receiver` / `set_peer_rope_receiver` sugar) take `peer_handle_t` where they took
    `std::string_view`;
  - `peer_up_fn_t` / `peer_down_fn_t` gain the handle as a leading parameter, beside the name
    they already carried — arrival is where a handle is minted and departure is where it is
    retired, so that is where a consumer binds and drops whatever hangs off it;
  - a new pure virtual `bus_link_t::peer_name(peer_handle_t, std::span<char> scratch)` is the
    ONE bridge back to the routing plane's NAME. Every kind answers it as a pure function of the
    handle's index (`p<slot>`, `n<node>`), so it takes no lock and is safe to call per frame from
    the delivery callback — which is what `fwd_router_t` does, once, where the name used to
    arrive for free. **Every out-of-tree `bus_link_t` implementation must supply it.**

  The ADDRESSING surface is unchanged and still speaks names: `enumerate_peers`, `peer_link` and
  `close_peer` take and produce peer names, because a name is what a routable `dst` segment
  carries. The handle is deliberately NOT a session reference (a session ref is one *supplier* of
  a handle — an announce-census CAN peer has no session and still needs a stable link key) and
  deliberately does NOT carry an auth subject (that is resolved from the handle at ACL-check
  time). It is never absent: a link with no meaningful per-peer identity mints the constant
  `net::kSolePeerHandle` at link-up, so no consumer needs a "handle absent" branch. This is a
  clean break with no compatibility overload — the seam exists at exactly one place and this
  changes it exactly once, rather than three times as #1266, #375 Part 2 and #1278 land.

- **BREAKING (behaviour): `max_peers = 0` no longer means UNCAPPED on the tcp/ws stream
  servers, and a DIRECTED send is bounded by `window / max_peers` rather than by the whole
  liveness window** ([#1295](https://github.com/avatarsd-llc/libtracer/issues/1295), the
  residual split out of [#838](https://github.com/avatarsd-llc/libtracer/issues/838)).
  #838's per-record send bound is `window ÷ peers-in-this-round`, which is right for a
  broadcast fan but made a *directed* send — `bus_link_t::peer_link(name)->send()` — a round
  of ONE that took the entire window for itself. Directed sends on one server serialize
  behind the same `write_m_`, so N callers targeting N stalled peers accumulated N full
  windows: bounded per call, unbounded per node, and multiplied simply by a peer opening
  more connections (the ADR-0079 class of defect). Two changes, and they are one fix:
  - `transport_tcp_server` / `transport_ws_server` (and the shared `net::slot_server_t`)
    now resolve their admission cap through the new `net::derive_max_peers`. A `max_peers`
    of `0` — the constructor default and the `max_peers` config key's default — takes the
    liveness window's own ceiling (`net::max_admissible_peers` = `window / kBoundedWaitMs`,
    so **100** on the default 10 s window) instead of admitting peers without limit, and a
    request ABOVE that ceiling is clamped to it. **A deployment that relied on `0` to accept
    unlimited concurrent peers will now see connections past the cap refused cleanly** (the
    existing at-cap behaviour: accepted, then immediately closed). Widen `liveness_window`
    to raise the ceiling, or inject the `max_peers` you want. The enforced value is readable
    from the new `net::slot_server_t::max_peers()`.
  - directed sends, and the ws poll thread's handshake reply and PONG, now take
    `net::slot_server_t::directed_send_bound_ms()` (`window ÷ max_peers`) instead of the
    whole window. The aggregate over every peer that can be stalled is one window — the same
    claim `broadcast_iov` makes for the fan — rather than one window each.

  The broadcast round still divides by the peers actually in it, unchanged. No wire, config
  TLV, or `conn_settings_t` surface changes: `max_peers` remains a kind-private key parsed by
  the tcp/ws factories out of the raw config TLV.

- **A wire `SUBSCRIBER`'s `PATH` target is now RESOLVED when it routes through a mount — a
  third party can wire a flow between two OTHER nodes and depart**
  ([#491](https://github.com/avatarsd-llc/libtracer/issues/491),
  [RFC-0021](../docs/spec/rfcs/0021-wire-subscriber-target-frame-of-reference.md) §4.B.1/§4.C).
  `graph_t::subscribe_wire` discarded the `PATH` child outright and bound every wire edge to
  `(caller = the arrival session, return_route = the accumulated src)`, so an orchestrator's
  subscription delivered back to the **orchestrator** and died with its session
  (`evict_link_edges`). A `PATH` spelled in the **producer's** frame
  (`/net/<module>/<link>/<consumer-path>`) is now run through the transport plane's ADR-0061
  strip-K mount descent — the same descent `fwd_router_t::subscribe_toward` uses, now shared as
  `net::fwd_router_t::split_subscriber_target` — and the edge binds to `(that mount's link, the
  residual below it)`. It therefore outlives the writer's departure and dies with the
  **delivery** link instead. **New API:** `graph_t::configure_wire_target_resolver(fn, ctx)`
  plus `graph::wire_target_split_t` / `graph::wire_target_fn_t`, the ADR-0047 `{fn, ctx}` seam
  L4 borrows the descent through; `fwd_router_t` installs it in its constructor, so a node with
  a transport plane has it and one without behaves exactly as before.
  **Deliberately unchanged, and NOT the full RFC:** a `SUBSCRIBER` with no `PATH`, or whose
  `PATH` matches no mount, keeps the arrival-session binding byte for byte — RFC-0021 §4.B.2's
  purely-local arm is unruled (§7 q3), and in-tree senders (the TypeScript client's
  `subscribe`) still spell that `PATH` in the consumer's own frame. A target that names a mount
  it cannot deliver through (the mount exactly, or a bus link's own NAME — RFC-0020) is refused
  with `INVALID_PATH`, never silently degraded to the arrival session. The `SUBSCRIBE` gate and
  the stored gate context stay the **writer's** (#81, ADR-0026, RFC-0021 §E); only the delivery
  link moves.
- **WIRE-VISIBLE: a remote fieldless `FWD{WRITE}` to an unresolved `dst` answers `NOT_FOUND`
  and no longer write-creates the vertex chain**
  ([#1139](https://github.com/avatarsd-llc/libtracer/issues/1139);
  [RFC-0005](../docs/spec/rfcs/0005-subtree-subscriptions.md) §D **amendment 1**, maintainer
  ruling option A). The terminus used to `mkdir -p` the target and every missing level above
  it on a peer's data write. That path consulted no type catalog, produced an untyped
  `STORED_VALUE`, counted nothing, bounded no depth, and — because the `CREATE` check is
  evaluated on the nearest **existing** ancestor — ran **no ACL check at all** when the graph
  held no ancestor above the address, i.e. for any brand-new top-level subtree. Creation from
  a peer is now the [ADR-0059](../docs/adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md)
  creator endpoint's job, where it is typed, catalogued and ACL-gated; the caller backs off and
  retries until whoever owns that structure establishes it. RFC-0005 §1's *appearance is the
  first write* mechanism is untouched — a creator-endpoint create IS a write and bubbles to the
  parent subscriber exactly as before; only the origin of an appearance moves.
  **Unchanged, deliberately:** the local `graph_t::write(path, value)` overload still
  write-creates (the in-process caller is the node's own trusted code and owns its graph's
  structure — the asymmetry is the ruling, not an oversight), and RFC-0005 §B **branch-write
  decomposition still creates its landing vertices**, because those land beneath a `dst` that
  already resolved and was already WRITE-gated, at a depth the peer's own frame bounds. No
  conformance vector changes: the v1 vectors are decode/encode byte vectors and none encodes a
  terminus's answer to an unresolved-`dst` WRITE.
- **`graph_t::ensure_vertex`'s SCRATCH allocations are gone — the write-create path no longer
  draws per-level temporaries from the global heap behind the injected `block_source_t`**
  ([#1139](https://github.com/avatarsd-llc/libtracer/issues/1139), family of
  [#873](https://github.com/avatarsd-llc/libtracer/issues/873)). `ensure_vertex_ptr` collected
  the per-level prefixes into a `std::vector<key_view_t>` and copied each level into a fresh
  `std::vector<std::byte>` to satisfy `register_vertex_key`'s owning-vector signature; both
  drew from the process heap, so a deployment that wired a bounded source did not get one on
  this path. The level walk is now the new allocation-free
  `wire::key_view_t::for_each_level(emit)` (public header addition; `split_levels` is
  reimplemented on top of it and is behaviourally identical), and the registration takes
  **borrowed** bytes through a private span-taking form — the descent never retained the key,
  so the copy bought nothing. `graph_t::register_vertex_key(std::vector<std::byte>, …)` keeps
  its signature and behaviour as the public door; `try_register_vertex` drops its copy too.
  Framing validation still **precedes** creation (two cheap walks, not one), so an
  illegally-spelled key still materializes no prefix. **Scope, stated plainly:** the `vertex_t`
  objects a create registers are still `std::make_unique`d from the process heap — that is the
  larger #873 arena question and is untouched here. What is closed is the per-call scratch, which
  scaled with the key's DEPTH and was the part a peer chose the size of.
- **Two peer-sized growths moved off the `-fno-exceptions` probe window onto the injected
  `mem::block_source_t`** ([#981](https://github.com/avatarsd-llc/libtracer/issues/981),
  follow-up to [#923](https://github.com/avatarsd-llc/libtracer/issues/923); umbrella
  [#873](https://github.com/avatarsd-llc/libtracer/issues/873)). `#923` made
  `detail::try_reserve` / `try_push_back` report allocation failure by value on every profile
  whose growth **throws**; under `-fno-exceptions` the helper can only probe the global heap,
  free the probe block and then run the throwing `reserve`, so a task switch in that window
  still `abort()`s the node ([#850](https://github.com/avatarsd-llc/libtracer/issues/850)).
  `graph_t::read_subtree_folded`'s collect stack and `net::fwd_router_t`'s remote-delivery
  egress iov tables — both sized by a **peer's** choice (which composed root it READs, how many
  links the delivered value carries) and both holding trivially-copyable elements — now use
  `mem::block_array_t` over the graph's injected `ctl` source: **one refusable `try_alloc` per
  growth, no probe, no window**, and the blocks come from the node's own resource rather than
  the process heap. No signature changed and the outcomes are the ones already documented
  (`BACKPRESSURE` for the composed read, a dropped delivery for the fan-out) — what changes is
  **which resource** they draw from, so a host that injects a bounded `ctl` now bounds these
  two allocations too. The six hot-symbol ratchet pins come back `+0`. The remaining 28
  `try_*` sites keep the helper because their element types (`view_t`, `std::vector<std::byte>`,
  `std::string`, `std::shared_ptr`) cannot ride `block_array_t`'s memcpy relocation; each now
  states that residual **at the site**, and the `rope_t::try_reserve` / `try_to_iovec` doc
  comments name it explicitly.

- **`register_module` now mints graph vertices, so `/net:children[]` enumerates every DECLARED
  module — not only the ones already carrying a connection** (RFC-0014 S2b,
  [#1302](https://github.com/avatarsd-llc/libtracer/issues/1302)). Discovery needs this: a
  creator has to find `/net/<module>/conn` before it can write the first SPEC to it, and a
  module that only appeared once it already had a connection could never be the first one
  written to. Two visible consequences for a consumer that enumerates `/net`: an empty declared
  module is now listed, and each `/net/<module>:children[]` listing carries `conn` alongside its
  member connections — hiding the endpoint from that listing is RFC-0014 **S4** (the
  enumeration-hide seam), which does not exist yet. `register_module` correspondingly gained a
  failure mode: it returns the graph's own refusal (e.g. `BACKPRESSURE`) if the endpoint could
  not be registered, and then declares nothing.
- **`graph::vertex_t::fill()`, `mark_unregistered()` and `add_child()` are `private` — the
  map-lock mutators belong to `graph_t`, and the compiler now says so**
  ([#867](https://github.com/avatarsd-llc/libtracer/issues/867), ruling 2). All three mutate
  state whose invariant is "written only under `graph_t`'s UNIQUE map lock" — the registration
  identity, the placeholder bit and the Composite child list — yet all three sat in `vertex_t`'s
  first `public:` block, so any caller holding a `vertex_t*` could stamp an identity, retire a
  node or splice the tree with no lock held and no ACL consulted.
  [PR #1133](https://github.com/avatarsd-llc/libtracer/pull/1133) closed the half of this that
  handed the pointer out (`subscription_t` became opaque); this closes the verbs themselves.
  `vertex_t` now declares `friend class graph_t`, which is the only caller in tree (`graph.cpp`
  — `register_vertex_key`, `retire_subtree`, `register_session_anchor`). **Pure visibility: no
  signature, no body and no layout changed**, and the six hot-symbol ratchet pins come back
  `+0`. `store()` deliberately stays public — it is the storage-layer verb the bare-`vertex_t`
  unit tests drive with no `graph_t` in sight, and no public API hands out a graph-owned
  `vertex_t*` for it to be abused through.
- **`<libtracer/vertex.hpp>` split into five headers — no type moved out of `tr::graph`, and
  every one of them is still reachable through `vertex.hpp`**
  ([#868](https://github.com/avatarsd-llc/libtracer/issues/868)). The vertex hub had grown to
  3421 lines fusing five unrelated concerns, and `graph.hpp` pulls it, so every net-plane TU
  re-read all five whenever any one changed. The concerns now have homes:
  **`<libtracer/app_fields.hpp>`** (the RFC-0010 field tables — `app_access_t`, `app_field_t`,
  `app_field_slot_t` / `app_field_static_t`, `borrowed_fields_t`, `app_field_table_t`,
  `app_field_group_t`), **`<libtracer/acl_ace.hpp>`** (the ACE records — `acl_right_t`,
  `ace_type_t`, `kAceInherit`, `ace_t`), **`<libtracer/subscriber.hpp>`** (the subscription edges —
  `delivery_policy_t`, `subscriber_fn_t`, `subscriber_remote_t`, `target_key_t`,
  `target_binding_t`, `try_make_target_key`, `subscriber_t`, `edge_view_t`, `edge_latch_t`,
  `edge_snapshot_t`, `pub_remote_t`, `pub_edge_t`, `edge_pub_t`, `edge_block_t` and the
  edge-pin retire helpers), and **`<libtracer/vertex_stripe.hpp>`** (`kStripeAlign`,
  `vertex_stripe_t`, `vertex_stripes`, `vertex_stripe_at` / `_index` / `_of` / `_cv`).
  `vertex.hpp` includes all four, so **no existing include breaks**: code that named any of
  these through `<libtracer/vertex.hpp>`, `<libtracer/graph.hpp>` or `<libtracer/tracer.hpp>`
  compiles unchanged. Pure code motion — no declaration's text, signature or order within a
  translation unit changed, and the six pinned hot-symbol sizes are byte-identical.
- **The ACE records leave `<libtracer/vertex.hpp>` for their own header, and
  `<libtracer/security_acl.hpp>` no longer includes `<libtracer/vertex.hpp>`**
  ([#868](https://github.com/avatarsd-llc/libtracer/issues/868)). `acl_right_t`, `ace_type_t`,
  `kAceInherit` and `ace_t` were declared in `vertex.hpp` while their evaluation
  (`allow_only_policy_t`, `full_acl_policy_t`, `effective_acl_t`) and their codec
  (`parse_acl`, `encode_acl`) lived in `security_acl.hpp`, which therefore had to pull the
  whole vertex hub to name four records it fully owns. **Why the records get their own header
  rather than joining the evaluator, which is the shape #868's brief named.** The two halves
  sit on opposite sides of the vertex: `vertex_ext_t` stores a `std::vector<ace_t>`, so the
  records are compiled by every net-plane TU, while nothing in the vertex core calls a policy
  or the codec. Declaring the records in `security_acl.hpp` was implemented and measured
  first — it took that header from **15 to 100 dependent TUs**, so an edit to an ACL
  evaluation rule would have rebuilt the whole tree, inverting the very property #868 exists
  to improve. The `acl_ace.hpp` seam keeps what the brief actually wanted — one home for the
  ACE data, no straddling — and leaves the codec downstream: `security_acl.hpp` is back at 15
  dependents and `vertex.hpp` grows by 15 preprocessed lines instead of 479. **Include-path
  note, and it is a note, not a break:** a TU that included `<libtracer/security_acl.hpp>`
  alone for the ACE records still compiles, because that header includes `acl_ace.hpp`
  (verified by compiling exactly such a TU); `<libtracer/acl_ace.hpp>` is simply the smaller
  include to reach for. The one real change: a TU that included `security_acl.hpp` alone and
  relied on it to drag in `vertex_t` / `graph_t` must now include `<libtracer/vertex.hpp>`
  (or `<libtracer/graph.hpp>`) itself.
- **BREAKING — a HANDLER's `on_write` takes a second argument: the writer's
  `graph::write_ctx_t`** ([#375](https://github.com/avatarsd-llc/libtracer/issues/375)).
  `handlers_t::on_write` (and its internal `value_handlers_t` mirror) go from
  `std::function<result_t<void>(const rope_t&)>` to
  `std::function<result_t<void>(const rope_t&, const write_ctx_t&)>`. **Every
  `on_write` handler in every consumer must be updated**; there is no compatibility overload,
  by ruling — a permanent dual overload costs a second ~32 B `std::function` in every
  HANDLER-bearing extension block for a signature nobody would keep. The new
  `write_ctx_t{subject}` carries the writer's resolved subject token — the ACL model's
  `subject → rights` principal (ADR-0018) and the RFC-0010 subject-table integration point.
  Before this, the ACL gate resolved the caller one stack frame before `on_write` and threw it
  away, so an application handler could not see WHO wrote; now it is handed the identical
  value the gate used, so a handler and the `:acl` that admitted the write cannot disagree.
  **`subject` is BORROWED for the call** — the same contract as the `rope_t&` beside it —
  **copy it if you retain it**. It is EMPTY for a local host write (`is_local_owner()`), which
  is not a magic string but the same trusted-local discriminator `acl_allows` short-circuits
  on (#905); there is deliberately no `OWNER@` sentinel (ADR-0020's erratum, #1033).
  Cost: **0 added per-vertex bytes, 0 allocation, 0 contention** — the ctx is a stack-built
  `string_view` pair on the existing write path.
- **BREAKING — `graph_t`'s three callback configuration seams take the ADR-0047 `{fn, ctx}`
  pair, and are spelled `configure_*`**
  ([#1049](https://github.com/avatarsd-llc/libtracer/issues/1049); the L4 analogue of
  [#914](https://github.com/avatarsd-llc/libtracer/issues/914) / PR #1048).
  `set_remote_delivery_sink(std::function<void(const remote_delivery_t&, const rope_t&)>)`
  → `configure_remote_delivery_sink(remote_delivery_fn_t, void*)`; `set_subject_resolver` →
  `configure_subject_resolver(subject_resolver_fn_t, void*)`; `set_subscription_observer` →
  `configure_subscription_observer(sub_observer_fn_t, void*)`. The types `subject_resolver_t`
  and `sub_observer_t` are **replaced** by `subject_resolver_fn_t` / `sub_observer_fn_t`, and
  `remote_delivery_fn_t` is new; the rename is deliberate, so a stale caller gets "no such
  member" rather than a confusing conversion error.

  The three were plain `std::function` members with public setters, read on the write hot
  path, on every gated read and write, and on the subscribe path — under a comment citing the
  "configure before frames flow" contract #914 RETIRED at the router. The defect is worse than
  the router's was: assigning a `std::function` **destroys the old target**, freeing its
  captured state, so a setter racing a reader that is already *inside* the call is a
  use-after-free, not a torn pair. `sink_slot_t` could not be pointed at them — it statically
  requires a pointer-sized function pointer — so the types were narrowed to that shape first.
  Each seam is now a `sink_slot_t`: a reader sees the whole new pair, the whole old one, or no
  sink for that one operation. `ctx` is caller-owned and must outlive every dispatch, exactly
  as `receiver_slot_t`'s must. Migration is mechanical — hoist the capture into a context
  object and pass a captureless thunk, which is what `fwd_router_t` now does with `this`.

  **`tr::net::sink_slot_t` moved to the layer-neutral `tr::sink_slot_t`**, since an L4 member
  spelled `tr::net::` would point down at a plane that sits above it. `tr::net::sink_slot_t`
  remains a working alias; no net-plane caller changes.

  **Cost: none measured.** Interleaved A/B of `bench_libtracer` against an A/A null from the
  same commit — the plain read, the scalar write and the wide fan-out are all inside the A/A
  band. An unset slot is one relaxed load, which is what the null check on the plain member
  cost; the coherent read on the remote leg is kept out of the `always_inline` per-edge
  dispatch body, whose test stays the single load it was.
- **The child-type catalog and the node identity record are locked, not merely documented**
  ([#1049](https://github.com/avatarsd-llc/libtracer/issues/1049)). No signature change.
  Both were declared under the same retired contract and neither is a callback, so the
  `{fn, ctx}` publication cannot reach them. `register_child_type` inserts into a `std::map`
  that the in-band `:children[]` creation path — driven by a **peer's bytes** — walks;
  `set_identity` / `clear_identity` free a buffer that `read_identity` tests for emptiness and
  then memcpys, and that facet resolves **above** the READ gate so an unauthenticated peer can
  pin the key on first use (RFC-0011 §C.2), which makes the use-after-free remotely reachable.
  Both paths are control-plane cold — one map lookup per created vertex, one identity read per
  peer per pin — so each took a lock rather than a publication trick, and no read, write or
  dispatch path is touched. Setup-only remains the doctrine; the locks make violating it slow
  rather than corrupting.

### Removed

- **BREAKING (source) — `net::iov_table_t`'s `mem::block_source_t& src` argument no longer
  defaults to `mem::heap_source()`; the overflow source is now MANDATORY**
  ([#873](https://github.com/avatarsd-llc/libtracer/issues/873) family 1). #1287 wired all
  three socket gather sites (`transport_tcp.cpp`'s `prefixed_iov_t`, `transport_udp.cpp`,
  `transport_ws.cpp` x2) onto the sending link's `transport_t::egress_source()`, which left
  the default argument as the last un-injected path in the family — an API-shaped invitation
  to draw the peer-sized overflow block off the process-wide heap and quietly escape the
  bound a deployer injected. Census: ZERO in-tree call sites relied on it (the one
  construction that did, `bench_failable_census`'s `iov_table_overflow_gather` arm, measures
  the heap draw on purpose and now names `tr::mem::heap_source()` explicitly). **No runtime
  change** — the emitted code for every injecting caller is identical; what changes is that
  omitting the source is now a compile error instead of a silent global-heap path.
  Out-of-tree callers that omitted the argument pass `mem::heap_source()` to restore the old
  behaviour, or better, the store whose size is meant to bound their egress.

- **`graph_t::has_first_level_child`**
  ([#1303](https://github.com/avatarsd-llc/libtracer/issues/1303); added under
  [#373](https://github.com/avatarsd-llc/libtracer/issues/373)). The placeholder-inclusive
  first-level shadow test was published on `graph.hpp` for a transport-plane caller that never
  landed: nothing in `core/`, the tests, the benches or the bindings called it, so the only
  thing it did was hold `map_mutex_` in a function no one entered. This also discharges the
  open Consequence
  [ADR-0061](../docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md) recorded
  ("retires the #373 `has_first_level_child` shadow-guard … only after verifying no other
  first-level shadowing depends on it") — the verification is the caller sweep. Non-wire,
  no RFC. An out-of-tree
  embedder that called it can get the same answer from `find` plus a walk of
  `read_children` on the root — the difference is only that `find` excludes unregistered
  structural placeholders.

### Fixed

- **`vertex_t`'s members are reordered so the LKV slot cannot straddle a cache line —
  `sizeof(vertex_t)` is unchanged, but the object's INTERNAL layout is not**
  ([#1285](https://github.com/avatarsd-llc/libtracer/issues/1285)). **ABI note:** the private
  members are reordered, so any object file compiled against an older `vertex.hpp` must be
  rebuilt. No declaration, signature, or size changes: 96 B on x86-64 and 72 B on rv32, exactly
  as the #361 RAM ratchets pin them. `lkv_` — the `std::atomic<std::shared_ptr<const rope_t>>`
  the write hot path contends on — is 16 bytes wide with an alignment of only 8, and it sat at
  offset 24 because `name_` (24 B) led the member block. glibc hands out 16-byte-aligned blocks,
  so a vertex lands at `address % 64 ∈ {0, 16, 32, 48}`, and at `32` the slot's two words fall on
  DIFFERENT 64-byte cache lines. This libstdc++ keeps the slot's spin lock as the LSB of its
  second word (there is no address-hashed lock table), so under N-way contention that placement
  dirties two lines per publish instead of one: measured **×0.34 throughput** with **1.9× the
  cache misses and 2.2× the cache references** on the 8-thread single-vertex write arm, i.e. a
  one-in-four allocator coin-flip that cost 3×. Moving `lkv_` to the head of the member block
  puts it at offset **0** on both ABIs, and a 16-byte object starting on a 16-byte boundary
  cannot cross a 64-byte one — the straddle is now unreachable for *every* glibc placement. A
  new `vertex_layout_gate_t` static-asserts `offsetof(vertex_t, lkv_) % 16 == 0` in the header,
  beside the size ratchets, so every target evaluates it under its own binding and no future
  member edit can silently reintroduce the straddle. The reorder is deliberately NOT padding:
  `alignas(16)` on the member would leave an 8-byte hole and spend the ratchet. 64-byte-aligning
  the vertex *allocation* — which would additionally lift `own_subs_`/`listeners_above_` off the
  contended line, at a real RAM cost — is explicitly out of scope here and belongs to
  [ADR-0079](../docs/adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md)'s
  placement store.

- **The pre-auth WebTransport QPACK decode no longer amplifies 16 KiB into ~1.5 MiB of
  throwing heap** ([#1305](https://github.com/avatarsd-llc/libtracer/issues/1305)). No public
  API change: `wt_h3.hpp` is module-private. The extended-CONNECT field section an
  UNAUTHENTICATED peer sends is capped in BYTES (`kMaxHandshakeBytes`, 16 KiB) while the
  decoder's cost was per ELEMENT — the cheapest representation is a one-byte Indexed Field
  Line, so 16 KiB became ~16 400 owning `header_t` elements (two `std::string`s each) grown by
  bare `push_back` on an msquic worker thread, reached from libmsquic's non-unwindable C
  frames where a `bad_alloc` is `std::terminate` in practice. Two changes, in the order that
  matters: `decode_field_section` now enforces an element ceiling
  (`kMaxFieldSectionHeaders = 32`, against the five field lines the handshake needs), which is
  the semantic fix because only a count bound closes a count gap; and the decode is now
  NON-OWNING — field lines are `std::string_view`s into the constexpr static table, into the
  caller's input buffer, or into a caller-owned fixed Huffman scratch
  (`kMaxFieldSectionScratch`, 4 KiB) supplied as a `wt_h3::field_section_t`. The decoder
  allocates nothing at all now; every refusal returns `nullopt` and is handled by the existing
  stream-scoped refusal path, matching #919's disposition. Both roles are covered — the LISTEN
  side's CONNECT request and the DIAL side's CONNECT response go through the same decoder.

- **The MINIMAL module set builds its tests — `core/tests` is gated on the same module
  options the library gates its sources on**
  ([#1293](https://github.com/avatarsd-llc/libtracer/issues/1293);
  [ADR-0047](../docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md)). No API change: a build-system
  fix. `core/CMakeLists.txt` compiles the net/routing plane and each transport only when its
  option is ON, while every target in `core/tests/CMakeLists.txt` was unconditional — so
  `-DLIBTRACER_NET_PLANE=OFF` with all four transports OFF configured, compiled the library,
  and then failed to LINK with hundreds of undefined references across 55 test and helper
  targets. Each of those targets now carries the option(s) that build the symbols it names
  (target-level guards, not in-source `#ifdef`s), and a new `build-test-minimal-set` leg in
  `core-ci.yml` configures that set with `BUILD_TESTING=ON` and runs its reduced ctest suite,
  so the configuration an MCU consumer selects is covered rather than assumed. The default
  all-ON build registers exactly the same tests as before.

- **A config-constructed connection publishes the liveness of the role its socket was actually
  built with** (drive-by, [#1302](https://github.com/avatarsd-llc/libtracer/issues/1302)).
  Creation published `UP`/`LISTENING` from the CATALOG TYPE's default role while handing the
  factory the EFFECTIVE role from `conn_settings_t`. They differ exactly when a `:children[]`
  SPEC overrode `role` in its config — so a `SPEC{type: "client", config{role: LISTEN}}` bound a
  listen socket and then reported `UP` over it. Both reads are now the effective role. No
  signature change; the endpoint door cannot reach this case at all, since there the role is
  positional.

- **The per-link subscriber index's insert is IDEMPOTENT, as its contract has always said —
  so a peer that renews a subscription stops paying arena RAM and a sort for it**
  ([#1266](https://github.com/avatarsd-llc/libtracer/issues/1266), follow-up to
  [#1071](https://github.com/avatarsd-llc/libtracer/issues/1071) / PR #1263). No signature
  changes; the behaviour behind `graph_t::link_edge_candidates` and both eviction entry points
  does. `graph_t::index_link_vertex` documented that "a vertex already listed for `link` is not
  listed twice", and did not do it: it appended unconditionally and squashed duplicates in a
  later amortized compaction, so a link's candidate list oscillated between D and 2D+8 entries
  forever — an arena reallocation whenever it outgrew capacity plus an `O(D log D)` sort every
  D+8 subscribes, for no distinct vertex gained. `link_edge_candidates` could not reveal the
  gap because it compacts before reporting, which is how the claim survived. The list is now
  `[0, compacted)` sorted plus a tail bounded by the compaction floor, and membership is a
  binary search plus a bounded scan — no memmove, so the sorted-insert form #1071 rejected
  (+19% on the subscribe path) is not reintroduced. Measured on the pinned host, 4 links / 8
  vertices with each subscription renewed ten times: index arena footprint **1552 B → 784 B**
  (peak 1680 → 816, allocator churn 2544 → 1008), i.e. it no longer depends on how often a peer
  renews. Subscribe path 0.955–0.999 against `main`, at or below its paired A/A null at 4 / 8 /
  16 / 32 / 65 links; departure and delivery unchanged. **`#1266`'s premise is not confirmed**:
  ablation puts the name lookup (mutex + hash + `unordered_map` find) at 24–39 ns of the index's
  52–69 ns, so interning the link key — the issue's proposal — cannot reach the ~5 ns it
  predicts, and a prototype that did intern it measured no faster and cost 1.2 KB of text.

- **Host stream sends no longer block INDEFINITELY on a stalled peer, and no longer hold the
  write mutex while they do** ([#838](https://github.com/avatarsd-llc/libtracer/issues/838);
  the host twin of [#835](https://github.com/avatarsd-llc/libtracer/issues/835) / PR #837).
  `tcp_transport_t::send`, `transport_ws_client::send` and both servers' per-peer writes held
  `write_m_` across a fully blocking `write_all_iov`/`write_all` with **no `SO_SNDTIMEO`
  anywhere**. A peer that was stalled-not-dead — alive, socket open, TCP receive window full —
  parked the SENDING APPLICATION THREAD inside that syscall forever, and because deliveries run
  on the writer's thread (`write` → `fan_out` → `deliver_remote` → `send`) one such subscriber
  froze the writing application and everything queued behind the mutex with it. Now: every
  stream socket gets a bounded `SO_SNDTIMEO` at admission/dial, and every record carries a
  monotonic **deadline** derived from the peer LIVENESS WINDOW — so the bound is on the record
  (and therefore on the lock hold), not merely on one syscall. `EAGAIN`/`EWOULDBLOCK` became its
  own write-fault class (`STALLED`); it previously fell into `MALFORMED_CALL`, which would have
  mis-filed a stalled peer as a libtracer defect. A stalled record is **counted, never silently
  dropped** (`stalled_tx()`, and the link's existing `dropped_tx()`), and the peer that caused it
  is closed — immediately if the record half-reached the wire (that stream's framing is desynced
  permanently), otherwise at `kMaxConsecutiveStalls` (3) consecutive stalls with no completed
  record in between, #837's brokenness-detector trichotomy transferred verbatim. The close is a
  `shutdown(SHUT_RDWR)`, so the recv/poll thread runs the ORDINARY remote-departure teardown and
  its departure notification, and a strike never outlives the session that earned it.
  **In scope:** tcp client + listen, ws client, and both servers' per-peer/broadcast writes.
  **Out of scope, deliberately:** quic / webtransport (native flow control) and CAN (already has
  `peer_ttl`), per the issue's scope ruling.

## [0.12.0] — 2026-08-14

### Added

- **`wire::type_t::PATH_REF_REVERSE` (`0x15`) — the reverse-direction bound-path list's own
  type code** ([#1260](https://github.com/avatarsd-llc/libtracer/issues/1260); RFC-0024 §7.1
  **amendment 2**). **Wire change.** The reverse list a mint-flagged request accumulates is
  now identified by this code rather than by amendment 1's positional rule ("the only trailing
  child"), which is **withdrawn**. Its body grammar is `PATH_REF`'s exactly — `opt.PL`/`opt.LL`
  0, `length` a multiple of 8, ≤ 255 elements — so the codec's shape gate now asks
  `wire::is_path_ref_type()` (new; one masked compare, `0x15` being adjacent to `0x14`) instead
  of testing one code. `wire::emit_path_ref` / `emit_path_ref_into` gained a defaulted `type_t`
  parameter (source-compatible; both refuse a type that is neither bound-path code). The
  reference core neither emits nor accepts the old spelling: amendment 1 shipped inside this
  same unreleased window, so no released frame carries it. New conformance vectors
  `fwd/fwd-reverse-mint` and `path-ref/reverse-len-not-multiple-of-8` (76 → 78); the origin's
  own frame is unchanged and `fwd/fwd-mint-request`'s bytes stand. Cost on the hop: **zero
  instructions** — `peek_trailing_mint` already compared each tail child's type byte, so the
  discriminant is the same compare against a different constant, with no cursor read and no
  body peek added. The un-foreclosed shape comes back with it: a mint-flagged `WRITE` whose
  payload is itself a raw `PATH_REF` now keeps its payload.

- **`graph_t::link_edge_candidates(std::string_view)` — the cost of a link's departure, made
  observable** ([#1071](https://github.com/avatarsd-llc/libtracer/issues/1071)). Reports how
  many vertices a `evict_link_edges` for that link would visit. A diagnostic, and the
  instrument the scoping property is asserted on; it reads the per-link index, which is a
  deliberate SUPERSET of the vertices actually holding edges (an individual unsubscribe does
  not un-index), so it is an upper bound on work and must not be read as an edge count.

### Changed

- **`net::peek_reply_mint` / `net::reply_mint_t` are renamed `net::peek_trailing_mint` /
  `net::trailing_mint_t`** ([#1260](https://github.com/avatarsd-llc/libtracer/issues/1260)).
  The function has served both directions since the reverse mint landed, and now takes the
  list's `wire::type_t` as its discriminant (defaulted to `PATH_REF`, so a reply-side caller is
  unchanged but for the name). "Reply" in the old name was already wrong, and would have been
  actively misleading next to a request-only type code.

- **A link's departure now costs that link's own subscribed vertices instead of the graph's
  whole subscribed set** ([#1071](https://github.com/avatarsd-llc/libtracer/issues/1071)).
  `graph_t::evict_link_edges` and `evict_route_edges` previously opened with a pre-order walk
  of every vertex in the graph, collecting each one holding any subscriber edge into a
  **global-heap** `std::vector` sized to that set, and only then tested the link per edge. One
  peer hanging up was therefore priced by every *other* peer's subscriptions — paid
  synchronously, inside the session's free-context callback, which on the ESP-IDF
  HTTP-server-hosted link is the single task that owns accept, receive and close for every
  other socket on that server. Both entry points now take their candidates from a per-link
  index maintained at the one subscriber-admission door, so the walk and its allocation are
  gone: the departure path allocates nothing at all. Behaviour is unchanged — same counts,
  same RFC-0005 unwind, same two-phase locking per vertex, same empty-key no-op (#1056), and
  the same subscribe-racing-teardown window, because the index is populated at exactly the
  `own_subs` bump the replaced walk's predicate read. The index is keyed by the edge's
  admitted-over spelling (`link`, falling back to `caller`), which is what keeps
  field-write-admitted edges (#943) reachable by a teardown.

- **The reverse-direction mint is implemented — a recycled `p<slot>` session no longer
  inherits the dead session's deliveries**
  ([#1223](https://github.com/avatarsd-llc/libtracer/issues/1223) steps 3+4 of 5, the
  confirmed-by-execution disclosure's close; RFC-0024 §7.1 **amendment 1**, spelling (b),
  forwarder-contributed). A forwarding hop relaying a mint-flagged request now contributes
  its arrival identity's element — the connection vertex for a point-to-point link, the
  accepted session's #1254 identity anchor for a bus session — to the request's trailing
  reverse `PATH_REF` child (`rebuild_request_reverse_mint`, the out-of-line mirror of the
  reply-side mint; create / extend / strip-whole per erratum 1 direction-reversed). The
  responder completes the list with its own element (`op_resolver_t::on_reverse_ref`, a new
  injected transport-plane seam) and stores it with the edge
  (`subscriber_remote_t::reverse_route`; `graph_t::subscribe_wire` gained a defaulted
  `reverse_route` parameter — source-compatible). Every delivery then consumes element 0
  locally (validated bounds + generation + ACL) and rides the bound form; the LAST hop
  dereferences the session anchor (`graph_t::session_anchor_route`, new) and egresses to
  the session with the `dst` re-headed canonical, so **an origin client's frames are
  bit-identical in both directions** — the `fwd/fwd-mint-request` conformance vector's
  bytes stand. A dead session's element fails the generation compare at the consuming hop,
  which drops AND answers §5.3's NACK — the same addressed echo step 5 correlates, so the
  stale edge retires on its first post-mortem delivery (`vertex_t::evict_route_edges`
  gained a `bound_echo` arm matching the echoed `PATH_REF` element-wise). An unflagged
  subscribe, an incomplete list, or an unbindable hop all degrade to the canonical-only
  subscription, byte-identical to before.

- **A delivery refused `tr::path::invalid` now reclaims the exact subscriber edge that
  stored the refused route** ([#1223](https://github.com/avatarsd-llc/libtracer/issues/1223)
  step 5 of 5 — the leak's independent close, zero new wire bytes). The RFC-0020
  bus-residual reject already echoes the refused route back as the reply `src` "so it can
  correlate"; the producer's router now correlates: every terminating `FWD{REPLY}` is peeked
  by offset (allocation-free; non-refusals bail after at most five header reads), and an
  addressed `STATUS{ERROR{tr::path::invalid}}` evicts the edge(s) whose delivery link AND
  stored return route both match, byte-equal. New public surface:
  `graph_t::evict_route_edges(link, route_wire)` and `vertex_t::evict_route_edges` — the
  narrow siblings of the link-teardown eviction, same two-phase locking, same RFC-0005
  unwind. RFC-0009 §D.4 is not contradicted: its premise ("not an error the producer
  observes") is precisely what RFC-0020 changed for this case, and the reclaim acts only on
  that observation. A stale route whose terminal name has been RECYCLED still delivers (the
  disclosure) — that is steps 3–4's validation work, not this seam's.

- **An accepted `slot_server_t` session now holds a session identity anchor — a vertex-map
  slot with a saturating generation, revived in place on slot reuse**
  ([#1223](https://github.com/avatarsd-llc/libtracer/issues/1223) step 2 of 5, realizing
  [ADR-0044](../docs/adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)'s
  2026-08-13 amendment). New public surface: `graph_t::register_session_anchor` /
  `find_session_anchor` / `session_anchor_slots`, `bus_link_t::set_peer_up_notifier` (the
  arrival twin of `set_peer_down_notifier`), `fwd_router_t::session_anchor_id`, and the
  protected `slot_server_t::publish_peer_up`. **Nothing observable changes**: no wire bytes,
  no `enumerate_peers` output, no `:children[]` bytes, no new address.
  An anchor is an **identity anchor, not an addressable node** — it hangs off a private
  structural root the addressable root cannot reach, so `find`, path descent and every
  `:children[]` listing are untouched (a bus mount's members stay exactly what
  `enumerate_peers` synthesizes, and RFC-0020 §3's "MUST NOT resolve the residual against
  its local graph" keeps its premise). What it has is the one thing it is for: a slot in the
  pinned, insert-only vertex map, so a later step can mint an RFC-0024 element naming the
  session and a route naming a DEAD session fails the §5.1 generation check.
  Bounded across churn by construction: a recycled `p<slot>` revives the SAME `vertex_t` in
  the SAME slot with only the generation bumped. Measured on `bench_forward_heap`:
  `session_anchor` 144 B live / 6 allocs (vs. a bare `vertex` at 128 B / 3), and
  `session_anchor_churn` **0 live bytes** — a retire+revive allocates nothing net.
  Only an ACCEPTING listener fires the arrival seam (`slot_server_t`, and the ESP
  `httpd_ws_link_t` at parity), so an announce-census bus — CAN — grows no vertices and
  keeps ADR-0044 §Decision 1 in full force.

- **`rope_cursor::for_each_span` now keeps its window containment in RELEASE builds, and
  `rope_cursor`/`span_cursor` gain `poisoned()`**
  ([#986](https://github.com/avatarsd-llc/libtracer/issues/986)). A bulk feed that
  overshoots the cursor's window is **truncated to it** instead of served, and the rope
  cursor latches; `parse_header` maps that latch to `FRAME_TRUNCATED` and the forward
  plane drops the frame rather than emitting a short one. Previously the precondition was
  a debug `assert`, so a release build fed the caller real bytes from outside the window
  and reported success — the one violation no sanitizer can see, because the overshot
  bytes really are in the link chain. `span_cursor::poisoned()` is a `static constexpr
  false` and its hot path is unchanged: giving the contiguous cursor the same treatment
  measured `compact-forward` x0.66 deliveries/s and `compact-terminus` x0.82 (4/4 and 3/4
  interleaved pairs, disjoint ranges), and a latency regression on the delivery path is an
  automatic reject. The shipped shape measures x1.00 on that A/B, +0 B on every pinned
  symbol and +0 B on the Cortex-M0 P0 footprint. Point reads (`byte_at`, `load_le`) and
  `region` keep the debug-only contract — see the frame-codec module docs for the recorded
  decision and the full table.

- **`length_prefix_framer::feed` reports each backpressure drop AT DECISION TIME through a
  new `on_drop` callback, and `result_t::dropped` is gone**
  ([#1255](https://github.com/avatarsd-llc/libtracer/issues/1255)). `feed` takes one more
  argument — a `void()` callable invoked once per frame shed to backpressure, the moment the
  drop is decided — and `result_t` now carries only `malformed`, the single outcome that
  STOPS the feed. The drop POLICY is untouched (what is shed and when is exactly #932's
  rules), and `dropped_rx()` keeps its documented meaning; only the moment the count becomes
  visible moves earlier. It had to move because a per-chunk tally can only be read after the
  whole chunk is processed: one chunk routinely carries a dropped frame AND a delivered one
  (small frames coalesce into a single msquic RECEIVE), so the count was published *after*
  the delivery that followed the drop, and a receiver woken by that frame could read a stale
  `dropped_rx()`. The ordering is now a documented contract of `feed` — `on_drop` always
  precedes the `on_frame` of any later frame — and a relaxed atomic is enough to carry it,
  because the increment is sequenced-before the delivery and rides whatever release/acquire
  edge publishes the frame. Callers that ignore drops pass a no-op lambda; the non-drop path
  is unchanged and allocation-free (the callback is a template parameter, invoked only on the
  DROP arm). Both in-tree consumers — the shared msquic endpoint behind `quic_transport_t` /
  `webtransport_transport_t`, and `transport_tcp_server` — now count inline, which makes the
  framed transports consistent with `transport_udp` / `transport_ws` / `transport_can`, all
  of which already counted at the drop site.

- **`rope_t::flatten` is out-of-line again, and `rope_t::materialize` is inlinable again —
  25–48% back on every `materialize` path**
  ([#1250](https://github.com/avatarsd-llc/libtracer/issues/1250)). No signature, no
  semantics and no error channel changes: `flatten`, `try_flatten`, `try_materialize` and
  `flatten_err_t` are exactly what [#917](https://github.com/avatarsd-llc/libtracer/issues/917)
  shipped, and an OOM still reads as `NO_MEMORY` backpressure rather than a malformed frame.
  What changed is the *shape*. [#1210](https://github.com/avatarsd-llc/libtracer/issues/1210)
  had defined `flatten` in the header as a wrapper that unwrapped `try_flatten`'s
  `expected`, which cost twice over: the temp was `const`, so `std::move(*r)` yielded a
  `const view_t&&` and bound the **copy** constructor (two extra atomic refcount RMWs per
  flatten), and the body's 32 B `expected` slot plus its stack canary pushed the two-line
  `materialize` past GCC's inline threshold — so even the **single-link zero-copy arm**,
  which never flattens, started paying an out-of-line call. Both wrappers now delegate to
  one out-of-line core (`flatten_core`) that returns the view straight into the caller's
  return slot. Measured on this host, 15 interleaved A/B/C triples with rotating start
  order and `taskset`: `lkv-store-pool/64` 0.521x before → **0.994x**, `lkv-store-heap/64`
  0.758x → **0.989x**, both 1024 B twins 0.585x/0.726x → **0.996x**, against `lkv-alloc-*`
  null-control arms reading 0.987–1.003x in the same triples. `lkv-store-heap/64/1/1` and
  `lkv-store-pool/64/1/1` are now `perf_gate.py` POINTS (twelve, from ten) — they are the
  only gated legs downstream of `materialize`, which is why the loss shipped green.
- **Build configuration is overridden by a `libtracer/config_override.hpp` fragment, not by
  CMake cache variables (#1142, ADR-0068 §Erratum 1).** `libtracer/config.hpp` is now ordinary
  hand-written C++ and the only place a default core's own build reads is spelled: core's
  `configure_file`, the configure-time drift gate that byte-compared the template's default
  render against the checked-in copy, and five cache variables
  (`LIBTRACER_VERTEX_LOCK_STRIPES`, `LIBTRACER_CACHE_LINE_BYTES`, `LIBTRACER_HAZARD_READER_SLOTS`,
  `LIBTRACER_EDGE_PIN_SLOTS`, `LIBTRACER_SPIN_WAIT_SAFE`) are
  **removed** — 78 lines of `core/CMakeLists.txt` existed to move six C++ constants into a C++
  header, and the gate existed only because the defaults were spelled twice. A target with
  non-default knobs now supplies a fragment earlier on the include path; `config.hpp` picks it
  up and uses the `config_t` it binds. The fragment INHERITS `default_config_t` and states only
  what differs, so a knob added later reaches every override at its new default — the property
  that makes the drift gate unnecessary rather than merely absent.
  **Transition, one release:** `-DLIBTRACER_ACL_FULL` and `-DLIBTRACER_LKV_SLOT` — the only two
  the CI matrix passes — keep working, with `core/CMakeLists.txt` writing the fragment on their
  behalf and saying so at configure time. A NEW knob does not get a CMake variable.
- **`config.hpp.in` is deleted; the ESP-IDF component writes a fragment (#1244, ADR-0068
  §Erratum 1).** The template survived #1142 only as the ESP-IDF component's rendering source.
  That component now writes a `libtracer/config_override.hpp` stating the four knobs an ESP
  target changes — `kVertexLockStripes` (menuconfig), `kCacheLineBytes` (derived from
  `CONFIG_FREERTOS_UNICORE`), `kEdgePinSlots` and `kSpinWaitSafe` (derived from `IDF_TARGET`)
  — and inherits the rest, so it no longer carries a second copy of core's defaults and a knob
  added to `config.hpp` reaches an ESP build with no hand-mirroring. **No configured value
  changed** and the Kconfig option names are unchanged. `core/CMakeLists.txt`'s install step
  drops the `PATTERN "config.hpp.in" EXCLUDE` that existed only for the deleted file.
- **`kSpinWaitSafe` is a `default_config_t` member (#1142).** `tr::mem::kSpinWaitSafe` is now
  derived from `config_t` rather than being a loose `inline constexpr` literal. A knob outside
  the one named type cannot be reached by an override fragment at all, which is why this one
  had been stranded in the build system; this restores ADR-0070's rule. No value changed.

- **The ACE `access_mask` canonical wire width is u32 (RFC-0026, #993).** The published
  corpus disagreed with itself: reference 05 §`0x0A`, the `acl/acl-aces` conformance vector
  and the Rust builder spelled the mask u16 while `tr::graph::encode_acl` — the encoder
  behind every `:acl` read — emitted u32. The amendment names u32 canonical: the layout
  text and the vector now spell four bytes (vector re-cut, 179 → 183 bytes) and a new host
  test byte-compares `encode_acl` against the vector so the typed builder can never drift
  from it again. **No C++ API or behaviour change**: `encode_acl` already emitted u32, and
  `parse_acl`'s acceptance rule (narrower payloads zero-extend, #906) is untouched, so ACLs
  stored with the old two-byte spelling remain readable.

- **BREAKING (admission): `graph_t::subscribe_wire` refuses an EMPTY `return_route` with
  `INVALID_PATH` (#1055).** The door previously validated neither of its two remote-binding
  arguments, so an edge could be admitted carrying a link with no route to deliver over it.
  `graph_t::dispatch_edge` gates its remote leg on `!link.empty()` but hands the sink the
  *return route* untested, so such an edge emitted one `FWD{WRITE}` per publish whose `dst`
  was a zero-byte PATH — on the COMPACT arm it became the label key, on the full-route arm it
  was spliced into the iov and counted into the body length. Both in-tree callers already
  satisfied the new rule (the resolver rejects a failed route copy as `BACKPRESSURE`, and
  `fwd_router_t::subscribe_toward` refuses an empty residual as `INVALID_PATH`), so no wire
  door changes behaviour; only an embedder driving the public door directly with an empty
  route is affected, and that call was already producing malformed deliveries. The gate is at
  the door rather than in the fan-out body deliberately: `dispatch_edge` is the wide
  fan-out loop's per-edge cost and is kept inlinable, so this adds nothing to the publish
  path. `subscriber_remote_t::return_route`'s declaration — which claimed being *populated*
  is what makes a subscriber remote, while the code tests `link` — now states the invariant
  the door establishes instead.

- **The hazard domain's orphan guarantee is weakened to what it can deliver, and the
  injected-resource lifetime contract tightened to compensate**
  ([#1037](https://github.com/avatarsd-llc/libtracer/issues/1037)). `detail_hp::retire_and_flush`
  documented that a value written by a thread that has since exited "is still released when its
  slot dies"; its `orphans` term is a relaxed check-then-act, so a `~participant_t` pushing that
  thread's list concurrently with the load is missed and those nodes wait for the next domain
  scan. No ordering inside that function recovers the strong reading — the adopting `scan` races
  the *same* push, so re-probing after the ticket moves the window rather than closing it — so
  the promise now reads **"released when the slot dies OR at the next domain scan"**.
  Correspondingly, [ADR-0039](../docs/adr/0039-pmr-memory-model-host-aligned-allocation.md)
  §Erratum 8 requires an injected `std::pmr::memory_resource` to outlive every value allocated
  from it, **every thread that wrote through it, and a domain quiescence point after the last
  such thread exits**. **No behaviour change and no API change**; embedders injecting a scoped
  arena under `hazard_slot_t` gain one stated obligation. The default `sp_atomic_slot_t` has no
  orphan path and is unaffected; on the process-lifetime heap a host build uses, a missed orphan
  was only ever a deferral. `lkv_slot_test` pins the weakened guarantee non-vacuously.

  delivery-drop counters (#1068).** A deliverer outside the graph — the net plane, resolving a
  COMPACT label to a vertex and writing it — performs deliveries no counting site inside
  `graph_t` can see, so the drops on that path were invisible in every direction. The new
  method adds to the existing `delivery_drops()` totals; `external_drop_t` names only
  `NO_TARGET` and `OUT_OF_MEMORY`, deliberately not `DENIED` (see below). A method rather than
  a friendship because the counters are a published surface while the internal drop sites are
  not. Additive: no existing member changed signature.

- **The ACE `access_mask` canonical wire width is u32 (RFC-0026, #993).** The published
  corpus disagreed with itself: reference 05 §`0x0A`, the `acl/acl-aces` conformance vector
  and the Rust builder spelled the mask u16 while `tr::graph::encode_acl` — the encoder
  behind every `:acl` read — emitted u32. The amendment names u32 canonical: the layout
  text and the vector now spell four bytes (vector re-cut, 179 → 183 bytes) and a new host
  test byte-compares `encode_acl` against the vector so the typed builder can never drift
  from it again. **No C++ API or behaviour change**: `encode_acl` already emitted u32, and
  `parse_acl`'s acceptance rule (narrower payloads zero-extend, #906) is untouched, so ACLs
  stored with the old two-byte spelling remain readable.

- **BREAKING (admission): `graph_t::subscribe_wire` refuses an EMPTY `return_route` with
  `INVALID_PATH` (#1055).** The door previously validated neither of its two remote-binding
  arguments, so an edge could be admitted carrying a link with no route to deliver over it.
  `graph_t::dispatch_edge` gates its remote leg on `!link.empty()` but hands the sink the
  *return route* untested, so such an edge emitted one `FWD{WRITE}` per publish whose `dst`
  was a zero-byte PATH — on the COMPACT arm it became the label key, on the full-route arm it
  was spliced into the iov and counted into the body length. Both in-tree callers already
  satisfied the new rule (the resolver rejects a failed route copy as `BACKPRESSURE`, and
  `fwd_router_t::subscribe_toward` refuses an empty residual as `INVALID_PATH`), so no wire
  door changes behaviour; only an embedder driving the public door directly with an empty
  route is affected, and that call was already producing malformed deliveries. The gate is at
  the door rather than in the fan-out body deliberately: `dispatch_edge` is the wide
  fan-out loop's per-edge cost and is kept inlinable, so this adds nothing to the publish
  path. `subscriber_remote_t::return_route`'s declaration — which claimed being *populated*
  is what makes a subscriber remote, while the code tests `link` — now states the invariant
  the door establishes instead.

- **The hazard domain's orphan guarantee is weakened to what it can deliver, and the
  injected-resource lifetime contract tightened to compensate**
  ([#1037](https://github.com/avatarsd-llc/libtracer/issues/1037)). `detail_hp::retire_and_flush`
  documented that a value written by a thread that has since exited "is still released when its
  slot dies"; its `orphans` term is a relaxed check-then-act, so a `~participant_t` pushing that
  thread's list concurrently with the load is missed and those nodes wait for the next domain
  scan. No ordering inside that function recovers the strong reading — the adopting `scan` races
  the *same* push, so re-probing after the ticket moves the window rather than closing it — so
  the promise now reads **"released when the slot dies OR at the next domain scan"**.
  Correspondingly, [ADR-0039](../docs/adr/0039-pmr-memory-model-host-aligned-allocation.md)
  §Erratum 8 requires an injected `std::pmr::memory_resource` to outlive every value allocated
  from it, **every thread that wrote through it, and a domain quiescence point after the last
  such thread exits**. **No behaviour change and no API change**; embedders injecting a scoped
  arena under `hazard_slot_t` gain one stated obligation. The default `sp_atomic_slot_t` has no
  orphan path and is unaffected; on the process-lifetime heap a host build uses, a missed orphan
  was only ever a deferral. `lkv_slot_test` pins the weakened guarantee non-vacuously.

- **`delivery_drops().out_of_memory` now counts the two sheds that happen BEFORE the fan-out
  (#1003).** A STREAM write whose history append was shed under allocation pressure abandoned
  its entire fan-out with every counter reading a zero delta: for a STREAM the ring drain *is*
  the fan-out, so the skipped entry is a delivery every subscriber loses, and the eager
  delivery then drained zero entries and returned before one edge was snapshotted — the loss
  never reached the dispatch plane's counting door. The `mark_pending` OOM legs (key render,
  set-node probe) shed a deferred `IF_NEWER` delivery just as silently, while their own comment
  claimed equivalence to "an eager delivery leg under the same pressure" that had in fact been
  counted since #896. Both now count at that same **one-per-subscriber** width. The write still
  answers `SUCCESS` in both cases — the value publish landed, and a stream's history is
  bounded-lossy by contract (RFC-0008 §E) — so no result code changes; what changes is that
  `delivery_drops()` may now move where it previously read zero. A branch **notify** fans its
  slice out eagerly and flushes the drain cursor, so its shed costs history rather than a
  delivery and is deliberately *not* counted. Threading the tally through the fan-out grows
  `dispatch_edge_target` by **70 B** (481 -> 551), so the symbol ratchet is re-pinned — priced
  first, per #1199: an interleaved same-runner A/B measured x1.00 on the wide fan-out
  (`inproc/64/1024/1`, p50 and deliveries/s alike) and x1.00-x1.02 on every other delivery
  loop, with identical `mem:` live bytes and block counts on all five shapes.
- **`vertex_t::store` takes an optional `store_drops_t*` out-parameter** reporting what the
  store shed, in the shape `snapshot_drops_t` established: the storage layer owns no counters,
  so it reports a tally by reference and `graph_t` folds it through its single counting door.
  Purely additive and defaulted — existing calls compile and behave unchanged.

- **`delivery_drops().denied` now counts an ACL refusal on EVERY plane (#1068).** It counted
  only a subscription edge's fan-in denial; it is now counted at `write_impl`'s WRITE gate, so
  a plain API `write`, a `FWD{WRITE}` terminus and both `COMPACT` terminus arms all count
  there too. This is a **meaning change to a public counter**, not just more coverage: an API
  caller that receives `PERMISSION_DENIED` now also advances it, so `denied` reads as
  *refusals* rather than *refusals nobody was told about* — a number whose value depended on
  which door a refusal came through could not be summed. A deployment alarming on `denied`
  should expect it to move for locally-refused writes it previously did not count.
  Deliberately NOT folded in: `assign`, a control-plane field write, and a denied READ.
  The motivating gap was an ACL-denied COMPACT delivery, which was silent in every direction —
  no counter, no sink, and (unchanged, by design) no wire signal, since `HANDLE_NACK` means
  "unknown label" and answering a denial with one would prompt an endless re-advertise.

### Removed

- **BREAKING: `view::view_can_frames_t` — replaced by the free functions
  `view::can_frame_count(payload, mode)` and `view::can_frame_at(payload, mode, i)`**
  ([#932](https://github.com/avatarsd-llc/libtracer/issues/932), `view_can.hpp`). The class,
  its static `split` factory and its `mode()` / `frame_count()` / `frame(i)` / `to_rope()`
  accessors are gone. #1110 had already deleted the `std::vector<view_t>` window table these
  wrapped, leaving a value that held the payload, the mode, and a memo of one ceiling
  division — state every caller already had. The sole production consumer
  (`transport_can::send_impl`) used only the count and the i-th window, so it now calls the
  two free functions directly; `mode()` and `to_rope()` had no production caller at all.
  **Migration:** `split(p, m).frame_count()` → `can_frame_count(p, m)`;
  `split(p, m).frame(i)` → `can_frame_at(p, m, i)`; `mode()` → the mode you passed in. There
  is deliberately no `can_join`/`to_rope` replacement — the production far side is
  `net::can_reassembly_t`, and the only callers of `to_rope()` were round-trip tests, which
  now chain the windows into a `rope_t` locally. Behaviour, wire bytes and cost are
  unchanged: the framing stays zero-copy, O(1) per window, allocation-free and infallible,
  and `view_can.hpp` no longer includes `rope.hpp`.

- **`vertex_t::try_edge_view_of`** — private, zero callers since the published-edge copy loop
  (#635) took over the writer-thread fan-out snapshot it documented itself as serving. Its
  replacement is `try_copy_published`.

### Fixed

- **`assign` no longer loses a delivery to a subscribe that races it on a weakly-ordered
  target ([#1140](https://github.com/avatarsd-llc/libtracer/issues/1140)).** `mark_pending`
  — the deferred half of the write path, and a **skip** gate: a vertex that misses its mark
  enters no sweep set, so the next covering `propagate` delivers it nowhere and only a later
  write can re-mark it — read the vertex's own subscriber count through the **relaxed**
  `vertex_t::own_subs()`. It now reads `vertex_t::own_subs_ordered()`, the `seq_cst` half of
  the Dekker pair `admit_subscriber`'s subscriber-count bump already holds up, which is the
  same read #635 gave the eager half (`fan_out`). Without it the two linearizations
  contradict each other on **the same vertex**: the publisher's skip says
  write-before-subscribe while ADR-0049's durability latch hands the new subscriber the
  **pre-write** value, and the assigned value reaches nobody at all. Reachable on
  **aarch64 and rv32** (a shipped target — esp32c3/c6); latent on x86-64, where the `seq_cst`
  LKV store lowers to a locked `xchg` and orders the later relaxed load in hardware. The
  ancestor half (`listeners_above`) stays relaxed by the #854 ruling. Public surface: only
  `vertex_t::own_subs_ordered`'s contract, which now names both skip sites. A new
  `graph_test` guard covers the **program-order** half (the count rising before the slot);
  the memory-order half is covered by a new weakly-ordered `ubuntu-24.04-arm` CI leg, since
  no x86-64 test can observe it.

### Documentation

- **`bus_link_t::peer_link`'s endpoint is documented as RESOLVE-PER-USE
  ([#1153](https://github.com/avatarsd-llc/libtracer/issues/1153)).** The contract
  promised pointer validity for the link's lifetime, which readers took for peer
  identity; those are different guarantees and only the first holds for every kind.
  Where a kind names peers POSITIONALLY (`slot_server_t`'s `p<slot>`, and slots recycle
  in place), an endpoint cached across the named peer's departure addresses whoever
  inherits the slot, and the endpoint's own `open` check is satisfied by that stranger.
  Identity-derived names are immune — `transport_can`'s `n<node-id>` binds name, table
  key and endpoint to one identity no other peer can inherit, now pinned by a test that
  caches a pointer across an expiry, a different peer's arrival and the node's return.
  **No behaviour change**: no production caller caches the pointer, so this states a
  contract rather than fixing a defect. The structural fix — explicit per-session naming
  across both server planes — remains open on #1013.

- RFC-0016 §B erratum ([#1030](https://github.com/avatarsd-llc/libtracer/issues/1030)):
  a composed branch-read reply may legitimately carry **zero child records** — produced
  deterministically when the READ-ACL prune removes every child, and transiently when a
  read races the retirement of the last registered child (the lock-free fork of #652).
  No code change; the shape was already legal and is now written down.
  `has_registered_child`'s contract note, the `read_fork_test` race harness (free-running
  reads are now shape-classified) and a deterministic `subtree_read_test` boundary case
  pin it.

## [0.11.0] — 2026-08-13

### Added

- **`quic_transport_t::dropped_tx()` and `webtransport_transport_t::dropped_tx()` — the msquic
  egress counts its shed frames (#932).** Both transports reported `drop_stats().dropped_tx`
  as a hardcoded `0`, which a generic `transport_t*` holder cannot tell apart from "nothing
  was dropped" — the last gap in the drop-counter seam #1213 hoisted. All three shed paths in
  the shared `msquic_endpoint_t` now count: a record over `kMaxFrame` (both `send` overloads,
  refused before any stream is consulted), a send with no live peer stream (dialing or torn
  down), and a `StreamSend` msquic refused. H3 handshake material sent through `send_raw` is
  deliberately **not** counted — a refused handshake write is a setup failure the handshake
  surfaces itself, not a frame the router believed it sent. No behaviour change: the same
  frames are shed as before, they are merely observable now.

- **`wire::key_view_t` gains record accessors — `record_end`, `record_from`, `record_t` and
  `record_cursor_t` (#888).** The NAME-record framing walk (4-byte header, u16 length, advance
  by `4 + len`) was hand-rolled at **eight** places, because the class offered no way to ask
  for a record: FOUR outside this header — `graph.cpp`'s Composite `segment_end`,
  `fwd_router.cpp`'s `subscribe_toward` **twice** (and its indexed accessor rescanned from
  offset 0 per segment), and `transport_vertex.cpp` (a near-verbatim copy of the already-public
  `last_segment`) — and FOUR inside it: `last_segment`, `parent`, `child_record_under` and
  `split_levels`. All eight now read the framing through `record_end`, which is the ONE
  place the decode is written — including the zero-length-record rejection (#932), which is
  now enforced once there instead of once per walk. Purely additive — no existing member
  changed signature or behaviour. `record_end(at)` answers the next record's offset, or `0`
  for ragged framing; `record_from(at)` adds the bounds and the payload span;
  `record_cursor_t` is the non-allocating INDEXED walk the strip-K mount descent asks by
  segment number, resuming instead of rescanning.

- **`config_reader_t::name_bytes(key)`** — the raw payload span of a `NAME` value child
  (the byte-span twin of `name()`, for values that are wire segments rather than text), and
  **`config_reader_t::settings(key)`** — the nested `SETTINGS` value child, or `nullptr`.
  Both run the same pair-consuming, last-well-formed-wins walk as every other accessor;
  they are the two accessors the L4 readers needed (#985).

- **`transport_can::dropped_presink()` — the named counter for the sink-install window
  (#1103, [ADR-0081](../docs/adr/0081-pre-sink-ingress-native-window-hold-or-named-drop-never-parked.md)
  §4).** A reassembled group that completes while the transport exists (link receiving, RX
  callback registered) but no receiver sink is installed yet — the span
  `transport_vertex_t::make_connection` opens between constructing the link and
  `fwd_router_t::add_child`, widened arbitrarily by graph map-lock contention — used to vanish
  into the empty `receiver_slot_t` with no counter moving. Reproduced through the production
  creation path over a real `vcan0`: with the window held 200 ms, 95 of 235 groups a bystander
  peer put on the bus were lost, and nothing named the loss. A bus cannot take either escape
  ADR-0081 offers — it has no per-peer flow control to hold bytes in, and withholding its RX
  callback would starve the liveness bookkeeping it drives (`last_heard`, the
  pending/reassembly sweeps) — so per §4 the group is dropped at the delivery seam and this
  distinctly named counter ticks: never parked in the library, never folded into `dropped_rx()`
  or `dropped_groups()`, never silent. Counts groups.

- **`tr::net::transport_vertex_t::is_structural(wire::key_view_t)` — the net plane names its
  own structural vertices (#1096).** `transport_vertex_t` mints two vertices nobody asked
  for: the net root (the `:children[]` creation target) and, lazily, each
  `<net_root>/<module>` segment a connection mounts under. Both are registered
  `role_t::STORED_VALUE` and carry no descriptor table, so an embedder walking
  `graph_t::for_each_vertex` saw them as ordinary value vertices someone forgot to describe —
  byte-identical `:schema` shape to a real leaf, differing only in the NAME. The predicate
  takes exactly the key `for_each_vertex` hands its callback, so no handle is unwrapped and
  no new `graph_t` accessor is added.
  **The answer is scoped to `transport_vertex_t` on purpose, and `graph_t` will never
  answer it**: an application's own structural vertex (a `/zone` holding nothing but children)
  is indistinguishable from a connection vertex on every graph-visible surface — same visit,
  same schema shape, same RFC-0016 composed branch read — so a graph-level predicate would be
  inventing an answer where there is no graph-visible basis for one. What the *library*
  minted, the library reports; what the *application* minted stays the application's business
  (ADR-0010). No `role_t::GROUP` enumerator was added — a role that names no read/write
  behaviour is not a role, and a new value in a public byte-wide enum breaks downstream
  `switch`es. Nothing under `docs/spec/` moved and no wire byte changed: the role has never
  been on the wire (reference/11 §outside the scope). Documented in
  [reference/11](../docs/reference/11-vertex-roles-and-aggregation.md) §structural vertices,
  with "structural vertex" minted as a [CONTEXT.md](../CONTEXT.md) term.
  **Name match, not provenance — a documented false positive.** Creation deduplicates against
  `graph_.find` and deliberately keeps no per-module minted set (commit `221ed983` deleted
  exactly that state), so a vertex an application registered at `<net_root>` or
  `<net_root>/<module>` *before* this object got there answers `true`. The predicate states a
  structural *position* of this net plane, not the identity of whoever registered it. The
  RFC-0014 per-module creator endpoint `/net/<module>/conn` (accepted, unimplemented) answers
  `false`: it is an addressable control surface with its own `:schema` catalog, not a
  grouping segment.

- **`transport_t::drop_stats()` — the interface-level shed-frame seam (#932).** Every transport
  counted *some* drops behind its own concrete accessors, so a consumer holding a
  `transport_t*` could observe none of them and swapping tcp for ws or CAN silently lost all
  drop observability. `transport_drop_stats_t {dropped_rx, malformed_rx, dropped_tx}` is the one
  shape they all answer with; the base returns all-zero (the honest answer for a link that
  counts nothing) and tcp / udp / ws / CAN / quic / webtransport and the ESP `httpd_ws_link_t`
  override it. Spelled `drop_stats`, not `stats`, because a platform link may already publish a
  richer kind-specific `stats()` block. The per-transport accessors are unchanged.

- **`dropped_tx()` on tcp / udp / ws (#932).** Every `send()` shed frames on oversize, a refused
  gather store, no peer or a dead socket with a bare `return` — the exact mirror of RX counters
  that did exist. Each of those legs now ticks a counter. (CAN already had `dropped_tx()`.)

### Changed

- **`config_reader_t` moved to `tr::wire`; `tr::net::config_reader_t` is now an alias
  (#985).** The pair-consuming `(NAME key, value)` walk (#927) decodes a `wire::tlv_t`, and
  its two remaining hand-written copies sat at L4 (`graph_t::create_child`'s creation-SPEC
  envelope and `parse_subscriber_tlv`'s SUBSCRIBER QoS SETTINGS), where `tr::net` may not be
  a dependency — so the type now lives in the layer that owns the grammar and both L4
  readers use it instead of restating its rule. The transport-plane spelling
  `tr::net::config_reader_t` remains valid as a `using` alias, so no call site moves.
  Consolidation side effect at the two L4 sites: the canonical plain NAME-field family
  semantics (#995) now apply uniformly — last **well-formed** occurrence wins (a repeated
  `delivery_compact`/`delivery_policy` no longer resolves "any nonzero"/per-occurrence) and
  an empty `VALUE` payload is ignored rather than read as zero.

- **`mem::pool_t` no longer advertises `is_isr_safe`; the new `is_nonblocking` trait carries
  what it actually guaranteed (#928).** The bare pool's `alloc`/`destroy` do an
  unsynchronized RMW on the intrusive free list, so a seam consumer selecting a backend by
  `is_isr_safe == true` was steered into free-list corruption the moment an ISR interleaved
  with task-context use. The two properties the one flag conflated now have distinct names:
  `is_isr_safe` (safe **concurrent with an ISR** — `pool_t`: **false**; ISR safety is
  `synchronized_pool_t` with an ISR-safe policy such as `tr::esp::portmux_sync_t`) and
  `is_nonblocking` (no heap, no syscall, no OS wait — `pool_t`: **true**). Every in-tree
  backend declares both (`heap`/`borrowed`/`cuda`: `is_nonblocking = false`), and
  `is_nonblocking` joins `is_isr_safe` as a `pool_sync_policy` requirement forwarded by
  `synchronized_pool_t` — the wait is the policy's fact, not the pool's guess. **Migration:**
  an out-of-tree sync policy must add `static constexpr bool is_nonblocking`, and a consumer
  reading `pool_t::is_isr_safe` for "no syscall" wants `is_nonblocking`.

- **`mem::transfer` refuses every DEVICE-space segment except the CUDA backend's (#928).**
  The DEVICE guard lived only in the generic fallback, so dispatch routed a
  `BORROWED_DEVICE` segment's device pointer into the host-`memcpy` fast path while a
  semantically identical UNKNOWN-tagged device segment got a clean `false` — the backend tag
  changed the outcome it is documented not to. The space check is hoisted into `transfer()`
  before the switch (CUDA exempt — it owns a real device copy), in both the multi-member and
  `POOL_ONLY` module sets; the borrowed-DEVICE test stand-in now exercises the refusal a real
  device link gets.

- **`net::config_reader_t` treats a wrong-width `VALUE` payload as absent (#928).** The typed
  accessors read through width-tolerant `detail::load_le`, so a 2-byte payload asked for as
  `u32` silently zero-extended and a 4-byte payload asked for as `u16` silently dropped its
  high bytes — a config the sender never wrote. `u8`/`u16`/`u32`/`flag` now require
  `payload.size() == sizeof(T)` exactly (the old empty-payload rejection's general case); an
  ill-sized occurrence is ignored like a wrong-typed one and never clobbers an earlier
  well-formed occurrence. `conn_spec_t` already emits full-width values, so the canonical
  builder is unaffected. Scoped to `config_reader_t` per the #928 ruling — the codebase-wide
  VALUE-decode convention is decided once alongside #906/#927.

- **`stream_endpoint_t::write_all_iov` takes `std::span<const ::iovec>` and no longer CONSUMES
  the gather (#932).** It used to advance `iov_base`/`iov_len` in place, a prose-only rule
  behind a bare mutable `::iovec*` that every fan-out site paid for with a per-peer copy; a
  partially-written entry is now finished with the plain writer and the gather re-entered at the
  next entry boundary. `slot_server_t::broadcast_iov` correspondingly takes a span and drops its
  per-peer scratch table — one fewer allocation, and one fewer exhaustion drop leg, on the
  multi-peer egress path.

- **`length_prefix_framer`: over LOCAL capacity is a DROP, not MALFORMED (#932).** A length past
  `backend.max_segment_size()` but inside the protocol cap used to be reported as malformed,
  which the callers act on by disconnecting the peer — a legitimate peer got torn down over
  *our* segment size. It is now drained, counted in `result_t::dropped`, and the stream resyncs;
  only a length above the protocol cap (`max_frame`) is malformed. `on_prefix`'s second
  parameter is consequently the PROTOCOL cap, not the effective cap.

- **`can_tx_pool_t::release` validates its slot pointer and returns `bool` (#932).** The pointer
  comes from a driver completion callback (the TWAI tx-done ISR), so a pointer outside the slot
  array or a double/foreign release used to corrupt `in_flight_[]`/`count_` from ISR context.
  Bounds check + CAS true→false; a refused release returns false and the TWAI ISR no longer
  gives a TX permit for it.

- **`tlv_view_t::materialize` reports a flatten OOM as `err_t::FLOW_BACKPRESSURE`** (WARN /
  TRANSIENT) instead of `err_t::FRAME_INVALID` (ERROR / PERMANENT) (#917). A caller that
  branched on `FRAME_INVALID` to drop-and-blame the peer must now also handle
  `FLOW_BACKPRESSURE`, which is retryable and is about this node, not the peer.
- **`receiver_slot`'s `deliver_rope` span fallback DROPS a frame whose materialize is refused**
  (#917). It previously handed the refusal's empty view to the span sink as though those were
  the frame's bytes — a truncated frame delivered as a complete one.
- **`graph_t::write` (both the branch-decomposition and field arms) answers `TYPE_MISMATCH` for
  a DEVICE-link value** (#917), instead of folding it into the `BACKPRESSURE` that an exhausted
  `value_backend_` raises. No retry makes a device payload CPU-decodable; backpressure said it
  would.
- **`webtransport_transport_t` DIAL: pre-sink ingress waits in msquic's window (#1101).** The
  DIAL constructor opens the WebTransport frame channel itself, so a server that pushed the
  instant the session came up reached the delivery slot before
  `transport_vertex_t::make_connection` had installed a receiver — dropped silently, with
  neither `dropped_rx()` nor `malformed_rx()` moving. This transport owns no receive thread to
  withhold (msquic's worker drives every RECEIVE), so per
  [ADR-0081](../docs/adr/0081-pre-sink-ingress-native-window-hold-or-named-drop-never-parked.md)
  §2 the hold is msquic's **per-stream receive window**, never a library buffer: the frame
  stream's RECEIVE events consume **zero** bytes while the gate is closed, the peer is
  flow-controlled by QUIC's own rules, and nothing is parked (ADR-0042 §2). The H3/QPACK state
  machine keeps consuming its own streams throughout — the gate is on delivery only, including
  the one callback where `0x41 ++ session-id ++ <first record>` arrives together (the preamble
  is consumed, the record is not).

  Public surface: the DIAL constructor takes a trailing **`bool defer_rx = false`** (the
  `tcp_transport_t` / `transport_ws_client` `defer_recv` shape — the default keeps the
  historical one-phase behavior), and `webtransport_transport_t` now overrides
  **`start_receiving()`**, which re-enables the frame stream's receive. It is idempotent and
  inert on a one-phase dialer, on a listener, and on a dial that never came up. The
  `webtransport` factory builds every SPEC-created dialer with `defer_rx = true`.

### Fixed

- **An eager `write` no longer erases a racing `assign`'s pending mark (#1185).** `clear_pending`
  erased the mark unconditionally under the sweep lock, so an `assign` that landed between a
  `write`'s store and that erase lost its deferred delivery outright: the newer value was
  published, its mark dropped, and no covering `propagate` ever delivered it. The erase is now
  conditional on the value this writer published still being the vertex's current LKV — a
  pointer compare against a strong reference the caller holds — so a mark whose value a sweep
  still owes survives. The failure direction is now always a duplicate delivery of the current
  LKV, never a lost one, which is what every fast path on the same function already permitted.
  No API change; `graph_t::write` / `assign` / `propagate` keep their signatures.

- **`key_view_t` walkers agreed on zero-length records (#932).** `child_record_under` rejected a
  4-byte (len == 0) trailing record while `parent()` / `split_levels()` / `last_segment()`
  accepted it. Empty path segments are illegal (`/sensor//temp` is invalid), so all four now
  treat a `len == 0` record as malformed framing. `transport_vertex.cpp`'s hand-rolled
  `last_segment` walk was replaced by `key_view_t::last_segment`, key_view's single locus.
- **`rope_t::try_flatten` / `try_materialize` — the flatten refusal now has a cause (#917).**
  `flatten()` answered an empty `view_t` for three different things: a rope with a DEVICE link
  (not CPU-flattenable, ever), an allocator refusal (transient), and a rope that legitimately
  holds zero bytes. Every caller's `empty()` test therefore asked three questions at once, and
  the one that mattered — `tlv_view_t::materialize` — resolved it as `err_t::FRAME_INVALID`,
  i.e. it reported this node's local OOM as a PERMANENT "your frame is malformed" verdict
  against a peer that had sent a perfectly valid frame. The new pair returns
  `std::expected<view_t, view::flatten_err_t>` with `NOT_HOST` vs `NO_MEMORY` kept apart, and a
  zero-length rope now flattens to a SUCCESS carrying an empty view without touching the backend
  at all. `flatten()` / `materialize()` remain, unchanged, as the lossy convenience wrappers.

### Documentation

- **`graph_t::has_subscribers` now states the staleness its ancestor half actually has
  (#1185/#854).** The predicate's `listeners_above` half is a relaxed load, so a `false` can
  miss a subtree subscribe that already completed on another thread. The header said nothing;
  it now names the window and carries the #555-standard justification for leaving it relaxed —
  #854's measured REFUTATION (the `seq_cst` candidate doubled the idle write's rv32 fence count
  and excluded no observation, because ADR-0049's latch snapshots the subscribed *ancestor's*
  own LKV, never a descendant's). `vertex_t::listeners_above` carries the same ruling, and the
  two concurrency design docs that claimed `own_subs_ordered()` is "the only read that decides
  whether to deliver at all" are corrected — it is not.

## [0.10.0] — 2026-08-12

### Added

- **`transport_t::link_up()` — one liveness question every link answers (#1059).** The tree
  carried two conflicting `ok()` conventions: `tcp_transport_t`'s DIAL `ok()` read the live
  connection fd (liveness), QUIC/WebTransport defined `ok()` as "came up" with a separate
  `link_up()`, and `transport_ws_client`'s declaration promised both while the backing flag
  was never cleared — a WS client whose link died kept answering "up" forever. The ruling is
  the QUIC convention, tree-wide: **`ok()` is the came-up predicate** (did the dial / bind /
  handshake succeed — asked once, right after construction, the `make_checked` gate; it never
  reverts), and **`link_up()` on the `transport_t` base is liveness** (a relaxed-atomic hint,
  default `true` for kinds with no closure concept — UDP, CAN, the multi-peer servers). The
  pull twin of `set_down_notifier`. Deliberately no is-always-lock-free assertion: one target
  is an rv32 core without the A extension.

- **`wire::wire_clock_t` + `wire::stamp_ts` — the wire-trailer timestamp WRITER (#1109).**
  The spec's per-TLV trailer TS was reachable only on the read side; no code path could set
  it, so RTT over the FWD plane was unmeasurable. `stamp_ts(tlv_t&, ...)` sets `opt.TS` and
  the TF=0 absolute trailer value together, from an **injected** `wire_clock_t` (or a raw
  `now_ns`) — the library never reads ambient time on a frame path, and the value's contract
  is CONTEXT.md's per-producer-monotonic `origin_timestamp`. TF=0 only: the TF=1 writer is
  gated on the spec's anchorless-reject check, which the codec does not yet implement.
  `wire::store_trailer_ts` / `emit_trailer_ts` / `trailer_ts_bytes` (tlv_emit.hpp) are the
  one home of the trailer-TS byte layout, covering **both** forms so #879's stream shape
  reuses them. The FWD forward hop now **preserves** an outer stamp verbatim
  (`fwd_pre_t::fwd_opt`, `fwd_rebuild_t::ts_off`/`ts_len`, `stack_writer::header`'s trailer
  bits; `kFwdMaxIov` 9 → 10), and the terminus **echoes** a TF=0 request stamp on every
  reply, error replies included — the ICMP-echo construction (`RTT = now − echoed_stamp`, no
  request id, no clock sync). `tlv_arena_t::root_trailer_ts()` exposes the root stamp the
  echo reads on the span tier.

### Changed

- **A kind-less connection `SPEC` matching no staged link now answers `TYPE_MISMATCH`, not
  `NOT_FOUND` (#1062).** Both `transport_vertex_t::make_connection` return sites for "no `kind`
  and no `provide_link` staging" moved from `status_t::NOT_FOUND` (wire:
  `tr::path::not_found`, `0x0020`) to `status_t::TYPE_MISMATCH` (wire:
  `tr::schema::type_mismatch`, `0x0030`). The config is *incomplete* — `kind` is a required
  field once no staging supplies the module, the same convention as a DIAL missing `addr` or
  either role with `port == 0` — whereas RFC-0014 reserves `tr::path::not_found` for an
  **absent** creator endpoint (the creatability probe), so the old answer let a peer confuse
  "no such creator endpoint" with "your config was missing `kind`". This pins the RFC-0014 §2
  error identity for the case under its own clause-kind rule (code + the new
  `errors/error-kindless-spec-type-mismatch` conformance vector). The genuine name-lookup
  `NOT_FOUND`s on the settings/link/remove doors are unchanged. Wire-visible.

- **`graph::valid_segment` now rejects `[` and `]` — the full reference/03 reserved set
  `/ : . [ ] * ?` (#996).** BEHAVIOR TIGHTENING on the shared segment predicate
  (ADR-0073 §1): a NAME carrying a bracket — `path_t::parse("/camera/frame[7]")`, a wire
  `SPEC` child name, a module registration — now answers `INVALID_PATH`, matching the
  MUST of reference/03 §Reserved characters (normative via spec v1 §3) that the Rust and
  TypeScript tiers already enforced; the C++ core was the documented five-character
  subset. No grammar change: the addressing grammar always reserved the brackets
  (`index` sits outside `name`), and address-index addressing, if it lands, stays outside
  the NAME bytes (cf. `.out-of-scope/range-slice-addressing.md`). The agreed set is
  pinned cross-tier by the new `path/path-reserved-brackets` conformance vector plus a
  host test in each tier's own suite (`core/tests/path_test.cpp`,
  `bindings/rust/tests/conformance_vectors.rs`,
  `bindings/typescript/packages/client/test/vectors.test.mjs`). Indexed child endpoints
  are spelled as ordinary child vertices (`/camera/frame/7`) — reference docs updated.

- **`transport_ws_client`: `ok()` no longer conflates liveness (#1059).** `ok()` keeps its
  construction-time answer (unchanged behaviour, corrected declaration), and the new
  `link_up()` override reads a relaxed atomic the recv loop's teardown path now clears on
  every death of the connection — peer CLOSE, remote hangup, fatal receive error, RFC 6455
  breach — before the departure seam fires. Previously the only truthful departure observable
  was the down-notifier; an owner that installed none could never learn the link died.
- **`tcp_transport_t`: DIAL `ok()` is now the came-up predicate (#1059).** It used to read
  the live connection fd and so flipped false on teardown; that liveness answer moved to the
  `link_up()` override (fd-derived, both roles — LISTEN answers false between peers). A
  DIAL link that came up and later died now answers `ok() == true`, `link_up() == false`;
  owners polling for death switch to `link_up()`. LISTEN `ok()` is unchanged.
- **`quic_transport_t` / `webtransport_transport_t`: `link_up()` now overrides the base
  virtual (#1059).** Same signature and meaning as before — these two already carried the
  ruled convention; their `ok()`/`link_up()` declarations now state it as the tree-wide
  contract.
- **Read/write disclosure parity on nonexistent fields — namespace-governed, and the answer a
  DENIED caller sees changes on both doors (#435, RFC-0010 §A erratum 2026-08-12).**
  `graph_t::read`'s field overload now resolves **protocol-owned name-validity above the READ
  gate**: an unknown top-level field name (`:status`), bare `:subscribers` or a
  `:subscribers.<tail>` spelling answers `SCHEMA_NOT_FOUND` caller-independently — matching
  the write door — where a denied caller was previously told `PERMISSION_DENIED`.
  Name-validity only: every existent facet keeps its gate, `:identity` stays pre-auth, and the
  pinned #869 selector-shape divergences are untouched. `graph_t::field_write`'s
  `settings.app.` arm now evaluates the caller's **WRITE right before resolving the owner
  name** (gate-before-resolve): a denied caller gets `PERMISSION_DENIED` for declared,
  undeclared, `ro` and `wo` spellings alike, where #430's hoist previously answered
  `SCHEMA_NOT_FOUND` pre-gate for undeclared and `ro` names and so disclosed the owner's
  field-name set. Admitted callers and the owner see no change anywhere. Conformance:
  `acl/denied-caller-undeclared-app-field`; pinned by `acl_test.cpp`
  `test_denied_caller_disclosure_parity`.
- **PlatformIO `espressif32` ships no WebSocket transport (#984, the #947 ruling's
  PlatformIO half).** The portable POSIX-socket `transport_ws_server` /
  `transport_ws_client` pair compiled into PlatformIO espressif32 images even after #978
  removed it from the ESP-IDF component — and on lwIP it never delivered data (#948:
  `lwip_sendmsg` rejects `MSG_NOSIGNAL` with `EOPNOTSUPP`, so every scatter-gather data
  frame was silently dropped while the handshake and PING/PONG worked). The
  `library.json` extra script now owns the package source filter (the manifest
  `srcFilter` moved there — a manifest filter takes precedence and cannot express a
  per-environment exclusion) and, on `espressif32` only, excludes the pair plus core's
  full-node `register_builtin_transports`, compiling a udp+tcp-only dispatcher
  (`integrations/platformio/builtin_transports_udp_tcp.cpp`) in its place — TU
  selection, no feature macros. The IDF-native links are **not** packaged for
  PlatformIO (their `esp_websocket_client` dependency under `framework-espidf` is
  unverified; sanctioned follow-up on #984) — a consumer needing WS on ESP32 uses the
  ESP-IDF component. Gated in `pio-esp32-can` CI on the linked fixture image:
  `tools/check_esp_ws_plane.py --ws-plane none` (zero portable **and** native WS
  symbols, with the symbol-table-floor guard). Non-`espressif32` platforms are
  unchanged.

- **`wire::encode` refuses a claimed-but-valueless timestamp (#1109).** `opt.TS` set with no
  `trailer->ts` value — or a value whose `relative` flag contradicts `opt.TF` — now returns
  the unambiguous empty vector instead of silently emitting a zero stamp (1970-01-01).
  `stamp_ts` sets bit and value together and cannot hit the refusal.
- **`wire::emit_tlv` clears trailer bits by construction (#1109).** It writes header + body
  and nothing after, so a trailer-bearing `opt` used to mint a frame claiming a trailer it
  did not carry (read back as truncation). Callers with a trailer to write use `encode` or
  `emit_header` + `emit_trailer_ts`.
- **`type_t::TIME` documented as reserved (#1109).** The application-domain payload timestamp
  type: core assigns the code and deliberately neither emits nor consumes it.

### Fixed

- **`max_frame` is now tighten-only, as the headers always promised (#1035).** Every framed
  transport (`tcp_transport_t` / `transport_tcp_server`, `transport_ws_client` /
  `transport_ws_server`, `quic_transport_t`, `webtransport_transport_t`) replaced its receive
  cap with the configured `max_frame` outright, so a `:settings max_frame` above the 16 MiB
  protocol default (`length_prefix_framer::kDefaultMaxFrame`) *raised* the ingress buffering
  bound — contradicting the five headers' tighten-only wording. The nine assignment sites now
  resolve the setting through the new `length_prefix_framer::configured_cap(max_frame)`
  (`0` → the default; otherwise `min(max_frame, kDefaultMaxFrame)`), so a config-writable key
  can only narrow what a node will buffer off the wire, never widen it. Behavioural change: a
  deployment that (contrary to the documentation) passed a `max_frame` above 16 MiB now tears
  down a connection carrying an over-default frame as MALFORMED, exactly as the default
  configuration always did — the protocol default is a ceiling, not a suggestion.

## [0.9.1] — 2026-08-10

### Added

- **`graph::target_binding_t` and `graph_t::target_canonical_resolves()` — local target edges
  carry a minted vertex binding instead of re-walking the address every delivery (#830).**
  `dispatch_edge_target` resolved its `target_key` from the root on *every* delivery, and
  `find_ptr` is linear in the target's address depth: the full `on_frame(FWD{WRITE})` terminus
  leg was measured at **+21.3 ns/segment** (a deep dst scales the arena decode, the mount peek
  and the descent), against a flat **~11 ns** slot deref, with the crossover at **D=2**. A
  subscription edge now carries the RFC-0024 slot pair minted once in `admit_subscriber` —
  after every door has settled what the target is, so `subscribe_wire`'s deliberate
  `target_key` clear is honoured — and delivery derefs it. Nothing about routing changes:
  `deref_vertex_slot` refuses a stale or saturated generation, an out-of-range index and an
  unregistered placeholder, and every refusal falls back to the canonical `find_ptr` walk
  rather than dropping, so drop-never-misroute holds and no delivery is lost. **ACL is still
  evaluated at the deref'd vertex per delivery** (RFC-0024 §6.2) — the mint is a cache of an
  answer, never of a permission. The new counter reports only the *fallback* walk, so the
  bound leg carries no atomic at all; a non-zero value means the edge was admitted before its
  target existed or its binding went stale. `subscriber_t`, `edge_view_t` and `pub_edge_t`
  each grow one 8-byte trivially-copyable field.

### Changed

- **`grammar::total_size_fits` takes its wrap-free bound only at the width that can wrap;
  the subtractive chain moves to the new `grammar::total_size_fits_narrow` (#1177).** #921
  replaced a single-compare total-encoded-size bound with a chain that never forms a value
  able to wrap, and that chain then ran at every `Size`. The defect it closes is real and
  **32-bit-only**: a wire `length` of `0xFFFFFFFF` overflows a 32-bit `std::size_t`, narrows
  to 17, passes `avail < total` and hands `walk` a payload span past the buffer. But `header`
  is at most 6, `length` at most `0xFFFFFFFF` and the trailer at most 12, so the total is
  under `2^32 + 18` — on any `Size` strictly wider than the 32-bit wire length no sum can
  wrap and the bound is one compare. `total_size_fits` now dispatches on `sizeof(Size)` at
  compile time and calls `total_size_fits_narrow` — the #921 body verbatim — only at 32-bit
  width. **No behavioural change at any width** and no wire-grammar change; the rv32 overflow
  check is byte-for-byte what it was. `parse_header` runs once per TLV, so the chain's extra
  compares multiplied across a frame: `compact-forward 64B/fan1/1ep` measured **−30.8% p50 /
  −31.4% 1-over-throughput** against an A/A null of +3.1%/+4.4% on the pinned bench host,
  returning the leg to its pre-#921 level. `total_size_fits_narrow` is new public surface only
  in the sense that it is reachable; callers should keep calling `total_size_fits`.

- **A WebTransport handshake that runs out of memory now aborts one stream instead of the
  connection (#1108).** `accumulate`'s two failure modes had one disposition. Exceeding
  `kMaxHandshakeBytes` is a statement about the PEER and remains connection-fatal
  (`kAppErrBadRequest`), unchanged. Running out of memory is a statement about US, and taking
  down a session the peer already established because our heap is tight is the over-broad
  refusal #919 removed — so it now aborts just that stream and leaves the connection and any
  live session up. Peer-driven allocations on this path (the handshake accumulator and the
  per-stream context list) go through the failable `tr::detail::try_reserve` seam and nothrow
  `new`, and the 0x41 frame-channel tail no longer copies at all — its buffer is moved out of
  the accumulator rather than duplicated.

## [0.9.0] — 2026-08-10

### Added

- **`mem::kSpinWaitSafe` — a build-configuration constant that makes the wrong pool a compile
  error instead of a hang (#1158, #963.3).** New `inline constexpr bool` in
  `libtracer/config.hpp` (namespace `tr::mem`, so an L0 header can read it without naming an
  L4 type), defaulting to **`true`** — every existing build is unchanged and
  `mem::sync_pool_t` stays exactly as usable as before. CMake: `-DLIBTRACER_SPIN_WAIT_SAFE`,
  whose value is the C++ token, as with `LIBTRACER_LKV_SLOT`.

  `synchronized_pool_t` now carries a `static_assert` that rejects `Sync = spin_sync_t` when
  the constant is false, naming `tr::esp::critical_pool_t` as the answer. It fires on
  **instantiation**, not on the alias declaration, so `using sync_pool_t =
  synchronized_pool_t<spin_sync_t>;` still compiles everywhere and only declaring or
  constructing one trips it.

  Why a build knob and not a policy trait: only the BUILD knows the target's concurrency
  model, and `spin_sync_t::lock()` is a bare `test_and_set` loop with no yield. On a
  priority-preemptive scheduler a spinner that outranks the lock holder never yields the CPU
  the holder needs to release it, so the O(1) section becomes unbounded priority inversion and
  the target hangs in its watchdog rather than merely running slowly — the failure compiles
  cleanly, survives a smoke test, and only appears under load with a specific priority
  ordering. The ESP-IDF component derives the value from `IDF_TARGET` (false on every chip,
  true on `linux`) and does not ask the integrator, the same reasoning that derives
  `kCacheLineBytes` from `CONFIG_FREERTOS_UNICORE`.

  **Zero cost**: `libtracer.a` is byte-identical before and after at `MinSizeRel` — a
  `static_assert` and an `inline constexpr bool` emit no code, so there is no footprint,
  latency, or throughput change to trade against. Covered by the `spin_pool_guard` CTest,
  which compiles one probe TU against both renderings of the config header and asserts both
  arms: the allowed one compiles, the forbidden one fails *and* names `critical_pool_t`.

- **`tr::net::bus_link_t::peer_named()` — the multi-peer MODE AUTHORITY, asked once
  (#889).** A virtual query, default `true`, overridden by `tr::net::slot_server_t` to return
  the `peer_named` its listener was constructed with. Before it, "which mode is this link in"
  had two answers that only coincided by wiring accident: `peer_named_` gated `bus()` and
  nothing else, while every runtime decision — the tcp/ws servers' per-frame tier select and
  the shared departure branch — keyed off `peer_rx_.has_any()`, i.e. off whether a peer sink
  happened to be installed. A kind that is a bus by construction (the CAN binding) keeps the
  default and is unaffected. Cold path only: an implementation's own per-frame tier select
  reads its stored flag, never this virtual.

- **`tr::net::conn_spec_t` + `tr::net::conn_spec(...)` (`libtracer/conn_spec.hpp`) — the
  connection-creation SPEC finally has a public ENCODER (#902).** `transport_vertex_t` has
  always shipped the decoder for the `/net:children[]` grammar
  `SPEC{NAME type, NAME name, SETTINGS config{role, port[, kind][, addr] …}}`, but nothing
  emitted it: every consumer of the production first-wiring step hand-built the TLVs from
  `wire::emit_tlv` / `wire::emit_name` and reached into the INTERNAL `tr::detail::store_le`
  to encode the port. Sixteen private near-copies existed across `core/tests`, `bench`,
  `core/examples/tree_of_ropes.cpp` and the ESP-IDF `full_node` example, and they had already
  drifted — `tree_of_ropes`' copy could not spell `kind` or `addr`, i.e. the example for
  "mount a transport" could not express the field that decides which MODULE the connection
  mounts under. `conn_spec_t` is a fluent builder that appends `(NAME key, value)` pairs in
  call order (`role`/`port`/`kind`/`addr`/`keepalive_ms`/`max_frame`/`backoff_ms`/
  `connect_timeout_ms`, plus generic `text`/`u8`/`u16`/`u32`/`flag` for a kind's PRIVATE keys,
  named to mirror `config_reader_t`'s accessors); a builder on which no setter ran emits no
  `config` at all, which is the `provide_link` spelling. `conn_spec(type, name, role, port,
  kind = {}, addr = {})` is the one-call sugar over it. **No wire surface changes** — the
  bytes are pinned byte-for-byte against the pre-existing hand-emit in
  `transport_vertex_test`, and the TypeScript client's `encodeConnSpec` (#408) already shipped
  this same grammar, so the C++ core was the odd one out. There is no `module` key and the
  builder invents none: a SPEC names its module through `kind` + `role`, resolved by the
  application's `register_module` declaration before any staged link is consulted (#883).

- **`tr::wire::emit_value_le<T>(out, value, width = sizeof(T))` (`libtracer/tlv_emit.hpp`) —
  the public way to write an integer VALUE TLV (#902).** The decode half of the
  `(NAME key, VALUE u8/u16/u32)` config pair has been public since `config_reader_t`; the
  encode half was not, so a consumer sized its own buffer and called `detail::store_le` or
  hand-rolled a shift loop. It lives in `tr::wire` because it turns a wire type into wire
  bytes; the layer-free LE byte primitive it builds on (`detail::append_le`, `byteorder.hpp`)
  stays in `tr::detail`, per that header's own layering note.

- **`tr::net::detail::tcp_peer_publishing_hook` (`libtracer/transport_tcp.hpp`) — a TEST-ONLY
  seam, null in production (#891).** Run by the shared accept path
  (`slot_server_t::accept_peer`, through this server's `on_slot_publishing` override) at the instant
  a new peer's fd is published and its slot is one store from open, inside the `write_m_`
  hold. The window a racing test would have to hit is two instructions wide; the hook lets a
  test HOLD that instant open, broadcast into it, and check the frame arrives at the peer
  being accepted (`tcp_test`). Same shape and same rules as `ws_peer_published_hook`: install
  it before the peer that should trip it connects, clear it before the test returns. The
  production cost is one predictable null-check per accepted connection on the cold accept
  path.

- **`graph::status_t::TRANSPORT_DOWN` — a link that could not come up now has its own status
  (#929).** The L4 status set had eight members and no transport member, so every
  dial/bind/handshake failure was reported as `NOT_FOUND` and `error_code(status_t)` — the
  total L4→wire map — could never emit `wire::err_t::TRANSPORT_DOWN` (0x0060). The new member
  maps to it. **Source-compatible in the direction that matters** (a caller comparing against
  the existing members still compiles), but a `switch` over `status_t` with no `default:` —
  the shape this library uses on purpose — gains an unhandled enumerator and, under
  `-Werror=switch`, will name itself at compile time. `to_string(status_t)` answers
  `"transport_down"`.

- **`net::transport_t::start_receiving()` — the second half of a two-phase link bring-up
  (#1025).** A virtual whose default is a no-op, so every existing transport and every
  embedder's is unaffected and an owner may call it unconditionally. It exists because
  `set_receiver` / `set_rope_receiver` / `set_down_notifier` all document "must be set before
  frames flow", and for a DIAL transport that spawns its receive thread inside its own
  constructor that contract is unsatisfiable from the outside: the thread is already draining
  the socket while the owner is still installing its sinks. Outside the tests,
  `transport_ws_client` and (since #1045) `tcp_transport_t` are the overrides under
  `core/include` + `core/src` + `integrations/` + `bindings/` today
  (`grep -rn start_receiving`); each takes effect only when constructed with its trailing
  `defer_recv` flag, so a direct `transport_ws_client(host, port)` behaves exactly as before.

- **`net::tcp_transport_t` takes the two-phase bring-up: a trailing `defer_recv` DIAL
  constructor flag and a `start_receiving()` override (#1045).** The `transport_ws_client`
  shape (#1025) applied to the transport this issue is scoped to, and nothing else — quic,
  webtransport, the ESP-IDF-native WS client link and CAN are untouched, and each has its own
  follow-up issue. `defer_recv` defaults to `false`, so a direct
  `tcp_transport_t(host, port)` behaves exactly as before and no existing call site HAS to
  change to keep compiling; the one that does change is the built-in `tcp` DIAL factory
  (`core/src/builtin_transport_tcp.cpp`), deliberately, so a SPEC-created dialer gets the
  deferred form. The LISTEN constructor is untouched. With `true` the connect still happens in the
  constructor and `ok()` still answers for it, but no receive thread exists and no byte is
  read until the owner calls `start_receiving()`. The override is idempotent and inert
  wherever there is nothing to arm — a second call, a one-phase link, a LISTEN link, and a
  link whose dial failed — because `transport_vertex_t::make_connection` calls it
  unconditionally on every link it wires. The built-in `tcp` kind's DIAL factory
  (`core/src/builtin_transport_tcp.cpp`) now constructs the deferred form, so a connection
  created by the in-band creating write is armed only once its receiver is installed and a
  peer's push-on-connect frame is delivered instead of being decoded into an empty sink and
  dropped with no counter moving.

- **A SPEC-created `webtransport` dialer can name its extended CONNECT `:path` (#1023).**
  `webtransport_transport_t`'s DIAL constructor has always taken the CONNECT `:path`, but
  the catalog factory hard-coded `"/"` and `parse_wt_config` read four keys — `cert`, `key`,
  `ca`, `insecure` — none of which was it. So an in-band
  `write /net:children[] += SPEC{kind=webtransport, …}` could reach only a server that
  serves its session at the root; anything else had to abandon the creation SPEC for the
  direct constructor plus `provide_link`, and the difference was not reported — the dial
  simply went to the wrong resource and creation answered `NOT_FOUND`. A fifth
  kind-PRIVATE key, `path` (`NAME`, DIAL, default `/`, empty normalised to `/`), now carries
  it, parsed by `parse_wt_config` alongside the others so nothing lands on the shared
  `conn_settings_t` (the ADR-0043 §5 leanness ruling). It is the one key `webtransport` does
  not share with `quic`, which has no HTTP layer to carry a resource; it does **not** collide
  with the `can` kind's unrelated `path` (kind-private namespaces are disjoint), and
  `docs/modules/connection-config.md` now says so on both rows. Same *shape* of gap as #918,
  one parameter over. **New public API:** `webtransport_transport_t::session_path()` returns
  the session's CONNECT `:path` — on LISTEN, the path the ACCEPTED CONNECT named (empty until
  one is accepted); on DIAL, the path this endpoint requests. The listener still serves every
  path (it validates `:method`/`:protocol` only), so this is an observation, not an admission
  decision — and it is what makes the fix testable over the real wire rather than by reading
  a config value back out of the dialer. `core/tests/webtransport_test.cpp` drives three
  legs through the real `:children[]` SPEC path against a directly-held listener:
  `path = "/tracer"` arrives as `/tracer`, an absent key still dials `/`, and an empty path
  normalises to `/`.

- **The `can_link_t` seam owns its admission rule, so the two ports can no longer disagree
  about which frames are real (#931).** `twai_link_t` filtered remote-request and 11-bit
  standard frames on ingress and bounded classic length on egress; `socketcan_link_t` did
  neither. A `CAN_RAW` socket carries no filter by default, so an RTR frame reached
  `socketcan_link_t`'s receiver as a data slice whose DLC promised bytes it never carried —
  and on egress a classic frame declaring 9–64 bytes `memcpy`'d straight past the 8-byte
  kernel `struct can_frame` on the stack, held back only by the seam's precondition. Rather
  than copy the twai checks into the sibling, the rule now lives at the seam itself, in
  `transport_can.hpp`: `tr::net::can_rx_admissible(extended, remote, error)` and
  `tr::net::can_tx_admissible(frame)`, over `tr::net::can_max_len(fd)` — a thin adapter onto
  the L1 widths in `tr::view::can_max_data`, so the seam adds no second copy of the numbers
  (`can_frame_data_t::data` is likewise sized from `tr::view::kCanFdMaxData` now). Both
  *bus* ports call them; each still decodes the flags from its own driver's representation,
  but the verdict is reached in one place. (The in-memory test links are exempt by
  construction — their carrier cannot express RTR, an 11-bit identifier, or an error flag.)
  Behaviour change: a `socketcan_link_t` receiver no longer sees RTR or 11-bit standard
  frames, and an over-length classic frame is dropped rather than emitted. Error frames are
  covered by the same predicate but were never a behaviour that existed here: the socket
  requests no `CAN_RAW_ERR_FILTER`, so the kernel's default zero mask has always withheld
  them. The check is the seam's rule holding for a port that does ask, not a change.

- **`fwd_router_t::receiver_ctx_count()` (#884).** How many per-child receiver contexts the
  router holds — one per NAME ever registered, live or tombstoned. The twin of
  `child_registry_t::size()` and introduced for the same reason: it is the length of the chain
  every name-keyed and slot-keyed lookup walks, so "create/remove churn does not grow it" is
  assertable rather than merely intended. Takes the control lock; never a per-frame call.

- **`delivery_drops()` counts the three drop sites it was missing, and counts them by
  DELIVERY (#896).** `graph_t::delivery_drops()` promised drop observability while three
  paths shed deliveries invisibly, so a node under memory pressure reported zero drops
  while an arbitrary number were shed. The worst read success on a write that delivered
  nothing at all: `write_impl`'s handler branch skips the whole fan-out when its notify
  clone cannot be allocated — every subscriber of the vertex, not one edge — and
  incremented nothing. The other two live in the fan-out snapshot: an edge whose owning
  copies (link NAME / stored caller) cannot be allocated is skipped, and a wide fan-out
  whose overflow buffer cannot be reserved is truncated to the inline prefix.

  Three changes to the public surface:
  - `delivery_drops_t` gains **`fan_out_truncated`** — deliveries shed by the capacity
    degrade, kept apart from `out_of_memory` so an operator can tell a buffer that could
    not be widened from a delivery whose clone failed.
  - `vertex_t::snapshot_edges` takes a third argument, **`vertex_t::snapshot_drops_t&`**
    (new nested type), reporting what the snapshot shed; `graph_t::fan_out` folds it into
    the graph's counters. The parameter is a required reference, not an optional pointer:
    a caller that cannot see the shed count is the defect being fixed.
  - Every drop **on the fan-out / dispatch plane** now goes through one internal counting
    door, so a path there that abandons an admitted delivery without counting it is a
    visible omission. That scope is deliberate and not yet the whole vertex: a STREAM
    ring-append shed under allocation pressure still abandons the write's entire fan-out
    uncounted, and `mark_pending`'s OOM legs shed a deferred IF_NEWER delivery the same
    way. Both are pre-existing, outside this change's sites, and tracked separately.

  Counts are **deliveries, not events**: a shed fan-out of N counts N. Nothing is added to
  the delivering path — the fold is one predicted-not-taken test per snapshot and the
  counters are touched only on a drop. No wire change.

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

- **`tr::net::write_fault_stats()` / `write_fault_stats_t` — the malformed-call write-fault
  tally (#948).** Process-wide (the full-write helpers are static and shared by every stream
  transport, and what it counts is a defect in libtracer's OWN syscall arguments, not a
  property of a connection): how many `send`/`sendmsg` attempts were rejected with an errno
  that means "this call was malformed" rather than "this socket is dead", and the errno of the
  most recent one. **Non-zero is always a bug** — on a supported host it stays 0 forever.
  `tr::detail::write_fault_inject_hook` accompanies it as the test seam that makes those arms
  reachable at all (a host kernel emits none of those errnos here); it is null in production,
  exactly like its neighbour `probe_fail_hook`.

- **`transport_can_config_t::rx_backend` and `kCanMaxGroupSlices` (#910/#911).**
  `rx_backend` (a `tr::mem::mem_backend_t*`, default `nullptr` = the process heap) is the
  byte seam an inbound CAN data slice is copied into before it enters the reassembly
  buffer — the companion to `reasm_mr`, which bounds the reassembly *structure* while this
  bounds the slice *bytes*. It is also the second, defaulted parameter of
  `can_transport_factory`, for the reason `reasm_mr` is: a backend is a pointer, not a wire
  value, so it cannot ride the config TLV and is injected at registration instead. A
  bounded backend (`mem::pool_t`) makes ingress exhaustion a by-value refusal on the RX
  thread rather than a reach into the global heap. `kCanMaxGroupSlices` is the largest
  address-shift group a node can place, DERIVED from the CAN-ID field widths
  (`can::kEndpointMax` minus the reserved control slot), not chosen.
  `can_reassembly_t::discard(key)` joins `erase` as its counted twin: `erase` is the
  post-delivery release (nothing lost, nothing counted), `discard` is the caller-side
  abandon of a group that will never complete, and it ticks `dropped_groups`. No wire
  change.

### Changed

- **An undefined FWD opcode now answers an addressed `ERROR` instead of being dropped (#904).**
  `kFwdOpcodeMask` admits `0x00`–`0x3F` and RFC-0004 §B defines four values, so opcodes 4–63
  fell through `apply_op`'s caseless switch and left by value — which `fwd_router_t` turns into
  a silent drop. Every other "your frame says something illegal" verdict (bad selector,
  malformed path, unknown vertex) already answered an addressed `ERROR`; only this one answered
  nothing, and nothing is the single reply an origin cannot act on, being indistinguishable from
  a dead link. A terminus with a captured return route now replies
  `ERROR{tr::schema::type_mismatch}` (`0x0030`) — the verdict
  `docs/reference/01-data-format.md` already prescribes for an unimplemented core-range type
  code one level up. **No new status code and no wire surface added**; behaviour-visible to a
  peer that was previously timing out. Flag bits 7–6 are unaffected: RFC-0024 §9.3 masking still
  degrades an unrecognised flag to the plain opcode, so `0x80 | READ` remains a `READ`. The
  reject sits at the terminus, not the forwarder — an intermediate hop routes on `dst` and stays
  opcode-agnostic.

- **`OWNER@` is withdrawn as a special subject (#1033).** Docs and comments only — no API, ABI,
  wire or behaviour change, and no conformance vector moves. `EVERYONE@` is now the one special
  subject everywhere. `OWNER@` was published as special by ADR-0020, reference-05 §`0x0A` and
  `CONTEXT.md`, but no evaluator ever special-cased it: `ace_applies` branches on exactly one
  string. So an operator writing `{subject: "OWNER@", access_mask: WRITE_ACL}` got an ACE that
  matched nobody — and because *any* present ACE closes an otherwise-open vertex, that write
  **locked** the vertex it was meant to delegate. The name was also impersonable, being an
  ordinary token a pass-through resolver could mint. Withdrawing removes both at once: with no
  document telling an operator to write that ACE, an impersonated `OWNER@` has nothing to match.
  Real owner semantics need a per-vertex owner identity the graph does not hold and would change
  how a *stored* ACE evaluates — an amendment, not this. `WRITE_OWNER` stays declared in the mask.

- **`[[nodiscard]]` on the fallible control-plane returns (#892).** `fwd_router_t::add_child`,
  `child_registry_t::add` and `graph_t::retire` now carry the attribute, as do six private
  `graph.hpp` `result_t` helpers. SOURCE-BREAKING only for a caller that discards one of these
  results: the fix is `(void)` and a note saying why, and no ABI, signature or behaviour
  changes.

  Two of the three had a doc block that explicitly **declined** the attribute, and both stated
  reasons turned out to be arguments *for* it. `add_child`'s said "every existing call site
  registers a name it composed itself, so it stays correct ignoring it" — true of the
  unaddressable-name half of the return and false of the other half, since
  `child_registry_t::add` also returns false when the table could not grow, which no composed
  name rules out. That is precisely what `make_connection` discarded when it minted a
  connection published UP and resolvable by no `dst` (#930).

  `result_t` itself **cannot** carry a type-level `[[nodiscard]]` — it is an alias template and
  C++ gives the attribute no such target — so the sweep is per-function, and `retire` was the
  only public fallible signature still missing it.

  In-tree call sites: 217 warned. 214 are tests that deliberately ignore a control-plane
  failure and now say so with `(void)`; the three that are not tests
  (`core/examples/two_node_fwd.cpp` ×2, `core/tests/fwd_node_server.cpp`) got a real check,
  because sample code that ignores a registration failure teaches the bug.

- **`view::view_can_frames_t` no longer stores its window table — `frames()` is replaced by
  `frame(i)` (#1110).** BREAKING for any caller of `frames()`: the accessor returned
  `const std::vector<view_t>&` and there is no longer a vector to return. `frame_count()`,
  `mode()`, `to_rope()` and `split()` are unchanged in name and meaning; `split()` is now
  `noexcept`.

  The window table was **derivable all along**: window `i` sits at `i * step` and runs to
  `min(step, total - i * step)`, so it is a pure function of the payload length and the mode.
  Storing it bought nothing and cost a `std::vector<view_t>` grown with a **THROWING**
  `push_back`, on a frame count that scales with a payload size the *sending peer* chooses —
  which on the `-fno-exceptions` MCU profile is `abort()`, not a dropped frame.

  **Why not the bounded-growth fix the issue asked for.** The established answer for exactly
  this shape is `net::iov_table_t`: an inline array overflowing into a `mem::block_source_t`
  whose exhaustion returns `nullptr` so the caller drops the frame. It cannot be used here —
  `mem::block_array_t` static-asserts a trivially copyable, trivially destructible element and
  `view_t` carries an intrusive refcounted `segment_ptr_t`. That dead end is the useful signal:
  machinery to make the residual affordable was the wrong half of the problem. With nothing
  stored there is no allocation to bound, no exhaustion path to signal, and `split` cannot fail.

  **Measured** (AMD EPYC 9115, fixed 2.6 GHz, pinned, 30 samples/arm trimmed, 3 rotations;
  A/A null +0.02% / +0.08% / +0.09%, so every figure below clears its null by two orders of
  magnitude):

  | payload | frames | allocations (was -> now) | split + full iteration |
  | ---: | ---: | ---: | ---: |
  | 24 B | 3 | 3 -> **0** | 50.97 -> 39.32 ns (**-22.9%**) |
  | 512 B | 64 | 7 -> **0** | 749.0 -> 638.6 ns (**-14.7%**) |
  | 4096 B | 512 | 10 -> **0** | 5795.2 -> 5044.4 ns (**-13.0%**) |

  The allocation count was `ceil(log2(frames)) + 1` — unbounded in the payload. The saving
  prices out: at 64 frames, 110.4 ns is ~287 cycles for 7 eliminated malloc/free pairs (~41
  cycles each); at 512 frames the remainder is the 511 element moves the reallocations did.

  **Footprint is neutral**: `transport_can.cpp.o` `.text` 26,695 -> 26,693 B at `-Os`.
  `send_impl` grows +60 B (the index arithmetic inlines into the emit loop) and the dropped
  `std::vector<view_t>` destructor instantiation (-132 B) more than pays for it.

  This closes ONE of the two allocating steps a CAN send owns; `view::over_bytes` still takes
  the owning payload block and still soft-fails to a drop. The end-to-end `send` gain is
  therefore smaller than the table above, and was not separately measured.


- **`config_t::kMaxVertexBytes64` 120 -> 96 and `kMaxVertexBytes32` 80 -> 72 — the RAM-diet
  bounds become RATCHETS pinned to the measured size (#361 §8).** Public constants, so the
  values are part of the API surface; nothing else moves and no runtime behaviour changes.
  `sizeof(vertex_t)` measured **96 B on 64-bit** and **72 B on rv32** (`-Os -fno-exceptions
  -fno-rtti`, `rv32imac_zicsr_zifencei`/`ilp32`), identical across all three configuration
  legs (`acl_full` OFF/ON, both `lkv_slot_t` bindings), so no supported build loses slack.

  The old numbers were ceilings held *above* the measurement, and a ceiling cannot express
  "keep this lean": both 112 B and 96 B satisfied the 120 B bound, so the 16 B the diet won
  after #380 §1 were invisible to every build and free for anyone to spend again. The 32-bit
  arm had drifted further — its own comment claimed rv32 "sits exactly on 80 with zero
  headroom" while the struct had already shrunk to 72, an assertion that stayed wrong because
  the `static_assert` beside it still passed. Pinning to the measurement keeps every reclaimed
  byte by construction and makes the prose re-derivable rather than remembered.

  The cost is one number to lower in whichever commit shrinks the struct; that is now stated
  in both `@brief` blocks and in both `static_assert` messages. Verified by ablation: adding
  one `std::uint64_t` member to `vertex_t` fails the build on the 64-bit assert, with the
  intended message.

- **`tr::graph::subscription_t` is OPAQUE — it no longer hands out the producer `vertex_t*`
  (#867).** The handle was a `struct` with two public members, `vertex_t* vertex` and
  `std::size_t slot`, so `sub.vertex->store(...)` and `sub.vertex->mark_unregistered()`
  compiled for any API user (`fill`, `refresh_registered_child` and `add_child` sit in the same
  public section of `vertex.hpp`). Those are lock-contract mutators — valid only under the
  graph's map/stripe locks, and not ACL-gated — so the handle was a documented-as-opaque door
  straight past the discipline `graph_t` exists to enforce.
  Aggregate initialization also let a caller FORGE `{any_pointer, any_index}` and feed it to
  `unsubscribe()`. It is now a `class` shaped exactly like `vertex_handle_t` (ADR-0056): both
  members private, `graph_t` the sole `friend` — the only code that can build one and the only
  code that can read the pair back. **Public surface kept:** default construction (still the
  `NOT_FOUND` no-op handle), copy/pass-by-value (`static_assert`ed trivially copyable, so
  privatizing costs no wrapper), and a new `operator==` — two handles compare equal iff they
  name the same slot on the same producer, which is how a caller now observes RFC-0009 §D.2
  slot reuse. **Public surface removed:** `.vertex` and `.slot`, and aggregate/2-arg
  construction. A caller that read either member (both in-tree readers were tests asserting
  slot reuse) migrates to `==`; there is no accessor to migrate to, by design. Doc-only
  entities and generated docs move with it (`doxygenstruct` → `doxygenclass`). Zero runtime
  cost: `graph.cpp.o`'s `.text` is byte-identical across the change.

- **BEHAVIOUR: `bus_link_t`'s peer-named wiring calls are REFUSED on a link that is not
  `peer_named()`, and a peer-named link no longer downgrades to flat delivery (#889).**
  `set_peer_receiver`, `set_peer_rope_receiver` and `set_peer_down_notifier` now return
  without installing when the link reports `peer_named() == false`. `bus_link_t` is a PUBLIC
  base, so those setters were reachable on a FLAT tcp/ws listener by an explicit upcast past
  the null `bus()` — and landing one silently flipped the server into peer-named delivery
  that the `bus() == nullptr` contract said did not exist. The refusal lives in `bus_link_t`
  itself, not in a derived shadow, so the upcast cannot dodge it. The mirror change: the
  tcp/ws servers' per-frame tier select now reads the constructed mode, so a **peer-named**
  server with only a flat `transport_t` receiver wired DROPS its inbound frames instead of
  delivering them untagged — an untagged frame off a many-peer link grows a return route that
  names the LINK, and a bus mount's own name is not a routable next-hop (RFC-0020 /
  ADR-0073 §3): its `send()` BROADCASTS, so the reply would go to every peer. In-tree nothing
  changes: `fwd_router_t::add_child` installs the peer receiver strictly inside
  `if (link.bus())`, so the two conditions already coincided everywhere the router wires.

- **The label control plane emits ADVERTISE and HANDLE_NACK by scatter-gather, not by
  building a frame (#885).** Four sites — `fwd_router_t::advertise` (the producer door),
  `on_advertise`'s forwarding-hop re-advertise, `on_compact`'s stale-label NACK and
  `on_nack`'s re-advertise — reached for the THROWING
  `encode_advertise` / `encode_handle_nack`. Three of them run on a transport receive thread
  and are entirely peer-provoked, so on the `-fno-exceptions` profile a peer could drive the
  node into `abort()` by exhausting the heap; which policy applied was decided by which
  spelling the author happened to reach for, since the same plane's COMPACT egress had
  already been zero-allocation since #862. **No frame changes on the wire** — the head
  arithmetic and the label child come from the same `label_tlv` / LL-widening loci the
  builders use, pinned across the u16→u32 widening boundary by `compact_cache_test` driving
  the real router doors. Public signatures are unchanged; the emitters are internal to
  `core/src/fwd_router.cpp`. What this does NOT close, and what #603 still owns: the label
  TABLES (`ensure_egress`, `bind_ingress*`) and `on_advertise`'s route deep-copy and
  `wire::encode` re-encode still allocate through throwing paths, so `on_advertise` remains
  a peer-reachable abort under `-fno-exceptions` for reasons this change does not touch.
- **`net::route_handle_t::release_egress(out_link, label, route)` — hand back a label taken
  from `ensure_egress` that never went on the wire (#833).** The unwind a refused forwarding
  bind needs: the egress entry is erased and, when the label is still the allocator's most
  recent, `next_label` walks back so the 16-bit space is returned too. It erases **only a
  MINT**, which is what makes it safe now that an egress entry is SHARED across every ingress
  flow with an identical stripped route (#913): an entry carries "the mint is still the only
  take of this label", set by `ensure_egress` when it creates the entry and cleared by the
  first reuse, and this call erases nothing once that is false. So an established flow — whose
  take was a reuse — is never unwound by a newcomer's refusal, and neither is an entry a
  second advertise took between this caller's mint and its refusal. A release for a link with
  no tables, a label that is not there, or a route the entry no longer holds is a no-op, and a
  release never CREATES a link shell. No wire surface moves: a refused bind advertises
  nothing, so a released label is one no peer has ever seen.

- **BREAKING: `net::child_registry_t::child_t` publishes its link and its SHAPE as ONE atomic
  word; the `link` and `multi_peer` data members are replaced by `egress()` / `link()`
  (#882).** The two were separate atomics — `add`'s rebind stored the shape, then the link —
  and the forward mount descent read them in the opposite order. A reconnect rebind that
  FLIPS a name's shape could therefore hand a forward a stale point-to-point shape paired
  with a fresh **bus** link, and the descent returned that link as a directed egress: its
  `send()` fans out to every open peer, which is the one-request/N-replies misroute (#409)
  the descent's own rejected-hit branch exists to prevent. `bound_egress` had the same shape.
  Reading the link first was measured **insufficient** — a second rebind landing between the
  two loads reproduces the same pairing — so the shape bit now lives in the link pointer's
  spare low bit (`child_registry_t::kBusShapeBit`) and the invalid pairing cannot be spelled.
  Migration: `c.link.load(order)` → `c.link()`; `c.multi_peer.load(order)` → shared with the
  link via `const auto eg = c.egress();` then `eg.link` / `eg.multi_peer`. `live()` is
  unchanged, `sizeof(child_t)` is unchanged at 80 bytes, and the forward path takes one
  acquire load where it took two. A tombstone now clears the pointer and KEEPS the shape bit,
  so a dead bus mount still rejects a residual segment instead of falling through to the
  local terminus (ADR-0073 §3). Recorded as ADR-0063 erratum 6.
- **`tr::net::slot_server_t` (`libtracer/posix_endpoint.hpp`) — the multi-peer slot/poll
  machinery is now ONE base class, and `transport_tcp_server` / `transport_ws_server` derive
  from it (#871).** Both servers used to restate the whole connection layer line-for-line
  (~230 lines, with byte-identical `run()` bodies): the slot struct and its threading rule,
  the bind/listen/getsockname bring-up, the free-slot-or-grow accept with its `max_peers`
  refusal and `p<slot>` naming, the poll loop, the two-phase `teardown_slot`, the
  `bus_link_t` query trio, the destructor slot sweep and the broadcast's
  pristine-iovec-copy-per-peer fan-out. All of that now lives once, in `slot_server_t`
  (the tier above `stream_endpoint_t`, the shape `msquic_endpoint_t` already uses for
  quic + webtransport), parameterised by two variance points — a per-accept setup/handshake
  hook and a per-readable-chunk framing hook. **No behaviour change on either wire**, and the
  ingress/egress surface of both servers is unchanged.

  **Source-compatible for callers**, but the class hierarchy is public API: the servers were
  `public transport_t, public bus_link_t, private stream_endpoint_t` and are now
  `public slot_server_t`, which is `public transport_t, public bus_link_t, protected
  stream_endpoint_t`. Every existing conversion (`transport_t*`, `bus_link_t*`, the
  `dynamic_cast` back to the concrete server) still compiles and still resolves; a
  `sizeof(transport_tcp_server)` or a member-offset assumption does not, since the shared
  members moved into the base. `ok()`, `local_port()`, `bus()`, `enumerate_peers()`,
  `peer_link()` and `close_peer()` are inherited rather than redeclared — same names, same
  signatures, same semantics, now with one implementation instead of two. The per-server
  `dropped_rx()` / `malformed_rx()` accessors, `transport_ws_server::effective_max_frame()`
  and both `kMaxFrame` constants stay where they were: they belong to the framing, which is
  what each server still owns.

- **A refused bus-NAME hop's error reply now carries TRAILER-LESS route bytes, like every
  other addressed error this library emits (#887).** `fwd_router_t`'s rejection built its
  `FWD{REPLY, kind=ERROR, STATUS{ERROR{tr::path::invalid}}}` with a hand-rolled encoder that
  re-serialized the request's two `PATH` nodes through `wire::encode` — which REBUILDS a
  trailer when the node carries one. The terminus resolver's error reply, from the same
  logical inputs, copies its routes trailer-sliced (ADR-0041 §4). A peer that timestamped or
  CRC'd its `src` therefore got those trailer bytes echoed back inside the reply's address
  from one path and not the other. Both paths now go through one assembler, so a refused hop
  answers with a route byte-identical to the terminus's: the trailer bytes are gone and the
  opt byte's TS/CR/CW/TF bits are clear. **Every frame this library emits is byte-identical to
  before**, because nothing here sets CW or TF on a route. A route that carries no trailer
  bytes but *does* set CW (`0x04`) or TF (`0x02`) is the one shape that changes: the retired
  encoder echoed those bits back, and the shared assembler clears them along with TS/CR, so
  such a reply's opt byte differs (measured: `06 44 …` before, `06 40 …` after, same 14-byte
  route). That is the intended correction — the reply's address must describe the bytes it
  actually carries — but it is a wire-visible difference and a conformance reader should not
  be told the trailer-less case is universally unchanged. No
  header signature changed; `assemble_reply` / `assemble_error_reply` live in `core/src`, not
  in `include/libtracer/`. The rejection reply's head segment is also now drawn from the
  router's injected `egress` backend (#795, ADR-0074) instead of the global heap, so a bounded
  node bounds this reply too — the same seam the terminus reply head already used. This
  does **not** make the rejection path nothrow: the owning `wire::decode` that opens it still
  allocates through a throwing `std::vector` (#885 owns the allocation policy).

- **The `webtransport` factory refuses a DIAL `path` that is not origin-form (#1039).** A
  config whose `path` key is non-empty and does not begin with `/` — `path = "tracer"` — used
  to construct a dialer and emit that string as the extended CONNECT `:path`; it now answers
  `graph::status_t::TYPE_MISMATCH` from `net::webtransport_transport_factory`'s DIAL branch,
  beside the existing empty-`addr` / zero-`port` preconditions, so no socket or TLS work
  happens. An `https` request's `:path` is non-empty and, in origin-form, `/`-prefixed (RFC
  9114 §4.3.1 / RFC 9113 §8.3.1), so such a value could only ever draw a `400` from a
  conformant server — which this side reports as a failed session, indistinguishable from a
  rejected certificate. Absent and empty still normalise to `/`, a `/`-prefixed value is
  passed through unchanged, and nothing beyond the leading `/` is judged (no percent-encoding,
  control-character or length rules). The `webtransport_transport_t` constructors, the CONNECT
  field-section encoder and the LISTEN-side accept arm are untouched — a direct
  `webtransport_transport_t(host, port, "tracer", …)` still dials as before, and this
  library's listener still serves every resource it is asked for.

- **`view::rope_t::try_reserve` keeps only its no-op arm in the inlinable body; the spilling
  arm moved to an out-of-line private member (#1065).** Signature, return values and
  observable effects are unchanged — the spilling arm is the previous body verbatim, and
  `try_reserve` now filters out only the inputs for which that body's `max_size` guard cannot
  fire. What changes is code shape: on `v0.8.0` gcc inlined the whole check into
  `graph_t::dispatch_edge_target`'s nothrow delivery clone, and on `main` it does not, so
  every path-target delivery paid a real `call` for a test that folds to one compare at a
  fresh 1-link rope. With the arms split, the no-op test inlines again (`nm -S`
  `dispatch_edge_target`: 0x19f → 0x2df, and the `call rope_t::try_reserve` is gone). Callers
  that actually reserve now pay one extra `call` on the arm that allocates.

- **`net::udp_transport_t` honours the universal `:settings max_frame` key (#926).** The
  constructor takes a new `max_frame` parameter **between `backend` and `recv_stack`** — a
  source-breaking change for any caller that passed `recv_stack` positionally as the fifth
  argument (in this tree, `bench/bench_conn_ram.cpp` was the only one). `0` keeps the
  previous behaviour exactly. A non-zero value is the largest datagram the connection
  accepts: a longer one is never delivered, the new `malformed_rx()` counter ticks, and the
  socket serves the next datagram normally — so long as the injected backend can furnish
  `max_frame + 1` bytes, since a segment bounded below that truncates the datagram before
  its length can be judged (#1074); the RX segment is drawn at the cap instead of at
  `kMaxDatagram`, so a tight cap is a RAM lever as well as an admission rule. Two new
  accessors, `malformed_rx()` and `effective_max_frame()`, mirror the names `tcp_transport_t`
  and the `ws` transports already carry. The `udp` factory threads `conn_settings_t::max_frame`
  into both the DIAL and the LISTEN shape, so the key now reaches the socket from a plain
  `/net:children[]` SPEC write — before this it was parsed, readable back from `:settings`,
  and ignored. Unlike the four framed kinds (see #1035), the key can only *tighten* here: a
  UDP payload cannot exceed `kMaxDatagram`, so a larger configured value is inert.

- **`view::rope_t::concat` no longer reserves on the cross-rope path — the self-concat
  guards are charged to the aliasing case alone (#1022).** The `r.concat(r)` safety added in
  #971 (an up-front `try_reserve` plus an indexed re-read of the source each step) was paid
  by *every* call, including the 1–2-link delivery clone on the path-target dispatch leg;
  that cost `inproc-target-handler` +3.5% and `inproc-target-stored` +10.1%. Source and
  destination storage can overlap in exactly one way — `&other == this` — so `concat` now
  branches on that: the aliasing arm keeps both guards verbatim, and the cross-rope arm
  walks the source span once, as it did before #971. The resulting chain is identical in
  both arms; what changes is that a *long* cross-rope `concat` takes the geometric
  `push_back` ladder again unless the caller reserves. Callers that know their final link
  count still call `try_reserve` themselves — the delivery clone and the composed-read reply
  builder already did, and `read_children_folded` now does at its own call site.

- **`ws::decode_frame_checked` fails the connection on a RESERVED opcode (RFC 6455 §5.2), and
  the new `ws::is_defined_opcode(opcode_t)` predicate names the six defined ones (#1060).**
  This changes what a WS peer observes: a frame whose opcode is outside
  `{CONT, TEXT, BINARY, CLOSE, PING, PONG}` was decoded and then silently dropped by both
  transports' `default:` arm; it is now `PROTOCOL_ERROR` off the first header byte, so both
  halves tick `malformed_rx()` and tear the connection down through the path they already
  used for a §5.5 breach. `0x3`–`0x7` are the half that met no OPCODE-SHAPE rule
  before — `is_control_opcode` is `& 0x08`, which sorts them with the DATA frames, so no
  rule a LEGAL-SHAPED one could fail applied to them; the #872 `max_payload` bound did
  still reject an over-cap reserved frame — and one of those
  arriving between two fragments was skipped without disturbing the assembler, so the
  continuations were stitched around it. `TEXT` and `PONG` are unaffected: they are DEFINED
  opcodes, they still decode, and each transport still ignores them. The UNCHECKED
  `ws::decode_frame` is unchanged and still decodes reserved opcodes — it owns no connection
  to fail, and `tests/conformance/ws_diff_fuzz.py` holds it against the TypeScript decoder.

- **`fwd_router_t::advertise` now REUSES the label already bound to an identical route
  instead of minting a fresh one per call (#913).** Both of the router's label-minting sites
  — this producer door and `on_advertise`'s mid-chain forwarding arm — called
  `route_handle_t::alloc_label` + `record_egress` unconditionally, with no reuse scan. Since
  re-advertising *is* the RFC-0004 §E.1 self-heal, a peer drives that path as often as its
  link flaps, and every cycle consumed one more of the link's 16-bit labels and appended one
  more egress entry. Neither is reclaimed individually: only a whole-link `clear_link` gives
  them back, so a reconnect loop walked a long-lived node to label exhaustion (permanent loss
  of compaction) and its egress table to `max_bindings_per_link`. Both sites now go through
  `route_handle_t::ensure_egress`, the primitive `deliver_remote` already used, which finds
  the label for an identical route under the egress table's own lock and mints only for a
  genuinely new route. The bound therefore comes from the route set the peer actually
  advertises, not from a cap on how often it may re-advertise. **Observable change:** repeated
  `advertise(link, route)` calls for the same route return the SAME label rather than
  successive ones; a new route still mints. The ADVERTISE frame still goes out on every call,
  so no peer sees a behaviour change and the wire is untouched. Minting and recording in one
  critical section also retires the old pair's split outcome — a label minted, then burned for
  nothing when the record was refused — and, because the reuse scan runs ahead of the table
  bound, a re-advertise of an ESTABLISHED flow now survives a full egress table.
  `core/tests/fwd_readvertise_reuse_test.cpp` counts the state: 51 identical re-advertise
  cycles left 51 distinct labels and 51 egress entries at every node of a two-hop chain, and
  now leave 1.

- **A vertex stores its `:acl` as ACEs and NOTHING else: `vertex_t::set_acl` takes only the
  parsed list, `vertex_t::acl_bytes` is REMOVED, and `vertex_t::with_acl` replaces it (#907).**
  The vertex used to keep the written TLV bytes *beside* the parsed ACEs, and `:acl` reads
  served those bytes verbatim while the gates walked the list — two artifacts that could
  describe different policies. They did: an outer `ACL` TLV whose `opt.PL` bit was clear
  carried its whole ACE collection as opaque **payload**, so it decoded with zero children,
  parsed as an **empty** ACE list, and was stored — clearing enforcement — while a read of
  `:acl` still returned the payload. An auditor saw ACEs present, which under the
  any-present-ACE-closes rule means *closed*, on a vertex that had just been thrown open.
  Two changes close it, and only the second is general:
  - The `:acl` write branch now requires a **structured** outer ACL; a primitive one is
    `TYPE_MISMATCH`, per the same rule #906 applied inside an ACE — a shape the builder never
    emits is refused, because leniency in an ACL widens a grant rather than losing a field.
    An **empty container** (`opt.PL=1`, zero children) remains the sanctioned clear.
  - `graph_t::read_acl` **re-encodes** the stored ACEs through `encode_acl`, so read-back is
    canonical by construction and there is no second copy left to disagree with the list
    `acl_allows` evaluates — for this shape or any future one. A read therefore returns the
    canonical spelling whichever accepted spelling was written (the two-byte `access_mask` of
    the `acl/acl-aces` vector comes back as four). Same cost class as the copy it replaces,
    on a control-plane-rare path.

  `vertex_ext_t::acl` (the byte copy) is gone, replaced by an `acl_present` bit that lands in
  existing padding: an ACL written **empty** still reads back as an empty container, distinct
  from the `NOT_FOUND` of a vertex that never had one. `with_acl(f)` hands `f` that bit and
  the ACE list together under one hold, since a clear landing between two accessors would
  otherwise be served as an ACL that no longer exists.

- **`vertex_ext_t::acl_cache_dirty` is REMOVED; ACL-cache validity is now the parity of
  `vertex_ext_t::acl_gen` (#880, [ADR-0078](../docs/adr/0078-acl-cache-coherence-is-a-published-generation-stamp-not-a-dirty-flag.md)).**
  The effective-ACE merge was guarded by a `{acl_gen, acl_cache_dirty}` pair: invalidators
  bumped the counter and stored the boolean **lock-free**, while the rebuilder cleared that
  same boolean under the stripe lock. A mark landing between the rebuilder's generation
  recheck and its clear was therefore **overwritten** — the cache stayed flagged clean over
  a pre-write ancestor chain, and every later `acl_allows` on that vertex evaluated the
  stale merge until the next `:acl` mutation anywhere in the chain. On the authorization
  path that is a revoked policy that keeps being enforced, in whichever direction the stale
  merge happens to point. Validity is now derived from the counter alone: `acl_gen` **odd**
  means the merge is stale, **even** means `eff_aces` is the merge published for exactly
  that value. Every invalidator advances it to the next odd value with one lock-free CAS,
  and the rebuilder publishes with `compare_exchange_strong(snapshot, snapshot + 1)` — so
  the recheck and the publish are the SAME atomic operation and there is no second store to
  lose. `acl_gen` starts at `1` (was `0`), a never-built cache being stale. The evaluation
  fast path stays at ONE atomic load plus a parity test, which is what the removed boolean
  cost — a separate stamp word measured ~1 % slower on the `acl-inherit-d4` gate bench and
  was rejected for it (ADR-0078 Erratum 1); the shipped form measures ~1 % *faster* than
  the pre-fix baseline, and `vertex_ext_t` loses 4 bytes. `vertex_t`'s verbs (`set_acl`,
  `mark_acl_cache_dirty`, `with_effective_aces`) keep their names and signatures; the
  removed `vertex_ext_t` field is the only source-visible change. The new
  `invalidate_acl_cache` helper that carries the counter advance is **private** — the
  adversarial pass caught it landing in `vertex_t`'s public section, which would have made
  that sentence false; nothing outside `vertex_t` calls it, and the build confirms it.
  Regression:
  `core/tests/acl_cache_race_test.cpp`, an ancestor `:acl` rewriter racing a descendant's
  gated evaluation.

- **SECURITY — a SPEC-created `quic` / `webtransport` dialer now VERIFIES the server
  certificate, and two new DIAL-side config keys say how (#918).** Every connection built
  through the `:children[]` SPEC path — `quic_transport_factory` and
  `webtransport_transport_factory`, i.e. every config-created dialer there is — hardcoded
  `insecure_no_verify = true` and passed an empty CA bundle, so the handshake accepted **any**
  server certificate, a MITM's included. The dial side already supported real verification
  (`quic_dial_tls_t` / `webtransport_dial_tls_t` both default secure, and the msquic
  credential honours both `ca_file` and the flag); a hand-constructed transport could reach it,
  the factory could not, and no config key existed to ask for it. Two things change:
  - **The default is verification.** Both factories now dial with the trust struct's declared
    defaults, so with no trust key present the handshake validates against the **system trust
    store** and a certificate that does not chain to it is refused — creation answers
    `TRANSPORT_DOWN`, the did-not-come-up status (this bullet said `NOT_FOUND` when #918
    landed; #929, later in this same unreleased cycle, gave the condition its own member).
    (Ruled over the issue's refuse-by-omission proposal: msquic with neither the flag nor a
    CA file performs default platform validation, which is the standard TLS-client convention
    and costs nothing for the dial-a-publicly-certified-endpoint case.)
  - **Two new kind-PRIVATE config keys, identical in both kinds**: `ca` (NAME, a PEM CA-bundle
    path) verifies against that bundle instead of the system store — the way to reach a
    self-signed or privately-issued peer while still authenticating it; and `insecure` (VALUE
    u8, default 0) set to `1` skips validation entirely, DEV ONLY and deliberately explicit.
    Both are read through `net::config_reader_t` (the pair-consuming #927 walk), and both
    factories now parse the config **before** the role split — the DIAL branch used to return
    before `parse_quic_config` / `parse_wt_config` ever ran, which is why the dial side had no
    reachable keys at all. `conn_settings_t` is untouched (ADR-0043 §5 leanness).

  **This is a behaviour break for anything that SPEC-dialed a self-signed peer** — the
  in-tree `quic_test` / `webtransport_test` e2e vectors did, and now pass `ca = <the dev
  cert>`; a dev or interop harness doing the same must add `ca` or `insecure = 1`. The
  breakage is the fix: a dialer that silently skipped validation because the test suite found
  it convenient is the defect. Hand-constructed transports (`quic_transport_t(host, port,
  tls)`) are unaffected — that constructor always took an explicit trust struct.
- **`graph::parse_acl` rejects the non-canonical width, pairing and key shapes that used to
  read leniently (#906).** Not *every* shape the builder never emits — a two-byte
  `access_mask` and a non-canonical key ordering are both unemitted and both still parse, on
  purpose. The `:acl` write gate read its fields leniently, and on a security surface
  leniency does not lose a field — it changes what the document grants. Four arms are closed, each with its
  own rejection vector in `core/tests/security_acl_test.cpp`:
  - **Width-tolerant numeric reads inverted a decision.** `detail::load_le` reads the low
    `min(size, sizeof(T))` bytes, so a `type` sent big-endian as `u16` `0x0001` (DENY)
    read as its low byte `0x00` — **ALLOW** — and passed the `t > 1` gate, while a `u64`
    `access_mask` was truncated to `u32` with its high bytes dropped and `has_mask` still
    set. A numeric field's payload must now be non-empty and no **wider** than the field
    (`type`/`flags` u8, `access_mask` u32, `expires_ns` u64); anything wider is
    `TYPE_MISMATCH`. A **narrower** payload is still accepted — little-endian
    zero-extension is exact, and it is the canonical spelling: reference/05 §`0x0A`
    declares `access_mask` as `u16` and the `acl/acl-aces` conformance vector (and the
    Rust core's builder) emit two bytes where `encode_acl` emits four.
  - **A known key carrying the wrong value TLV type was silently skipped**, so an
    `expires_ns` paired with a non-`VALUE` child left `expires_ns = 0` and a time-limited
    grant became permanent. It is now `TYPE_MISMATCH`.
  - **Unknown keys were ignored**, dropping whatever restriction a newer writer meant to
    add. They are now `TYPE_MISMATCH` — the deliberate OPPOSITE of `net::config_reader_t`
    (#927), which skips them: config is where a newer peer legitimately sends more than
    the receiver understands, an ACL is not.
  - **The field scan visited every offset**, so a `NAME`-typed value (the legal
    `OWNER@`/`EVERYONE@` subject spelling) was re-read as the next key and bound the
    *following* key's name as the subject. The walk is now **pair-consuming**, the
    mechanics of `config_reader_t`: a non-`NAME` in a key slot, an odd child count (a
    trailing key with no value), and a repeated key within one ACE are all
    `TYPE_MISMATCH`.

  Not a wire change: `encode_acl`'s output and the published `acl/acl-aces` vector both
  still parse. Callers that hand-built a non-canonical `:acl` blob now get `TYPE_MISMATCH`
  at write time instead of a grant that differs from what they wrote.

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

### Removed

- **`tr::net::try_encode_advertise` / `tr::net::try_encode_compact`
  (`libtracer/route_handle.hpp`) — deleted (#885).** They existed to make a per-frame
  allocation on the label plane *refusable* rather than fatal. That allocation no longer
  exists: since this change every ADVERTISE, COMPACT and HANDLE_NACK the router puts on a
  link is written as a 12-byte head on the stack with the route or payload referenced, so
  there is nothing left to refuse and machinery that made the residual affordable outlives
  the residual. Their two production call sites (`fwd_router_t::deliver_remote`'s
  auto-promote leg) now emit through the same gather locus the forwarding hop already used.
  A caller that genuinely wants a frame as a VALUE keeps
  `encode_advertise` / `encode_compact` / `encode_handle_nack`, which are unchanged in
  signature and in the bytes they produce and are now documented as builders for tests,
  tooling and conformance vectors rather than for an egress path.

### Fixed

- **WebTransport: a peer-opened stream is reclaimed when it finishes (#1163).** `impl_t::ctxs`
  had **no `erase` anywhere** in the TU: the only frees were two wholesale harvests (peer
  replacement, endpoint teardown), so every stream a peer opened and closed leaked its
  `stream_ctx_t`, its accumulator buffer and its unreleased msquic handle for the life of the
  *session* — and the peer chooses how many that is. `PeerBidiStreamCount`/`PeerUnidiStreamCount`
  do not bound it: they cap how many streams may be open **at once**, not how many may be opened
  over a session's life, so open/close cycling grows the list as fast as RTT allows. The refuse
  path made it worse, not better — the cheapest thing a peer could do was also a leak.

  `stream_cb` now handles `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`, the one point at which msquic
  guarantees no further callback for the stream. Exactly one side frees: whichever of the
  shutdown and a harvest reaches `conn_m` first takes the ctx, and the loser sees that decision
  through the existing `harvested` flag. Because a harvest releases `conn_m` before calling
  `StreamClose` — which blocks until in-flight callbacks return — there is no deadlock.

  **New public accessor:** `webtransport_transport_t::live_streams()`, the count of stream
  contexts the live session holds. A count that only ever rises is the signature of this bug
  class and a deployment could not see it otherwise, the same reason `dropped_rx()` and
  `malformed_rx()` are public. It is a gauge, not a monotonic counter.

  Measured: 40 open/close cycles hold the count at its baseline of 7 (peak 8). With the
  reclamation ablated the same vector reads **peak 47, after 47** — exactly baseline + one
  entry per stream ever opened. Clean under TSan and ASan/UBSan.

- **A FLAT multi-peer server no longer reports the WHOLE LINK down when one of its several
  sessions closes (#889).** `slot_server_t::teardown_slot` fired the RFC-0009 §D.5 departure
  seam on every session close, and in flat mode that seam is `transport_t::notify_down` —
  which `fwd_router_t::link_down` answers by evicting every subscriber edge and label binding
  registered under the link's NAME. A flat listener admits unbounded concurrent peers by
  default (`max_peers = 0`), so one client's hangup evicted the routing state the peers still
  connected were relying on, silently and with nothing on the wire to explain it. The whole-
  link seam now waits for the LAST open session to depart; a mid-life close notifies nothing
  and the survivors keep routing. Peer-named mode is unchanged — it evicts exactly the
  departed peer (`notify_peer_down(name)`), which is the finer seam and always was. The rule
  lives once, in the slot layer both stream servers share since #871. Admission was NOT
  clamped to one peer instead: a flat server's `send()` broadcast to every open peer is a
  used surface, not an accident.

- **A refused forwarding bind no longer strands the out-label and egress route it had to take
  first (#833).** `on_advertise`'s forwarding arm takes its downstream label before it can bind
  the inbound swap, because the binding names that label. When the bind refuses — a full
  ingress table, or the #827 epoch guard — the hop returns **without advertising**, so what it
  took stayed in the LIVE downstream table with no ingress binding aiming at it and no peer
  that had ever seen it, reclaimable only by that link's next `clear_link`. Per refused route
  that cost one label out of the saturating 16-bit space, the retained route bytes, and — on a
  node with `max_label_bindings_per_link` set — one of the downstream table's bounded slots,
  which is enough to make a later legitimate flow refuse for want of room. The arm now hands
  the take back (`route_handle_t::release_egress`). The established-flow reuse path is
  untouched by construction: only a MINT is reclaimable, and an established flow's take is a
  reuse. Nothing on the wire changes in either direction — the refusal still advertises
  nothing and the upstream's next `COMPACT` still draws the ordinary stale-label
  `HANDLE_NACK`.

- **A connection SPEC now resolves its MODULE before its link, so a `provide_link` staging can
  no longer be picked by leaf NAME alone (#883).** `provide_link` keys its staging
  `<module>/<name>`, but `make_connection` scanned the whole staged map comparing only the
  substring after the last `/`, took the first hit in the map's lexicographic order, and read
  the module half back **out of that hit** — the half was never compared against anything. Two
  consequences, both silent: with `mod-a/x` and `mod-b/x` staged, a SPEC meaning `mod-b`
  mounted at `/net/mod-a/x` wired to `mod-a`'s transport (wrong module *and* wrong link); and
  because the scan ran *before* the `(kind, role) → module` declaration, a SPEC naming a
  `kind` was captured by any staged link sharing its leaf NAME — the kind's factory never ran.
  Creation now resolves the module first (a `kind` names its declared module per ADR-0073 §4;
  a kind-less SPEC takes it from the staged set) and then looks the staging up **directly**,
  by the exact key the map is keyed with.

  **Contract change, three parts.** (1) `kind` is no longer "ignored when a link is staged": a
  staged link takes precedence over construction only **within the module the SPEC resolves
  to**. A SPEC whose `kind` is declared under a different module now builds that kind's socket
  there and leaves the staging untouched — previously the staging captured it. An application
  that relied on a leaf-NAME match across modules must stage under the module its `kind`
  declares (or omit the `kind`, which is the staged-link spelling). (2) A kind-less SPEC whose
  leaf NAME matches **two or more** stagings is refused with `status_t::TYPE_MISMATCH` (wire
  `SCHEMA_TYPE_MISMATCH`, PERMANENT) instead of binding one by map order; the SPEC must carry
  a `kind` whose declared module says which staging it meant. The refusal is total — neither
  staging is consumed. (3) A SPEC carrying a `kind` for which **no module is declared for that
  role** now fails `status_t::SCHEMA_NOT_FOUND`, even when a link is staged under the matching
  leaf NAME — module resolution runs first, so the leaf-NAME scan that used to rescue this shape
  is never reached. This is the one user-visible break that is a hard failure rather than a
  re-route, and it applies to single-module staging too. The migration is to call
  `register_module(<module>, <kind>, <role>)` once; a `kind` used purely to disambiguate needs
  only that declaration and **not** a `register_transport_type` factory, because a staged link
  at the resolved module short-circuits before the factory lookup.

- **`graph_t::evict_link_edges("")` / `vertex_t::evict_link_edges("")` now match nothing and
  return 0, instead of reclaiming local `delivery_compact` edges (#1056).** The predicate keys
  a slot on the link it was **admitted over** — `subscriber_remote_t::link` when the cold half
  carries one, the stored `caller` gate context otherwise (#943) — and did not exclude the
  EMPTY spelling. A local admission passes the empty caller context, so a local edge's
  admitting link is empty too, and an empty parameter compared equal to it. That is reachable
  because a local edge *can* carry a cold half: the shared SUBSCRIBER parse calls
  `ensure_remote()` for the `delivery_compact` opt-in at every door, local ones included
  (a local edge with no settings has no cold half at all and was never affected). The
  consequence was that one `evict_link_edges("")` silently reclaimed every local
  compact-opted-in edge on the node — SUBSCRIBER view, target key and cold block released,
  slot deactivated, RFC-0005 listener bookkeeping unwound to match — and reported it as an
  ordinary eviction count. The entry point returns a count and has **no error channel**, so
  the empty key is a no-op returning 0, not a new status. **No signature change**, and an
  eviction on a real link name is untouched: an empty `admitted_over` could never equal a
  non-empty key, so the guard only short-circuits the case that had no legitimate match. The
  reachable door was the public graph-level API, not a transport: `fwd_router_t::link_down` is
  the only non-test caller of `graph_t::evict_link_edges` in tree, the three non-test
  `notify_peer_down` call sites (`transport_tcp.cpp`, `transport_ws.cpp`, the ESP-IDF
  `httpd_ws_link.cpp`) each guard on a non-empty departed name, and `fwd_router_t::add_child`
  refuses an empty mount name outright (`routable_mount_name`), so the point-to-point notifier
  and `remove_child` cannot carry one either.

- **A FWD frame the router could classify but whose op VALUE it could not read is now
  resolved at the terminus on BOTH cursor tiers, instead of vanishing on the rope one
  (#870).** Router ingress classification was written twice — `on_frame_rope_impl` for the
  scatter-gather tier and `on_frame_impl` for the contiguous one — and the two copies had
  drifted at their tails. The span arm concluded "type byte says FWD ⇒ terminus"; the rope
  arm asked `peek_fwd_op` once more and, on `nullopt`, fell through to the control sink,
  where `peek_control` refuses a FWD and the frame was dropped with no reply and no
  diagnosable drop. Among the frames reaching that tail, the one with an OBSERVABLE reply is a
  bound (`PATH_REF`) `dst` that `peek_fwd_dst_any` accepts carrying an EMPTY op VALUE, which
  the terminus resolver reads as `fwd_op_t::READ` — so a resolvable bound READ was answered
  when it arrived contiguously and disappeared when the identical bytes arrived fragmented.
  That is the observable instance, not the whole set: any FWD with an unreadable op reached
  that tail, including ones whose `dst` the peek REFUSES, and the rope tier's disposition of
  all of them now matches the span arm's. The
  span arm's disposition is the one kept (a FWD-classified frame is data-plane, never
  control), and both tiers now run ONE templated classification driver over the grammar
  `Cursor` seam, parameterised on the four genuinely per-tier actions. The
  ADVERTISE / COMPACT / HANDLE_NACK control switch is likewise one function now, taking the
  make-contiguous seam as its parameter. **No public signature changes** — the new
  `route_fwd_ingress` / `dispatch_control` members are private. The wire-observable change is
  broader than the bound-READ case above: a FWD whose `dst` the peek refuses and whose op is
  empty was silently dropped on the rope tier and now draws the addressed terminus reply
  (measured: 0 of 35 interior splits answered before, 35 of 35 after).

- **`graph_t::set_delivery_mode` concurrent with an `assign` no longer double-delivers, and
  `vertex_t::delivery_mode_` is atomic (#895).** `graph_t::mark_pending` chose which RFC-0008
  sweep set a vertex belonged in by reading `vertex_t::delivery_mode()` with NO lock, then
  rendering the vertex's key (an O(depth) parent walk plus an allocation), and only then
  taking `sweep_mutex_` to insert into `pending_`. `set_delivery_mode` holds that same lock
  across the mode store and both set edits, so a flip to `UNCONDITIONAL` landing inside that
  window ran to completion first and the marker's insert then put the key into `pending_` as
  well — the vertex sat in BOTH sets, and the next covering `propagate` collected it from
  each and **delivered it twice in one sweep**. The same window could leave a now-`EXPLICIT`
  vertex in `pending_`, which an ancestor sweep must never include at all. `mark_pending` now
  re-reads the mode under `sweep_mutex_` before inserting, which makes the two sets mutually
  exclusive by construction; the unlocked read is kept only as the fast path it always was.
  Separately, that unlocked read against `set_delivery_mode`'s store was a **data race** on a
  plain `delivery_mode_t` byte — UB. (An earlier draft called it "the one member of its
  four-byte field group that was not atomic while `own_subs_`, `listeners_above_` and `flags_`
  were"; that is wrong twice over — `role_` and `registered_` are also plain members of that
  group, and `own_subs_` / `listeners_above_` are 4-byte atomics outside it.) The member is now
  `std::atomic<delivery_mode_t>` with relaxed accessors. `vertex_t::delivery_mode()` and
  `vertex_t::set_delivery_mode()` keep their signatures, `sizeof(vertex_t)` is unchanged (the
  atomic is byte-wide, and the `#361` ceilings still hold), and the sweep's delivery semantics
  across `IF_NEWER` / `UNCONDITIONAL` / `EXPLICIT` are unchanged.

- **`transport_tcp_server` publishes an accepted peer's `open`/`fd` under `write_m_`, fd
  first (#891).** The two halves of a slot's lifecycle used different disciplines on the same
  two fields: `teardown_slot` reset them under `write_m_` — so an in-flight send either
  finished against the still-open fd or observed the reset — while `accept_peer` stored them
  with no lock at all, and stored `open = true` *before* the fd. A broadcast holding
  `write_m_` mid-accept could therefore read `open == true` next to `fd == -1` and hand the
  record to `write_all_iov(-1)`, which drops it: the frame reached no peer, silently, and any
  future per-fd state would have been read in the same half-published instant. The publish is
  now one `write_m_` critical section with the fd stored first, so "open ⇒ fd valid" holds for
  every sender. Accept is cold — once per connection, off the delivery path — and the
  disassembly confirms the price: in both `transport_tcp.cpp` and `transport_ws.cpp` the
  functions a peer's traffic runs through carry identical INSTRUCTION STREAMS — both `send`
  overloads, `peer_endpoint_t::send`, `teardown_slot`, `service_peer`, `run`, `peer_link`,
  `close_peer`, `enumerate_peers` and the destructor — and `accept_peer` is the only body whose
  instructions changed. Not byte-identical, and the difference is worth stating precisely: an
  independent rebuild found three of those functions differ in alignment-padding NOPs and the
  intra-function jump offsets that follow from them, because `accept_peer` grew and moved what
  sits after it. No executed instruction changes, which is what carries the claim that
  `acq_rel → relaxed` on the exchange and `release → relaxed` on the stores cost nothing here. `transport_ws_server`
  had the safer order already (it publishes a slot NOT-open and flips `open` past the 101
  under the same lock) and now takes the same lock around its publish, so **one rule covers
  both servers**: `open`/`fd` are mutated only under `write_m_`, `name` only under `peers_m_`
  — the rule [#871](https://github.com/avatarsd-llc/libtracer/issues/871)'s shared slot server
  should lift rather than re-fork. With the lock doing the ordering, both fields drop to
  uniform `relaxed` on every access; the `release`/`acq_rel` they carried before paired only
  with relaxed loads and ordered nothing.

- **A transport that could not come up is no longer reported to a peer as a permanent
  wrong-address (#929).** `make_checked` (the shared `!ok()` check behind the built-in
  udp/tcp/ws factories) and the five hand-rolled `!ok()` sites in `transport_quic.cpp`,
  `transport_webtransport.cpp` and `transport_can.cpp` returned `status_t::NOT_FOUND`, which
  the terminus maps to `tr::path::not_found` (0x0020) — PERMANENT in the RFC-0002 registry,
  *don't retry*. A refused connect, a rejected TLS/WebTransport handshake, a listener that
  could not bind and a CAN interface the kernel would not open are all TRANSIENT, and they
  now answer `status_t::TRANSPORT_DOWN` ⇒ `tr::transport::down` (0x0060). Wire-visible: a
  `SPEC` create over the wire whose link fails now replies with 0x0060 in the ERROR TLV where
  it replied 0x0020 before. The graph address the create named is unaffected — it resolved,
  which is why `NOT_FOUND` was the wrong word for it. **The contract an embedder implements
  moved with it:** `transport_vertex_t::transport_factory_t` — the signature
  `register_transport_type` takes — documented `NOT_FOUND` as the did-not-come-up answer and now
  documents `TRANSPORT_DOWN`, so a factory written outside the library answers as the built-ins
  do; the `quic`, `webtransport` and `can` factory docs are corrected at each site.

- **`graph_t::evict_link_edges` / `vertex_t::evict_link_edges` now reclaim an edge admitted
  through the `:subscribers[N]` field-write door (#943).** They matched a slot on
  `subscriber_remote_t::link` alone. `graph_t::subscribe_wire` — the `SUBSCRIBE` op and the
  wire `:subscribers[]` append — stores that, but `graph_t::field_write`'s `:subscribers[]` /
  `:subscribers[N]` arms store the inbound link **only** as the gate context
  (`subscriber_remote_t::caller`), because such an edge re-dispatches to a *local* target and
  owns no return route to deliver over. The RFC-0009 §D.1 replace arm is reached from the
  wire (the resolver diverts only an *append* bearing a `SUBSCRIBER` to `subscribe_wire`), so
  a remote `FWD WRITE` to `/v:subscribers[N]` produced an edge no link teardown could ever
  match: permanently `active`, permanently counted in `own_subs`, holding its slot against
  `add_edge` reuse, and still writing into its target under a departed session's context —
  boot-lifetime, one per occurrence, and the reason RFC-0009 §D.5's "evicts every subscriber
  edge that named that link" was not true of every such edge. Eviction now matches the link
  an edge was **admitted over**: the delivery link when it carries one, the stored caller
  context otherwise (ADR-0018 defines that context as this node's NAME for the inbound link,
  i.e. the same name space). Edges with no cold half and edges under other links are
  unaffected, and the wire is untouched — this is entirely host-side, exactly as §D.5 says.
  The fix is deliberately *not* an `r.link.assign(caller)` at the admission door:
  `graph_t::dispatch_edge` gates its remote leg on a non-empty link, so that would have added
  a phantom `FWD{WRITE}` per publish carrying an empty return route.

- **A label-compacted (`COMPACT`) delivery is ACL-gated under the inbound link's name, like
  the full-route `FWD{WRITE}` it compacts (#974).** `graph_t::acl_allows` settles the EMPTY
  caller context as fully trusted before it invokes the subject resolver (#905) — that arm is
  the local API call. Both terminus write arms of `fwd_router_t::on_compact` reached the graph
  with the DEFAULT caller, so a peer whose flow was auto-promoted to `COMPACT` (RFC-0004 §E.1)
  wrote an ACL-protected vertex with no ACE evaluated at all, while the same peer's full-route
  `FWD{WRITE}` to the same vertex was denied — two forms of one delivery disagreeing about
  whether a policy applies. Both arms now pass `inbound_name`, which is the same string
  `op_resolver_t` presents as `inbound_link`, so RFC-0004 §F's "the target vertex's `:acl`
  authorizes the actual WRITE at the final hop" holds for either form. **No wire surface
  changes**: a denied `COMPACT` is dropped exactly as an unwritable one always was, and the
  authorization is re-evaluated per frame rather than memoized with the resolution — which is
  [ADR-0062](../docs/adr/0062-resolve-once-label-bindings-hold-resolutions-not-names.md)'s
  own rule, "a binding caches the address, never the authorization", restored rather than
  invented: an ACL written after a flow warmed applies to that flow's very next frame. The private
  `fwd_router_t::deliver_local` now takes its caller as a REQUIRED parameter — an omitted one
  is what inherited full trust here. Enforcement remains opt-in: with no subject resolver
  installed, `acl_allows` returns true on its first line and nothing is gated, whatever the
  caller — the added argument is the whole cost, and `bench_compact_delivery`'s
  `compact-terminus` rows do not move outside the noise its untouched `compact-forward`
  control shows on the same host. Guarded by
  `core/tests/fwd_compact_acl_test.cpp`, which proves the cold (`deliver_local`) and warm
  (memoized-handle) arms separately, each against a positive control.

- **A SPEC-created `ws` DIAL connection no longer drops a message the server pushes on
  connect (#1025).** `transport_ws_client`'s constructor dials, runs the opening handshake
  AND spawns the recv thread before it returns, so nothing the owner does can run first.
  `transport_vertex_t::make_connection` only wires the receiver several steps later
  (register the identity vertex, insert the connection, then `fwd_router_t::add_child`) —
  and for a DIAL link the peer's push is triggered by our own connect, so its first message
  is in flight through that whole window. Decoded before the sink exists, it hit
  `receiver_slot_t`'s empty-slot path and was dropped silently: no `dropped_rx()`, no
  `malformed_rx()`, a healthy connection. This is the door #1020's frame goes out of once
  the handshake stops eating it. The built-in `ws` factory now constructs its DIAL client
  with `defer_recv`, and creation arms the link with `start_receiving()` as its last wiring
  step, so the ordering the base class documents is the ordering that actually happens. A
  directly-constructed `transport_ws_client` keeps the historical one-phase shape unless it
  opts in. Guarded from both ends: `core/tests/ws_transport_test.cpp` has a peer write the
  `101`, a PING and a COMPLETE pushed message in ONE `send` and asserts the deferred client
  answers nothing at all until `start_receiving()`, then delivers the message; and
  `core/tests/transport_vertex_test.cpp` pins that creation arms the link only once its
  receiver is already installed.

- **A `net::fwd_router_t` sink can be installed or cleared while frames flow without
  handing the new callback the old context (#914).** The router's five observer/terminus
  sinks — `on_reply`, `on_inbound`, `on_raw`, `on_compact_delivery`, `on_stale_label` —
  were ten plain non-atomic members. Each setter stored `fn` and then `ctx` with no lock
  or atomics while the frame path read them with check-then-call on the transports'
  receive threads (`on_frame_impl`, `on_frame_rope_impl`, `on_compact`). That is a data
  race by the C++ memory model, and it has an observable failure beyond the formal one: a
  reader whose pointer load lands after the `fn` store and whose context load lands before
  the `ctx` store calls the NEW sink with the PREVIOUS sink's context, which every sink
  then casts. The documented runtime clear ("passing nullptr clears a sink") is the path
  that invites it. The five pairs now live in **`tr::net::sink_slot_t`** (new public header
  `libtracer/sink_slot.hpp`) — the observer-shaped sibling of `receiver_slot_t`, which owns
  the same publish-and-snapshot discipline for the transport delivery seam plus the tier
  select a plain observer has no use for. A slot is three words — a generation counter and
  the pair: it publishes through the counter and reads with plain atomic loads, so the frame
  path takes **no lock and never spins**. A reader that lands inside a publish reports *no
  sink* for that frame rather than waiting, which is also what keeps a single-core RTOS out
  of the unbounded-priority-inversion corner ADR-0063 erratum 1 rejected a spinlock over. An
  unset slot costs ONE load — exactly what the plain member it replaced cost — and
  `fwd_router_t` serializes the five setters against each other with one mutex that no
  reader ever takes. No signature changed: the five setters keep their `(fn, ctx)` shape and
  the sinks keep their fn-pointer types. Two behaviours are worth stating. Installing or
  clearing a sink leaves a window of a few instructions in which the slot reads as empty, so
  a frame landing exactly there is dispatched to neither the old sink nor the new one —
  deliberate, and strictly better than the wrong-context call it replaces. And a clear still
  does not stop a dispatch already in flight, so a context must outlive every possible
  dispatch exactly as it must for `receiver_slot_t`. `core/tests/fwd_sink_race_test.cpp`
  flips a sink between two self-identifying contexts on one thread while another pumps
  frames, and fails if any sink is ever handed the other's context — with the production
  change reverted it failed both of its two scenarios on all 10 runs. Costs `fwd_router_t`
  80 bytes of per-instance state on x86-64 (`sizeof` 440 → 520: five 24-byte slots and one
  mutex in place of ten pointers); the router is a per-node object, not a per-frame or
  per-link one.

- **A `net::transport_ws_client` no longer drops the frames a server pipelines behind its
  `101` (#1020).** `transport_ws_client::handshake` accumulated the HTTP response into a
  buffer of its own until `\r\n\r\n`, validated the `101`, and returned a bare `bool` — so
  everything the same `recv` returned *after* the header block died with that buffer. Those
  bytes are already off the socket, so the recv loop could never read them back: a server
  that pushes state the instant the handshake completes (legal, and what any push-on-connect
  server does) lost its first message with no counter moving and the connection looking
  healthy. Timing-dependent — it needs the `101` and the frame to coalesce into one `recv` —
  so it presented as flakiness rather than a clean failure. The handshake now hands the
  post-header remainder to `serve`, which seeds its receive buffer with it and drains before
  polling, so a complete pipelined frame is decoded even when nothing further arrives. This
  is the DIAL half of a rule the accept half already followed
  (`transport_ws_server::service_peer`'s carry-over). Two adjacent corrections come with it.
  The 16 KiB runaway-response guard now bounds the HEADER scan only: it used to be applied on
  the same pass that completed the header, so a response whose header block plus the bytes
  pipelined behind it crossed 16 KiB was refused as runaway even though its header had ended
  (a latent edge rather than the reported failure — the read chunk is 1 KiB, so reaching it
  needs a header block already near 16 KiB). And the `101` / `Sec-WebSocket-Accept` checks now
  scan the header block rather than the whole buffer, so neither can take a match out of the
  frame bytes behind it. No public signature changed — `handshake` and `serve` are private.
  `core/tests/ws_transport_test.cpp` writes the `101`, a PING, and the first fragment of a
  BINARY message in ONE `send`, and asserts both the PONG and the assembled message; the
  100 ms pause that masked this in `core/tests/ws_rx_bound_test.cpp` is removed.

- **The hazard domain's overflow spin lock no longer shares a cache line with `orphans`
  (#1027).** `detail_hp::registry_t` ended with two unpadded members, so `overflow_lock` landed
  eight bytes past `orphans` — one `kDomainAlign` line for both (offsets +8384 and +8392,
  measured on x86-64 with `kCacheLineBytes = 64` and `kHazardReaderSlots = 64`). A thread that
  could not claim an index takes and drops that flag once per load, store and clear — a
  `test_and_set` at one end of its `ticket_t` and a `clear` at the other, two unconditional
  read-modify-writes; `orphans` is LOADED by threads inside the budget — `detail_hp::scan` opens
  with one and `retire_and_flush` does too, so once per `~hazard_slot_t`. An over-capacity
  thread therefore took exclusive a line that in-capacity threads read: the same class as #899
  against a different pair of fields, and the residue that PR named rather than claimed away.
  The flag moves into `detail_hp::overflow_lock_t`, a `kDomainAlign`-aligned wrapper whose
  alignment is `static_assert`ed the way `cell_t`'s and `claims_t`'s are; `orphans` ends up
  alone on its line as a consequence, since the padded claim table precedes it. Cost is 64
  bytes of `.bss` in a registry that was already 8,448 (now 8,512), and none on a single-core
  profile, where `kCacheLineBytes` is 0. No public signature changed and no behaviour changed —
  this is storage, and `sp_atomic_slot_t`, the default binding, emits none of it.

- **An over-capacity `hazard_slot_t` thread no longer CAS-sweeps every reader's announcement
  line on every operation (#899).** `detail_hp::cell_t` packed the claim flag into the same
  `kDomainAlign`-aligned struct as `pinned`, the announcement a reader writes on every
  `load()`. A thread that found every index taken keeps `kNoIndex` forever, so each `ticket_t`
  — one per load, store and clear — re-ran `participant_t::claim()`, which
  `compare_exchange_strong`ed all `kHazardReaderSlots` flags with no prefilter; a failed CAS
  still takes its line exclusive, so one over-capacity thread invalidated the hot line of
  every in-capacity reader, which is precisely the false sharing `kDomainAlign` is spent to
  prevent. The claim state moves into `detail_hp::claims_t` — a packed bitmap on its own
  padded line, with `try_claim` / `release_claim` the one operation participants and the exit
  sweep share — and `claim()` prefilters each word with a relaxed load, so probing a full
  table is `kClaimWords` shared reads and **no** read-modify-write at all (measured through
  the guard: 32,000 claim RMWs over 500 reads before, 0 after). The prefilter is deliberately
  allowed to read stale: a slot freed a moment ago is picked up on the thread's next
  operation, where a permanent "claim failed" flag would strand it on the overflow index for
  life. `participant_t::claim_probes()` is new — a plain per-thread count of claim-table RMWs,
  which is the only thing that distinguishes one probe from sixty-four. No behaviour change
  for a thread that holds an index, and none at all for the default `sp_atomic_slot_t`
  binding, which emits none of this.

- **SECURITY — `EVERYONE@` is now a RESERVED subject token, enforced on the resolver's output
  (#908).** `detail_acl::ace_applies` special-cased an ACE whose subject bytes spell
  `"EVERYONE@"` to match every resolved subject, but nothing reserved that string: `parse_acl`
  accepts any non-empty subject bytes, and no check constrained what a `subject_resolver_t`
  returned. Since the wire has ONE spelling for a subject token — the `acl/acl-aces`
  conformance vector sends `peer-a` and `EVERYONE@` as the same opaque VALUE — a deployment
  whose resolver passes a caller-supplied identity through (a username, a certificate CN, a
  peer name) could mint a principal that IS the wildcard, and an ACE meant for that one
  principal would grant everyone. The reservation lived only in prose, so every integrator had
  to know to blacklist it. `graph_t::acl_allows` now refuses a resolved subject equal to the
  reserved token — at every gate, on a guarded vertex and on a bare one, the same fail-closed
  arm the resolver's own error return takes (#905) — and both policies' `allows` return
  `NO_MATCH` for such a subject, so the pure seam cannot be fooled either. **New public API:**
  `tr::graph::kEveryoneSubject` (the spelling) and `tr::graph::is_reserved_subject` (the
  predicate, so an integrator's resolver can refuse the token at its own door). No wire change:
  an ACE still names the wildcard exactly as the vector spells it. `OWNER@` is deliberately not
  reserved — no evaluator in this core special-cases it, so it stays an ordinary opaque token
  until one does. Moving the wildcard out of the value space entirely (a distinguished wire
  encoding plus an `ace_t` flag, the issue's other proposal) would change the wire surface and
  needs an RFC.

- **A STREAM whose ring append was SHED under pressure no longer re-delivers the previous
  entry (#925).** `vertex_t::drain_unflushed` derived "how many ring entries are new" from a
  `write_seq_` delta, but `vertex_t::store` bumps that sequence **unconditionally** — and it
  is right to: the sequence is the await/readiness cursor, the LKV publish above the append
  already landed, so a shed append must still wake `wait_for_change` and still move
  `current_seq()`. What it must not do is imply a ring entry. Because a drain removes nothing
  from the ring, the surplus delta re-took the newest **already-flushed** entry, and the
  subscriber observed the same stream element twice — a duplicate on machinery whose whole
  point is an in-order queue rather than a coalesce (RFC-0008 §E). The drain now counts ring
  APPENDS: `vertex_ext_t::last_flushed_seq` is replaced by `vertex_ext_t::appended_since_flush`,
  incremented only inside the probe-success append branch under the stripe lock both sides
  already hold, and reset by `drain_unflushed` / `mark_flushed` / retirement. No new lock, no
  new allocation, no field-width change, and `write_seq_` semantics are untouched everywhere
  else. `core/tests/graph_oom_softfail_test.cpp` drives a shed append through the real store
  path with an injected allocator failure and asserts each element is delivered exactly once.

  Not in scope: the shed write still answers `SUCCESS` and moves no `delivery_drops()`
  counter — the other half of the same shed, tracked as #1003.

- **SECURITY — a `net::webtransport_transport_t` LISTENER now pins WHICH `0x41` stream may
  become its frame channel (#919), and no longer dies on an unknown H3 frame (#920).** Both
  live in `classify_bidi` and they move strictness in OPPOSITE directions, which is the
  point: identity must be pinned, unknown extensions must be ignored.
  - **Adoption is strict (#919).** A bidirectional WEBTRANSPORT_STREAM was adopted as *the*
    frame channel on nothing but a not-yet-harvested check — the code's own comment said
    "any id is accepted". So a peer could (a) stream frames with the extended CONNECT never
    completed (a handshake bypass), (b) name any session id, and (c) open a SECOND `0x41`
    stream that silently overwrote `frame_stream` while the first context kept feeding the
    one shared `length_prefix_framer` — two independent streams interleaved into one
    length-prefix reassembly, i.e. garbled frames delivered upward or a spurious malformed
    teardown. `PeerBidiStreamCount = 4` made that reachable. Three guards now run under
    `conn_m`: the session must be established, the session-id varint must name THAT CONNECT
    stream, and no frame channel may be adopted yet (**first valid one wins**). A refusal
    aborts **only that stream** (`StreamShutdown(ABORT)`, context parked as `DRAIN`) — a
    nonconforming stream cannot take down a live session.
  - **Unknown frame types are ignored (#920).** Any first frame type other than `0x41` or
    HEADERS shut the whole connection down with `kAppErrBadRequest`. RFC 9114 §7.2.8 requires
    unknown/reserved types to be IGNORED, and §9 has conformant peers — Chrome included —
    emit reserved GREASE types (`0x1f * N + 0x21`) precisely to catch endpoints that don't:
    a **conformant browser could take the node down**. Classification is now a skip loop that
    reads the unknown frame's length varint, drops that many bytes and continues; a declared
    length beyond the existing `kMaxHandshakeBytes` handshake cap is still refused (ignoring
    the type is obligatory, buffering an arbitrary pre-auth payload is not), and skipped bytes
    leave the accumulator before every "need more" return, so an unbounded GREASE run is
    bounded memory.

  No wire change, no public API change, and no delivery-path cost: all of it is
  stream-open/handshake-time classification on the LISTEN side. A peer that opened a second
  frame stream, or that expected an unknown H3 frame to be fatal, will observe the new
  behaviour; the well-behaved DIAL client is unaffected.

- **`net::transport_ws_server` / `net::transport_ws_client` take the injected RX seam every
  other framed transport takes, and their ingress is bounded by it (#872).** Both
  constructors gained `mem::mem_backend_t* backend` + `std::size_t max_frame` in the
  **same positions** `tcp_transport_t` / `transport_tcp_server` use — a **source-breaking
  reorder** for the server, whose signature is now
  `(bind_port, backend, max_frame, max_peers, peer_named, recv_stack)`. A call that passed
  `max_peers`/`peer_named` positionally must be updated; `transport_ws_server(port)` and
  `transport_ws_client(host, port)` are unchanged.

  What it fixes: inbound bytes accumulated in a plain `std::vector` with no size check while
  `ws::decode_frame` decoded the full announced 64-bit length and simply waited for that many
  bytes. An unauthenticated peer therefore named the receiver's memory budget, and on the
  `-fno-exceptions` profile the failed growth is a peer-triggered `abort()`. The declared
  length is now checked against the effective cap — `min(max_frame,
  backend.max_segment_size())`, resolved through the shared
  `length_prefix_framer::effective_cap`, so the bound is the injected resources' and never a
  literal — **off the frame HEADER, before a body byte is buffered**; and against the
  **reassembled total** of a fragmented message, so the CONT route is not a way around it.
  Either breach fails the connection (RFC 6455 §7.1.7). Message fragments are now copied into
  segments drawn from `backend` (`view::over_bytes`'s seam-taking overload) instead of the
  global heap.

  New public API: `transport_ws_server::kMaxFrame` (the shared
  `length_prefix_framer::kDefaultMaxFrame`, 16 MiB), and on both roles `dropped_rx()` /
  `malformed_rx()` — the same two counter names tcp/quic/webtransport expose, with the
  same meanings (backend exhaustion sheds the message and keeps the link; a protocol or cap
  breach fails it) — plus `effective_max_frame()`. `:settings max_frame` now reaches `ws`:
  the built-in factory forwards `conn_settings_t::max_frame` and the process `rx_backend`,
  which it previously discarded.

  **`ws::decode_frame_checked` gained a required `std::size_t max_payload` parameter**
  (deliberately not defaulted — a transport that forgets to name its bound is the defect
  being closed). `ws::decode_frame`, the decoder held byte-for-byte against the TypeScript
  core by `tests/conformance/ws_diff_fuzz.py`, is **unchanged**: it applies neither the §5.5
  control rules nor a length cap (`ws::kNoPayloadCap`), because it never buffers on the
  caller's behalf. The RFC 6455 §5.5 control-frame limits shipped in #856 are untouched.

- **`net::transport_can` no longer attributes a group's slices to a stale binding when the
  endpoint space wraps (#909).** The endpoint sub-field is 12 bits and `alloc_base` resets
  to the first data slot when a reservation runs off the end, so a base **recurs** — routine,
  not exceptional. Two receive-side structures keyed on that base and both aliased once a run
  was re-issued, because `learned_` was written only via `operator[]` on the exact base id and
  never erased:
  - **The stale binding shadowed the live one.** `process_data` takes the FIRST `learned_`
    entry (ascending base order) whose `[base, base + slice_count)` contains a slice's
    endpoint, so a wider stale range with a lower base won the scan and filed the slice under
    the wrong group at the wrong index.
  - **The reassembly key collided.** The group key is `(node, base-endpoint)`, so a recurring
    base merged slices left over from an incomplete group into the fresh one. `is_complete`
    could then be satisfied by a MIX of old and new slices and a **byte-corrupted frame was
    delivered as valid** — silent cross-talk between two unrelated payloads, not a crash.
    `core/tests/transport_can_test.cpp` drives the real `send` path around a real wrap and,
    unfixed, receives one slice of one payload welded onto another.

  Both close on one invariant, enforced when an advertise is learned: **at most one binding
  may claim an endpoint slot of a node, and a reassembly group lives exactly as long as the
  binding that feeds it.** A fresh advertise now retires every same-node binding whose run it
  overlaps and discards the group each was feeding. No wire change and no new API: the
  overlap test is arithmetic on the CAN ID's own endpoint field, so the bound stays the
  wire's, and the reclamation counts on the existing `transport_can::dropped_groups()` —
  whose meaning widens from "a `max_groups` eviction or an `rx_ttl` age-out" to include a
  re-issued run, one counter for "a group's buffered slices were reclaimed before delivery".
  A caller that read `dropped_groups() == 0` as "no wraparound has occurred" will now see it
  tick. `learned_` is no longer insert-only; `learned_binding()` returns `nullopt` for a
  base whose run has been re-issued.

  Not implemented: the producer **generation** in the advertise framing that
  [ADR-0077](../docs/adr/0077-can-advertise-carries-a-producer-generation-keying-reassembly.md)
  also proposes. Its *Implementation status* section records why — redundant against the
  invariant above, and unable to reach the residues neither instrument closes: a slice
  parked before its advertise, and a stale binding that no re-issue overlapped being fed by
  frames whose own advertises were lost. Both are bounded by `rx_ttl`; a generation rides
  the advertise and a data frame carries none, so it is silent for both.

- **`fwd_router_t` resolves a re-added child NAME to its current tenancy, and connection
  churn no longer grows its receiver chain (#884).** `remove_child` left the child's
  `child_rx_ctx_t` on the published receiver chain and `add_child` of the same NAME appended
  a second one, while the name-keyed walk answers with the FIRST match. Name reuse is a
  supported flow — `remove_connection` retires the vertex so a later connection may take the
  name, and the registry rebinds its tombstone — so after one create/remove/create cycle every
  name-keyed consumer (`connection_ref`, `hop_mint`, and through them `adopt_binding` and the
  reply-mint contribution) resolved the DEAD context, whose `conn_slot` names the retired
  tenancy: a re-created child could be permanently unbindable on the bound path while its
  canonical spelling worked, and a NAME re-added as a bus mount kept answering with the
  point-to-point slot it no longer had. The chain also grew by one `child_rx_ctx_t` (plus its
  mount run) per cycle, unboundedly, lengthening the per-bound-frame `ctx_by_conn_slot` walk —
  on a bounded node, a reboot. A ctx is now TOMBSTONED in place on removal (it stays linked,
  because a lock-free reader may be standing on it, but every walk skips it) and a re-add of
  the same NAME REBINDS that ctx instead of appending, which is the one-slot-per-name rule
  `child_registry_t::add` has followed since #494/#521. `conn_slot` is re-resolved per
  registration rather than inherited. Not a use-after-free: every pointer involved stays live
  (the deque is never popped, registry chunks are never freed, and the graph's slot table is
  pinned and insert-only) — the defect was a live pointer naming the wrong tenancy, which is
  why ASan reports nothing on either side of the fix. Measured before: 52 contexts after 51
  remove/re-add rounds on one name; after: 1.

- **A CAN group too large for the endpoint window is refused whole instead of advertised
  and then truncated (#910).** `send_impl` emitted the advertise manifest — promising
  `slice_count` slices and `group_total_len` bytes — *before* the per-slice loop in which
  `can::slice_can_id` runs out of endpoint slots and `break`s. Any frame over
  `kCanMaxGroupSlices` windows (>32 760 B classic, >262 080 B FD) therefore told every
  listener on the bus to expect N slices and delivered N−1, so each of them created a
  reassembly group that could never complete and buffered the partial slices until the
  `rx_ttl` sweep reclaimed them — silent at the sender, with no error and no counter.
  `alloc_base` is now the RESERVATION: it computes the run of consecutive endpoint slots
  in `std::size_t` and refuses any group that fits at no base, so the manifest is emitted
  only for a group that will be delivered in full; a refusal drops the whole frame and
  ticks `dropped_tx`. The same guard closes the silent `std::uint16_t` narrowing beside
  it — a >65 535-slice group wrapped both the reservation span and `advertise_t::slice_count`
  to `0`, which put the HELLO/presence form on the wire and left every data slice that
  followed unbindable and parked at the receiver. Advertise-then-retract was declined as
  the alternative shape: a retraction is a second wire concern (a new control-frame
  semantic every peer must implement, itself lossy on the medium that lost the tail
  slices), where the capacity is a purely local fact the sender already holds. No wire
  change.

- **A CAN ingress allocation failure drops and counts instead of fabricating an empty
  slice (#911).** `process_data` inserted
  `tr::view::over_bytes(frame.bytes()).value_or(tr::view::view_t{})` into the reassembly
  buffer. `over_bytes` returns `nullopt` for exactly one reason — the backend refused;
  an empty input still returns an engaged empty view — so `value_or` converted a
  backpressure refusal into a fabricated engaged-EMPTY slice. The buffer counts entries
  without inspecting their length, so the placeholder satisfied `is_complete`, `assemble`
  chained it, and the `min(total, rope->total_length())` trim quietly shortened the
  result: a **byte-wrong, short frame was delivered upstream as valid data**, with no
  counter moving. UDP counts the same condition as a drop. The refusal is now handled as
  backpressure: the whole group is abandoned (`can_reassembly_t::discard`, ticking
  `dropped_groups`), the slice ticks `dropped_rx`, and nothing is delivered. The copy
  draws from the injected `rx_backend` rather than unconditionally from the global heap.
  No wire change.

- **`wire::encode` no longer mints a `PATH_REF` frame its own `decode` rejects (#886).** The
  grammar has exactly one per-type structural rule — a `PATH_REF` body is a fixed-stride
  8-byte record array, so `opt.PL` and `opt.LL` are both forbidden and the length is a
  bounded multiple of 8 (RFC-0024 §4.2/§4.3) — and `grammar::parse_header` has always
  enforced it. The generic `encode` did not: it serialized any `tlv_t` verbatim, so a
  `PATH_REF` built with `opt.pl` even took the children branch and emitted per-child TLV
  framing. All four ill-formed shapes produced bytes this library answers with
  `tr::frame::invalid` — the codec round-tripped into a frame it would not accept. `encode`
  now applies `wire::path_ref_body_valid`, the same single predicate the decoder and the
  lazy forward tier call, so the rule keeps one home rather than gaining an encoder copy.

  **Scope: this core only** — and the divergence it opened is now **closed**. The same
  asymmetry was alive in the Rust and TypeScript cores, whose generic `encode` serialized an
  ill-formed `PATH_REF` verbatim while their own decoders rejected those bytes, so the three
  cores diverged on the same input tree. #1004 applied the same rule and the same
  emits-nothing postcondition in both bindings; all three cores now refuse identically. See
  `bindings/rust/CHANGELOG.md` and `bindings/typescript/CHANGELOG.md`.

  **API note — the failure mode is a new `encode` postcondition.** `encode` has no error
  channel, and an assert was declined: `NDEBUG` is set in exactly the Release /
  RelWithDebInfo profiles that put bytes on a wire, so a debug-only check would leave the
  shipped defect intact. `encode` instead **emits nothing** — the contract
  `emit_path_ref` already carries — and returns an empty vector. Empty is unambiguous: an
  accepted TLV always carries at least its 4-byte header, so no well-formed `tlv_t` encodes
  to nothing. A refused TLV refuses its ancestors too, rather than being dropped into a
  frame that decodes one component short. Well-formed input is untouched and stays
  byte-identical to `emit_path_ref`'s output; the guard costs one predicted-not-taken
  compare per TLV and allocates nothing. The conformance suite gains the standing property
  that every `encode` success must `decode`, so a future decoder rule forgotten in the
  encoder fails there.

- **The hazard domain's exit sweep no longer frees lists a live thread still owns (#898).**
  Only builds that bind `hazard_slot_t` (`-DLIBTRACER_LKV_SLOT=hazard_slot_t`) reach this;
  the default `sp_atomic_slot_t` has no domain. `final_sweep_t::~final_sweep_t` ran at static
  destruction and unconditionally `delete`d every index's `retired` and `freelist` and then
  assigned `lists_t{}` over each — with no check of `cells[i].claimed`, the flag that exists
  precisely to mark live ownership, no scan of the `pinned` announcements, and no lock. The
  sweep is a function-local static of `registry()`, so it is ordered only against objects
  constructed *after* it: any static constructed earlier is destroyed after the sweep and may
  join a worker that ran during it, and no ordering at all covers a thread that has simply not
  exited. A still-claimed participant inside `store()`/`load()` therefore had its `freelist`
  and `retired` mutated and freed underneath it — a data race and a use-after-free, reachable
  without detached threads. The sweep now takes each index through the **same** operation a
  participant uses to take it (a `compare_exchange_strong` on `claimed`, a `test_and_set` on
  `overflow_lock`), which makes the check an interlock rather than a sample: either a live
  thread holds the index and the sweep never touches its lists, or the sweep holds it and no
  thread can claim it while they are being freed. Nothing blocks — an index the sweep cannot
  get is skipped, never waited for. It also mirrors `scan`'s `seq_cst` fence and announcement
  read, so a node a live reader has pinned is left allocated rather than freed. Skipped state
  leaks, which is the correct report for a thread that outlived the domain; normal teardown is
  unchanged, since a participant that has run its destructor has already released its index.

- **`transport_vertex_t::set_link_state` and `::module_for` are now thread-safe (#881).** Both
  are public, and both read `ctl_m_`-guarded state with no lock: `set_link_state` did
  `conns_.find` while `make_connection` inserted into and `remove_connection` erased from that
  same `std::map` under the mutex, and `module_for` walked the module-declaration vector while
  `register_module` `push_back`'d it. The deployment shape makes it reachable rather than
  theoretical — `set_link_state` is the documented liveness door for a *provided* link, so it is
  called from a transport thread, while connection create/remove is wire-driven on a receive
  thread. The unguarded `find` could walk the map mid-rebalance or be handed the very node the
  erase was destroying (its `vertex` handle then read after free); the unguarded walk could be
  invalidated outright by the vector's reallocation.
  The lock could not simply be added in place: `make_connection` holds the same **non-recursive**
  `std::mutex` when it calls both, so taking it again would self-deadlock. Each is therefore
  split into a private already-holding-the-lock body plus a public locking wrapper —
  `make_connection` calls the bodies, external callers get the locked surface, and every call
  still takes at most one acquisition. The vertex write stays inside the locked section, in the
  order the class declares (this → `fwd_router_t` → `graph_t` → the vertex stripe).
  **Contract change:** `module_for`'s documented "this read takes no lock, so declare every
  module before other threads touch this object" restriction is **withdrawn** — it is now safe
  concurrently with `register_module`. No signature changed. The lock is control-plane only
  (create / remove / liveness); nothing on the forward or delivery path takes it, so there is no
  hot-path cost. `net_control_plane_race_test` gained a section that drives both public readers
  against their writers through the production `:children[]` wiring; it is TSan-clean with the
  fix and reports on either wrapper reverted.

- **A connection whose link cannot be wired into the router is now rolled back instead of
  published as a live-looking dead connection (#930).** `transport_vertex_t::make_connection`
  called `fwd_router_t::add_child` as a plain statement and discarded its `bool`. That `bool`
  is `false` exactly when the child registry could not grow, and `add_child` is the only place
  that can report it — so on an exhausted heap the identity vertex stayed registered, the
  `conns_` entry stayed inserted, `UP`/`LISTENING` liveness was published, and the create
  returned success, while the link was in no registry entry: no `dst` could route to it, no
  inbound frame resolved to it, and `remove_child` did not know it existed. Peer-drivable on a
  bounded node by creating connections until the registry slab exhausts. `make_connection` now
  checks the return and unwinds the whole creation in the order `remove_connection` uses —
  retire the identity vertex, then erase the `conns_` entry (destroying the config-constructed
  socket) — publishes no liveness, and answers `status_t::BACKPRESSURE`. A `provide_link`
  staging is likewise consumed only once the wiring has succeeded, so a retry after the
  pressure clears still finds its link. Callers of a `/net:children[]` create see one new
  outcome: `BACKPRESSURE` where the call previously reported success. The success path is
  byte-identical, and no signature changed.

- **`net::config_reader_t` no longer lets a string VALUE be re-read as a key (#927).** The
  SETTINGS walk advanced one child at a time, so the `NAME` child that is the *value* of one
  pair was also tested as the *key* of the next position. Combined with the ignore-unknown-keys
  forward-compat rule, any pair whose string value textually equalled a known key silently bound
  the FOLLOWING child as that key's value — and last-match-wins then overrode a legitimate
  earlier occurrence: a newer peer's `link_hint = "addr"` made an older node parse an `addr` it
  was never sent, and it needed no unknown key at all (an ordinary `kind = "addr"` mis-bound the
  same way). The walk is now **pair-consuming** — it steps over `(NAME key, value)` pairs and
  advances past the value it consumed — so an unknown key is skipped as a WHOLE pair and a value
  can never be re-read as a key. Forward-compat tolerance is unchanged and deliberate (the
  opposite ruling from the ACL parse, where an unknown key is rejected because a dropped
  attribute widens access); so are wrong-type-ignored, empty-`VALUE`-ignored and repeat-key
  last-wins. Two behaviour differences beyond the fix: a child that is not a `NAME` where a key
  belongs now stops the walk instead of resynchronizing on the next offset, and a trailing
  unpaired key is still ignored. Well-formed configs parse identically. No signature change.

- **The same defect is closed on the webtransport cert/key parse and on two graph-layer pair
  walks (#927).** `parse_wt_config` (`transport_webtransport.cpp`) kept a hand-rolled
  every-offset copy of the walk, so the defect survived on the one shape that names a **private
  key file**: a forward-compat `hint = "key"` pair let the string `"key"` be re-read as a key,
  binding the following child as the private-key path and overriding the legitimate one under
  last-wins. It now goes through `net::config_reader_t` like the quic factory, leaving all six
  transport-side consumers on one walk. Two L4 parsers read the same positional grammar and
  cannot depend on `tr::net` (dependencies point up the layers only), so they carry the
  pair-consuming *rule* instead: `graph_t::create_child` — where a `hint = "name"` pair created
  the child at an address the sender never asked for, and the same shape re-bound `type` or
  `config` — and the SUBSCRIBER QoS `SETTINGS` parse, where it injected a `delivery_policy`
  (reliability, priority, the durability request) into a subscription that requested none. Both
  now step whole pairs and stop, rather than resynchronize, on a desynchronized stream.
  `graph::parse_acl` is the one every-offset scan left; #906 rewrites that walk whole under the
  opposite unknown-key ruling and owns it. Well-formed frames parse identically throughout.

- **The full-write helpers no longer mistake a malformed call for a dead socket (#948).**
  `write_all_iov` (and `write_all`) treated EVERY non-EINTR failure as peer-gone and dropped
  the rest of the frame in silence. `EOPNOTSUPP`/`EINVAL` do not mean the peer left — they mean
  libtracer handed the kernel arguments it rejected, on a socket that is still perfectly alive
  with the bytes still deliverable. That conflation is what let ONE unimplemented `sendmsg`
  flag on one platform become an invisible TOTAL data outage: the connection stayed up, the
  handshake and pings (single-buffer `send`, a different syscall) kept working, and every
  scatter-gather data frame vanished with nothing recorded anywhere. The shared policy is now
  three-way (`classify_write_fault` in `posix_endpoint.cpp`): EINTR resumes (#903, unchanged);
  a socket-dead errno — `EPIPE`, `ECONNRESET`, `ENOTCONN`, the unreachable/down family, and
  `EBADF`/`ENOTSOCK`, whose recycled-fd shape must not fabricate a defect report — drops the
  rest silently as before (link-down is #66 lifecycle); anything else is booked in
  `write_fault_stats()` with its errno and the write is re-attempted once, so a single spurious
  rejection can no longer truncate a framed stream. The re-attempt allowance is one per stretch
  of progress and is a proof rather than a tunable: a call malformed in its arguments is
  deterministic, so the second identical result establishes the defect is real (counted, then
  abandoned) — zero re-attempts would truncate silently, an unbounded retry would spin. No
  signature change; nothing on the success path changed (the classification lives entirely
  inside the pre-existing `n <= 0` arm).

- **A `status_t` gained without a wire mapping is now a build error, not a mislabelled error
  code on the wire (#876).** The L4→wire bridge `error_code(status_t)` ended in
  `return wire::err_t::PATH_NOT_FOUND;` after an already-exhaustive switch, so the first
  enumerator anyone added to `status_t` would have been reported to peers as
  `tr::path::not_found` (0x0020) — telling them their *address* was wrong, and inverting the
  retry disposition they read off the RFC-0002 registry. Both hand-written maps out of
  `status_t` (the bridge, and `to_string` in `status.hpp`) lose their fall-through tails, and
  the library compiles with `-Werror=switch` (MSVC `/we4062`), on the host build and in the
  ESP-IDF component alike, so the unmapped enumerator reddens the build at both sites. The
  two enums stay separate — `err_t` is the wire registry, `status_t` is L4 vocabulary — and
  every existing status maps to exactly the `err_t` it mapped to before; no wire change.
  `to_string` narrows its contract: its argument must be a `status_t` enumerator (every
  status the library produces is one), where before an out-of-range cast answered `"unknown"`.

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

- **`view::rope_t::concat` is self-aliasing-safe (#915).** `r.concat(r)` walked `other`'s
  links while `append` mutated that very storage. In INLINE mode the `kInline+1`-th append
  spills the chain, which zeroes `inline_n_` and overwrites every inline slot with `view_t{}`
  mid-walk — so a two-link `r.concat(r)` produced `[a, b, a, {}]` instead of `[a, b, a, b]`:
  **silent data corruption, wrong bytes on the wire**. In HEAP mode `push_back` could
  reallocate the vector the walk pointed into (a dangling span — undefined behaviour, now
  reproduced under ASan). `concat` now `try_reserve`s the joined link count before touching
  anything, so none of its appends spills or reallocates, and walks the source **by index**,
  re-reading it each step so a link is fetched from wherever the chain currently lives. The
  reservation is a no-op while the joined chain still fits inline, so the hot 1–2-link case
  still allocates nothing (ADR-0053 §6); for a long cross-rope concat it replaces the
  geometric `push_back` ladder with one sized growth. Cross-rope `concat` and `operator+`
  are otherwise unchanged.
- **`wire::grammar::rope_cursor` asserts its bounds preconditions instead of hiding a
  violation (#916).** `region(off, len)` clamped nothing, so a cursor could claim bytes the
  chain does not hold; `locate` answered any at/past-end offset with `{last_link, 0}`, so
  `byte_at` returned **byte 0 of the last link — a real but wrong byte** that no sanitizer
  could see (the sibling `span_cursor`'s out-of-range read is span UB that ASan/fuzz CI
  catches), and on an empty chain it was hard UB. `region`, `byte_at` and `locate` now carry
  the same debug-only preconditions `view::view_t::subview` has had — **zero release cost, no
  new branch on the hot read path** — and `locate` returns the one-past-the-end link index
  rather than fabricating a valid one, so a release-build violation is an out-of-range
  subscript a sanitizer reports. `for_each_span` — the one bulk reader — carries the same
  `off + n <= size()` containment precondition, and returns early on an empty feed, which the
  grammar's CRC path legitimately issues at the window end. Its own guards (chain-end, and
  `locate`'s past-chain assert) do not see a feed that overshoots a **narrowed** window while
  staying inside the chain, so without the precondition a two-link 3+2 rope narrowed to
  `region(0, 3)` fed `for_each_span(0, 5, …)` handed the caller chain bytes 3 and 4 and
  reported success — while the identical slip through `byte_at(3)` on that cursor aborts.
  Note the asymmetry: `byte_at`'s release-build violation still degrades to a sanitizer-visible
  out-of-range subscript, but an overshooting feed reads bytes the chain genuinely holds, so in
  a release (`NDEBUG`) build it stays silent and unsanitizable — **in release this contract is
  the caller's to keep.** No signature changed; every in-bounds caller is unaffected. Giving
  `byte_at` a **release** guarantee (an `optional` or a poisoned flag mapped to
  `FRAME_TRUNCATED`) is a separate design decision, not taken here.

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
  `-fno-exceptions` abort risk on this path, not addressed here) *(That residual was filed as
  #1110 and closed by deleting the window vector outright — see the Unreleased entry.)*

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
