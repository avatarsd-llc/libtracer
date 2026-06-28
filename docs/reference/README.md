# libtracer Protocol Reference (protocol v1, draft)

> **Status**: draft suite. All sections written; promotion to "frozen" gated by the conformance milestone tracked in the issue tracker.
> **Last revision**: 2026-05-03. Notable architectural commitments:
>
> 1. **Six-layer model**, numbered bottom-up: L0 memory substrate / L1 views and ownership / L2 frame envelope / L3 TLV semantics / L4 graph endpoint logic / L5 application semantics.
> 2. **Everything is a module.** No "core vs module" split. Some modules are required for every conforming node (frame codec, dispatcher, refcount/view machinery, router, bridge logic); the rest are opt-in (transports, discovery, security, executors, memory backends, view modules). The required set is identified in [10-module-catalog.md](10-module-catalog.md), not by architectural privilege.
> 3. **Wire format is one-shot.** No per-frame version bit. Future incompatible changes are versioned at the discovery layer (different mDNS service name, port). Get it right once.
> 4. **Fixed-width length** with `LL` bit selecting u16 (default, ≤ 64 KiB) or u32 (≤ 4 GiB). No u64 — interop ceiling forces address-shift discipline.
> 5. **Trailer-positioned CRC and TS.** Header + payload + optional trailer. Trailer is append-only at egress, strip-only at ingress. Payload bytes invariant from publication to all subscribers.
> 6. **No generic `LIST` type code** (`0x05` retired). Every structured TLV declares its purpose via type code. User-defined records use user-range types `0x80–0xFF` with `opt.PL=1`.
> 7. **ROUTER as bridge envelope.** The `ROUTER` TLV wraps the data TLV at the bridge boundary; bridges shed it on ingest, attach a fresh one on egress. Cycle dedup uses `(origin_peer_id, origin_timestamp)` recent-set.
> 8. **No fragmentation in the wire format.** Logically large messages are addressed across `ep[0..N]` slices with shared timestamp.
> 9. **Path handles, encoded once.** Every vertex address used more than once is encoded into a PATH TLV at build time (`.rodata` literal) or at node init (one allocation), not at every write. Hot-path APIs accept handles, never strings; `snprintf` is a code-size luxury, not a protocol requirement. Normative in [../spec/v1.md](../spec/v1.md) §3.1; design in [03-addressing.md](03-addressing.md), [05-protocol-tlvs.md](05-protocol-tlvs.md), [04-communication-flows.md](04-communication-flows.md), [06-user-data-packing.md](06-user-data-packing.md).
>
> Design rationale is recorded in [../../docs/adr/](https://github.com/avatarsd-llc/libtracer/tree/main/docs/adr/) and git history; this suite is the byte-level spec.
> **Audience**: a second implementer writing an interoperable libtracer in any language, on any platform, without reading the C reference implementation.
> **Reading time**: full suite ~2.5 h.

---

## Purpose

This directory describes **the libtracer protocol as a standard**, independent of any implementation. The C reference implementation under [../../core/](../../core/) is one conforming implementation; a TypeScript core, a Rust core, a hardware FPGA implementation are all admissible if they conform to what is written here.

Design rationale is recorded in [../../docs/adr/](https://github.com/avatarsd-llc/libtracer/tree/main/docs/adr/) and git history — they explain *why* the protocol looks the way it does. Reference documents here describe *what it is*, and are canonical.

---

## Section index

The reference is ordered with most-significant concerns first (graph mental model and how nodes talk), narrower-scope concerns later (per-TLV byte spec, substrate layers).

| File | Layer | Topic |
| ---- | ---- | ---- |
| [00-overview.md](00-overview.md) | all | The standard in one document; six-layer model; load-bearing claims; conformance profiles; portability. |
| [01-data-format.md](01-data-format.md) | L2 | TLV header (4-byte default, 6-byte extended); `opt` bits PL/TS/CR/LL/CW/TF; trailer-positioned TS + CRC; type-code registry; rejected designs. |
| [02-graph-model.md](02-graph-model.md) | L4 | Vertex / edge / path / view / segment definitions; the same-substrate insight (TLV in memory IS graph node IS wire bytes); refcount memory ordering; structured-TLV-as-abstraction / memory-as-rope; ROUTER shedding rule; schema discipline. |
| [03-addressing.md](03-addressing.md) | L4 | Path EBNF, field-chain resolution, atomic multi-field writes, wildcards, address-shift slicing rules, address scopes (local/bridged/global), canonicalization. |
| [04-communication-flows.md](04-communication-flows.md) | L4 | ASCII sequence diagrams for read, write+fanout, await, subscribe, unsubscribe, QoS update, bridge republish, address-shift fanout, deadline expiry, liveness loss, partition+recovery, schema discovery. |
| [05-protocol-tlvs.md](05-protocol-tlvs.md) | L3 | Per-TLV byte spec for `0x01`–`0x0D` (with `0x05` retired): VALUE, NAME, DESCRIPTION, SUBSCRIBER, PATH, POINT, ERROR, STATUS, ACL, SETTINGS, TIME, ROUTER. Error code registry. Reserved-range policy. |
| [06-user-data-packing.md](06-user-data-packing.md) | L4/L5 | Worked examples spanning eight orders of magnitude: 1-byte boolean, GPIO register as MMIO view, IMU record, 1 GB/s ADC streaming with DMA, camera+LIDAR temporal join, shared-variable pattern. Mix/split/concat invariants. |
| [07-host-embedding.md](07-host-embedding.md) | L4 | Per-host DAG (own vertices + bridge proxies); global topology (any shape, cycles allowed); cycle handling via `(origin_peer_id, origin_timestamp)` recent-set; bridge identity; embedding examples (RC car, robot, fleet, mesh, WAN). |
| [08-views-and-ownership.md](08-views-and-ownership.md) | L1 | Refcounted-view layer. Canonical view struct; rope (chain of views) semantics; refcount memory ordering; the TLV-as-cast operation; two parser contexts (wire-receive vs in-memory walk); view-module catalog; cross-substrate transitions; **end-to-end DMA→ADC→network trace** across all six layers. The **resolved modular memory-binding contract** (ADR-0012) for the hard integrations: MMIO TOCTOU, cross-process refcount + grace/epoch, lwIP pbuf, rope-flatten, DMA coherency, register binding. |
| [09-memory-substrate.md](09-memory-substrate.md) | L0 | Categories of memory (heap, pool, MMIO, DMA, network-stack buffers, shared memory, peripheral FIFOs); backend interface (`mem_backend_t`); backend catalog; ownership rules; cache coherency; pressure handling. |
| [10-module-catalog.md](10-module-catalog.md) | all | Every module across all layers, in one place. Required vs optional. Pairing table: which L0 backend pairs with which L1 view module pairs with which transport. Inter-module interfaces. Per-profile build manifests. |
| [11-vertex-roles-and-aggregation.md](11-vertex-roles-and-aggregation.md) | L4 | The vertex-facade principle: a path names a contract, not an implementation. Seven vertex roles (stored, stream, sink-with-model, computed, proxy, aggregate, live MMIO). The canvas worked through both transferred and mirror modes. Address grouping (multi-source fan-in, multi-sink fan-out, compound vertices, per-transport split). |
| [12-deployment-profiles.md](12-deployment-profiles.md) | all | The deployment-rung spectrum (in-process → single-transport leaf → bridge → RTSP → ROS 2 → flagship GPU); which optional modules each rung adds; conformance profile per rung. |
| [13-network-formation.md](13-network-formation.md) | L4 | How a third party (typically a web UI) forms a graph across nodes: discover → delegate admin → create (controllers *and* transport connections, one in-band mechanism) → bind (consumer-initiated subscribe-writes) → depart, leaving devices wired. The two-ACL fan-in/fan-out guard; consumer-dials/producer-pushes; arbitrary folding. |
| [14-can-transport.md](14-can-transport.md) | L0/L1/transport | Header-elided CAN: the structured 29-bit extended ID (`version\|node\|endpoint`, lower ID = higher bus priority); classic/CAN-FD framing modes; multi-frame reassembly via address-shift slicing / advertise+id-match (not ISO-TP); the in-band advertise frame and the dynamic, self-healing identity↔path map held inside `transport_can`. |

> **Note on file numbering vs significance ordering**: 00–07 follows the original layer-agnostic narrative (overview → wire → graph → addressing → flows → TLV registry → user data → host embedding). 08–09 are the substrate layers (added when the L(-1)/L(-2) design split out into its own pair of docs); they sit at the end because most readers reach them only after the protocol layers click. 10 is the cross-cutting catalog. Layer numbers (L0..L5) are bottom-up by architecture, not by file order.

---

## Conformance profiles (build-size axes)

Distinct from the architectural layers above — these describe what set of modules a deployment loads.

| Profile | What it loads | Typical use |
| ---- | ---- | ---- |
| **P0 — in-process** | required modules only | unit tests; in-process pub/sub; the substrate other profiles compose against |
| **P1 — single-transport leaf** | required + 1 transport | RC car over UART; sensor over CAN; ESP32 over Wi-Fi |
| **P2 — bridge** | required + ≥2 transports + cycle-dedup | gateway between buses (CAN ↔ IP); edge router |
| **P3 — full** | P2 + discovery + executor + security | production deployment |

Higher profiles are strict supersets. See [10-module-catalog.md](10-module-catalog.md) §per-profile manifests for the literal module list per profile.

---

## Reading paths

**First-time reader (top-down, narrative)**: 00 → 01 → 02 → 03 → 04 → 05 → 06 → 07.

**First-time reader (bottom-up, substrate-first)**: 00 → 09 → 08 → 01 → 02 → 03 → 04 → 05 → 06 → 07. Use this path if you want to understand zero-copy and ownership before the wire format.

**Writing a parser/sender in another language**: 01 → 03 → 05 → 06, then 02 + 08 once you optimize for zero-copy.

**Porting libtracer to a new platform** (new MCU, new RTOS, new buffer ecosystem): 09 → 08 → 10 → 01 + 02. Substrate work is in the lower-layer docs; the protocol contract is unchanged.

**Implementing a router or bridge**: 02 → 03 → 04 → 07 mandatory; 06 illustrative; 08 if you need to reason about cross-substrate transitions.

**Designing an application's data layout**: 06 → 03 → 02. Refer back to 05 for any TLV you handle.

**Targeting a 16–32 KB MCU (Cortex-M0/M3/M4, RISC-V µC)**: [../spec/v1.md](../spec/v1.md) §3.1 → [03-addressing.md](03-addressing.md) §static path handles → [05-protocol-tlvs.md](05-protocol-tlvs.md) §static / pre-encoded PATH TLV → [06-user-data-packing.md](06-user-data-packing.md) §MCU-friendly publishing → [04-communication-flows.md](04-communication-flows.md) §the static-path write flow. This path explains the no-`snprintf`, no-malloc-on-the-hot-path discipline that makes libtracer fit in a Cortex-M0 ISR.

**Auditing a deployment for cycles or routing storms**: 07 → 04 (bridge republish flow).

**Building or extending a module** (transport, discovery, security, executor, memory backend): 10 first, then the layer-specific spec (08 for view modules, 09 for memory backends, 05 for protocol-level wraps).

**Tracing the DMA→ADC→network path end-to-end**: [08-views-and-ownership.md](08-views-and-ownership.md) §end-to-end trace. This walks one buffer from a DMA-half-complete interrupt all the way to an egress NIC, naming each layer's contribution.

---

## Promotion rule

A reference section is promoted from "draft" to "frozen for v1" when:

1. The corresponding plan-doc section is implemented and tested in the C reference.
2. A second-implementer review confirms the spec is sufficient to write an interoperable parser/sender/bridge from the spec alone (not from the C source).
3. The conformance test suite covers the section's behavior.

Until all three are satisfied, the reference doc is the operating reference for second-implementer questions.

The wire format does not version per-frame. v1 is committed once; future incompatible changes are versioned at the discovery layer (different mDNS service name, port, etc.). See [01-data-format.md](01-data-format.md) §versioning and compatibility.

---

## What this suite is NOT

- Not a C ABI specification. The reference C implementation's headers describe its ABI; this suite is language-agnostic.
- Not a build / packaging guide (see the `core/` rebuild).
- Not a feature comparison vs Zenoh / DDS / MQTT. See [../../README.md](../../README.md).
- Not a security architecture. The wire format is security-agnostic; security wraps it at the transport layer per [10-module-catalog.md](10-module-catalog.md).
- Not a roadmap. See the issue tracker.
