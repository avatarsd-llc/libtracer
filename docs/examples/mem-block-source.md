# The failable block seam (L0 substrate)

`tr::mem::block_source_t` is the seam every allocation a **peer** can provoke draws from
([reference 09](../reference/09-memory-substrate.md) §the second L0 seam). It is three members
wide — a nothrow `try_alloc(bytes, align)`, a **sized** `release(p, bytes, align)`, and a
`name()` — and its entire failure vocabulary is `nullptr`. There is no throwing spelling, no
fallback to the global heap, and no `abort()`; the caller turns the `nullptr` into whatever
reject its own operation owns.

The example shows both sides of the seam, because both are the reader's: the default source a
node gets for free, and a source of one's own, which is how a deployment states its bound.

## What to notice

- **The type exists because `std::pmr::memory_resource` structurally cannot do this.** That
  type's `allocate` is annotated `returns_nonnull` in libstdc++, so a caller's `if (p == nullptr)`
  is undefined-behaviour-deletable — and inspecting `riscv32-esp-elf-g++ 15.2.0` with the
  deployment flags shows the branch surviving at `-O0`…`-O3` and **gone at `-Os`/`-Oz`**, which
  is the level an ESP-IDF node ships at
  ([ADR-0065](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0065-failable-allocation-gets-its-own-seam-block-source.md)).
  A seam whose failure signal disappears at exactly the optimization level the target uses is
  not a seam.
- **Nothrow is a compile-time property, asserted as one.** Both halves are `noexcept`, and the
  example `static_assert`s it. On the shipping profile a throw reaches ESP-IDF's link-wrapped
  `__cxa_throw` → `abort()` stub, so "it rarely throws" is not a weaker version of this
  guarantee — it is a peer-reachable reboot.
- **`release` is sized, and that is load-bearing.** The caller hands back the `(bytes, align)`
  it asked for, so a bump or pool source needs **no per-block header** at all — the property
  [`mem_pool_source`](mem-pool-source.md) turns into exact packing.
- **A source names itself.** A bounded node's operator watching a refusal needs to know *which*
  seam ran out; `name()` is that, and it costs one borrowed literal.
- **Implementing one is the point.** The whole extension surface is two overrides. The
  `budget_source_t` in the example is ~15 lines and is also the shape a test uses to inject a
  failure deliberately.
- **The refusal is provoked on a budgeted source, never on the platform heap.** A request the
  real allocator cannot serve is a *sanitizer's fatal error*, not a `nullptr`, so an example
  that asked the heap for an impossible block would fail the ASan leg rather than demonstrate
  anything.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/mem_block_source.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) ·
[memory substrate reference](../reference/09-memory-substrate.md) ·
[the two L0 seams](mem-source-vs-backend.md).
