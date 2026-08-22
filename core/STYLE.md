# `core/` style — naming, namespaces, documentation

The mechanical conventions for the C++23 reference implementation. The *architectural* rationale (why the substrate is shaped this way) is [ADR-0016](../docs/adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md); this file is the "how to spell it." C/C++ also passes `clang-format` (config at [`.clang-format`](../.clang-format)).

## Namespaces — mirror the layer model

The root namespace is `tr::`. Sub-namespaces mirror the [six-layer model](../docs/reference/00-overview.md#the-six-layer-model), one per layer:

| Namespace | Layer | Holds |
| --- | --- | --- |
| `tr::mem` | L0 — memory substrate | `mem_backend_t`, `heap_backend_t`, `pool_t`, `borrowed_backend_t`, `io_dir_t`, `alloc_hint_t` |
| `tr::view` | L1 — views & ownership | `segment_t`, `segment_ptr_t`, `view_t`, `rope_t` |
| `tr::wire` | L2/L3 — frame + TLV codec | `tlv_t`, `opt_t`, `type_t`, `err_t`, `trailer_t`, `crc_t`, `decode`/`encode` (`decode(view_t)` is the L1↔L2 cast), `grammar::parse_header` |
| `tr::graph` | L4 — graph runtime | `graph_t`, `vertex_t`, `path_t`, `status_t`, `result_t`, `delivery_policy_t`, `handlers_t`, `role_t`, `subscriber_t` |
| `tr::net` | transport plane | `transport_t`, `peer_id_t`, `conn_settings_t`, `fwd_router_t`, `child_registry_t`, `route_handle_t`, `transport_vertex_t`, `udp_transport_t`/`tcp_transport_t`, `loopback_channel_t`/`loopback_endpoint_t` |

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
| Constants / `constexpr` | `kCamelCase` (existing) | `kInline`, `kNil` |
| Macros (rare; build-config only) | `LIBTRACER_SCREAMING` | `LIBTRACER_NO_ATOMIC` |

The trailing `_t` is safe under `tr::`: C/POSIX reserves global trailing-`_t`, but a namespaced `tr::mem::pool_t` cannot collide with anything POSIX.

### Canonical memory-op verbs (the L0/L1 seam)

| Op | Meaning |
| --- | --- |
| `mem_backend_t::alloc(size, hint)` | Allocate a `segment_t*` (refcount = 1); `nullptr` = backpressure / OOM / unsupported. Returns a **raw** pointer (layering: L0 must not name L1's `segment_ptr_t`). |
| `mem_backend_t::destroy(seg)` | The refcount-hit-zero reclaim hook. **Never user-called**; `segment_ptr_t` invokes it. Never called on a live segment. |
| `mem_backend_t::before_io(seg, io_dir_t)` | Cache prep *before* a DMA transfer (clean/invalidate per direction). No-op on cacheless cores. |
| `mem_backend_t::after_io(seg, io_dir_t)` | Cache reconcile *after* a DMA transfer. The method carries timing; `io_dir_t` carries direction. |
| `mem::transfer(seg, host, io_dir_t)` | Tag-dispatched host↔device byte-move (ADR-0047 §2): host `memcpy` bracketed by the cache hooks when `needs_cache_ops`; a `DEVICE` backend routes to its device copy. Retired `cuda_copy_from_host`/`_to_host`. |
| `segment_ptr_t::adopt(seg)` | Take ownership of an existing reference **without** bumping (wraps `alloc`'s result). |
| `segment_ptr_t::retain(seg)` | Take a **new** shared reference (bumps the count). |
| `segment_ptr_t::reset()` | Drop this reference (acq_rel); fires `destroy` at zero. |

`alloc_hint_t` is an **opaque, backend-private** strong typedef (`enum class alloc_hint_t : std::uint32_t { NONE = 0 }`): a hint's meaning is private to the backend that defines it, there is no cross-backend hint registry, and a hint-ignoring backend accepts any value.

## Introspection — one vocabulary for every bounded resource

Every bounded resource in the tree (a block source, a pool, a label table, a link's TX slots, a ring) answers the same questions, and before [#1503](https://github.com/avatarsd-llc/libtracer/issues/1503) each answered them with its own spelling and its own polarity. These are the words. A new accessor or stats field uses them; an existing one that disagrees is renamed or aliased to them.

| Noun | Meaning |
| --- | --- |
| `capacity` | The **effective** ceiling — the value that actually produced the refusal, never the compile-time default. A node must be able to REPORT the ceiling it hit instead of quoting a constant (#1160). |
| `in_use` | Occupancy, **always used-polarity**. Free is derived (`capacity - in_use`), never reported as the primary. |
| `peak` | High-water mark of `in_use` since construction. |
| `refused` | Requests answered **by value** — a degrade or a `BACKPRESSURE`/`nullptr`. The caller was told. |
| `dropped` | Work **lost** — nobody was told, which is exactly why it must be counted. |
| `largest_refused` | For variable-sized requests: the biggest one that was refused. The tail is what refuses, so the median tells a sizing operator nothing (#1492). |

**Used-polarity is not negotiable.** `pool_t::available()` (free slots) is the one shipped free-polarity accessor; it stays for compatibility and is **not** the spelling new code adds. Where both exist, `in_use` is the primary and `available()` is the derived legacy name.

`refused` and `dropped` are separate counters, never one total. The degrade/loss axis is precisely what an operator needs in order to decide whether a number is a sizing problem or a correctness problem.

### The snapshot-coherence clause (stated here once; cite it, don't restate it)

> Introspection counters are **monotonic since construction and sampled without synchronization**. A multi-field snapshot is read one relaxed load at a time, so a reader racing a mutating thread may see a torn total. That is deliberate: making the snapshot atomic would put a lock (or a seq-lock's fences) on the refusal path in order to serve a diagnostic. The intended use is the **difference between two snapshots**, never the instant.

Doc blocks in the tree say this in prose at `transport_drop_stats_t`, `graph_t::delivery_drops`, and `httpd_ws_link_t::stats` — those are the historical statements. New surfaces cite **this clause** (`core/STYLE.md` §Introspection) rather than paraphrasing it a fourth way.

### The counting doctrine (where the counters may and may not be bumped)

1. **Failure path only.** A counter is incremented on the refusal/exhaustion/drop arm and *nowhere else*. The success arm of a hot allocation, a delivery, or a send must not gain a single instruction — [ADR-0039](../docs/adr/0039-pmr-memory-model-host-aligned-allocation.md)'s `bench_forward_heap == 0` steady-state hop and [ADR-0067](../docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md)'s rv32 text figure are the standing referees, measured per PR.
2. **Counted, never enforced.** Nothing in the library reads its own counters; a deployment chooses whether to alarm (`graph_t::delivery_drops`).
3. **Per-seam, never aggregated.** [ADR-0079](../docs/adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md)'s amendment rules "no composition is the default" — there is no node-wide census object to fold into ([ADR-0067](../docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md) measured the cacheline storm a shared one causes).
4. **One event, one counter.** A refusal with a single operator symptom stays fused (`route_handle_t`'s slab/count refusals); a cause an operator would *size against* gets its own counter. Cross-plane double counting is a defect: `graph_t::count_external_drop` is the one door the net plane counts through, and its exclusions exist to keep one refusal from being tallied twice.
5. **Storage, not synchronization.** Prefer plain counters guarded by the discipline the resource already has (single-owner, or its existing `Sync` policy / mutex) over adding atomics. On rv32 a 64-bit atomic is not lock-free — it takes a hidden libatomic lock per access, which is strictly worse than the lock already held. Where an atomic is genuinely required it is word-sized (`std::size_t`/`std::uint32_t`) and relaxed.
6. **Tuning knobs are not limits.** `kMaxInlineIov`, `kInlineFanout` and friends select a strategy; exceeding one is not a refusal and they report nothing (CONTEXT.md §Resource bound).

### Names this vocabulary deliberately does *not* unify

- `pool_source_t::overflowed()` counts a **recycling degrade** — a freed block whose size class did not fit the injected class table, so the block stays carved. It is not an allocation refusal, and `stats().refused` is. The two must not be conflated; a source can overflow with zero refusals and refuse with zero overflows.
- `ok()` spans four different severity levels across the tree; it is a predicate, not part of this vocabulary.
- `reserve` names three unrelated concepts (a container's growth, a ring's byte reservation, a policy's headroom); it is not an introspection noun.

## Documentation — Doxygen, CI-enforced

Every **public** declaration carries a `/** … */` Doxygen block. Use `/** … */` block comments **exclusively** — never `///` line comments; trailing member docs use the `/**< … */` form. The rule balances strictness against boilerplate: `@brief` is mandatory; the argument/return tags appear **only when they add information** the name and type don't already give.

**Scope — doxygen-capable comments EVERYWHERE (maintainer directive, 2026-07-08).** The
`/** @brief … */` form is not just for CI-gated public headers: it applies to every
**entity-attached** comment in the codebase —

- `.cpp` implementation files: the file header is a `/** @file @brief … */` block; every
  function definition, file-local type, anonymous-namespace helper, and static table gets a
  `/** @brief … */` block; logical section dividers use `/** @name … */`-style blocks.
- The other-language cores follow the same doctrine in their native doc syntax that is
  `/** */`-shaped: **Rust** uses `/** … */` rustdoc block comments (never `///`),
  **TypeScript** uses JSDoc `/** … */`.

The one exception: **statement-level comments inside function bodies stay `//`** — a
free-floating `/** */` block attaches to no entity, and orphan doc blocks trip
`WARN_AS_ERROR` once a file enters the Doxyfile INPUT. If a body comment documents a
*member or variable declaration*, prefer the trailing `/**< … */` form.

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
