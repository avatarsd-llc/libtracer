# `core/` style — naming, namespaces, documentation

The mechanical conventions for the C++23 reference implementation. The *architectural* rationale (why the substrate is shaped this way) is [ADR-0016](../docs/adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md); this file is the "how to spell it." C/C++ also passes `clang-format` (config at [`.clang-format`](../.clang-format)).

## Namespaces — mirror the layer model

The root namespace is `tr::`. Sub-namespaces mirror the [six-layer model](../docs/reference/00-overview.md#the-six-layer-model), one per layer:

| Namespace | Layer | Holds |
| --- | --- | --- |
| `tr::mem` | L0 — memory substrate | `mem_backend_t`, `heap_backend_t`, `pool_t`, `borrowed_backend_t`, `io_dir_t`, `alloc_hint_t` |
| `tr::view` | L1 — views & ownership | `segment_t`, `segment_ptr_t`, `view_t`, `rope_t` |
| `tr::` (root) | L2–L4 (until split) | wire codec, graph, transport, bridge — not yet sub-namespaced |

Two hard rules:

1. **Dependencies point up the layers only.** A `tr::view` symbol may name a `tr::mem` symbol; a `tr::mem` symbol naming a `tr::view` symbol is a layering violation — grep for it. The **one sanctioned exception** is `view::segment_t`, the boundary type mutually defined with `mem_backend_t`: the L0 backend interface names it (`alloc` returns `segment_t*`, `destroy` takes one), and that is the *only* legitimate `tr::view` hit inside `tr::mem`. `segment_ptr_t` is **not** a boundary type, so handle-producing helpers (`heap_alloc`, `borrow`, `borrow_const`) live in `tr::view`, not `tr::mem`. See [ADR-0016 §2](../docs/adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md).
2. **Code sub-namespaces never use an error-concept word.** The eight words `frame`, `tlv`, `path`, `schema`, `flow`, `access`, `transport`, `version` are reserved by the `tr::` error-identity namespace ([ADR-0009](../docs/adr/0009-built-in-error-model-tr-concept-namespace.md)). `tr::frame::*` is always an error identity (a string path); never name a C++ namespace after one.

## Type and value naming

Follow the standard-library / kernel aesthetic, not Google/PascalCase.

| Kind | Convention | Examples |
| --- | --- | --- |
| Types (class/struct/enum/alias) | `snake_case` + `_t` suffix | `mem_backend_t`, `segment_ptr_t`, `view_t`, `rope_t`, `io_dir_t` |
| Enum values | `SCREAMING_SNAKE`, scoped (`enum class`) | `io_dir_t::DEVICE_TO_CPU`, `alloc_hint_t::NONE` |
| Functions / methods | `snake_case` | `alloc`, `destroy`, `before_io`, `after_io`, `subview` |
| Member variables | `snake_case_` trailing underscore | `slab_`, `free_head_`, `count_` |
| Constants / `constexpr` | `kCamelCase` (existing) | `kMaxDispatchDepth`, `kNil` |
| Macros (rare; build-config only) | `LIBTRACER_SCREAMING` | `LIBTRACER_NO_ATOMIC` |

The trailing `_t` is safe under `tr::`: C/POSIX reserves global trailing-`_t`, but a namespaced `tr::mem::pool_t` cannot collide with anything POSIX.

### Canonical memory-op verbs (the L0/L1 seam)

| Op | Meaning |
| --- | --- |
| `mem_backend_t::alloc(size, hint)` | Allocate a `segment_t*` (refcount = 1); `nullptr` = backpressure / OOM / unsupported. Returns a **raw** pointer (layering: L0 must not name L1's `segment_ptr_t`). |
| `mem_backend_t::destroy(seg)` | The refcount-hit-zero reclaim hook. **Never user-called**; `segment_ptr_t` invokes it. Never called on a live segment. |
| `mem_backend_t::before_io(seg, io_dir_t)` | Cache prep *before* a DMA transfer (clean/invalidate per direction). No-op on cacheless cores. |
| `mem_backend_t::after_io(seg, io_dir_t)` | Cache reconcile *after* a DMA transfer. The method carries timing; `io_dir_t` carries direction. |
| `segment_ptr_t::adopt(seg)` | Take ownership of an existing reference **without** bumping (wraps `alloc`'s result). |
| `segment_ptr_t::retain(seg)` | Take a **new** shared reference (bumps the count). |
| `segment_ptr_t::reset()` | Drop this reference (acq_rel); fires `destroy` at zero. |

`alloc_hint_t` is an **opaque, backend-private** strong typedef (`enum class alloc_hint_t : std::uint32_t { NONE = 0 }`): a hint's meaning is private to the backend that defines it, there is no cross-backend hint registry, and a hint-ignoring backend accepts any value.

## Documentation — Doxygen, CI-enforced

Every **public** declaration carries a `/** … */` Doxygen block. Use `/** … */` block comments **exclusively** — never `///` line comments; trailing member docs use the `/**< … */` form. The rule balances strictness against boilerplate: `@brief` is mandatory; the argument/return tags appear **only when they add information** the name and type don't already give.

```cpp
/**
 * @brief Allocate a fresh segment of at least @p size bytes (refcount = 1).
 *
 * Owned by the caller, who adopts it via tr::view::segment_ptr_t::adopt.
 *
 * @param hint   Backend-private allocation hint; NONE for "don't care".
 * @retval nullptr  Backpressure (pool full / OOM) or allocation unsupported (MMIO).
 */
[[nodiscard]] virtual segment_t* alloc(std::size_t size,
                                       alloc_hint_t hint = alloc_hint_t::NONE);
```

| Tag | Rule |
| --- | --- |
| `@brief` | **Mandatory** on every public type, member function, enum, enumerator, public field. One line, ends with a period. |
| `@param` / `@return` / `@retval` | **Only when informative.** `@retval nullptr …` (semantic return) is required; `@param size The size.` (restates the obvious) is **forbidden** — that is the boilerplate this rule exists to avoid. |
| `@note` / `@warning` | For contracts: thread-/ISR-safety, `noexcept` rationale, "never called on a live segment", lifetime guarantees. |
| Prose body | Keep it. The `@brief` distills the existing `//` prose; it does not replace it. |

**Enforcement:** a [`Doxyfile`](Doxyfile) with `WARN_IF_UNDOCUMENTED=YES` and `WARN_AS_ERROR=YES` runs over the public headers in CI; an undocumented public symbol is a **red build** — the same gate pattern as the ≤16 KB sentinel. The generated Doxygen is wired into the Sphinx site (`docs/conf.py`) as source references.

## Language profile

- **Floor: C++23** — the standard the MCU toolchains implement. The whole `core/` compiles under `-std=c++23` on every target.
- **C++26: opportunistic only** — behind `__cpp_*` feature-test macros with a C++23 fallback. Nothing in the MCU profile gates on `-std=c++26`.
- **Templating: zero-cost / erased only** above the seam (strong types, concepts, `constexpr`, inlining CRTP); the ownership seam stays virtual + monomorphic (one `segment_t`, virtual `mem_backend_t`). See [ADR-0016 §3](../docs/adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md).
- **MCU profile:** `-fno-exceptions -fno-rtti -Os`, `std::expected`-based `Result<T>`, `LIBTRACER_NO_ATOMIC` single-core. The ≤16 KB Cortex-M0 sentinel is the gate that keeps aggressive templating honest.
