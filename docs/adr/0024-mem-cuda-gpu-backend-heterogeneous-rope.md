# `mem_cuda` is a CPU-opaque value-payload backend; framing stays host-side and a TLV becomes a heterogeneous host+GPU rope

Status: accepted

The flagship use case — **100 ksps from an STM32 over CAN, fed directly into tensor cores (GPU memory) at lowest latency** — needs GPU memory to be a libtracer L0 backend so payload bytes can live where the tensor cores read them. CUDA device memory, however, breaks an assumption baked into the codec: **the CPU cannot dereference a GPU device pointer**, yet libtracer's framing, CRC, and decode are CPU operations. This ADR defines `mem_cuda` around that constraint rather than against it.

## Decision

**`mem_cuda` is an L0 `mem_backend_t` whose `segment_t` bytes are CUDA memory** (device via `cudaMalloc`, pinned-host via `cudaMallocHost`, or managed/UVM via `cudaMallocManaged`); `alloc`→allocate, `destroy`→`cudaFree`. The cache hooks carry the transfer: `before_io(CPU_TO_DEVICE)` = the host→device copy / stream prep, `after_io(DEVICE_TO_CPU)` = `cudaStreamSynchronize`.

**A device-memory `mem_cuda` segment is CPU-opaque and may back only a VALUE *payload*, never a framed TLV the CPU must parse.** The `type`/`opt`/`length` header, the trailer, and the CRC live in **host** memory; only the leaf payload bytes live in GPU memory. So a GPU-backed TLV is a **heterogeneous rope** (the two-compositions keystone applied to two memory spaces):

```
TLV = rope { view → host segment (header + trailer/CRC) ,  view → mem_cuda segment (payload, device) }
```

The CPU walks the host links (frames, CRCs the header, validates structure); the **GPU/tensor cores read the device link**. Neither dereferences the other's memory. CRC over the payload, when required, is computed host-side *before* the data leaves for the device (or skipped — a live/raw `mem_cuda` binding imposes no CRC, per ADR-0012).

**The CAN→GPU data path (CPU-mediated, [ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) branch A):** `transport_can` RX frames host-side; the payload is written to a `mem_cuda`-backed vertex; **samples are batched host-side into one tensor and copied `cudaMemcpyAsync` host→device once per batch** (pinned staging + async streams), because a per-sample H2D copy at 100 ksps is latency-bound. The batch *is* an **advertise+id-match rope group** ([ADR-0022](0022-transport-framing-modes-elided-full-tlv-advertise.md)): N CAN slices → one GPU tensor → tensor cores.

**True one-sided GPUDirect (NIC DMAs straight into GPU memory, no host copy) is the `transport_rdma` + `mem_cuda` GPUDirect-RDMA path — [ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) branch B — a *different topology* (a sensor over RDMA), not CAN.** CAN controllers do not GPUDirect; for CAN the host-staged batched copy is the lowest-latency route.

## Considered options

- **`mem_cuda` as a general segment backing the CPU reads/CRCs like any other.** Rejected: device memory is not host-addressable; CRC/decode would fault or force a device→host copy on every frame — defeating the purpose. The CPU-opaque, payload-only rule is forced by the hardware.
- **Copy each CAN sample host→device individually.** Rejected: 100 ksps of tiny H2D copies is dominated by per-copy launch latency; batching into one tensor per inference window is the only way to feed tensor cores at rate.
- **GPUDirect for CAN.** Rejected: GPUDirect RDMA is an RDMA-NIC capability; a CAN peripheral cannot DMA into GPU memory. GPUDirect belongs to the `transport_rdma` topology.
- **Materialize the whole TLV (header+payload) in GPU memory.** Rejected: then the CPU could not frame/CRC/route it; the header must stay where the router runs (host).

## Consequences

- A GPU-backed value is a **heterogeneous rope** (host header + device payload); this extends the rope/two-compositions model cleanly across memory spaces — the same `view_t`/`rope_t` types, one link host, one link device.
- The flagship is **CPU-mediated batching**: CAN→host frame → batch → one async H2D copy → `mem_cuda` tensor → tensor cores. Lowest latency comes from batch sizing, pinned staging, and CUDA streams, not from per-sample zero-copy.
- **GPUDirect zero-copy** is available for the **RDMA** topology (`transport_rdma`+`mem_cuda`, branch B), recorded as a distinct path, not the CAN one.
- `mem_cuda` requires the **CUDA toolkit + a GPU** to build/test — **not present in this environment** — so it ships as a complete, buildable-elsewhere backend behind a `LIBTRACER_WITH_CUDA` CMake option, *not* in `core`'s default `ctest`.
- Rope walking, CRC, and decode must tolerate a link whose bytes are non-host-readable: a `view_t`/`segment_t` carries a memory-space tag so the codec **skips CPU access to device links** (it only ever needs the host header/trailer). This is the one core change the GPU path asks of L1/L2.

