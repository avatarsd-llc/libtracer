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
  | [`integrations/arduino/library.properties`](../integrations/arduino/library.properties) | Arduino (**not distributed** — stamped for tree consistency only, see below) |
  | [`integrations/esp-idf/libtracer/idf_component.yml`](../integrations/esp-idf/libtracer/idf_component.yml) | ESP Component Registry |
  | `bindings/typescript/packages/*/package.json` (×4) | npm (`@avatarsd-llc/*`) |
  | [`bindings/typescript/package-lock.json`](../bindings/typescript/package-lock.json) | npm (mirrors the four above) |
  | [`bindings/rust/Cargo.toml`](../bindings/rust/Cargo.toml) | crates.io |

  The stamper also rewrites the TS packages' internal `@avatarsd-llc/*` dependency
  **ranges** (leaving `*`/`workspace:` dev links alone). npm copies each workspace
  manifest into a `packages/<dir>` lockfile entry, so the stamper rewrites those
  entries in place — no `npm install`, no network — leaving every resolved
  third-party pin byte-for-byte untouched. The ROS 2 stub is unreleased and
  intentionally excluded.

- **CI enforces it.** [`version-consistency.yml`](workflows/version-consistency.yml)
  runs `python3 tools/sync-version.py --check` and fails any PR where an artifact
  has drifted from `VERSION`, so a bump can never land half-applied. Its `paths:`
  filter is an allowlist — a new stamped artifact must be added there too, or the
  gate simply will not run on the PR that breaks it.

To reconcile the tree to a version: run `python3 tools/sync-version.py X.Y.Z`
(stamps `VERSION`, every manifest **and** the npm lockfile), refresh the Rust
lockfile (`cd bindings/rust && cargo update -p libtracer`), and commit them
together.

