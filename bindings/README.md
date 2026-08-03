# bindings — native per-language cores

Each binding here is a **native reimplementation** of the libtracer wire codec in its own language — a pure-TypeScript codec, a native-Rust codec — **not** an FFI wrapper over [`core/`](../core/), and it ships no C dependency or WASM bundle. That is the deliberate choice of [ADR-0028](../docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md): idiomatic, native-perf codecs kept from drifting by a shared **contract** rather than a shared binary — every core is CI-gated against the same language-agnostic conformance vectors under [`tests/conformance/vectors/v1/`](../tests/conformance/vectors/) (the C++ core is golden). Neither implements the **L4 graph runtime** — that is `core/` only. The Rust binding is the L2/L3 codec alone (`decode`/`encode` + the CRC primitives, `#![no_std]`); the TypeScript side ships the same codec core **plus** a remote-operation client SDK over `FWD` (`read`/`readField`/`write`/`writeField`/`await_`/`subscribe`, [RFC-0004](../docs/spec/rfcs/0004-remote-operation-addressing.md)) and two transport packages, so "codec only" describes the Rust side, not both. Independent third-party implementations are catalogued separately in [`docs/implementations.md`](../docs/implementations.md).

| Binding                    | Registry  | Package                                 | Status |
|----------------------------|-----------|-----------------------------------------|--------|
| [rust/](rust/)             | crates.io | `libtracer`                             | codec; **published** — 0.7.0, five releases since 2026-07-08 ([crates.io/crates/libtracer](https://crates.io/crates/libtracer)) |
| [typescript/](typescript/) | npm       | `@avatarsd-llc/libtracer` (+ `-client`, `-ws`, `-webtransport`) | **all four published at 0.7.0**: codec core, client SDK, and both transports. The client SDK is pre-1.0 and describes itself as EXPERIMENTAL — that is a surface-stability caveat, not an unpublished-scaffold one ([ADR-0034 erratum](../docs/adr/0034-typescript-client-sdk.md)) |

Both registry rows are **checked against the registry**, not against intent: the crate and all
four npm packages publish automatically on a `vX.Y.Z` release tag, so a version bump lands them
without a doc edit. `-webtransport` is the fourth TS package [ADR-0033](../docs/adr/0033-npm-subpackage-monorepo.md)
did not contemplate; its erratum records that the package set is larger than the ADR's §3
scaffold note describes.

Binding-level changes follow normal PR flow. They MUST NOT change wire-format behavior — if a fix requires that, it's a spec change (see [GOVERNANCE.md](../.github/GOVERNANCE.md)).
