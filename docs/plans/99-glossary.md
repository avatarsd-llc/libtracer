# 99 — Glossary

> **Status**: draft, v0.1, 2026-05-03 — terms used across the libtracer doc set, with one-line definitions and pointers to the doc that defines each canonically.
> **Audience**: anyone reading any other doc and wanting a quick reminder.
> **Reading time**: ~5 min.

---

## A

**ACL (Access Control List)** — A LIST TLV (type `0x0A`) holding capabilities that gate read/write/subscribe access to a vertex. Defined in [doc 03](03-wire-format-and-data-model.md), enforced by `security_acl` ([doc 06](06-modules-executor-security-gui.md)).

**Address-shift slicing** — The mechanism that replaces wire-level fragmentation: a logically-large message is published as N TLVs to enumerated child paths `ep[0..N]` with the same timestamp. Subscribers reassemble by `(timestamp, index)`. Defined in [doc 04](04-graph-and-endpoint-api.md).

**ABI (Application Binary Interface)** — The C-level binary contract a libtracer module exports to the core. The `transport_vtable_t` and the `transport_meta_t.abi_version` field govern compatibility. See [doc 05](05-modules-transport-and-discovery.md).

## B

**Best-effort** — A `RELIABILITY` QoS value (default): publishers fire and forget; loss is silent. Contrast with `reliable`. See [doc 04](04-graph-and-endpoint-api.md).

**Bridge** — A vertex that re-publishes incoming TLVs from one transport onto the local graph. Core mechanism, not a module. Makes one address space across heterogeneous transports possible. See [doc 04](04-graph-and-endpoint-api.md) and [doc 05](05-modules-transport-and-discovery.md).

**Buffer chain** — A linked sequence of buffer-views over (possibly disjoint) backing memory. The substrate for the same-substrate insight: nested LIST TLVs are buffer chains. Defined in [doc 03](03-wire-format-and-data-model.md).

**Buffer view** — A struct `{owner, offset, length}` referring to a region of refcounted backing memory without owning it copy-style. The lightweight currency of the libtracer data model. See [doc 03](03-wire-format-and-data-model.md).

## C

**Callback (executor)** — A user-supplied function bound to a vertex via `executor_c` (or future `executor_micropython` etc.). Transforms or filters incoming TLVs. See [doc 06](06-modules-executor-security-gui.md).

**Capability** — An entry in an ACL: `{subject, permissions, expires_ns}`. Tokens granted to subjects by an authority (post-MVP).

**Coherency** — In libtracer, the property that timestamped samples carry the same wall-clock-aligned timestamp across the network. PTP-synced clocks deliver sub-µs coherency on supporting hardware. **Not** consensus or causal-consistency. See [doc 04](04-graph-and-endpoint-api.md) §coherency.

**Control plane vs data plane** — libtracer is a control plane (negotiates, names, dispatches metadata) for HPC/RDMA workloads. For LAN/MCU workloads, it is also the data plane (carries the bytes through TLVs). See [doc 00](00-vision-and-reality-check.md).

**Core** — The minimum libtracer build: buffer management + routing + endpoints + subscribers + bridge. Target ≤ 16 KB on Cortex-M3 with C23. See [doc 02](02-roadmap-weeks-1-to-8.md) week 4.

**CRC-32C (Castagnoli)** — The integrity check, polynomial `0x1EDC6F41`. Replaces the broken XOR-16 in the current code. Hardware-accelerated on x86 SSE 4.2 and ARMv8. See [doc 03](03-wire-format-and-data-model.md).

## D

**Deadline** — A QoS knob: the maximum time between writes before a liveness fault is raised. See [doc 04](04-graph-and-endpoint-api.md).

**Discovery** — How nodes find each other. Modular: `discovery_mdns` (LAN multicast), `discovery_static` (config file), `discovery_gossip` (post-MVP WAN). See [doc 05](05-modules-transport-and-discovery.md).

**Durability** — A QoS knob: `volatile` (default, no replay) or `transient-local` (most recent N samples replayed to late joiners).

## E

**Edge** — A subscription: a SUBSCRIBER TLV sitting in a vertex's `:subscribers[N]` slot. Drawn as a directed line in the graph from publisher to subscriber. See [doc 04](04-graph-and-endpoint-api.md).