**Arduino is not a release channel.** The Arduino Library Manager path is **not
planned** (maintainer decision, `9751fd32`) — see
[`integrations/arduino/README.md`](../integrations/arduino/README.md). libtracer
has never been submitted to [`arduino/library-registry`](https://github.com/arduino/library-registry)
and no release step does so. As laid out, it could not be: the registry requires
`library.properties` **in the repository root**, and ours lives under
`integrations/arduino/`. `tools/sync-version.py` still stamps it so the number
cannot drift inside the tree (and `version-consistency.yml` enforces that), but
nothing consumes it. Point Arduino users at the **PlatformIO** package or the
**ESP-IDF** managed component.

**Consumers of the raw tagged tree** (a git pin rather than a registry) see the
committed manifests, not the tag-time stamped ones — so reconciling them before
tagging is still **recommended**; tag-time stamping makes it non-blocking for the
CI-published registries, not unnecessary. CMake `FetchContent` is already covered
— `core/CMakeLists.txt` prefers the git tag over `VERSION`.

## Pre-release gates

1. **Authorization.** The maintainer has explicitly decided to cut this release.
2. **Spec-freeze gate (stable releases only).** For a `1.0`/stable release,
   [`docs/spec/v1.md`](../docs/spec/v1.md) must be marked released/frozen (not
   DRAFT), and every open RFC under [`docs/spec/rfcs/`](../docs/spec/rfcs/) must
   be resolved (accepted / rejected / superseded) — you cannot freeze a wire spec
   with pending normative changes. A `0.x` preview MAY skip this and ship the
   DRAFT with the "pin to a commit" note.
3. **CHANGELOG — all four of them.** Every package changelog's `[Unreleased]`
   section is complete (every public-API change has a note, per
   [CONTRIBUTING](CONTRIBUTING.md)): [`core/CHANGELOG.md`](../core/CHANGELOG.md),
   [`bindings/rust/CHANGELOG.md`](../bindings/rust/CHANGELOG.md),
   [`bindings/typescript/CHANGELOG.md`](../bindings/typescript/CHANGELOG.md) and
   [`integrations/esp-idf/libtracer/CHANGELOG.md`](../integrations/esp-idf/libtracer/CHANGELOG.md).
   One tag publishes all four in lockstep, and since #607 the release body
   concatenates all four — a package whose entries are missing is a package whose
   users learn nothing from the release. (The failure this fixes was real: #676's
   TypeScript **BREAKING** entries were invisible to npm consumers because only
   `core/CHANGELOG.md` was read.) If any entry is marked **BREAKING**, check
   before cutting that the release is the one the maintainer intends to carry it:
   consumers pin a range, so a break lands on them at the next bump. State every
   break in the release summary, not only in the CHANGELOG body.
   ([`bindings/ros2/CHANGELOG.md`](../bindings/ros2/CHANGELOG.md) exists since
   `v0.13.0` but is **not** a fifth gated package: `rmw_tracer` publishes to no
   registry, so it is neither stamped by `sync-version.py` nor passed to
   `gen_release_notes.py`, and it does not appear in the release body. It is a
   repo-local record for a binding that is still being built out.)
4. **CI is green on `main`** — all workflows, including `core-ci` (build + ctest +
   sanitizers + the `install-consume` packaging guard), `conformance` (3-core
   cross-match + diff-fuzz), `esp-idf`, `quic`, and `docs`.

## Steps

1. **Changelog-cut PR (+ recommended version reconcile) — the LAST pre-tag
   commit.** Consolidation is done *at tag time*, never speculatively: a
   long-lived `[Unreleased]` accumulates one `### Category` heading per landing PR
   (18 against 3 distinct names, at the time of writing), and merging them early
   only creates conflicts for every PR still in flight. Do it mechanically:
   ```sh
   python3 tools/consolidate_changelog.py \
     core/CHANGELOG.md bindings/rust/CHANGELOG.md \
     bindings/typescript/CHANGELOG.md integrations/esp-idf/libtracer/CHANGELOG.md \
     --release X.Y.Z --write
   ```
   That merges each file's repeated `### Category` subsections into one per
   category in Keep-a-Changelog order (entry order preserved, text moved verbatim),
   renames the section to `## [X.Y.Z] — YYYY-MM-DD`, and opens a fresh empty
   `[Unreleased]`. `--check` is the read-only form; omit `--write` to preview on
   stdout. Read the diff before committing — the tool moves text, it does not
   judge it. Recommended in the same PR (it keeps the committed manifests honest
   for anyone consuming the tagged tree by git pin — see "Source of truth"
   above): `python3 tools/sync-version.py X.Y.Z` (which now
   carries `bindings/typescript/package-lock.json` with it) and refresh the Rust
   lockfile (`cargo update -p libtracer`). Merge it (signed, per DCO).
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
   - **GitHub Release** — an AI-written summary (from the CHANGELOGs + commits)
     above the extracted `## [X.Y.Z]` section of **every package changelog**, each
     under its own heading (`tools/gen_release_notes.py`, one `--changelog` per
     package). A package whose section is still called `[Unreleased]` at tag time
     contributes that instead, so a binding that missed the consolidation PR is
     still published. Needs `ANTHROPIC_API_KEY`; without it, the CHANGELOG sections
     alone are the body.
   - **npm** — the four `@avatarsd-llc/*` packages at `X.Y.Z`. Needs `NPM_TOKEN`.
   - **crates.io** — `libtracer` at `X.Y.Z`. Needs `CARGO_REGISTRY_TOKEN`.
   - **PlatformIO** — `pio package publish`. Needs `PLATFORMIO_AUTH_TOKEN`.
   - **ESP Component Registry** — `compote component upload`. Needs
     `IDF_COMPONENT_API_TOKEN`.

   ([`publish-npm.yml`](workflows/publish-npm.yml) remains as a manual
   `workflow_dispatch` **dry-run tester** for the npm packages only.)
3. **Verify — per registry, not just the run's conclusion.** Because a missing
   secret **skips** a publish job rather than failing it, a green `release` run
   does not prove all five artifacts shipped. Check the GitHub Release, and check
   that npm (×4) / crates.io / PlatformIO / ESP actually serve `X.Y.Z`. Two
   gotchas: the npm package names (`@avatarsd-llc/libtracer{,-ws,-webtransport,-client}`)
   are **not** the `bindings/typescript/packages/` directory names, and the ESP
   Component Registry index can lag the upload by several minutes — poll it before
   concluding the job skipped. `find_package(libtracer X.Y REQUIRED)` is already
   proven by the `install-consume` CI job. There is no Arduino step (see "Source
   of truth" above).

## Notes

- **This repo's standing rule:** a release is cut only when the maintainer
  explicitly says so. Reconciling the version markers (`tools/sync-version.py`) is
  **not** a release — only the signed `vX.Y.Z` tag is.
- **Released so far:** `v0.3.0` (2026-07-08) through `v0.13.0` (2026-08-16).
  `VERSION` reads the most recently shipped release, so the next cut bumps it.
  Do not take this list as authoritative —
  `git tag -l 'v*'` and `gh release list` are; it is here so a reader knows the
  process has run before and is not being exercised for the first time.
- Spec/governance context: [GOVERNANCE.md](GOVERNANCE.md) (RFC process, BDFL
  model, spec immutability), [CONTRIBUTING.md](CONTRIBUTING.md) (DCO signing).
