# Compile-time dispatch where identity is per-target and the path is hot; runtime dispatch where identity is dynamic or the call is wiring-frequency

Status: accepted. Supersedes [ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) §Decision 3 (the runtime-polymorphic `mem_backend_t` vtable and the "templates never cross the seam" rule); supersedes the receiver-signature spelling of [ADR-0042](0042-refcounted-receiver-seam-view-delivery.md) §1 and fulfills its §4 (the rope overload, now that rope-aware decode is committed); maintainer-ratified 2026-07-04 (revised same day: the net-plane control structures stay runtime-dispatched — see §4). **Amended 2026-07-05: §3's fn-ptr receiver conversion is DEFERRED — `std::function` is retained on the delivery path pending a measured need (see [§Amendment](#amendment-2026-07-05-fn-ptr-receiver-conversion-deferred)).**

## Context

ADR-0016 §3 made the substrate seam runtime-polymorphic — one `mem_backend_t` vtable — and forbade templates through it, rejecting `segment<Backend>` monomorphization because it would metastasize into `view<Backend>`, `rope<Backend>`, and the graph, and would destroy heterogeneous runtime backends (the [ADR-0024](0024-mem-cuda-gpu-backend-heterogeneous-rope.md) heap-header + GPU-payload rope). Those two dangers are real and are **retained as constraints** here. But the vtable was never the only alternative to open templating, and the maintainer direction is explicit: the library must run on ESP32-class MCUs and 128-core ARM/x86 hosts from one codebase, resolving at compile time **what is appropriate to resolve at compile time** — with runtime dispatch retained where it is the genuinely right tool. Separately, the review that triggered this ADR found the per-frame delivery path carries `std::function` type-erasure — a code-size and heap liability on embedded targets independent of any dispatch-speed argument.

## Decision

### 1. The appropriateness rule (normative for every seam)

A seam dispatches at **compile time** only when **both** hold:

- **Fixed identity** — the set of participating types is per-target build configuration, not something that varies with runtime data; and
- **Hot or size-critical path** — the dispatch runs per-frame/per-byte, or the mechanism's *existence* costs code size / heap on MCU profiles.

A seam stays **runtime-dispatched** when either fails: when the identity arrives as runtime data (a `:children[]` creation naming its kind), or when the call runs at wiring frequency (once per connection/child), where an indirect call costs nothing measurable. Link-time module selection (which sources a target compiles in) closes a type set without any metaprogramming and is always in force regardless of dispatch mechanism.

The compile-time mechanism, where the rule grants it, is a **build-time-closed module set**: a per-target type list (config header / CMake-generated) with **tag dispatch** — the boundary types stay uniform and untemplated (`segment_t`, `view_t`, `rope_t`; the anti-metastasis rule of ADR-0016 survives), a tagged object dispatches via a generated `switch` → `static_cast` → direct inlinable call. A **single-member set folds completely** (constant tag, no switch, no-op hooks compile to nothing — the MCU contract); a multi-member set keeps full heterogeneous coexistence (mixed-backend ropes per ADR-0024) behind a predictable switch.

### 2. L0: the backend seam qualifies — concept + constexpr traits + tag dispatch

The backend seam passes the rule (identity per target; `destroy` runs per segment-release, hooks per transfer). `mem_backend` becomes a `concept` (alloc/destroy/space shape) plus compile-time traits replacing prose contracts: `needs_cache_ops`, `is_isr_safe`, `alignment` as `static constexpr` members callers can `static_assert` on. `tr::mem::backend_set` is the per-target module set. Host↔device transfer joins the seam as tag-dispatched `mem::transfer(dst, src, io_dir_t)` with a memcpy default — the CUDA-named free functions (`cuda_copy_from_host`/`_to_host`) are retired, giving `after_io` its first in-tree caller. `alloc_hint_t` keeps its ADR-0016 shape (opaque, backend-private, no shared vocabulary); its first interpreter is an `esp_heap_caps_backend_t` in the esp-idf integration mapping private hints to `MALLOC_CAP_DMA`/`MALLOC_CAP_INTERNAL`. `alignment()` as a virtual is gone (it is a trait).

### 3. The delivery path drops type-erasure; the owning tier delivers a rope

