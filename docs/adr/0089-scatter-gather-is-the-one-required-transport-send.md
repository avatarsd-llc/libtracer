# Scatter-gather is the one required transport send; the span send is a non-virtual wrapper and the retained send stays an optional override

<!-- status: accepted -->

Status: **accepted** (2026-11-08, ruling 47, for #2442 (T1 of the #2432 from-scratch cleanup); the six open questions are answered in §Rulings, and the work is ticketed as #2679 (step 1), #2680 (step 2) and #2681 (step 3)). Reference-implementation domain per [GOVERNANCE.md](../../.github/GOVERNANCE.md): it changes the C++ `tr::net::transport_t` contract and no wire byte, so it is an ADR, not an RFC (see §Why not an RFC). Amends the send surface that [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) §1 kept virtual, without changing that ruling. Keeps the standing design rules of [`CLAUDE.md`](../../CLAUDE.md) §Design rules: no timers or clock reads, complexity lowered by deletion, compile-time by default, receiver pays, no library-internal buffers.

Every `file:line` below is read at `origin/main` `3f0def32f62fd80319bd3ceb8354de1630d8172c`. Figures marked [est] are read from code, not measured.

## Context

`transport_t` has three send entry points, added in this order:

1. `send(std::span<const std::byte>)`, the **span send**, is the one pure virtual (`core/include/libtracer/transport.hpp:689`).
2. `send(std::span<const std::span<const std::byte>>)`, the **scatter-gather send**, is virtual with a default body (`transport.hpp:731-764`). The default gathers every span into a `mem::block_array_t` drawn from the link's egress store and then calls the span send.
3. `send(head, value, lane)`, the **retained send** (RFC-0028 §6.9), is virtual with a default body (`transport.hpp:801-818`). The default builds an iov table on the stack (overflow from the egress store, `iov_table.hpp:48`) and lowers to the scatter-gather send. It copies no payload.

The router already speaks scatter-gather on every hot path: forwards and replies call `send(iov)` (`core/src/fwd_router.cpp:2121`, `:2155`, `:2306`, `:2385`, `:2803`) and remote deliveries call the retained send (`fwd_router.cpp:2561`). Only two cold reply arms call the span send (`fwd_router.cpp:675`, `:696`).

The links have followed. There are 17 production link classes. Eleven of them already treat the span send as a one-span wrapper over their own scatter-gather send: TCP's client, server and per-peer endpoint (`core/src/transport_tcp.cpp:266-271`, `:428-431`, `:476-479`), the WS server (`core/src/transport_ws.cpp:271-279`, `:336-341`), the WS client (`transport_ws.cpp:546-549`), Noise (`core/include/libtracer/transport_noise.hpp:330-333`), self-heal (`core/src/self_heal_link.cpp:194-197`), the ESP httpd link and its per-peer handle (`integrations/esp-idf/libtracer/httpd_ws_link.cpp:4387-4390`, `:4468-4471`) and the ESP WS client (`integrations/esp-idf/libtracer/esp_ws_client_link.cpp:934-938`). UDP keeps a separate `sendto` body (`core/src/transport_udp.cpp:125-140`) beside its `sendmsg` one (`:146-195`). QUIC and WebTransport keep two `send_frame` overloads (`core/src/msquic_endpoint.hpp:562`, `:590`).

The remaining three classes, CAN's two and loopback, override only the span send, and they pay for the old order:

- **CAN** (`can_transport_t` and its directed `peer_endpoint_t`) overrides only the span send (`core/include/libtracer/transport_can.hpp:499`, `:695`). A routed frame therefore takes the base gather (copy 1, from the egress store, `transport.hpp:748-762`). `send_impl` then copies it again through `view::over_bytes` (`core/src/transport_can.cpp:424`) to get an owned `view_t` that `can_frame_at` (`core/include/libtracer/can_framing.hpp:120-128`) can subview. That second copy draws from the process value pool (`mem_heap.hpp:584-590` → `heap_alloc`, `core/src/mem_heap.cpp:482-484`), not from the link's injected egress store. So a deployer who bounds the CAN link's egress store, as [ADR-0079](0079-allocation-store-composition-defaults-to-per-plane-mid.md) §Decision 4 tells them to, does not bound this draw. Each slice is then copied into its CAN frame record (`transport_can.cpp:497`), which is the wire fill and cannot be avoided.
- **Loopback** (`loopback_endpoint_t`, `core/include/libtracer/loopback.hpp:56`) overrides only the span send. A routed frame takes the base gather (copy 1) and is copied again into a block drawn from the peer's receive source (`core/src/loopback.cpp:23-38`, copy at `:33`). #2266 (loopback: three copies and a thread hop per frame) already names this.

The #2429 survey proposed making scatter-gather the one virtual (row T1). Verification corrected it: CAN goes from two copies to one if it keeps `can_frame_at`, and the base gather draws from the injected egress store, so it is not a library-internal buffer in the strict sense. The maintainer ruled T1 a separate design ticket (#2432 Implementation Decisions), because the contract change touches about 89 files. This note counts **92** at the base commit (§Migration).

## Decision

### 1. The contract

**A link implements one required method, the scatter-gather send. The span send becomes a non-virtual wrapper in the base, and the retained send stays an optional override whose default lowers onto the scatter-gather send.**

The recommended shape (§Considered options weighs the alternatives; open question Q1) keeps `send` as the only spelling a caller uses and gives the override points their own names, so no derived class can hide a base overload:

```cpp
class transport_t {
   public:
    // The three caller-facing forms. NONE is virtual. Every call site is unchanged.
    void send(std::span<const std::byte> frame) {
        const std::span<const std::byte> one[1] = {frame};
        send_gather(one);
    }
    void send(std::span<const std::span<const std::byte>> iov) { send_gather(iov); }
    void send(std::span<const std::span<const std::byte>> head, const graph::value_t& value,
              const graph::delivery_lane_t& lane) { send_retained(head, value, lane); }

   protected:
    // REQUIRED: emit iov, in order, as ONE frame. Best-effort: a frame the link cannot
    // carry is dropped and counted in drop_stats (dropped_tx), never truncated.
    virtual void send_gather(std::span<const std::span<const std::byte>> iov) = 0;

    // OPTIONAL: a link that queues keeps value by reference instead of gathering it.
    // The default is today's stack iov-table lowering (transport.hpp:801-818), unchanged.
    virtual void send_retained(std::span<const std::span<const std::byte>> head,
                               const graph::value_t& value, const graph::delivery_lane_t& lane);
};
```

- **The span send** becomes a one-entry gather in the base. Eleven of the 17 production link classes already do exactly this in their own override. They delete that override.
- **The scatter-gather send** has no default. The 35-line base gather (`transport.hpp:731-764`) is **deleted**, and with it the last place the library allocates a payload-sized temporary on a link's behalf. A link whose medium needs contiguous bytes gathers into memory it already owns: Noise into its seal buffer (`transport_noise.hpp:353`), QUIC/WT into the msquic send context (`msquic_endpoint.hpp:590-608`), loopback into the peer's receive block.
- **The retained send** keeps its contract and its default. It is a capability, not a fourth way to send bytes: only a link that queues needs it (TCP, WS server and client, httpd, ESP WS client, self-heal). Folding it into the required method would make every test fake take a value pointer and a lane it ignores (Q2).
- **The best-effort contract is unchanged** (`transport.hpp:661-688`): `void`, and a counted drop.
- **`egress_source()` stays** (`transport.hpp:837`). Its last base user (the gather) goes, but the retained default's iov-table overflow and every socket link's gather table still draw from it.

### 2. CAN sends straight from the gather

`can_transport_t::send_gather` walks the caller's spans with a cursor. It fills each CAN frame record's data field from as many spans as the slice crosses, in the same `memcpy` that fills the record today (`transport_can.cpp:497`). Slice count and lengths are unchanged (`can_frame_count`, `can_framing.hpp:83-87`). The advertise still takes the total length (`transport_can.cpp:458-469`). Both payload-sized copies and both allocations go: the base gather and the `over_bytes` segment. `can_frame_at` loses its production caller (Q3).

Every slice is written in-call (`link_->write_raw`, `transport_can.cpp:500`), so the cursor borrows the caller's spans only for the call. That is the contract the scatter-gather send already has, and no ownership is needed.

### 3. Loopback gathers once, into the receiver's block

`loopback_endpoint_t::send_gather` draws the frame block from the peer's receive source (receiver pays, as today) and gathers the spans straight into it. This is the send half of #2266's fix shape. That ticket keeps the deque, thread and delivery-tier work, and T2 (loopback leaves required core) keeps the move.

### 4. Nothing else moves

- No wire byte changes. Every link emits the same bytes whichever entry point a caller used, because eleven of the 17 production classes already funnel into one body and the other six emit identical bytes from either body. CAN emits the same advertise and the same slices.
- There is no new member on `transport_t`, no new knob, and no new field in `conn_settings_t`.
- No buffer is added, in the library or in any link.

## Copy counts per transport

A **copy** here is a whole-payload `memcpy` in user space between the caller's spans and the bytes handed to the OS, the driver or the peer's block. Kernel copies are not counted. Nor is CAN's per-slice wire fill into an 8/64 B frame record, which is the medium's own and is listed separately. "Queued" means the stream link's writer role is busy, so the frame waits in a lane (ADR-0087 §2). "Retained" means the head-plus-value form.

| Transport | Entry form | Before | After | Where (before) |
| --- | --- | --- | --- | --- |
| UDP | span | 0 (`sendto`) | 0 (`sendmsg`, one entry) | `core/src/transport_udp.cpp:125-140` |
| UDP | iov / retained | 0 (`sendmsg`) | 0 | `transport_udp.cpp:146-195`; retained via `transport.hpp:801-818` |
| TCP | span / iov, idle | 0 | 0 | `core/src/transport_tcp.cpp:266-310` (`write_gather`, `core/src/posix_endpoint.cpp:336-339`) |
| TCP | span / iov, queued | 1 (whole frame into the shared egress frame) | 1 | `posix_endpoint.cpp:375-393`, copy at `:386` |
| TCP | retained, queued | 0 payload (head copied, value kept) | 0 | `transport_tcp.cpp:277-289`; `posix_endpoint.cpp:375-393` |
| WebSocket server | span / iov, idle | 0 (unmasked, header + spans in one `writev`) | 0 | `core/src/transport_ws.cpp:271-334` |
| WebSocket server | span / iov, queued | 1 | 1 | as TCP, `posix_endpoint.cpp:386` |
| WebSocket server | retained, queued | 0 payload | 0 | `transport_ws.cpp:285-304` |
| WebSocket client | all | 1 (mask-and-gather, RFC 6455 §5.1 requires it) | 1 | `transport_ws.cpp:568-596`, copy at `:585` |
| ESP httpd WS | iov | 1 (gathered once into the block every peer shares) | 1 | `httpd_ws_link.cpp:4392-4400` |
| ESP httpd WS | retained | 0 payload | 0 | `httpd_ws_link.cpp:4380-4385` |
| ESP WS client | all | 1 (masked) | 1 | `esp_ws_client_link.cpp:934-960` |
| Noise | one span | 1 (sealed out of place into the datagram buffer) | 1 | `transport_noise.hpp:353-357` |
| Noise | iov | 1 (gathered into the buffer, then sealed in place) | 1 | `transport_noise.hpp:343-360` |
| QUIC / WebTransport | all | 1 (into the msquic send context, which must outlive the call) | 1 | `core/src/msquic_endpoint.hpp:590-608`, copy at `:605` |
| Self-heal | all | 0 of its own (forwards to the inner link) | 0 | `core/src/self_heal_link.cpp:194-217` |
| **CAN** | iov / retained (every routed frame) | **2** (base gather from the egress store; `over_bytes` from the value pool) + wire fill | **0** + wire fill | `transport.hpp:748-762`; `core/src/transport_can.cpp:424`; fill `:497` |
| **CAN** | span | **1** (`over_bytes`) + wire fill | **0** + wire fill | `transport_can.cpp:420-430` |
| **In-process (loopback)** | iov / retained | **2** (base gather; copy into the peer's receive block) | **1** | `transport.hpp:748-762`; `core/src/loopback.cpp:33` |
| **In-process (loopback)** | span | 1 | 1 | `loopback.cpp:23-38` |

Allocations follow the copies. CAN goes from 2 payload-sized allocations per routed send to 0. Loopback goes from 2 to 1. Every other row is unchanged, because those links already gather natively. Noise and QUIC/WT keep their one copy for reasons this note does not touch: encryption output, and msquic buffer lifetime (#2259 owns the second).

## Migration

### The count

At the base commit, **92 files** declare a `send(std::span<const std::byte>)` override. They were counted by a multi-line scan for `void send(...) override` over every tracked `.hpp`/`.cpp`:

| Area | Files | Override span only (must gain a gather override) | Override span + iov (delete span, rename iov) | Also override retained |
| --- | --- | --- | --- | --- |
| Core production headers (`core/include/libtracer/`) | 9 | 2 (`transport_can.hpp`, `loopback.hpp`) | 7 | 3 (`transport_tcp.hpp`, `transport_ws.hpp`, `self_heal_link.hpp`) |
| ESP-IDF integration | 3 | 0 | 3 | 3 |
| Core tests (`core/tests/`) | 56 | 32 | 24 | 1 |
| Benches (`bench/`) | 16 | 4 | 12 | 0 |
| Examples (`core/examples/`) | 8 | 2 | 6 | 0 |
| **Total** | **92** | **40** | **52** | **7** |

The out-of-line definitions add 9 more source files: `core/src/{loopback,self_heal_link,transport_can,transport_quic,transport_tcp,transport_udp,transport_webtransport,transport_ws}.cpp` and `integrations/esp-idf/libtracer/esp_ws_client_link.cpp` (`httpd_ws_link.cpp` is already in the 92). Add `transport.hpp` itself, and the flip touches **102 source files**. Four doc pages name the send forms: `docs/modules/transport.md`, `docs/modules/interface-map.md`, `docs/modules/fwd-router.md` and `docs/reference/14-can-transport.md` §Egress (non-normative). There are two changelog fragments, core and esp-idf. No binding is affected: `bindings/` has no C++ `transport_t` subclass.

### The order

Each step builds, passes every gate and can be measured on its own.

1. **CAN and loopback gain native gather overrides** (2 headers, 2 sources, tests). This step is behaviour-only and not BREAKING: both classes add `send(iov) override` beside their span override. It delivers the whole copy win (§Copy counts) and lands the bench rows that prove it (§Perf), before any contract moves. CAN's span override becomes a one-span wrapper like everyone else's. If #2266 has already landed, the loopback half is done.
2. **Every fake that overrides only the span send gains a gather override**: 38 files (32 tests, 4 benches, 2 examples). This is mechanical, and no test changes behaviour. It can be split by directory into as many PRs as the train wants.
3. **The flip, BREAKING, one atomic PR.** In `transport.hpp`, the three `send` forms become non-virtual and the base gather is deleted. `send_gather` becomes pure, and the retained default moves to `send_retained`. In all 92 + 9 files, `send(iov) override` is renamed to `send_gather`, `send(head, value, lane) override` to `send_retained`, and every one-span wrapper override is deleted. UDP's separate `sendto` body is deleted too (Q6). The same PR carries the docs pages and the two `changelog.d/2442.changed-breaking.md` fragments (core, esp-idf), each with a migration snippet for an embedder link. This step is mechanical apart from `transport.hpp`, and a grep gate (no `void send(` declared in any `transport_t` subclass) proves it complete.
4. **Follow-ups, each its own ticket:** `can_frame_at` loses its production caller (Q3); T2 moves loopback out of required core; #2259 moves QUIC/WT onto the egress module.

Steps 1 and 2 can run in parallel. Step 3 must be the only open PR that touches transport headers while it is in review, because any link PR that lands under it has to be redone (one actor per PR set). #2617 (wait module: epoll and poll adapters, receive timeouts deleted) is open now and edits the posix links, so step 3 goes after it.

## Per-target cost

The profile rungs are those of [reference/12](../reference/12-deployment-profiles.md): NARROW is a single-transport leaf, MID is a forwarder with two or more transports (the ESP32-C6 build), and WIDE is a full host.

| | NARROW | MID (ESP32-C6, IDF v6.0) | WIDE (x86-64 host) |
| --- | --- | --- | --- |
| Links affected in the image | CAN (TWAI below it), when the leaf is a CAN leaf | httpd WS, ESP WS client, CAN/TWAI, self-heal | UDP, TCP, WS, Noise, QUIC, WT, CAN (SocketCAN), self-heal, loopback |
| Flash | The Cortex-M0 sentinel links no transport, so the gate does not move. A CAN leaf loses the inline base gather and the `over_bytes` call, and gains a cursor loop: about −100 to −300 B [est] | Deleted one-span wrapper bodies (httpd ×2, ESP client, self-heal) and the CAN changes above; 1 vtable slot less per `transport_t` class (4 B each on rv32): about −0.3 to −0.8 KiB [est], to be measured by `tools/esp_size_gate.py` | Same deletions on more classes; immaterial for the host |
| Static RAM (`.bss`/`.data`) | 0 | 0 | 0 |
| Per-object RAM | 0 (no member added or removed; `egress_src_` stays) | 0 | 0 |
| Transient heap per CAN send | 2 × payload + 2 block headers → **0** | same: a 4 KiB CAN group no longer needs 8 KiB of transient blocks | same |
| Stack | CAN: the cursor (span index + offset, 8 B on rv32) replaces a `block_array_t` and an `optional<view_t>` frame; net smaller [est] | same | same |
| Library-internal buffers | none added; the base gather temporary is deleted | none added | none added |

**Receiver pays** holds throughout. No cost moves onto a receiver. Loopback's block still comes from the receiving endpoint's source. CAN's removed draws were sender-side, and one of them ignored the sender's own injected store.

**Compile-time by default:** [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) §1 ruled `transport_t` runtime-dispatched, because a transport's identity is runtime wiring data (`:children[]` creation by name) and `send` is syscall-dominated. This note keeps that ruling. It removes a virtual and adds none, and it adds no runtime member or flag, such as a "supports scatter-gather" bit, that a build could have decided.

**Transport-vertex lean requirement:** untouched. No kind-specific field enters `conn_settings_t` or `transport_vertex_t`. The change is entirely inside each kind's own class.

## Perf expectations and the rows that prove them

- **Non-CAN links: no change.** The byte paths are identical. The span send costs one extra non-virtual inline call into the same body it reaches today. Proof: the existing perf gates at 64 B and 1 KiB / 4 KiB / 16 KiB, NARROW/MID/WIDE, stay inside the A/A band. The relevant rows are `bench/bench_forward_rope.cpp`, `bench/bench_transports.cpp` (UDP/TCP/WS), `bench/bench_noise_link.cpp`, `bench/bench_transport_iov.cpp` (the scatter-gather width at which a link starts allocating: unchanged at 17 spans) and `bench/bench_forward_heap.cpp`'s `allocs=0` gate.
- **CAN: allocations and copies per routed send fall to zero.** On a fake `can_link_t`, expected per send: `allocs_per_op` 2.00 → 0.00, `bytes_per_op` ≈ 2N + 2 headers → 0, `payload_copies` 2 → 0, at 64 B / 1 KiB / 4 KiB / 16 KiB. CPU time per send falls by two allocate/free pairs and 2N bytes of `memcpy`. On a real bus the send is bus-time-dominated, so latency gains are visible on `vcan` and the fake link, not on the wire. **New rows needed:** a `can-tx` stage in `bench/bench_lean_value_path.cpp` (columns `allocs_per_op bytes_per_op payload_copies ns_per_op`, the existing format) and a CAN egress arm in `bench/bench_wire_heap.cpp`, which today has no CAN arm. Both land in migration step 1 with the before numbers, and step 1's PR shows the after numbers.
- **Loopback: one copy and one allocation fewer per routed send.** Same `bench_lean_value_path` row shape, if #2266 has not already pinned it.
- **Flash:** `tools/esp_size_gate.py` on the ESP32-C6 build is measured before and after step 3, and `tools/rv32_footprint.py` on demand.

## Why not an RFC

The change reaches no normative page. [v1.md §3](../spec/v1.md) incorporates reference/03 and reference/23 as annexes. Neither describes the C++ send surface, and no frame, type code, conformance vector or binding changes. reference/14 (CAN) is non-normative and describes the egress split. Its §Egress text is updated in step 3 to say the windows are cut from the caller's gather, not from an owned view.

## Considered options

- **A. Keep the name `send` for the pure virtual (`virtual send(iov) = 0`, non-virtual `send(span)` in the base).** It is the smallest diff for the 52 files that override both forms, which just delete the span override. Rejected as the recommendation: C++ name hiding means any derived class that declares a `send` override hides the base's non-virtual span form for callers that hold the concrete type, so every such class needs `using transport_t::send;`. `core/examples/net_transport_seam.cpp:75` already carries one. That is about 70 classes each carrying a line whose absence fails only at a caller's compile. Q1.
- **B. Distinct override names, `send_gather` and `send_retained`, with every caller-facing `send` non-virtual (recommended, §1).** No hiding, and every call site stays as it is: the router, the tests, the embedders' callers. The override set becomes greppable. Cost: the 52 + 9 overriding declarations are renamed, which step 3 touches anyway. Two private helpers already use nearby names and are merged or renamed in step 3: `ws_server_transport_t::send_retained(session_base_t*, ...)` (`transport_ws.cpp:289`), and the WS server's and TCP's calls to the base retained default on another object (`transport_ws.cpp:297-300`, `transport_tcp.cpp:449-452`). Those calls need the default lowering reachable as a protected static, or the overflow arm written in place.
- **C. One virtual for all three forms**, over a frame descriptor `{iov, const value_t* kept_tail, lane}`, close to TCP's and WS's private `send_record` (`transport_tcp.cpp:291`). It is truly one method. Rejected for now: every fake would take two parameters it ignores, and a link that does not queue would need a branch it does not have today. Q2.
- **D. CAN keeps `can_frame_at`, gathering once from the egress store into an owned view.** This is the survey's corrected 2 → 1. Rejected: it keeps a payload-sized allocation per send for no benefit, because the slices are written in-call and need no ownership. Q3.
- **E. Leave it.** This keeps the 35-line base gather, CAN's two copies (one of them outside the injected store) and loopback's extra copy, and leaves the old order where every new link and fake writes two overrides.

## Consequences

- BREAKING for any embedder `transport_t` subclass: the override point is renamed, and a link that overrode only the span send must now gather. Callers of `send` are unaffected. A core and an esp-idf changelog fragment carry a six-line migration snippet: walk the spans and gather into the link's own memory, or into `egress_source()` and drop on refusal.
- The base class loses its only payload-sized allocation, and `transport.hpp` loses the 35-line gather and its comment block.
- CAN TX stops drawing from the value pool, so ADR-0079 §Decision 4 ("bounded node is a property the deployer injects") holds for CAN egress.
- ADRs and rulings touched: [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) §1 (`send` stays virtual; kept), [ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md) and [ADR-0079](0079-allocation-store-composition-defaults-to-per-plane-mid.md) §Decision 4 (the egress store; its base user goes, the store stays), [ADR-0083](0083-one-allocation-seam.md) §1 (CAN's stray value-pool draw goes), [ADR-0087](0087-the-peer-path-is-one-frame-ingress-one-shared-egress-frame-one-wait-module-per-node.md) §2 (the retained send and lanes; unchanged), RFC-0028 §6.9 (retained send; unchanged), the transport-vertex lean requirement (untouched), and the compile-time-by-default and receiver-pays rules (§Per-target cost).

### Risks

- **The flip is one large PR.** Step 3 touches 102 source files. It is mechanical, but it collides with any open link PR. Mitigation: steps 1–2 shrink it to renames and deletions, it lands alone, and a grep gate proves completeness.
- **CAN slice boundaries now cross spans.** A slice can draw bytes from several spans, so the cursor is new code on the TX path. Mitigation: the existing CAN tests compare delivered frames byte for byte, and `can_frames_test.cpp` and `transport_alloc_softfail_test.cpp` gain gather widths that put span boundaries mid-slice and on slice edges.
- **UDP's span path changes syscall** from `sendto` to a one-entry `sendmsg`. Same datagram; the cost difference is the `msghdr` setup [est, not measured]. The UDP rows in `bench_transports.cpp` cover it (Q6).
- **Out-of-tree links break at compile time, not at run time.** This is the safe direction: no link silently falls back to a gather that no longer exists.

## Rulings (2026-11-08, ruling 47)

The maintainer took every recommendation below.

1. **Q1.** Option B: distinct protected `send_gather` / `send_retained`, and a non-virtual caller-facing `send`.
2. **Q2.** The retained send (head plus value) stays a separate optional override.
3. **Q3.** CAN cuts its slices from the caller's spans with a cursor, so it makes 0 copies. `can_frame_at` moves into the tests as the oracle.
4. **Q4.** No flatten helper.
5. **Q5.** Steps 1 and 2 ship in v0.23.0 (#2679, #2680). Step 3, the breaking switch-over, ships in the next release, after #2617 (#2681). In step 1 loopback makes only the 2 -> 1 cut, and #2266 (loopback: three copies per frame) stays open.
6. **Q6.** UDP's `sendto` body is deleted, and single spans go through a one-entry `sendmsg`, with a bench in the PR.

## Open questions as posed (answered in §Rulings)

1. **Q1, override names.** Option B (`send_gather` / `send_retained`, non-virtual `send`), or option A (keep `send` virtual on the iov form, with `using transport_t::send;` in every subclass)? Recommendation: B. The names are new vocabulary; CONTEXT.md gains no entry, because they are C++ member names, not domain terms.
2. **Q2, retained send.** Keep it as a separate optional override (recommended), or fold all three forms into one descriptor-taking virtual (option C)?
3. **Q3, CAN framing.** Use the cursor over the gather (0 copies, recommended), or keep `can_frame_at` with one gather (1 copy)? With the cursor, `can_frame_at` keeps only test callers: delete it with its tests, or keep it as the test oracle?
4. **Q4, embedder helper.** Ship no flatten helper (recommended: no in-tree link needs one, and a helper would bring back the base gather under another name), or a documented free function that gathers into a given `block_source_t`?
5. **Q5, release and order.** Steps 1–2 in v0.23.0 and step 3 (BREAKING) after #2617 lands, in the same release or the next? And should the loopback half wait for #2266, or should step 1 do it?
6. **Q6, UDP's span path.** Accept the one-entry `sendmsg` and delete the `sendto` body (recommended: delete, do not keep a special case), or keep it?
