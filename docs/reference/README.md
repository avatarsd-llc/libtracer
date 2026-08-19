# Reference suite — reading guide

> **Status**: draft. Promotion of a section to "frozen for v1" follows the promotion rule below.
>
> **Audience**: a second implementer writing an interoperable libtracer in any language, on any platform, without reading the reference implementation.
>
> **Reading time**: full suite ~2.5 h.

## Architectural commitments

The nine claims below are load-bearing across every section. A design that contradicts one of them is not a variant of libtracer.

1. **Six-layer model**, numbered bottom-up: L0 memory substrate / L1 views and ownership / L2 frame envelope / L3 TLV semantics / L4 graph endpoint logic / L5 application semantics.
2. **Everything is a module.** No "core vs module" split. Some modules are required for every conforming node (frame codec, dispatcher, refcount/view machinery, forwarder logic); the rest are opt-in (transports, discovery, security, executors, memory backends, view modules). The required set is identified in [10-module-catalog.md](10-module-catalog.md), not by architectural privilege.
3. **Wire format is one-shot.** No per-frame version bit. Future incompatible changes are versioned at the discovery layer (different mDNS service name, port). Get it right once.
4. **Fixed-width length** with `LL` bit selecting u16 (default, ≤ 64 KiB) or u32 (≤ 4 GiB). No u64 — the interop ceiling forces address-shift discipline.
5. **Trailer-positioned CRC and TS.** Header + payload + optional trailer. The trailer is append-only at egress, strip-only at ingress. Payload bytes are invariant from publication to all subscribers.
6. **No generic `LIST` type code** (`0x05` is a reserved code with no assigned meaning in v1). Every structured TLV declares its purpose via type code. User-defined records use user-range types `0x80–0xFF` with `opt.PL=1`.
7. **Explicit-source-routed net plane.** A remote operation rides an `FWD` frame carrying its own route: `dst` shrinks per hop, `src` accumulates the way back — loop-free by construction, no dedup state. `0x0D ROUTER` is a reserved, decodable wire code with no implemented mechanism.
8. **No fragmentation in the wire format.** Logically large messages are addressed across `ep[0..N]` slices with a shared timestamp.
9. **Path handles, encoded once.** Every vertex address used more than once is encoded into a PATH TLV at build time (a `.rodata` literal) or at node init (one allocation), not at every write. Hot-path APIs accept handles, never strings; `snprintf` is a code-size luxury, not a protocol requirement. Normative in [../spec/v1.md](../spec/v1.md) §3.1; described in [03-addressing.md](03-addressing.md), [05-protocol-tlvs.md](05-protocol-tlvs.md), [04-communication-flows.md](04-communication-flows.md) and [06-user-data-packing.md](06-user-data-packing.md).

---

## Purpose

