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

There is **one hand-edited version for the core release axis**: the repo-root
[`VERSION`](../VERSION) file. Everything else derives from it or is checked
against it, so the number cannot drift:

- **`core/CMakeLists.txt`** reads `VERSION` for its `project(VERSION …)` — except
  when building at a release **git tag `vMAJOR.MINOR.PATCH`**, which wins, so a
  tagged checkout reports its exact tagged version.
- **The static package manifests cannot read git or the file**, so they are
  *stamped* from `VERSION` by [`tools/sync-version.py`](../tools/sync-version.py):

  | Manifest | Ecosystem |
  | --- | --- |
  | [`library.json`](../library.json) | PlatformIO |
  | [`integrations/arduino/library.properties`](../integrations/arduino/library.properties) | Arduino |
  | [`integrations/esp-idf/libtracer/idf_component.yml`](../integrations/esp-idf/libtracer/idf_component.yml) | ESP Component Registry |

- **CI enforces it.** [`version-consistency.yml`](workflows/version-consistency.yml)
  runs `python3 tools/sync-version.py --check` and fails any PR where a manifest
  has drifted from `VERSION`, so a bump can never land half-applied.

To bump the core version: edit `VERSION`, run `python3 tools/sync-version.py`,
commit the file + the three stamped manifests together.

The **Rust and TypeScript bindings version independently** of the core (their own
`Cargo.toml` / `package.json`), on their own cadence — a core release does not
force a binding release, or vice versa. They are intentionally *not* touched by
`sync-version.py`.

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

1. **Bump + changelog PR.** In one PR: edit the [`VERSION`](../VERSION) file to
   `X.Y.Z` and run `python3 tools/sync-version.py` (stamps the three manifests —
   `version-consistency` CI verifies no drift), and move `core/CHANGELOG.md`'s
   `[Unreleased]` entries under a new `## [X.Y.Z] — YYYY-MM-DD` heading. Merge it
   (signed, per DCO).
2. **Tag.** On the merge commit, create a **signed, annotated** tag and push it:
   ```sh
   git tag -s vX.Y.Z -m "libtracer vX.Y.Z"
   git push origin vX.Y.Z
   ```
   From here CMake reports `X.Y.Z` for any checkout at that tag.
3. **GitHub Release.** Publish a GitHub Release from the tag (release notes = the
   new CHANGELOG section). This is the **C++ core** release. It does **not**
   publish npm — the tag/Release are deliberately **decoupled** from
   `publish-npm.yml` (the TS packages version independently and republishing an
   already-published version would fail). See step 4 for npm.
4. **Package publishes (each independent — do the ones you're actually
   releasing):**
   - **npm (`@avatarsd-llc/*` TS packages)** — bump the TS package versions
     intentionally (they are **not** the core version), then run
     [`publish-npm.yml`](workflows/publish-npm.yml) via **`workflow_dispatch`**
     (dry-run first; needs the `NPM_TOKEN` secret for a real publish). It is
     manual-only — a core tag never triggers it.
   - **crates.io** — `cargo publish` from `bindings/rust/` (the crate is
     `libtracer`; versioned independently — only when the Rust binding is being
     released; not yet published).
   - **PlatformIO / Arduino / ESP Component Registry** — no publish workflow
     exists yet; register/update from the `VERSION`-stamped manifests per each
     registry's process. *(Automating these is a TODO.)*
5. **Verify.** Install from a clean checkout at the tag and confirm
   `find_package(libtracer X.Y REQUIRED)` resolves (the `install-consume` CI job
   does this per-PR; re-confirm the tagged artifact). Check any published npm /
   crates versions.

## Notes

- **This repo's standing rule:** the pending `0.3.0` release is **held** until the
  maintainer explicitly says to cut it. The version markers already read `0.3.0`
  (in-development); that is reconciliation, **not** a release — no tag exists yet.
- Spec/governance context: [GOVERNANCE.md](GOVERNANCE.md) (RFC process, BDFL
  model, spec immutability), [CONTRIBUTING.md](CONTRIBUTING.md) (DCO signing).
