# Folding `vertex_ext_t` into the `vertex_t` allocation

libtracer will not fold the `vertex_ext_t` extension block into the `vertex_t`
allocation (neither unconditionally nor conditionally-at-registration). The
separate, lazily-allocated, atomically-published ext block — the status quo after
[#551](https://github.com/avatarsd-llc/libtracer/issues/551) routed it through the
injected `memory_resource` — is preserved. This is the last surviving fragment of
[#571](https://github.com/avatarsd-llc/libtracer/issues/571) (fragments 1 and 3 were
resolved: 1 subsumed by #551, 3 shipped as #573) and it descends from the arena
refutation in [`per-vertex-allocation-arena.md`](per-vertex-allocation-arena.md).

## The premise

Fragment 2 argued: fold `vertex_ext_t` (~112 B, the largest single per-vertex block)
into the `vertex_t` allocation *when the vertex identity is known at registration*,
saving one allocation per non-default vertex plus locality, at the cost of the lazy
`ensure_ext` fallback branch.

## Why this is out of scope

**`ext_` is not merely an allocation — it is a lock-free hot-path synchronisation
signal.** `vertex_t::ext_` is an `std::atomic<vertex_ext_t*>`, `null` for a plain
leaf, CAS-published once and never cleared (ADR-0057 insert-only), and **read
lock-free on the hot path** — `handlers` loads it with no stripe lock
(`vertex.hpp:2358`, `:2435`). The `null` value *is* the "no ext" signal. Folding
breaks or taxes this:

- **Unconditional inline** destroys the `null`-means-no-ext signal (a validity flag
  must replace it) and bloats *every* bare leaf by ~112 B — reversing the exact
  program RFC-0022 §3.B advanced when it *removed* a condition to keep strictly more
  vertices ext-less. A bare leaf is *"the overwhelming majority of an MCU node's
  vertices"* (the code's own words); the rv32 RAM discipline forbids the bloat.
- **Conditional inline** (fragment 2's literal proposal) forces two vertex layouts —
  inlined-ext and lazy-pointer-ext — so every hot `handlers` read must first branch on
  *"is this one inlined?"*. That puts a branch on a lock-free read in a latency-first
  core, **paid forever, to save a one-time cold allocation**. Wrong trade.

**The residual is irreducible.** ext is materialised not only at registration but at
**runtime** — a plain vertex that later gains `:acl`, an edge, or a stream append
calls `ensure_ext()` then (the CAS exists precisely because registration and
field-writes race). "A vertex can gain ACL / a subscription / a stream at runtime" is
the *feature*, not an artifact, so ext-need cannot be made compile-time-known per
vertex. Unlike a residual that can be designed away, this one cannot — so there is no
clean single-target layout that is zero-leaf-cost **and** zero-indirection **and**
single-allocation at once.

**The remaining win does not justify the machinery.** After #551 the ext block is
drawn from the pooled `memory_resource` (~3–8 ns, no glibc header left to collapse),
so folding saves only one cold pooled acquire plus one pointer-chase on the *minority*
ext-bearing vertices' `handlers` leg. Buying that by templating `vertex_t` — the
central graph type — into two maintained layouts is machinery disproportionate to the
residual, exactly the trade the RAM/latency discipline refuses.

## The reconsider-trigger

Revisit **only** if the per-configuration allocation-store bench
([#941](https://github.com/avatarsd-llc/libtracer/issues/941)) shows the ext-bearing
`handlers`-load indirection is a *measured* host hot-path cost that the NARROW
configuration actually pays. If it is, the answer is a **build-time-closed layout
policy** — MCU build all-`separate` (leaves free), host build uniform all-`inline`
(RAM is free there, so no branch and no indirection because *every* vertex has ext at a
fixed offset, and the `null`-signal protocol still holds uniformly) — **never** the
conditional-fold branch. No such measurement exists today.
