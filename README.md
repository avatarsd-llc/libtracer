# libtracer

**A lightweight, decentralized graph-based networking protocol — and a reference implementation — for making sensors, microcontrollers, gateways, and applications talk to each other across vendors, transports, and protocols.**

libtracer is built on a simple idea: every node in your system, from a battery-powered sensor to a cloud service, exposes its state as a small, typed, addressable graph. Any node can route, translate, or relay any other node's state. Big things connect to small things. Small things connect to other small things through whatever transport happens to be available — UART, BLE, Wi-Fi, LoRa, or a smart device acting as a bridge to something incompatible.

The goal of this repository is not just to ship a library. It is to **define a protocol that the community can implement, extend, and interoperate around** — including across competing products and proprietary ecosystems.

---

## What libtracer gives you

- **A wire-format specification** ([docs/spec/](docs/spec/)) — versioned, normative, and conformance-tested. Implement it in any language and you are libtracer-compatible.
- **A reference implementation in C/C++** ([core/](core/)) — header-first, no-RTTI, no-exceptions, suitable for ESP32, STM32, and bare-metal targets. Apache 2.0 licensed.
- **Language bindings** — [Rust](bindings/rust/) (crates.io) and [TypeScript](bindings/typescript/) (npm), thin wrappers over the C core, suitable for gateways, CLI tools, and web dashboards.
- **Platform integrations** — [PlatformIO](integrations/platformio/), [ESPHome external component](integrations/esphome/), and [Arduino](integrations/arduino/) packaging so you can drop libtracer into your firmware project with one config line.
- **Conformance test vectors** ([tests/conformance/](tests/conformance/)) — the same vectors every implementation runs. Pass them and you interoperate.

## Design intent: connect everything to everything

libtracer is intentionally protocol- and transport-agnostic. A typical deployment looks like this:

```text
  ┌──────────┐  BLE  ┌──────────┐  Wi-Fi  ┌──────────┐  HTTP  ┌──────────┐
  │  sensor  │──────▶│  ESP32   │────────▶│ gateway  │───────▶│  cloud   │
  │  (tiny)  │       │  bridge  │         │  (Rust)  │        │  (TS)    │
  └──────────┘       └──────────┘         └──────────┘        └──────────┘
                          │                     ▲
                          │  LoRa               │  Modbus / proprietary
                          ▼                     │
                     ┌──────────┐         ┌──────────┐
                     │ off-grid │         │ legacy   │
                     │  sensor  │         │   PLC    │
                     └──────────┘         └──────────┘
```

Every arrow speaks the same protocol. The bridge in the middle does not need to understand what the sensor measures or what the cloud will do with it — it just forwards typed state across transports. A "smart device" in this picture is **any node that translates an incompatible protocol into libtracer**, making the legacy device a first-class citizen of the graph.

This is the problem libtracer is designed to solve: **community-driven interoperation between devices that were never intended to talk to each other.**

---

## How this project balances open community and proprietary products

libtracer is built around a deliberate three-layer separation. This lets vendors build proprietary products on top without fragmenting the ecosystem, and lets the community contribute without fearing capture.

### Layer 1 — The Protocol (open, normative)

The wire format, framing, identifiers, and conformance rules live in [docs/spec/](docs/spec/). The spec is versioned (v1, v2, …), uses RFC-2119 keywords (MUST / SHOULD / MAY), and is accompanied by conformance test vectors that any implementation can run.

**An implementation is libtracer-compatible if it passes the conformance vectors for a given spec version and honors all MUST clauses.** That is the entire compatibility contract. No source-code dependency required.

Spec changes are governed by [GOVERNANCE.md](GOVERNANCE.md).

### Layer 2 — The Reference Implementation (open source, Apache 2.0)

This repository ships a permissively-licensed reference implementation in C/C++, with Rust and TypeScript bindings. Apache 2.0 means **vendors can ship libtracer in proprietary firmware without copyleft obligations.** Use it, fork it, embed it, sell products that contain it.

Independent re-implementations are welcome and encouraged. A second, independently-developed implementation is the strongest possible validation that the spec is unambiguous. See [implementations/](implementations/) for the registry of known implementations.

### Layer 3 — Proprietary Products and Services (yours to build)

