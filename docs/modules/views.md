# views — zero-copy windows (L1)

```{admonition} In one paragraph
:class: tip
A **`view_t`** is a `(segment, offset, length)` window onto real bytes; copying it is
a refcount clone, not a byte copy. A **`rope_t`** is a chain of views, so one logical
message can span several buffers (a static header + a live DMA payload) without
copying. **`decode(view_t)`** realizes L1's load-bearing claim — *a TLV is a cast from
a view* — by running the M1 decoder over a view's bytes in place.
```

## What it does

L1 sits between real memory (L0) and TLV bytes (L2). Its types — `view_t`, `rope_t`,
and `segment_ptr_t` — live in `tr::view`. It owns the *ownership
semantics*, not the bytes. A single-link `view_t` is the hot path and allocates
nothing; a multi-link `rope_t` models scatter-gather. `rope_t::to_iovec()` hands the
chain to `writev`/`sendmsg`-style egress with **zero copies**; `rope_t::flatten()`
materializes it into one contiguous segment only when a flat-buffer consumer
demands it (the single transport-boundary copy). Assembling a multi-buffer message is
**chaining views into a `rope_t`, never a memcpy** — a contiguous copy happens only
when `flatten()` runs at a substrate boundary that cannot scatter-gather.

`decode(v)` is just `decode(v.bytes())` — the decoded `tlv_t`'s payload spans
point *into* the view's segment, and the view's `segment_ptr_t` keeps them alive. No
decode-into-a-struct step: the wire bytes **are** the in-memory value.

Ownership is an intrusive refcount on the segment, not on the view: cloning a
`segment_ptr_t` increments **relaxed**, dropping one decrements **acq_rel** and fires
the backend's `destroy` when the pre-decrement value was 1 (`tr::view::detail::ref_count_t`,
`core/include/libtracer/segment.hpp:52-54`; the clone and release sites are
`segment_ptr_t`'s copy constructor and `reset`, `segment.hpp:126` and `:139`). Relaxed
on the increment is sound because a clone is always made from a reference the caller
already holds; the acq_rel decrement is what orders the last writer's stores before the
destructor reads them. A `LIBTRACER_NO_ATOMIC` build substitutes a plain counter with
the same call shape (`segment.hpp:44-47`).

## Interface

