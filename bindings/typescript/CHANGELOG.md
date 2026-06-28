# Changelog — libtracer TypeScript packages

All notable changes to the npm packages under `bindings/typescript/`. The core
package follows [Semantic Versioning](https://semver.org/); see
[ADR-0033](../../docs/adr/0033-npm-subpackage-monorepo.md) for the
versioning/publish strategy.

## Unreleased

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
