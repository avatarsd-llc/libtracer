# Reference 09 — Memory substrate (L0)

> The memory layer beneath the wire format. Specifies the real-buffer / register / pool substrates that the view layer ([08-views-and-ownership.md](08-views-and-ownership.md)) refcounts and the TLV layer ([01-data-format.md](01-data-format.md)) is cast over.
> **Audience**: anyone implementing a memory backend for a new host or integrating libtracer with an existing buffer ecosystem (lwIP, Linux kernel skbuff, iceoryx2, RDMA-registered memory, peripheral DMA descriptors).

---

## What L0 is

L0 is the foundation: real bytes in real memory, owned by some real allocator with real lifetime rules. It is everything beneath the libtracer protocol — the buffer that holds TLV bytes, the register whose value the TLV represents, the pool that allocated the buffer, the queue that delivered it from a peripheral.

libtracer **does not allocate memory itself**. It receives memory from L0 backends, wraps it in refcounted views (L1, [08-views-and-ownership.md](08-views-and-ownership.md)), and interprets the bytes as TLV frames (L2, [01-data-format.md](01-data-format.md)). Every byte that flows through libtracer is owned at L0 by something more concrete than libtracer cares to know — and L0 is where that ownership story is honored.

This separation is what makes zero-copy real:

- A TLV constructed over a TCP receive buffer doesn't copy bytes out of the receive buffer — it just holds a view onto it, with the receive buffer's lifetime extended by the view's refcount.
- A TLV constructed over a memory-mapped GPIO register doesn't copy the register value — it just points the view at the register's address.
- A TLV split across a chain of lwIP `pbuf`s doesn't materialize the chain into one buffer — it holds a rope of views, each into one pbuf.

---

## Why a substrate layer

Real systems don't have one kind of memory. A robot fleet might run libtracer across:

- An ESP32 with `heap_caps` allocator and lwIP pbufs.
- An STM32 with a static pool, peripheral DMA buffers, and bare MMIO.
- A Linux gateway with `malloc`, `mmap`, and skbuffs.
- A future RDMA appliance with libfabric-registered pinned memory.

The L2 wire format is the same on all of them. The L1 view abstraction is the same. But the L0 memory substrate is wildly different. Cramming all these into one allocator strategy would either:

