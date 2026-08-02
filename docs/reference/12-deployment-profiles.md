# Reference 12 — Deployment profiles

The module catalog ([10-module-catalog.md](10-module-catalog.md)) is a menu. This page assembles it into concrete deployments, simplest first. Each rung names its module tree, its conformance [profile](00-overview.md#conformance-profiles), and its framing mode ([ADR-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0022-transport-framing-modes-elided-full-tlv-advertise.md)).

A rung is a deployment *shape*, not a stage of work. The rungs are ordered by how many optional modules they light up, and each is a valid endpoint: nothing on rung 1 needs rung 2 to be useful.

The invariant across all of them: a vertex is the *same* model at every scale. A 9-byte control input and a gigabyte camera tensor are the same graph with different optional fields and different backends ([ADR-0021](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0021-colon-field-plane-is-the-vertex-ioctl.md)).

---

## Rung 0 — In-process (P0)

One process, no wire. Unit tests and the API substrate other layers compose against.

```
graph_runtime + frame_codec + tlv_registry        (required)
mem_pool_static | mem_heap   (L0)   →   view_basic (L1)
```

`read` / `write` / `await` and `:subscribers[]` fan-out, all in-process, zero-copy through refcounted views. No transport, no forwarder.

## Rung 1 — Single-transport leaf (P1)

One MCU, one bus: an RC car over UART, a sensor over CAN.

```
Rung 0  +  one transport  +  its paired L0/L1 module
  transport_uart  → mem_uart_rx_dma → view_uart_dma
  transport_can   → can_reassembly  → view_can_frames   (framing: header-elided)
```

The leaf publishes a few paths and a host subscribes. Framing is **header-elided** — the CAN ID *is* the path — which is what keeps the rung inside its ≤ 25 KB Cortex-M0 footprint budget; the budget is a design target for the composition, not a measured figure.

## Rung 2 — Bus-to-web forwarder (P2)

Two transports on one node: field devices on a serial bus, a browser on WebSocket, one graph between them.

```
Rung 0  +  forwarder (P2)
  transport_can  (header-elided)   ── field devices on the bus
  transport_ws   (full-TLV)        ── the browser UI
  mem_borrowed (live IO values) · can_reassembly · mem_pool_class
  dispatcher: per-vertex delivery policy — value-agnostic delivery_mode,
              UNCONDITIONAL / IF_NEWER (default) / EXPLICIT
  schema_registry: :schema POINT (dtype / unit / range)
```

- **Zero added overhead.** The CAN and WS frames are byte-unchanged in both directions, and the value bytes are *borrowed* rather than copied into the graph. Fan-out to N subscribers is therefore N refcount bumps, not N serialize-and-copy passes — the forwarder adds addressing, not bytes.
- **The forwarder stays uniform.** It joins CAN ↔ WS ↔ in-process without holding per-link state; the framing difference lives entirely in the two transport adapters, so the router sees one addressing scheme regardless of which bus a frame arrived on ([ADR-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0022-transport-framing-modes-elided-full-tlv-advertise.md)).
- **Delivery policy is structural, never value-based.** Whether a subtree sweep selects a vertex is decided by that vertex's `delivery_mode`, which never inspects the value bytes ([RFC-0008](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0008-vertex-operations-assign-propagate.md)).

## Rung 3 — RTSP source (P2+)

A camera as a **lazy / on-demand** vertex ([CONTEXT.md](../../CONTEXT.md)): subscribing to `/cam/0` starts the RTSP pull, and the last unsubscribe stops it.

```
Rung 2  +  an RTSP source vertex (handler role; gates production on :subscribers[] count)
        +  transport_ws / RTP egress
        +  mem_dma_buffer (frame buffers) → view_dma_descriptor
        +  advertise+id-match rope groups (a frame split into slices)
```

Frames are advertised rope groups; each NAL unit or slice is a lean id-matched frame. The `rtsp` source has no entry in the module catalog — it is named here as the shape a lazy source takes, not as a catalogued module.

## Rung 4 — ROS 2 node (P3)

A ROS 2 node speaks libtracer transparently through the **`rmw_tracer`** RMW plugin ([ADR-0023](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0023-ros2-binding-via-rmw-tracer.md)). Like `rtsp`, `rmw_tracer` is a binding rather than a catalogued module.

```
Rung 2/3  +  bindings/ros2/rmw_tracer  (RMW_IMPLEMENTATION=rmw_tracer)
  ROS topic ↔ path · ROS QoS ↔ SUBSCRIBER.delivery_policy · CDR msg = opaque VALUE
  loaned-message API → zero-copy take
  + discovery_mdns (topic/graph discovery) + executor_c (vertex compute)
```

What this composition reaches that a DDS or Zenoh RMW does not: **ROS 2 over CAN or UART in header-elided framing**, which puts a ROS node on a 16 KB MCU.

## Rung 5 — Sensor bus to GPU (P3)

A high-rate sensor on a field bus feeding tensor cores on a host.

```
[MCU]    transport_can (elided, advertise+id-match)  ── 100 ksps, 9-byte samples
   │  CAN-FD bus
[Host]   transport_can RX → host frame/CRC → forwarder (uniform TLV)
   │
   │  batch N samples host-side  (one advertised rope group = one tensor)
   ▼
   mem_cuda  (pinned staging → cudaMemcpyAsync H2D, CUDA streams)
   ▼
   tensor cores  (the GPU reads the device link of a host+GPU heterogeneous rope)
```

- **The path is CPU-mediated** ([ADR-0016](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) branch A): the host frames and CRCs, because a CAN controller cannot GPUDirect. Lowest latency comes from **batching, pinned staging and async streams**, not from per-sample zero-copy. A sample is a slice, a batch is the advertised rope group ([ADR-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0022-transport-framing-modes-elided-full-tlv-advertise.md)), and the tensor is a `mem_cuda` segment ([ADR-0024](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md)).
- **True one-sided GPUDirect** — zero host copy — belongs to a different topology: `transport_rdma` + `mem_cuda` ([ADR-0016](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) branch B), for a sensor reached over RDMA rather than over CAN.

---

## The spectrum at a glance

| Rung | Profile | New modules over the rung below | Framing |
| --- | --- | --- | --- |
| 0 in-process | P0 | required only | — |
| 1 single-transport leaf | P1 | one transport + paired L0/L1 | elided |
| 2 bus-to-web forwarder | P2 | `transport_can`, `transport_ws`, `fwd_router`, dispatcher QoS, `:schema` | elided + full-TLV |
| 3 RTSP source | P2+ | `rtsp` source, `mem_dma_buffer`, rope groups | + advertise |
| 4 ROS 2 node | P3 | `rmw_tracer`, `discovery_mdns`, `executor_c` | + discovery |
| 5 sensor bus to GPU | P3 | `mem_cuda` (+ `transport_rdma` for GPUDirect) | elided + advertise |

The profile column is the conformance profile of [00-overview.md](00-overview.md#conformance-profiles): profiles are strict supersets, so a rung inherits every module obligation of the rungs below it.

## Pitfalls

- **Reading the rungs as a sequence to climb.** Each rung is a shipping shape. A P1 leaf that will only ever own one bus does not become more conformant by loading a second transport; it becomes a P2 forwarder, with the extra footprint that implies.
- **Assuming the forwarder normalises framing.** It does not. Header-elided and full-TLV frames coexist on one node; the adapters translate at the transport edge, and a router that inspects framing mode to decide routing has put per-link state where the design says there is none.
- **Treating a `delivery_mode` of `IF_NEWER` as value comparison.** It selects vertices that are structurally pending, not vertices whose bytes changed. An implementation that diffs value bytes to decide delivery will drop republications of an unchanged value that the sender intended to be delivered.
- **Expecting GPUDirect on rung 5.** The CAN topology stages through host memory by construction. An implementation that promises zero host copies over a CAN link is describing the RDMA topology, not this one.