```cpp
namespace tr::view {

struct view_t {                                          // view.hpp
    segment_ptr_t owner;  std::size_t offset, length;
    static view_t over(segment_ptr_t) noexcept;                   // whole segment      :41
    std::span<const std::byte> bytes() const noexcept;            //                    :53
    view_t subview(std::size_t off, std::size_t len) const;       // shares owner       :74
};

/** Own a copy of borrowed bytes as a view_t; nullopt == allocation failure. */
std::optional<view_t> over_bytes(std::span<const std::byte>) noexcept;  // mem_heap.hpp:340
std::optional<view_t> over_bytes(std::span<const std::byte>, mem::mem_backend_t&) noexcept; // :375

class rope_t {                                           // rope.hpp — ordered chain of views
    rope_t(view_t);                                               // a view is a 1-link rope :53
    void append(view_t);   rope_t& concat(const rope_t&);         //                 :56, :73
    std::size_t link_count() const noexcept;                      //                   :119
    std::size_t total_length() const noexcept;                    //                   :154
    template <class Fn> void walk(Fn&&) const;                    //                   :204

    const view_t& only() const noexcept;                          // SINGLE-LINK ONLY  :134
    view_t materialize(mem_backend_t& = mem::heap_backend()) const;  // 0 or 1 copy    :148

    std::vector<std::span<const std::byte>> to_iovec() const;     // zero-copy egress  :213
    bool try_to_iovec(std::vector<std::span<const std::byte>>&) const noexcept;  //    :230
    view_t flatten(mem_backend_t& = mem::heap_backend()) const;   // one-copy          :247
};

}  // namespace tr::view

std::expected<tlv_t, err_t> tr::wire::decode(const view_t&);   // the L1 → L2 cast  frame.hpp:197
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

A rope holds its first two links in small-buffer storage (`kInline = 2`,
`core/include/libtracer/rope.hpp:373`); the third link spills the whole chain to the
heap, which is the chain's only allocation (`rope_t::append`, `rope.hpp:76-91`).

## Owning a copy of borrowed bytes

Bytes handed up by a transport are borrowed: they live in a connection buffer that is
reused as soon as the callback returns. Keeping them means owning a copy, and the
canonical way to take one is `tr::view::over_bytes`
(`core/include/libtracer/mem_heap.hpp:340`) — one call in place of the
`heap_alloc` + `memcpy` + `view_t::over` triplet. A second overload (`:377`) takes the
backend to draw from, which is what a peer-driven ownership copy uses so the copy lands in
the node's injected seam rather than the global heap.

```cpp
tr::graph::result_t<void> store(tr::graph::graph_t& g, const tr::graph::path_t& path,
                                std::span<const std::byte> borrowed) {
    std::optional<tr::view::view_t> owned = tr::view::over_bytes(borrowed);
    if (!owned) return std::unexpected(tr::graph::status_t::BACKPRESSURE);  // allocation failed
    return g.write(path, tr::view::rope_t{*owned});
}
```

The `std::optional` return exists to separate two outcomes that a bare `view_t` conflates:

| Result | Meaning | Caller's move |
| --- | --- | --- |
| `std::nullopt` | the segment allocation failed | map to `BACKPRESSURE`; drop or retry |
| engaged, empty | `bytes` was legitimately empty (`heap_alloc(0)` is not called) | proceed; the value is the empty value |
| engaged, non-empty | an owned copy of `bytes` | proceed |

The same call — in its seam-taking overload, drawing from the transport's injected
backend rather than the global heap — is what the RFC 6455 fragment assembler uses to turn
each borrowed fragment into an owning link before chaining it (`ws_assembler_t::on_data`,
`core/src/transport_ws.cpp:100`), so the copy out of the connection buffer is the one
legitimate substrate-boundary copy and the chaining that follows is pointer-linking.

## Consequences

- **A TLV is a view** — no parse-into-struct; the decoder returns borrowed spans,
  so reading a field is a pointer load.
- **Scatter-gather without copies** — compose a message from separate buffers and
  emit it with one `writev`; flatten only when a transport truly needs contiguity.
- **Slicing is free** — `subview`/`concat` build new view structs that bump the
  segment refcount; no bytes move.
- **Contiguity is a consumer's explicit choice, not a default** — a value arrives as a
  rope, and the consumer states whether it can accept a chain (`walk`, `to_iovec`) or
  needs one buffer (`materialize`, `flatten`). The rejected alternative was flattening
  on the way out of the graph, which pays the copy for every consumer including the ones
  that scatter ([ADR-0053, Lazy rope-backed decode-view and partial-path
  routing](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md)).
- **`over_bytes` lives in `tr::view`, not `tr::mem`** — it hands back an owning handle,
  and an owning handle is an L1 concept ([ADR-0016, Substrate zero-copy, layer
  namespaces, no templates through the
  seam](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md)).

## Pitfalls

**`only()` is valid only on a single-link rope.** The precondition is `link_count() == 1`
and it is *debug-asserted* (`rope_t::only`, `core/include/libtracer/rope.hpp:183-189`).
With `NDEBUG` the assert is compiled out and `only()` returns the first link, so a
multi-link value is read as if the first buffer were the whole message — a silent
truncation, not a diagnostic. This is invisible on a purely local graph, where every
value is one segment, and appears the moment a real transport is attached: every
transport whose `transport_t::delivers_ropes()` returns true
(`core/include/libtracer/transport.hpp:458`; TCP, UDP, WS, QUIC, WebTransport and CAN
all override it) can hand up a chain. A CAN reassembly group chains one link per slice
(`can_reassembly_t::assemble`, `core/include/libtracer/can_reassembly.hpp:191-199`), and
a fragmented WebSocket message chains one link per fragment
(`ws_assembler_t::on_data`, `core/src/transport_ws.cpp:86-111`). A consumer that cannot
promise contiguity calls `materialize()` (`rope.hpp:204`) instead — zero copy when the
rope happens to be single-link, one `flatten` copy otherwise. `only()` is the right call
only where the surrounding code has already established that the rope is one link.

**`to_iovec()` allocates and can throw.** It `reserve`s a span table per call, which
under `-fno-exceptions` turns an out-of-memory into `abort()`. Egress paths that build
this table per send use `try_to_iovec(out)`, which probes the exact allocation first and
returns `false` instead, leaving `out` empty (`rope.hpp:286-308`). ⚠️ The probe is not a hard
nothrow guarantee: `tr::detail::try_reserve` frees its probe block and *then* runs the
throwing `reserve`, so on a multi-threaded node a racing allocation between the two can still
abort ([#850](https://github.com/avatarsd-llc/libtracer/issues/850)); the header qualifies its
own comment with "single-threaded" for exactly this reason.

**Treating an empty `over_bytes` result as an empty value loses backpressure.**
`std::nullopt` and an engaged-but-empty view are different answers; collapsing them
reports a failed allocation as a successful write of nothing.

## API reference

```{doxygenstruct} tr::view::view_t
:project: libtracer
:members:
```

```{doxygenclass} tr::view::rope_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::view::over_bytes(std::span<const std::byte>)
:project: libtracer
```

```{doxygenfunction} tr::view::over_bytes(std::span<const std::byte>, mem::mem_backend_t&)
:project: libtracer
```

```{doxygenfunction} tr::view::segment_alloc
:project: libtracer
```

The CAN splitter is the other L1 view producer; it lives with the rest of the CAN
stack on [can](can.md). The lazy, rope-backed *decode* view — what a rope-delivered
frame becomes on the read side — is L2 and lives on
[frame-codec](frame-codec.md).

See: [segment](segment.md), [frame-codec](frame-codec.md), [graph](graph.md),
[can](can.md),
[reference 08 — views and ownership](../reference/08-views-and-ownership.md).
