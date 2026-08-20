# The rope: assembly is chaining, never `memcpy` (L1 views)

A **rope** is the L1 composite — an ordered chain of views over possibly different segments,
forming one logical byte sequence ([CONTEXT.md](../../CONTEXT.md) §Rope / assembly). A static
header segment plus a live DMA payload segment become one payload by *chaining* them.
`append` links; `total_length` sums what the links already hold. Nothing is copied, and
"assembly" and "reassembly" both mean exactly this.

[Rope scatter-gather](rope-scatter.md) shows the same type at the egress end; this page is the
composition itself.

## What to notice

- **A view converts to a one-link rope implicitly.** The trivial case is meant to be free, and
  the single-link value is the hot path — a rope-valued vertex slot costs what a view-valued
  slot cost.
- **`only()` is the consumer's explicit "this is one segment".** Zero copy, and debug-asserted.
  A consumer that cannot promise contiguity asks for `materialize()` instead, which is the
  visible choice between a refcount bump and one flatten copy.
- **The first two links live in inline storage.** A one- or two-link chain allocates nothing at
  all; the third link spills the chain to the heap. That threshold is a **cost tuning knob, not
  a limit** — nothing refuses a longer rope, and a caller that knows its final count can
  `try_reserve` it up front.
- **`operator+` takes its left operand by value.** The example checks that the original rope
  still has its three links afterwards: concatenation chains, it does not consume.
- **A rope is not a list of TLVs.** It composes *storage*; `opt.PL` composes *meaning*
  ([structured vs opaque](wire-structured-vs-opaque.md)). The two axes are independent, which
  is why a link boundary may fall mid-header — see [the lazy view](wire-lazy-view.md).
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/view_rope_compose.cpp
:language: cpp
:linenos:
```

See also: [views module](../modules/views.md) ·
[views & ownership reference](../reference/08-views-and-ownership.md) ·
[composition axes](tree-of-ropes.md).
