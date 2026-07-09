<!--
SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# Releasing libtracer

The maintainer-facing checklist for cutting a libtracer release. It documents the
**mechanics that exist today**; it does not authorize a release. **Cutting a
release is an explicit maintainer decision** — nothing here auto-tags or
auto-publishes (the git tag is the trigger, and only the maintainer pushes it).

## Two version axes (do not conflate them — [ADR-0002](../docs/adr/0002-versioning-protocol-vs-release-no-per-frame-version.md))

- **Protocol version** — the wire format. `v1` is defined by
  [docs/spec/v1.md](../docs/spec/v1.md). It **freezes and becomes immutable once
  released**; a wire-incompatible change is **protocol v2** on a new discovery
  name. There is no version bit on the wire (peers learn the version from the
  discovery service name).
- **Library release** — this reference implementation's own `MAJOR.MINOR.PATCH`.
  Ships `0.x`, `1.0`, … that all still speak the same **protocol v1**. This is
  what a git tag and the package manifests carry.

A `0.x` **library** release MAY ship against a **DRAFT protocol** (`v1.md` still
says "not yet stable — pin to a commit"). A **stable** `1.0` library release
SHOULD NOT: freeze `v1.md` first (see the gate below).

## Source of truth for the version

**At release time, the pushed `vX.Y.Z` git tag is authoritative.**
[`release.yml`](workflows/release.yml) derives `X.Y.Z` from the tag and every
publish job stamps the checked-out tree with
`tools/sync-version.py X.Y.Z` before packaging — so what npm / crates.io /
PlatformIO / ESP see is always the tag's version, regardless of what the
committed manifests say (a mismatch is a workflow **warning**, not an error).

**Between releases**, the repo-root [`VERSION`](../VERSION) file is the one
hand-edited version, and everything else derives from it or is checked against
it, so the number cannot drift inside the tree:

- **`core/CMakeLists.txt`** reads `VERSION` for its `project(VERSION …)` — except
  when building at a release **git tag `vMAJOR.MINOR.PATCH`**, which wins, so a
  tagged checkout reports its exact tagged version.
- **Every publishable manifest is stamped** from `VERSION` by
  [`tools/sync-version.py`](../tools/sync-version.py) — **unified lockstep**, so
  one `vX.Y.Z` release means `X.Y.Z` everywhere and no registry ever sees a
  version collision:

  | Manifest | Ecosystem |
  | --- | --- |
  | [`library.json`](../library.json) | PlatformIO |
  | [`integrations/arduino/library.properties`](../integrations/arduino/library.properties) | Arduino |
  | [`integrations/esp-idf/libtracer/idf_component.yml`](../integrations/esp-idf/libtracer/idf_component.yml) | ESP Component Registry |
  | `bindings/typescript/packages/*/package.json` (×4) | npm (`@avatarsd-llc/*`) |
  | [`bindings/rust/Cargo.toml`](../bindings/rust/Cargo.toml) | crates.io |

  The stamper also rewrites the TS packages' internal `@avatarsd-llc/*` dependency
  **ranges** (leaving `*`/`workspace:` dev links alone). The ROS 2 stub is
  unreleased and intentionally excluded.

- **CI enforces it.** [`version-consistency.yml`](workflows/version-consistency.yml)
  runs `python3 tools/sync-version.py --check` and fails any PR where an artifact
  has drifted from `VERSION`, so a bump can never land half-applied.

To reconcile the tree to a version: run `python3 tools/sync-version.py X.Y.Z`
(stamps `VERSION` **and** every manifest), refresh the lockfiles
(`cd bindings/typescript && npm install --package-lock-only`;
`cd bindings/rust && cargo update -p libtracer`), and commit them together.

**One consumer bypasses the pipeline:** the Arduino Library Registry indexes
[`library.properties`](../integrations/arduino/library.properties) directly
from the **tagged tree** — no CI job stamps it on the way. So reconciling the
committed manifests before (or promptly after) tagging is still **recommended**;
tag-time stamping makes it non-blocking for the four CI-published registries,
not unnecessary. The same applies to anyone consuming the raw tagged tree by
git pin (CMake `FetchContent` is covered — `core/CMakeLists.txt` prefers the
git tag over `VERSION`).

