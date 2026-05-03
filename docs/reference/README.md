# libtracer Protocol Reference (v0.1-draft)

> **Status**: scaffold. Each section is a stub pointing at the corresponding planning doc under [../plans/](../plans/). Content lands once the section is frozen by an implementation milestone (see [../plans/02-roadmap-weeks-1-to-8.md](../plans/02-roadmap-weeks-1-to-8.md)).
> **Audience**: a second implementer writing an interoperable libtracer in any language, on any platform, without reading the C reference implementation.
> **Reading time**: full suite ~2 h once filled.

---

## Purpose

This directory describes **the libtracer protocol as a standard**, independent of any implementation. The C reference implementation under [../../libtracer/](../../libtracer/) is one conforming implementation; a TypeScript core, a Rust core, a hardware FPGA implementation are all admissible if they conform to what is written here.

Planning documents under [../plans/](../plans/) — design rationale, roadmap, comparisons — explain *why* the protocol looks the way it does. Reference documents here describe *what it is*. When the two disagree, reference wins; planning docs are revised.

The reference covers, per the user's framing:

1. **Data format** — the byte layout of every protocol-defined message on the wire.
2. **Structural aspects** — the graph model: vertices, edges, paths, naming, addressing.
3. **Execution flows of communication** — read, write, await, subscribe, fan-out, bridge republish, address-shift slicing, failure handling.
4. **Subscription mechanics** — the field-write subscription model, per-subscriber state, QoS interaction, liveness.
5. **Protocol-specific TLV contents** — every TLV the protocol itself defines (`STATUS`, `SUBSCRIBER`, `SETTINGS`, `ACL`, `TIME`, `ROUTER`, `LIST`, `PATH`, etc.) with byte-precise layout.
6. **How user data packs into the graph** — LIST nesting, address-shift across `ep[N]`, ownership-transfer semantics at delivery.
7. **How a host's local graph embeds into the larger system** — per-host DAG view, global topology that can include cycles, dedup rules, bridge identity, the rule that "every host is a router."

---

## Section index

| File | Topic | Source plan doc |
| ---- | ---- | ---- |
| [00-overview.md](00-overview.md) | What the protocol is in one page; conformance levels; versioning. | [../plans/00-vision-and-reality-check.md](../plans/00-vision-and-reality-check.md) |
| [01-data-format.md](01-data-format.md) | TLV byte layout, header, opt bits, varint, finite-pool mode, CRC-32C, type code registry. | [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) |
| [02-graph-model.md](02-graph-model.md) | Vertex / edge / path semantics, naming rules, wildcards. | [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) |
| [03-addressing.md](03-addressing.md) | Path syntax, field paths, address-shift slicing, host-local vs global address scopes. | [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) |
| [04-communication-flows.md](04-communication-flows.md) | Sequence diagrams for read, write, await, subscribe, fanout, bridge republish, deadline expiry, partition. | [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) |
| [05-protocol-tlvs.md](05-protocol-tlvs.md) | Byte-precise spec for every protocol-defined TLV in the reserved range (`0x00–0x7F`). | [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) |
| [06-user-data-packing.md](06-user-data-packing.md) | How application payloads become graph nodes: LIST nesting, address-shift across `ep[N]`, ownership transfer, view trees. | [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) |
| [07-host-embedding.md](07-host-embedding.md) | Per-host DAG view, global topology (any shape, including cycles), routing dedup, bridge identity, "every host is a router." | [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) + [../plans/05-modules-transport-and-discovery.md](../plans/05-modules-transport-and-discovery.md) |

---

## Promotion rule

A reference section is promoted from "stub" to "frozen" when the corresponding plan-doc section is implemented and tested in the C reference, AND a second-implementer review confirms the spec is sufficient to write an interoperable parser/sender from the spec alone (not from the C source). Until promoted, the planning doc is canonical.

Versioning: the protocol version follows the wire-format version (see [01-data-format.md](01-data-format.md), `opt.VR` bit). v0.1 = draft, breakable. v1.0 = frozen wire format, semver thereafter.