Receiver installation becomes function-pointer + context (`void (*)(void* ctx, …)`), replacing `std::function` on every transport and on `fwd_router`. The honest rationale is **code size, guaranteed no-heap, and freestanding-friendliness** on embedded builds that link transports — an erased *call* costs the same as an indirect call; the erasure *machinery* is what MCU profiles cannot afford. Simultaneously (fulfilling ADR-0042 §4): the owning tier's payload generalizes `view_t` → `rope_t`, with an inline small-buffer chain (~2 links) so a single-link delivery — the contiguous case, i.e. everything ADR-0042 §1 covered — stays zero-alloc and semantically identical. A scattered frame (CAN reassembly group, WS fragmented message) crosses the seam as the rope it already is; transport padding is trimmed by shortening the last link, never by flattening. Span delivery remains the borrowed tier; ADR-0042's "no adapter wraps a borrowed span into an owning view" rule is unchanged.

### 4. The net plane's control structures FAIL the rule — they stay runtime-dispatched

Applying §1 honestly: `fwd_router_t::add_child` and the capability queries (`delivers_views`/owning-tier capability, `bus()`) run **once per connection** — wiring frequency, where virtuals are free. And in-band creation ([ADR-0017](0017-in-band-vertex-creation-controller-orchestration.md)/[ADR-0027](0027-transport-and-connections-are-vertices.md)) receives the connection kind as **runtime data** — an orchestrator writes `"ws"`/`"quic"` into `:children[]` — so factory dispatch-by-name is semantically inherent, not an implementation shortcut. Therefore `fwd_router_t`, `transport_vertex_t`, and `child_registry_t` are **not** templated over any transport list; `transport_t` keeps its virtual capability surface (with the §3 receiver-signature change); `send()` stays virtual (per-frame but syscall-dominated — the indirect call is noise). The transport type set is closed the way it already is today: **link time** — the sources a target compiles and the factories it registers. No TMP in the net plane's control structures.

### 5. Enforcement: the size sentinels gate the doctrine

ADR-0016 §3 cited a "≤ 16 KB stripped Cortex-M0 sentinel" that does not exist in CI (only the esp32 full-node gate of `.github/workflows/esp-idf.yml` does). That sentinel now lands as part of this work — an `arm-none-eabi-g++ -std=c++23 -Os -fno-exceptions -fno-rtti` required-modules build with a hard size gate — and both sentinels are the standing referee: template/metaprogramming techniques are admissible exactly as far as the gates stay green. Note the referee cuts both ways: it catches vtable/erasure bloat *and* template-instantiation bloat.

## Considered options

- **Keep ADR-0016 §3 (one vtable) everywhere.** Rejected: leaves per-segment-release indirect dispatch and `std::function` machinery on targets that could resolve them statically, and forecloses the single-backend MCU fold.
- **Whole-stack compile-time (transport plane included).** Adopted briefly, then rejected on the appropriateness rule: templating `fwd_router`/`transport_vertex`/`child_registry` over a transport list buys zero runtime (wiring-frequency calls) while costing readability, per-target instantiation, and fighting the inherently name-keyed `:children[]` creation path.
- **Open monomorphization (`segment<Backend>`).** Still rejected, same grounds as ADR-0016: type-system metastasis into L1+ and loss of heterogeneous ropes. The module set captures compile-time dispatch *without* this.
- **Runtime-pluggable binary module ABI (dlopen-style).** Rejected: never used, contradicts the source-first embedded reality; interop between implementations is the wire, not an ABI ([ADR-0013](0013-v1-scope-boundaries.md)).
- **`std::function` receivers retained for ergonomics.** Rejected: the erasure machinery (code size, heap capability, exception paths) is the largest avoidable embedded liability on the delivery path; fn-ptr + context is the freestanding-friendly equivalent.

## Consequences

