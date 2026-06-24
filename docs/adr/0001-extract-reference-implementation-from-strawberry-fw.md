# 1. Extract the reference implementation from strawberry-fw's `io_layer`

- **Date:** 2026-06-24
- **Status:** Proposed
- **Deciders:** Avatar LLC (spec + reference-impl domains, see [GOVERNANCE.md](../../GOVERNANCE.md))
- **Related:** [docs/plans/00-vision-and-reality-check.md](../plans/00-vision-and-reality-check.md),
  [docs/plans/02-roadmap-weeks-1-to-8.md](../plans/02-roadmap-weeks-1-to-8.md),
  [docs/reference/02-graph-model.md](../reference/02-graph-model.md),
  [docs/reference/08-views-and-ownership.md](../reference/08-views-and-ownership.md)

## Context

libtracer today is **specification-rich and implementation-poor**. The six-layer model
(L0 memory → L1 views/ownership → L2 frame → L3 TLV → L4 graph endpoint → L5 application),
the graph/TLV same-substrate model, static path handles, and the zero-copy view/segment
substrate are all described in depth under `docs/reference/` and `docs/spec/v1.md`. The code
in `core/include/libtracer/` is ~528 LOC of header stubs that (a) do not compile, and (b)
encode a **v0.0 wire shape that contradicts the v0.1 spec** on ~14 points (8-byte in-header
CRC vs trailer CRC-32C, retired `LIST` type, wrong `opt` bitfield, exceptions despite the
no-exceptions commitment). 47 modules are catalogued in
[docs/reference/10-module-catalog.md](../reference/10-module-catalog.md); **zero** are
implemented. The greenfield 8-week roadmap assumes building L0–L4 from scratch.

In parallel, **`avatarsd-llc/strawberry-fw`** (the Strawberry 1170 / Gorshok-v4 grow
controller, ESP-IDF, ~111 K LOC first-party, 70 ADRs) has — independently and without
reference to libtracer — **already built and shipped most of libtracer's core on real
hardware**. A read-only architecture analysis of both repos (2026-06-24) found this
correspondence:

| libtracer concept | strawberry-fw equivalent | status |
| --- | --- | --- |
| Vertex (addressable point) | `io_layer` endpoint + `io_descriptor_t` | shipping |
| Path (canonical identity) | dotted-path string id → `io_handle_t` (16-bit generational) | shipping (string, not PATH-TLV) |
| `read` / `write` / `await` | `io_get_last` / `io_set_*` / `io_subscribe_*` | shipping |
| VALUE + TIME TLV | `io_sample_t` / `iov_t` = value + `ts_ns` + valid | shipping (struct, not bytes) |
| Subscription edge | `io_subscribe_*`, `IO_NOTIFY_ON_CHANGE` | shipping |
| Bridge / ROUTER | `can_io_bridge` (TX/RX mirror over CAN) | shipping on CAN |
| Discovery / ANNOUNCE | `can_sys` ANNOUNCE catalog; `mb`/`ow` scanners | shipping |
| Transport-neutral egress | `egress_scheduler` (strawberry ADR-0056) | shipping (WS) |
| L0 memory substrate | static `.bss` + `heap_governor` + `osal_ring` | shipping |
| Multi-target platform | `components/platform/` (ESP32 / STM32H563 / POSIX) | shipping |
| **L1 zero-copy views** | — `io_sample_t` **copies** scalars/blobs | **gap** |
| **L2 unified wire format** | per-transport: CAN scalar **or** protobuf **or** direct | **gap** |
| Static path handles in `.rodata` | runtime string→handle resolution | partial |

