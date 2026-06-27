# libtracer

**A decentralized, zero-copy, graph-based pub/sub protocol that supersedes DDS — one wire format from a 16 KB Cortex-M0 to a GPU, across vendors and transports.**

A libtracer node is a graph of addressable **vertices**. The load-bearing idea: **the same TLV bytes are the wire encoding, the in-memory value, *and* the graph node** — so publishing moves **zero bytes** (a refcount bump), and any node can route, translate, or relay any other node's state without understanding it. Big things connect to small things; small things connect to each other through whatever transport is available (UART, CAN, BLE, Wi-Fi, WebSocket, UDP, LoRa) or through any node acting as a bridge to something incompatible.

This repo is not just a library — it **defines a protocol the community can implement, extend, and interoperate around**, including across competing products and proprietary ecosystems.

---

## Why libtracer — the keystones

- **Graph unification (one substrate).** Wire bytes ≡ in-memory value ≡ graph node. There is no encode-on-send / decode-on-receive / copy-into-the-graph tax: an in-process hand-off is a refcount bump, and a network hop ships the bytes that *already are* the value. Decoupling *storage* composition (memory ropes) from *meaning* composition (TLV trees) is what makes this zero-copy hold end-to-end.
- **Zero-copy everywhere.** Values are refcounted **views** over backing memory; a logically contiguous value scattered across segments is a **rope** (a chain of views, never `memcpy`). Fan-out to N subscribers is N refcount bumps, not N copies. Egress lowers a rope straight to the transport's scatter-gather (`iovec`/`sendmsg`, CAN descriptor chains, RDMA verbs) — one syscall per composite, no flatten. The borrowed/loaned read path is **flat ~80 ns even at 8 KB**.
- **Stateless, decentralized graph routing.** The bridge/`ROUTER` is **stateless and uniform across framing modes** — it forwards typed bytes without interpreting them. **Any node with ≥2 transports forwards**; there are **no `router`/`orchestrator` roles**, only transient hats a peer wears. The network **folds arbitrarily** (leaf → backbone → another fold), is **pure-decentralized and self-healing** (bindings re-form on reconnect, no central authority), and loops terminate by `hop_count`/`MAX_HOPS` rather than shared state — MCU-friendly.
- **Three calls + an `ioctl`.** The entire data API is **`read` / `write` / `await`** (plus refcounts) — no `connect`/`subscribe`/`disconnect`. Control (subscriptions, QoS, ACLs, liveness) is **field-writes on a `:` plane** — the vertex's `ioctl`, on one identity (`/sensor/temp:subscribers[]`). Subscribing *is* a consumer-initiated client-write into the producer's subscriber list; delivery *is* an ordinary write.
- **Zero-overhead on constrained buses.** **Header-elided framing** lets the transport's native identity (a CAN ID, a WS channel) *be* the path — the TLV header is synthesized on ingress and elided on egress, so it **never hits the bus**; existing CAN/WS frames are byte-unchanged. The *same* **advertise + id-match** mechanism scales from a 9-byte elided CAN sample to a multi-GB advertised rope group.
- **One model across 8 orders of magnitude.** A 1-bit boolean, a GPIO MMIO register, an IMU record, a 1 GB/s ADC stream, and a tensor in GPU memory are all the *same* vertex model — addressed identically, lighting up different optional `:` fields. A Cortex-M0 is a **first-class node** (~16 KB, static path handles, no `snprintf`/malloc on the hot path, ISR-safe). Memory backends (heap, pool, MMIO, DMA, CUDA) are pluggable; libtracer is a **transparent byte router** that imposes no copy/CRC on a live binding.

## Performance — faster *and* lower-latency than zenoh

