# 00 — Vision and Reality Check

> **Status**: draft, v0.1, 2026-05-03 — load-bearing doc. Read first.
> **Audience**: maintainer, prospective contributors, anyone evaluating whether libtracer is worth depending on.
> **Reading time**: ~15 min.

---

## TL;DR

libtracer is positioned as **"the WireGuard of pub/sub graphs"** — a tiny, composable, opinionated alternative to monolithic middleware like Zenoh, eCAL, and Cyclone DDS.

The verdict is **build it**, on a deliberately reduced scope:

- **In scope for v0.1**: embedded + LAN. C23 modular core (≤ 16 KB on Cortex-M3+). Five differentiators worth building (see §[Five Differentiators](#five-differentiators-that-justify-existing)).
- **Out of scope for v0.1**: HPC/RDMA, CRDT consensus, FPGA executor, code-over-the-wire, behavior-tree replacement, full DDS QoS surface.
- **Re-evaluate at week 6** of the roadmap (doc 02). If the embedded + LAN story is not credible by then, fold the project.

Marketing rule: **do not** pitch this as a Zenoh replacement. Pitch it as the smallest pub/sub graph protocol that spans CAN, I2C, IP, and SHM under one address space, with a unified read/write API.

---

## Vision distilled

The user is building a cross-platform protocol for connecting **devices, sensors, and compute** across heterogeneous transports — from a 5-byte RC-car control packet to a 10 MB camera frame, from an 8-bit MCU on a CAN bus to a Linux host doing AI inference. The unifying idea: **one address space, one wire format, one substrate for memory + transport + graph node**.

### Problem statement (one sentence)

> *"I want to wire any sensor or actuator on any bus to any other endpoint on any other host, subscribe to data flows, deploy logic where it runs best, and never write a serialization layer or a transport adapter again."*

### Success criteria (testable)

1. **A 5-byte RC-car control packet** travels from a browser button → over WebSocket → to a Linux router → over UART → to a Cortex-M3 servo controller, with one libtracer build per node, no per-link glue code.
2. The **same library binary** (after configuration) runs on a 64 KB-flash MCU and on a Linux server. Module composition determines footprint and capability.
3. A subscriber on host B reads a sensor on host A's I2C bus by referencing a graph path; transport choice (TCP, UDP, CAN bridge, SHM, …) is invisible to the subscriber.
4. The **wire format equals the in-memory graph format**. A received TLV becomes a graph node by ownership transfer, not deserialization.
5. The **API is read/write**. There is no `connect`/`disconnect`. Subscribing to an endpoint is a write to that endpoint's `subscribers[]` field. Setting QoS is a write to `settings.*`. This is the load-bearing simplification vs Zenoh and DDS.

### Non-goals (explicit)

- Not a database. No durable storage layer.
- Not a query language. No "find all sensors with X property" — that's a discovery + scan pattern users build on top.
- Not a consensus engine. No CRDT, no Raft. Last-write-wins by timestamp; if you need stronger guarantees, layer them above libtracer.
- Not a replacement for ROS2 — only the transport substrate underneath ROS-like systems.
- Not a security framework. Security modules are opt-in; the core does not assume crypto.
- Not a deployable-code system. Graph topology is deployable. Executable code is pre-flashed. WASM is post-MVP and sandboxed.

### Non-goal that the user keeps reaching for: "1000s of Gbps with 1 Gsa/s into NCCL"

This framing is unrealistic for any software-defined wire protocol. At 1 ns/sample no software touches the data path. For libtracer specifically, when the workload is GPU-direct sampling into a collective (NCCL, UCX, GPUDirect), libtracer is unambiguously a **control plane** — it negotiates the queue pair, names the buffer, propagates timestamps and metadata. The data plane bypasses libtracer's TLV layer entirely.

This is acknowledged here so that the rest of the design does not get warped by an aspiration the protocol cannot satisfy. **A future RDMA module (post-MVP, doc 05) makes libtracer the control plane that sets up the bypass; it does not make libtracer the data plane.**

---

## What exists in the wild

A one-glance map of who already does what in this space. Detailed feature matrix lives in [doc 01](01-comparison-to-existing-protocols.md).

| Project | What they own | License |
| ---- | ---- | ---- |
| **Eclipse Zenoh** | Pub/sub + query + storage middleware. Peer-to-peer or client-router. Runs on MCUs via `zenoh-pico`. ROS2 bridge `rmw_zenoh`. | EPL-2.0 / Apache-2.0 |
| **Eclipse iceoryx2** | True-zero-copy intra-host SHM IPC with engineered memory pool bookkeeping. Rust core, C++/C/Python bindings. | Apache-2.0 |
| **Eclipse eCAL** | Pub/sub + RPC with strong introspection tooling (eCAL Monitor, eCAL Recorder). Linux/Windows-first. | Apache-2.0 |
| **Cyclone DDS** | Reference-grade DDS implementation. Full 22-policy QoS. ROS2 default middleware. | EPL-2.0 |
| **eProsima Fast DDS** | Alternative DDS, also ROS2-blessed. | Apache-2.0 |
| **NNG** | Modern Nanomsg — REQ/REP, PUB/SUB, BUS, SURVEY patterns. Lightweight C library. | MIT |
| **NATS** | Cloud-native pub/sub with JetStream for durability. Server + client model. | Apache-2.0 |
| **Aeron** | Ultra-low-latency unicast/multicast messaging. Replicated logs. Java + C clients. | Apache-2.0 |
| **MQTT / MQTT-SN** | Broker-mediated pub/sub for IoT. SN variant for sensor networks (UDP, no TCP overhead). | OASIS std |
| **CoAP** | REST-over-UDP for constrained devices. Discovery via CoRE Link Format. | IETF RFC 7252 |
| **Cap'n Proto / FlatBuffers** | Zero-parse-overhead serialization formats. Not transports. | MIT / Apache-2.0 |
| **Protobuf** | Schema-driven serialization, parse-on-access. | BSD-3 |
| **OPC UA** | Industrial automation pub/sub + RPC + browse. Heavy. | OPC Foundation |
| **Matter / Thread** | Smart-home device protocol over 802.15.4 / Wi-Fi / Ethernet. | CSA |
| **Apache Kafka / Pulsar** | Durable log-based pub/sub. Server-heavy. | Apache-2.0 |

**Where libtracer plausibly fits**: between Zenoh (too monolithic for the smallest targets), MQTT-SN (no graph model, broker-mediated), and Iceoryx2 (intra-host only). The space is real but narrow. The case for libtracer rests on doing the narrow space *substantially* better, not on being-yet-another in the broad space.

---

## What Zenoh lacks (the explicit gap targeted)

Zenoh is the closest competitor and the natural baseline. Its strengths are real (mature, ROS-blessed, scouting, peer-or-router topology, runs on Cortex-M4 via `zenoh-pico`). But it has specific weaknesses libtracer can exploit:

1. **Monolithic build.** `zenoh-pico` is ~100 KB flash on Cortex-M4 with TCP. There is no "router-only", "publisher-only", or "TCP-only" build. You take the whole library or nothing. For a sensor that just needs to publish 12 bytes at 10 Hz over UART, this is overkill by 10×.
2. **Two API surfaces for sync I/O vs events.** `z_get` (query, sync), `z_subscribe` (event, async callback), `z_put` (publish), `z_pull` (pull-based subscription). A user has to learn four primitives for what is conceptually two operations (read, write).
3. **Zero-copy is gated on the SHM transport.** Inter-host or non-SHM intra-host paths copy. libtracer's same-substrate insight aims for zero-copy on *any* transport that lands a buffer in RAM the receiver controls.
4. **Keyspace addressing, not graph paths.** Zenoh uses URI-like strings (`robot/sensor/temp`) with wildcard matching. Hierarchical, but flat conceptually — there is no "subscribe to this subgraph and inspect its structure". A graph model lets you walk and introspect.
5. **Discovery requires the routing protocol.** Zenoh's scouting is wedded to its routing. You can't pick "I want mDNS but not Zenoh routing." libtracer makes discovery a separate module so you can swap implementations.
6. **TLS-only security on TCP transport.** Good for IP networks; useless for CAN/I2C/UART, where the answer is pre-shared key. Zenoh has no PSK story for non-IP buses. libtracer treats security as per-transport modules.

**None of these are killing flaws for Zenoh** — they are deliberate engineering trade-offs that fit Zenoh's sweet spot (LAN/WAN robotics middleware). They open a niche for libtracer at the smaller, more-composable end.

---

## The gap libtracer fills

Three specific things nobody else does cleanly today:

### 1. One address space across CAN + I2C + IP + RDMA

Every existing middleware draws a hard line at the IP boundary. Below the line you write a driver, above the line you publish messages. The bridge is glue code per-project.

libtracer makes the bridge a **first-class core mechanism**: a vertex that re-publishes incoming TLVs from one transport onto the local graph. A subscriber reads `/robot/wheel-encoder/left` and does not know whether the data arrived via CAN, was bridged through a Linux router over IP, then landed in shared memory on the consumer host. The transport choice is a configuration concern, not an API concern.

This is a real competitive advantage if it works. It does not exist in DDS, Zenoh, or eCAL — they all assume IP or SHM. It does not exist in the bus protocols (CANopen, Modbus, etc.) — they are bus-only. This is the gap.

### 2. Modular by construction (RC-car build = robot-fleet build = same library)

The smallest useful libtracer build (one transport, no discovery, no executor, no security) is the core plus one module. The largest (every transport, scripting, security, GUI) is the core plus dozens of modules. They are the same library; they share an ABI; a vertex on the small build talks to a vertex on the large build.

This requires the core to be *truly* small (≤ 16 KB target, doc 02 week 4 sentinel test) and the module ABI to be stable from day one. If the core sprawls or modules require their own ABIs, the differentiator collapses.

### 3. Same TLV substrate for memory, wire, and graph node

Most middleware has three distinct data representations: the in-memory message struct, the wire encoding (Protobuf, CDR, etc.), and the routing topology (separate graph metadata). libtracer collapses them: a TLV in memory IS a graph node IS the wire bytes. Mix/split/concat operations rearrange views over buffer chains; bytes do not move. Ownership transfers at endpoint delivery via pointer swap.

This is the load-bearing technical insight. If the implementation can pull it off cleanly, the protocol genuinely has a story nobody else tells.

---

## Five differentiators that justify existing

Synthesizing the above. These are what doc 01 must defend in matrix form, and what doc 02 must produce evidence for, milestone by milestone.

| # | Differentiator | Evidence required by week 8 |
| ---- | ---- | ---- |
| 1 | **Modular core ≤ 16 KB** on Cortex-M3+ | `arm-none-eabi-gcc -std=c23 -Os` produces ≤ 16 KB binary with core only, no modules linked |
| 2 | **Same TLV substrate for memory + wire + graph** | Round-trip test: receive TLV → use as in-memory graph node by pointer-swap → re-publish without re-serialization |
| 3 | **Unified read/write API** (no `connect`/`disconnect`) | All week-1 through week-7 demos use only `tracer_read` and `tracer_write`. Subscribe = write to `ep:subscribers[self]`. |
| 4 | **Address-shift slicing replaces wire-level fragmentation** | A 1 MB payload from host A is delivered as `ep[0..N]` to host B; transport (TCP, UDP, CAN) is unaware of "fragmentation". |
| 5 | **Bridges in core** — one address space across heterogeneous transports | Week 6 demo: CAN sensor on STM32 → bridged via Linux router → consumed by ESP32 over Wi-Fi, all reading the same path |

If any of these five fail to materialize by week 8, the worth-it case is broken and the project should be re-scoped or folded.

---

## Risk register

| # | Risk | Likelihood | Impact | Mitigation |
| ---- | ---- | ---- | ---- | ---- |
| R1 | **Scope creep** — the user's vision is expansive; week-by-week the suite balloons | High | Kills the modular-core differentiator | Doc 02 freezes the 8-week scope; anything new is post-MVP |
| R2 | **Bus-factor of one** — single maintainer, ambitious surface | High | Project stalls before MVP; consumers can't depend on it | Pick a v0.1 boundary that one person can maintain (the lean 8-doc set is the artifact of this) |
| R3 | **"Modular" → "no configuration is well-tested"** — N modules ⇒ 2^N build matrices | Medium | Subtle bugs only appear in some module combinations | Define a small set of "blessed builds" (RC-car, robot-fleet, headless-Linux) that CI runs on every commit |
| R4 | **Performance claims unverified** — "as fast as protobuf" framing is wrong-shaped (see doc 01 wrong-benchmarks) | Medium | Marketing claim collapses on first benchmark | Doc 02 week 8 publishes honest numbers with a defined harness; doc 01 specifies what to compare against |
| R5 | **XOR-16 → CRC-32C upgrade is a wire break** | Certain | v0.0 (current code) → v0.1 incompatible | Fine — v0.0 has no users; bump version byte and don't carry the cruft |
| R6 | **Cortex-M0 atomic-instruction absence** — the buffer-chain refcount needs LDREX/STREX | Certain | M0 cannot use the SMP-safe core mode | Two build modes documented in doc 03: atomic (Cortex-M3+) and `LIBTRACER_NO_ATOMIC` (single-threaded only). M0 minimum support is the no-atomic mode. |
| R7 | **Schema-discovery for field-write control surface** is under-defined | Medium | Modules invent fields, schemas drift, third-party tools break | Doc 04 fixes core field names and module-namespace discipline; `ep:schema` is the discovery point |
| R8 | **Two markets in one product** (RC car vs HPC GPU sampling) — orthogonal constraints | High | Over-generalized API serves neither well | Doc 00 (this doc) declares v0.1 scope = embedded + LAN. HPC documented as future work, may need design revisits |
| R9 | **Iceoryx, eCAL, Zenoh ship faster than libtracer** | Certain | Existing options entrench; libtracer arrives without a unique market | Differentiators 1-5 above are the answer; if they don't materialize, fold |
| R10 | **PTP unavailability on STM32F4** limits coherency story | Certain | Sub-µs sync requires HW PTP (F7+ / ESP32 with HW PTP) | Doc 04 acknowledges; ms-accurate SW PTP is the floor on F4 |

The risk this register is most serious about is **R1 + R2 combined**: an ambitious user with limited bandwidth will scope-creep the project to death. The doc set's job is to force the discipline.

---

## Three honest paths

The user could plausibly take any of these. The plan picks (a). The other two stay on record so that if (a) stalls at week 6, the pivot is named, not improvised.

### (a) Build libtracer fresh — RECOMMENDED

Pursue the five differentiators. Eight-week roadmap to MVP. Re-evaluate at week 6.

- **Pro**: full design control; the same-substrate insight only works in a clean codebase; the modular-core target is incompatible with grafting onto a monolith.
- **Con**: high effort; ecosystem starts from zero; competing against Zenoh's mindshare.

### (b) Fork or extend Zenoh

Take `zenoh-pico` and add the gap features as PRs upstream or in a fork.

- **Pro**: ecosystem and ROS bridge for free; user community exists; battle-tested.
- **Con**: the modular-core target is incompatible with Zenoh's monolithic build; the same-substrate insight requires breaking changes Zenoh maintainers would never accept; the "unified read/write API" needs ripping out four primitives. In practice this becomes a hard fork that diverges immediately and inherits Zenoh's complexity.

### (c) Build only the gap features atop NNG/Zenoh as transport

Treat Zenoh (or NNG) as a generic message bus and build only the graph + addressing + bridge layer above it.

- **Pro**: leverages existing transports; small surface; ships fastest.
- **Con**: cannot deliver differentiator #1 (≤ 16 KB core — Zenoh and NNG are larger than the target by themselves); cannot deliver #4 (address-shift slicing requires control of the wire format); reduces the project to a thin shim of questionable value.

---

## Verdict

**Build it.** Path (a). On the reduced scope this doc and doc 02 specify.

The five differentiators are credible-but-unproven. By week 8 the implementation either backs them up or it doesn't. If it does, libtracer occupies a real niche between Zenoh and Iceoryx2 — small enough for the bus-protocol world, structured enough for the network-protocol world, with a substrate insight nobody else has. If it doesn't, the doc trail makes it cheap to fold, and the user has lost weeks not months.

**Do not market this as a Zenoh replacement.** It isn't, and the framing invites comparisons libtracer will lose. Market it as **the smallest protocol that turns CAN, I2C, IP, and SHM into one address space, with an API smaller than DDS by an order of magnitude.**

**Re-evaluate at end of week 6** (CAN bridge milestone). That demo either proves the cross-bus story or it doesn't. Everything downstream depends on it.

---

## What's NOT in this doc

- Wire format details — see [doc 03](03-wire-format-and-data-model.md).
- API shape — see [doc 04](04-graph-and-endpoint-api.md).
- Week-by-week implementation plan — see [doc 02](02-roadmap-weeks-1-to-8.md).
- Comparison matrix — see [doc 01](01-comparison-to-existing-protocols.md).
- Module catalog — see [doc 05](05-modules-transport-and-discovery.md), [doc 06](06-modules-executor-security-gui.md).
- Glossary — see [doc 99](99-glossary.md).
- "Coherency for distributed control via PTP" — one paragraph in doc 04; the deep version is post-MVP and not promised.
