# libtracer (Rust binding)

Native Rust implementation of the libtracer wire codec. The crate is `libtracer`,
versioned in **lockstep** with the C++ core (one `vX.Y.Z` tag releases everything
at that version). **Not yet published to crates.io** — it publishes on the next
release tag (see [Releasing](#releasing)); until then, depend on the git repo.

## Status

**Native wire codec implemented** ([#57](https://github.com/avatarsd-llc/libtracer/issues/57)).
A from-scratch `#![no_std]` (`alloc`-only, no external dependencies) implementation of
the protocol-v1 wire format — `decode` / `encode` plus the CRC-32C / CRC-16-CCITT
primitives. It is kept in lock-step with the C++ and TypeScript cores by the shared
conformance vectors and the cross-core CI gate; see the [CHANGELOG](CHANGELOG.md).
Higher-level graph/transport APIs are still to come.

## Strategy

The original plan (decision **Q2.3**) was a **two-crate FFI** layout: `libtracer-sys`
(raw `bindgen` bindings to the C++ core) plus a safe `libtracer` wrapper.

**This was superseded by [ADR-0028](../../docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md).**
Rust gets a **native, pure-Rust core** — not an FFI shim over C++ — kept in lock-step
with the C++ and TypeScript cores by the shared conformance vectors and the CI gate
(`tests/conformance/run-all.py`). The published crate is `libtracer` (single crate; no
`-sys` crate, because there is no native library to link).

Rationale: a native core compiles to WASM and `no_std` targets without dragging a C++
toolchain through the FFI seam, and the conformance gate makes "three independent
implementations" cheaper to keep consistent than one impl behind two language wrappers.

## Use

```toml
[dependencies]
# Not yet on crates.io — depend on the git repo until the first crate release:
libtracer = { git = "https://github.com/avatarsd-llc/libtracer" }
```

## Security posture

**Unsafe by default in v0.1** — no `security_*` modules exist yet (TLS/DTLS/PSK/ACL
enforcement are post-MVP per the [module catalog](../../docs/reference/10-module-catalog.md)).
Run on a trusted link or tunnel the transport until `security_*` lands.

## Releasing

The crate publishes to crates.io automatically on a `vX.Y.Z` release tag, in
lockstep with the C++ core and the npm packages — see
[`.github/RELEASING.md`](../../.github/RELEASING.md) and
[`release.yml`](../../.github/workflows/release.yml). Needs the
`CARGO_REGISTRY_TOKEN` secret.
