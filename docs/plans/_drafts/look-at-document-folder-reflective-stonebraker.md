# libtracer — Design Document Suite Plan (v2, post-feedback)

## Context

The user is building **libtracer**, a "lightweight decentralized graph-based networking protocol." Today's code is small and uneven:

- **Implemented (~14 KB):** [tlv.h](libtracer/tlv.h), [tlv_vector.hpp](libtracer/tlv_vector.hpp) (XOR-16 — not a real CRC), [tlv_string.hpp](libtracer/tlv_string.hpp).
- **Stubs:** [serdes.h](libtracer/serdes.h), [tracer.hpp](libtracer/tracer.hpp) (`name_t` redefined 3× — won't compile, wrong `STATUS` ID).
- **Docs/ folder (untracked):** chat logs about C++ buffer-segment plumbing — not a protocol design.

After a first pass, the user reframed the project substantially. The new positioning is **"WireGuard to Zenoh's OpenVPN"** — sharper, smaller, more composable, not a Zenoh fork. Load-bearing reframes:

1. **Modular by design.** Core is tiny: *buffer management + routing + endpoints + subscribers + bridge*. Everything else (transports, discovery, security, executors, scripting, GUI) is an **opt-in module**. RC car over UART pulls one transport module + core; a robot fleet pulls many.
2. **Same TLV substrate for memory, wire, and graph.** TLVs nest into a graph; underneath, the graph is a buffer-chain of views over real memory. Mix/split/concat at the graph level *without touching memory* — only views move. At endpoint delivery, **ownership of the incoming buffer is transferred** (pointer swap), not memcpy'd.
3. **Read/write only API.** No `connect`/`disconnect`. Control surface lives either as ioctl-style separate calls or as writable fields on the endpoint itself (e.g. `ep:subscribers[self].tx-rate-limit = 12`). The same primitive serves sync I/O and event delivery.
4. **No fragmentation rules in the wire format.** Address-shift handles it: a logically large message lands across `ep[0]..ep[N]`; same timestamp across the slices proves coherency. Each TLV is addressed independently.
5. **User-extensible type codes** with a small reserved core range; the protocol does not preordain every payload type.
6. **Variable-width integers** as default, with a finite-pool mode for tiny MCUs (preallocated slot sizes).
7. **Bridges in core**: decentralization is non-optional. A subscriber on any graph chunk gets data regardless of whether it's locally produced or arriving via a bridged transport. This is what makes `CAN -> IP -> RDMA -> another host` look like one address space.
8. **Both control plane AND data plane**, depending on which modules are loaded. With small modules it's a control plane; with big modules (RDMA, SHM, GPU) it's also a data plane carrying real bytes through the same addressing scheme.
9. **Goal: deliver views of buffer chunks onto endpoints, append, stream-subscribe, define graph rules** — all with smaller surface than Zenoh.

The user chose: lean 8-doc set, pure C99 core with C++/TS wrappers, git-tracked `docs/` at repo root. Those choices stand.

This plan creates the doc set. The docs *describe* the protocol and roadmap — they do not write code yet. After the docs land, week-1 implementation begins per doc 02.

---

## Deliverables — 8 documents under `docs/` (git-tracked)

### `docs/00-vision-and-reality-check.md` (~600 lines)

The honest "is it worth it" gate. Reframed verdict: **build it, focused on the modular-core + same-substrate-as-wire differentiators.**

- **Vision distilled** (2 pp): rewrite the brain-dump as crisp problem statement. The unique pitch — "WireGuard to Zenoh's OpenVPN": tiny composable core, opt-in modules, single substrate from memory through wire.
- **What exists in the wild** (1 pp table): Zenoh, Iceoryx2, eCAL, Cyclone DDS, NNG, Aeron, NATS, Cap'n Proto / FlatBuffers, Matter, OPC-UA — what each owns.
- **What Zenoh lacks** (1 pp, explicit, requested by user): monolithic dep tree, no pick-a-piece-only build, separate API surfaces for sync I/O vs events, zero-copy gated on SHM-specific path, keyspace-string addressing rather than graph paths, large binary even on `zenoh-pico`.
- **The gap libtracer fills** (1 pp): (a) one address space spanning bus+IP+RDMA, (b) modular by construction (RC-car build vs robot-fleet build are the *same library*), (c) one TLV substrate for memory, wire, and graph — no separate serialization layer.
- **Risk register** (1 pp): scope creep, single-maintainer bus factor, "1000 Gbps" framing reality check, XOR-16 → CRC-32C requirement, the danger of "modular" turning into "no one configuration is well-tested."
- **Verdict** (3 ¶): build v0.1 with the modular-core + same-substrate framing as the differentiator. **Do not** market as a general Zenoh replacement; market as "the WireGuard of pub/sub graphs." Re-evaluate at end of milestone 3 (week 6 of doc 02).

### `docs/01-comparison-to-existing-protocols.md` (~500 lines)

Feature matrix + narrative.

- Matrix axes: **modular build (pick parts)**, smallest-binary footprint, unified r/w API, zero-copy intra-host, zero-copy inter-host, MCU support, ROS bridge, pub/sub, request/reply, query/storage, QoS knobs, discovery, security model, language bindings, license, primary maintainer.
- Rows: libtracer (target), Zenoh, Iceoryx2, eCAL, Cyclone DDS, FastDDS, NNG, NATS, Aeron, MQTT/MQTT-SN, CoAP, Cap'n Proto, FlatBuffers, Protobuf, ROS2 native, OPC-UA, Matter.
- **Narrative per closest competitor** (Zenoh, Iceoryx2, eCAL — half page each):
  - What they do that libtracer never will (Zenoh: ecosystem & ROS bridge; Iceoryx: true-zero-copy SHM with the engineered bookkeeping; eCAL: introspection tooling + monitor).
  - What libtracer can do they don't (modular pick-a-piece build, unified read/write, same TLV substrate for memory+wire).
- **Wrong benchmarks corrected**: not "as fast as protobuf." Compare to *Cap'n Proto / FlatBuffers* (zero-parse access), *Zenoh / NNG / Aeron* (end-to-end pub/sub latency), *Iceoryx2* (intra-host throughput), *MQTT-SN / CoAP* (constrained-device footprint). Three target workloads frozen here: 5-byte RC control @ 100 Hz, 1 KB sensor @ 1 kHz, 10 MB camera frame @ 30 Hz.

### `docs/02-roadmap-weeks-1-to-8.md` (~400 lines)

Demo-driven plan. Every week ships a working demo that breaks the previous week's assumptions.

- **Week 1 — Core shape exists.** Fix [tracer.hpp](libtracer/tracer.hpp) (3 `name_t` collisions, wrong `STATUS` ID). Replace XOR-16 with CRC-32C. Set up CMake. **Define the core/module boundary in code** (`libtracer/core/`, `libtracer/modules/<name>/`). Demo: in-process two endpoints, one publishes, one subscribes, both via core only, no transport. Read/write API only — no connect/disconnect.
- **Week 2 — TCP module + wire format frozen.** First transport module (`modules/transport_tcp/`). Demo: button on host A → relay GPIO on host B, loopback or LAN. Wire format v0.1 frozen (doc 03 promoted to "stable").
- **Week 3 — Discovery module.** mDNS-equivalent module (`modules/discovery_mdns/`). Demo: device plug-in → other devices see it without static config.
- **Week 4 — C ABI extraction.** Split current C++ into pure C99 core + thin C++ wrapper. Catch2 tests against the C ABI. Sentinel test: build core *without* any modules and confirm binary is < 16 KB on `arm-none-eabi-gcc -Os`.
- **Week 5 — TypeScript binding + browser demo.** Compile core + TCP-over-WebSocket module to WASM (WASI). Browser button → ESP32 relay over LAN.
- **Week 6 — First non-IP transport.** CAN or I2C module (whichever target hardware is on hand). Proves "graph spans bus + IP" is real, not aspirational. Bridge mechanism in core gets exercised end-to-end.
- **Week 7 — Executor module v0.** Register `tlv_t * (*fn)(const tlv_t *, void *user)` C-callback on a vertex. Sensor → on-vertex transform → republish. Embedded MicroPython is a stretch, not a milestone commitment.
- **Week 8 — Benchmark + diagnostic GUI v0.** Benchmark harness against Zenoh + NNG on the three workloads from doc 01. Publish numbers honestly. Ship a minimal web GUI that introspects a live graph (vertex list, edge list, sample rate per edge) — proves the introspection story.

Explicit non-deliverables for the 8 weeks: FPGA executor, RDMA/UCX, NCCL plugin, CRDT coherency, Noise protocol, behavior-tree replacement, multi-language beyond TypeScript, full security ACL stack.

### `docs/03-wire-format-and-data-model.md` (~700 lines)

Combined: how bytes look on the wire AND how the same TLV substrate represents in-memory graph nodes via buffer-chains/views. Frozen end of week 2.

- **8-byte header** (carried from current [tlv.h](libtracer/tlv.h)): `type:u8`, `opt:u8`, `crc:u32` (upgraded from u16 XOR — break, version-bump), `length:varint`. Endianness: little-endian.
- **CRC**: CRC-32C (Castagnoli, hardware-accelerated on x86/ARM).
- **Length field**: variable-width integer (LEB128) by default. **Finite-pool mode** for tiny MCUs: opt-in flag in `opt` byte selects fixed slot widths from a preallocated pool — varint logic is compiled out.
- **No fragmentation rules.** Logically large payloads land across `ep[0]..ep[N]` with shared timestamp. Reassembly is an addressing concern, not a wire concern. This is a deliberate departure from DDS/MQTT/Zenoh.
- **Type code allocation**:
  - `0x00-0x1F` reserved core (current 13 IDs from [tlv.h](libtracer/tlv.h)).
  - `0x20-0x7F` reserved for future core extensions (registry-managed).
  - `0x80-0xFF` user-defined, no protocol opinion. Per user feedback: payload types are not preordained by the protocol.
- **Nested TLVs = graph over buffer chains** (the load-bearing insight):
  - A TLV's payload may be a list of TLVs (LIST type). The list IS a graph node.
  - Underneath, it's a `buffer_chain<view>` — each child TLV is a `view` over real memory, no copies.
  - Mix/split/concat operations rearrange views, never bytes. Spec-level proof obligation: any sequence of mix/split/concat must produce identical byte output if serialized.
  - At endpoint delivery, the incoming top-level buffer's ownership is **handed over** to the endpoint (pointer swap, refcount transfer). Existing draft work in [Docs/drafts/Write custom buffer_segment class.md](Docs/drafts/Write custom buffer_segment class.md) is the prior art — cite directly.
- **Versioning**: 1 bit of `opt` for major bump, document forward/backward compatibility.
- **Timestamp**: 64-bit nanoseconds since Unix epoch. Either embedded in `opt`-flagged header timestamp slot or as a separate `TIME` TLV in a LIST (both supported; spec calls out when each is used).
- **Wire byte tables** with concrete hex examples for every core type code.
- **Memory-layout examples**: side-by-side "this on the wire = this in memory as a buffer-chain view tree."

### `docs/04-graph-and-endpoint-api.md` (~600 lines)

The data model and the API contract. *Read/write only.*

- **Vertex / Edge / Path**: vertex = endpoint with a name; edge = a subscription/binding; path = ordered list of names addressing a vertex through a hierarchy. Compared side-by-side with ROS topics, DDS DataReader/Writer, MQTT topic trees, Zenoh keyspace.
- **Naming**: UTF-8, case-sensitive, `/`-separated, length cap. Wildcard rules (subscribe to `/sensor/*/temp`).
- **Endpoint API — read/write only**:
  - `tracer_read(path, &out_tlv)` — synchronous fetch.
  - `tracer_write(path, in_tlv)` — synchronous push.
  - Event delivery uses **the same write/read primitives**: subscriber's read blocks/awaits; publisher's write fans out. No separate event API. *This is the unification the user called out as a key Zenoh-distinguisher.*
- **Control surface — open question, doc proposes the answer**:
  - **Recommended**: control via writable fields on the endpoint itself. Subscribe = `write(/some/ep:subscribers[self], my_subscription_tlv)`. Set rate limit = `write(/some/ep:subscribers[self].tx-rate-limit, 12)`. Disconnect = clear the subscriber slot. The endpoint's *schema* exposes a `subscribers[]` array with addressable fields. **No `connect`/`disconnect` calls in the API at all.**
  - **Fallback** (also documented): an ioctl-style separate call `tracer_ctl(path, op, arg)` for cases where field-write is too verbose.
  - Doc presents both; recommendation is field-write, with rationale that it preserves the unified API.
- **Address-shift addressing (replaces fragmentation)**: `ep[0]..ep[7]` is one logical message. Same timestamp on all slices = coherent group. Out-of-order arrival is fine; subscriber assembles by index. Loss detection = missing index in the timestamp group.
- **QoS knobs (small, opinionated subset of DDS)**: RELIABILITY, DURABILITY, HISTORY (keep-last N), DEADLINE, LIVELINESS. Set via field-write (`ep:settings.reliability = reliable`). Defer the other 17 DDS policies indefinitely.
- **Per-subscriber state** (user feedback): each subscriber has its own writable slice (rate limit, filter, etc.) without affecting other subscribers or the source endpoint. Spec defines the slot semantics.
- **Failure model**: vertex missing (heartbeat-based liveness via subscriber field), partition (graph repartitions, eventual rejoin via discovery module).
- **Bridge as core mechanism**: a bridge is a vertex that re-publishes incoming TLVs from one transport onto the local graph. Subscribers read the local graph; transport choice is invisible to them. **This is what makes one address space across CAN→IP→RDMA possible.**
- **I2C/CAN-as-router pattern** (user feedback): on a host, the I2C/CAN driver is exposed as a vertex tree — each peripheral is a child vertex. Reads/writes to peripheral go through the unified API. Driver layer becomes a libtracer module.

### `docs/05-modules-transport-and-discovery.md` (~600 lines)

The opt-in transport and discovery module catalog, plus the contract every transport module must satisfy.

- **Transport module ABI**: small C interface a module exports to plug into core. Methods: `init`, `bind`, `send_tlv`, `poll_recv`, `mtu_hint`, `shutdown`.
- **MTU is the transport's problem** (user feedback): core sends one logical TLV; transport may itself slice (TCP segments, CAN frames) and reassemble. Addressing-level fragmentation (ep[N]) is *separate* and only used when the *source* deliberately splits a message across endpoints.
- **Catalog** (each is a separate module, separately compilable):
  - `transport_tcp` — week 2, MVP.
  - `transport_udp` — unicast and multicast.
  - `transport_quic` — post-MVP.
  - `transport_ws` — WebSocket for browser/TS binding.
  - `transport_unix` — Unix domain socket.
  - `transport_shm` — Iceoryx-style shared memory ring (post-MVP, pulls in big bookkeeping).
  - `transport_can` — week 6.
  - `transport_i2c`, `transport_spi`, `transport_uart` — pulled in by need.
  - `transport_ble_gatt` — sketched only.
- **Discovery modules** (each opt-in):
  - `discovery_mdns` — week 3, MVP.
  - `discovery_static` — config file fallback for headless / no-mDNS environments.
  - `discovery_gossip` — WAN, post-MVP, sketched only.
- **Bridging**: bridges are configured at startup (which transport modules listen on which interfaces, which paths they republish). Spec defines the routing table format.

### `docs/06-modules-executor-security-gui.md` (~500 lines)

The remaining opt-in modules. None are core.

- **Executor module(s)** — vertex-side logic:
  - `executor_c` — register C callback on a vertex (the v0.1 baseline, week 7).
  - `executor_micropython` — embed MicroPython on ESP32, callback in Python.
  - `executor_python` — CPython on Linux.
  - `executor_lua` — alternative for tinier embedded.
  - `executor_wasm` — load WASM module via WAMR (post-MVP, portable).
  - **Behavior trees explicitly out of scope.** Future-direction one-paragraph note about a directed acyclic dataflow scheduler — not for v0.1.
  - **FPGA executor** — one paragraph, aspirational only.
- **Security modules** (each opt-in, none required):
  - `security_tls` — TLS for TCP/QUIC transports.
  - `security_dtls` — DTLS for UDP.
  - `security_psk` — pre-shared key for CAN/I2C/UART (asymmetric crypto too expensive there).
  - `security_acl` — capability-based ACL enforcement (auth + ACL deferred per user; spec is sketched but not implemented in v0.1).
  - `security_noise` — Noise protocol framework (post-MVP, sketch).
  - **No security at all** is a valid build for trusted segments.
- **GUI diagnostic module** (`tools/diag-gui/`) — week 8 deliverable:
  - Web UI (TS, talks to a libtracer node via the WS transport module).
  - Live vertex list + edge list, sample rate per edge, per-subscriber QoS state.
  - Lets user point-and-click subscribe to any path to inspect data flowing.
  - This is the introspection answer for "the project is literally named libtracer."

### `docs/99-glossary.md` (~150 lines)

One-line definitions — vertex, edge, point, router, bridge, path, name, subscriber, sample, executor, TLV, segment, view, buffer-chain, transport module, discovery module, address-shift, finite-pool mode, ownership transfer. Cross-reference each to the doc that defines it canonically.

---

## Open design questions the docs must explicitly resolve (with recommended answer)

1. **Read/write only vs read/write + connect/disconnect?** → **Read/write only**, control via writable fields on the endpoint itself. (User-asked, doc 04 makes this the recommendation with an ioctl-style fallback documented.)
2. **Are type codes core or user-extensible?** → **Both**: small core reserved range (0x00-0x7F), open user range (0x80-0xFF). (User-asked, doc 03.)
3. **Variable-width integers everywhere?** → **Yes by default**, finite-pool mode for tiny MCUs. (User answered, doc 03 specifies.)
4. **Same TLV substrate for memory and wire?** → **Yes** — central insight. Doc 03 makes this precise.
5. **Bridges in core or as a module?** → **Core.** Decentralization is non-optional. Doc 04 specifies; doc 05 specifies how transports plug into the bridge.

## Things explicitly NOT in this pass (cut after review)

- Separate `language-bindings.md` — folded into doc 02 weeks 4-5.
- Separate `security.md` — folded into doc 06 (still small).
- Separate `buffer-zero-copy.md` — folded into doc 03 (substrate insight is *the* doc).
- Separate `coherency.md` — CRDT/consensus deferred indefinitely; one paragraph in doc 04.
- Separate `observability.md` — folded into doc 06 (GUI section).
- FPGA executor design — one paragraph in doc 06.
- RDMA / UCX / libfabric / NCCL / GPUDirect — control-vs-data plane distinction noted in doc 00; **the modular architecture means RDMA can be a future transport module, doc 05 lists it as a future entry**.

## Files to create

**New, all under `docs/`:**
- [docs/00-vision-and-reality-check.md](docs/00-vision-and-reality-check.md)
- [docs/01-comparison-to-existing-protocols.md](docs/01-comparison-to-existing-protocols.md)
- [docs/02-roadmap-weeks-1-to-8.md](docs/02-roadmap-weeks-1-to-8.md)
- [docs/03-wire-format-and-data-model.md](docs/03-wire-format-and-data-model.md)
- [docs/04-graph-and-endpoint-api.md](docs/04-graph-and-endpoint-api.md)
- [docs/05-modules-transport-and-discovery.md](docs/05-modules-transport-and-discovery.md)
- [docs/06-modules-executor-security-gui.md](docs/06-modules-executor-security-gui.md)
- [docs/99-glossary.md](docs/99-glossary.md)

**Not modified in this pass** (referenced; code changes happen during week 1 of the roadmap, not now):
- [libtracer/tlv.h](libtracer/tlv.h)
- [libtracer/tlv_vector.hpp](libtracer/tlv_vector.hpp)
- [libtracer/tlv_string.hpp](libtracer/tlv_string.hpp)
- [libtracer/serdes.h](libtracer/serdes.h)
- [libtracer/tracer.hpp](libtracer/tracer.hpp)
- [README.md](README.md)
- [Docs/](Docs/) — leave the chat-log brain-dump untouched.

## Existing material to reuse / cite

- [libtracer/tlv.h](libtracer/tlv.h): 13-type enum, 8-byte header, `opt_t` bitfield → carry forward into doc 03 with CRC-32C upgrade.
- [libtracer/tlv_vector.hpp](libtracer/tlv_vector.hpp): header-accessor pattern → cite as the "C++ wrapper over a buffer view" reference impl in doc 03.
- [libtracer/tracer.hpp](libtracer/tracer.hpp): the `point_i` interface is the wrong shape (has connect/disconnect) but the read/write halves carry into doc 04. Note for week-1 fix.
- [Docs/drafts/Write custom buffer_segment class.md](Docs/drafts/Write custom buffer_segment class.md): shared_ptr/weak_ptr ownership variant is the prior art for the **ownership-transfer-at-delivery** mechanism. Cite directly in doc 03's substrate section.
- [Docs/drafts/macro-to-metaprog.md](Docs/drafts/macro-to-metaprog.md): C99-vs-C++ guidance → cite in doc 02's week-4 C-ABI extraction section.
- [Docs/tracer-begin.md](Docs/tracer-begin.md): chain/view/segment vocabulary → cite in doc 99 glossary.

## Verification

Plan is complete when:

1. All 8 files exist under `docs/` and are git-add-able.
2. Each opens with a 3-line "Status / Audience / Reading time" header.
3. Each "what's NOT here" section is explicit — no silent scope leakage.
4. Doc 00's verdict is unambiguous: *build, focused, with the modular-core + same-substrate framing*. Not hedged.
5. Doc 02 week 1 is concrete enough to start Monday: file paths to fix, named demo, defined "done."
6. Doc 03 is byte-table precise — a second implementer can write an interoperable parser from it.
7. Doc 03 explains the **same-substrate insight** with a worked example (wire bytes ↔ buffer-chain-view tree).
8. Doc 04 has a worked example of "subscribe via field-write, no `connect` call."
9. Doc 04 has a worked example of address-shift fragmentation (`ep[0]..ep[7]` carrying one logical message with shared timestamp).
10. Doc 05 has the transport-module ABI written out as a C interface, ready for week 2 implementation.
11. Doc 06 explicitly defers FPGA, behavior-tree replacement, full ACL — they are *named* and *deferred*, not silently dropped.
12. Doc 01's matrix is filled, every cell populated.
13. Total suite ≤ 4500 lines combined.

End-to-end check: open `docs/00-vision-and-reality-check.md`, then `docs/02-roadmap-weeks-1-to-8.md`, then `docs/04-graph-and-endpoint-api.md`. If those three together don't tell the user (a) whether to build it, (b) what to do Monday, and (c) what the API actually looks like in code, the suite has failed.
