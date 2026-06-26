# views — zero-copy windows (L1)

```{admonition} In one paragraph
:class: tip
A **`view_t`** is a `(segment, offset, length)` window onto real bytes; copying it is
a refcount clone, not a byte copy. A **`rope_t`** is a chain of views, so one logical
message can span several buffers (a static header + a live DMA payload) without
copying. **`view_as_tlv`** realizes L1's load-bearing claim — *a TLV is a cast from
a view* — by running the M1 decoder over a view's bytes in place.
```

## What it does

L1 sits between real memory (L0) and TLV bytes (L2). Its types — `view_t`, `rope_t`,
and `segment_ptr_t` — live in `tr::view`. It owns the *ownership
semantics*, not the bytes. A single-link `view_t` is the hot path and allocates
nothing; a multi-link `rope_t` models scatter-gather. `rope_t::to_iovec()` hands the
chain to `writev`/`sendmsg`-style egress with **zero copies**; `rope_t::flatten()`
materializes it into one contiguous segment only when a flat-buffer consumer
demands it (the single bridge-boundary copy). Assembling a multi-buffer message is
**chaining views into a `rope_t`, never a memcpy** — a contiguous copy happens only
when `flatten()` runs at a substrate boundary that cannot scatter-gather.

`view_as_tlv(v)` is just `decode(v.bytes())` — the decoded `tlv_t`'s payload spans
point *into* the view's segment, and the view's `segment_ptr_t` keeps them alive. No
decode-into-a-struct step: the wire bytes **are** the in-memory value.

## Interface

```cpp
struct view_t {
    segment_ptr_t owner;  std::size_t offset, length;
    static view_t over(segment_ptr_t);                  // whole segment
    std::span<const std::byte> bytes() const;
    view_t subview(std::size_t off, std::size_t len) const;   // narrower, shares owner
};

class rope_t {                                          // ordered chain of views
    rope_t(view_t);                                     // a view is a 1-link rope
    void   append(view_t);   rope_t& concat(const rope_t&);
    std::size_t total_length() const;   template<class F> void walk(F) const;
    std::vector<std::span<const std::byte>> to_iovec() const;     // zero-copy egress
    view_t flatten(mem_backend_t& = mem::heap_backend()) const;   // one-copy materialize
};

std::expected<tlv_t, error_t> view_as_tlv(const view_t&);   // the L1 -> L2 cast
```

## Rope = one message, many buffers

```{mermaid}
flowchart LR
    H["view A · header<br/>static segment"] --> P["view B · payload<br/>DMA segment"] --> T["view C · tail<br/>pool segment"]
    H -.-> S1[(seg 1)]
    P -.-> S2[(seg 2)]
    T -.-> S3[(seg 3)]
    R["rope_t.to_iovec() → writev()"]:::e
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