- `core/` is consumed as source, per target — there is no prebuilt-library-accepts-unknown-backends deployment. (There never was.)
- TUs that dispatch through the L0 seam see the target's `backend_set` header; compile time grows bounded by set size. Net-plane TUs are unaffected.
- The uniform boundary types, the L0↔L1 namespace rule, heterogeneous ropes, backend-private `alloc_hint_t`, and ADR-0042's ownership semantics (injected backends, backpressure-not-OOM, `store_ref_min_bytes`) all survive unchanged.
- ADR-0016 §Decisions 1–2 (CPU-mediated scatter-gather; namespace = layer) remain in force; only its §3 dispatch mechanism is superseded, and only where §1's rule grants compile-time dispatch.
- CONTEXT.md's **module set** entry names the two realizations explicitly: tag-dispatched at L0; link-time in the net plane.
- Reference docs [09](../reference/09-memory-substrate.md)/[10](../reference/10-module-catalog.md) §module ABI are updated to describe the concept-and-set seam as the reference implementation's mechanism.
- Future seams cite §1's appropriateness rule instead of re-litigating vtable-vs-template per case.

## Amendment (2026-07-05): fn-ptr receiver conversion deferred

§3's replacement of `std::function` receivers with `void (*)(void* ctx, …)` + context is **deferred**; `std::function` is retained on the delivery path until a gated profile demonstrates it is the bottleneck. The other half of §3 — the owning tier delivering a `rope_t` (fulfilling ADR-0042 §4) — is **unaffected** and still planned.

The re-examination (against §1's own "hot **or** size-critical" test and the maintainer's measurement-first doctrine):

- **Speed** — neutral. A `std::function` call is one extra indirection over a raw fn-ptr call (invoke the type-erased thunk, which calls the target); against per-frame work that is noise. §3 already conceded "an erased *call* costs the same as an indirect call," so the appropriateness rule's "hot path" limb does not, by itself, justify the change.
- **Heap** — weak in practice. libstdc++ `std::function` has small-buffer optimization; a `[this]` capture (one pointer — the dominant receiver shape here) lives inline, **no heap**. Heap allocation only occurs for a capture wider than the SBO (≈2 pointers), which this path does not use.
- **Code size** — the only real cost: each distinct `std::function<Sig>` instantiates an invoke thunk plus a copy/move/destroy manager, on the order of **0.5–2 KB of flash** across the delivery path's signatures.
- **But that cost is unmeasured on any gated profile.** The receiver/delivery path (`fwd_router`, `transport_*`, the graph runtime) is **not** part of the ≤16 KB Cortex-M0 P0 sentinel (`sentinel_node.cpp` is L0/L1 + codec + path only). No sentinel or CI gate currently measures a graph+transport MCU node, so the fn-ptr saving is speculative for every profile we referee — while the ergonomic cost (each install site becomes a static trampoline + context, and can no longer be a capturing lambda) is paid everywhere, immediately.

Per §1's own rule ("size-critical" must be *demonstrated*, not assumed) and the measurement-first doctrine, the conversion is not justified today. **Re-open it** when a graph+transport footprint gate exists and shows `std::function` pushing that node over its budget; until then `std::function` stays — it is speed-equal, heap-free for the captures in use, ergonomic, and binds a method (via `ctx`) as readily as a free function.

## Amendment (2026-07-08): §3 realized for the receiver seam via `receiver_slot_t`

The 2026-07-05 deferral is **superseded for the transport receiver seam** — not by a
new footprint measurement, but because the 2026-07-08 architecture review (maintainer-
grilled) surfaced what the deferral's accounting had missed: every adapter was paying a
**mechanism cost** to keep `std::function` viable on the delivery path. The installed
router closure exceeds the SBO (it captures the router *and* the child name), so all
six adapters carried a per-adapter `rx_dirty_` snapshot dance (mutex + atomic flag +
loop-thread-local copies) purely to dodge the per-frame heap copy — six divergent
implementations of one concept, and a residual "loop must re-snapshot" invariant in
every adapter's interface. With the seam deepened into one `receiver_slot_t`
(`libtracer/receiver_slot.hpp`) holding trivially-copyable `{fn-ptr, ctx}` pairs, the
dance is not centralized but **deleted** — a per-frame snapshot is a four-word copy
under an uncontended lock — and the ergonomic objection is answered by a template
lvalue-callable sugar on the setters (call sites keep writing lambdas; the slot binds
them by address, zero erasure). The rest of the 2026-07-05 analysis stands: this is
not a speed argument, and `std::function` remains appropriate elsewhere (the router's
`on_reply`/`on_inbound` hooks, wiring-frequency callbacks).
