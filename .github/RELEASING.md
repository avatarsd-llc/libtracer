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

The **git tag `vMAJOR.MINOR.PATCH`** is authoritative. `core/CMakeLists.txt`
derives `project(VERSION …)` from `git describe --tags` (falling back to an
in-development version only when no tag is reachable), so there is no hardcoded
C++ version to drift. The **static package manifests cannot read git**, so they
carry a hardcoded version that must be bumped in lockstep with the tag:

| Manifest | Ecosystem |
| --- | --- |
| [`library.json`](../library.json) | PlatformIO |
| [`integrations/arduino/library.properties`](../integrations/arduino/library.properties) | Arduino |
| [`integrations/esp-idf/libtracer/idf_component.yml`](../integrations/esp-idf/libtracer/idf_component.yml) | ESP Component Registry |

The **Rust and TypeScript bindings version independently** of the core (their own
`Cargo.toml` / `package.json`), on their own cadence — a core release does not
force a binding release, or vice versa.

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

1. **Bump + changelog PR.** In one PR: set the three manifests above to `X.Y.Z`,
   and move `core/CHANGELOG.md`'s `[Unreleased]` entries under a new
   `## [X.Y.Z] — YYYY-MM-DD` heading. Merge it (signed, per DCO).
2. **Tag.** On the merge commit, create a **signed, annotated** tag and push it:
   ```sh
   git tag -s vX.Y.Z -m "libtracer vX.Y.Z"
   git push origin vX.Y.Z
   ```
   From here CMake reports `X.Y.Z` for any checkout at that tag.
3. **GitHub Release.** Publish a GitHub Release from the tag (release notes = the
   new CHANGELOG section). Publishing the Release **triggers
   [`publish-npm.yml`](workflows/publish-npm.yml)**, which publishes the three
   scoped `@avatarsd-llc/*` TypeScript packages (needs the `NPM_TOKEN` secret; a
   `v*` tag push also triggers it, and `workflow_dispatch` offers a dry run).
4. **Manual publishes (not yet automated — do by hand when releasing that
   surface):**
   - **crates.io** — `cargo publish` from `bindings/rust/` (the crate is
     `libtracer`; versioned independently — only when the Rust binding is being
     released).
   - **PlatformIO / Arduino / ESP Component Registry** — no publish workflow
     exists yet; register/update from the manifests above per each registry's
     process. *(Automating these is a TODO.)*
5. **Verify.** Install from a clean checkout at the tag and confirm
   `find_package(libtracer X.Y REQUIRED)` resolves (the `install-consume` CI job
   does this per-PR; re-confirm the tagged artifact). Check the published npm /
   crates versions.

## Notes

- **This repo's standing rule:** the pending `0.3.0` release is **held** until the
  maintainer explicitly says to cut it. The version markers already read `0.3.0`
  (in-development); that is reconciliation, **not** a release — no tag exists yet.
- Spec/governance context: [GOVERNANCE.md](GOVERNANCE.md) (RFC process, BDFL
  model, spec immutability), [CONTRIBUTING.md](CONTRIBUTING.md) (DCO signing).
