# A `DEVICE` link, and why `NOT_HOST` is permanent (L0/L1 substrate)

A segment carries the **address space** of the backend that made it. `HOST` bytes are
CPU-addressable; `DEVICE` bytes — GPU or accelerator memory
([ADR-0024](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md))
— are not, and the codec must never dereference them. A rope may therefore be
**heterogeneous**: a host header link chained to a device payload link. `all_host()` is the one
question a host-side operation asks before it touches a byte.

## What to notice

- **`NOT_HOST` is a property of the rope, not of the moment.** No retry ever fixes it; the
  payload has to leave via its device path. That is a different verdict from `NO_MEMORY`
  ([the pool page](view-pool-backend.md)), which is transient — and keeping them apart is
  exactly what [#917](https://github.com/avatarsd-llc/libtracer/issues/917) bought.
- **The refusal is about the *link*, not the rope type.** The example takes the host sub-range
  of the same rope and it is host, and flattenable. Nothing about a heterogeneous rope is
  poisoned wholesale.
- **`borrow_device` tags ordinary host memory `DEVICE`.** A vendor-free stand-in: it registers
  no byte-mover, so `mem::transfer` declines it — and it declines it for the *space* tag, which
  is the same refusal a real device link gets. Every verdict on this page is deterministic in a
  stock build with no accelerator present.
- **The real device backend is a `backends/`-tier module.** It registers its own transfer hook
  through `register_device_backend`; core assigns the space and nothing else. That tiering is
  why this example needs no CUDA and takes no skip.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/view_device_rope.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) · [views module](../modules/views.md) ·
[memory substrate reference](../reference/09-memory-substrate.md).
