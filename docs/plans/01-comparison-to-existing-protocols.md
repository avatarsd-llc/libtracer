# 01 — Comparison to Existing Protocols

> **Status**: draft, v0.1, 2026-05-03 — every cell in the matrix should have a value or "n/a"; no stubs.
> **Audience**: anyone evaluating libtracer against alternatives; the maintainer when justifying scope decisions.
> **Reading time**: ~20 min.

---

## How to read this doc

§[Feature matrix](#feature-matrix) is the one-screen summary. §[Closest competitors](#closest-competitors-narrative) is the half-page-each narrative for the three projects libtracer most directly competes with (Zenoh, iceoryx2, eCAL). §[Wrong benchmarks corrected](#wrong-benchmarks-corrected) re-frames the user's "as fast as protobuf" pitch into actual benchmarks against actual competitors. §[The three target workloads](#the-three-target-workloads) freezes the workloads the week-8 benchmark harness will measure.

---

## Feature matrix

Legend: ✅ supported / first-class · 🟡 partial / opt-in module · ❌ not supported · 🔮 planned post-MVP / aspirational · n/a not applicable.

| Feature | **libtracer (target)** | Zenoh | iceoryx2 | eCAL | Cyclone DDS | Fast DDS | NNG | NATS | Aeron | MQTT-SN | CoAP | Cap'n Proto | FlatBuffers | Protobuf | ROS2 native | OPC UA | Matter |
| ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- |
| **Modular build (pick parts)** | ✅ core ≤ 16 KB + opt modules | ❌ | ❌ | ❌ | ❌ | ❌ | 🟡 | ❌ | 🟡 | n/a | n/a | n/a | n/a | n/a | ❌ | ❌ | ❌ |
| **Smallest binary footprint (MCU)** | ≤ 16 KB target (Cortex-M3+) | ~100 KB (`zenoh-pico`) | ~MB (Rust) | ~MB | ~MB | ~MB | ~50 KB | n/a | ~200 KB | <10 KB | <10 KB | n/a | n/a | n/a | n/a | ~200 KB | ~200 KB |
| **Unified read/write API** | ✅ no `connect`/`disconnect` | ❌ (`get`/`put`/`subscribe`/`pull`) | ❌ (publish/subscribe) | ❌ (publish/subscribe + RPC) | ❌ (DDS DataReader/Writer) | ❌ | ❌ (REQ/REP/PUB/SUB...) | ❌ (publish/subscribe + JetStream) | ❌ | ❌ | 🟡 (REST verbs) | n/a | n/a | n/a | ❌ | ❌ | ❌ |
| **Zero-copy intra-host** | ✅ via SHM module + ownership transfer | 🟡 SHM-only path | ✅ first-class | 🟡 SHM transport | 🟡 SHM | 🟡 SHM | ❌ | ❌ | ✅ | ❌ | ❌ | ✅ access | ✅ access | ❌ | 🟡 | ❌ | ❌ |
| **Zero-copy inter-host** | ✅ ownership transfer at delivery | ❌ (copies) | n/a | ❌ | ❌ | ❌ | ❌ | ❌ | 🟡 | ❌ | ❌ | n/a | n/a | ❌ | ❌ | ❌ | ❌ |
| **MCU support (Cortex-M)** | ✅ M3+ (M0 single-thread mode) | ✅ via `zenoh-pico` | 🟡 | ❌ | 🟡 | 🟡 | 🟡 | ❌ | ❌ | ✅ | ✅ | n/a | n/a | n/a | 🟡 | ❌ | ✅ |
| **ROS2 bridge** | 🟡 via eCAL/Zenoh interop, not first-class | ✅ `rmw_zenoh` | 🟡 ext | ✅ ros2-bridge | ✅ default | ✅ default | ❌ | ❌ | ❌ | ❌ | ❌ | n/a | n/a | n/a | ✅ | ❌ | ❌ |
| **Pub/sub** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | 🟡 (observe) | n/a | n/a | n/a | ✅ | ✅ | ✅ |
| **Request/reply** | 🟡 via field-write semantics | ✅ (queryable) | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ | n/a | n/a | n/a | ✅ | ✅ | ✅ |
| **Query / storage** | ❌ explicitly out of scope | ✅ first-class | ❌ | ✅ recorder | 🟡 | 🟡 | ❌ | ✅ JetStream | ✅ archive | ❌ | ❌ | n/a | n/a | n/a | 🟡 | ✅ historical | ❌ |
| **QoS knobs** | 🟡 5 of 22 DDS policies (RELIABILITY, DURABILITY, HISTORY, DEADLINE, LIVELINESS) | 🟡 ~10 | ❌ | 🟡 | ✅ all 22 | ✅ all 22 | 🟡 | 🟡 | 🟡 | 🟡 | ❌ | n/a | n/a | n/a | ✅ | ✅ | 🟡 |
| **Discovery** | 🟡 module: mDNS / static / gossip | ✅ scouting | 🟡 SHM-local | ✅ multicast | ✅ SPDP | ✅ SPDP | ❌ | ✅ via server | 🟡 | ❌ | ✅ multicast | n/a | n/a | n/a | ✅ | ✅ | ✅ | ✅ |
| **Security model** | 🟡 modules: TLS / DTLS / PSK / ACL | ✅ TLS, ACL | 🟡 OS-level | ✅ TLS | ✅ DDS Security | ✅ DDS Security | 🟡 TLS | ✅ NATS Auth + TLS | ✅ TLS | 🟡 | ✅ DTLS | n/a | n/a | n/a | ✅ | ✅ | ✅ |
| **Bus protocols (CAN/I2C/SPI/UART)** | ✅ first-class via transport modules + bridge | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | n/a | n/a | n/a | ❌ | 🟡 OPC UA over TSN | 🟡 802.15.4 |
| **One address space across IP + bus + SHM** | ✅ via core bridge | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | n/a | n/a | n/a | ❌ | ❌ | ❌ |
| **Same substrate: memory = wire = graph** | ✅ load-bearing insight | ❌ | 🟡 (SHM struct ≈ wire) | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | 🟡 (memory ≈ wire) | 🟡 (memory ≈ wire) | ❌ | ❌ | ❌ | ❌ |
| **Wire-level fragmentation rules in spec** | ❌ deliberately none — address-shift slicing instead | ✅ | n/a | ✅ | ✅ | ✅ | 🟡 | n/a | ✅ | 🟡 | ✅ | n/a | n/a | n/a | ✅ | ✅ | 🟡 |
| **Browser binding (TS/WASM)** | ✅ via WS module | ✅ `zenoh-ts` | ❌ | ❌ | ❌ | ❌ | 🟡 WS | ✅ nats.ws | ❌ | 🟡 | 🟡 | ✅ | ✅ | ✅ | ❌ | 🟡 | ❌ |
| **Language bindings** | C23 / C++ / TS in v0.1; Python post-MVP | C/C++/Rust/Python/TS/Go/Java | C/C++/Rust/Python | C++/C/Python | C/C++/Python | C++ | C/C++/Go/Python | many | Java/C++/C# | C | C/many | many | many | many | C++/Python | C/C++/Python/.NET | C/C++ |
| **License** | Apache-2.0 (matches LICENSE file) | EPL-2.0 / Apache-2.0 | Apache-2.0 | Apache-2.0 | EPL-2.0 | Apache-2.0 | MIT | Apache-2.0 | Apache-2.0 | OASIS std | IETF | MIT | Apache-2.0 | BSD-3 | various | OPC Foundation | CSA |
| **Primary maintainer** | individual (see git log) | Eclipse Foundation / ZettaScale | Eclipse Foundation / ApexAI | Eclipse Foundation / Continental | Eclipse Foundation / ZettaScale | eProsima | Staysail Systems | Synadia | Adaptive Financial Consulting / Real Logic | OASIS | IETF | Cap'n Proto LLC | Google | Google | Open Robotics | OPC Foundation | CSA |

**Read across the row "Same substrate: memory = wire = graph" — only iceoryx2 and the zero-copy serialization formats partially do this, and none of them combine it with a graph addressing model.** That is the most defensible row.

---

## Closest competitors (narrative)

### Zenoh

**What Zenoh does that libtracer never will:**

- A mature ROS2 transport (`rmw_zenoh`) with growing adoption — replacing it costs the user the entire ROS2 ecosystem.
- A real query/storage layer (`zenoh-storage`, backed by RocksDB or memory). libtracer explicitly says no to durable storage.
- Geographic-scale routing with `zenoh-router` running across the open Internet, including peer-to-peer NAT traversal patterns. libtracer's bridge model is structurally simpler (LAN + bus, not WAN).
- A real ecosystem: 5+ years of production use, multiple language bindings hand-tuned by maintainers, conferences, blog posts, paid support from ZettaScale.

**What libtracer can do that Zenoh doesn't:**

- ≤ 16 KB core target on Cortex-M3+. `zenoh-pico` is ~100 KB and not configurable down — that floor is a hard architectural choice in Zenoh, not a tuning knob.
- Single read/write API. Zenoh has `z_get`, `z_put`, `z_subscribe`, `z_pull`, `z_queryable`. libtracer has `tracer_read` and `tracer_write`, with subscribe being a write-to-`subscribers[]`. This is a real ergonomic and footprint win.
- Bus-protocol transports as first-class modules. Zenoh has no story for "this device is on a CAN bus; bridge it onto IP." You write that adapter yourself.
- Same TLV substrate from memory through wire. Zenoh has a separate encoding layer (`z_encoding`).

**Honest take:** Zenoh wins on ecosystem and scope. libtracer wins (if it ships) on size, API simplicity, and the bus-protocol story. The two are not strictly competing — they cover overlapping but distinct needs.

### iceoryx2

**What iceoryx2 does that libtracer never will:**

- Engineered, audited, true-zero-copy intra-process SHM with deterministic memory pool semantics. Their bookkeeping survives crashes, supports lock-free MPMC queues, and handles the cache-line dance. libtracer's SHM transport (post-MVP, doc 05) will not match this depth.
- Rust safety guarantees on the core data structures.
- An adoption story in safety-critical automotive (carried over from iceoryx 1.x at Bosch / Continental).

**What libtracer can do that iceoryx2 doesn't:**

- Inter-host transport. iceoryx2 is intra-host only — no IP, no CAN, no bridges. Different problem space.
- Graph addressing. iceoryx2 is service+publisher+subscriber; not a graph.
- MCU support. iceoryx2 wants modern hardware with atomics, mmap, SHM file descriptors — Linux/QNX-class OSes.

**Honest take:** iceoryx2 owns intra-host zero-copy on Linux. libtracer's SHM module (post-MVP) is not aiming to displace it; it's a "good enough for the libtracer SHM use case" implementation. If the user needs serious intra-host zero-copy on Linux, **use iceoryx2**, not libtracer's SHM module.

### eCAL

**What eCAL does that libtracer never will:**

- A polished introspection ecosystem: eCAL Monitor (live topic tree), eCAL Recorder/Player (replay), eCAL Sys (process orchestration). Production tooling for robotics dev workflows.
- A mature SHM transport for intra-host on Linux/Windows.
- Direct ROS bridge (`ecal-rs`).
- Strong adoption at Continental and other robotics companies.

**What libtracer can do that eCAL doesn't:**

- MCU support. eCAL is desktop-class Linux/Windows, not MCU.
- Modular core. eCAL is take-it-or-leave-it.
- One address space across non-IP buses.

**Honest take:** eCAL is a robotics-development convenience layer. libtracer is closer to a wire protocol. If the user is doing robotics dev on Linux, **eCAL's tooling is hard to beat** — libtracer's CLI `tracer top` (week 8 of doc 02) is a much smaller answer.

### Honorable mention: NNG

NNG is the spiritual ancestor of "small composable C messaging library." It supports REQ/REP, PUB/SUB, BUS, SURVEY scalability protocols. ~50 KB binary. **The differences from libtracer:**

- NNG has no graph model — it's a connection-oriented patterns library.
- NNG has no zero-copy story across transports — copies on the I/O boundary.
- NNG has no bus-protocol transports — TCP, IPC, in-process only.
- NNG's API is connection-based (`nng_dial`/`nng_listen`/`nng_send`/`nng_recv`) — closer to BSD sockets than to a graph protocol.

If the user's needs collapse to "I just want a small reliable pub/sub on TCP," NNG is an excellent choice. libtracer aims for a layer above that.

---

## Wrong benchmarks corrected

The user's pitch contained: *"as fast as protobuf but with more real-time agility in data shapes."*

This compares a wire format to a wire format and asserts a property protobuf doesn't even pretend to have. It is not a useful benchmark. The right comparisons are:

| Question being asked | Compare libtracer against | Why |
| ---- | ---- | ---- |
| "How fast can I read a field from a received message without parsing?" | **Cap'n Proto, FlatBuffers** | These are the zero-parse-overhead formats. libtracer's TLV header gives you `type` and `length` in 8 bytes; nested-payload access is pointer arithmetic. This is the right peer. |
| "How fast can a publisher hand a message to a subscriber on the same host (process-to-process)?" | **iceoryx2, Aeron, Zenoh SHM** | Intra-host zero-copy throughput / latency. libtracer's SHM module (post-MVP) competes here. |
| "How fast can a publisher hand a message to a subscriber on another host over LAN?" | **Zenoh, NNG, NATS, Aeron** | End-to-end pub/sub latency over IP. libtracer's TCP module competes here. |
| "How small can the runtime be on a Cortex-M3?" | **MQTT-SN, CoAP, `zenoh-pico`** | Constrained-device footprint. libtracer aims to win this. |
| "How does it compare to protobuf?" | **Don't ask this question.** | Protobuf is a serialization format you call from your own transport. The question is category-confused. |

### The three target workloads

Frozen here, used by the doc 02 week-8 benchmark harness.

| Workload | Description | What it measures |
| ---- | ---- | ---- |
| **W1 — RC control** | 5 B payload, published @ 100 Hz, single subscriber on same LAN | Per-message overhead. Header + framing should dominate; payload should not. |
| **W2 — Sensor stream** | 1 KB payload, published @ 1 kHz, fanout 10× | Steady-state throughput at modest fanout. Realistic robotics sensor scenario. |
| **W3 — Camera frame** | 10 MB payload, published @ 30 Hz, single subscriber over LAN (Gigabit Ethernet) | Big-payload throughput. Tests whether ownership transfer + buffer-chain views actually keep memcopies out of the path. |

Reported metrics for each workload, against libtracer + Zenoh + NNG:

- **p50, p99, p99.9 end-to-end latency** (publisher `tracer_write` returns to subscriber callback fires).
- **Throughput** in messages/second (W1, W2) and MB/second (W3).
- **CPU consumption** (`%CPU` from `top` or `perf stat -e cycles`).
- **Resident memory** (`/proc/<pid>/status` VmRSS).
- **Binary size** (just-link statically, then `size`).

Harness rules (doc 02 specifies in detail):

- Same machine, same kernel, same OS, all three stacks compiled `-O3`.
- `taskset` to pin publisher and subscriber to specific cores; isolate from system load.
- 1M iterations per measurement; discard first 10 K (warm-up).
- Loopback (TCP `127.0.0.1`) for the LAN cases; document this honestly — real-LAN numbers will be worse and need a separate run.

---

## What libtracer is NOT competitive at — today

Two flavors of "not competitive": **structurally out of scope** (the architecture rules it out) and **addressable via a future module** (the architecture allows it; nobody has written it yet). Be explicit about which is which — the modular core means most "missing" features are extension points, not closed doors. If most gaps live in the second list, the architecture is doing its job; if every gap demands a core change, the architecture is wrong.

### Structurally out of scope (don't pitch libtracer for these)

- **Strict consistency / consensus across replicas.** Raft / Paxos / CRDT semantics belong in a layer above libtracer. The wire format carries last-write-wins by timestamp; anything stronger is the application's job. → Use etcd / a CRDT library on top.
- **Smart-home device certification** (Matter, Thread). Compliance, not protocol. → Use Matter.
- **Linux-only intra-host zero-copy with safety-cert pedigree.** Matching iceoryx2's audit trail and lock-free MPMC SHM bookkeeping is a multi-year project, not a module. A "good-enough" SHM module is on the roadmap; head-to-head displacement is not. → Use iceoryx2 if cert pedigree matters.
- **Web framework messaging.** Browser ↔ REST/WebSocket app patterns are well-served by gRPC-Web / SSE / plain fetch. libtracer is overkill for that shape — it's pub/sub graph addressing, not request/response. (Browser as a *node in a libtracer graph* via the WS module is in scope; "use libtracer as your web app's HTTP layer" is not.)

### Addressable via a future module (architecturally fine; just not built yet)

- **WAN-scale routing** with NAT traversal, STUN/TURN relays, bandwidth metering. The bridge mechanism in core is the building block; a `router_wan` module on top of `transport_quic` or `transport_ws` is the path. The hard work is NAT/relay engineering, not protocol. Until that module ships, use Zenoh or NATS.
- **Durable / replayable message logs.** A passive recorder host subscribed to `/**` writing to disk is a single-purpose module. No core change required. Until it ships, use NATS JetStream or Kafka.
- **Industrial automation (OPC UA / Modbus / EtherCAT compatibility).** A bridge module per protocol; the address-space-unification story is a natural fit. Real engineering per protocol — not free. Until they ship, use OPC UA / Modbus libraries directly.
- **ROS request/reply (services) and actions.** Pure read/write doesn't carry a return-path token natively. Solvable as a `request_reply` endpoint type in core or as a module. Latency floor with ownership transfer is competitive with `rmw_zenoh`; the gap vs full ROS is action state machines and parameters, not transport.
- **Production robotics dev tooling.** Recorder, monitor, launcher, replay — each is a separate process speaking libtracer, not part of the library. Modularity helps here (the GUI is just a subscriber on `/**`); it doesn't replace the engineering work. Until the tooling exists, use eCAL Monitor / rosbag2 / Foxglove.

---

## What's NOT in this doc

- The wire format details that the matrix's "same substrate" cell points at — see [doc 03](03-wire-format-and-data-model.md).
- The actual benchmark harness implementation — see [doc 02](02-roadmap-weeks-1-to-8.md) week 8.
- Risk register (it's in [doc 00](00-vision-and-reality-check.md), not duplicated here).
- Roadmap of when libtracer ships the features in the matrix — see [doc 02](02-roadmap-weeks-1-to-8.md).
