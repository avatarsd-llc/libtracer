# Configuration is a named traits type, bound once — not a template parameter

Status: **accepted.** Supersedes [ADR-0068](0068-build-configuration-is-plain-cpp-config-header.md) §2 (the "`basic_graph_t<config_t>` follow-on"), which is hereby **withdrawn as a plan** on measurement. Keeps everything else ADR-0068 decided: configuration stays generated, type-checked plain C++, delivered through one header, never a preprocessor definition.

## Context

ADR-0068 landed the config header and sketched a follow-on: thread the configuration through the library as a **template parameter** — `basic_graph_t<config_t>`, `basic_vertex_t<config_t>` — with an application declaring one binding via `using`. On 2026-07-30 that follow-on was ruled to land *before* the remaining sizing work, on evidence supplied in this repo's own reporting.

**That evidence was wrong, and the error was mine.** It read:

> A global `inline constexpr` count cannot fix [the stripe table's 896 B of padding], because the *alignment* is part of the type. A traits parameter can. That is the counterexample ADR-0068's mechanism structurally cannot express.

The first clause is true and the second does not follow. The *count* cannot reach the alignment — but the alignment is itself expressible as a config constant, and the type stays one type per build. Measured on rv32 with the ESP toolchain (GCC 15.2, `-Os -fno-exceptions -fno-rtti`, compiling the real `core/src/graph.cpp`), changing `alignas(64)` to `alignas(kCacheLineBytes)`:

| | stripe table `.bss` | TU `.bss` + `.sbss` |
| --- | ---: | ---: |
| `alignas(64)` | 1,024 B | 1,200 B |
| knob = 0 | **128 B** | **304 B** |

−896 B, **zero templates**. Shipped in #659 without any refactor. The claimed counterexample was not one.

### What was then measured properly

Four probes, each attacked by three adversarial verifiers, on the real tree:

**The mechanism is latency-neutral.** Threading the free stripe entities through a template parameter — all 46 call sites, no forwarding wrapper — produces **identical machine code**: host g++ 14.3 at `-O0/-O1/-O2/-O3/-Os` and rv32 g++ 15.2 `-Os`, across eight knob combinations. Reproduced independently with name-blind comparators and a deliberate one-bit mutation as a control, so the null is not a normalisation artifact. There is nothing at runtime for the mechanism to move: no dispatch to devirtualize, no indirection to hoist.

**The premise underneath is weaker still.** Ablating the knob's *constness* — putting a `volatile` behind the `%` in the stripe index, i.e. giving up compile-time config entirely on that path — is a non-difference (23/40 paired rounds). Constant-folding a configuration value is worth approximately nothing on these paths. Whatever there is, `inline constexpr` already delivers.

**Optimizer visibility is a different axis, and templating cannot reach it.** `vertex_t` is *already* entirely header-defined — there are no out-of-line `vertex_t::` member definitions — so templating exposes nothing that was hidden. `graph_t` is the only seam where visibility could change, and ADR-0068 §2 keeps it out-of-line via explicit instantiation *by construction*. Going header-only instead measures at zero: GCC declines to inline `graph_t::read` even with the definition in the same TU (25/40, a non-difference), and forcing it with `always_inline` is a **loss** (8/40, z = −3.8 — the i-cache signature). The one arm that wins is **LTO** (38/40 read, 36/40 write), which is a CMake flag with no API churn.

**The one unique capability costs the thing it was meant to save.** Two configurations in one binary is the only thing an alias cannot express. But `vertex_stripes`, `vertex_stripe_cv` and `detail_hp::registry()` are process-global *free entities*, not members, so per-config instantiation forks them: two bindings with *numerically identical* knobs still get two full stripe tables and two hazard registries, because the compiler cannot merge distinct types. Against a mandatory RAM diet, the capability is a regression.

**And the ruling's own spelling does not link.** "The app declares the config globally with `using`" cannot reach the library's five out-of-line translation units (`graph.cpp` alone is 2,138 lines); an app preset compiles clean and dies at link. The instantiable set closes at *libtracer* build time, so a traits parameter would **layer on** the generated header, not replace it — which also removes the hoped-for simplification of deleting the generator.

**Directionally negative, in one place.** ADR-0068 §2's `extern template` half turns a fully-inlined seven-instruction stripe lookup into `auipc/jalr` plus a 16-byte frame on rv32, and reaches an instantiation-declared static through the GOT on host PIE.

### The argument that survived

One pro is real and was not raised before the measurement: a template parameter would let **one** host translation unit `static_assert` every preset's `sizeof(vertex_t)`. That mattered, because the ceiling was in `core/tests/vertex_size_test.cpp` — which gated exactly the one configuration the build had chosen, and **never the 32-bit arm at all**, since no CI leg cross-compiled that test. `sizeof(vertex_t)` on rv32 is *exactly* `kMax32` = 80, with zero headroom, so the ceiling that mattered most was the one not being checked.

That gap has a cheaper and stricter fix, and it is part of this decision.

## Decision

**1. The configuration is one named type.** `tr::graph::default_config_t` carries every compile-time knob — counts, widths, ceilings, and the policy type aliases. `using config_t = default_config_t;` is the single binding, and it is the one thing an application overrides. The loose names the library and its consumers already use (`kVertexLockStripes`, `lkv_slot_t`, …) remain, **derived** from `config_t`, so introducing the type moved no call site.

**2. It is bound once per build. It is NOT a template parameter.** No `basic_graph_t<Cfg>`, no `basic_vertex_t<Cfg>`, no explicit-instantiation lists. Per the measurements above: the parameter buys no latency, its unique capability costs RAM, and it cannot be reached by an app-side declaration anyway.

**3. An application declares its configuration by inheritance and one alias:**

```cpp
struct my_node_config_t : tr::graph::default_config_t {
    static constexpr std::size_t kCacheLineBytes = 0;   // single-core: no false sharing
};
using config_t = my_node_config_t;
```

Inheriting means a knob added later does not break the preset — it inherits the new default rather than failing to compile. Delivery is unchanged: the build renders this header ahead of the checked-in default on the include path, exactly as ADR-0068 specified.

**4. The RAM-diet ceilings move into the configuration, and the gate moves into the header.** `config_t::kMaxVertexBytes64` and `kMaxVertexBytes32` are members; the `static_assert`s live in `vertex.hpp` beside the type they constrain. A `static_assert` in a header is evaluated by **every translation unit that includes it** — so every target, every configuration and every consumer's build checks its own binding, for free, with no CI job to remember. The ESP-IDF legs compile `vertex_t` for esp32c6 and esp32c3 on every PR, so the 32-bit arm this repo could never check is now checked on every pull request, at zero cost.

This is strictly stronger than what the template parameter offered. Templating would have gated the presets somebody remembered to name in one host TU; a header assert gates whatever anyone actually builds.

## Consequences

- **Zero codegen change**, verified rather than assumed: `core/src/graph.cpp` compiled from `main` and from this change gives byte-identical disassembly and identical section sizes on host x86-64 `-O2` and rv32 `-Os`. (The comparison initially showed one byte of `.rodata` difference; that was an assertion string carrying the two scratch worktrees' differing directory-name lengths, and it vanishes when both are built from equal-length paths.)
- **Zero call-site churn.** The derived names are the ones in use.
- **The 32-bit ceiling is enforced for the first time.** It has no headroom, so the next inlined 32-bit member is a build failure by design — which is what the gate is for.
- **A ceiling is now raised in the configuration**, where it appears in a diff, rather than by editing a test until it passes.
- **The `configure_file` + drift-gate machinery stays.** ADR-0068's two-renderer footgun is unchanged by this decision; the hoped-for deletion of the generator was contingent on the app-declared traits type, which does not work. *(Overtaken by events: [ADR-0068 §Erratum 1](0068-build-configuration-is-plain-cpp-config-header.md) deleted both — core's in #1142, the ESP-IDF component's in #1244 — by making the checked-in header the only copy and overriding it with a small `config_override.hpp` fragment. This ADR's own contribution, the one named traits type, is what made that fragment possible: a knob outside `config_t` cannot be reached by one.)*
- **Two configurations in one binary remain unavailable.** Nothing in this codebase has asked for it, and §Context prices it as a RAM regression if it ever does.

## Considered options

**Thread the traits type as a template parameter (ADR-0068 §2's plan).** Rejected on measurement — the whole of §Context. Additionally: `vertex_t` templated is 306 occurrences across 37 files (96 in `graph.cpp`); `vertex_handle_t`, `fwd_router_t`, `op_resolver_t` and `transport_vertex_t` follow because they hold a `graph_t&`; `graph.hpp`'s forward declaration of `graph_t` cannot survive, since an alias to a template instantiation is not forward-declarable; and per-preset instantiation lists would add a failure mode with no gate — a mismatch between what CMake requests and what was instantiated surfaces as an undefined reference in a consumer's tree.

**Leave the configuration as loose declarations.** Rejected: it answers none of the maintainer's ask. The configuration was not nameable, not passable to a test, and the ceilings sat in a test file that gated one binding.

**Keep the ceilings in `vertex_size_test.cpp` and add a cross-compiled CI leg.** Rejected as strictly worse: it costs a new job, still gates only the presets that job names, and leaves the ceiling somewhere a contributor can quietly raise. The header assert costs nothing and cannot be forgotten.

**Make the ceilings CMake cache variables.** Rejected: it would let a build raise the RAM ceiling from the command line, which is exactly the decision that should require a diff.
