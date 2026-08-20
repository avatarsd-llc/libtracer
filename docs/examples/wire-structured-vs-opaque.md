# Structured or opaque — one bit decides (L2/L3 codec)

`opt.PL` is the whole of TLV composition. Clear, and the body is *payload bytes* — an
**opaque TLV**, the leaf. Set, and the body is a packed run of sub-TLVs — a **structured
TLV**, the composite, whose type code says what the children mean
([CONTEXT.md](../../CONTEXT.md) §Two compositions). There is no LIST type and no array flag;
nesting is this bit plus a purpose type byte.

The example makes the point by encoding `POINT{VALUE, VALUE}` once, clearing `PL` on a copy,
and decoding **the same bytes** as one opaque payload whose length is exactly the children
region's.

## What to notice

- **Meaning composes at L3; storage composes at L1, and they do not constrain each other.**
  A structured TLV's children are a *meaning* tree. The [rope](view-rope-compose.md) that
  physically holds those bytes is a *storage* tree, and a link boundary may fall anywhere,
  including mid-header — see [the lazy view](wire-lazy-view.md). That decoupling **is** the
  zero-copy story.
- **A decoded TLV is one or the other, never both.** `children` is populated and `payload`
  empty for a structured TLV; the reverse for an opaque one. Reading the wrong field gets you
  a legitimately empty container, not an error.
- **The children region is contiguous bytes with no framing of its own.** That is why the
  same body reads as one opaque payload of `frame.size() - 4`: sub-TLVs are self-describing,
  so no count field and no separators are needed.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/wire_structured_vs_opaque.cpp
:language: cpp
:linenos:
```

See also: [frame codec](../modules/frame-codec.md) ·
[data-format reference](../reference/01-data-format.md) ·
[protocol TLVs](../reference/05-protocol-tlvs.md).