- Force the lowest common denominator (small fixed pool everywhere — wastes the host's capability), or
- Force every host to implement the most capable substrate (large RTOS-class memory infrastructure on a Cortex-M0 — impossible).

Splitting L0 into substrate **backends** (modules) lets each host pull only what it needs. A bare-metal STM32 with simple I/O pulls in the static-pool backend. A gateway pulls in heap + lwIP pbuf + skbuff backends. The protocol code above L1 sees the same view abstraction regardless.

---

## Categories of substrate

The substrates libtracer must integrate with fall into a few categories. This catalog informs the backend abstraction (next section) and the catalog of backends (the section after).

### Allocator-managed memory

Bytes live in heap or in a pool; allocation and free are explicit operations.

- `malloc` / `free` (host-class).
- jemalloc, tcmalloc (host-class with better fragmentation).
- ESP-IDF `heap_caps` (region-tagged: DRAM, IRAM, DMA-capable, SPI-RAM).
- FreeRTOS `pvPortMalloc` / `vPortFree`.
- Static-class pools (preallocated slabs by size class).
- Linux kernel `mempool` variants.

Lifetime: explicit. Ownership: whoever holds the pointer must eventually call the matching free.

### Network-stack buffers

Bytes arrive from the network and live in stack-managed buffers with their own lifetime conventions.

- lwIP `pbuf` — chain-of-buffer, refcounted, multiple types (`POOL`, `RAM`, `REF`, `ROM`).
- Linux `sk_buff` — kernel-only, refcounted, complex linkage.
- BSD `mbuf` — chain-of-buffer.
- ESP-IDF Wi-Fi RX queues — fixed-size frames, drained via callback.

Lifetime: stack-managed. Ownership: typically refcounted; the stack expects callers to release.

### Memory-mapped I/O

Bytes are at fixed addresses; "allocation" is meaningless because the memory was never allocated — it's hardware.

- GPIO data / set / clear registers.
- Peripheral SFRs (UART data registers, CAN mailboxes, I²C transfer FIFOs).
- Memory-mapped flash regions (read-only data sections).

Lifetime: permanent (until power-off). Ownership: hardware.

### DMA buffers

Bytes are in cache-coherency-aware regions where ownership oscillates between CPU and hardware.

- TX descriptors: CPU writes, then hands to hardware; hardware DMAs out, returns ownership.
- RX descriptors: hardware writes, then returns; CPU invalidates cache and reads.
- Cyclic DMA: two or more buffers ping-ponged between CPU and HW.

Lifetime: long-lived (allocated at init, reused). Ownership: oscillates between CPU and DMA controller.

### Shared memory

Bytes live in regions visible to multiple CPUs, processes, or cores.

- POSIX `shm_open` / `mmap`.
- System V shared memory.
- Multi-core shared SRAM on heterogeneous SoCs (RP2040 dual-core scratch SRAM, ESP32 dual-core DRAM).
- iceoryx2-managed segments.

Lifetime: explicit (created, mapped, eventually unmapped). Ownership: typically single-writer multi-reader by convention, sometimes refcounted.

### Hardware FIFOs

Bytes transit through peripheral hardware queues; the "buffer" is at fixed registers but its semantics are queue-like, not address-like.

- UART RX / TX FIFOs.
- CAN message mailboxes.
- I²C / SPI buffers.
- USB endpoint buffers.

Lifetime: continuous (data flows through, no stable identity per byte). Ownership: peripheral hardware.

### Substrate categories at a glance

```mermaid
flowchart TB
    subgraph CPU_OWNED["CPU-owned (free at will)"]
        A1[mem_heap]
        A2[mem_pool_static]
        A3[mem_pool_class]
    end
    subgraph HW_OWNED["HW-owned (lifetime determined externally)"]
        B1[mem_dma_buffer<br/><i>cache hooks required</i>]
        B2[mem_mmio<br/><i>permanent segment</i>]
        B3[mem_uart_rx_dma<br/>can_reassembly]
    end
    subgraph STACK_OWNED["Network-stack-owned"]
        C1[mem_lwip_pbuf]
        C2[mem_skbuff<br/><i>future</i>]
    end
    subgraph SHARED["Shared / cross-process"]
        D1[mem_shared<br/><i>single-publisher default</i>]
        D2[mem_iceoryx2<br/><i>future, robust SHM</i>]
        D3[mem_rdma<br/><i>aspirational</i>]
    end
    style CPU_OWNED fill:#dcfce7,stroke:#166534
    style HW_OWNED fill:#fef3c7,stroke:#92400e
    style STACK_OWNED fill:#dbeafe,stroke:#1e40af
    style SHARED fill:#fce7f3,stroke:#9f1239
```

All four families implement the same `tr::mem::mem_backend_t` interface ([§the backend abstraction](#the-backend-abstraction) below); the differences are in how they honor `destroy`, whether they need cache hooks, and what their per-segment lifetime story is.

---

## The backend abstraction

Each L0 substrate is wrapped behind a small seam. The reference implementation expresses it as a C++23 base class — `tr::mem::mem_backend_t` — that a backend subclasses; this is the **reference implementation's** seam, not a normative cross-language ABI (implementations interoperate over the wire, never via a shared ABI — [ADR-0013](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0013-v1-scope-boundaries.md)). The seam intentionally does NOT make allocation mandatory — many substrates can't allocate (MMIO, hardware FIFOs):

```cpp
namespace tr::mem {

// Direction of a DMA / cache-coherency transfer. The hook METHOD carries the
// timing; this enum carries only the DIRECTION; the backend maps the pair.
enum class io_dir_t : std::uint8_t {
    DEVICE_TO_CPU = 1,   // CPU will read bytes a device just wrote (invalidate)
    CPU_TO_DEVICE = 2,   // a device will read bytes the CPU just wrote (clean)
};

// An OPAQUE, backend-private allocation hint (a strong typedef). Its meaning is
// defined by — and private to — the backend that interprets it; there is NO
// cross-backend hint vocabulary or registry. A backend that ignores hints MUST
// accept any value. This is the anti-bloat fence.
enum class alloc_hint_t : std::uint32_t { NONE = 0 };

// The L0 substrate seam. Subclass to bind libtracer to any allocator — a heap,
// a fixed caller-owned arena, live registers, lwIP pbufs, DMA descriptors.
class mem_backend_t {
   public:
    explicit mem_backend_t(const char* name) noexcept;
    virtual ~mem_backend_t() = default;

    // Allocate a fresh segment of at least `size` bytes (refcount = 1, for the
    // caller to adopt). Returns nullptr on backpressure / OOM / unsupported
    // (MMIO, hardware FIFOs). Note the return is a raw segment_t*, NOT a
    // segment_ptr_t: if alloc returned the L1 owning handle, tr::mem would
    // depend on tr::view (an upward layering violation). The caller adopts it
    // via tr::view::segment_ptr_t::adopt(seg).
    [[nodiscard]] virtual tr::view::segment_t* alloc(
        std::size_t size, alloc_hint_t hint = alloc_hint_t::NONE) {
        return nullptr;
    }

    // Reclaim a segment whose refcount has reached zero — the ONLY reclaim
    // hook. There is NO release() method: the L1 owning handle (segment_ptr_t)
    // does the acq_rel refcount decrement in its destructor/reset and calls
    // backend->destroy(seg) only when the count hits zero. Never called on a
    // live segment.
    virtual void destroy(tr::view::segment_t* seg) noexcept = 0;

    // Optional cache-coherency hooks for non-coherent DMA paths. `before_io`
    // preps the cache before the buffer is handed to a transfer; `after_io`
    // reconciles the cache after the transfer completes. No-ops by default and
    // on cacheless cores (Cortex-M0/M3/M4); only DMA-class backends override.
    virtual void before_io(tr::view::segment_t* seg, io_dir_t dir) noexcept {}
    virtual void after_io (tr::view::segment_t* seg, io_dir_t dir) noexcept {}

    [[nodiscard]] virtual std::size_t alignment()        const noexcept;
    [[nodiscard]] virtual std::size_t max_segment_size() const noexcept;

    [[nodiscard]] const char* name() const noexcept;
};

}  // namespace tr::mem
```

The `tr::view::segment_t` carries the refcount and a pointer back to its backend. It is an L1 (`tr::view`) type — not an L0 (`tr::mem`) one — precisely *because* it carries the refcount, the L1 ownership concern:

```cpp
namespace tr::view {

struct segment_t {
    detail::ref_count_t     refcount;  // intrusive: inc relaxed, dec acq_rel
    tr::mem::mem_backend_t* backend;   // who reclaims these bytes
    std::span<std::byte>    bytes;     // the real bytes (data + capacity)
    tr::mem::mem_space_t    space;     // HOST or DEVICE, inherited from the backend
};

}  // namespace tr::view
```

When a segment's refcount drops to zero (L1 machinery, [08-views-and-ownership.md](08-views-and-ownership.md): the `segment_ptr_t` owning handle's destructor/reset does the acq_rel decrement), the handle invokes `backend->destroy(seg)`. The `destroy` override is **backend-specific**:

- heap-backend: `free` the bytes, then the segment control block.
- pool-backend: return the slot to the pool free list; the segment is part of a static array.
- lwIP-backend: `pbuf_free(pbuf)`, then the segment block.
- MMIO-backend: no-op (refcount is permanently held by a static segment descriptor).
- borrowed-backend: reclaim only the control block; the caller's bytes are never touched.

L0 is ignorant of L1 view semantics; L1 is ignorant of how L0 honors `destroy`. The protocol's zero-copy story is the contract that they cooperate via this seam.

### The second L0 seam: `block_source_t` (failable blocks)

`mem_backend_t` vends **refcounted `segment_t`s** — the right shape for payload bytes that many views share. The other shape is the **failable** one: the objects a node builds when it *registers* something (a vertex, a route label, a reassembly entry) have exactly **one owner** and no header, so a refcount on them is pure overhead. They are served by a second, deliberately smaller seam:

```cpp
namespace tr::mem {

// The nothrow failable-block seam. Raw bytes, single owner, no refcount.
class block_source_t {
   public:
    explicit constexpr block_source_t(const char* name) noexcept;
    virtual ~block_source_t() = default;

    // Storage for `bytes`, aligned to at least `align` — NOTHROW.
    // nullptr means exhaustion. It never falls back to the global heap and
    // never aborts; WHICH reject the caller answers is the caller's, and
    // follows the operation (see the consumers below).
    [[nodiscard]] virtual void* try_alloc(std::size_t bytes, std::size_t align) noexcept = 0;

    // Sized reclaim: `bytes`/`align` MUST match the try_alloc that served the
    // block, so a bump or pool source needs no per-block header.
    virtual void release(void* p, std::size_t bytes, std::size_t align) noexcept = 0;

    [[nodiscard]] const char* name() const noexcept;
};

// The process-wide default: the platform heap, nothrow.
[[nodiscard]] block_source_t& heap_source() noexcept;

}  // namespace tr::mem
```

Why it is a distinct type rather than a `std::pmr::memory_resource` with a documented "may return null" contract — the question is not stylistic, and the answer is checkable. `memory_resource::allocate` is annotated `__attribute__((__returns_nonnull__))` in libstdc++, so a caller's `if (p == nullptr)` is undefined behaviour and the optimizer may delete it. Inspecting the generated code on `riscv32-esp-elf-g++ 15.2.0` with the deployment flags shows it does exactly that, and **only at the size-optimized levels**:

| `-O0` | `-O1` | `-O2` | `-O3` | `-Os` | `-Oz` |
| --- | --- | --- | --- | --- | --- |
| check kept | kept | kept | kept | **deleted** | **deleted** |

`-Os` is what an ESP-IDF node ships (`CONFIG_COMPILER_OPTIMIZATION_SIZE`). A seam whose failure signal disappears at exactly the optimization level the target uses is not a seam, so failable allocation gets its own type, whose `try_alloc` carries no such annotation and whose name cannot be confused with `allocate` at a call site ([ADR-0065 — failable allocation gets its own seam](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0065-failable-allocation-gets-its-own-seam-block-source.md)).

The two seams are injected independently and a node may point both at the same underlying store ("one slab, whole stack") or split them.

Two companions ship with it, because the migrated call sites all need the same pair:

- **`bump_source_t`** — a caller-owned buffer handed out by bump, falling back to an upstream source once full. The nothrow twin of `std::pmr::monotonic_buffer_resource`. The upstream is what makes it a *capability-preserving* substitution: a monotonic resource also spills past its buffer, but it spills to a **throwing** resource, which on `-fno-exceptions` is the `abort()` this seam exists to remove. Pass `null_source()` as the upstream to make the buffer a hard bound instead.
- **`block_array_t<T>`** — a nothrow growable array of trivially-copyable `T`. Growth returns `false` instead of throwing, and relocation is a `memcpy`. Same four-word footprint as `std::pmr::vector`, one virtual call per growth instead of the allocator's two.

`block_array_t` exposes `push_slot()` — claim one uninitialized slot and fill it **in place** — alongside `push_back`. That is not a convenience: building a 48-byte element as a temporary and copying it in writes the aggregate field-by-field to the stack and reads it back as wide loads, and the resulting store-forwarding stall made the first working migration of the terminus decode **45 % slower while executing fewer instructions** (IPC 5.03 → 2.55); with `push_slot` the same decode measures **236 ns against the unmigrated 241–251 ns**. Hot paths use `push_slot`.

### Where the wire decode draws from

The terminus arena decoder is on the **RX path, behind no ACL**, and a peer chooses both the nesting depth and the node count of the frame it sends. All three of its draws (the node array, the walk's open-node stack, and the walk stack's spill past its inline slots) come from a `block_source_t`, so exhaustion is `TLV_NESTING_TOO_DEEP` — the status [RFC-0006](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0006-resource-bounded-nesting-depth.md) defines for "exceeds this receiver's decode resources" — and never an allocation failure. There is no depth constant anywhere in the decode, and none is wanted: the bound is the receiver's injected resource.

A **scope-lifetime** consumer composes this as a `bump_source_t` over a stack buffer — construct it, decode, drop it. The branch-write decode does exactly that with a 4 KiB stack buffer, naming the graph's injected failable seam as the bump's *upstream*, so overflow spills into the node's own injected store rather than the global heap and exhaustion stays a value (`core/src/graph.cpp:1266-1267`). Naming `null_source()` as the upstream instead makes the buffer a hard ceiling; that is the composition a node picks when it wants the stack buffer to *be* the bound.

A **long-lived** seam (a router's `rx`, a graph's `ctl`) must not be a bump source: bump blocks are never reclaimed, so it fills monotonically and then refuses everything. An 8 KiB bump source wired as a router's `rx`, **decoding a 53-byte FWD**, decoded **6 frames and rejected the next 194**. The frame size is what makes that a measurement rather than an anecdote — the figure is 8192 bytes divided by the arena footprint of one decode of *that* frame, so it is capacity arithmetic and does not vary with host or build flags ([ADR-0067 §1 — a bounded seam recycles through segregated size classes](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md)).

A long-lived bounded seam needs a **recycling** source, which is `pool_source_t` — segregated exact-size free lists over a caller slab, with **no per-block header** (the seam's sized `release` makes one unnecessary). Both bounds are injected: the caller supplies the slab *and* the span of `size_class_t` slots, so neither the byte ceiling nor the class count is a constant in the library.

Its one limitation is worth knowing before you size a slab: **classes do not share.** A freed 64 B block cannot serve a 128 B request. Replaying 70,937 recorded `try_alloc`/`release` events from the host suite — 12 distinct sizes, three of which cover 99.8 % of allocations — against each candidate policy, and taking the peak slab needed to serve every request:

| policy | slab | vs the 23,552 B peak-live floor |
| --- | ---: | ---: |
| segregated exact-size classes | 26,176 B | **+11.1 %** |
| first-fit + boundary-tag coalescing | 27,448 B | +16.5 % |
| TLSF (4 B header + second-level rounding) | 28,440 B | +20.8 % |

So the ~11 % is entirely the inability to reuse a freed block at a different class. A coalescing allocator can do that and still loses, because splitting a remainder under geometric growth rarely produces the size of the next request: re-running the replay with the header zeroed decomposes the gap as **1,088 B of external fragmentation against only 184 B of header**. `classes_used()` and `overflowed()` report what to size the class span against.

:::{warning}
**Own one per receiver; do not share one across receive threads.** A shared free-list pool
collapses to roughly a fifteenth of its own single-thread rate on a 12-core host
(T=1 8.3 M ops/s → T=8 1.36 M ops/s, p50 60 ns → 3,587 ns) while the platform heap
*scales* over the same sweep (15.8 M → 31.0 M ops/s, p50 30 ns → 70 ns); independently
reproduced on a 4-core CI runner. Pure serialization would hold flat at the T=1 figure, so
the collapse is a cacheline storm and **the problem is the shared list, not the flavour of
the guard** — a lock-free `[index | ABA-tag]` CAS on the list head replaces one contended
word with the same word ([ADR-0060 erratum 1](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0060-lkv-copy-store-injected-value-backend.md),
[ADR-0067 §3](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md)).
A router therefore accepts an optional per-child source; each transport has its own receive
thread, so a source parked on the child is touched by exactly one. That also makes the bound
per-peer: one noisy link cannot starve another's decode. A source shared at **wiring**
frequency (a graph's `ctl`) is fine behind a lock, which is the one place a `sync_mutex_t`-style
policy belongs.
:::

### Where the forward hop draws from

The decode arena is not the only thing a peer sizes. A FWD hop reading a **multi-link rope** must build a scatter-gather entry table whose length is one entry per straddled link per emitted region — again the sender's choice, again on a path behind no ACL, and this one is not even the terminus. The hop gathers it into a `block_array_t` over the same injected receive source, so exhaustion **drops the hop**.

Dropping is the whole answer, not half of one: emitting the entries that did fit would put a **truncated FWD on the wire**, which is strictly worse than silence, and FWD is not delivery-guaranteed — the sender retries. Note the shape difference from the decode: the decode has a *status* to return, so it answers by value; a forward hop has no reply channel, so its only honest answer is to stay quiet.

The **single-link** (contiguous) hop allocates nothing: each region yields exactly one sub-span, so the entry count is bounded by the frame layout and a stack array of that layout-derived size holds it (`core/include/libtracer/fwd_frame_view.hpp:846` — `kFwdMaxIov`).

### Divergent rejects: an open conformance question

The reject belongs to the operation, not to the seam. That is a deliberate rule, but it leaves the wire status a peer observes on exhaustion **unspecified**, and the three consumers of the block seam in the reference implementation demonstrate the spread:

| Consumer | Answer on exhaustion |
| --- | --- |
| Terminus decode | `tr::tlv::nesting_too_deep` — the status RFC-0006 defines for "exceeds this receiver's decode resources" |
| Branch-write decode | `tr::schema::type_mismatch` — it cannot distinguish "the value did not parse" from "the arena ran out", and does not try |
| Rope forward hop | nothing at all — no reply channel exists, so silence is the only sound answer |

Whether a conforming receiver **must** narrow the second of those to a resource status is open. A second implementer should therefore treat "which status names an exhausted decode arena" as unfixed, and must not build a peer that distinguishes resource exhaustion from a malformed value by status alone. `BACKPRESSURE` is what a *store* answers when its value backend is exhausted ([§Backpressure, not fallback](#backpressure-not-fallback)); it is not a property of the block seam.

---

## Backend catalog

Each entry: module-set membership, what it wraps, allocation supported, footprint, when to use, when to avoid. Membership in the v1 module set is a statement about the module *set* ([10-module-catalog.md](10-module-catalog.md)), not about what any one implementation has built. The reference implementation provides `mem_heap`, `mem_pool` (a fixed-slot pool plus a synchronized composition of it), `mem_borrowed` and `mem_cuda`; the remaining entries describe the substrate shape a backend for that category has to honor. Footprint figures are order-of-magnitude sizing budgets for a porter, not bench measurements.

### `mem_heap`

- **Module set**: v1.
- **Wraps**: `malloc` / `free`.
- **Allocation**: yes.
- **Footprint**: ~200 bytes plus per-segment overhead.
- **When to use**: Linux / macOS hosts; ESP-IDF when `heap_caps` granularity isn't needed.
- **When to avoid**: tightly-bounded MCUs (use a static pool); DMA-capable buffers (use `mem_dma_buffer`).

### `mem_pool_static`

- **Module set**: v1.
- **Wraps**: a single preallocated slab carved into fixed-size slots, with the free list threaded through the slab itself so there is no auxiliary heap.
- **Allocation**: yes (from free list); exhaustion is a `nullptr` return, never an OOM.
- **Footprint**: pool size + ~32 bytes per slot for header.
- **When to use**: bare-metal MCU; FreeRTOS without dynamic allocation; deterministic-latency targets.
- **When to avoid**: highly variable payload sizes (fragmentation in a single-class pool wastes memory; use `mem_pool_class`).

### `mem_pool_class`

- **Module set**: v1.
- **Wraps**: multiple fixed-slot pools at different size classes (e.g. 64, 256, 1024, 4096 bytes).
- **Allocation**: yes (chooses smallest class that fits).
- **Footprint**: sum of class pools.
- **When to use**: MCU with variable payload sizes; the general MCU choice where payload sizes spread.

### `mem_borrowed`

- **Module set**: v1.
- **Wraps**: caller-owned bytes — a register, a program variable, a const ROM table, any externally-owned buffer — as a segment that does **not** own them. `destroy` reclaims only the small control block libtracer allocated; the caller's bytes are never touched.
- **Allocation**: no — a borrow wraps bytes that already exist.
- **When to use**: transparent byte routing, where the protocol routes whatever bytes a binding exposes and imposes no snapshot, copy or CRC semantics ([ADR-0012 — modular memory binding, transparent router](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0012-modular-memory-binding-transparent-router.md)).
- **When to avoid**: anywhere the bytes must be durably stored — a borrowed segment declares `owns_bytes = false` and the write path must copy instead ([§why the value copy exists](#why-the-value-copy-exists)).

### `mem_lwip_pbuf`

- **Module set**: v1.
- **Wraps**: lwIP `pbuf` chains. The segment holds a pointer to a pbuf head; its `destroy` calls `pbuf_free`.
- **Allocation**: yes (`pbuf_alloc`).
- **Footprint**: dependent on lwIP's pool config; ~300 B code on top of lwIP itself.
- **When to use**: ESP-IDF and lwIP-using MCUs; the natural backend for TCP and UDP transports on those targets.
- **When to avoid**: hosts not using lwIP.

### `mem_skbuff`

- **Module set**: future (kernel-only; the v1 module set is userspace).
- **Wraps**: Linux kernel `sk_buff`. Applicable only to a kernel-side libtracer ingestion path.

### `mem_dma_buffer`

- **Module set**: v1.
- **Wraps**: a statically-allocated buffer registered with the SoC's DMA controller. Adds cache-coherency hooks via `before_io` / `after_io`.
- **Allocation**: yes (from a small DMA-capable pool — `heap_caps_malloc(MALLOC_CAP_DMA)` on ESP32, manual-region on STM32).
- **Footprint**: ~600 bytes code plus pool size.
- **When to use**: peripheral I/O on Cortex-M7 / -M33 with cache; ESP32 SPI/I²S DMA flows; CAN with HW FIFO.
- **When to avoid**: Cortex-M0/M3 without cache (DMA buffers are just regular memory — a static pool is enough).

### `mem_mmio`

- **Module set**: v1.
- **Wraps**: a static segment descriptor pointing at a fixed MMIO address. The `destroy` callback is a no-op; refcount is permanently held by an initial reference that's never released.
- **Allocation**: no.
- **Footprint**: ~40 bytes per registered region.
- **When to use**: exposing GPIO registers, peripheral SFRs, or memory-mapped flash as libtracer vertices ([06-user-data-packing.md](06-user-data-packing.md) §GPIO register example).
- **Note**: the truly zero-allocation static descriptor is not built in the reference implementation; `mem_borrowed` covers the same shape dynamically, at the cost of a heap-allocated control block per borrow.

### `mem_shared`

- **Module set**: future.
- **Wraps**: POSIX `shm_open` / `mmap` regions; on heterogeneous SoCs, multi-core shared SRAM.
- **Allocation**: yes (from the shm region's own free list).
- **When to use**: intra-host inter-process libtracer; a shared-memory transport's data plane.

### `mem_cuda`

- **Module set**: v1, opt-in.
- **Wraps**: CUDA device memory (`cudaMalloc` / `cudaFree`). The segment reports `DEVICE` space, so the codec must never CPU-dereference it; a device segment backs a VALUE payload inside a heterogeneous host+device rope ([ADR-0024 — the `mem_cuda` GPU backend](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md)).
- **Allocation**: yes.
- **When to use**: a host that publishes tensors already resident on the GPU.
- **When to avoid**: any MCU target.

### `mem_iceoryx2`

- **Module set**: future.
- **Wraps**: iceoryx2 `Sample<T>` loans. The segment holds the loan; its `destroy` returns the sample to the publisher.
- **When to use**: safety-cert intra-host zero-copy.
- **When to avoid**: any MCU target.

### `mem_rdma`

- **Module set**: future, aspirational.
- **Wraps**: RDMA-registered pinned memory. Cooperates with libfabric or UCX for the data plane.
- **When to use**: an HPC RDMA transport.

### `mem_uart_rx_simple`, `mem_uart_rx_dma`

- **Module set**: v1, per-target.
- **Wraps**: a per-peripheral RX buffer.
- **Allocation**: no — the buffer is statically allocated and reused; what's "allocated" at L1 is a view-segment over the current valid bytes.
- **Footprint**: small (200-500 bytes code per peripheral).
- **When to use**: bytes-arrive-incrementally patterns (UART, I²C in master-receive mode, SPI slave). The simple variant is byte-by-byte ISR; the DMA variant uses cache hooks.

### `can_reassembly` — not an L0 backend

- **Layer**: `tr::net`, not `tr::mem`. The type was originally named for L0, which was a layer inversion — an L0 type referencing the L1 rope it assembles. It sits beside the CAN transport ([ADR-0048 — one-wire grammar, chunk cursor, rope-aware decode](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md)); see [14-can-transport.md](14-can-transport.md) §Multi-frame reassembly.
- **Wraps**: a per-group reassembly buffer accumulating multi-frame CAN slices until a complete TLV is present; its structure is drawn from an injected resource with a config-bounded live-group count (evict-oldest plus a dropped-group counter), never OOM.
- **When to use**: internal to the CAN transport's RX path.

---

## Ownership at L0

Each backend is responsible for honoring **its own** ownership rules. libtracer's L1 refcount governs how long the segment **stays referenced**, but the actual destruction logic is L0's concern.

This decoupling has three consequences:

### 1. Lifetime extension is visible to L0

When a TLV is received over TCP into a lwIP pbuf, libtracer's L1 creates a view holding the pbuf segment with refcount=1. If the TLV is fanned out to N subscribers, refcount becomes N+1 (one for each subscriber, one for the original). The pbuf is **NOT freed** until all subscribers release their views. lwIP doesn't see the pbuf as "consumed"; the segment-refcount holds it alive.

This means the pbuf pool must be sized for the worst-case fan-out latency. A subscriber that holds views for too long applies pressure to the pbuf pool — and that pressure surfaces as the **pool's own** exhaustion (`tr::flow::backpressure`), not through a per-vertex cap: the `queue_max_bytes` knob this paragraph used to name was inert and was removed by [RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.E. Bounds come from the injected resource ([CONTEXT.md](../../CONTEXT.md) §No synthetic limits).

### 2. Different substrates have incompatible lifetime models

Heap segments can be freed in any order. lwIP pbufs MUST be freed via `pbuf_free`. MMIO "segments" are never freed. Pool slots return to their specific pool. The `destroy` callback per backend handles this; the L1 machinery is uniform above it.

### 3. Cross-substrate moves require copies (sometimes)

If a TLV arrives via lwIP pbuf and is forwarded to a CAN transport, the CAN transport's egress can't directly DMA out of the pbuf (CAN's DMA expects a different region). The transport egress materializes the rope of pbuf-views into a contiguous CAN-DMA buffer (one copy at the transport boundary).

This is an **egress-time** copy, not a per-fanout copy. Subscribers on the lwIP side still see zero-copy fanout; only the cross-substrate hop pays. See [02-graph-model.md](02-graph-model.md) §read-vs-write copy semantics for the per-transport copy table.

---

## I/O integration patterns

How L0 backends interact with the host's I/O paths.

### Polling RX (UART, I²C master-receive, SPI slave)

```
ISR or polling loop:
  byte arrives → write into mem_uart_rx_simple's static segment at offset cursor
  cursor++
  if (cursor matches a valid TLV tail by L2 framing rules):
      atomically promote: take a view over [tail-N..tail], hand to recv callback
      reset cursor (or wrap)
```

The L0 backend's job is just "manage the static segment and the cursor." The L1 view is constructed when the framer detects a complete TLV; the segment's refcount is bumped, the cursor advances. No copy.

### DMA RX (cache-coherent variant)

```
HW writes into mem_dma_buffer's preallocated segment (HW owns).
On DMA-complete IRQ:
  backend.after_io(seg, io_dir_t::DEVICE_TO_CPU)   // invalidates cache
  framer scans for TLV boundaries in seg
  for each complete TLV: create view, hand off
HW continues filling next segment (double-buffer or ring).
```

The cache-coherency hooks live in the backend; framers and L1 don't see them.

### lwIP RX

```
lwIP delivers a pbuf via netif input callback.
  segment_t* seg = wrap_pbuf(pbuf);            // refcount=1; destroy = pbuf_free
  auto owner = tr::view::segment_ptr_t::adopt(seg);  // L1 takes ownership
  framer parses TLVs out of the pbuf chain (may be a rope across pbuf links)
  for each complete TLV:
      view_t v = view_t::over(owner).subview(off, len)  // shares ownership, no copy
      hand to recv callback
  // owner drops here; the views the subscribers hold keep the segment alive
```

If the TLV spans multiple `pbuf` links (typical for large frames), the resulting view is a **rope** with one link per `pbuf` segment. See [08-views-and-ownership.md](08-views-and-ownership.md) §rope semantics.

### TX path mirror

```
Application creates TLV (a view tree, possibly a rope).
Transport sees an outgoing view tree; needs to emit bytes on the wire.
  for transport with iovec syscall (writev / sendmsg):
      backend extracts iovec from the view tree (zero-copy)
      kernel syscall does the per-iovec copy itself (one copy per element)
  for transport with single-buffer egress (CAN, simple UART):
      backend serializes the rope into a contiguous TX segment (one copy)
      TX segment is emitted byte-by-byte or via DMA
```

The view tree's structure determines whether egress is true zero-copy (iovec scatter-gather) or single-copy (rope flatten). The application doesn't choose this; the transport module does, based on the underlying I/O facility.

### Hardware-FIFO direct emit

```
Transport calls backend.alloc(size, hint).   // hint is a backend-private alloc_hint_t
  → returns a pseudo-segment whose base is the FIFO's MMIO data register
  → writes to base[0] enqueue into the FIFO
TLV is serialized one byte at a time into the FIFO.
```

The "segment" is fictional — there's no real buffer — but the abstraction holds: an L0 backend says "writes to this address are how the bytes leave the host."

---

## Cache coherency

On Cortex-M7 / -M33 / Cortex-A class CPUs with data caches, DMA buffers need explicit cache management:

- **Before HW reads a CPU-written buffer** (TX): clean the cache so HW sees the writes. `before_io(seg, io_dir_t::CPU_TO_DEVICE)`.
- **After HW has written a buffer the CPU will read** (RX): invalidate the cache so CPU doesn't see stale lines. `after_io(seg, io_dir_t::DEVICE_TO_CPU)`.

The method carries the *timing* (before vs. after the transfer); the `io_dir_t` carries the *direction*; the backend maps the pair to clean/invalidate. The `mem_dma_buffer` backend implements these; the rest of libtracer (L1, L2+) doesn't see cache concerns.

Cortex-M0 / -M3 / -M4 without cache: the hooks are no-ops.

The one in-tree caller of the hooks is **`tr::mem::transfer(seg, host, io_dir_t)`** ([ADR-0047 §2 — build-time closed module sets, compile-time seams](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md)) — the module-set's tag-dispatched host↔device byte-mover. `CPU_TO_DEVICE` copies host bytes into the segment; `DEVICE_TO_CPU` copies them back out. A host-addressable backend transfers with a `memcpy`, bracketed by `before_io`/`after_io` **only** when its `needs_cache_ops` trait is set (below), so a cacheless backend folds the bracket away at compile time. A `DEVICE`-space backend (`mem_cuda`) routes to its device copy (`cudaMemcpy` plus the `after_io` stream barrier). One seam covers both, rather than a CUDA-named pair of free functions.

### Module-set traits

Each concrete backend carries **compile-time contracts** as `static constexpr` members, replacing prose ([ADR-0047 §2](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md)): `needs_cache_ops` (does a transfer need the cache hooks — read by `mem::transfer`), `is_isr_safe` (are `alloc`/`destroy` callable from an ISR — a slot pool yes, a general heap no), and `owns_bytes` (are the bytes backend-owned and thus safe to durably store — false for a borrow). They are `static_assert`-able and, being compile-time, cost nothing on the MCU profile.

---

## Alignment

Each backend declares its alignment guarantee via `alignment()`. Typical values:

- `mem_heap`: 8 (host malloc usually).
- `mem_pool_static`: 4 or 8 (configurable per pool).
- `mem_dma_buffer`: 32 or 64 (typical DMA cache-line alignment).
- `mem_mmio`: declared per-region (a u32 register is 4-aligned; an 8-byte FIFO mailbox might be 8-aligned).
- `mem_lwip_pbuf`: 4 (lwIP's default).

L2 frame parsing tolerates unaligned access (per [01-data-format.md](01-data-format.md) §alignment), so this is informational rather than required. A higher-performance application that wants aligned access can request specific alignment when allocating; the backend may decline (return `nullptr`) if it can't satisfy.

---

## What L0 does not specify

- The protocol's wire bytes — see [01-data-format.md](01-data-format.md).
- The view abstraction or refcount semantics — see [08-views-and-ownership.md](08-views-and-ownership.md).
- Allocation policy (which backend a given vertex uses, fallback strategy on pool exhaustion) — application or framework concern.
- Cross-substrate routing decisions — forwarder / transport-egress concern.
- Garbage collection — there is none; lifetime is explicit per backend's destroy callback.

---

## Injection points

L0 backends are *supplied to* a runtime, never named by it — "which backend a given vertex uses" is the framework concern [§what L0 does not specify](#what-l0-does-not-specify) defers. The protocol-level obligation is narrow and has three parts. First, **every bound a receiver enforces is an injected resource**, not a constant compiled into the library: a host that wants a ceiling supplies the store, and a host that wants none supplies nothing and gets the platform allocator. Second, the three kinds of allocation a node makes — durable value bytes, the small control objects a stored write needs, and the failable blocks a *peer* can provoke — must be **separately boundable**, because bounding only some of them leaves the rest unbounded and the node unbounded with them. Third, **exhaustion of any of them is answered, never hidden**: the answer is a status where the operation has a reply channel and a drop where it does not, and it is never a silent fallback to another store. A concrete mapping of these three onto the reference implementation's constructor seams, with the composition guidance for pointing all of them at one slab, is in [../design/allocation-and-backpressure.md](../design/allocation-and-backpressure.md).

### Why the value copy exists

A stored write does not always own the bytes it is handed. The delivery seam ([ADR-0042 — refcounted receiver seam, view delivery](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0042-refcounted-receiver-seam-view-delivery.md)) distinguishes two tiers:

- **Owning delivery** — the transport hands the runtime a refcounted view over bytes it will keep alive (a reassembled frame, a pinned receive buffer). The last-known value stores a **subview** of that frame: zero copy, just a refcount bump ([08-views-and-ownership.md](08-views-and-ownership.md) §rope semantics).
- **Borrowed delivery** — the transport hands a *transient* span, valid only for the duration of the receive callback (a WebSocket receive buffer freed the moment the callback returns is the common case). The bytes cannot be pinned, so the last-known value must **copy** them into a segment it owns before the transient buffer disappears.

That copy is the single flatten on the write path, and it draws its owned segment from the injected value-bytes backend. On a node whose transport delivers borrowed, *every stored write* mints one such segment, so routing it through a pool rather than the general heap is what makes the write path's value memory deterministic and fragmentation-free.

### Backpressure, not fallback

When the injected value backend is exhausted — a pool with no free slot, or a value larger than the slot — `alloc` returns `nullptr` and the write **rejects with `STATUS=BACKPRESSURE`**; it does *not* silently fall back to the heap. A silent fallback would breach the bounded-memory guarantee the host bought by injecting a pool: exhaustion is an injected-resource signal, never a hidden allocation. The value-size distribution is the host's to compose (a uniform-telemetry pool, or a size-class / pool-plus-heap-fallback composite backend), never runtime logic — this is the "no synthetic limits" line ([00-overview.md](00-overview.md)) applied to value memory.

### Thread-safety of an injected value backend

A value segment self-routes its own reclaim: when the last reference drops, the L1 owning handle calls `backend->destroy(seg)` **on whatever thread dropped it** — typically a reader or subscriber, concurrent with a writer allocating the next value. An injected value backend must therefore be thread-safe. A general-purpose heap is; a slot pool is composed with the target's arch-appropriate synchronization — an interrupt-disable critical section on single-core MCUs, a spinlock on multi-core, never a heavyweight OS mutex whose round-trip would dominate the O(1) free-list op. Sharding the pool per lock-stripe does **not** remove the race (reclamation is cross-thread regardless of which stripe allocated the segment), so one thread-safe pool is the model.

The guard is a correctness requirement, not a scaling one, and the two must not be confused: on a many-core host a *shared* guarded pool is measured slower than the heap it replaced (the collapse figures in the warning under [§where the wire decode draws from](#where-the-wire-decode-draws-from)). On a single-core target with an interrupt-disable critical section there is no concurrent read-modify-write to storm, and a deterministic bounded ceiling is the point rather than throughput.

---

## Pressure and pool exhaustion

When a backend's allocation fails (heap returns NULL, pool is full):

- The transport module's recv path SHOULD drop the incoming TLV and emit `STATUS=ERROR(BACKPRESSURE)` to the source if a return path exists.
- A subscriber whose queue is full SHOULD apply its own drop policy (drop oldest, drop newest, or block); the *bound* is the capacity of the resource it was injected with, never a per-vertex magic number. Producer-side retention (the STREAM ring depth) is declared HOST-side by the vertex owner through `graph_t::set_history_depth`, and has no wire surface at all — the `:settings.history_keep_last` knob it used to be spelled with was deleted by [RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.C; the `queue_max_bytes` knob this line used to name never functioned and was removed by [RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.E.
- Backends MAY expose pool-utilization metrics via implementation-defined paths (e.g., `/_libtracer/mem/heap:utilization`); these are introspection conveniences, not protocol-level requirements.

The protocol does not mandate behavior under pressure; it specifies the **error reporting** (`STATUS=BACKPRESSURE`) and lets implementations choose the policy.

---

## Pitfalls

**A bump source parked on a long-lived seam stops working without failing loudly.** Bump blocks are never reclaimed, so the source fills monotonically and then refuses everything; the node does not abort and the seam behaves exactly as specified. An implementation that wires a bump source as a router's receive seam will serve a handful of frames and then reject every frame after, with no signal distinguishing that from a peer sending malformed input. A long-lived seam needs a recycling source.

**Sizing a bounded seam from a frame count is sizing it from the wrong number.** "Six frames from 8 KiB" is capacity arithmetic over one frame shape; a different frame shape gives a different count. Size from the arena footprint of the frames the deployment actually carries.

**A shared block source is a scaling hazard even when it is correct.** Adding a lock makes a shared free list correct and leaves it slower than an unbounded platform heap on a many-core host, because the contention is on the cacheline holding the list head rather than on the guard. An implementation that scales a bounded seam by improving the guard will measure no improvement. Scale by giving each receive thread its own source.

**Sizing a class-segregated pool by total bytes under-provisions it.** Classes do not share, so the peak slab is the sum of per-class peaks, not the peak of the sum. An implementation that sizes to the observed peak-live total will hit exhaustion while free blocks sit in the wrong class.

**Treating an exhaustion status as a parse error, or vice versa, is not safe.** The status a receiver returns on decode-arena exhaustion is not fixed across operations, and one in-tree operation reports it as a type mismatch. A peer that retries only on a resource status will not retry, and a peer that gives up on a schema status will give up on a transient condition.

**Durably storing borrowed bytes is a use-after-free, not a copy elision.** A backend that declares `owns_bytes = false` hands out bytes whose lifetime ends with the receive callback. The write path must copy them before storing; an implementation that stores the view instead sees the value change under it or read freed memory.
