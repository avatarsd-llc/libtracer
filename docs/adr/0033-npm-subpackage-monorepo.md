# npm: ship the TypeScript side as a workspace monorepo — one cross-validated core package, per-transport subpackages, and per-layer slicing via subpath exports (not many packages)

Status: accepted (the npm packaging architecture; **tooling** domain per
[GOVERNANCE.md](../../GOVERNANCE.md), so an ADR — not an RFC — governs it). The
conservative foundation (workspace + core `exports` map + a transport scaffold)
lands with this ADR; the per-transport internals and any per-layer TS code are
deferred to follow-up issues. Resolves the "npm subpackages monorepo" tracking
item flagged in [ADR-0032](0032-continuous-cross-core-perf-conformance-matrix.md).
Implements [#98](https://github.com/avatarsd-llc/libtracer/issues/98).

## Context

libtracer is **one cross-validated core, many packagings**
([ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md)): the
C++, Rust and TypeScript cores all decode/encode the shared vectors identically
or CI fails. The TS core (`@avatarsd-llc/libtracer`, #56) is today a single
package containing the L2/L3 wire codec.

Issue #98 asks to publish the TS side as an npm **monorepo with multiple
subpackages**, so a consumer pulls only what it needs:

1. **core** — the in-process codec + graph, no transports.
2. **per-transport** packages — WebSocket ([ADR-0029](0029-websocket-first-transport-quic-deferred-per-link.md)),
   later WebTransport ([ADR-0031](0031-direct-browser-to-robot-binding-and-webtransport.md)).
3. **"per L1–L5 module combination"** — curated layer compositions / deployment
   profiles ([reference 12](../reference/12-deployment-profiles.md)).

Two things make this design-led rather than mechanical, which is why #98 is
`ready-for-human`:

- **A transport must not be a dependency of the core.** A browser bundle that
  only decodes frames should never pull a WebSocket server stack. The core/
  transport split is a real package boundary ([ADR-0027](0027-transport-and-connections-are-vertices.md):
  transports are swappable vertices behind a seam).
- **"Per L1–L5 module combination" is a combinatorial trap.** Six layers × the
  composition variants in ADR-0032's `n-layer-folded` axis (heap vs pooled vs
  borrowed vs rope; lean vs stream vs lean-cached) is dozens of permutations.
  Minting an npm package per permutation would be unmaintainable, would multiply
  the publish/version surface, and would fragment the cross-validated core into
  packages that can drift. The mechanism for "pull only the layers you use" has
  to be chosen deliberately.

This ADR fixes the architecture and ships only the **reversible foundation**;
nothing here forces the irreversible combinatorial decisions, which are deferred
to the maintainer and to follow-up issues.

## Decision

### 1. Monorepo via **npm workspaces**, rooted at `bindings/typescript/`

`bindings/typescript/` becomes a private workspace root (`"workspaces":
["packages/*"]`, `"private": true`) with one lockfile. Packages live under
`bindings/typescript/packages/*`. The existing core moves to
`packages/core/` and still publishes as `@avatarsd-llc/libtracer` — same name,
same public API.

- **npm workspaces, not pnpm/yarn.** The repo's CI already provisions Node and
  runs the TS harness under plain `node` with no extra package manager; the
  toolchain stays zero-new-dependency. pnpm's stricter linking and disk savings
  are real but unneeded at two packages, and adding a package manager is a CI +
  contributor cost. Revisit if the workspace grows large.
- **Rooted in `bindings/typescript/`, not a new top-level dir.** Keeps every
  language binding under `bindings/` (repo layout invariant) and keeps the npm
  packages adjacent to the conformance harness they are gated by.

### 2. Core package `@avatarsd-llc/libtracer` = pure in-process core, **no transports**

`packages/core` is the codec (L2/L3 today; L0/L1/L4 in-process as they land). It
declares `sideEffects: false` (tree-shaking), ESM only, `engines.node >= 18`,
and an `exports` map. It has **no runtime dependency on any transport**. A
transport depends on the core, never the reverse.

### 3. Per-transport **subpackages** — one package per transport

Each transport is its own package: `@avatarsd-llc/libtracer-ws` (WebSocket,
ADR-0029) now as a scaffold, `@avatarsd-llc/libtracer-webtransport` later
(ADR-0031). The core is a **`peerDependency`** of each transport, so the
transport binds against whatever cross-validated core version the consumer
installs (one core in the tree, no duplication).

TS transports are **not implemented yet** (#54 ships WS). This ADR fixes only
the **package boundary, name, and `exports` shape**; `packages/transport-ws`
ships as a `private: true` scaffold with no functional code, so it cannot be
published until the implementation lands.

### 4. "Per L1–L5 module combination" → **subpath exports**, not many packages

We slice layers with **subpath `exports` entry points** inside the core, *not*
by minting a package per layer or per composition:

```jsonc
"exports": {
  ".":      { "types": "./dist/index.d.ts", "default": "./dist/index.js" }, // barrel
  "./wire": { "types": "./dist/wire.d.ts",  "default": "./dist/wire.js"  }, // L2/L3 codec
  "./package.json": "./package.json"
}
```

A consumer writes `import { decode } from '@avatarsd-llc/libtracer/wire'` and,
with `sideEffects: false`, a bundler drops every layer it does not import. As the
remaining layers are ported to TS they get their own subpath — the **reserved**
names are `./mem` (L0), `./view` (L1), `./graph` (L4). They are *not* added until
the code exists (no faking; same honesty rule as ADR-0032's coverage target).

Curated "profiles" (browser-WS-client, node-server — reference 12) are **not**
packages either. A profile is a thin convenience re-export (a documented import
recipe, or at most a tiny package that just re-exports a fixed set of subpaths +
one transport). We do **not** pre-mint profile packages in this slice; if demand
proves real, a profile becomes one small package over the stable subpath API —
cheap and reversible — rather than the API being designed around profiles now.

**Why subpaths over packages** (the core trade-off):

- *Bundle size* is already solved by `exports` subpaths + `sideEffects: false` +
  tree-shaking — the actual goal of #98 ("pull only what you need") is met
  without package sprawl.
- *One package = one cross-validated unit.* The whole thesis is one core kept
  identical by vectors (ADR-0028). Splitting layers into separately-versioned
  packages invites drift and a version-matrix (`mem@2` + `wire@1`?) with no
  benefit a subpath doesn't already give.
- *Publish/maintenance surface* stays at ~one core + N transports, not N×layers.
- Transports *are* separate packages because they carry **distinct runtime
  dependencies and environment constraints** (a WS server stack, browser-only
  APIs) — that is a real dependency boundary; a pure-code layer split is not.

### 5. Versioning & publish

- **Independent SemVer per package**, not lockstep. The core and a transport
  evolve on different clocks; a transport pins the core via a `peerDependency`
  range. (Two packages today — independent is simple; if cross-package churn
  ever dominates, a fixed/`changesets` lockstep is reconsidered.)
- **ESM-only**, `type: module`, `exports`/`types` conditions on every entry,
  `engines.node >= 18` (current LTS floor; ESM + modern `exports` are stable).
- Publish remains tag-triggered (`ts-vX.Y.Z` → `publish-npm.yml`, to be added);
  scaffolded/`private` packages are excluded until they have real code.

## Considered options

- **One blob package (status quo).** Rejected: a transport would become a core
  dependency, defeating #98's "pull only what you need."
- **A package per layer and per composition** (`@avatarsd-llc/libtracer-mem`,
  `-view`, `-wire`, `-graph`, + profile packages). Rejected: combinatorial
  package sprawl, an N-way version matrix, and fragmentation of the one
  cross-validated core — for a bundle-size win that subpath `exports` already
  delivers.
- **pnpm/yarn workspaces.** Rejected for now: npm workspaces need zero new CI
  tooling at this scale; revisit if the workspace grows.
- **Lockstep versioning (changesets, fixed version).** Rejected for now:
  unnecessary coupling at two packages; the `peerDependency` range already keeps
  a transport honest against the core.
- **Pre-mint profile packages (browser-WS-client, node-server).** Deferred: a
  profile is cheaply expressible as an import recipe over the subpath API;
  promote one to a package only on real demand.

## Consequences

- **Foundation shipped, green:** the workspace exists, the core publishes with a
  tree-shakeable `exports` map (`.` + `./wire`), and `packages/transport-ws` is a
  named scaffold. The conformance harness moved with the core
  (`bindings/typescript/packages/core/conformance/harness.mjs`);
  `tests/conformance/harnesses.json` is updated and `run-all.py` still
  cross-matches cpp + ts + rust.
- **No public API change to the core** — same name, same exports; `./wire` is
  additive. `sideEffects: false` and the `exports` map are the only
  consumer-visible additions (recorded in `bindings/typescript/CHANGELOG.md`).
- **Adding a transport is "add a package," adding a layer is "add a subpath"** —
  neither is a redesign, mirroring ADR-0028's "add a core" shape.
- **Reversible:** if subpaths ever prove insufficient (e.g. a layer grows a heavy
  optional dependency), that single layer can be promoted to its own package over
  the same import name later. Nothing here is one-way.

### Deferred to follow-up (maintainer review)

- ~~WebSocket transport **internals** (`@avatarsd-llc/libtracer-ws`) — #54.~~
  **Implemented (#54).** `@avatarsd-llc/libtracer-ws` is now functional (`0.1.0`,
  no longer `private`): an RFC 6455 frame codec at the `/ws` subpath
  (cross-validated byte-for-byte against the C++ `tr::net::ws` codec) plus a
  `TransportWs` client that carries a libtracer TLV as one BINARY frame,
  wire-compatible with the C++ `tr::net::transport_ws`. The core remains a
  `peerDependency`; the per-transport package boundary fixed by this ADR is
  unchanged.
- Any **per-layer TS code** behind `./mem` / `./view` / `./graph` — those
  subpaths are reserved, not implemented.
- The **`publish-npm.yml`** release workflow and npm-org publish access.
- Whether to ever mint **profile** packages (reference 12) — only on real demand.
- Lockstep vs independent versioning revisit if/when cross-package churn appears.

## Relates

- [ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md) — one cross-validated core, many packagings (the invariant this preserves).
- [ADR-0032](0032-continuous-cross-core-perf-conformance-matrix.md) — flagged the npm subpackages monorepo as a tracking item; `n-layer-folded` axis is the composition surface this slices with subpaths.
- [ADR-0027](0027-transport-and-connections-are-vertices.md) — transports are swappable vertices: the real core/transport package boundary.
- [ADR-0029](0029-websocket-first-transport-quic-deferred-per-link.md) / [ADR-0031](0031-direct-browser-to-robot-binding-and-webtransport.md) — the transports that become per-transport packages.
- [reference 12](../reference/12-deployment-profiles.md) — deployment profiles (the "compositions"); expressed as subpath recipes, not pre-minted packages.
- `bindings/typescript/` (workspace), `tests/conformance/harnesses.json` (updated ts harness path), issue [#98](https://github.com/avatarsd-llc/libtracer/issues/98).
