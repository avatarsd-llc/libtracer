# Reference 12 — Deployment Profiles: from one vertex to STM32 → CAN → tensor cores

> **Status**: draft, v1, 2026-06. The module-usage spectrum. Reads the [module catalog](10-module-catalog.md) as a *menu* and assembles it into concrete deployments, simplest first. Each rung names its module tree, the conformance [profile](00-overview.md#conformance-profiles), and the framing mode ([ADR-0022](../adr/0022-transport-framing-modes-elided-full-tlv-advertise.md)).

The point of this page: a vertex is the *same* model at every scale; you light up more modules as the system grows. A 9-byte control input and a GB camera tensor are the same graph with different optional fields and backends ([ADR-0021](../adr/0021-colon-field-plane-is-the-vertex-ioctl.md)).

---

## Rung 0 — In-process (P0)

One process, no wire. Unit tests, the API substrate.

```
graph_runtime + frame_codec + tlv_registry        (required)
mem_pool_static | mem_heap   (L0)   →   view_basic (L1)
```

`read`/`write`/`await` + `:subscribers[]` fan-out, all in-process, zero-copy via refcounted views. No transport, no forwarder.

## Rung 1 — Single-transport leaf (P1)

One MCU, one bus. An RC car over UART; a sensor over CAN.

```
Rung 0  +  one transport  +  its paired L0/L1 module
  transport_uart  → mem_uart_rx_dma → view_uart_dma
  transport_can   → can_reassembly → view_can_frames   (framing: header-elided)
```

The leaf publishes a few paths; a host subscribes. Framing is **header-elided** (the CAN ID *is* the path); ≤ 25 KB on a Cortex-M0.

## Rung 2 — First-ready-to-strawberry (P2)

The milestone: **drop-in replacement for the strawberry-fw io_layer over its existing CAN + WebSocket buses, zero overhead.**

```
Rung 0  +  forwarder (P2)
  transport_can  (header-elided)   ── strawberry's remote boards / sensors
  transport_ws   (full-TLV)        ── strawberry's web-ui
  mem_borrowed (live IO values) · can_reassembly · mem_pool_class
  dispatcher: per-vertex delivery policy (value-agnostic delivery_mode ✓: UNCONDITIONAL/IF_NEWER/EXPLICIT — RFC-0008)
  schema_registry: :schema POINT (dtype/unit/range, the io_descriptor)
```

- **Zero overhead** because the CAN/WS frames are byte-unchanged and the value bytes are *borrowed* (no copy); fan-out is a refcount bump — *less* overhead than the io_layer's dispatch deep-copy.
- The **forwarder** joins CAN ↔ WS ↔ in-process; it stays stateless/uniform (the adapters uniform addressing, [ADR-0022](../adr/0022-transport-framing-modes-elided-full-tlv-advertise.md)).
- **Deferred to later rungs:** NFSv4 ACL enforcement, in-band `SPEC` creation, controllers, discovery.

## Rung 3 — RTSP source (P2+)

A camera as a **lazy/on-demand** vertex ([CONTEXT.md](../../CONTEXT.md) §lazy source): subscribing to `/cam/0` starts the RTSP pull; the last unsubscribe stops it.

```
Rung 2  +  an RTSP source vertex (handler role; gates production on :subscribers[] count)
        +  transport_ws / RTP egress
        +  mem_dma_buffer (frame buffers) → view_dma_descriptor
        +  advertise+id-match rope groups (a frame split into slices)
```

Frames are advertised rope groups; each NAL/slice a lean id-matched frame. (`rtsp` is a new module — not yet in the catalog.)

## Rung 4 — ROS 2 node (P3)

Any ROS 2 node speaks libtracer transparently via the **`rmw_tracer`** RMW plugin ([ADR-0023](../adr/0023-ros2-binding-via-rmw-tracer.md)).

```
Rung 2/3  +  bindings/ros2/rmw_tracer  (RMW_IMPLEMENTATION=rmw_tracer)
  ROS topic ↔ path · ROS QoS ↔ :settings · CDR msg = opaque VALUE
  loaned-message API → zero-copy take (the edge over rmw_zenoh)
  + discovery_mdns (topic/graph discovery) + executor_c (vertex compute)
```

The differentiator: **ROS 2 over CAN/UART (header-elided)** — ROS on a 16 KB MCU, where DDS/Zenoh cannot reach.

## Rung 5 — Flagship: STM32 → CAN → tensor cores, 100 ksps, lowest latency (P3)

```
[STM32]  transport_can (elided, advertise+id-match)  ── 100 ksps, 9-byte samples
   │  CAN-FD bus
[Host]   transport_can RX → host frame/CRC → forwarder (uniform TLV)
   │
   │  batch N samples host-side  (one advertised rope group = one tensor)
   ▼
   mem_cuda  (pinned staging → cudaMemcpyAsync H2D, CUDA streams)
   ▼
   tensor cores  (the GPU reads the device link of a host+GPU heterogeneous rope)
```

- **CPU-mediated** ([ADR-0016](../adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) branch A): the host frames/CRCs (CAN can't GPUDirect); lowest latency comes from **batching + pinned staging + async streams**, not per-sample zero-copy. A sample is a slice; a batch is the advertised rope group ([ADR-0022](../adr/0022-transport-framing-modes-elided-full-tlv-advertise.md)); the tensor is a `mem_cuda` segment ([ADR-0024](../adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md)).
- **True one-sided GPUDirect** (zero host copy) is the *RDMA* topology — `transport_rdma` + `mem_cuda` (branch B) — for a sensor over RDMA, not CAN.

---

## The spectrum at a glance

| Rung | Profile | New modules over the rung below | Framing |
| --- | --- | --- | --- |
| 0 in-process | P0 | required only | — |
| 1 leaf | P1 | one transport + paired L0/L1 | elided |
| 2 **strawberry** | P2 | `transport_can`, `transport_ws`, `fwd_router`, dispatcher QoS, `:schema` | elided + full-TLV |
| 3 RTSP | P2+ | `rtsp` source, `mem_dma_buffer`, rope groups | + advertise |
| 4 ROS | P3 | `rmw_tracer`, `discovery_mdns`, `executor_c` | + discovery |
| 5 flagship | P3 | `mem_cuda` (+ `transport_rdma` for GPUDirect) | elided + advertise |

Modules **not yet in the catalog** and owned by this roadmap: `rtsp` (source), `rmw_tracer` (ROS), `mem_cuda` (GPU). Each has an ADR (23/24) or a follow-up.
