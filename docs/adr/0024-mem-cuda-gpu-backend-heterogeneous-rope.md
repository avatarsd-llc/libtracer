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