## Amendment 1 (2026-08-20): `mem_cuda` moves to the `backends/` tier and registers a transfer hook ([#1381](https://github.com/avatarsd-llc/libtracer/issues/1381))

Nothing above is contradicted. The ADR never said *where* the file lives, and §Consequences'
"ships as a complete, buildable-elsewhere backend … not in `core`'s default `ctest`" is preserved
in substance — strengthened, in fact: it is now not in core's default *build* either. What this
amendment records is the seam, because the original shape had core naming a vendor.

**1. Location.** `mem_cuda.{hpp,cpp}` and its GPU test move from `core/` to **`backends/cuda/`**, a
sibling of `bindings/` and `integrations/` and a standalone CMake project that *consumes* core the
way [`bench/`](../../bench/CMakeLists.txt) does. `LIBTRACER_WITH_CUDA` disappears: the option was a
core option, and with it goes the PUBLIC compile definition that every consumer TU of a
CUDA-enabled build used to carry. Configuring `backends/cuda` and linking `libtracer_cuda` *is*
the option now. `grep -rniE 'cuda|nvidia' core/src core/include` is empty and stays that way.

**2. The `#ifdef` becomes a registration.** `backend_set.cpp` used to route a CUDA-tagged segment
to `cuda_transfer` under `#ifdef LIBTRACER_WITH_CUDA`; every further device backend (ROCm, an NPU,
dmabuf) would have added another vendor name and another `#ifdef` to core. Instead a `DEVICE`-space
backend now registers the pair **`{backend, transfer hook}`** through
`tr::mem::register_device_backend`, and `tr::mem::transfer` routes a `DEVICE` segment to its own
backend's hook. This is deliberately the L0 mirror of `transport_vertex_t::register_transport_type`
— the module seam [ADR-0043 §1](0043-quic-webtransport-optional-module-msquic.md) cites *this* ADR
as its precedent for optional modules; the debt is repaid in the other direction. Two forced
deviations, recorded rather than buried:

- **Namespace scope, not an instance method.** There is no "memory plane" object to hang a
  `register_*` method on — `tr::mem::transfer` is a free function — so the registry is a
  namespace-scope function in `tr::mem` over a bounded table (`config_t::kDeviceBackendSlots`,
  an [ADR-0068](0068-build-configuration-is-plain-cpp-config-header.md) knob). It never
  allocates; overflow is a refusal that registers nothing.
- **Keyed by backend IDENTITY, not by `mem_space_t`.** `DEVICE` is a single enumerator shared by
  every accelerator, so a space-keyed registry would give one global device slot and force the
  second vendor to add an enumerator *to core* — the vendor-name problem again. The segment's
  `backend` pointer is already the identity `destroy` routes on, so it is the key.

**3. `backend_tag::CUDA` is retired.** With the registry keyed by identity the enumerator had no
remaining role, and it was a vendor name in a public core header. Removing it is safe precisely
because [ADR-0047 §2](0047-build-time-closed-module-sets-compile-time-seams.md) says in its own
words that the tag is *a fast path, never a correctness dependency*: `CUDA` was never in
`destroy_dispatch`'s fast switch, so a CUDA segment already took the virtual `destroy` and takes
the identical one now with the default `UNKNOWN` tag. It was the last enumerator, so every other
value is unchanged and no segment layout moves. It is nevertheless a **public API break**, noted in
`core/CHANGELOG.md`.

**4. What core still owes the GPU path.** Everything §Decision and §Consequences describe:
`mem_space_t` on `segment_t`/`view_t`, `flatten()`'s refusal of a heterogeneous rope, the
CPU-opaque payload-only rule. Those are substrate *interfaces* and they stay in core. What left is
only the vendor implementation and the one line that named it.

**5. Cost.** A build with no vendor backend pays nothing structural: with the tier removed from
core, 28 of 29 `libtracer` objects are byte-identical, the 29th (`backend_set.cpp.o`) differs only
in that its already-existing `space == DEVICE` arm tail-jumps to the registry instead of returning
`false` inline (89 → 88 instructions; `destroy_dispatch` instruction-identical), and the
single-backend `LIBTRACER_BACKEND_SET_POOL_ONLY` object — the MCU profile — is **byte-identical**,
because that fold has no device arm and never links the registry TU.
