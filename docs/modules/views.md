# views — zero-copy windows (L1)

```{admonition} In one paragraph
:class: tip
A **`View`** is a `(segment, offset, length)` window onto real bytes; copying it is
a refcount clone, not a byte copy. A **`Rope`** is a chain of views, so one logical
message can span several buffers (a static header + a live DMA payload) without
copying. **`view_as_tlv`** realizes L1's load-bearing claim — *a TLV is a cast from
a view* — by running the M1 decoder over a view's bytes in place.
```

## What it does

L1 sits between real memory (L0) and TLV bytes (L2). It owns the *ownership
semantics*, not the bytes. A single-link `View` is the hot path and allocates
nothing; a multi-link `Rope` models scatter-gather. `Rope::to_iovec()` hands the
chain to `writev`/`sendmsg`-style egress with **zero copies**; `Rope::flatten()`
materializes it into one contiguous segment only when a flat-buffer consumer
demands it (the single bridge-boundary copy).

`view_as_tlv(v)` is just `decode(v.bytes())` — the decoded `Tlv`'s payload spans
point *into* the view's segment, and the view's `SegmentPtr` keeps them alive. No
decode-into-a-struct step: the wire bytes **are** the in-memory value.

## Interface

```cpp
struct View {
    SegmentPtr owner;  std::size_t offset, length;
    static View over(SegmentPtr);                       // whole segment
    std::span<const std::byte> bytes() const;
    View subview(std::size_t off, std::size_t len) const;   // narrower, shares owner
};

class Rope {                                            // ordered chain of Views
    Rope(View);                                         // a view is a 1-link rope
    void   append(View);   Rope& concat(const Rope&);
    std::size_t total_length() const;   template<class F> void walk(F) const;
    std::vector<std::span<const std::byte>> to_iovec() const;     // zero-copy egress
    View flatten(MemBackend& = mem::heap_backend()) const;        // one-copy materialize
};

std::expected<Tlv, Error> view_as_tlv(const View&);     // the L1 -> L2 cast
```

## Rope = one message, many buffers

```{mermaid}
flowchart LR
    H["view A · header<br/>static segment"] --> P["view B · payload<br/>DMA segment"] --> T["view C · tail<br/>pool segment"]
    H -.-> S1[(seg 1)]
    P -.-> S2[(seg 2)]
    T -.-> S3[(seg 3)]
    R["Rope.to_iovec() → writev()"]:::e
    H --- R
    classDef e fill:#dbeafe,stroke:#1e40af;
```

## Benefits

- **A TLV is a view** — no parse-into-struct; the decoder returns borrowed spans,
  so reading a field is a pointer load.
- **Scatter-gather without copies** — compose a message from separate buffers and
  emit it with one `writev`; flatten only when a transport truly needs contiguity.
- **Slicing is free** — `subview`/`concat` build new view structs that bump the
  segment refcount; no bytes move.

See: [segment](segment.md), [frame-codec](frame-codec.md), [graph](graph.md).
