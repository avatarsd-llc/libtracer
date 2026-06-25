# Memory binding is a modular spectrum; libtracer is a transparent byte router

libtracer's L0/L1 memory substrate is a **modular, unopinionated binding layer**. An endpoint's bytes may be bound anywhere on a spectrum — a heap snapshot, a shadow vertex the publisher updates, or a live/raw view over an MMIO register or program variable (no copy, no CRC, lock-free) — and the choice is the user's, made by selecting a backend module ([09-memory-substrate.md](../reference/09-memory-substrate.md) `mem_backend_t`). In the live/raw case libtracer is a **transparent byte router**: it routes whatever bytes the binding exposes and imposes no snapshot, copy, or CRC semantics (CRC is an optional higher-layer concern, [01-data-format.md](../reference/01-data-format.md) `opt.CR`). The protocol does **not** forbid "dangerous" access; instead **each backend module owns and declares its per-architecture concurrency/coherency contract** — allocation, cache hooks (`prepare_for_io`/`finalize_after_io`), ISR-safety, atomicity granularity, memory ordering (x86 TSO vs weak ARM/MIPS), `destroy` thread-affinity, and any lock-free read protocol (e.g. a seqlock for consistent multi-word live reads). Safety (snapshot/shadow) is **recommended by default but never mandated**.

## Considered options

- **Mandate snapshot/shadow and forbid live aliasing** (the original `08` per-callout defaults). Rejected: paternalistic and breaks real use — direct register publishing, lock-free SMP/ISR data paths, and CRC-less transparent conduits are legitimate. The protocol already treats application *data* as opaque ([ADR-0010](0010-closed-protocol-error-boundary.md)); memory semantics are likewise the user's to own.
- **A single fixed backend contract.** Rejected: heap, MMIO, DMA, lwIP-pbuf, cross-process SHM, and a raw register have irreconcilable allocation / coherency / threading models; one contract forces the lowest common denominator. `mem_backend_t` is deliberately a small, capability-declaring seam.

## Consequences

- The seven `08` "hard integration" open questions collapse to one rule: each is a **backend-module contract** with a recommended-safe default and a documented hazard, not a protocol mandate. (DMA cache hooks, lwIP `tcpip_callback` free-affinity, cross-process refcount + grace/epoch, MMIO TOCTOU, rope-flatten, register-binding are all instances.)
- A backend SHOULD declare its contract (a capability/traits seam on `mem_backend_t`: allocation, cache-hooks, ISR-safety, atomicity granularity, ordering, cross-process) so L1 / transports / the router can reason about it. Exact bits are an ABI detail, not frozen here.
- Live/raw bindings carry **no CRC by construction** (CRC over volatile bytes is meaningless); a binding that wants CRC snapshots at compute time.
- Descriptive / no wire change. Extends [ADR-0010](0010-closed-protocol-error-boundary.md)'s "the protocol is a transparent carrier of opaque user data" from the data plane to the **memory-binding** plane.
