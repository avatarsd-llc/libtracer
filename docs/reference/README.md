# libtracer Protocol Reference (v0.1-draft)

> **Status**: draft suite. All sections written; promotion to "frozen" gated by the conformance milestone of [../plans/02-roadmap-weeks-1-to-8.md](../plans/02-roadmap-weeks-1-to-8.md).
> **Last revision**: 2026-05-03 — wire-format design review introduced fixed u32 length, trailer-positioned CRC + wire-time TS, four-layer model, and the explicit ROUTER shedding rule. The plans under [../plans/](../plans/) predate this revision and are now historical context for the design choices, not the byte-level spec.
> **Audience**: a second implementer writing an interoperable libtracer in any language, on any platform, without reading the C reference implementation.
> **Reading time**: full suite ~2 h.

---

## Purpose

This directory describes **the libtracer protocol as a standard**, independent of any implementation. The C reference implementation under [../../libtracer/](../../libtracer/) is one conforming implementation; a TypeScript core, a Rust core, a hardware FPGA implementation are all admissible if they conform to what is written here.

Planning documents under [../plans/](../plans/) — design rationale, roadmap, comparisons — explain *why* the protocol looks the way it does. Reference documents here describe *what it is*. When the two disagree, reference wins; planning docs are revised.

The reference covers:

1. **Data format** — the byte layout of every protocol-defined message on the wire.
2. **Structural aspects** — the graph model: vertices, edges, paths, naming, the same-substrate insight.
3. **Addressing** — path syntax, field-path resolution, wildcards, address-shift slicing.
4. **Execution flows** — read, write, await, subscribe, fan-out, bridge republish, deadline expiry, liveness, partition.
5. **Protocol-specific TLVs** — every TLV the protocol itself defines, byte-precise.
6. **User data packing** — how application data of any size (single byte → GB/s stream) lands in the graph; worked examples covering boolean, GPIO MMIO, structured records, ADC streaming, camera + LIDAR temporal sync, and the "shared variable" pattern.
7. **Host embedding** — per-host DAG ↔ global topology with cycles, dedup rules, bridge identity, "every host is a router."

---

## Section index

| File | Topic | Companion plan doc(s) |
| ---- | ---- | ---- |
| [00-overview.md](00-overview.md) | The standard in one document; conformance levels (L0–L3); module catalog index; portability to C++/Rust/Go; versioning. | [../plans/00-vision-and-reality-check.md](../plans/00-vision-and-reality-check.md) |
| [01-data-format.md](01-data-format.md) | TLV header, opt bits (VR/PL/TS/FP/CR/R), LEB128 + finite-pool length, CRC-32C, type-code registry, forward/backward compat. | [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) |
| [02-graph-model.md](02-graph-model.md) | Vertex / edge / path / view / segment definitions; the same-substrate insight (TLV in memory IS graph node IS wire bytes); refcount memory ordering; read=zero-copy / write=single-copy at medium boundary; schema discipline. | [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) + [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) |
| [03-addressing.md](03-addressing.md) | Path EBNF, field-chain resolution, atomic multi-field writes, wildcards, address-shift slicing rules, address scopes (local/bridged/global), canonicalization. | [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) |
| [04-communication-flows.md](04-communication-flows.md) | ASCII sequence diagrams for read, write+fanout, await, subscribe, unsubscribe, QoS update, bridge republish, address-shift fanout, deadline expiry, liveness loss, partition+recovery, schema discovery. | [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) |
| [05-protocol-tlvs.md](05-protocol-tlvs.md) | Per-TLV byte spec for `0x01`–`0x0D`: VALUE, NAME, DESCRIPTION, SUBSCRIBER, LIST, PATH, POINT, ERROR, STATUS, ACL, SETTINGS, TIME, ROUTER. Error code registry. Reserved-range policy. | [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) |
| [06-user-data-packing.md](06-user-data-packing.md) | Worked examples spanning eight orders of magnitude: 1-byte boolean, GPIO register as MMIO view, IMU record, 1 GB/s ADC streaming, camera+LIDAR temporal join, shared-variable pattern. Mix/split/concat invariants. | [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) + [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) |
| [07-host-embedding.md](07-host-embedding.md) | Per-host DAG (own vertices + bridge proxies); global topology (any shape, cycles allowed); cycle handling via `(origin_peer_id, origin_timestamp)` recent-set; bridge identity; embedding examples (RC car, robot, fleet, mesh, WAN). | [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) + [../plans/05-modules-transport-and-discovery.md](../plans/05-modules-transport-and-discovery.md) |

---

## Reading paths

**First-time reader**: 00 → 01 → 02 → 03 → 04 → 05 → 06 → 07.

**Writing a parser/sender in another language**: 01 → 03 → 05 → 06, then 02 once you optimize for zero-copy.

**Implementing a router or bridge**: 02 → 03 → 04 → 07 are mandatory; 06 is illustrative.

**Designing an application's data layout**: 06 → 03 → 02. Refer back to 05 for any TLV you handle.

**Auditing a deployment for cycles or routing storms**: 07 → 04 (bridge republish flow).

---

## Promotion rule

A reference section is promoted from "draft" to "frozen for v0.1" when:

1. The corresponding plan-doc section is implemented and tested in the C reference.
2. A second-implementer review confirms the spec is sufficient to write an interoperable parser/sender/bridge from the spec alone (not from the C source).
3. The conformance test suite (week 4 of [../plans/02-roadmap-weeks-1-to-8.md](../plans/02-roadmap-weeks-1-to-8.md)) covers the section's behavior.

Until all three are satisfied, the planning doc is the operating reference for active development; the reference doc is the operating reference for second-implementer questions.

Wire-format versioning follows the `opt.VR` bit (see [01-data-format.md](01-data-format.md)). v0.1 = draft, breakable. v1.0 = frozen, semver thereafter.

---

## What this suite is NOT

- Not a C ABI specification. The reference C core's headers describe its ABI; this suite is language-agnostic.
- Not a build / packaging guide. See [../plans/02-roadmap-weeks-1-to-8.md](../plans/02-roadmap-weeks-1-to-8.md).
- Not a feature comparison vs Zenoh / DDS / MQTT. See [../plans/01-comparison-to-existing-protocols.md](../plans/01-comparison-to-existing-protocols.md).
- Not a security architecture. The wire format is security-agnostic; security wraps it at the transport layer per [../plans/06-modules-executor-security-gui.md](../plans/06-modules-executor-security-gui.md).
- Not a roadmap. See [../plans/02-roadmap-weeks-1-to-8.md](../plans/02-roadmap-weeks-1-to-8.md).