This directory describes **the libtracer protocol as a standard**, independent of any implementation. The C++23 reference implementation under [`core/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/) is one conforming implementation; a TypeScript core, a Rust core, or a hardware FPGA implementation are all admissible if they conform to what is written here.

Design rationale — *why* the protocol looks the way it does — lives in [`docs/adr/`](https://github.com/avatarsd-llc/libtracer/tree/main/docs/adr/) and in git history. The sections here describe *what it is*, and are canonical for that question.

---

## Section index

The suite is ordered with the most-significant concerns first (the graph mental model and how nodes talk), narrower-scope concerns later (per-TLV byte spec, substrate layers).

The toctrees at the foot of this page are the order of record: the table below carries a layer and a summary that a toctree cannot, but where the two disagree about placement, the toctree wins.

| File | Layer | Topic |
| ---- | ---- | ---- |
| [00-overview.md](00-overview.md) | all | The standard in one document; six-layer model; load-bearing claims; conformance profiles; portability. |
| [01-data-format.md](01-data-format.md) | L2 | TLV header (4-byte default, 6-byte extended); `opt` bits PL/TS/CR/LL/CW/TF; trailer-positioned TS + CRC; type-code registry; rejected designs. |
| [02-graph-model.md](02-graph-model.md) | L4 | Vertex / edge / path / view / segment definitions; the same-substrate insight (a TLV in memory *is* a graph node *is* wire bytes); refcount memory ordering; structured-TLV-as-abstraction and memory-as-rope; schema discipline. |
| [03-addressing.md](03-addressing.md) | L4 | Path EBNF, field-chain resolution, atomic multi-field writes, wildcards, address-shift slicing rules, address scopes (local/routed/global), canonicalization. |
| [04-communication-flows.md](04-communication-flows.md) | L4 | Sequence diagrams for read, write + fan-out, await, subscribe, unsubscribe, QoS update, multi-hop FWD forwarding, address-shift fan-out, deadline expiry, liveness loss, partition and recovery, schema discovery. |
| [05-protocol-tlvs.md](05-protocol-tlvs.md) | L3 | Per-TLV byte spec for the first block `0x01`–`0x0E`: VALUE, NAME, DESCRIPTION, SUBSCRIBER, PATH, POINT, ERROR, STATUS, ACL, SETTINGS, TIME, SPEC (`0x05` is a reserved code with no assigned meaning; `0x0D` ROUTER is reserved and decodable with no implemented mechanism). The fast-track `0x0F`–`0x1F` range: `0x0F` FWD, `0x10` FIELD, `0x11`–`0x13` route-handle control frames, `0x14` PATH_REF (the bound-path address form, RFC-0024) and `0x15` PATH_REF_REVERSE (the reverse-direction list a mint-flagged request accumulates, RFC-0024 §7.1 amendment 2). Error-code registry. Reserved-range policy. |
| [06-user-data-packing.md](06-user-data-packing.md) | L4/L5 | Worked examples spanning eight orders of magnitude: 1-byte boolean, GPIO register as MMIO view, IMU record, 1 GB/s ADC streaming with DMA, camera + LIDAR temporal join, shared-variable pattern. Mix / split / concat invariants. |
| [07-host-embedding.md](07-host-embedding.md) | L4 | Per-host DAG (own vertices plus transport-vertex links); global topology (any shape, cycles allowed); loop safety by explicit source routes (`dst`-monotonicity — no visited-set, no revisit `ERROR`); node identity; embedding examples (RC car, robot, fleet, mesh, WAN). |
| [08-views-and-ownership.md](08-views-and-ownership.md) | L1 | The refcounted-view layer. Canonical view struct; rope (chain of views) semantics; refcount memory ordering; the TLV-as-cast operation; the two parser contexts (wire-receive and in-memory walk); view-module catalog; cross-substrate transitions; the end-to-end DMA→ADC→network trace across all six layers. The modular memory-binding contract (ADR-0012) for the hard integrations: MMIO TOCTOU, cross-process refcount with grace/epoch, lwIP pbuf, rope flatten, DMA coherency, register binding. |
| [09-memory-substrate.md](09-memory-substrate.md) | L0 | Categories of memory (heap, pool, MMIO, DMA, network-stack buffers, shared memory, peripheral FIFOs); backend interface (`mem_backend_t`); backend catalog; ownership rules; cache coherency; pressure handling. |
| [10-module-catalog.md](10-module-catalog.md) | all | Every module across all layers, in one place. Required versus optional. Pairing table: which L0 backend pairs with which L1 view module pairs with which transport. Inter-module interfaces. Required module set per conformance profile. |
| [11-vertex-roles-and-aggregation.md](11-vertex-roles-and-aggregation.md) | L4 | The vertex-facade principle: a path names a contract, not an implementation. Seven vertex roles (stored, stream, sink-with-model, computed, proxy, aggregate, live MMIO). The canvas worked through in both transferred and mirror modes. Address grouping (multi-source fan-in, multi-sink fan-out, compound vertices, per-transport split). |
| [12-deployment-profiles.md](12-deployment-profiles.md) | all | The deployment-rung spectrum (in-process → single-transport leaf → forwarder → RTSP → ROS 2 → flagship GPU); which optional modules each rung adds; the conformance profile per rung. |
| [13-network-formation.md](13-network-formation.md) | L4 | How a third party (typically a web UI) forms a graph across nodes: discover → delegate admin → create (controllers *and* transport connections, one in-band mechanism) → bind (consumer-initiated subscribe-writes) → depart, leaving devices wired. The two-ACL fan-in/fan-out guard; consumer-dials/producer-pushes; arbitrary folding. |
| [14-can-transport.md](14-can-transport.md) | L4 | Header-elided CAN: the structured 29-bit extended ID (`version\|node\|endpoint`, lower ID = higher bus priority); classic and CAN-FD framing modes; multi-frame reassembly via address-shift slicing and advertise + id-match (not ISO-TP); the in-band advertise frame and the dynamic, self-healing identity↔path map held inside `transport_can`. |
| [15-concurrency-and-scaling.md](15-concurrency-and-scaling.md) | cross-cutting | What a conforming implementation must guarantee under concurrency; the four hardware regimes that decide whether adding threads helps (disjoint / one contended RMW / a blocking lock / a spinning lock) and how to recognise each in one's own data; why an owning read is a write; which graph topologies scale and which cannot. Measurements of the reference implementation live in [`../design/concurrency/`](../design/concurrency/README.md), not here. |
| [16-websocket-session-auth.md](16-websocket-session-auth.md) | L4 | Post-handshake session authentication on the WebSocket transport: why a browser cannot present a header and why the credential must not ride the URL; the in-band authentication frame as an opaque CARRIER (accept / continue / reject, with a reply payload) rather than a token format; what "admitted but served nothing" gates in both directions; the session subject and its boundary with #375; the deadline that bounds an unauthenticated session; the two application-range close codes. |
| [17-reclamation-policy.md](17-reclamation-policy.md) | cross-cutting | WHEN libtracer may free the memory behind a user-code seam after the user releases it: why `unsubscribe()` cannot answer that on its own (fan-out snapshots and dispatches outside every lock, and the `{fn, ctx}` leg is the one part it does not own), the **grace point** model, and the three build-time-closed policies that name one — `reclaim_strict` (the return), `reclaim_local` (this thread's dispatch stack unwinds; the default, because it makes MCU and host behave identically), `reclaim_qsbr` (specified, not yet implemented). The rule that shapes all of them: the library owns the tracking and SIGNALS release — the embedder never polls in-flight state. Measured latency and footprint for each. |
| [18-composition-over-the-network.md](18-composition-over-the-network.md) | L4 | What the graph model becomes when it crosses a node boundary: the transport-vertex mount as the whole mechanism; what a remote vertex is (a path, never a replica, proxy or handle); what survives the boundary unchanged and what necessarily differs (location-dependent addresses, the local/remote creation asymmetry, the mount-routed-only wire subscribe door, no cross-node atomicity or ordering); the three failure modes composition introduces — partition, late join, retire. |
| [19-transports-are-vertices.md](19-transports-are-vertices.md) | L4 | The transport-as-vertex commitment: what is a vertex on the net plane (`/net`, `/net/<module>`, the creator endpoint, the connection vertex) and what deliberately is not (bus peers, connection config, `:stats`); what uniform addressing, lifecycle and introspection buy; what they cost (path bytes per hop, two lifecycles, structural vertices, control-plane work on a receive thread, a peer-drivable creation door, no reconfiguration door); and the standing leanness rule that keeps `conn_settings_t` free of kind-private fields. |

> **File numbers are authoring order, not significance order.** 00–07 is the original layer-agnostic narrative (overview → wire → graph → addressing → flows → TLV registry → user data → host embedding); 08–09 are the substrate layers, appended when the substrate split into its own pair of documents; 10 and above are cross-cutting or later topics. Layer numbers (L0..L5) are bottom-up by architecture and bear no relation to file order. The `NN` prefixes are the citation key used throughout the repository and in source comments, so they are stable even where the narrative order has moved on.

---

## Conformance profiles (build-size axes)

Distinct from the architectural layers above — a profile describes what set of modules a deployment loads.

| Profile | What it loads | Typical use |
| ---- | ---- | ---- |
| **P0 — in-process** | required modules only | unit tests; in-process pub/sub; the substrate other profiles compose against |
| **P1 — single-transport leaf** | required + 1 transport | RC car over UART; sensor over CAN; ESP32 over Wi-Fi |
| **P2 — forwarder** | required + ≥ 2 transports | gateway between buses (CAN ↔ IP); edge router |
| **P3 — full** | P2 + discovery + executor + security | production deployment |

Higher profiles are strict supersets: every module required at P*n* is required at P*n+1*. The literal module list per profile is in [10-module-catalog.md](10-module-catalog.md) §required modules per conformance profile.

---

## Reading paths

**First-time reader.** 00 → 01 → 02 → 03 → 04 → 05 → 06 → 07. Readers who want ownership and zero-copy settled before the wire format insert 09 → 08 immediately after 00.

**Writing a codec or sender in another language.** 01 → 03 → 05 → 06, then 02 and 08 once the implementation optimizes for zero-copy.

**Porting to a new platform** (new MCU, new RTOS, new buffer ecosystem). 09 → 08 → 10, then 01 and 02. Substrate work is confined to the lower-layer sections; the protocol contract is unchanged by a port.

**Targeting a 16–32 KB MCU** (Cortex-M0/M3/M4, RISC-V µC). [../spec/v1.md](../spec/v1.md) §3.1 static path-handle conformance → [03-addressing.md](03-addressing.md) §static path handles → [05-protocol-tlvs.md](05-protocol-tlvs.md) §static / pre-encoded PATH TLV → [06-user-data-packing.md](06-user-data-packing.md) §MCU-friendly publishing → [04-communication-flows.md](04-communication-flows.md) §the static-path write flow. These five sections carry the no-`snprintf`, no-malloc-on-the-hot-path discipline that lets a node fit in a Cortex-M0 ISR; no toctree reproduces that route, because it addresses sections rather than whole files.

**Tracing the DMA→ADC→network path end to end.** [08-views-and-ownership.md](08-views-and-ownership.md) §end-to-end trace follows one buffer from a DMA half-complete interrupt to an egress NIC, naming each layer's contribution at every step.

---

## Promotion rule

A reference section is promoted from "draft" to "frozen for v1" when all three of the following hold:

1. The corresponding behaviour is implemented and tested in the reference implementation.
2. A second-implementer review confirms the section is sufficient to write an interoperable parser, sender or forwarder from the section alone — not from the reference source.
3. The conformance test suite covers the section's behaviour.

Until all three hold, the section is the operating reference for second-implementer questions.

The wire format does not version per frame. v1 is committed once; future incompatible changes are versioned at the discovery layer (a different mDNS service name, a different port). See [01-data-format.md](01-data-format.md) §versioning and compatibility.

---

## What this suite is NOT

- **Not an API/ABI specification.** The reference implementation's headers describe its own API; this suite is language-agnostic and names no type or function as the subject of a rule.
- **Not a build or packaging guide.** The reference implementation's own configuration space — which module set a build contains, which sizes and policies it binds, what each costs per target — is in [`../design/config/`](../design/config/README.md).
- **Not a feature comparison against Zenoh, DDS or MQTT.** See the [project README](https://github.com/avatarsd-llc/libtracer/blob/main/README.md).
- **Not a security architecture.** The wire format is security-agnostic; security wraps it at the transport layer, per [10-module-catalog.md](10-module-catalog.md).
- **Not a roadmap.** The issue tracker holds that.
- **Not a performance specification.** A few sections quote measurements — [15-concurrency-and-scaling.md](15-concurrency-and-scaling.md) most heavily, and [09-memory-substrate.md](09-memory-substrate.md) and [00-overview.md](00-overview.md) where a figure is what makes a structural claim checkable. Every such number appears as *evidence for a claim about hardware*, with the host named — never as a number an implementation must hit. Anything specific to the reference implementation's own locks and costs is deliberately outside this suite, in [`../design/concurrency/`](../design/concurrency/README.md), so that every section here stays writable from the spec alone.

```{toctree}
:caption: Overview & cross-cutting
:hidden:
:maxdepth: 1

Overview — the six-layer model <00-overview>
Module catalog & composition <10-module-catalog>
Deployment profiles <12-deployment-profiles>
Concurrency & scaling <15-concurrency-and-scaling>
Reclamation policy <17-reclamation-policy>
```

```{toctree}
:caption: Substrate — L0 memory, L1 views
:hidden:
:maxdepth: 1

Memory substrate <09-memory-substrate>
Views & ownership <08-views-and-ownership>
```

```{toctree}
:caption: L2/L3 — wire format
:hidden:
:maxdepth: 1

Data format <01-data-format>
Protocol-defined TLVs <05-protocol-tlvs>
```

```{toctree}
:caption: L4 — graph semantics
:hidden:
:maxdepth: 1

Graph model <02-graph-model>
Addressing <03-addressing>
Communication flows <04-communication-flows>
User data packing <06-user-data-packing>
Vertex roles & aggregation <11-vertex-roles-and-aggregation>
```

```{toctree}
:caption: Embedding, formation & transports
:hidden:
:maxdepth: 1

Host embedding <07-host-embedding>
Network formation <13-network-formation>
CAN transport <14-can-transport>
WebSocket session authentication <16-websocket-session-auth>
Composition over the network <18-composition-over-the-network>
Transports are vertices <19-transports-are-vertices>
```
