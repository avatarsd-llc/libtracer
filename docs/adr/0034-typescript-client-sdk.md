# TypeScript client SDK: ship a conservative, vector-pinned payload-builder + frame-I/O client; defer the path-addressed request envelope until the spec pins it

Status: proposed (the public-API surface of the browser/Node client SDK, **reference-impl / tooling** domain per [GOVERNANCE.md](../../.github/GOVERNANCE.md), so an ADR — not an RFC — governs it). The conservative, reversible foundation lands with this ADR as a `private`, experimental `0.0.0` package; every operation whose wire behavior the v1 spec does **not** pin is documented here as deferred, not implemented. Part of [#56](https://github.com/avatarsd-llc/libtracer/issues/56) (rides [#54](https://github.com/avatarsd-llc/libtracer/issues/54)).

## Context

[#56](https://github.com/avatarsd-llc/libtracer/issues/56) asks for the **TypeScript client SDK** that the web-UI of the originating production firmware (an ESP32-C6 smart-agriculture node) builds against: a native pure-TS client (strategy A, [ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md)) that a browser uses to bind directly into a robot's graph over WebSocket ([ADR-0031](0031-direct-browser-to-robot-binding-and-webtransport.md)), issuing the consumer-initiated subscribe-write of [ADR-0026](0026-consumer-initiated-subscription-client-write.md).

The TS workspace already has the two layers a client composes ([ADR-0033](0033-npm-subpackage-monorepo.md)):

- `@avatarsd-llc/libtracer` — the cross-validated wire **codec** (`encode`/`decode`/`equal`, the `TYPE` registry, `Tlv`), gated byte-for-byte by the shared vectors.
- `@avatarsd-llc/libtracer-ws` — `TransportWs`, a dial-out WebSocket connection exposing `send(bytes)` + `onFrame(cb)`, wire-compatible with the C++ `tr::net::transport_ws`.

A client SDK is the curated composition over those two that ADR-0033 §4 anticipated ("a profile becomes one small package over the stable subpath API … only on real demand"). #56 is that real demand.

### The load-bearing constraint: most client *operations* are not pinned by the wire spec yet

`read` / `write` / `await` and "subscribe via field-write" are described as **API-level** operations in [reference/04-communication-flows.md](../reference/04-communication-flows.md) — but against a **local** router. The **wire encoding of a path-addressed remote operation** — a frame that carries `(verb, destination vertex:field, payload)` to a peer over a shared connection — is **not pinned anywhere in v1**:

- [spec/v1.md](../spec/v1.md) §1 Scope, §2 Terminology, and **§3 Wire format are literally "(to be written)"**. The only normative wire content in v1 is §3.1 (the static **PATH TLV** byte layout) and §4 ("passes every test vector").
- The conformance vectors (`tests/conformance/vectors/v1/`) are all **single TLVs** (VALUE, NAME, DESCRIPTION, SUBSCRIBER, PATH, STATUS, SETTINGS, ROUTER, TIME, CRC framing). **There is no request-envelope vector** that bundles a verb + a destination `vertex:field` + a payload.
- The only frame↔graph mapping the C++ reference implements on the wire is the **bridge** (`core/src/bridge.cpp`): outbound = a subscribed vertex's VALUE wrapped in a ROUTER envelope (`export_vertex`); inbound = written to **one configured `mount` vertex** (`set_mount`). There is no per-frame "write to an arbitrary `vertex:field`" verb on the wire; destination addressing is the transport/mount binding (configured out-of-band) plus ROUTER `original_path` for sub-mount routing.

So a multiplexed, path-addressed client request protocol — the thing `read(path)`, `write(path, value)`, `await(path)`, and `subscribe(producerPath, …)` would each need to express on the wire — **does not exist in v1**. Inventing one here would be inventing protocol semantics the spec has not committed to, exactly what this delivery must not do.

### What *is* pinned (byte-exact, vector-gated)

1. **VALUE** TLV encoding — `value-bool-true`, `value-ll-u32`, `value-ts-abs`.
2. **SUBSCRIBER** TLV encoding — `subscriber-path` = `SUBSCRIBER{ PATH{ NAME… } }` (the subscribe-write *payload*).
3. **PATH** TLV encoding — `path-sensor-temp`, and the normative byte layout in spec/v1.md §3.1.
4. **Decoding any inbound frame** — every vector round-trips through the core codec.
5. **ROUTER** shed/unwrap — `router-wrapped`, mirroring `bridge_t::router_unwrap` (the bridge shedding rule, [reference/02](../reference/02-graph-model.md)).

## Decision

Ship a **payload-builder + frame-I/O client**: implement only the byte-products and frame plumbing the spec pins; defer every operation whose addressing/correlation envelope is unspecified, naming the gap.

### 1. `LibtracerClient` over an injectable transport seam

```ts
/** The minimal connection seam the client needs. `TransportWs` satisfies it structurally. */
export interface ClientTransport {
  send(frame: Uint8Array): void;
  onFrame(receiver: ((bytes: Uint8Array) => void) | null): void;
}

export class LibtracerClient {
  constructor(transport: ClientTransport);

  /** Decode every inbound frame; deliver decoded VALUE payloads (shedding one ROUTER wrapper). */
  onValue(handler: (value: Uint8Array, tlv: Tlv) => void): void;
  /** Inbound decode failures (CodecError) and other client errors land here, never thrown into the transport callback. */
  onError(handler: (err: Error) => void): void;

  /** PINNED: encode a VALUE TLV and emit it as ONE frame (the data-write payload). */
  write(value: Uint8Array, opts?: ValueOptions): void;
  /** PINNED: encode a SUBSCRIBER{PATH{…}} and emit it as ONE frame; register `handler` for inbound deliveries. */
  subscribe(targetPath: string[], handler: (value: Uint8Array, tlv: Tlv) => void): Subscription;

  /** Pure, side-effect-free builders — the exact byte-products, independently testable. */
  static encodeValue(value: Uint8Array, opts?: ValueOptions): Uint8Array;
  static encodePath(segments: string[]): Uint8Array;
  static encodeSubscriber(targetPath: string[], opts?: SubscriberOptions): Uint8Array;
}
```

The transport is **injected** (not constructed) so the client is browser/Node-agnostic and testable against an in-memory fake. `TransportWs` is the production transport; it is an **optional** peer (the seam is structural — the client never imports it).

### 2. Exact wire mapping for each method

| Method | Wire product | Pinned by |
| --- | --- | --- |
| `encodeValue(bytes, opts)` | `VALUE` TLV (`type=0x01`), opt per `opts` (LL/TS/CR) | `value-bool-true`, `value-ll-u32`, `value-ts-abs` |
| `encodePath(segs)` | `PATH` TLV (`type=0x06`, PL=1) of `NAME` children | `path-sensor-temp`, spec §3.1 |
| `encodeSubscriber(targetPath)` | `SUBSCRIBER` TLV (`type=0x04`, PL=1) wrapping a `PATH` | `subscriber-path` |
| `write(value)` | emits `encodeValue(value)` as one frame | VALUE vectors |
| `subscribe(targetPath, h)` | emits `encodeSubscriber(targetPath)` as one frame; registers `h` | `subscriber-path` + VALUE decode |
| inbound delivery | `decode(frame)`; if root is `ROUTER`, shed to the `NAME "data"` last child | `router-wrapped` + all vectors |

`subscribe`'s `targetPath` is the SUBSCRIBER's `target_path` child — *where the producer dispatches deliveries* ([reference/05](../reference/05-protocol-tlvs.md) §SUBSCRIBER), supplied as path **segments** (`["sensor","temp"]`) to avoid re-implementing the `:field`-plane string parser (itself part of the unspecified addressing). Each segment is validated (1..64 UTF-8 bytes, no reserved `/ : . [ ] * ?`) per [reference/03](../reference/03-addressing.md); a violation throws before any bytes are emitted.

### 3. Deferred operations (named, with the spec gap quoted)

These are **not** implemented, because v1 does not pin a wire envelope for a path-addressed, correlated request:

- **`write(path, value)` as a path-addressed remote write**, **`subscribe(producerPath, …)` as a subscribe-write into `producer:subscribers[]`**, **`read(path)`**, **`await(path)`**, and the **`connect(to=…)`** sugar ([ADR-0026](0026-consumer-initiated-subscription-client-write.md)) — all require a `(verb, destination vertex:field, payload[, correlation-id])` envelope. spec/v1.md §3 reads, verbatim: *"(to be written — see `docs/reference/` for the in-progress design notes)"*, and no conformance vector pins such a frame.
- **`read`/`await` additionally** need a request/response **correlation** mechanism over a shared connection, which v1 has not specified at all (the C++ side answers `read`/`await` against the **local** graph; cross-node `read` rides the bridge's last-value cache, not a wire request).

The client's `write(value)`/`subscribe(targetPath)` therefore carry only the **pinned payload TLV** as a frame; the producer they reach is the vertex the transport connection is bound to (the C++ mount/bridge model), not an in-band path. The path-addressed forms land once a maintainer-ratified request envelope (a future spec §3 / RFC) exists.

### 4. Package, versioning, dependencies

- New package **`@avatarsd-llc/libtracer-client`** at `bindings/typescript/packages/client/`, consistent with ADR-0033 §4 (a profile = one small package over the stable subpath API, minted on real demand).
- **`private: true`, version `0.0.0`, clearly "experimental"** — the surface is not ratified (the addressing envelope is deferred), so it MUST NOT be published or imply a stable public API, exactly as `transport-ws` started as a `private` scaffold.
- **`@avatarsd-llc/libtracer` is a `peerDependency`** (the cross-validated codec the consumer installs — ADR-0033 §3). `@avatarsd-llc/libtracer-ws` is an **optional** peer (structural seam; only needed for the production transport). ESM-only, `type: module`, `sideEffects: false`, `engines.node >= 18`.

### 5. Error model

- Inbound **decode failures** (`CodecError`, carrying `.code`) are routed to `onError`, never thrown back into the transport's `onFrame` callback (a throw there would cross the socket-read boundary).
- **Connection loss** stays the transport's concern: `TransportWs` rejects `connect()` on early close and throws on `send` before `connect`. The client adds no reconnect logic in this slice (auto-reconnect + subscribe-re-issue is the transport/binding layer's job — #54 / ADR-0026).

## Considered options

- **Implement full `read`/`write(path)`/`await`/`subscribe(path)` now, designing a request envelope.** Rejected: it invents wire semantics v1 has not committed to (spec §3 unwritten, no vector), would not cross-match the C++ core, and would bake an irreversible public API around a guess.
- **Extend `@avatarsd-llc/libtracer-ws` with the client instead of a new package.** Rejected: it would couple the codec-level transport to a higher-level client surface and to the codec as a hard dep; ADR-0033 keeps the transport a thin codec-validated layer and expresses compositions as their own small packages.
- **Fold the client into the core package.** Rejected: a browser bundle that only decodes frames must not pull a client/transport surface (ADR-0033 §2 — the core has no transport dependency).
- **Publish as a stable `0.1+` public API.** Rejected: the deferred addressing envelope means parts of the obvious surface (`read`/`write(path)`/`await`) are absent; a stable version would imply they exist. `private 0.0.0` is honest and reversible.

## Consequences

- The web-UI gets, **today and byte-exactly**: construction of the subscribe-write payload (SUBSCRIBER) and the data-write payload (VALUE), live decoding of inbound VALUE deliveries (ROUTER-shed), over the real `TransportWs` or any injected seam — all vector-pinned and mock-testable with no live peer.
- **The under-specified area is surfaced, not papered over:** the path-addressed request envelope (verb + `vertex:field` + correlation) is a maintainer decision (a future spec §3 / RFC). The client's API is shaped so those operations slot in **additively** when the envelope lands — no redesign.
- **Reversible:** the package is `private 0.0.0`; nothing here is published or load-bearing for other packages until the surface is ratified.
- A live C++↔TS subscribe interop is **deferred**: the existing `ws_interop_server` only echoes; a graph-backed node-over-WS fixture is a follow-up (tracked under #54/#56).

## Relates

- [#56](https://github.com/avatarsd-llc/libtracer/issues/56) — the TS client SDK; [#54](https://github.com/avatarsd-llc/libtracer/issues/54) — `transport_ws` (the connection this rides).
- [ADR-0033](0033-npm-subpackage-monorepo.md) — the workspace/package model (core peerDep, profiles as small packages).
- [ADR-0026](0026-consumer-initiated-subscription-client-write.md) — consumer-initiated subscribe-write (`connect` sugar); [ADR-0031](0031-direct-browser-to-robot-binding-and-webtransport.md) — the browser↔robot target use case.
- [ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md) — the cross-match that makes the SUBSCRIBER/VALUE bytes safe across cores.
- [reference/04](../reference/04-communication-flows.md) (the API-level flows), [reference/05](../reference/05-protocol-tlvs.md) (per-TLV bytes), [spec/v1.md](../spec/v1.md) §3 (the unwritten wire-format the deferred operations wait on).
