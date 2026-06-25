# Changelog — core (libtracer reference implementation)

All notable changes to the **public API** of the `core/` reference implementation
(the headers under `include/libtracer/`) are recorded here, per
[CONTRIBUTING](../CONTRIBUTING.md) / [CLAUDE.md](../CLAUDE.md). This tracks the
*implementation's* C++ API — which is implementation-defined per
[ADR-0013](../docs/adr/0013-v1-scope-boundaries.md) and versioned independently of
the immutable **protocol-v1** wire format it implements.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The
reference implementation is pre-1.0; everything currently lives under
`[Unreleased]`.

## [Unreleased]

### Added

- **M2 — L0/L1 memory substrate.** The layer that owns the lifetime of the bytes
  M1's borrowed `Tlv` points at; makes the zero-copy claim safe, not just fast.
  - `<libtracer/backend.hpp>` — `MemBackend`, the small user-implementable
    allocation seam (subclass it to bind any allocator / arena), and `IoDir`.
    Each backend declares its own concurrency/coherency contract (ADR-0012).
  - `<libtracer/segment.hpp>` — `Segment` (refcounted bytes + backend) and the
    intrusive `SegmentPtr` handle. Uses the canonical intrusive_ptr orderings
    required by `docs/reference/02-graph-model.md` (increment `relaxed`,
    decrement `acq_rel`). Build with `-DLIBTRACER_NO_ATOMIC` for single-threaded
    / Cortex-M0 targets (plain integer refcount; no cross-thread sharing).
  - `<libtracer/mem_heap.hpp>` — `mem::heap_backend()`, `mem::heap_alloc()`: the
    owning host allocator backend.
  - `<libtracer/mem_borrowed.hpp>` — `mem::borrow()`, `mem::borrow_const()`: wrap
    caller-owned bytes with a no-op-on-bytes destroy. The transparent
    byte-router / live-raw MVP (ADR-0012).
  - `<libtracer/mem_pool.hpp>` — `mem::Pool`: a bounded fixed-slab backend over a
    caller-owned buffer; `alloc` returns `nullptr` on exhaustion (BACKPRESSURE).
    The free list is threaded through the slab — no auxiliary allocation.
  - `<libtracer/view.hpp>` — `View` (zero-copy `(segment, offset, length)`
    window; copy = clone) and `view_as_tlv()` (the L1→L2 cast: a TLV is a cast
    from a view).
  - `<libtracer/rope.hpp>` — `Rope` (a chain of views spanning segments):
    `concat`, `walk`, `to_iovec` (scatter-gather egress), `flatten` (one-copy
    materialize).

- **M1 — protocol-v1 wire codec.** The L2/L3 borrowed (zero-copy) codec.
  - `<libtracer/tlv.hpp>` — `Type` (the `0x01`–`0x0D` registry) and the `Opt`
    options bitfield (decode/encode, reserved-bit rejection).
  - `<libtracer/crc.hpp>` — `crc::crc32c()` (Castagnoli, default) and
    `crc::crc16_ccitt()`; header-only, `constexpr` tables.
  - `<libtracer/frame.hpp>` — the `Tlv` model (`Tlv`, `Trailer`, `Timestamp`,
    `Crc`), `decode()` (→ `std::expected<Tlv, Error>`, iterative, depth-capped,
    CRC-verified), `encode()`, and `equal()`.
  - `<libtracer/tracer.hpp>` — umbrella header including the whole public API.