**Endpoint (vertex)** — A named entity in the graph that data flows through. Has a path, exposes data via read/write, exposes control fields under `:`. See [doc 04](04-graph-and-endpoint-api.md).

**Executor** — A module that binds user logic (C, Python, Lua, WASM) to a vertex. See [doc 06](06-modules-executor-security-gui.md).

## F

**Field-write** — The mechanism for the control surface: subscribing, configuring QoS, etc. by writing TLVs to `:`-prefixed paths on a vertex. The load-bearing simplification vs DDS / Zenoh. See [doc 04](04-graph-and-endpoint-api.md) §subscription via field-write.

**Finite-pool mode** — A compile-time build option (`LIBTRACER_FINITE_POOL`) that replaces LEB128 length fields with fixed-width pool-class indices, sized to a preallocated buffer pool. For tiny MCUs. See [doc 03](03-wire-format-and-data-model.md).

## G

**Graph** — The collection of all vertices and their subscription edges, possibly spanning multiple hosts and transports via bridges. The user's mental model for what libtracer manages.

**GUI / `tracer-top`** — The CLI introspection tool (week 8 of [doc 02](02-roadmap-weeks-1-to-8.md)) and its post-MVP web sibling. See [doc 06](06-modules-executor-security-gui.md).

## H

**History (`history_keep_last`)** — A QoS knob: how many recent samples are retained per subscriber for `transient-local` durability. See [doc 04](04-graph-and-endpoint-api.md).

## I

**Iterative parser** — Mandatory pattern for parsing nested TLVs with an explicit work queue rather than recursion, to protect small MCU stacks. `TLV_MAX_DEPTH = 32`. See [doc 03](03-wire-format-and-data-model.md).

## L

**LEB128 (Little-Endian Base-128)** — Variable-width integer encoding (DWARF / Protobuf style); 7 data bits + 1 continuation bit per byte. The default `length` field encoding. See [doc 03](03-wire-format-and-data-model.md).

**`LIBTRACER_NO_ATOMIC`** — Build flag for single-threaded targets without LDREX/STREX (Cortex-M0). Refcount becomes plain `uint32_t`; cross-thread share is the caller's contract violation if attempted. See [doc 03](03-wire-format-and-data-model.md).

**LIST** — TLV type `0x05` whose payload is a sequence of nested TLVs. The graph-node-as-TLV mechanism. See [doc 03](03-wire-format-and-data-model.md).

**Liveness** — The mechanism for detecting that a publisher / subscriber / transport is still alive: heartbeats at `:liveness.heartbeat_hz`, observed at `:liveness.last_seen_ns`. See [doc 04](04-graph-and-endpoint-api.md).

## M

**Module** — An opt-in component loaded into a libtracer node: transports, discovery, executors, security. Each implements a small ABI and lives under `libtracer/modules/<name>/`. See [doc 05](05-modules-transport-and-discovery.md).

**MTU hint** — A transport-module-reported preferred maximum TLV size. Advisory; publishers MAY use it for address-shift slicing decisions but the transport `send` MUST handle larger TLVs.

## N

**NAME** — TLV type `0x02`: a UTF-8 path component. See [doc 03](03-wire-format-and-data-model.md).

**Node** — A running libtracer process. Has a name, a set of loaded transports / discovery / executors, and a local graph of vertices.

## O

**`opt`** — The header byte of a TLV, holding the bitfield: `VR | PL | TS | FP | CR | reserved`. See [doc 03](03-wire-format-and-data-model.md).

**Ownership transfer** — The mechanism for zero-copy delivery: the receive buffer's refcount is moved from the transport into the endpoint, no memcpy. See [doc 03](03-wire-format-and-data-model.md) §same-substrate insight.

## P

**Path** — A vertex's full address, like `/sensor/temp` or `/can-bridge/imu/accel:settings.reliability`. Hierarchical, `/`-separated, `:`-prefixed for fields. See [doc 04](04-graph-and-endpoint-api.md).

**POINT** — TLV type `0x07`: an endpoint definition record. See [doc 03](03-wire-format-and-data-model.md).

**PTP (Precision Time Protocol)** — IEEE 1588. Provides sub-µs clock sync on supporting hardware (STM32F7+, ESP32 with HW PTP, most server NICs). libtracer uses PTP-synced timestamps for coherency where available; degrades to NTP (~ms) otherwise. See [doc 04](04-graph-and-endpoint-api.md).

