# Build-time-closed module sets: seams dispatch at compile time, and runtime type-erasure leaves the hot paths

Status: accepted. Supersedes [ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) §Decision 3 (the runtime-polymorphic `mem_backend_t` vtable and the "templates never cross the seam" rule); supersedes the receiver-signature spelling of [ADR-0042](0042-refcounted-receiver-seam-view-delivery.md) §1 and fulfills its §4 (the rope overload, now that rope-aware decode is committed); maintainer-ratified 2026-07-04.

## Context

ADR-0016 §3 made the substrate seam runtime-polymorphic — one `mem_backend_t` vtable — and forbade templates through it, rejecting `segment<Backend>` monomorphization because it would metastasize into `view<Backend>`, `rope<Backend>`, and the graph, and would destroy heterogeneous runtime backends (the [ADR-0024](0024-mem-cuda-gpu-backend-heterogeneous-rope.md) heap-header + GPU-payload rope). Those two dangers are real and are **retained as constraints** here. But the vtable was never the only alternative to open templating, and the maintainer direction is now explicit: the library must run on ESP32-class MCUs and 128-core ARM/x86 hosts from one codebase, resolving **everything resolvable at compile time** — platform selection, dispatch, capability queries, no-op hooks — with runtime indirection reserved for what is genuinely dynamic. Separately, the review that triggered this ADR found the per-frame delivery path carries `std::function` type-erasure — a heavier runtime-indirection cost than the vtable it sits beside.

## Decision

### 1. The seam mechanism is a build-time-closed **module set** with tag dispatch

Each pluggable seam names, per target, the closed set of module types linked into that build — a compile-time type list (a config header / CMake-generated), e.g. `tr::mem::backend_set = ⟨heap_backend_t, pool_t⟩` on a leaf, `⟨heap, pool, borrowed, cuda⟩` on a host build. The boundary types stay **uniform and untemplated** (`segment_t`, `view_t`, `rope_t` — the anti-metastasis rule of ADR-0016 survives): a `segment_t` carries its backend instance pointer plus a small **tag**, and seam operations (`destroy`, `transfer`, cache hooks, `space`) dispatch by a generated `switch` over the set — tag → `static_cast` to the concrete type → direct, inlinable, non-virtual call.

- A **single-member set folds completely**: the tag is a constant, the switch disappears, a no-op hook on a cacheless core compiles to nothing. This is the MCU contract.
- A **multi-member set** keeps full heterogeneous coexistence — mixed-backend segments chain in one rope exactly as under ADR-0024; dispatch is a predictable switch instead of an indirect call.
- Adding a platform module = appending its type to the target's set. The set is the seam's adapter mechanism; core is never edited.

### 2. The L0 backend contract becomes concept + constexpr traits

`mem_backend` is a `concept` (alloc/destroy/space shape) plus compile-time traits that replace prose contracts: `needs_cache_ops`, `is_isr_safe`, `alignment` as `static constexpr` members callers can `static_assert` on. Host↔device transfer joins the seam as the tag-dispatched `mem::transfer(dst, src, io_dir_t)` with a memcpy default — the CUDA-named free functions (`cuda_copy_from_host`/`_to_host`) are retired, giving `after_io` its first in-tree caller. `alloc_hint_t` keeps its ADR-0016 shape (opaque, backend-private, no shared vocabulary); its first interpreter is an `esp_heap_caps_backend_t` in the esp-idf integration mapping private hints to `MALLOC_CAP_DMA`/`MALLOC_CAP_INTERNAL`. `alignment()` as a virtual is gone (it is a trait).

### 3. The delivery path drops type-erasure; the owning tier delivers a rope

Receiver installation becomes function-pointer + context (`void (*)(void* ctx, …)`), replacing `std::function` on every transport and on `fwd_router` — nothing on the per-frame path allocates or erases types. Simultaneously (fulfilling ADR-0042 §4): the owning tier's payload generalizes `view_t` → `rope_t`, with an inline small-buffer chain (~2 links) so a single-link delivery — the contiguous case, i.e. everything ADR-0042 §1 covered — stays zero-alloc and semantically identical. A scattered frame (CAN reassembly group, WS fragmented message) crosses the seam as the rope it already is; transport padding is trimmed by shortening the last link, never by flattening. Span delivery remains the borrowed tier; ADR-0042's "no adapter wraps a borrowed span into an owning view" rule is unchanged.

### 4. The transport **type set** closes per target; instances stay runtime

`tr::net::transport_set` closes the transport types per build; capability queries (`delivers_views`→owning-tier capability, `bus`) become constexpr traits folded at compile time in `fwd_router`/`transport_vertex` dispatch. What stays dynamic is exactly what is semantically dynamic: **instances**. Connections are created at runtime by in-band `:children[]` writes ([ADR-0017](0017-in-band-vertex-creation-controller-orchestration.md)/[ADR-0027](0027-transport-and-connections-are-vertices.md)); `child_registry_t` remains a runtime table of instances — of compile-time-closed types.

### 5. Enforcement: the size sentinels gate the doctrine

ADR-0016 §3 cited a "≤ 16 KB stripped Cortex-M0 sentinel" that does not exist in CI (only the esp32 full-node gate of `.github/workflows/esp-idf.yml` does). That sentinel now lands as part of this work — an `arm-none-eabi-g++ -std=c++23 -Os -fno-exceptions -fno-rtti` required-modules build with a hard size gate — and both sentinels are the standing referee: template/metaprogramming techniques are admissible exactly as far as the gates stay green.

## Considered options

- **Keep ADR-0016 §3 (one vtable).** Rejected by maintainer direction: it leaves per-frame indirect calls and type-erasure on targets that could resolve them statically, and forecloses compile-time platform specialization the ESP32↔many-core spread requires.
- **Open monomorphization (`segment<Backend>`).** Still rejected, same grounds as ADR-0016: type-system metastasis into L1+ and loss of heterogeneous ropes. The module set is precisely the design that captures compile-time dispatch *without* this.
- **Runtime-pluggable binary module ABI (dlopen-style).** Rejected: never used, contradicts the source-first embedded reality; interop between implementations is the wire, not an ABI ([ADR-0013](0013-v1-scope-boundaries.md)).
- **`std::function` receivers retained for ergonomics.** Rejected: heap-capable type-erasure on the per-frame path is the largest runtime-indirection cost in the stack; fn-ptr + context is the freestanding-friendly equivalent.

## Consequences

- `core/` is consumed as source, per target — there is no prebuilt-library-accepts-unknown-backends deployment. (There never was.)
- Every TU that dispatches through a seam sees the target's module-set header; compile time grows bounded by set size.
- The uniform boundary types, the L0↔L1 namespace rule, heterogeneous ropes, backend-private `alloc_hint_t`, and ADR-0042's ownership semantics (injected backends, backpressure-not-OOM, `store_ref_min_bytes`) all survive unchanged.
- ADR-0016 §Decisions 1–2 (CPU-mediated scatter-gather; namespace = layer) remain in force; only its §3 dispatch mechanism is superseded.
- CONTEXT.md gains **module set** as the canonical term (distinct from the wire type-code registry, the error registry, the device controller catalog, and the network manifest).
- Reference docs [09](../reference/09-memory-substrate.md)/[10](../reference/10-module-catalog.md) §module ABI are updated to describe the concept-and-set seam as the reference implementation's mechanism.