Anything built **on top of** the protocol — cloud services, fleet management, certified-compatibility programs, hosted bridges to closed protocols, premium support, hardware — is not part of this repository and does not need to be open. Vendors can compete on these layers while still interoperating at Layer 1.

**The "libtracer" name is a trademark.** See [TRADEMARKS.md](TRADEMARKS.md) for usage guidelines: in short, you may freely say "compatible with libtracer" if your implementation passes the conformance suite, and you may not use the name to imply endorsement of a product that is not.

### What this means in practice

- You want to ship a closed-source IoT product? **Use the reference impl, follow the trademark policy, you're done.**
- You want to build a competing implementation in a different language? **Pass the conformance vectors, register in [implementations/](implementations/), you're peers with the reference impl.**
- You want to contribute to the protocol itself? **Open an RFC under [docs/spec/rfcs/](docs/spec/), follow [GOVERNANCE.md](GOVERNANCE.md).**
- You want to build a smart device that bridges Modbus / Z-Wave / proprietary-vendor-X into libtracer? **That is exactly the use case this protocol exists for.** Contribute the bridge to [integrations/](integrations/) or ship it independently — both are first-class.

---

## Repository layout

```text
libtracer/
├── core/                          Reference C/C++ implementation (Apache 2.0)
│   ├── include/libtracer/         Public headers — #include <libtracer/...>
│   ├── src/                       Implementation (.cpp)
│   ├── tests/                     Unit tests
│   └── CMakeLists.txt
├── bindings/
│   ├── rust/                      → crates.io ("libtracer")
│   └── typescript/                → npm ("libtracer")
├── implementations/               Registry of third-party implementations
├── integrations/
│   ├── platformio/                → registry.platformio.org
│   ├── esphome/                   external_components/ (git-sourced)
│   └── arduino/                   library.properties
├── examples/                      Runnable examples per platform
├── docs/
│   ├── spec/                      Normative protocol specification
│   └── reference/                 Architecture, design notes, ADRs
├── tests/conformance/             Cross-implementation test vectors
├── GOVERNANCE.md                  Decision-making process
├── CONTRIBUTING.md                How to contribute
├── TRADEMARKS.md                  "libtracer" name and badge usage
└── LICENSE                        Apache 2.0
```

## Getting started

| If you want to...                   | Look here                                            |
|-------------------------------------|------------------------------------------------------|
| Read the protocol                   | [docs/spec/](docs/spec/)                             |
| Use it on ESP32 / PlatformIO        | [integrations/platformio/](integrations/platformio/) |
| Use it in ESPHome                   | [integrations/esphome/](integrations/esphome/)       |
| Use it from Rust                    | [bindings/rust/](bindings/rust/)                     |
| Use it from Node / browser          | [bindings/typescript/](bindings/typescript/)         |
| Build a bridge to a closed protocol | [docs/reference/](docs/reference/)                   |
| Contribute                          | [CONTRIBUTING.md](CONTRIBUTING.md)                   |
| Propose a spec change               | [docs/spec/](docs/spec/)                             |

## Status

Pre-1.0. The spec is being drafted; the wire format is not yet stable. Pin to a specific commit if you depend on this today.

## License

The libtracer project uses three licenses, matched to what each part is:

| Scope                                                            | License                     | File                                   |
|------------------------------------------------------------------|-----------------------------|----------------------------------------|
| Reference implementation (`core/`, `bindings/`, `integrations/`) | **Apache License 2.0**      | [LICENSE](LICENSE)                     |
| Protocol specification (`docs/spec/`)                            | **CC BY 4.0**               | [docs/spec/LICENSE](docs/spec/LICENSE) |
| Example code (`examples/`)                                       | **CC0 1.0 (public domain)** | [examples/LICENSE](examples/LICENSE)   |

Copyright is held by **Avatar LLC** for company-authored work; outside contributions remain copyright of their authors, licensed in accordance with the scope above. See [NOTICE](NOTICE) and [GOVERNANCE.md](GOVERNANCE.md#stewardship) for stewardship details.

Contributions are accepted under the [Developer Certificate of Origin](https://developercertificate.org/) — sign your commits with `git commit -s`.

The **"libtracer" name** is a trademark of Avatar LLC and is not granted by the licenses above. See [TRADEMARKS.md](TRADEMARKS.md).