**Publisher / subscriber** — Roles, not types: any vertex can be both. A vertex publishes by being written to; it has subscribers if its `:subscribers[]` is non-empty.

## Q

**QoS (Quality of Service)** — Per-subscription configuration: `RELIABILITY`, `DURABILITY`, `HISTORY`, `DEADLINE`, `LIVELINESS`. Five of DDS's 22 policies. See [doc 04](04-graph-and-endpoint-api.md).

## R

**Refcount** — The atomic counter on a buffer segment that tracks how many views reference it. Atomic memory orderings spec'd in [doc 03](03-wire-format-and-data-model.md).

**Reliable (`RELIABILITY=reliable`)** — A QoS value: transport-dependent guarantee of delivery. Free on TCP/QUIC; requires app-layer ack/nack on UDP (post-MVP). See [doc 04](04-graph-and-endpoint-api.md).

**Router** — A vertex (TLV type `0x0D`) that re-dispatches TLVs. A bridge is a special router that re-publishes from one transport onto the local graph.

## S

**Same-substrate insight** — The architectural claim: a TLV in memory IS a graph node IS the wire bytes. No separate serialization layer. The load-bearing technical differentiator. See [doc 03](03-wire-format-and-data-model.md).

**Schema (`:schema`)** — A read-only field on every vertex: a LIST TLV describing the vertex's exposed fields and their types. The introspection root. See [doc 04](04-graph-and-endpoint-api.md).

**Segment** — Synonym for buffer view in this codebase, used in the C API (`segment_t`). Distinct from "vertex segment in path" — context disambiguates.

**SETTINGS** — TLV type `0x0B`: a LIST of named-value TLVs encoding QoS knobs. See [doc 03](03-wire-format-and-data-model.md).

**STATUS** — TLV type `0x09`: a LIST of ERROR + DESCRIPTION; empty payload = OK. The standard libtracer response/event for operational state.

**SUBSCRIBER** — TLV type `0x04`: a record with PATH (target) + SETTINGS + optional ACL. Written into `:subscribers[N]` to establish a subscription. See [doc 03](03-wire-format-and-data-model.md), [doc 04](04-graph-and-endpoint-api.md).

## T

**TIME** — TLV type `0x0C`: a u64 nanoseconds-since-Unix-epoch timestamp. See [doc 03](03-wire-format-and-data-model.md).

**TLV (Type-Length-Value)** — The fundamental libtracer data unit. 8-byte header (`type:u8, opt:u8, crc:u32, length:varint`) plus payload. See [doc 03](03-wire-format-and-data-model.md).

**Transport** — A module that moves TLVs across some medium (TCP, UDP, CAN, I2C, WS, SHM, …). Each implements the transport ABI. See [doc 05](05-modules-transport-and-discovery.md).

## V

**VALUE** — TLV type `0x01`: opaque application data. The most common payload type.

**Vertex** — Synonym for endpoint. The node in the graph that data flows through.

**Version bit (`VR`)** — Bit 7 of the `opt` byte. `0` = wire format v0.1; `1` = future major. Allows receivers to refuse incompatible traffic with a clear error. See [doc 03](03-wire-format-and-data-model.md).

## W

**WASM (WebAssembly)** — Sandboxed bytecode format. `executor_wasm` (post-MVP) loads `.wasm` modules as vertex callbacks; the only way libtracer plans to support code-over-the-wire safely. See [doc 06](06-modules-executor-security-gui.md).

**Wire format** — The byte layout of a TLV on the wire / in memory. v0.1 frozen at end of week 2 of [doc 02](02-roadmap-weeks-1-to-8.md). See [doc 03](03-wire-format-and-data-model.md).

## Z

**Zero-copy** — Delivery without memcpy: the receive buffer is refcount-shared between the transport, the router, and all subscribers via buffer-view structs. The libtracer goal on every transport, achieved via the same-substrate insight. See [doc 03](03-wire-format-and-data-model.md).

**Zenoh** — The closest competitor; libtracer's positioning is "the WireGuard to Zenoh's OpenVPN." See [doc 00](00-vision-and-reality-check.md), [doc 01](01-comparison-to-existing-protocols.md).

---

## What's NOT in this doc

- Detailed semantics — every term here points at the doc that defines it.
- Acronyms used only in one doc — those are expanded inline in their doc.
- Terms specific to a single module's internals — those live in module headers / READMEs once they exist.
