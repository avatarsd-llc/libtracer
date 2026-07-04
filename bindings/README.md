# bindings — Language bindings over the C/C++ core

Each binding here is a thin wrapper over [`core/`](../core/), published to the language's standard package registry. They do not reimplement the protocol — for that, see [`docs/implementations.md`](../docs/implementations.md).

| Binding                       | Registry  | Crate / package name |
|-------------------------------|-----------|----------------------|
| [rust/](rust/)                | crates.io | `libtracer`          |
| [typescript/](typescript/)    | npm       | `libtracer`          |

Binding-level changes follow normal PR flow. They MUST NOT change wire-format behavior — if a fix requires that, it's a spec change (see [GOVERNANCE.md](../.github/GOVERNANCE.md)).
