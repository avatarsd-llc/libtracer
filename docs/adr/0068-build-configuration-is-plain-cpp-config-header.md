# Build configuration is plain C++: one config header, no macros

Status: **accepted.** Extends [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) on the *binding* axis: §1's appropriateness rule already says **which** seams dispatch at compile time; this ADR fixes **how** a per-target choice is spelled — as generated, type-checked C++, never as a preprocessor definition. Reaffirms ADR-0047's rejection of `std::function` on per-frame paths (applied to `fwd_router_t` in the companion change). Grounded in the 2026-07-29 metaprogramming survey (below) and the maintainer rulings of the same date: the project is pre-production ([dev stage — the API may move freely](../../.github/GOVERNANCE.md)), must fully serve **both** the single-core MCU and the many-core host, and its C++23 surface should not carry 20th-century configuration mechanics.

## Context

### The survey

Every seam in the tree was judged against ADR-0047 §1 (compile-time dispatch iff **fixed identity** ∧ **hot/size-critical**), with the constraint that the public API stays clean:

| seam | fixed identity? | hot / size-critical? | verdict |
| --- | --- | --- | --- |
| LKV slot policy (#604 — MCU wants write-cheap, host wants read-cheap; measured in `bench_lkv_slot`) | yes — per target | yes — per read *and* per write | **compile-time policy** (follow-on ADR) |
| segment refcount (`LIBTRACER_NO_ATOMIC`) | yes — per target | yes — per clone/drop | **compile-time policy** (modernize the macro; rides the slot mechanism) |
| ACL policy (`LIBTRACER_ACL_FULL`) | yes — per target | size-critical (ADR-0050) | already compile-time; **modernize the `#if` binding** |
| stripe count (`LIBTRACER_VERTEX_LOCK_STRIPES`) | yes — per target | size-critical (N mutexes + condvars) | already compile-time; **modernize the `#ifndef` default** |
| `fwd_router_t` callbacks (`std::function` ×5) | — | per-frame RX path | **fn-ptr + ctx**, per ADR-0047's existing rejection — `subscriber_fn_t` is the L4 precedent |
| memory seam (`block_source_t`, `mem_backend_t`) | **no** — injected per graph/link at runtime | yes | stays virtual (ADR-0039/0065 by design) |
| transport plane | **no** — `:children[]` names a kind as wire data | wiring-frequency | stays runtime (ADR-0047 tried and reverted this) |
| `segment_t`/`view_t`/`rope_t` monomorphization | — | — | rejected twice (ADR-0016/0047 anti-metastasis); not reopened |

Two facts sharpen the LKV row beyond ADR-0064's single-thread picture: at T=1 today's slot reads at 34 ns where hazard pointers read at 8.6 ns but write at 43 ns vs 32 ns — so a write-dominated sensor node and a read-heavy host genuinely want **different slots**, not one compromise slot. And single-core is **not** single-threaded: the ESP32 target compiles `posix_endpoint.cpp`/`loopback.cpp`, both of which spawn tasks, so reclamation can never be compiled out — only rebound.

### The binding problem

The tree carried **three different mechanisms** for the same kind of per-target choice:

- `#if defined(LIBTRACER_ACL_FULL)` selecting `acl_policy_t` (security_acl.hpp);
- `#ifndef LIBTRACER_VERTEX_LOCK_STRIPES` defaulting a macro used to size an `inline constinit` array (vertex.hpp) — where a `-D` mismatch across TUs is an **ODR violation**, as the ESP-IDF component's PUBLIC-definition plumbing has to document at length;
- `LIBTRACER_NO_ATOMIC` changing `segment_t`'s layout, which forces `substrate_test_no_atomic` to recompile a hand-listed set of sources because the differently-shaped library archive cannot be linked.

Macros are invisible to the type system, unscoped, unspellable in `tr::` vocabulary, and their per-TU nature is exactly what creates the ODR hazard. Meanwhile the repo already has the right idiom in a different corner: `builtin_transports.cpp.in` is CMake-`configure_file`d into **plain C++ naming only the enabled transports** — "No preprocessor macros", as its comment says.

## Decision

### 1. One config header, generated as plain C++

A single public header, `libtracer/config.hpp`, holds every per-target compile-time choice as ordinary C++ — `inline constexpr` constants and `using` alias bindings:

```cpp
namespace tr::graph {
inline constexpr std::size_t kVertexLockStripes = 16;
using acl_policy_t = allow_only_policy_t;
}
```

- The **checked-in** `core/include/libtracer/config.hpp` carries the defaults, so raw `-Icore/include` consumers (the Cortex-M0 footprint gate, vendored source drops) build with stock settings and no build-system participation.
- A CMake build **always** generates the same header from `config.hpp.in` into the build tree and prepends that directory to the PUBLIC include path, so the generated file **shadows** the in-tree default. Non-default knobs exist only there.
- **Drift gate:** at configure time, the default-valued render of `config.hpp.in` is byte-compared against the checked-in default; a mismatch is a `FATAL_ERROR`. The template is the single source of truth; the checked-in copy cannot silently rot (the derive-don't-hand-maintain rule).
- The install step ships the *generated* header, so an installed non-default package is self-consistent.
- ESP-IDF: the component generates the same header from its Kconfig values (`CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES`), listed before `core/include` in the component's PUBLIC `INCLUDE_DIRS` — every dependent TU sees one file, which **dissolves** the ODR hazard the PUBLIC compile definition existed to manage.

CMake option names (`LIBTRACER_ACL_FULL`, Kconfig knobs) are unchanged — CI matrices and embedder scripts keep working; only the delivery vehicle changes from `-D` to a generated line of C++.

### 2. Policy selection is an alias in that header, not a template on the public type

> **⚠ SUPERSEDED by [ADR-0070](0070-configuration-is-a-named-traits-type.md).** The
> `basic_graph_t<slot_t>` / explicit-instantiation plan below was **withdrawn on measurement**
> and never built. Threading configuration through a template parameter produces byte-identical
> machine code (eight knob combinations, five optimization levels, two targets), so it buys no
> latency; its one unique capability — two configurations in one binary — forks the
> process-global stripe and hazard tables and so *costs* RAM; and an app-declared traits type
> cannot reach this library's out-of-line translation units at all. The `~3 s per extra
> instantiation` figure below was also measured optimistically (6.60 s locally for `graph.cpp`
> alone, before counting the other four TUs). **What survives from this section is its first
> clause and its last: the public spelling stays untemplated, and a policy is bound by an alias
> in this header.** ADR-0070 keeps that and adds the one thing this section was reaching for —
> a *named* configuration type — without the parameter.


Where §1 of ADR-0047 grants a seam compile-time dispatch and the policy is **stateful** (it lives in a type's layout — the LKV slot, the segment refcount), the public spelling stays untemplated: the implementation type takes the policy parameter (`basic_graph_t<slot_t>`), and `config.hpp` binds the public name (`using graph_t = basic_graph_t<>;` with the default drawn from the alias). Explicit instantiation in the `.cpp` keeps `graph.cpp` a translation unit; CMake emits which instantiations a target opts into (host: both, so one test binary covers both; MCU: one, so no dead code reaches flash). Measured before adopting: the public header costs 1.05 s/TU and `graph.cpp` 3.68 s — a second instantiation adds ~3 s **once per build**, which clears the bar. Stateless policies (ACL) need no template at all — the alias alone binds them, exactly as today.

### 3. Per-frame callbacks are function-pointer + context

`fwd_router_t`'s five `std::function` members sit on the per-frame RX path — the seam ADR-0047 already called "the largest avoidable embedded liability on the delivery path" when it rejected `std::function` receivers. They become `void (*)(void*, …)` + `void* ctx`, the same shape as `graph_t::subscriber_fn_t`. Wiring-frequency `std::function`s (`posix_endpoint_t::start`, `peer_visitor_t`) are **kept** — §1 fails on the hot-path arm there, and the erasure buys real ergonomics at zero measurable cost.

## Considered options

- **Keep the macros.** Rejected: three mechanisms for one concept, an ODR hazard needing a page of comments to manage, and configuration invisible to the type system in a C++23 codebase.
- **A generated header with no checked-in default.** Rejected: breaks every raw-include consumer (the Cortex-M0 footprint gate compiles with a bare `-I`), and makes the source tree un-browsable — the shipped defaults would exist nowhere readable.
- **A checked-in default with no drift gate.** Rejected on this repo's own measured history: every hand-maintained duplicate list in the docs pipeline had silently rotted (chart families 163/323, test suites 21/64). A byte-compare at configure time costs nothing.
- **Templating the public types directly (`graph_t<slot_t>` as the user spelling).** Rejected: API cleanliness was a stated constraint; the `basic_string`/`string` idiom delivers per-target selection with zero churn in the 67 files that spell `graph_t`.
- **Header-only libtracer** (raised in the same session). Rejected on measurement: it moves `graph.cpp`'s ~2.6 s onto every consumer TU (~65 test targets × 2 CI legs), and makes the socket headers public — today `sys/socket.h`/`netinet` appear in 5 `.cpp` and **0** `.hpp`, an isolation worth keeping. ADR-0047 already records that core is consumed as source per target, so header-only would buy no distribution capability that does not already exist.
- **Whole-tree metaprogramming** ("template everything"). Rejected by re-affirming ADR-0047 §1 seam-by-seam — the survey table above is the record; the runtime seams fail on identity-arrives-as-runtime-data, which is their design, not their debt.

## Consequences

- `libtracer/config.hpp` is the **only** place a per-target compile-time choice is spelled; a new knob lands there or it does not land. `#if`-based configuration in public headers is retired.
- `LIBTRACER_ACL_FULL` and `LIBTRACER_VERTEX_LOCK_STRIPES` survive as **CMake/Kconfig option names** but no longer exist as preprocessor symbols; a bare `-DLIBTRACER_VERTEX_LOCK_STRIPES=8` compile flag no longer does anything — the knob is the CMake cache variable (or menuconfig) instead. Pre-production, so no deprecation shim.
- The ESP-IDF component's stripe plumbing simplifies from a PUBLIC compile definition (plus ODR essay) to include-path shadowing.
- `substrate_test_no_atomic`'s hand-listed source recompile becomes unnecessary **once** the refcount macro is rebound through this mechanism (follow-on, rides the slot ADR) — the alias changes the type via one shared header instead of a per-TU `-D`, so one library build per config, no ABI fork inside a build.
- The LKV slot work (#604) lands as `basic_graph_t<slot_t>` + explicit instantiation under this ADR's §2 binding; its own ADR still owes the reclamation-scheme argument (hazard vs epoch), not the mechanism.
- Compile-time cost of the second host instantiation (~3 s, one TU) is accepted and was measured, not assumed.