strawberry-fw's own ADRs name the same ideas libtracer's reference docs do: ADR-0006
("the path is the identity"), ADR-0057 ("a unified typed Value seam — the wire carries
type + ts"), ADR-0019/0056 (the CAN domain bus and a transport-neutral egress scheduler),
ADR-0020 (the ESP32/STM32/POSIX platform split). Critically, strawberry is **already a
decentralised graph across two MCUs**: an ESP32 owns the logic and the `io_layer`, an STM32
satellite is a real-time I/O gateway, and `can_io_bridge` mirrors endpoints between them over
a 29-bit CAN domain bus — i.e. libtracer's bridge model, in production.

What strawberry does **not** have is the thing libtracer specifies: **one transport-agnostic
wire format**. Today its horizontal links (CAN, Modbus RTU, OneWire) and its vertical link
(protobuf-over-WebSocket to cloud/CLI) are three separate codecs bolted onto one
already-graph-shaped core (`io_layer`).

## Decision

**Invert the build strategy. Extract libtracer's reference implementation from strawberry-fw's
proven `io_layer` + `can_io_bridge`, instead of authoring it greenfield — and use the
extraction to collapse strawberry's three transport codecs into a single graph wire format.**
Freeze wire format v1 from code that runs on hardware, not from the spec in the abstract.

Concretely, four gaps close the distance between the shipping `io_layer` and libtracer:

1. **One wire format.** Replace `can_io_bridge`'s ad-hoc 29-bit scalar encoding and the
   protobuf framing with libtracer TLV (`PATH` + `VALUE` + `TIME`), one codec under every link.
2. **Path = identity.** Promote the existing hierarchical dotted-path id (node/unit/device/leaf)
   to the canonical `PATH` TLV, and add static path handles for the hot path.
3. **Zero-copy L1.** Introduce the view/segment substrate
   ([reference/08](../reference/08-views-and-ownership.md)) for the blob/array/video path,
   where `io_sample_t` currently copies — this is the high-bandwidth-sensor requirement.
4. **Unify the bridge.** `can_io_bridge` becomes a libtracer ROUTER; ESP32↔STM32 CAN, the
   cloud WS link, and Modbus all become libtracer *transports* behind one bridge + discovery.

Sequenced ship-first, each phase with a checkpoint:

- **Phase 0 — Workspace.** Side-by-side full-history clones of both repos under
  `~/usr-prj/strawberry-tracer/`.
- **Phase 1 — Reconcile code to spec.** Make `core/` match `docs/spec/v1.md` (trailer CRC-32C,
  correct `opt` bits, retire `LIST`, no-RTTI/no-exceptions), compile, pass a single-TLV host
  round-trip. *Checkpoint: `core/` builds + round-trips.*
- **Phase 2 — Host_test adapter spike.** libtracer TLV as an alternative encoding for `io_layer`
  values, behind strawberry's existing transport seam; **byte-exact** round-trip via the
  `components/proto/host_test/contract_lock` pattern. *Checkpoint: golden-vector green on POSIX.*
- **Phase 3 — Collapse `can_io_bridge` onto libtracer.** CAN TX/RX mirroring as
  `PATH`+`VALUE`+`TIME` over `can_bus`; ANNOUNCE → libtracer discovery. Benchmark on real CAN
  (ESP32↔STM32): latency, bytes/frame, throughput. *Checkpoint (strategic gate): libtracer ≥
  the bespoke scheme on footprint **and** latency, or the static-graph thesis is reworked
  before going wider.*
- **Phase 4 — Extract canonical core.** The proven `io_layer`+bridge code becomes the reference
  implementation; **freeze wire format v1 from it**; backfill `tests/conformance/` from captured
  live CAN traffic.
- **Phase 5 — Second transport + adopter.** Bring the cloud WS link onto libtracer, then the
  high-bandwidth/video path via the L1 view model.

## Consequences

**Positive**
- libtracer gets its first real implementation **and** its first adopter at once, breaking the
  protocol chicken-and-egg problem (a protocol with zero implementers has near-zero value).
- The wire format is frozen from code proven on hardware, the central commitment of
  [docs/plans/00-vision-and-reality-check.md](../plans/00-vision-and-reality-check.md).
- strawberry gains a unified "horizontal + vertical → graph" comms substrate, retiring two
  bespoke codecs.
- The static-graph / static-polymorphism thesis is validated (or falsified) on a real CAN link
  **before** broad investment — Phase 3 is an explicit go/no-go gate.

**Negative / risks**
- strawberry-fw becomes the de-facto spec author → bus-factor and single-product bias; the spec
  must stay general enough for a genuine second implementer (the conformance gate in Phase 4).
- Refactoring a shipping firmware's comms core is invasive. Mitigation: all work lands behind the
  existing `components/platform/transport` seam and is gated on the real-CAN benchmark; nothing
  ships until it matches the current scheme.
- Zero-copy L1 (gap #3) is genuinely new engineering, not extraction — the riskiest piece.
- `core/` must first be reconciled to its own v0.1 spec (Phase 1) before extraction can target it.

**Relationship to existing docs**
- Complements the "reality check" in
  [docs/plans/00-vision-and-reality-check.md](../plans/00-vision-and-reality-check.md).
- Supersedes the *greenfield* framing of
  [docs/plans/02-roadmap-weeks-1-to-8.md](../plans/02-roadmap-weeks-1-to-8.md): the L0–L4 work is
  sourced from strawberry-fw's `io_layer` rather than written from scratch.
- This is the **first ADR** in the repo; it establishes `docs/adr/` per
  [CLAUDE.md](../../CLAUDE.md).
