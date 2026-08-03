# Reference 08 — Views and ownership (L1)

> **Scope**: the view layer between raw memory ([09-memory-substrate.md](09-memory-substrate.md)) and the wire format ([01-data-format.md](01-data-format.md)) — the canonical view struct, refcount semantics, rope structure, the TLV-as-cast operation, and the catalog of view-modules that pair with specific I/O capabilities.
> **Audience**: anyone implementing the view layer; anyone integrating libtracer with a specific I/O subsystem (UART simple, UART DMA, lwIP, CAN, SHM); anyone reasoning about zero-copy semantics or rope traversal.

---

## What L1 is

L1 is the layer between **real memory** (L0) and **TLV bytes** (L2). It provides three things:

1. **A view struct** — a `(segment, offset, length)` triple naming a span of bytes inside a refcounted segment.
2. **Shared ownership** — refcounting on segments, so multiple views can outlive each other while keeping the underlying memory alive.
3. **Ropes** — chains of views, so a logical sequence of bytes can span multiple non-contiguous segments without copying.

A rope is **storage** composition, not meaning: it is the L1 axis of the *two orthogonal compositions* described in [02-graph-model.md §the two compositions](02-graph-model.md#the-two-compositions-storage-and-meaning) — a rope chains *bytes*, a structured TLV (L3) nests *meaning*, and the two are independent (a view boundary may fall mid-TLV-header).

The load-bearing claim of L1:

> **A TLV at L2 is a cast from an L1 view.**

Given a view whose bytes constitute a valid TLV, the L2 layer interprets the bytes in place. No copy. The TLV's payload is itself a view (or a sub-view, or a rope) into the same segment(s). Nested TLVs are views into the parent's bytes. The whole structure is one tree of views over one or more L0 segments.

This is what separates libtracer from middleware that decodes wire bytes into in-memory message structs. There is no decode step in the sense that middleware means it: the wire bytes, the view and the in-memory representation are the same bytes.

---

## The view struct

The view is deliberately POD-simple — one owning handle plus two sizes (reference
implementation: `tr::view::view_t`, [core/include/libtracer/view.hpp](https://github.com/avatarsd-llc/libtracer/blob/main/core/include/libtracer/view.hpp)):

```cpp
namespace tr::view {

struct view_t {
    segment_ptr_t owner;    // owning handle to the refcounted L0 segment
    std::size_t   offset;   // bytes from the segment's base
    std::size_t   length;   // bytes covered
};

}
```

A **rope** is a separate composite type — an ordered chain of views (`rope_t`) — not
an intrusive `next` pointer inside the view. A view is always exactly one window
over one segment; the rope composes several of them into one logical payload.
Keeping the chain out of the view keeps the single-link hot path allocation-free:
a plain `view_t` allocates nothing. How the rope stores its chain is an
implementation choice, and short chains need not allocate at all — the reference
implementation holds the first two links in inline storage and spills the whole
chain to a heap vector on the third append
([core/include/libtracer/rope.hpp](https://github.com/avatarsd-llc/libtracer/blob/main/core/include/libtracer/rope.hpp)).

Invariants:

- `offset + length <= owner->bytes.size()`. A view never escapes its segment.
- **Copy IS clone.** Copying a view copies its `segment_ptr_t`, which bumps the
  segment refcount (relaxed) — never a byte copy.
- **Destruction IS release.** Ownership is RAII: when a view is destroyed or
  reassigned, its `segment_ptr_t` does the acq_rel decrement and invokes the
  backend's `destroy` at zero. There is no manual release call to forget.
- A view does not own any bytes — it borrows them via the segment refcount.

Sub-views are cheap: `subview(off, len)` produces a new view with the same `owner`
(refcount bumped), narrower offset/length. Useful for zero-copy slicing. A view
also knows whether its bytes are CPU-addressable (`is_host()` / `is_device()`) —
a DEVICE window (e.g. GPU memory, [ADR-0024 — CUDA/GPU backend and the heterogeneous rope](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md))
must not be dereferenced on the CPU.

---

## The segment struct (recap from L0)

```cpp
namespace tr::view {

struct segment_t {
    detail::ref_count_t     refcount;  // intrusive: inc relaxed, dec acq_rel
    tr::mem::mem_backend_t* backend;   // who reclaims these bytes
    std::span<std::byte>    bytes;     // the real bytes (data + capacity)
    tr::mem::mem_space_t    space;     // HOST or DEVICE, inherited from the backend
};

}
```

The view layer sees segments as nothing but a refcount, a backend pointer, the byte span and the address space the bytes live in. The address space is inherited from the backend at construction, so a DEVICE segment is recognisable without asking the backend; a view over it must not be CPU-dereferenced. Backend-specific state (lwIP `pbuf*`, DMA descriptor index, MMIO register table, and any module-set tagging the implementation adds) lives behind the backend and is L0's concern; reclamation is `backend->destroy(seg)`, invoked by the owning handle (`segment_ptr_t`) when the count hits zero ([09-memory-substrate.md](09-memory-substrate.md) §the backend abstraction).

---

## Refcount semantics

The atomic memory orderings for the segment refcount are specified once in [02-graph-model.md](02-graph-model.md) §required atomic operations and recapped here for completeness:

| Operation | Order | Why |
| ---- | ---- | ---- |
| Increment (clone view) | `relaxed` | The caller holds a reference of its own; the data dependency travels via that reference |
| Decrement (release view) | `acq_rel` | release: flush all writes before another thread observes the count drop; acquire: the thread that observes the drop to zero synchronizes with every prior release |
| Read for inspection (debug/metrics) | `acquire` | Pairs with each decrement; gives a consistent snapshot |
| Weak-to-strong upgrade (CAS loop) | `acq_rel` on success, `acquire` on failure | Same logic as the increment, plus synchronization with the last decrementer |

The decrement returns the value *before* it, so a return of `1` is the "this caller dropped the last reference" test.

On a target with no load-linked/store-conditional pair (Cortex-M0/M0+) and in bare-metal single-threaded contexts, the refcount may be a plain unsigned integer with the same three operations. The application then carries the obligation the atomics discharged: no segment is shared across threads or between an ISR and thread context.

When a segment's refcount drops to zero, the owning handle invokes `backend->destroy(seg)`. Destruction returns the bytes to whichever L0 backend owns them.

---

## View and rope operations

The clone/release pair of a manual-refcount design is expressed as ordinary C++ value semantics; everything else is a small set of member operations ([core/include/libtracer/rope.hpp](https://github.com/avatarsd-llc/libtracer/blob/main/core/include/libtracer/rope.hpp)):

### Clone — copy the view

Copying a `view_t` bumps its segment's refcount (relaxed) via the copied `segment_ptr_t`. Copying a `rope_t` copies its links — one bump per link's segment.

### Release — destroy the view

Destroying (or reassigning) a `view_t` does the acq_rel decrement; at zero, the segment's backend `destroy` fires. Destroying a rope releases every link. RAII: there is no leak-by-forgotten-release failure mode.

### `view_t::subview(sub_offset, sub_length) -> view_t`

A narrower window into the same segment: `offset + sub_offset`, length `sub_length`, refcount bumped. A view is single-segment by definition — slicing across segment boundaries is a rope-level concern (walk to the appropriate link first, or take a `subrope`, which trims the covering links with `subview` and shares their segments).

### `rope_t::append(view)` / `rope_t::concat(rope)` / `operator+`

Chain links onto the rope. Appending `rope2` extends the chain with all of `rope2`'s links, in order. **No bytes are copied** — assembly is chaining, never memcpy. A single `view_t` converts implicitly to a one-link rope. There is no nested rope type: concatenation splices chains flat, because nesting belongs to the *meaning* axis (the TLV tree), not the storage axis.

### `rope_t::total_length() -> size_t`

Sums `length` over the links. O(N) in chain length.

### `rope_t::walk(fn)`

Visits each link's contiguous bytes in order. Used by parsers, serializers, and CRC accumulators.

### `rope_t::to_iovec() -> spans`

Scatter-gather egress: one span per link, pointing into the original segments (no copy). Hand the result to `writev` / `sendmsg`-style I/O for true zero-copy transmit. Each transport lowers the same rope to its own native scatter-gather facility — iovec, CAN descriptors, RDMA verbs.

### `rope_t::flatten(backend) -> view_t`

Materializes the rope into one contiguous segment allocated from `backend` — the **single bridge-boundary copy**, taken only when a flat-buffer consumer demands it. Fails (returns an empty view) if the backend cannot allocate or if the rope has a DEVICE link the CPU must not touch ([ADR-0024 — CUDA/GPU backend and the heterogeneous rope](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md)). The failure is a value, not an exception: a caller that ignores the empty result loses the payload silently.

---

## Ropes: chains of views

A **rope** (`rope_t`) is an ordered chain of views representing a logical sequence of bytes that may span multiple segments. The chain lives in the rope, not in the views: each link is an ordinary `view_t`, and the rope holds them in order.

```
Logical TLV bytes:
   ┌───────────────────────────────────────────────────────┐
   │ header(4) │ payload byte 0..N-1                       │
   └───────────────────────────────────────────────────────┘

Underlying rope:
   ┌─────────┐    ┌──────────────────────┐    ┌───────────┐
   │ view A  │ →  │ view B               │ →  │ view C    │
   │ (4 B)   │    │ (first half payload) │    │ (second…) │
   │ → seg 1 │    │ → seg 2              │    │ → seg 3   │
   └─────────┘    └──────────────────────┘    └───────────┘
        │                  │                       │
        ▼                  ▼                       ▼
    static seg        DMA RX seg              MMIO seg
    (header bytes)    (payload first half)    (sensor reg)
```

**Properties of a rope:**

- The wire serialization of the rope is `seg1[off1..off1+len1] || seg2[off2..off2+len2] || ...` — concatenated bytes from each link.
- Each link's bytes are contiguous within their segment, but consecutive links may live in different segments.
- The proof obligation from [02-graph-model.md](02-graph-model.md) §spec-level proof obligation guarantees: any rope, when serialized, produces the same bytes as the equivalent flat buffer would.

**Use cases:**

- **MMIO + dynamic header**: a header in static memory plus a payload that points at an MMIO register. Rope makes this a single TLV without copy.
- **Multi-pbuf TCP receive**: lwIP delivers a 4 KiB TLV across three 1.5 KiB pbufs. The TLV is a 3-link rope.
- **Aggregated transmit**: a publisher composes a TLV from a static header buffer + a runtime payload buffer + a fixed tail buffer. Three segments, one rope, one TLV on the wire.
- **Forward hop**: the forwarder receives a `FWD` frame and sub-views the pieces that survive the hop — the `dst` tail after the stripped segment, the untouched payload region — off the original segment; every sub-view shares the inbound frame's segment via refcount, no bytes move.

**Walking a rope**: parsers and serializers use `rope_t::walk` or equivalent. The iterative TLV parser ([01-data-format.md](01-data-format.md) §iterative parsing requirement) treats a rope-cursor specially when traversing children: advancing the cursor across a link boundary is one extra branch in the iteration, but otherwise the parser logic is identical.

### Refcount fan-out and release sequence

The lifetime drawing for a single segment under fan-out:

```mermaid
sequenceDiagram
    autonumber
    participant TX as Transport RX
    participant V as View layer
    participant D as Dispatcher
    participant S1 as Subscriber 1
    participant S2 as Subscriber 2
    participant L0 as L0 backend

    TX->>V: create segment (refcount=1)
    Note over V: backend retains 1 (recv buffer)
    V->>D: hand off view (refcount += 1 → 2)
    Note over V: TX releases its hold (refcount = 1)
    D->>V: clone for Subscriber 1 (refcount = 2)
    D->>V: clone for Subscriber 2 (refcount = 3)
    D->>S1: deliver view
    D->>S2: deliver view
    Note over D: dispatcher releases its own hold<br/>(refcount = 2)
    S1->>V: drop view (refcount = 1)
    S2->>V: drop view (refcount = 0)
    V->>L0: backend->destroy(seg)
    Note over L0: backend reclaims bytes<br/>(heap free / pool return /<br/>DMA half marked reusable)
```

The publisher and the transport never wait for subscribers; back-pressure surfaces as **the segment refcount staying high**, which the L0 backend observes when it tries to reuse the slot. This is the protocol's flow-control signal at the substrate layer.

---

## Casting a view to a TLV

Given a view whose bytes hold an L2 TLV, the cast decodes and **validates** it. Because it produces a `tlv_t`, the cast itself lives at L2 (`tr::wire`, not `tr::view`) — L1 never depends upward:

```cpp
std::expected<tlv_t, tr::wire::err_t> tlv = tr::wire::decode(v);
```

The cast is a `decode` overload over the view — `decode(v)` is exactly `decode(v.bytes())` (`decode`, [core/include/libtracer/frame.hpp](https://github.com/avatarsd-llc/libtracer/blob/main/core/include/libtracer/frame.hpp)): it **validates** the framing (minimum size, reserved-bit and type-`0x00` rejects, the `LL` length width, trailer sizing, CRC verification, and the receiver's decode-resource bound on nesting depth) and, on success, materializes an owning `tlv_t` tree. The decoded payload spans (and every child's) **borrow** `v`'s bytes, so the view — and thus its refcounted segment (§refcount semantics) — must outlive the returned `tlv_t`. On malformed input it yields the `err_t` the grammar rejected with (`FRAME_TRUNCATED` / `FRAME_INVALID` / `FRAME_CRC_FAIL` / `TLV_NESTING_TOO_DEEP`).

Nesting depth has no constant: `TLV_NESTING_TOO_DEEP` means "exceeds *this* receiver's decode resources", and a receiver's open-node budget is the bound (RFC-0006). An implementation that hardcodes a depth number will reject frames a conforming peer may legitimately send.

There is **no non-validating cast**. The receive path always validates, so a lazy non-validating accessor has no consumer to serve; the forwarder's offset peeks over already-validated framing are a net-plane optimization, not a public cast ([ADR-0048 — one wire grammar, chunk cursor, rope-aware decode](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md) §4). Validation may be *staged* rather than eager — the bounds anchored where the frame enters, each child's header as the walk steps over it, each TLV's CRC where that TLV is consumed — but no path hands a consumer bytes that no check has accepted.

### Rope-aware decode

A flat (single-link) view decodes by reading its bytes directly. A multi-link rope does not have to be flattened first: the same header/trailer grammar reads through a link-walking cursor instead of a contiguous span (`rope_cursor`, [core/include/libtracer/rope_decode.hpp](https://github.com/avatarsd-llc/libtracer/blob/main/core/include/libtracer/rope_decode.hpp); [ADR-0048 — one wire grammar, chunk cursor, rope-aware decode](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md) §1). A header or trailer that straddles a link boundary is stitched a byte at a time into a small bounded stack scratch — headers and trailers are small and bounded — and a payload the CRC must cover is fed to the CRC link by link. Payload spans stay zero-copy: a payload inside one link is a subview, a payload across links is a sub-rope.

Two rope entry points exist, matching the two validation timings:

| Entry point | What it checks | When |
| ---- | ---- | ---- |
| Ingress check | Root header, the total-size anchor, and — when `opt.CR` is set — the whole-frame trailer CRC, in one linear link-by-link scan. No descent. | Everything ingress is allowed to verify; a malformed child surfaces where that child is consumed. |
| Strict whole-tree walk | The full grammar over every level of the rope, iteratively, without flattening. Rejects with the same `err_t` a flattened decode would. | Opt-in: a verify-all-then-apply consumer, or a differential test. |

What rope-aware decode does *not* do is materialize a rope frame directly into an eager owning `tlv_t` or into a terminus arena node. Both of those sink node types hold a borrowed contiguous span, which cannot name a payload that straddles a link boundary ([ADR-0041 — terminus arena decode span contract](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0041-terminus-arena-decode-span-contract.md) §2). Producing those sinks from a rope therefore costs one explicit copy: either flatten the rope and cast the resulting contiguous view, or materialize from the lazy rope-backed node. The lazy node — one TLV holding its parsed header facts plus a refcounted sub-rope of the frame, materializing children one header at a time and never decoding what is not accessed — is the decode-side representation of a rope-delivered frame, and it may outlive the transport's read loop because each sub-rope keeps exactly its own links' segments alive ([ADR-0053 — lazy rope-backed decode view and partial-path routing](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md)).

---

## Two parser contexts revisited

[01-data-format.md](01-data-format.md) §two parser contexts names the distinction; here is the mechanism:

### Context A: wire-receive

The parser sees a flat byte buffer (one segment) and walks byte offsets within it.

```c
const uint8_t *buf;
size_t         offset = 0;
size_t         end;
while (offset < end) {
    const tlv_t *t = (const tlv_t *)(buf + offset);
    size_t total = tlv_total_size(t);  // header + payload + trailer
    /* process t */
    offset += total;
}
```

Cursor advance is `offset += total`. No segment-boundary crossings; the buffer is one segment.

### Context B: in-memory walk (rope)

The parser sees a rope and steps across link boundaries (the rope owns the link order — `rope.links()` is the ordered chain):

```cpp
std::size_t link_idx = 0, in_link = 0;
while (link_idx < rope.link_count()) {
    const view_t& link  = rope.links()[link_idx];
    auto          bytes = link.bytes();
    while (in_link < bytes.size()) {
        auto         maybe = decode(link.subview(in_link, bytes.size() - in_link));
        const tlv_t& t     = *maybe;   // std::expected<tlv_t, err_t>; borrows the link's bytes
        std::size_t  total = tlv_total_size(t);
        if (in_link + total <= bytes.size()) {
            /* TLV fits in this link; process and advance within link */
            in_link += total;
        } else {
            /* TLV spans a link boundary; need a rope-aware cursor advance */
            rope_advance(rope, link_idx, in_link, total);
        }
    }
    ++link_idx;
    in_link = 0;
}
```

Implementations share the iterative pattern (recurse on `PL=1`, bound depth by the receiver's own open-node budget rather than a constant) but specialize the cursor advance per context. Most TLVs in practice fit within one link, so the rope-aware path is rare and hot-path performance is dominated by the flat case.

Recursion is not an acceptable substitute for the iterative walk in either context: a deep frame is attacker-chosen input, and a recursive descent turns it into a call-stack overflow on a small MCU ([01-data-format.md](01-data-format.md) §iterative parsing requirement).

---

## L1 module catalog

L1 is core (the view + refcount machinery) **plus** module-style integrations with specific I/O paths. The integrations expose the same uniform view API to L2+ but pair with specific L0 backends and handle their idioms. **Maturity** below names how settled the module's contract is, not a schedule.

### `view_basic` — contiguous segments

- **Maturity**: defined in v1.
- **Pairs with**: any L0 backend that exposes contiguous segments (`mem_heap`, `mem_pool_*`, `mem_mmio`, `mem_dma_buffer`).
- **What it provides**: the canonical view/rope ops (clone-on-copy, RAII release, `subview`, rope `append`/`concat`/`walk`).
- **Footprint**: ~1 KB code.
- **When to use**: every host. The default integration.

### `view_pbuf` — lwIP pbuf chains

- **Maturity**: defined; realised alongside an lwIP-based TCP transport.
- **Pairs with**: `mem_lwip_pbuf`.
- **What it provides**: rope-aware wrap of lwIP pbuf chains. Each pbuf in a chain becomes one rope link; the rope's logical length equals the pbuf chain's `tot_len`.
- **Footprint**: ~600 bytes code on top of `view_basic`.
- **When to use**: lwIP-using hosts (ESP-IDF, mbed-OS, FreeRTOS+TCP).
- **Special semantics**: `release` walks the chain and calls `pbuf_free` exactly once per pbuf head; sub-views into individual pbufs share the chain's refcount via lwIP's own pbuf-ref mechanism.

### `view_iovec` — POSIX scatter-gather

- **Maturity**: defined; realised alongside a syscall-based TCP transport.
- **Pairs with**: any backend; converts a rope to/from POSIX `struct iovec` arrays.
- **What it provides**:
  - rope → `iovec[]`: adapts `rope_t::to_iovec()`'s spans into an `iov[]` array so that `writev` / `sendmsg` emits the rope's bytes in one syscall.
  - `iovec[]` → rope: wraps an `iov[]` into a rope, with each element backed by an externally-owned segment (typical for `recvmsg`).
- **Footprint**: ~400 bytes.
- **When to use**: any time the underlying syscall accepts scatter-gather. Avoids one host-side copy per egress.

### `view_dma_descriptor` — DMA scatter-gather descriptor lists

- **Maturity**: defined; realised alongside the CAN and SPI/I²S DMA paths.
- **Pairs with**: `mem_dma_buffer`.
- **What it provides**: wraps DMA scatter-gather descriptor lists as ropes. Cooperates with the DMA controller's scatter-gather engine when present (ESP32 GDMA, STM32 BDMA SG mode).
- **Special semantics**: cache-coherency hooks (`before_io` / `after_io`) fire automatically at view egress / ingress boundaries.

### `view_uart_simple` — UART ring buffer

- **Maturity**: per-target; pulled in by need.
- **Pairs with**: `mem_uart_rx_simple`.
- **What it provides**: a ring-buffer-style segment whose currently-valid bytes are exposed as a single contiguous view. Each completed-TLV detection produces a sub-view; the underlying ring's free space is reclaimed when all sub-views into the consumed range release.
- **Footprint**: ~300 bytes per peripheral.
- **When to use**: bare-metal MCU with UART-only I/O, no DMA available.

### `view_uart_dma` — UART double-buffered DMA

- **Maturity**: per-target; pulled in by need.
- **Pairs with**: `mem_uart_rx_dma`.
- **What it provides**: the `view_uart_simple` semantics plus cache hooks and double-buffer / ping-pong handling. Each DMA-half-complete IRQ produces a view over the just-filled half; framers detect TLV boundaries and emit sub-views.
- **When to use**: Cortex-M7-class MCU with UART + DMA.

### `view_can_frames` — CAN reassembly slots

- **Maturity**: defined; realised alongside the CAN transport ([14-can-transport.md](14-can-transport.md)).
- **Pairs with**: `can_reassembly`.
- **What it provides**: per-peer reassembly buffer surfaced as a view once a complete TLV's frames have arrived. The reassembly pool is per-`(peer × inflight)`; on RX-frame, bytes accumulate; on completion, a view is handed off and the slot is reclaimed when the view releases.
- **Special semantics**: timeout reclamation if a reassembly never completes; emits `STATUS=ERROR(TIMEOUT)` at the transport ingress.

### `view_shm` — cross-process shared memory

- **Maturity**: post-v1.
- **Pairs with**: `mem_shared`.
- **What it provides**: views into shared-memory regions visible to multiple processes. Refcounting uses cross-process atomic primitives (futex on Linux, equivalents elsewhere).
- **When to use**: intra-host inter-process libtracer.

### `view_iceoryx2` — iceoryx2 sample loans

- **Maturity**: sketched.
- **Pairs with**: `mem_iceoryx2`.
- **What it provides**: views over iceoryx2 sample loans. The view's lifetime is tied to the sample loan; release returns the sample to the publisher's pool.

### `view_rdma` — RDMA-registered regions

- **Maturity**: aspirational.
- **Pairs with**: `mem_rdma`.
- **What it provides**: views over RDMA-registered memory regions. Egress hands the view's iovec to libfabric / UCX for one-sided RDMA write; ingress wraps an incoming RDMA buffer.

---

## Module pairing

Some L0 backend / L1 module pairings are natural and standard:

| L0 backend | Natural L1 module |
| ---- | ---- |
| `mem_heap` | `view_basic` (and `view_iovec` for syscall paths) |
| `mem_pool_static` | `view_basic` |
| `mem_pool_class` | `view_basic` |
| `mem_lwip_pbuf` | `view_pbuf` |
| `mem_skbuff` | (kernel-only future) |
| `mem_dma_buffer` | `view_dma_descriptor` (or `view_basic` if no SG) |
| `mem_mmio` | `view_basic` (with permanent-refcount semantics) |
| `mem_shared` | `view_shm` |
| `mem_iceoryx2` | `view_iceoryx2` |
| `mem_rdma` | `view_rdma` |
| `mem_uart_rx_simple` | `view_uart_simple` |
| `mem_uart_rx_dma` | `view_uart_dma` |
| `can_reassembly` | `view_can_frames` |

A host loads only the pairings it needs. A one-UART bare-metal build loads `mem_uart_rx_simple` + `view_uart_simple` plus a tiny `mem_pool_static` for outgoing TLVs. A gateway loads heap + pbuf + DMA + their corresponding view modules.

---

## Cross-substrate transitions

When a TLV crosses a substrate boundary (e.g., received over lwIP and forwarded to CAN), the L1 layer handles the transition. Two patterns:

### Pattern A: re-chain (zero-copy, when target is also rope-friendly)

If the target transport accepts iovec-style scatter-gather, the rope view is handed over as-is. Each segment retains its L0 backend; the target transport's egress walks the rope and emits per-link bytes through whatever its egress facility is.

Example: TLV received over lwIP (pbuf rope) forwarded to a Linux raw-socket transport that uses `sendmsg` — `rope.to_iovec()` produces the per-link spans (adapted to a `struct iovec[]`), the kernel does scatter-gather DMA, no userspace copy.

### Pattern B: materialize (single-copy, when target needs a flat buffer)

If the target transport needs a contiguous buffer (CAN with limited DMA descriptors, UART with byte-by-byte FIFO, a transport without iovec support), the transport egress materializes the rope into a flat segment in the target's substrate.

Example: TLV received over lwIP (pbuf rope) forwarded to CAN — the egress allocates a CAN-capable segment from `can_reassembly`'s TX pool, walks the pbuf rope, and copies bytes into the CAN segment. One copy at the transport boundary; no further copies during CAN egress.

The transition cost is **per cross-substrate hop**, not per fanout. Subscribers on the lwIP side see zero-copy delivery regardless; only the cross-substrate traffic pays.

---

## Worked examples

### A. GPIO MMIO register as a TLV vertex

Goal: expose `*(uint32_t *)0x40020010` (STM32F4 GPIOA IDR) as a libtracer vertex returning a `VALUE` TLV whose 4 payload bytes are the live register value. The shape (informative sketch, not a header dump):

```cpp
// 1. A permanent segment over the MMIO region. Its refcount is held forever by
//    a static descriptor; mem_mmio's destroy is a no-op (the "memory" is
//    hardware — see 09-memory-substrate.md §mem_mmio).
segment_ptr_t idr_seg = mmio_region(/*base=*/0x40020010, /*size=*/4);

// 2. A permanent segment over the 4 static TLV header bytes
//    (type=VALUE, opt=0, length=4 u16 LE): { 0x01, 0x00, 0x04, 0x00 }.
segment_ptr_t header_seg = static_const_region(header_bytes);

// 3. Chain a two-link rope: [header][live register]. No bytes copied.
rope_t tlv = rope_t{view_t::over(header_seg)} + view_t::over(idr_seg);

// 4. Register the rope as the read-handler for /gpio/A/IDR.
//    Each read returns a copy of the rope — a refcount bump on both segments.
//    The MMIO segment is read live every time (its bytes ARE the register).
tracer_register_read_only_vertex("/gpio/A/IDR", read_gpio_idr, tlv);
```

Result: every read of `/gpio/A/IDR` returns a 2-link rope. The header bytes are static (refcount-permanent); the payload bytes are at the live register address. **Zero copy, ever.** A subscriber reading the TLV's payload bytes literally reads from `0x40020010`.

### B. TCP receive over lwIP, fanout to two subscribers

```
1. lwIP delivers pbuf (chain of 2 links, total 800 bytes) to libtracer netif callback.
2. mem_lwip_pbuf_wrap(pbuf) → segment_t with destroy=pbuf_free, refcount=1.
3. Framer parses TLV: 4-byte header + 796-byte payload, fits within pbuf chain.
   Constructs a 2-link rope view over the relevant pbuf links.
4. Router fans out to 2 subscribers:
     copy of the rope for sub 1  (refcounts on both pbufs += 1)
     copy of the rope for sub 2  (refcounts on both pbufs += 1)
5. Original rope is dropped (refcounts -= 1).
   Both pbufs hold refcount = 2 (one per subscriber).
6. Sub 1 consumes and releases. Refcounts → 1.
7. Sub 2 consumes and releases. Refcounts → 0.
8. lwIP's pbuf_free fires for each pbuf. Memory returned to pbuf pool.
```

No userspace copy from receive to delivery. The pbufs stay alive exactly as long as the slowest subscriber needs them.

### C. ADC DMA stream

```
1. ADC fills a 4 KiB DMA buffer (segment from mem_dma_buffer).
2. On DMA-half-complete: backend.after_io(seg, io_dir_t::DEVICE_TO_CPU) → cache invalidate.
3. Framer wraps the just-filled half as a view; emits address-shift slices
   ([06-user-data-packing.md] §streaming a high-speed ADC).
4. Each slice is a sub-view (offset, length) into the DMA segment.
   refcount += N (one per slice).
5. Subscribers consume slices; as each releases, refcount decrements.
6. When all slices in this half are consumed, refcount returns to baseline.
   The DMA half is reusable for the next fill.
```

The DMA buffer's refcount is the back-pressure signal: if subscribers are slow, refcount stays high, the next half-complete IRQ may find the previous half not yet released, and the framer either drops (per QoS) or stalls (per QoS). No copy at any point in the data path.

### D. CAN reassembly

```
1. CAN frame arrives with sequence-bit indicating "first" of a multi-frame TLV.
2. can_reassembly allocates a per-peer slot, copies first-frame payload.
3. Subsequent frames append (each is one HW DMA into the reassembly slot).
4. Last-frame bit: framer constructs a view over the completed reassembly slot.
5. View is dispatched to subscribers; refcount holds the slot.
6. When all subscribers release, the slot returns to the reassembly pool.
```

CAN's hardware framing forces one copy per frame into the reassembly slot; from there to the graph it is zero-copy via the view.

---

## End-to-end trace: ADC sample by DMA, fanned out over the network

This is the **acid test** for the zero-copy claim. A single ADC sample arrives via DMA and lands on a multicast subscriber's buffer with no intermediate copy of the sample bytes. Every layer is named.

### Setup

- **Hardware**: STM32 with ADC peripheral driving DMA into a 4 KiB ring buffer in main RAM. DMA is configured as double-buffered (half-complete IRQ + complete IRQ).
- **L0 backend**: `mem_dma_buffer` owns the 4 KiB ring as one persistent segment with two halves. The segment is preallocated at boot; `alloc()` is **never called** for this segment — it is an MMIO-shaped backend, with its `destroy()` recycling halves back to a half-pool.
- **L1 module**: `view_dma_descriptor` paired with `mem_dma_buffer`.
- **L2 codec**: `frame_codec` constructs a `USER_SAMPLE_RECORD` TLV (user-range type code `0x80`, `opt.PL=1`).
- **L4 vertex**: `/adc/raw` is a graph vertex with two subscribers — one local recorder, one multicast UDP subscriber.
- **Transport**: `transport_udp` configured for multicast on a LAN.

### Per-step trace

```
Step 1 — Hardware fills DMA buffer half A.
   The ADC peripheral writes samples directly into bytes [0..2047] of the
   ring. The CPU is not involved. No libtracer code runs.

Step 2 — DMA-half-complete IRQ fires.
   The ISR enters mem_dma_buffer.on_half_complete(half = A).

Step 3 — Cache invalidate (L0 → L1 hand-off).
   mem_dma_buffer calls after_io(seg = A, io_dir_t::DEVICE_TO_CPU).
   On a non-coherent SoC: invalidates cache lines covering [0..2047].
   On a coherent SoC: no-op.
   At this point, CPU reads of [0..2047] see the just-DMA'd bytes.

Step 4 — view_dma_descriptor creates a view (L1).
   view_t payload_v = {segment A, offset = 0, length = 2048}.
   This bumps segment_A.refcount from 1 (the static "DMA owns it" count)
   to 2. No copy.

Step 5 — Frame codec wraps as a TLV (L1 → L2).
   The header bytes (4 bytes for type=0x80, opt=PL|TS=1, length=2048) are
   constructed in a small heap segment H from a tiny header pool:
     view_t header_v = {segment H, offset = 0, length = 4}.
   The TLV-as-rope is:
     rope_t{header_v} + payload_v (+ optional trailer_v)
   This is a 2- or 3-link rope. No bulk-payload copy. The header's bytes
   are computed once into segment H; the payload's bytes are still in
   segment A's just-DMA'd half.

Step 6 — TLV registry recognizes the type code (L2 → L3).
   tlv_registry sees type=0x80 (user range) and opt.PL=1 — the TLV is
   structured. For dispatch purposes, the registry treats this as opaque:
   it is forwarded to the graph layer as the bytes-of-this-TLV.

Step 7 — Graph runtime dispatches to /adc/raw (L3 → L4).
   graph_runtime.dispatch(path = "/adc/raw", tlv = rope) looks up the
   vertex, finds two registered subscribers.

Step 8 — Dispatcher fans out to subscribers (L4).
   For each subscriber:
     - the dispatcher copies the rope. This bumps refcounts on every
       segment in the chain: segment_H.refcount++, segment_A.refcount++.
     - The subscriber's queue receives the rope copy.
   After fan-out, segment_A.refcount = 2 (DMA) + 1 (recorder) + 1 (UDP) = 4.

Step 9 — Local recorder consumes (subscriber 1).
   The recorder writes the rope's bytes to a memory-mapped log file via
   sendmsg-style scatter-gather (or by walking the rope and writev'ing).
   When done, the recorder drops its rope copy; RAII decrements every
   segment's refcount: segment_H.refcount--, segment_A--.
   segment_A.refcount = 3.

Step 10 — UDP transport consumes (subscriber 2).
   transport_udp.send_tlv(rope, peer = multicast_group):
     - There is no capability flag to consult: UDP has native
       scatter-gather, so it overrides the gathered-span send overload
       and no flatten copy happens.
     - It calls rope.to_iovec(), yielding one span per link (two here).
     - It calls the kernel's sendmsg() with that iovec.
     - The kernel's UDP stack constructs UDP/IP/Ethernet headers in
       its own buffer, then DMAs the headers + the iovec payload to the
       NIC. (Modern NICs scatter-gather natively; the iovec is preserved
       all the way down.)
     - sendmsg returns. transport_udp drops its rope copy.
   segment_H.refcount-- = 0  → header pool reclaims segment H.
   segment_A.refcount-- = 2.

Step 11 — DMA half A is reusable.
   segment_A.refcount = 2 = (DMA static) + (no live views).
   The static count means the segment never reaches 0. Instead,
   mem_dma_buffer's accounting tracks "live views beyond the static count."
   When that count drops to zero on segment_A, half A is marked reusable.
   The next ADC half-complete IRQ for half A finds it free and starts the
   cycle over.

Net data-path copies:    ZERO bytes copied for the 2 KiB sample payload.
Net allocations:         ONE small (4-byte) header segment from a fast
                         pool (recycled per-TLV in step 10's release).
NIC work:                The kernel constructs UDP/IP/Eth headers; the NIC
                         DMA-gathers from those + the libtracer iovec.
```

### Where copies would have appeared, but do not

- **Wire-format encoding step**: naïve encoders allocate a contiguous output buffer and copy header + payload into it. Here, the rope avoids that — the iovec to the kernel walks the rope as-is.
- **Fan-out step**: a single-buffer scheme would have to copy or arena-share. Here, refcount cloning of the rope hands every subscriber the same view tree.
- **Transport egress**: a transport that demanded a contiguous buffer would force `rope_t::flatten()`, costing one copy. UDP's scatter-gather avoids this; CAN, which has its own framing model, is the case where flattening or per-frame splitting happens.

### Where the trace breaks down

- **Egress to a transport without scatter-gather** (some embedded UART drivers): the forwarder calls `rope_t::flatten()` once at egress. One copy at the transport boundary, paid only on that path; subscribers on scatter-gather-capable paths see zero copies.
- **Cross-process transport**: the byte flow leaves the publisher's address space and one copy is always paid (§cross-process refcount on `mem_shared`).
- **Slow subscriber stalls the DMA cycle**: if subscribers do not release fast enough, the back-pressure manifests as `segment_A.refcount` not dropping; the next half-complete IRQ finds the half busy. Per QoS, this is either a drop or a stall. The data path itself remains zero-copy; the failure mode is throughput, not integrity.

This trace is the working specification for what "zero-copy" means in libtracer: the *bytes the application produced* (the ADC samples) are the *exact bytes the NIC sees*, with refcounted views threading them through every layer in between.

---

## Memory-binding contract

Memory binding is a **modular spectrum**, and libtracer is a **transparent byte router**: it imposes no snapshot, copy or CRC semantics on a backend ([ADR-0012 — modular memory binding, transparent router](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0012-modular-memory-binding-transparent-router.md)). Each hard integration below has a **recommended-safe** default, but a backend module may offer any point on the spectrum — snapshot, shadow vertex, or live/raw direct-register access, lock-free, no-CRC. The protocol does not stop a user reaching for the dangerous end; instead **each backend module owns and declares its per-architecture contract**: allocation, cache-coherency hooks, ISR-safety, atomicity granularity, memory ordering (x86 TSO versus weak ARM/MIPS), and `destroy` thread-affinity. CRC is an optional higher-layer concern ([01-data-format.md](01-data-format.md) `opt.CR`); a live, no-copy binding simply carries no CRC, or snapshots at CRC-compute time.

### Boost asio streambuf integration

A `boost::asio::streambuf` is a read-write buffer with **consume-on-read** semantics: `prepare()` may move or reallocate the underlying storage, so a view pinning some of its bytes is in conflict with the buffer's own advance. Four bindings resolve that conflict:

- **Wrap and pin** — modify or fork streambuf so consume waits on libtracer's view refcount.
- **Supply the buffer** — libtracer provides a type satisfying asio's `DynamicBuffer_v2` concept, backed by libtracer segments. libtracer then owns `consume()` and honours the pin itself, so nothing upstream changes and no bytes are copied.
- **Copy on import** — at the boost-asio↔libtracer boundary, copy bytes into a `mem_heap` segment. One copy per ingress.
- **Don't integrate** — leave the boost-asio↔libtracer boundary to user code; a copy-on-import shim is trivial to write against the public API.

**Rule**: v1 does not integrate. The boundary is user code.

Supplying the buffer is the only binding that is both copy-free and fork-free, and it is measured to buy nothing here: it costs 64 KiB of ring per connection against 232 B for an entire established connection, and the shipped transports already allocate nothing per frame on egress without it. Rationale in [ADR-0071 — the host transport is a separate TU](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0071-host-transport-is-a-separate-tu-with-shared-nothing-epoll.md); catalog entry in [10-module-catalog.md](10-module-catalog.md) §hard integrations.

### MMIO register-as-view: volatile bytes

A view over an MMIO register is a view onto bytes that change asynchronously. All three bindings are first-class — the user picks per backend:

- **Snapshot at view creation** (recommended-safe) — copy the register value into a small segment; stable bytes, any CRC consistent. Use for *publish-a-moment*.
- **Live view** — a `mem_mmio` segment pointing at the live register; the byte router stays transparent (no copy, typically no CRC). The backend declares its **atomicity granularity** (an aligned `u32` is torn-read-free on ARM/MIPS/x86; a multi-word register block is not) and may offer a **lock-free consistent read** (e.g. a seqlock: the reader retries on a writer version bump) for multi-word live data. ISR/SMP safety and any memory barriers are the backend's declared contract.
- **No-CRC raw** — a live binding with `opt.CR=0` is a pure transparent conduit; a CRC over volatile bytes is meaningless, so a live binding either omits the CRC or snapshots at compute time.

**Rule**: all three ship. Snapshot is the recommended-safe default and *publish-a-moment* the recommended mental model; live, raw and lock-free bindings are first-class for users who own the hazard.

### Cross-process refcount on `mem_shared`

A POSIX SHM region mapped into multiple processes has independent libtracer refcounts in each. They cannot decrement each other.

- **Single-publisher, multi-reader** — only the publisher owns the segment; readers' views are reaped by the publisher's heartbeat-GC. Acceptable for unidirectional pub/sub. **The publisher MUST NOT reclaim a segment until readers have observably released it, or a generation counter has invalidated stale views** — otherwise a reader mid-read races the reap. The grace/epoch is required, not optional.
- **Robust shared refcount** — atomic + robust mutex in the SHM region; every process participates. Complex.
- **Copy at process boundary** — each process treats the other's SHM as foreign; the boundary copies. No zero-copy across process.

**Rule**: single-publisher, multi-reader, and v1's `mem_shared` documents that constraint. A robust shared refcount belongs to an iceoryx2-style module.

### lwIP pbuf: aliasing the libtracer refcount with `pbuf_ref`/`pbuf_free`

Two libtracer subscribers cloning a `view_pbuf` over the same `pbuf*` create two libtracer-side refcount holds; lwIP-side, libtracer holds **one** `pbuf_ref` for the segment's lifetime. The hazard is calling `pbuf_free` from a non-lwIP-thread context — from an interrupt, or from a view destroyed on another thread.

**Rule**: a pbuf segment's `destroy` callback schedules `pbuf_free` via `tcpip_callback` (or the equivalent), never frees synchronously from outside lwIP's thread context. The backend documents this explicitly.

### Rope walk versus flatten

A scatter-gather-capable transport walks the rope at egress (zero-copy). A flat-buffer-only transport must materialize the spans into one contiguous buffer first (one copy).

**Rule**: there is **no capability flag** — the choice is made by virtual dispatch, at the transport, and the caller never branches on it. Egress always hands the rope's spans to the scatter-gather overload `transport_t::send(std::span<const std::span<const std::byte>>)`. A transport with native scatter-gather **overrides** it and writes the spans as one `sendmsg`/`writev` (`transport_udp`, `transport_tcp`, `transport_ws`, `transport_quic`, `transport_webtransport`); a flat-buffer-only transport — `transport_can`, and any embedder's — inherits the base-class default, which gathers the spans into one temporary and re-enters the contiguous `send`, paying exactly one copy ([`core/include/libtracer/transport.hpp:246-268`](https://github.com/avatarsd-llc/libtracer/blob/main/core/include/libtracer/transport.hpp)). That default reserves through the **nothrow** probe and **drops** the frame on an exhausted heap rather than aborting a no-exceptions build (#477) — the forward hot path reaches it.

`rope_t::flatten()` is the caller-side spelling of the same copy, and nothing on the **egress** path calls it: its in-tree callers are all ingress/ownership-side (`core/src/tlv_view.cpp:75`, `core/src/op_resolve_view.cpp:136`).

### DMA cache-coherency races

On non-coherent SoCs, the `before_io` / `after_io` hooks must be called at exactly the right moment. Missing them yields stale CPU reads or clobbered DMA writes.

**Rule**: `mem_dma_buffer`'s ISR is the only place that calls `after_io`. Application code never calls these hooks. The backend's spec documents the required interleaving.

### Reference to a live value (variable or register)

The "expose an endpoint backed by `&my_uint32` directly" pattern — both bindings are first-class:

- **Shadow vertex** (recommended) — the graph stores values; the publisher writes the value when the variable changes; subscribers read the shadow. Protocol-clean; no aliasing hazard.
- **Live view** — a `mem_mmio`-style segment over the live address; the byte router stays transparent. A real binding, not merely a footgun helper: the backend declares its atomicity/ordering/ISR contract per [§MMIO register-as-view](#mmio-register-as-view-volatile-bytes), and atomic or lock-free access is the backend's to provide. Exposed as `tracer_attach_register(&my_var)`.

**Rule**: shadow vertex is the recommended-safe default; the live binding is fully supported for users who own the hazard.

---

## Pitfalls

| Rule | Failure mode when it is missed |
| ---- | ---- |
| A view borrows bytes; the segment refcount is the only thing keeping them alive. | Code that takes a raw pointer out of a view's byte span and keeps it after the view is destroyed reads reclaimed memory. The decoded TLV tree borrows too: releasing the view while a `tlv_t` decoded from it is live is the same bug one layer up. |
| A DEVICE window is not CPU-addressable. | An implementation that memcpys or CRCs a rope without checking that every link is HOST dereferences device memory. `flatten` returns an empty view for a non-HOST rope and rope validation rejects a device link with `FRAME_INVALID` — a caller that ignores the empty view drops the payload silently and blames the transport. |
| A link boundary may fall anywhere, including mid-TLV-header. | A reader that casts the first link's leading bytes as a header misreads any straddling frame, and does so only under fragmentation patterns the local transport rarely produces — so it passes on loopback and fails against a peer. Rope-aware readers stitch a straddling header through a small bounded scratch. |
| Nesting depth is bounded by the receiver's decode resources, not by a constant. | An implementation that hardcodes a depth number rejects frames a conforming peer may legitimately send, and reports `TLV_NESTING_TOO_DEEP` for a frame that is not too deep for anyone else. |
| Every path validates before a consumer sees bytes. | Skipping validation because "the forwarder does not validate either" is a misread: the forwarder peeks at framing an earlier step already accepted. A cast that trusts unvalidated bytes turns a malformed frame into out-of-bounds reads. |
| Validation may be staged, so a check passing at ingress does not mean the whole tree is valid. | A consumer that mutates state per child as it walks can apply half a frame before a deeper child's CRC fails. An endpoint whose members form one transaction verifies all, then applies. |
| Back-pressure appears as a refcount that does not drop. | Reading that as a leak sends the investigation into the L0 backend. The real question is which subscriber is holding a rope past its consume window. |
| `flatten` is failable. | Its failure is an empty view, not an exception. On a target built without exceptions this is the only signal there is, and an unchecked result is a dropped payload. |
| The pbuf `destroy` callback runs wherever the last view dies. | Freeing a pbuf from an ISR or a non-lwIP thread corrupts lwIP's pool; the crash surfaces later and elsewhere. |

---

## What L1 does not specify

- The wire format that views are interpreted as — see [01-data-format.md](01-data-format.md).
- The graph-level meaning of a TLV at a vertex — see [02-graph-model.md](02-graph-model.md).
- Per-substrate allocation and destruction details — see [09-memory-substrate.md](09-memory-substrate.md).
- Transport-level framing (when bytes form a complete TLV) — see [10-module-catalog.md](10-module-catalog.md).
- Memory-pressure policy — see [09-memory-substrate.md](09-memory-substrate.md) §pressure and pool exhaustion.