Benchmarked against [Eclipse Zenoh](https://zenoh.io) across a payload × fan-out × topic-count grid (both at `-O3`). Bottom line: **libtracer beats zenoh-c on both throughput and latency on every path.** (`rmw_tracer`/`rmw_zenoh` are thin wrappers over these engines, so the engine delta is the dominant ROS-level term.)

| Path | libtracer vs zenoh-c |
| --- | --- |
| In-process, fan-out 1 → 8192 | **2.5× → 6.4× throughput**, **2.6× → 6.6× lower p50** |
| In-process, payload 1 B → 8 KB | **2.2× → 2.6×** throughput/latency |
| Borrowed (loaned) read path | flat **~80 ns** even at 8 KB |
| Network latency (localhost UDP) | **~14 µs vs ~64 µs p50** (≈4.5× lower) — one datagram per message, no batching timer |
| Network throughput (scatter-gather) | up to **~46.6 M values/s** via one `sendmsg` per composite, vs zenoh-c ~3.5 M @ 62 µs |

The throughput win is **structural, not a timer**: zenoh raises throughput with a batching (Nagle-style) timer — which is why its latency is worse. libtracer batches by **composition** — a composite endpoint's value is a rope already batched in memory, shipped in one syscall — so throughput scales with composition size *at flat latency*. Full harness, figures, and honest caveats: [`docs/performance.md`](docs/performance.md), [`bench/RESULTS.md`](bench/RESULTS.md).

## How it supersedes existing solutions

- **vs DDS / RTPS.** DDS is powerful but heavy — it does not fit a 16 KB MCU and copies samples on fan-out. libtracer fits the MCU, does **zero-copy refcounted fan-out**, and needs no heavyweight discovery to talk on a bus.
- **vs Zenoh.** Comparable design goals, but libtracer wins **both** throughput and latency (above), reaches **micro-ROS-class targets Zenoh cannot**, and carries ROS 2 messages in GPU memory. (Honest niche: Zenoh's ROS integration is mature and official; libtracer's edge is embedded / ultra-low-latency / zero-copy / on-robot fabric.)
- **vs MQTT.** No central broker required — the network is decentralized and **any node can bridge**. State is a **typed, addressable graph**, not opaque topic payloads, and delivery is zero-copy.
- **vs a custom CAN/UART glue layer.** Header elision means libtracer rides your existing bus frames **with zero added bytes**, while still giving you addressing, fan-out, QoS, and a bridge to IP — instead of a bespoke, un-routable point-to-point hack.

## How you'd use it — simplest to most complex

The same protocol scales across a deployment spectrum (modules layer on a common base; see [`docs/reference/12-deployment-profiles.md`](docs/reference/12-deployment-profiles.md)):

| | Scenario | What it adds |
| --- | --- | --- |
| **0** | In-process pub/sub | graph runtime + codec only |
| **1** | Single-transport leaf | one transport (UART/CAN) — an RC car, a CAN sensor |
| **2** | **Gateway** (CAN + WebSocket) | a 2nd transport + the stateless bridge — e.g. a grow controller fanning a CAN sensor field to a web UI |
| **3** | RTSP / camera source | lazy on-demand streams as rope groups |
| **4** | ROS 2 node (`rmw_tracer`) | drop-in RMW: `RMW_IMPLEMENTATION=rmw_tracer`, no node code changes |
| **5** | Flagship | 100 ksps STM32 → CAN → **GPU tensor cores**, zero host copy |

A "smart device" here is **any node that translates an incompatible protocol (Modbus, Z-Wave, vendor-X) into libtracer** — making the legacy device a first-class citizen of the graph.

```text
  ┌──────────┐  CAN  ┌──────────┐  WS / UDP ┌──────────┐  HTTP ┌──────────┐
  │  sensor  │──────▶│  ESP32   │──────────▶│ gateway  │──────▶│  cloud   │
  │  (tiny)  │       │  bridge  │           │          │       │ (web UI) │
  └──────────┘       └────┬─────┘           └────▲─────┘       └──────────┘
                          │ LoRa                 │ Modbus / proprietary
                          ▼                      │
                     ┌──────────┐          ┌──────────┐
                     │ off-grid │          │ legacy   │
                     │  sensor  │          │   PLC    │
                     └──────────┘          └──────────┘
```

Every arrow speaks one protocol; the bridge forwards typed state across transports without understanding the payload.

## Boundary cases & honest limits

- **Fold depth is bounded by `MAX_HOPS`** (default 32): a delivery path longer than that is cut. Fine for ordinary topologies; the limit is pathologically deep gateway nesting.
- **Dense meshes cost duplicate deliveries.** Cycle dedup is best-effort (a bounded recent-set); a loop dies by `MAX_HOPS` regardless, after at most ≈ `MAX_HOPS × fanout` duplicates.
- **No global clock or cross-producer ordering.** Timestamps are per-producer monotonic (HLC-style); `(origin_peer_id, ts)` is a collision-free in-flight identity, but cross-node coherence needs a coordinated trigger.
- **u32 length ceiling (no u64).** Payloads above 4 GiB use **address-shift slicing** across `ep[0..N]` — the deliberate interop discipline, not a wire-level fragmentation layer.
- **Reliable stream transport is in progress.** localhost UDP is best-effort; a reliable byte-stream (TCP/QUIC) for `RELIABLE` QoS / WAN is the remaining transport work.
- **v1 does authorization, not authentication.** ACLs gate operations on a pluggable subject-token (the transport-authenticated `origin_peer_id` in v1); PKI is a deferred module — the ACL model is unchanged when it lands.
- **Pre-1.0.** The wire format is being drafted and is not yet frozen; pin to a commit if you depend on this today.

---

## What libtracer gives you

- **A wire-format specification** ([docs/spec/](docs/spec/)) — versioned, normative, conformance-tested. Implement it in any language and you are libtracer-compatible.
- **A C++ reference implementation** ([core/](core/)) — header-first, no-RTTI, no-exceptions; ESP32 / STM32 / bare-metal capable. Apache 2.0.
- **Native language cores, kept in lock-step by shared conformance vectors** (not FFI bindings) — [Rust](bindings/rust/) and TypeScript ([`@avatarsd-llc/libtracer`](bindings/typescript/), a pure-TS client SDK). Each ships a conformance harness and is CI-gated against the same vectors, so the cores can't drift.
- **Platform integrations** — [PlatformIO](integrations/platformio/), [ESPHome](integrations/esphome/), [Arduino](integrations/arduino/).
- **Conformance test vectors** ([tests/conformance/](tests/conformance/)) — the same vectors every implementation runs. Pass them and you interoperate (`tests/conformance/run-all.py`).

## Open community + proprietary products — three layers

A deliberate separation lets vendors build proprietary products on top without fragmenting the ecosystem, and lets the community contribute without fearing capture.

1. **The Protocol** (open, normative, [docs/spec/](docs/spec/)). RFC-2119 keywords + conformance vectors. *An implementation is compatible if it passes the vectors for a spec version and honors all MUST clauses* — no source dependency. Changes governed by [GOVERNANCE.md](GOVERNANCE.md).
2. **The Reference Implementation** (Apache 2.0). Ship it in proprietary firmware without copyleft. Independent re-implementations are encouraged — register in [implementations/](implementations/).
3. **Proprietary products & services** (yours to build) — cloud, fleet management, hosted bridges, hardware. Compete here while interoperating at Layer 1. **"libtracer" is a trademark** ([TRADEMARKS.md](TRADEMARKS.md)): say "compatible with libtracer" if you pass the suite; don't imply endorsement otherwise.

In practice: ship a closed-source product → use the reference impl + follow the trademark policy. Build a competing core → pass the vectors, register, you're peers. Change the protocol → open an RFC under [docs/spec/rfcs/](docs/spec/). Bridge Modbus / Z-Wave / vendor-X → that's exactly the point.

## Repository layout

```text
libtracer/
├── core/                  Reference C/C++ implementation (Apache 2.0)
│   ├── include/libtracer/ Public headers — #include <libtracer/...>
│   ├── src/  tests/        Implementation + unit/conformance tests
│   └── CMakeLists.txt
├── bindings/
│   ├── rust/              Native Rust core → crates.io
│   └── typescript/        Native pure-TS core → npm (@avatarsd-llc/libtracer)
├── implementations/        Registry of third-party implementations
├── integrations/           platformio/ · esphome/ · arduino/
├── examples/               Runnable examples per platform
├── bench/                  libtracer↔zenoh benchmark + figures
├── docs/
│   ├── spec/              Normative protocol specification + RFCs
│   ├── reference/         Descriptive six-layer architecture (the "what it is")
│   └── adr/               Architecture decision records (the "why")
├── tests/conformance/      Cross-implementation vectors + run-all.py driver
├── GOVERNANCE.md  CONTRIBUTING.md  TRADEMARKS.md  CONTEXT.md (glossary)
└── LICENSE                 Apache 2.0
```

## Getting started

| If you want to… | Look here |
|---|---|
| Understand the architecture | [docs/reference/00-overview.md](docs/reference/00-overview.md) · [CONTEXT.md](CONTEXT.md) (glossary) |
| Read the protocol | [docs/spec/v1.md](docs/spec/) |
| See the numbers | [docs/performance.md](docs/performance.md) · [bench/](bench/) |
| Use it on ESP32 / PlatformIO / ESPHome | [integrations/](integrations/) |
| Use it from Rust / Node / browser | [bindings/](bindings/) |
| Build a bridge or form a network | [docs/reference/13-network-formation.md](docs/reference/13-network-formation.md) |
| Contribute / propose a spec change | [CONTRIBUTING.md](CONTRIBUTING.md) · [docs/spec/rfcs/](docs/spec/) |

## License

| Scope | License | File |
|---|---|---|
| Reference implementation (`core/`, `bindings/`, `integrations/`) | **Apache 2.0** | [LICENSE](LICENSE) |
| Protocol specification (`docs/spec/`) | **CC BY 4.0** | [docs/spec/LICENSE](docs/spec/LICENSE) |
| Example code (`examples/`) | **CC0 1.0** | [examples/LICENSE](examples/LICENSE) |

Copyright **avatarsd LLC** for company-authored work; outside contributions remain their authors', licensed per the scope above. Contributions are accepted under the [Developer Certificate of Origin](https://developercertificate.org/) — sign commits with `git commit -s`. The **"libtracer" name** is a trademark of avatarsd LLC, not granted by the licenses above ([TRADEMARKS.md](TRADEMARKS.md)).
