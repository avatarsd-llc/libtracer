# Research — nnstreamer synergy: edge-connection backend, GStreamer bridge, dmabuf interop

> **Status:** research / design note (not published — `docs/conf.py`'s `include_patterns`
> allowlist does not cover `docs/research/**`, so this file lives in the repository for
> contributors, like the two notes beside it). Answers
> [#1384](https://github.com/avatarsd-llc/libtracer/issues/1384): of the three pinned
> nnstreamer seams, which are real, what does each need that libtracer does not have, and
> which one goes first? **Assessment only** — no integration code lands off this note until
> it survives a grill.

```{admonition} Freshness — what was read upstream, and when
:class: note
Every upstream claim below was read from the **current upstream sources on 2026-08-19**, not
from recall, and each is cited to the file it came from. The two upstream trees are separate
projects with separate licences and separate release cadences:

| Upstream | Licence | Latest tag | Tree state read |
| --- | --- | --- | --- |
| [`nnstreamer/nnstreamer-edge`](https://github.com/nnstreamer/nnstreamer-edge) | **Apache-2.0** | `v0.2.5` | `main` @ `010be163`, last commit **2026-03-11** (its `CMakeLists.txt` already declares `VERSION 0.2.8` — the tree is ahead of the last tag) |
| [`nnstreamer/nnstreamer`](https://github.com/nnstreamer/nnstreamer) | **LGPL-2.1** | `v2.6.0` (released 2025-11-28) | `main`, last pushed **2026-05-19** |

Where a claim could not be verified from source it is marked **[unverified]** rather than
asserted. One brief-level premise turned out to be wrong on the facts and is corrected in
seam 2.
```

## TL;DR — the three seams

| # | Seam | Verdict | Effort | Needs from libtracer that does not exist | Target band it serves |
| --- | --- | --- | --- | --- | --- |
| 1 | libtracer as an **nnstreamer-edge connection backend** | **pursue — first** | **MID** | *nothing in core* — an out-of-tree `.so` over an existing vtable | MID / WIDE |
| 2 | **`tensor_src` / `tensor_sink`** GStreamer elements bridging a pipeline to a graph vertex | **do not build — it collapses into seam 1** | — (avoided) | — | MID / WIDE |
| 3 | **dmabuf `mem` backend** as the zero-copy NPU interop point | **real, but decoupled from 1 and 2** | **MID→WIDE**, blocked on [#1381](https://github.com/avatarsd-llc/libtracer/issues/1381) | a backend-registration seam (`backend_tag` is closed in core today) | WIDE, and MID SoCs with an NPU |

The load-bearing finding is the middle row: **seam 2 does not need a new GStreamer element**,
because seam 1's plugin is already reachable from a stock nnstreamer pipeline. That collapse
removes the only part of the programme that would have meant authoring LGPL-2.1-derived code.

The second load-bearing finding is that **seam 3 buys nothing across seams 1 and 2** — both
nnstreamer data structures on that path are host-pointer-only, and neither upstream tree
contains a single dmabuf reference. Seam 3 is worth doing; it is not worth doing *for*
nnstreamer.

---

## Seam 1 — libtracer as an nnstreamer-edge connection backend

### What the upstream surface actually is

nnstreamer-edge picks its transport from a five-value enum in
[`include/nnstreamer-edge.h`](https://github.com/nnstreamer/nnstreamer-edge/blob/main/include/nnstreamer-edge.h):

```c
typedef enum {
  NNS_EDGE_CONNECT_TYPE_TCP = 0,
  NNS_EDGE_CONNECT_TYPE_MQTT,
  NNS_EDGE_CONNECT_TYPE_HYBRID,
  NNS_EDGE_CONNECT_TYPE_CUSTOM,
  NNS_EDGE_CONNECT_TYPE_UNKNOWN
} nns_edge_connect_type_e;
```

`CUSTOM` is the seam, and it is a real, documented plugin ABI rather than a fork point.
[`include/nnstreamer-edge-custom.h`](https://github.com/nnstreamer/nnstreamer-edge/blob/main/include/nnstreamer-edge-custom.h)
defines a **15-function-pointer struct**, `nns_edge_custom_s`:

`get_description` · `create` · `close` · `start` · `stop` · `connect` · `disconnect` ·
`subscribe` · `is_connected` · `start_discovery` · `stop_discovery` · `set_event_cb` ·
`send_data` · `set_info` · `get_info`

A plugin is a shared object exporting one symbol,
`const nns_edge_custom_s *nns_edge_custom_get_instance (void)`. Upstream loads it with plain
`dlopen(lib_path, RTLD_LAZY)` + `dlsym` in
[`src/libnnstreamer-edge/nnstreamer-edge-custom-impl.c`](https://github.com/nnstreamer/nnstreamer-edge/blob/main/src/libnnstreamer-edge/nnstreamer-edge-custom-impl.c),
and `ENABLE_CUSTOM_CONNECTION` is **ON by default** in its `CMakeLists.txt`. A ~250-line
worked reference plugin ships in the tree as
[`tests/nnstreamer-edge-custom-test.c`](https://github.com/nnstreamer/nnstreamer-edge/blob/main/tests/nnstreamer-edge-custom-test.c).

Direction of travel across that vtable:

- **Egress** is a call *into* the plugin: `send_data(priv, nns_edge_data_h)`.
- **Ingress** is a call *out of* it. `nnstreamer-edge-internal.c` hands the application's
  callback **straight through** to `nns_edge_custom_set_event_callback` — it is not wrapped —
  so the plugin raises received data itself via `nns_edge_event_invoke_callback(..., NNS_EDGE_EVENT_NEW_DATA_RECEIVED, ...)`.
  That is exactly the shape the reference plugin uses for `NNS_EDGE_EVENT_DEVICE_FOUND`.

### Licence

nnstreamer-edge is **Apache-2.0**, the same licence this repository ships under
(`SPDX-License-Identifier: Apache-2.0` on every source file here). A custom-connection plugin
links against nnstreamer-edge headers only. Nothing on this seam touches the **LGPL-2.1**
nnstreamer tree, so no copyleft obligation is created and no dual-licence question arises.
This is the cleanest licence position of the three seams and it is a reason to prefer it.

### What libtracer already has

Every piece a minimal backend needs is in the tree today:

- **A transport seam with the right two verbs.** `tr::net::transport_t` is
  `send(std::span<const std::byte>)` plus `set_receiver` / `set_rope_receiver`
  (`core/include/libtracer/transport.hpp`), with the scatter-gather overload
  `send(std::span<const std::span<const std::byte>>)` beside it — a plugin that gathers a
  tensor's slices need not flatten them.
- **Runtime kind registration.** `transport_vertex_t::register_transport_type(std::string kind, transport_factory_t)`
  (`core/include/libtracer/transport_vertex.hpp:311`) is the documented out-of-tree hook; an
  unregistered kind fails creation with `SCHEMA_NOT_FOUND`.
- **In-band wiring as data.** A connection is created by writing a SPEC built with
  `tr::net::conn_spec_t` (`core/include/libtracer/conn_spec.hpp`) to `/net:children[]`; its
  `kind` NAME selects the factory, and `text` / `u32` / `flag` are the kind-private escape
  hatch. So "which libtracer link does this nnstreamer node ride" is configuration, not a
  recompile.
- **Built-in kinds to ride on day one.** `transport_tcp.hpp`, `transport_quic.hpp`,
  `transport_can.hpp`, `builtin_transports.hpp`.

Nothing in `core/` moves for this seam. That is the whole argument for doing it first.

### What it would prove

That a stock nnstreamer pipeline can carry tensors over **CAN, WebSocket or QUIC** — links
nnstreamer-edge does not have (it ships TCP and MQTT) — and that it can reach a node class
nnstreamer cannot build for at all. It also puts libtracer's latency claim in front of an
edge-AI audience on their own harness rather than ours.

### Cost on the target spectrum

- **NARROW (MCU-class, ESP32-C6 and below): untouched.** Not a cost — an *exclusion*. The
  custom-connection loader is `dlopen`/`dlsym` against a POSIX shared object; ESP-IDF has no
  `dlopen`. nnstreamer-edge is a pthread-and-dlopen C library, so an MCU cannot host the
  plugin **even though it can host the libtracer peer on the other end of the link.** The
  asymmetry is the interesting part: NARROW nodes participate as *peers*, never as plugin
  hosts.
- **MID / WIDE: this is where it lands.** A Linux edge box or a host. Cost is one out-of-tree
  `.so` plus whatever libtracer link it opens; the standing four measurements (throughput,
  latency, RAM, in-binary delta) are all **zero against core**, because core does not change —
  a claim that must be demonstrated by object-file comparison, in the
  [#1377](https://github.com/avatarsd-llc/libtracer/issues/1377)/[#1379](https://github.com/avatarsd-llc/libtracer/issues/1379)
  evidence discipline, not asserted.

**Effort class: MID.** The vtable is 15 functions of which the reference plugin leaves several
as `NNS_EDGE_ERROR_NOT_SUPPORTED`; the substantive work is lifecycle mapping and the
ingress-callback plumbing, not new protocol.

---

## Seam 2 — the `tensor_src` / `tensor_sink` bridge

### The brief's premise is wrong on the element names, and the correction is good news

Two facts from the current nnstreamer tree:

1. **There is no generic `tensor_src` element.**
   [`gst/nnstreamer/elements/meson.build`](https://github.com/nnstreamer/nnstreamer/blob/main/gst/nnstreamer/elements/meson.build)
   registers `tensor_aggregator`, `tensor_converter`, `tensor_crop`, `tensor_debug`,
   `tensor_decoder`, `tensor_demux`, `tensor_if`, `tensor_merge`, `tensor_mux`, `tensor_rate`,
   `tensor_repo`, `tensor_reposink`, `tensor_reposrc`, `tensor_sink`, `tensor_sparsedec`,
   `tensor_sparseenc`, `tensor_split`, `tensor_transform`, `tensor_trainer` — plus
   `gsttensor_srciio.c` (`tensor_src_iio`) behind a `tensor_src_iio_build` option.
   `gsttensor_src.md` is a *design document* for the "tensor source" category, not an element.
2. **`tensor_sink` does exist** and is signal-based: `gsttensor_sink.c` registers `new-data`,
   `stream-start` and `eos` via `g_signal_new`, with a `signal-rate` property.

So the seam as briefed — "write a `tensor_src` and a `tensor_sink`" — would mean authoring two
new GStreamer elements inside, or derived from, an LGPL-2.1 tree.

**It is not necessary.** nnstreamer already ships the bridge, in `gst/edge/`: the `edgesrc` and
`edgesink` elements. Both expose the same property pair
([`gst/edge/edge_src.c`](https://github.com/nnstreamer/nnstreamer/blob/main/gst/edge/edge_src.c),
[`gst/edge/edge_sink.c`](https://github.com/nnstreamer/nnstreamer/blob/main/gst/edge/edge_sink.c)):

```
connect-type=CUSTOM   custom-lib=/path/to/libtracer_nns_edge.so
```

which is precisely the loader path of seam 1. **Seam 2 therefore collapses into seam 1**: ship
the Apache-2.0 plugin, and a stock `gst-launch-1.0` pipeline bridges to a libtracer graph with
no new element, no new GObject class, and no LGPL-2.1-derived code.

### Lifecycle and backpressure — the two questions the brief asked

Those remain real, and they are the plugin's design problem rather than an element's:

- **Element lifecycle vs graph lifecycle.** GStreamer state changes map onto the vtable
  (`create`/`start`/`connect`/`subscribe` … `disconnect`/`stop`/`close`). libtracer's
  connection lifecycle is in-band — a `:children[]` SPEC write creates the connection and the
  RFC-0014 §4 `backoff` key drives self-heal. The two do not disagree, but they do not agree
  either: **who owns retry** when GStreamer says PLAYING and the libtracer link is down is an
  open question, not a settled mapping.
- **Backpressure vs STREAM rings.** A libtracer vertex's `role_t::STREAM`
  (`core/include/libtracer/vertex.hpp:206`) is a bounded history ring whose depth is declared
  **owner-side** by `graph_t::set_history_depth` (`core/include/libtracer/graph.hpp:1165`,
  RFC-0022 §3.C). GStreamer's queueing is per-pipeline and negotiated. The honest reading is
  that a bounded ring is a *drop* policy and a GStreamer `queue` is a *block* policy, so the
  mapping is lossy by construction and the plugin has to pick which end absorbs the mismatch.
  **[unverified]** — nothing was measured here; this is a design reading of two documented
  behaviours, not a result.

**Effort class: none, as a separate build.** The verdict is *do not build it*; the work it
would have carried is already counted inside seam 1.

---

## Seam 3 — a dmabuf `mem` backend

### The interop premise does not survive contact with the source

The brief's case for dmabuf is zero-copy tensor memory across the nnstreamer boundary. On the
current trees, that boundary is host-pointer-only at **both** ends:

- `nns_edge_data_add (nns_edge_data_h, void *data, nns_size_t data_len, nns_edge_data_destroy_cb)`
  — a `void*` and a length
  ([`include/nnstreamer-edge-data.h`](https://github.com/nnstreamer/nnstreamer-edge/blob/main/include/nnstreamer-edge-data.h)).
  There is no fd-carrying variant.
- `GstTensorMemory` is `{ void *data; size_t size; }`
  ([`gst/nnstreamer/include/tensor_typedef.h`](https://github.com/nnstreamer/nnstreamer/blob/main/gst/nnstreamer/include/tensor_typedef.h)).

And the corroborating negative result: a GitHub code search over both repositories returns
**zero** hits for `dmabuf` and zero for `GstDmaBufAllocator`. The two hits for `dma` in
`nnstreamer/nnstreamer` are prose in `README.md` and
`Documentation/getting-started-android.md`. Neither project handles a dmabuf fd anywhere.

This is not the same as saying dmabuf and GStreamer are unrelated. GStreamer itself has
`GstDmaBufAllocator` in gst-plugins-base
([`gst-libs/gst/allocators/gstdmabuf.c`](https://github.com/GStreamer/gstreamer/blob/main/subprojects/gst-plugins-base/gst-libs/gst/allocators/gstdmabuf.c)),
and its own comment records the decisive detail: *"The memory is only mmapped on
`gst_buffer_map()` request."* So a dmabuf-backed `GstBuffer` reaching nnstreamer **works** —
by being CPU-mapped. The fd is consumed by the allocator, never forwarded, and by the time
nnstreamer sees the tensor it is a `void*` again.

**Conclusion: a dmabuf backend buys nothing across seams 1 and 2.** Its value is for a
libtracer node **co-resident with the NPU** — publishing device-visible frames into the graph —
which is the [reference/12](../reference/12-deployment-profiles.md) Rung 3 (`mem_dma_buffer`
frame buffers) and Rung 5 (sensor bus to accelerator) topology, and it is worth building on
those merits. It should not be justified by nnstreamer.

### Does the segment model map onto a dmabuf fd?

Mostly yes, with one structural wrinkle.

**What maps cleanly — the cache hooks.** `mem_backend_t::before_io` / `after_io`
(`core/include/libtracer/backend.hpp:145,153`) take an `io_dir_t` of `DEVICE_TO_CPU` or
`CPU_TO_DEVICE`; the method carries *timing*, the enum carries *direction*, and the backend
maps the pair to clean/invalidate (`CONTEXT.md` §`io_dir_t`). The kernel's dmabuf uapi is the
same shape: `DMA_BUF_IOCTL_SYNC` with `DMA_BUF_SYNC_START` before a CPU access and
`DMA_BUF_SYNC_END` after, each flagged read/write (`linux/dma-buf.h`). That is a **1:1 fit** —
`before_io` issues `SYNC_START`, `after_io` issues `SYNC_END`, and the direction flag comes
straight off `io_dir_t`. Refcounting also maps: `segment_t`'s intrusive refcount already
governs when `destroy` runs (`core/include/libtracer/segment.hpp`), and `destroy` is the
natural place to `munmap` + `close(fd)`.

**What does not map — where the fd lives.** `segment_t::bytes` is a
`std::span<std::byte>` (`core/include/libtracer/segment.hpp:81`) and there is no spare field.
Unlike `mem_cuda`, whose device handle *is* pointer-shaped and so rides in the span, a dmabuf
fd is an `int` with no home in the segment. The workable answer is that the segment carries the
**mmap'd address** and the backend keeps the fd in its own side table keyed by that segment —
which means a dmabuf segment is `mem_space_t::HOST` (it is CPU-addressable once mapped), not
`DEVICE`. That is a design choice worth grilling, not a settled one: it makes the segment
honest about dereferenceability at the cost of making "this is device memory" invisible above
L0.

**What blocks it today.** `backend_tag` (`core/include/libtracer/backend.hpp:81`) is a
build-time-closed enum **inside core**, and `core/src/backend_set.cpp` routes device transfers
through a `switch` with `case backend_tag::CUDA:` under `#ifdef LIBTRACER_WITH_CUDA`. Adding a
`DMABUF` tag today means editing core — exactly the coupling
[#1381](https://github.com/avatarsd-llc/libtracer/issues/1381) exists to remove by moving vendor
backends into a `backends/` tier behind a registration seam. **#1381 is the precondition**;
building the dmabuf backend before it would add the second `#ifdef` that #1381 was filed to
prevent.

Also worth recording: `mem_dma_buffer` is already a **documented-but-unimplemented** catalog
entry (`docs/reference/10-module-catalog.md`, "A peripheral DMA buffer (preallocated, recycled,
with cache hooks)", module set v1; specified in `docs/reference/09-memory-substrate.md`
§`mem_dma_buffer`). A Linux dmabuf backend is a *sibling* of that entry, not the same module —
same hooks, different acquisition path (an imported fd rather than a `heap_caps_malloc(MALLOC_CAP_DMA)`
pool).

### Cost on the target spectrum

- **NARROW: no.** `dma-buf` is a Linux kernel subsystem; the MCU story here is the existing
  `mem_dma_buffer` shape, which is a different module.
- **MID (Linux SoC with an NPU/VPU): this is the real customer.** The cache hooks are not
  optional there — a non-coherent SoC that skips the sync bracket reads stale lines
  (`docs/reference/10-module-catalog.md` §cache coherency states this rule for
  `mem_dma_buffer` already).
- **WIDE: yes, alongside `mem_cuda`.** The heterogeneous host+device rope of
  [ADR-0024](../adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md) is the same construction.

**Effort class: MID→WIDE, gated.** The backend itself is small; the gate is #1381's
registration seam plus a decision on the HOST/DEVICE question above.

---

## How the three fit together

```{mermaid}
flowchart LR
    subgraph gst["GStreamer / nnstreamer (LGPL-2.1) — stock, unmodified"]
        P1["camera or file src"] --> TC["tensor_converter"] --> TF["tensor_filter<br/>(NPU inference)"]
        TF --> ES["edgesink<br/>connect-type=CUSTOM<br/>custom-lib=..."]
        ER["edgesrc<br/>connect-type=CUSTOM<br/>custom-lib=..."] --> TS["tensor_sink<br/>(new-data signal)"]
    end
    subgraph edge["nnstreamer-edge (Apache-2.0) — stock, unmodified"]
        DL["dlopen + dlsym<br/>nns_edge_custom_get_instance"]
    end
    subgraph plug["SEAM 1 — the thing we would write (Apache-2.0)"]
        VT["nns_edge_custom_s vtable<br/>send_data / set_event_cb"]
    end
    subgraph lt["libtracer"]
        NET["tr::net transport_t<br/>send / send(iov) / rope receiver"]
        GR["tr::graph vertex<br/>role STREAM + history depth"]
        MEM["SEAM 3 — dmabuf mem backend<br/>before_io / after_io<br/>NOT on this path"]
    end
    ES --> DL
    DL --> VT
    VT --> NET
    NET --> GR
    GR --> NET
    NET --> VT
    VT --> DL
    DL --> ER
    MEM -. "host-pointer-only boundary:<br/>no fd crosses here" .-> VT
```

The dotted edge is the finding, not decoration: seam 3 does not sit on the seam-1 path.

---

## Open questions (for the grill)

1. **Who owns reconnect?** GStreamer PLAYING vs libtracer's in-band `backoff` self-heal. Two
   retry loops on one link is a defect; picking the wrong owner is a hang.
2. **Drop vs block.** A `role_t::STREAM` ring drops the oldest; a GStreamer `queue` blocks
   upstream. Which end of the plugin absorbs the mismatch, and is that a per-connection config
   key (the `conn_spec_t` escape hatch) or a plugin-wide policy?
3. **One `nns_edge_data_h` = one vertex, or one graph?** `nns_edge_data_add` can be called
   repeatedly to build a multi-tensor frame. Mapping N tensors onto N vertices under one path
   prefix is the natural libtracer shape and would exercise `send(iov)`; mapping the whole
   `data_h` onto one opaque VALUE is simpler and proves less.
4. **Does a dmabuf segment report `HOST` or `DEVICE`?** §Seam 3 argues `HOST` (it is mapped
   and therefore dereferenceable) while conceding that loses the "device memory" signal above
   L0. This is the one genuinely hard-to-reverse choice in the programme and is **ADR-worthy**
   if seam 3 proceeds.
5. **Where does the plugin live?** `integrations/` today holds `arduino`, `esphome`, `esp-idf`
   and `platformio` — all *device-side*. A host-side dlopen'd `.so` is a different genre. New
   tier, or stretch `integrations/`?

## Recommendation and the first concrete step

Argued on the standing lens — throughput, latency, RAM, and the NARROW/MID/WIDE spectrum:

- **Seam 1 first**, and alone. It is the only one of the three that ships value with a
  **zero-byte change to core**, so its RAM and in-binary deltas against `main` are structurally
  zero (to be shown by object-file comparison, not claimed). It is Apache-2.0 clean. It reaches
  MID and WIDE, and its exclusion of NARROW is an exclusion of the *plugin host*, not of the
  NARROW peer — an MCU still publishes into the same graph the pipeline reads.
- **Seam 2: closed.** The verdict is that it should not be built; `edgesrc`/`edgesink` with
  `connect-type=CUSTOM` already is the bridge.
- **Seam 3: sequence behind [#1381](https://github.com/avatarsd-llc/libtracer/issues/1381)**,
  and re-justify it on Rung 3/Rung 5 rather than on nnstreamer, because the nnstreamer
  justification does not hold.

**First concrete step:** a hello-world custom-connection plugin — `nns_edge_custom_get_instance`
returning a vtable whose `send_data` writes into a libtracer vertex and whose `set_event_cb`
target is fed by a rope receiver — proving that

```
gst-launch-1.0 … ! edgesink connect-type=CUSTOM custom-lib=libtracer_nns_edge.so
        ⟶  libtracer graph  ⟶
gst-launch-1.0 edgesrc connect-type=CUSTOM custom-lib=libtracer_nns_edge.so ! … ! tensor_sink
```

round-trips **one tensor**. Modelled on upstream's own
`tests/nnstreamer-edge-custom-test.c`. Deliberately *not* a benchmark: a round-trip is the
falsifiable claim at this stage, and a latency number drawn before the seam works would be a
number about the harness. The perf comparison against `connect-type=TCP` is the step after,
and it is best-of-rounds with throughput, latency and RSS all reported, per the standing
discipline.

---

Sources (all read 2026-08-19):
[nnstreamer/nnstreamer-edge](https://github.com/nnstreamer/nnstreamer-edge) ·
[`nnstreamer-edge-custom.h`](https://github.com/nnstreamer/nnstreamer-edge/blob/main/include/nnstreamer-edge-custom.h) ·
[`nnstreamer-edge-custom-impl.c`](https://github.com/nnstreamer/nnstreamer-edge/blob/main/src/libnnstreamer-edge/nnstreamer-edge-custom-impl.c) ·
[`tests/nnstreamer-edge-custom-test.c`](https://github.com/nnstreamer/nnstreamer-edge/blob/main/tests/nnstreamer-edge-custom-test.c) ·
[nnstreamer/nnstreamer `gst/edge/`](https://github.com/nnstreamer/nnstreamer/tree/main/gst/edge) ·
[`gst/nnstreamer/elements/meson.build`](https://github.com/nnstreamer/nnstreamer/blob/main/gst/nnstreamer/elements/meson.build) ·
[`tensor_typedef.h`](https://github.com/nnstreamer/nnstreamer/blob/main/gst/nnstreamer/include/tensor_typedef.h) ·
[GStreamer `gstdmabuf.c`](https://github.com/GStreamer/gstreamer/blob/main/subprojects/gst-plugins-base/gst-libs/gst/allocators/gstdmabuf.c) ·
`linux/dma-buf.h` (kernel uapi) ·
in-tree: `core/include/libtracer/{backend,segment,transport,transport_vertex,conn_spec,vertex,graph}.hpp`,
`core/src/backend_set.cpp`, [reference/09](../reference/09-memory-substrate.md),
[reference/10](../reference/10-module-catalog.md), [reference/12](../reference/12-deployment-profiles.md),
[ADR-0024](../adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md),
[ADR-0047](../adr/0047-build-time-closed-module-sets-compile-time-seams.md)
