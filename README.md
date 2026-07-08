<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC -->

# libtracer

**A decentralized, zero-copy, graph-based pub/sub protocol — one wire format from a 16 KB Cortex‑M0 to a GPU, across vendors and transports.**

[![conformance](https://github.com/avatarsd-llc/libtracer/actions/workflows/conformance.yml/badge.svg)](https://github.com/avatarsd-llc/libtracer/actions/workflows/conformance.yml)
[![core CI](https://github.com/avatarsd-llc/libtracer/actions/workflows/core-ci.yml/badge.svg)](https://github.com/avatarsd-llc/libtracer/actions/workflows/core-ci.yml)
[![docs](https://img.shields.io/badge/docs-libtracer.avatarsd.com-2ea043)](https://libtracer.avatarsd.com/)
[![spec: protocol v1 (draft)](https://img.shields.io/badge/spec-protocol%20v1%20(draft)-blue)](https://libtracer.avatarsd.com/docs/spec/v1.html)
[![license: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)
[![stars](https://img.shields.io/github/stars/avatarsd-llc/libtracer?style=flat)](https://github.com/avatarsd-llc/libtracer/stargazers)

A libtracer node is a graph of addressable **vertices**. The load‑bearing idea: **the same TLV bytes are the wire encoding, the in‑memory value, *and* the graph node** — so publishing moves **zero bytes** (a refcount bump), and any node can route, translate, or relay another node's state without understanding it. Big things connect to small things; small things connect to each other over whatever transport is available (UART, CAN, BLE, Wi‑Fi, WebSocket, UDP, QUIC) or through any node bridging to something incompatible.

This repo is not just a library — it **defines a protocol the community can implement, extend, and interoperate around**, including across competing products and proprietary ecosystems.

> **📖 Full documentation:** **[libtracer.avatarsd.com](https://libtracer.avatarsd.com/)** — architecture, the normative spec, reproducible benchmarks, worked examples, and the [implementation capability matrix](https://libtracer.avatarsd.com/docs/capability-matrix.html).

---

## Why libtracer — the keystones

- **One substrate.** Wire bytes ≡ in‑memory value ≡ graph node. No encode‑on‑send / decode‑on‑receive / copy‑into‑the‑graph tax: an in‑process hand‑off is a refcount bump, and a network hop ships the bytes that *already are* the value.
- **Zero‑copy everywhere.** Values are refcounted **views** over backing memory; a logically contiguous value scattered across segments is a **rope** (a chain of views, never `memcpy`). Fan‑out to N subscribers is N refcount bumps. Egress lowers a rope straight to the transport's scatter‑gather — one syscall per composite, no flatten. The borrowed read path is **flat ~80 ns even at 8 KB**.
- **Stateless, decentralized routing.** Explicit source‑routing: a remote endpoint is addressed by its full path through transport‑vertices, and each forwarding node **strips its own segment and passes the rest** — stateless, loop‑free by construction, no dedup, no shared state. **Any node with ≥2 transports forwards**; there are **no privileged `router`/`orchestrator` roles**. The mesh is pure‑decentralized and self‑healing.
- **Three calls + an `ioctl`.** The entire data API is **`read` / `write` / `await`** — no `connect`/`subscribe`/`disconnect`. Control (subscriptions, QoS, ACLs, liveness) is **field‑writes on a `:` plane** — the vertex's `ioctl`. Subscribing *is* a consumer‑initiated write into the producer's subscriber list; delivery *is* an ordinary write.
- **Zero overhead on constrained buses.** **Header‑elided framing** lets a transport's native identity (a CAN ID, a WS channel) *be* the path — the TLV header is synthesized on ingress and elided on egress, so it never hits the bus; existing CAN/WS frames are byte‑unchanged.
- **One model across 8 orders of magnitude.** A 1‑bit boolean, a GPIO register, an IMU record, a 1 GB/s ADC stream, and a GPU tensor are the *same* vertex model — addressed identically, lighting up different optional `:` fields. A Cortex‑M0 is a first‑class node (~16 KB, no malloc/`snprintf` on the hot path, ISR‑safe).

## Implementations — what's real today

libtracer is a **spec first**, then implementations. The C++ core is the golden reference; the Rust and TypeScript cores are *from‑scratch native reimplementations* kept byte‑identical to it by the same [shared conformance vectors](tests/conformance/) ([ADR‑0028](docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md)). Every claim below is backed by a CI‑run test — see the **[capability matrix](https://libtracer.avatarsd.com/docs/capability-matrix.html)** (generated from that evidence, so it can't drift).

| Implementation | Scope | Status |
| --- | --- | --- |
| **C++ core** ([`core/`](core/)) | the **full** protocol — codec, graph runtime, FWD routing, transports (tcp/udp/ws/quic/webtransport/can) | reference; extensive test suite |
| **Rust** ([`bindings/rust/`](bindings/rust/)) | native `#![no_std]` **wire codec + typed tier** (builders, PATH, ERROR registry, FWD/FIELD) — no transports/runtime yet | byte‑verified; pre‑release |
| **TypeScript** ([`bindings/typescript/`](bindings/typescript/)) | native browser/**edge** — codec + client (read/write/await/subscribe) + WebSocket/WebTransport | byte‑verified; client/transports experimental |
| **ESP‑IDF** ([`integrations/esp-idf/`](integrations/esp-idf/)) | packages the full C++ node as a managed component (CI‑built esp32c6/c3 + linux) | working |
| **PlatformIO** ([`integrations/platformio/`](integrations/platformio/)) | packages + compiles the portable core (codec/graph/tcp/udp/ws/can) | working; esp32 CAN via a best‑effort build hook (board‑unverified) |
| **ESPHome · Arduino** ([`integrations/`](integrations/)) | platform packaging | ESPHome is a placeholder stub · Arduino **not planned** |
| **ROS 2** (`rmw_tracer`, [`bindings/ros2/`](bindings/ros2/)) | drop‑in RMW over the C++ graph | **early stub** |

## Quick start

**C++ (the reference):**
```bash
cmake -S core -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```
Then `find_package(libtracer)` + `target_link_libraries(app PRIVATE libtracer::libtracer)`. See the **[getting‑started guide](https://libtracer.avatarsd.com/docs/getting-started.html)**.

**Other surfaces** (available once `v0.3.0` is published — the pipeline publishes them in lockstep):

```bash
npm install @avatarsd-llc/libtracer          # TypeScript codec
cargo add libtracer                          # Rust codec (git dep until first crate release)
# ESP-IDF:  idf.py add-dependency "avatarsd-llc/libtracer"
```

## How it compares

- **vs DDS / RTPS** — DDS doesn't fit a 16 KB MCU and copies samples on fan‑out; libtracer fits the MCU and does zero‑copy refcounted fan‑out with no heavyweight discovery.
- **vs Zenoh** — comparable goals; libtracer is benchmarked head‑to‑head on the **[performance page](https://libtracer.avatarsd.com/docs/performance.html)** (absolute numbers, generated in CI, no cherry‑picked ratios) and reaches embedded targets Zenoh can't. *(Honest niche: Zenoh's ROS integration is mature; libtracer's edge is embedded / ultra‑low‑latency / zero‑copy / on‑robot fabric.)*
- **vs MQTT** — no central broker; state is a typed, addressable graph, not opaque topic payloads; delivery is zero‑copy.
- **vs a bespoke CAN/UART glue layer** — header elision rides your existing bus frames with **zero added bytes**, while still giving you addressing, fan‑out, QoS, and a bridge to IP.

## Boundary cases & honest limits

- **Pre‑1.0.** The wire format is **DRAFT** and not yet frozen — pin to a commit/tag if you depend on it today.
- **Route length is bounded by the frame size** — a remote op carries its explicit `dst` path in the frame.
- **No auto‑multipath / mesh failover.** Routing is explicit source‑routing; a peer reachable two ways is two explicit addresses (deliberate redundancy, not automatic dedup).
- **No global clock or cross‑producer ordering** — timestamps are per‑producer monotonic; cross‑node coherence needs a coordinated trigger.
- **v1 does authorization, not authentication.** ACLs gate operations on a pluggable subject‑token (the transport‑authenticated `origin_peer_id` in v1); PKI/key management is a deferred module.

## Open community + proprietary products

A deliberate separation lets vendors build proprietary products on top without fragmenting the ecosystem, and lets the community contribute without fearing capture:

1. **The Protocol** (open, normative, [`docs/spec/`](docs/spec/)) — RFC‑2119 + conformance vectors. *An implementation is compatible if it passes the vectors for a spec version.* Changes go through the [RFC process](.github/GOVERNANCE.md).
2. **The Reference Implementation** (Apache 2.0) — ship it in proprietary firmware without copyleft. Independent re‑implementations are welcome — register in [`docs/implementations.md`](docs/implementations.md).
3. **Proprietary products & services** — cloud, fleet management, hosted bridges, hardware. Compete there while interoperating at Layer 1. **"libtracer" is a trademark** ([TRADEMARKS.md](.github/TRADEMARKS.md)): say "compatible with libtracer" if you pass the suite.

## Repository layout

```text
libtracer/
├── core/                Reference C/C++ implementation (Apache 2.0) — src, include, tests
├── bindings/            Native cores — rust/ (crates.io), typescript/ (npm @avatarsd-llc/*)
├── integrations/        Platform packaging — esp-idf/ (working) · platformio/ · esphome/ · arduino/
├── examples/            Runnable, CI-built examples
├── bench/               libtracer↔Zenoh benchmark harness (feeds the live perf page)
├── docs/                Spec (normative) · reference (descriptive) · ADRs (rationale)
├── tests/conformance/   Cross-implementation vectors + run-all.py driver
└── CONTEXT.md           Canonical glossary
```

## Contributing

Every change lands via a pull request; commits are DCO‑signed (`git commit -s`). Spec changes go through an [RFC](.github/GOVERNANCE.md). Start with **[CONTRIBUTING.md](.github/CONTRIBUTING.md)** and the [architecture overview](https://libtracer.avatarsd.com/docs/reference/00-overview.html).

## License

| Scope | License |
| --- | --- |
| Reference implementation (`core/`, `bindings/`, `integrations/`) | **Apache 2.0** ([LICENSE](LICENSE)) |
| Protocol specification (`docs/spec/`) | **CC BY 4.0** |
| Example code (`examples/`) | **CC0 1.0** |

Copyright **avatarsd LLC** for company‑authored work; outside contributions remain their authors', licensed per the scope above. The **"libtracer" name** is a trademark of avatarsd LLC ([TRADEMARKS.md](.github/TRADEMARKS.md)).
