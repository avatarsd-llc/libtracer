# Reference 20 — The bindings map

libtracer ships in more than one language and on more than one platform. This page is the canonical answer to *what exists*, *what each thing is for*, *which of them are independent implementations of the protocol and which are wrappers around one implementation*, *how they are held to the same wire format*, and *how a reader picks one*. Other pages and READMEs should link here rather than restate any of it — that is the rule the article set was minted under ([#1382](https://github.com/avatarsd-llc/libtracer/issues/1382)).

The enumeration below is taken from the [`bindings/`](https://github.com/avatarsd-llc/libtracer/tree/main/bindings/) and [`integrations/`](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/) trees as they stand, not from a plan. Per-artifact maturity that is derived from CI evidence lives in the [capability matrix](../capability-matrix.md), which a CI check regenerates and compares; this page carries the *structure* the matrix reports against. Implementations maintained outside this repository are registered in [`docs/implementations.md`](../implementations.md) — that registry is currently empty.

---

## The distinction that organises everything: core, port, adapter

Three kinds of artifact live under those two trees, and the difference is which layers of the [six-layer model](00-overview.md) the artifact *contains* versus *delivers*.

| Kind | Definition | Consequence |
| --- | --- | --- |
| **Native core** | A from-scratch implementation of the wire format in its own language. No FFI, no WASM-of-C, no shared binary. | It can disagree with the other cores, so it is gated against the shared conformance vectors. It is an implementation of the *protocol*. |
| **Port** | A packaging of the C++ reference implementation for a platform's build system, plus whatever platform-native code that platform's transports need. | It cannot disagree about the wire format — it *is* the C++ codec. Its risk surface is build configuration and platform I/O, not encoding. |
| **Adapter** | A translation of a foreign API or protocol onto the C++ graph runtime. | Same wire risk as a port. Its risk surface is the concept mapping. |

Native cores are the deliberate choice of [ADR-0028](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md): rather than one C core reached through FFI everywhere, each language gets an idiomatic implementation and the *contract* — a shared vector set, not a shared binary — is what keeps them from drifting. That ADR explicitly supersedes the earlier two-crate `libtracer-sys` FFI plan for Rust.

**The directory a thing lives in does not tell you which kind it is.** `bindings/` holds two native cores *and* one adapter ([`bindings/ros2`](https://github.com/avatarsd-llc/libtracer/tree/main/bindings/ros2/), the `rmw_tracer` RMW implementation, which compiles against the C++ core). `integrations/` holds no native core at all. Read the kind column below, not the path.

---

## What exists

### Native cores

| Core | Tree | Layers implemented | Layers absent |
| --- | --- | --- | --- |
| **C++** (reference, golden) | [`core/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/) | L0–L4 plus the transport plane | — |
| **Rust** | [`bindings/rust/`](https://github.com/avatarsd-llc/libtracer/tree/main/bindings/rust/) | L2/L3 codec + the typed TLV tier | L0/L1 substrate, L4 graph runtime, transports, client |
| **TypeScript** | [`bindings/typescript/packages/core/`](https://github.com/avatarsd-llc/libtracer/tree/main/bindings/typescript/packages/core/) | L2/L3 codec | L0/L1 substrate, L4 graph runtime; transports and the remote-operation client are separate packages |

### Ports and adapters

| Artifact | Tree | Kind | Delivers |
| --- | --- | --- | --- |
| **ESP-IDF component** | [`integrations/esp-idf/`](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/esp-idf/) | port | the full C++ node on ESP32 silicon, plus four platform-native translation units |
| **PlatformIO library** | [`integrations/platformio/`](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/platformio/) | port | the default C++ source set to any PlatformIO board, plus an ESP32 TWAI build hook |
| **ESPHome component** | [`integrations/esphome/`](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/esphome/) | port (stub) | nothing yet — one empty Python module |
| **Arduino metadata** | [`integrations/arduino/`](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/arduino/) | packaging (not planned) | nothing — metadata only |
| **`rmw_tracer`** | [`bindings/ros2/`](https://github.com/avatarsd-llc/libtracer/tree/main/bindings/ros2/) | adapter | ROS 2 nodes over the C++ graph, when it is finished |

There are no in-tree *bridges* — the `integrations/bridges/<protocol>/` slot that [CONTRIBUTING.md](https://github.com/avatarsd-llc/libtracer/blob/main/.github/CONTRIBUTING.md) reserves for foreign-protocol translators (Modbus, Z-Wave, vendor RPC) is empty.

---

## The three native cores

### C++ — the reference, and the only complete node

[`core/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/) is the only implementation that is a *node* rather than a *codec*. It carries the memory substrate (L0), the refcounted view layer (L1), the frame and TLV codec (L2/L3), the graph runtime (L4) and the transport plane — the split is spelled out in [core/STYLE.md](https://github.com/avatarsd-llc/libtracer/blob/main/core/STYLE.md)'s namespace table and catalogued in [10-module-catalog.md](10-module-catalog.md).

It is **golden**: when the wire changes, the C++ core blesses the new or updated vectors and every other core must then match them ([ADR-0028](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md)). That is an asymmetry with teeth — a disagreement between two cores is resolved in the C++ core's favour by construction, so the other cores are validated *against* it rather than merely alongside it.

It also spans the whole target spectrum. It is the only core with a **NARROW** footprint gate — the Cortex-M0 sentinel, a workflow of its own driving a measuring script in [`tools/`](https://github.com/avatarsd-llc/libtracer/tree/main/tools/) — and the only one that runs on a **WIDE** many-core host under the concurrency guarantees of [15-concurrency-and-scaling.md](15-concurrency-and-scaling.md). (A RISC-V footprint script sits beside the Cortex-M0 one in `tools/`, but no workflow runs it, so RV32 footprint is measurable on demand rather than gated.)

### Rust — the codec and the typed tier, `no_std`

[`bindings/rust/`](https://github.com/avatarsd-llc/libtracer/tree/main/bindings/rust/) is a from-scratch `#![no_std]` implementation that needs only `alloc` and declares no external dependencies. It is not a shim over C++ ([#57](https://github.com/avatarsd-llc/libtracer/issues/57)).

What it contains, module by module, is more than the phrase "wire codec" suggests: the raw `decode`/`encode` and the CRC primitives sit in the crate root, and on top of them sit six typed modules — a PATH string↔TLV converter with RFC-0018 packed-segment records, a `:field` selector codec, the `FWD` remote-operation envelope ([RFC-0004](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md)), typed accessors and builders for the structured containers (POINT, SETTINGS, ACL, SUBSCRIBER, SPEC), a TLV builder set, and the error/status registry.

**What is missing, exactly:**

- No L4 graph runtime. There is no `graph`, no vertex, no `read`/`write`/`await`, no subscription fan-out. A Rust program can *speak* the protocol's bytes; it cannot *be* a node.
- No transports. Nothing dials, listens or frames onto a bus.
- No client SDK. The `FWD` envelope can be built and parsed, but nothing drives a request/reply round trip over a socket — that is the TypeScript client's job today.
- No L0/L1 equivalent. There is no segment/rope substrate, so nothing here is zero-copy in the sense [08-views-and-ownership.md](08-views-and-ownership.md) means.
- No footprint gate. The crate is `no_std` and therefore *admissible* on a **NARROW** target, but unlike the C++ core no CI job measures what it costs there. "Compiles without `std`" is a weaker claim than "fits", and only the weaker one is currently backed.

### TypeScript — a codec package, plus a client and two transports beside it

[`bindings/typescript/`](https://github.com/avatarsd-llc/libtracer/tree/main/bindings/typescript/) is an npm workspace, not a single package; the layout is decided by [ADR-0033](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0033-npm-subpackage-monorepo.md). Four packages publish:

| Package | Directory | What it is |
| --- | --- | --- |
| `@avatarsd-llc/libtracer` | `packages/core` | the native TS wire codec — the cross-validated part |
| `@avatarsd-llc/libtracer-client` | `packages/client` | the remote-operation SDK over `FWD`/`FIELD` ([ADR-0034](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0034-typescript-client-sdk.md), [ADR-0035](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0035-implementing-rfc-0004-remote-operation-addressing.md)) |
| `@avatarsd-llc/libtracer-ws` | `packages/transport-ws` | a WebSocket transport |
| `@avatarsd-llc/libtracer-webtransport` | `packages/transport-webtransport` | a WebTransport transport |

The split is load-bearing, not cosmetic: the core package ships **no transport**, so a consumer that only needs the codec pulls no socket code, and the client takes its transport through an injected structural seam rather than importing one. Layer slicing inside the core is done with subpath `exports` (`@avatarsd-llc/libtracer/wire`) rather than by exploding into a package per layer.

**What is missing, exactly:**

- The core package publishes exactly two live entry points, the barrel and `./wire`. The `./mem` (L0), `./view` (L1) and `./graph` (L4) subpaths named in the ADR are **reserved, not implemented** — importing them resolves to nothing.
- There is therefore no TS graph runtime. The capability matrix records that as absent *by design*, not as a gap awaiting work.
- The client SDK and both transports are pre-1.0. The client package's own README labels itself EXPERIMENTAL; the workspace README and the [capability matrix](../capability-matrix.md) mark the client and both transports functional-but-experimental. That is a surface-stability caveat, not an unpublished-scaffold one: they are on the registry, they carry test suites, and CI runs them — but their APIs may still move. The codec core does not carry the caveat.
- The client's `kind=ERROR` reply codes are provisional until the ERROR registry ([#8](https://github.com/avatarsd-llc/libtracer/issues/8)) pins the code set.
- There is no MCU story. TypeScript addresses the **WIDE** end — browser, edge function, Node service — and nothing else. A `wasm32` target for one of the native cores, which would give the browser a non-TS option, is an open request ([#1386](https://github.com/avatarsd-llc/libtracer/issues/1386)), not a shipped capability.

### One version, one tag

Every publishable artifact is stamped in lockstep from the repo-root `VERSION` file by [`tools/sync-version.py`](https://github.com/avatarsd-llc/libtracer/blob/main/tools/sync-version.py) — the crate, the four npm packages and the lockfile, the PlatformIO manifest, the Arduino metadata and the ESP component manifest. One `vX.Y.Z` tag therefore means the same number everywhere, a CI gate fails on drift, and the ROS 2 package is deliberately left unstamped because it is unreleased.

The practical consequence for a reader: **treat a version number written into prose as stale until proven otherwise.** The authorities are `VERSION` and the registries, and a release needs no doc edit to happen.

---

## How the cores are held together

Four mechanisms, in increasing width of what they cover.

**1. The shared vectors are the contract.** Every vector under [`tests/conformance/vectors/v1/`](https://github.com/avatarsd-llc/libtracer/tree/main/tests/conformance/vectors/) is a directory holding `input.bin`, `expected.json` and `description.md`; a negative case carries `reject.bin` instead and names the decode error every core must answer. Passing them is the operational definition of compatibility. The category set is whatever the directory holds — at the time of writing, fourteen categories covering framing, CRC, the type-code registry, paths and packed path segments, path labels, path references, `:field`, `FWD`, ACL, SETTINGS, SUBSCRIBER policy, STREAM history, creation SPECs and errors.

**2. A polyglot driver gates every core on all of them.** [`tests/conformance/run-all.py`](https://github.com/avatarsd-llc/libtracer/tree/main/tests/conformance/) reads a harness registry, runs each core's TAP-emitting harness over the vector set, prints an implementation × vector matrix, and exits non-zero if any enabled core fails a vector, is *missing* a vector that exists on disk, or two cores disagree. The registry is one file, and the differential fuzzer reads the same one, so the two drivers cannot drift on how a core is invoked.

**3. A differential fuzzer covers the shapes the curated vectors miss.** [`tests/conformance/diff_fuzz.py`](https://github.com/avatarsd-llc/libtracer/tree/main/tests/conformance/) generates random *valid* frames from an explicit seed and asserts byte equality across all three cores' decode-then-encode round trips. The generator computes its own CRCs, so it is a fourth independent codec: agreement proves the cores match the canonical form, not merely each other.

**4. Live interop covers what a vector cannot.** Byte agreement on a decoded frame says nothing about whether a real TypeScript client can talk to a real C++ server. Three standalone workflows close that: `ws-interop` runs the TS WebSocket transport against a C++ `transport_ws_server` over a real socket (a genuine RFC 6455 handshake, a masked client frame in, an unmasked server frame back) and differential-fuzzes the two WebSocket frame decoders against each other; `wt-interop` drives a browser WebTransport client against a C++ server; `mesh-testbed` stands up a four-node mesh and drives it from the TS client SDK to assert multi-hop routing.

Alongside these, a coverage audit walks every vector as a TLV tree and reports which type codes and `opt` bits the green actually exercises — so "all vectors pass" can be qualified by *what* was covered rather than asserted bare.

What the green amounts to today, as the driver prints it: **92 vectors × 3 cores, all passing, with 16/16 v1 type codes and 6/6 `opt` bits covered.** That figure is a snapshot of the vector set at the version in `VERSION`, quoted here as evidence that the gate is non-vacuous — the vector set is append-only for a published spec version, so it only grows.

**What this does not cover.** The vectors are a codec contract. They say nothing about the L4 graph semantics, because only one core implements L4 at all; nothing about transports beyond the interop jobs listed above; and nothing about a port, because a port compiles the C++ codec and cannot diverge from it. A port's CI evidence is a *build* (`esp-idf` builds the ESP32-C6 and C3 targets; `pio-esp32-can` packs the library and builds a consumer project), and a build is a weaker claim than a passing vector — it proves the code compiles and links for that platform, not that frames move on a real bus.

---

## Ports and adapters, one by one

### ESP-IDF — the only port that is not thin

[`integrations/README.md`](https://github.com/avatarsd-llc/libtracer/blob/main/integrations/README.md) opens by calling the integrations thin — "they configure build files, register the library with the host's package format, and provide a minimal example." **That is accurate for PlatformIO, ESPHome and Arduino, and not accurate for ESP-IDF**, which is worth stating plainly rather than leaving the reader to discover it. The ESP-IDF component carries four platform-native translation units of its own (an IDF-native WebSocket server link over `esp_http_server`, an IDF-native WebSocket client link, a TWAI CAN link, and a critical-section pool), five public headers, a Kconfig, a `poll.h` compatibility shim, four example projects and a host-test suite of twenty-one test translation units run against faked IDF headers.

The reason it is not thin is a correctness ruling, not scope creep: ESP-IDF's WebSocket plane must never use POSIX sockets, because lwIP's `sendmsg` refuses the flag the portable scatter-gather egress asks for — on silicon the portable server completed handshakes and answered pings while silently discarding every data frame ([#947](https://github.com/avatarsd-llc/libtracer/issues/947), [#948](https://github.com/avatarsd-llc/libtracer/issues/948)). Platform-native links are the fix, and they can only live in the platform's tree. Translation-unit selection carries the difference; there are no feature macros in shared sources.

### PlatformIO — ships the core, one hook, one honest gap

Ships and compiles the default C++ source set, with an ESP32 build hook that compiles the ESP-IDF TWAI driver so `transport_can` has a real bus. Two things a reader must not miss: a PlatformIO `espressif32` build ships **no WebSocket transport at all** (the portable pair is excluded per-environment for the reason above, and the IDF-native replacements are not packaged for PlatformIO), and the CAN glue is gated at compile-and-link only — the workflow packs the package and builds a consumer project, energising no pin. Moving frames on a real bus is a separate, still-open sign-off.

### ESPHome — a stub, and the README says so

One empty Python module under `components/libtracer/`. The README documents an intended conformance profile and module set; none of it is implemented.

### Arduino — metadata, deliberately not planned

A `library.properties` and nothing else. It is not on the Arduino Library Manager and has never been submitted, and the layout would not satisfy the registry anyway — Arduino requires the manifest in the repository root. The README states the reason it is not planned (the Arduino toolchain and its AVR/no-STL targets are a poor fit for a C++23 core) and routes readers to PlatformIO or ESP-IDF.

### `rmw_tracer` — an adapter with a plan and one translation unit

Selecting `RMW_IMPLEMENTATION=rmw_tracer` is intended to put any ROS 2 node — `rclcpp`, `rclpy`, `rclc`/micro-ROS — on a libtracer graph with no node code changes ([ADR-0023](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0023-ros2-binding-via-rmw-tracer.md)). Today it is an early stub: one real translation unit (the implementation-identifier entry points, eighteen lines), a package manifest and a CMake file. It is build-verified in a `ros:jazzy` container by a helper script, never in CI, because the runners carry no ROS toolchain. The full RMW C ABI is roughly 198 entry points and the package README carries the phased plan for them.

The mapping question a reader should know about before relying on it: **ROS QoS has no single carrier in libtracer, and the page that said it did was retracted.** Reliability and durability ride the *subscription's* packed delivery policy; history depth is owner-side and a remote subscriber cannot set it; deadline, liveliness and lifespan map to nothing at all. Reference 12's rung 4 describes the composition an `rmw_tracer` node would sit in ([12-deployment-profiles.md](12-deployment-profiles.md)); the QoS carriers and the retraction are set out in the package's own README, which is canonical for them until a reference article on the ROS 2 integration exists ([#1382](https://github.com/avatarsd-llc/libtracer/issues/1382) article 4).

---

## Picking one

Start from what you are building, not from what language you like.

| If you are… | Take | Why |
| --- | --- | --- |
| building a node — something that owns vertices, stores values, fans out to subscribers, or forwards | the **C++ core** | it is the only implementation with an L4 graph runtime. There is no second option, in any language. |
| putting a node on ESP32 silicon | the **ESP-IDF component** | it carries the platform-native WebSocket and CAN links a chip build requires; a portable build is silently broken there |
| putting a node on some other embedded board | the **PlatformIO library** | it ships the core and lets the project select its transport — read the `espressif32` WebSocket exclusion first |
| writing a browser or edge-function client | the **TypeScript client** over one of the TS transports | it is the only client SDK that drives `FWD` round trips, and the only core that runs in a browser today |
| writing a TypeScript program that only needs to parse or build frames | the **TS core package** alone | no transport dependency, and it is the cross-validated part of the workspace |
| writing Rust that parses or builds frames — a test tool, a proxy, a bus sniffer, an MCU firmware that emits telemetry | the **Rust crate** | native, `no_std`, no C++ toolchain in the build. Accept that it is not a node. |
| writing Rust that needs to *be* a node | nothing here yet | there is no Rust graph runtime and no Rust transport. Either drive the C++ core yourself or wait. |
| putting ROS 2 on a libtracer graph | not yet | `rmw_tracer` is one translation unit of ~198 entry points |
| writing a fourth implementation | the [spec](../spec/v1.md) and the vectors | the reference suite is written to be sufficient without reading `core/`; register the result in [`docs/implementations.md`](../implementations.md) |

On the target spectrum: the **C++** core is the only one that spans NARROW through WIDE. **Rust** is admissible at the NARROW end and unproven there. **TypeScript** is WIDE-only. Nothing outside the C++ core has a MID or WIDE *node* story at all, because a node needs L4.

---

## Pitfalls

- **Reading `bindings/` as "the native cores".** It holds two cores and one adapter. `bindings/ros2` compiles against the C++ core and implements no wire format of its own; the capability matrix files it with the ports for exactly that reason.
- **Reading "cross-validated" as covering more than the codec.** All-green means three cores agree byte-for-byte on the frames the vectors and the fuzzer describe. It does not mean three cores implement the same amount of libtracer — two of them implement no graph at all.
- **Reading a published package as a finished surface.** Publication and maturity are independent axes here: the crate and all four npm packages publish on a release tag, and three of the four npm packages are marked experimental. "On npm" answers *can I install it*, not *will the API hold*.
- **Assuming a port inherits every transport.** It does not. A PlatformIO `espressif32` build has no WebSocket; an ESP-IDF chip build registers no `ws` kind in the built-in catalog, so a `:children[]` creation SPEC naming `kind=ws` with no staged link answers `SCHEMA_NOT_FOUND` — its WebSocket links are handed in by the application rather than constructed by a factory.
- **Reading a build gate as a runtime gate.** The ESP and PlatformIO CAN jobs prove compile-and-link. Neither drives a bus. A green check on those workflows is not evidence that frames moved.
- **Quoting a version number from prose.** Every artifact is stamped from `VERSION` on release with no doc edit, so any number written into a README is a snapshot of the day it was written.
