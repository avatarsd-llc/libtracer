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

- **`@avatarsd-llc/libtracer-ws`** package scaffold (WebSocket transport,
  ADR-0029) — package boundary, name, and `exports` shape only; `private: true`,
  no functional transport code yet (deferred to #54).

### Notes

- The `./mem`, `./view`, `./graph` per-layer subpaths are **reserved** in the
  ADR; they are not implemented in TS yet and are not part of this change.
