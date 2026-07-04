# bindings — native per-language cores

Each binding here is a **native reimplementation** of the libtracer wire codec in its own language — a pure-TypeScript codec, a native-Rust core — **not** an FFI wrapper over [`core/`](../core/), and it ships no C dependency or WASM bundle. That is the deliberate choice of [ADR-0028](../docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md): idiomatic, native-perf cores kept from drifting by a shared **contract** rather than a shared binary — every core is CI-gated against the same language-agnostic conformance vectors under [`tests/conformance/vectors/v1/`](../tests/conformance/vectors/) (the C++ core is golden). Independent third-party implementations are catalogued separately in [`docs/implementations.md`](../docs/implementations.md).

| Binding                       | Registry  | Crate / package name |
|-------------------------------|-----------|----------------------|
| [rust/](rust/)                | crates.io | `libtracer`          |
| [typescript/](typescript/)    | npm       | `libtracer`          |

Binding-level changes follow normal PR flow. They MUST NOT change wire-format behavior — if a fix requires that, it's a spec change (see [GOVERNANCE.md](../.github/GOVERNANCE.md)).
