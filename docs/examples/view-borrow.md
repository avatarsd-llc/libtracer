# `borrow`: route the application's own bytes (L0/L1 substrate)

The L0/L1 substrate is a **binding layer**, and the far end of the spectrum is the
*transparent byte router*
([ADR-0012](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0012-modular-memory-binding-transparent-router.md),
[CONTEXT.md](../../CONTEXT.md) §Memory-binding spectrum): point a segment at an MMIO register,
a program variable, or a const ROM table, and libtracer routes those bytes — no copy, no
snapshot, no CRC imposed. `tr::view::borrow` allocates only the small `segment_t` control
block; the payload pointer is the caller's, and so is the lifetime promise.

The contrast that makes the choice visible is `over_bytes`, which **copies** into a fresh
segment. Both hand back a `view_t`; only one keeps the pointer.

## What to notice

- **Pointer identity is the whole distinction, and it is asserted both ways.** The borrowed
  view's `bytes().data()` *is* the application array's address; `over_bytes`'s is not.
- **A borrow is live, not a snapshot.** The example writes through the application buffer after
  taking the view and reads the new value back out of it — then writes again and shows the
  earlier `over_bytes` copy unchanged. Safety by snapshotting is *recommended*, never mandated.
- **The lifetime is the application's promise.** `mem_borrowed`'s `destroy` frees only the
  control block; the bytes are never touched. Its `owns_bytes` trait is `false`, which is the
  seam's way of saying such a segment must not be *durably stored*.
- **`borrow_const` is for ROM and other read-only bytes.** libtracer never writes through one;
  the `const_cast` inside merely restores the segment's uniform writable-at-the-type-level
  base.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/view_borrow.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) · [segment module](../modules/segment.md) ·
[memory substrate reference](../reference/09-memory-substrate.md).