## Pre-release gates

1. **Authorization.** The maintainer has explicitly decided to cut this release.
2. **Spec-freeze gate (stable releases only).** For a `1.0`/stable release,
   [`docs/spec/v1.md`](../docs/spec/v1.md) must be marked released/frozen (not
   DRAFT), and every open RFC under [`docs/spec/rfcs/`](../docs/spec/rfcs/) must
   be resolved (accepted / rejected / superseded) — you cannot freeze a wire spec
   with pending normative changes. A `0.x` preview MAY skip this and ship the
   DRAFT with the "pin to a commit" note.
3. **CHANGELOG.** [`core/CHANGELOG.md`](../core/CHANGELOG.md)'s `[Unreleased]`
   section is complete (every public-API change has a note, per
   [CONTRIBUTING](CONTRIBUTING.md)).
4. **CI is green on `main`** — all workflows, including `core-ci` (build + ctest +
   sanitizers + the `install-consume` packaging guard), `conformance` (3-core
   cross-match + diff-fuzz), `esp-idf`, `quic`, and `docs`.

## Steps

1. **Changelog-cut PR (+ recommended version reconcile).** In one PR: move each
   `CHANGELOG.md`'s `[Unreleased]` entries under a new `## [X.Y.Z] — YYYY-MM-DD`
   heading. Recommended in the same PR (required only for the Arduino registry,
   see "Source of truth" above): `python3 tools/sync-version.py X.Y.Z` and
   refresh the lockfiles (`npm install --package-lock-only`,
   `cargo update -p libtracer`). Merge it (signed, per DCO).
2. **Tag + push — this triggers the whole release.** On the merge commit:
   ```sh
   git tag -s vX.Y.Z -m "libtracer vX.Y.Z"
   git push origin vX.Y.Z
   ```
   [`release.yml`](workflows/release.yml) fires on the `v*` tag, derives
   `X.Y.Z` **from the tag** (the tag is authoritative — each publish job stamps
   the tree with `tools/sync-version.py X.Y.Z` before packaging; a committed
   `VERSION` that disagrees is only a warning), then does all of the following
   automatically. Each
   publish is an **independent job**: a missing secret **skips** that registry
   with a warning (add tokens incrementally) rather than failing the release.
   - **GitHub Release** — an AI-written summary (from the CHANGELOG + commits)
     above the extracted `## [X.Y.Z]` CHANGELOG section. Needs `ANTHROPIC_API_KEY`;
     without it, the CHANGELOG section alone is the body.
   - **npm** — the four `@avatarsd-llc/*` packages at `X.Y.Z`. Needs `NPM_TOKEN`.
   - **crates.io** — `libtracer` at `X.Y.Z`. Needs `CARGO_REGISTRY_TOKEN`.
   - **PlatformIO** — `pio package publish`. Needs `PLATFORMIO_AUTH_TOKEN`.
   - **ESP Component Registry** — `compote component upload`. Needs
     `IDF_COMPONENT_API_TOKEN`.

   ([`publish-npm.yml`](workflows/publish-npm.yml) remains as a manual
   `workflow_dispatch` **dry-run tester** for the npm packages only.)
3. **Arduino (manual, one-time).** The Arduino Library Registry is a submission
   PR to [`arduino/library-registry`](https://github.com/arduino/library-registry),
   not a tag push — do it once; thereafter it tracks new tags automatically.
4. **Verify.** Check the GitHub Release and that npm / crates.io / PlatformIO /
   ESP show `X.Y.Z`. `find_package(libtracer X.Y REQUIRED)` is already proven by
   the `install-consume` CI job.

## Notes

- **This repo's standing rule:** the pending `0.3.0` release is **held** until the
  maintainer explicitly says to cut it. The version markers already read `0.3.0`
  (in-development); that is reconciliation, **not** a release — no tag exists yet.
- Spec/governance context: [GOVERNANCE.md](GOVERNANCE.md) (RFC process, BDFL
  model, spec immutability), [CONTRIBUTING.md](CONTRIBUTING.md) (DCO signing).
