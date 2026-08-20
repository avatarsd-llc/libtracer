# Start here — route by intent

Every other entry surface on this site is organised by **what a document is**: the
normative specification, the descriptive reference suite, the C++ API reference, the
worked examples. This page is organised by **what a reader is trying to do**. Its only
job is to hand you one first page and the short ordered route after it.

> **Audience**: a reader who has decided libtracer is possibly relevant and now needs
> the one page that answers their question. This page explains nothing twice — every
> row below is a pointer, and where a lane rests on a commitment, the document that
> records that commitment is named rather than paraphrased.

Two rules govern it:

- **One intent, one first page.** Where two lanes share a destination, the lane says
  what to read it *for*; the destination is never re-explained here.
- **Nothing here is normative.** When this page and [the
  specification](spec/v1.md) disagree, the specification wins — as it does against
  every other informative document ([spec index](spec/index.md) §what is normative).

---

## Pick the lane

| I want to… | Start at | Then | Target scale |
| --- | --- | --- | --- |
| Move data between two boards | [Getting started §5](getting-started.md) | [lane 1](#lane-1--move-data-between-two-boards) | NARROW → MID |
| Let a browser see robot state | [Reference 16 — WebSocket session authentication](reference/16-websocket-session-auth.md) | [lane 2](#lane-2--let-a-browser-see-robot-state) | MID |
| Implement the wire protocol myself | [The specification](spec/index.md) | [lane 3](#lane-3--implement-the-wire-protocol-myself) | any |
| Embed a node on an MCU | [Reference 12 — deployment profiles](reference/12-deployment-profiles.md) | [lane 4](#lane-4--embed-a-node-on-an-mcu) | NARROW |
| Decide whether it fits at all | [Performance & conformance](performance.md) | [other doors](#other-doors) | any |
| Build a device other vendors integrate with | [Interoperability](interoperability.md) | [other doors](#other-doors) | NARROW → WIDE |
| Extend or embed the C++ core in a host application | [C++ API reference](modules/index.md) | [other doors](#other-doors) | MID → WIDE |
| Understand the model before choosing any of the above | [Reference 00 — core overview](reference/00-overview.md) | the reference suite's own [reading paths](reference/README.md) | any |

**Target scale** is the deployment the lane is written for, and that is the *only* thing
these three words mean: **NARROW** = a constrained node with tens of KB of RAM, **MID** =
a gateway or SBC process, **WIDE** = a many-core host — the spectrum
[ADR-0082](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0082-auth-subject-and-peer-named-are-decoupled-claims-default-stays-false.md)
§reason 2 draws on when it calls the single-upstream MCU "the common NARROW deployment".
They once doubled as the names of the three allocation-store compositions in
[ADR-0079](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md)
— with the mapping *inverted*, so that reading collided with this one; that spelling is
retired (ADR-0079's 2026-08-20 amendment) and the compositions are now **folded /
per-plane / per-thread**. Target scale is a reading hint, not a
conformance statement — the protocol is one model at every scale
([Reference 12](reference/12-deployment-profiles.md), opening).

---

## Lane 1 — Move data between two boards

Two nodes, one link, a value produced on one and observed on the other.

1. **[Getting started §5 — two nodes over a wire](getting-started.md)** — the build,
   then the smallest end-to-end exchange.
2. **[Two nodes over a wire — FWD delivery](examples/two-node-fwd.md)** — the same
   thing as a program CI builds and runs on every change.
3. **[Reference 07 — host embedding](reference/07-host-embedding.md)** — what a route
   *is*. A remote endpoint is addressed by its full path from your own root, walking
   through transport vertices; each hop strips its whole mount run and forwards the
   rest ([Reference 00](reference/00-overview.md) §conformance, item 6).
4. **[Reference 13 — network formation](reference/13-network-formation.md)** — who
   issues the writes that create the link and bind the flow, and why the party that
   issues them can then leave.
5. **[Transport module](modules/transport.md)** and
   **[connection configuration](modules/connection-config.md)** — the reference
   implementation's own surface and its config keys.

What the lane asks you to accept before the API makes sense:

- There is no `connect` and no `subscribe` primitive. The data API is `read` / `write`
  / `await`; subscribing is a write into the producer's `:subscribers[]`
  ([ADR-0006](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0006-read-write-await-api-no-connect.md),
  [ADR-0026](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0026-consumer-initiated-subscription-client-write.md)).
- A connection is a **vertex**, created by an ordinary write to a creator endpoint —
  not by a transport-specific call
  ([ADR-0027](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0027-transport-and-connections-are-vertices.md),
  [ADR-0059](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md),
  [RFC-0014](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)).
- Routing is explicit source-routing, so a peer reachable two ways is **two addresses**
  — there is no automatic multipath or failover
  ([ADR-0040](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0040-net-plane-is-explicit-source-routed-only.md)).

Not yet written as one article: the single narrative that joins path-as-route,
mounting and network composition. It is article 1 of
[#1382](https://github.com/avatarsd-llc/libtracer/issues/1382); until it lands the
story is distributed across reference 03, 07 and 13.

---

## Lane 2 — Let a browser see robot state

A browser tab is an ordinary peer, not a client tier
([CONTEXT.md](../CONTEXT.md) §peer / peer symmetry).

1. **[Reference 16 — WebSocket session authentication](reference/16-websocket-session-auth.md)**
   — start here rather than at a transport page, because the browser constraint is the
   thing that shapes the design: a browser cannot present a header on the opening GET,
   and the credential must not ride the URL.
2. **[Reference 13 — network formation](reference/13-network-formation.md)** — the web
   UI joins as an ephemeral peer with delegated admin, creates and binds, then departs;
   the devices keep talking. Read it for what the UI must *not* do: it must not proxy
   the data.
3. **[TypeScript binding](https://github.com/avatarsd-llc/libtracer/tree/main/bindings/typescript)**
   (see [`bindings/README.md`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/README.md))
   — a native TypeScript codec plus a client SDK and WebSocket / WebTransport
   transports. All four npm packages are published; the client SDK describes itself as
   experimental, which is a surface-stability caveat
   ([ADR-0034](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0034-typescript-client-sdk.md)
   erratum).
4. **[Capability matrix](capability-matrix.md)** — what the TypeScript core actually
   covers today, generated from CI evidence rather than hand-maintained.

Rationale worth reading once, in the repository: direct browser-to-robot binding and
WebTransport
([ADR-0031](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0031-direct-browser-to-robot-binding-and-webtransport.md)),
and why WebSocket is the first transport with QUIC deferred per link
([ADR-0029](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0029-websocket-first-transport-quic-deferred-per-link.md)).

The bindings have no home in this doc tree yet — their READMEs live in the repository.
That is article 3 of [#1382](https://github.com/avatarsd-llc/libtracer/issues/1382).

---

## Lane 3 — Implement the wire protocol myself

A second implementation, in any language, that interoperates byte-for-byte.

1. **[The specification](spec/index.md)** — read this *before* `v1.md`. Normative
   status is not a property of a directory: `v1.md` incorporates three reference
   documents as normative annexes, and a reader of `v1.md` alone never sees the frame
   layout.
2. **[Protocol v1](spec/v1.md)** — scope, terminology, the conformance procedure, the
   static path-handle requirements (§3.1).
3. The three annexes, in this order:
   **[01 — data format](reference/01-data-format.md)** (frame layout, the `opt` bits,
   the trailer), **[05 — protocol-defined TLVs](reference/05-protocol-tlvs.md)** (the
   type-code registry and each payload layout),
   **[03 — addressing](reference/03-addressing.md)** §path syntax.
4. **[Conformance vectors](https://github.com/avatarsd-llc/libtracer/tree/main/tests/conformance/)**
   — the language-agnostic test vectors every core is gated against; the C++ core is
   golden
   ([ADR-0028](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md)).
5. **[Capability matrix](capability-matrix.md)** — how far the Rust and TypeScript
   cores have got, so you know which vector categories are already exercised.
6. **[Implementation registry](implementations.md)** — where to list yours.

Two things to know before you start:

- The wire format is **draft** and not yet stable; pin to a specific commit if you
  depend on it today — the specification says so on its own first line
  ([Protocol v1](spec/v1.md) §status).
- If you are writing only a codec or a sender, the reference suite has a
  section-granular route for exactly that — 01 → 03 → 05 → 06 — in its own
  [reading paths](reference/README.md). This page does not restate it.

---

## Lane 4 — Embed a node on an MCU

NARROW: a constrained node where flash, static RAM and hot-path allocation all bind.

1. **[Reference 12 — deployment profiles](reference/12-deployment-profiles.md)** —
   rung 1, the single-transport leaf: the exact module tree, its conformance profile,
   and header-elided framing, where the bus's own identity *is* the path
   ([ADR-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0022-transport-framing-modes-elided-full-tlv-advertise.md)).
   Its rung-1 flash figure is stated there as a design target for that composition, not
   as a measured figure — read it as such.
2. **[Protocol v1](spec/v1.md) §3.1** and
   **[Reference 03 — addressing](reference/03-addressing.md)** — path handles. Every
   address used more than once is encoded once, into a build-time literal or one
   init-time allocation, and the hot path takes the handle. This is what removes
   `snprintf` and per-write allocation, and it is a conformance requirement, not an
   optimisation ([Reference 00](reference/00-overview.md) §conformance, item 7).
   The reference suite's [reading paths](reference/README.md) name a five-section MCU
   route at section granularity; follow it there.
3. **[ESP32 node profile](interop/esp32-production-node.md)** — running libtracer as a
   primary communication stack inside an MCU's RAM, flash and task budget, and the
   sites where the bounded-reactor discipline is not yet complete.
4. **[ESP-IDF component](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/esp-idf)**
   and **[PlatformIO](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/platformio)**
   — consume a packaged integration rather than building the core directly
   ([Getting started §2](getting-started.md)).
5. **[The configuration space](design/config/00-configuration-space.md)** — the size
   and policy axes that decide static RAM. They are a different kind of knob from the
   module set and are not visible on the integrator's CMake line.

The footprint claim is gated, not asserted: a stripped Cortex-M0 build is measured by
[`footprint-cortexm0.yml`](https://github.com/avatarsd-llc/libtracer/blob/main/.github/workflows/footprint-cortexm0.yml)
on every change under `core/`, and its budget verdict currently runs in **warn** mode —
so take the size from that job's own output rather than from a figure quoted elsewhere.

---

## Other doors

| I want to… | Read | Why this one |
| --- | --- | --- |
| Decide whether it fits at all | [Performance & conformance](performance.md), then the [capability matrix](capability-matrix.md) | The performance page is generated from the live harnesses in CI, and the matrix from test evidence — neither is hand-authored, so neither can drift from what ran. |
| Build a device other vendors integrate with | [Interoperability](interoperability.md), then [building a custom interoperable device](interop/custom-device.md) | The first says why interop here is a legibility discipline with no certification body; the second is the can / may / must-not matrix for your own device. |
| Extend or embed the C++ core in a host application | [C++ API reference](modules/index.md), then [Reference 10 — module catalog](reference/10-module-catalog.md) | The module pages pair a usage narrative with declarations rendered from the headers; the catalog says which module pairs with which, and what is required versus optional. |
| Know what one word means | [CONTEXT.md](../CONTEXT.md) | The canonical glossary, and the vocabulary of record: where it and another page disagree, the other page is brought into line with it. |
| Understand why a design looks the way it does | [ADRs](https://github.com/avatarsd-llc/libtracer/tree/main/docs/adr/) and [RFCs](https://github.com/avatarsd-llc/libtracer/tree/main/docs/spec/rfcs/) in the repository | Rationale and change proposals are contributor instruments and are deliberately not published on this site ([spec index](spec/index.md) §how the specification is layered). |
| Measure the reference implementation's own cost | [Design notes](design/README.md) | Explicitly not the standard: the measured cost of the C++23 implementation's choices, and how cost measurement goes wrong here. |

---

## Which genre answers which question

A destination is easier to trust once you know which genre it belongs to. Precedence
runs top to bottom.

| Genre | Where | Answers | Status |
| --- | --- | --- | --- |
| Normative specification | [`docs/spec/v1.md`](spec/v1.md) + its three annexes | What a conforming implementation MUST do | Wins against every other document |
| Descriptive reference | [`docs/reference/`](reference/README.md) | What the protocol *is*, implementation-independently | Canonical for that question; draft for v1 |
| API reference | [`docs/modules/`](modules/index.md) | What the C++23 reference implementation exposes | One implementation, not the standard |
| Measured evidence | [Performance](performance.md), [capability matrix](capability-matrix.md), [design notes](design/README.md) | What it costs, and what is verified | Generated from CI runs |
| Rationale and proposals | ADRs and RFCs, in the repository | Why it looks this way; what is being changed | Not published on this site |

---

## How this page relates to the other entry points

This page routes; it does not replace. The [site landing page](../index.md) still
carries its six orientation cards, [Getting started](getting-started.md) still owns the
ten-minute build, and the [reference suite's reading guide](reference/README.md) still
owns the section-granular routes through the standard. Where their coverage overlaps
this one, the overlap is recorded on
[#1382](https://github.com/avatarsd-llc/libtracer/issues/1382); folding any of them in
is a separate, later change, so nothing was removed to make room for this page.
