# Changelog — libtracer TypeScript packages

All notable changes to the npm packages under `bindings/typescript/`. The core
package follows [Semantic Versioning](https://semver.org/); see
[ADR-0033](../../docs/adr/0033-npm-subpackage-monorepo.md) for the
versioning/publish strategy.

## Unreleased

### Added

- **`LibtracerClient.writeField(path, selector, valueTLV)` in
  `@avatarsd-llc/libtracer-client` (#408)** — the write counterpart of
  `readField`, emitting `FWD{op=WRITE, dst, field, src, payload}`
  (vector-pinned by `fwd-write-subscriber-field`). `write()` takes no
  selector, so **every field-addressed write — the whole in-band formation
  plane — was previously out of reach from this client**: neither
  `:children[]` vertex creation (ADR-0017 / ADR-0027) nor a
  `:subscribers[]` subscribe-write (ADR-0026) could be expressed at all.
- **`encodeConnSpec(options)` in `@avatarsd-llc/libtracer-client` (#408,
  ADR-0027)** — builds the connection-creation `SPEC` TLV that the in-band
  `write /net:children[] += SPEC{…}` carries: the formation write a third
  party (typically a web UI holding delegated admin) issues on a device to
  bring a transport link up, leaving two devices talking with nothing in the
  data path (reference/13 §2). Covers `type`/`name`/`role`/`port`/`kind`/`addr`
  plus the ws-private `peerNamed`/`maxPeers` keys (ADR-0043 §5, ADR-0044).
  **Byte-pinned against the C++ emitter** in `test/conn-spec.test.mjs` — the
  SPEC is now built independently by two encoders and a device only forms the
  link if they agree exactly, so the vectors are the real C++ output.

## [0.4.0] — 2026-07-09

### Removed

- **BREAKING — `MAX_DEPTH` is removed from `@avatarsd-llc/libtracer` (RFC-0006).**
  Nesting depth is receiver-resource-bounded, never a constant: `decode()`'s
  explicit work stack is a growable array, so the depth-cap reject is gone.
  `ERROR.TLV_NESTING_TOO_DEEP` remains registered ("exceeds the receiver's
  decode resources") for harness parity and peers' ERROR frames; `decode()`
  itself no longer throws it.

## [0.3.0] — 2026-07-07

### Added

- **New package `@avatarsd-llc/libtracer-webtransport` — the browser
  WebTransport transport (ADR-0043 Phase B / ADR-0031, #92).**
  `TransportWebTransport` drives the runtime's `WebTransport` (HTTP/3 extended
  CONNECT), opens ONE bidirectional stream as the frame channel, and carries
  each libtracer TLV as a `u32-LE length ++ frame` record — wire-compatible
  with the C++ `tr::net::webtransport_transport_t` (`libtracer_quic` module).
  Satisfies the client SDK's `ClientTransport` seam structurally
  (send/onFrame/onClose), exactly like `TransportWs`. Dev trust via
  `serverCertificateHashes` (ECDSA cert valid <= 14 days — the browser rule;
  see the package README). The pure length-prefix codec is exported at the
  `./framing` subpath (`encodeRecord`, `FrameReassembler`,
  `MalformedPrefixError`, `MAX_FRAME`). Node has no native WebTransport
  client, so unit tests run over a mocked session (web streams); a
  puppeteer/chrome-headless interop harness against the C++
  `wt_interop_server` echo binary skips gracefully unless
  `LIBTRACER_WT_INTEROP_SERVER` is set and puppeteer is installed (the wire is
  additionally proven end-to-end in C++ by `core/tests/webtransport_test.cpp`).

- **Client session hardening (v0.1 must-fix bundle).**
  `@avatarsd-llc/libtracer-client`: pending one-shot requests now **reject when
  the transport closes** (`ClientTransport` gains an optional `onClose` hook,
  implemented by `TransportWs`; after a close, new requests fail fast with
  `"transport closed"`); a **per-request timeout** rejects a reply-less request
  (`ClientOptions.requestTimeoutMs`, default 10 000 ms, `0`/`Infinity` disables —
  a timed-out slot still consumes its late FWD{REPLY} so FIFO correlation holds);
  and an inbound **ADVERTISE (`0x11`) / COMPACT (`0x12`)** frame now surfaces a
  typed `CompactFlowError` on `onError` (compact route-handle delivery is not
  supported by this client yet) instead of being silently dropped.
- **RFC-0002 error-registry parity.** `FWD_ERROR` now carries the full 15-code
  registry (adds `FRAME_TRUNCATED`, `FRAME_INVALID`, `FRAME_CRC_FAIL`,
  `TLV_NESTING_TOO_DEEP`, `ADDRESS_SHIFT_GAP`, `TRANSPORT_DOWN`,
  `VERSION_MISMATCH`), mirroring `docs/reference/05` §`0x08` / the C++
  `error.hpp` registry, plus `FWD_ERROR_PATH` / `fwdErrorPath` /
  `fwdErrorCodeForPath` for the canonical `tr::…` path names. `FwdError` exposes
  `.path`, and a string-form (`NAME` `tr::…`) ERROR reply now surfaces typed on
  `FwdError` (a registered path resolves back to its code) instead of collapsing
  to `UNKNOWN`.

### Changed

- **`@avatarsd-llc/libtracer-ws`: the RFC 6455 codec is no longer re-exported from
  the package barrel.** `TransportWs` frames over the runtime's own WebSocket and
  never uses the hand-rolled codec, so re-exporting the codec (`Opcode`,
  `encodeFrame`, `acceptKey`, …) from the main entry forced every `TransportWs`
  consumer to bundle SHA-1 + base64 + the frame (de)coder. The codec now lives
  **only** at the `./ws` subpath (`@avatarsd-llc/libtracer-ws/ws`) — unchanged for
  its real consumers (the `ws_diff_fuzz` interop oracle, the ws-codec tests,
  no-native-WebSocket runtimes), and it now tree-shakes out of transport-only
  bundles. No effect on `TransportWs`, which the barrel still exports.

- **Version alignment for v0.1**: `@avatarsd-llc/libtracer` (core) bumped
  `0.0.1` → `0.1.0`, so all three packages ship as `0.1.0` (matching the C++
  `project(libtracer VERSION 0.1.0)`). The client/transport peerDependency
  ranges on the core (and the client's on `@avatarsd-llc/libtracer-ws`) are now
  the explicit `>=0.1.0 <0.2.0` — avoiding the `^0.x` caret pitfall.

- **`TYPE.FWD` (`0x0f`) and `TYPE.FIELD` (`0x10`)** in the wire codec's type-code
  registry (RFC-0004 / ADR-0035, slice 1). Both are structured TLVs handled by the
  existing generic codec; no codec logic changed. Cross-core conformance vectors
  under `tests/conformance/vectors/v1/{fwd,field}/` round-trip byte-for-byte
  against the C++ and Rust cores.
- **`@avatarsd-llc/libtracer-client` now speaks the RFC-0004 remote operations over
  `FWD`** (#56, ADR-0035): `read` / `write` / `await_` / `readField` / `subscribe`,
  each a path-addressed `FWD` frame whose source-routed `FWD{REPLY}` is decoded back
  (a `kind=ERROR` reply surfaces as a typed `FwdError`). Adds the `encodeFwd` /
  `encodeField` / `decodeFwd` builders (pinned to the `fwd-*` / `field-*` /
  `fwd-reply-*` vectors byte-for-byte) and an end-to-end interop test against a live
  C++ `fwd_node_server` over a real ws socket (all five ops, incl. a live subscribe
  delivery), CI-gated by the new `fwd-interop` job. The package is **promoted from
  `private` / `0.0.0` to a public `0.1.0`** (still experimental, pre-1.0);
  error-reply codes are provisional pending the ERROR registry (#8).

### Changed

- **`bindings/typescript/` is now an npm workspace monorepo** (ADR-0033). The
  existing core moved from `bindings/typescript/` to
  `bindings/typescript/packages/core/` and continues to publish as
  `@avatarsd-llc/libtracer` (unchanged public API). The single lockfile now lives
  at the workspace root.
- **`@avatarsd-llc/libtracer` gained a tree-shakeable `exports` map.** The barrel
  stays at `.`; the wire codec is now also reachable at the subpath entry
  `@avatarsd-llc/libtracer/wire`. Added `sideEffects: false`, an `engines.node`
  floor of `>=18`, and an explicit `directory` in `repository`. No exported
  symbols were removed or renamed.

### Added

- **`@avatarsd-llc/libtracer-client` — EXPERIMENTAL client SDK (`private`,
  `0.0.0`, #56, ADR-0034).** A payload-builder + frame-I/O client over the
  cross-validated codec (`@avatarsd-llc/libtracer`, a `peerDependency`) and an
  injected transport seam (`@avatarsd-llc/libtracer-ws`'s `TransportWs` satisfies
  it structurally; it is an optional peer). Implements only the wire byte-products
  v1 pins: `encodeValue` / `encodePath` / `encodeSubscriber` (matching the
  `value-*`, `path-sensor-temp`, and `subscriber-path` conformance vectors
  byte-for-byte), a `LibtracerClient` with `write` (VALUE frame) / `subscribe`
  (SUBSCRIBER frame + handler) / `onValue` (inbound VALUE delivery, shedding one
  ROUTER wrapper) / `onError`. The path-addressed request envelope
  (`write(path,…)` / `read` / `await` / `subscribe(producerPath,…)`) is **deferred**
  because the v1 wire format for an addressed remote operation is unspecified
  (`spec/v1.md` §3). Not published. The workspace root `build` script now builds
  `@avatarsd-llc/libtracer` first so the dependent packages always see fresh
  core `dist/`.
- **Both packages are now cleanly publishable to npm (ADR-0033, #98).** Each of
  `@avatarsd-llc/libtracer` and `@avatarsd-llc/libtracer-ws` gained
  `publishConfig.access: "public"` (scoped packages default to restricted), a
  `prepublishOnly` script that rebuilds `dist/` via `tsc` so a publish always
  ships fresh output, an Apache-2.0 `LICENSE` in the package directory, and a
  tightened `files` allowlist (`dist`, `README.md`, `LICENSE`) so the tarball
  excludes `src/`, tests, `tsconfig.json`, and bench. The
  `@avatarsd-llc/libtracer-ws` core dependency stays a `peerDependency` (not
  bundled). A standalone `.github/workflows/publish-npm.yml` publishes both
  packages (ESM, public access, npm provenance) on a published release / `v*`
  tag, plus a manual dry-run dispatch; it leaves the required core gates
  untouched. Real publishes require the `NPM_TOKEN` repository secret.
- **`@avatarsd-llc/libtracer-ws` is now a functional package (`0.1.0`, #54).**
  The WebSocket transport scaffold is implemented: an RFC 6455 frame codec at the
  `/ws` subpath (`acceptKey`, `encodeFrame`, `encodeClientFrame`, `decodeFrame`,
  overflow-safe) cross-validated byte-for-byte against the C++ `tr::net::ws`
  codec, plus a `TransportWs` client (barrel entry) that carries a libtracer TLV
  as one BINARY frame, wire-compatible with the C++ `tr::net::transport_ws`. The
  package is no longer `private`; the core stays a `peerDependency`. Tests cover
  the RFC 6455 known vectors and a Node `ws` round-trip.
- The earlier scaffold entry below is superseded by this implementation.

- **`@avatarsd-llc/libtracer-ws`** package scaffold (WebSocket transport,
  ADR-0029) — package boundary, name, and `exports` shape only; `private: true`,
  no functional transport code yet (deferred to #54).

### Notes

- The `./mem`, `./view`, `./graph` per-layer subpaths are **reserved** in the
  ADR; they are not implemented in TS yet and are not part of this change.
