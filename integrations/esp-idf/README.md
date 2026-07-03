# ESP-IDF integration

Packages the libtracer C++ reference core as an [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) component, so an ESP-IDF project can depend on libtracer through the component manager.

## Use

### As a local component (vendored)

The component is the `libtracer/` subfolder (its folder basename is the ESP-IDF component name, so dependents `REQUIRES libtracer`). Point `EXTRA_COMPONENT_DIRS` straight at it, or symlink/copy it under your project's `components/`:

```cmake
# top-level CMakeLists.txt
set(EXTRA_COMPONENT_DIRS "path/to/libtracer/integrations/esp-idf/libtracer")
```

The component's `CMakeLists.txt` references the core sources in `../../../core/`, so it must sit inside a checkout of the libtracer repo (it is the in-tree component, used in place, not a flattened copy). The bundled [`examples/`](examples/) wire it exactly this way.

### Via the component manager (planned)

Once published to the [ESP Component Registry](https://components.espressif.com/), add to `idf_component.yml`:

```yaml
dependencies:
  avatarsd-llc/libtracer: "^0.0.1"
```

## What the component packages

**The full node** ([#183](https://github.com/avatarsd-llc/libtracer/issues/183)) — everything a strawberry-shaped device needs to run libtracer as its MAIN transport:

| Layer | Sources | Notes |
| --- | --- | --- |
| L2/L3 wire codec | `frame`, `tlv_arena` | arena decode (ADR-0041) included |
| L0/L1 substrate | `mem_heap`, `mem_pool`, `rope` | `pool_t` = the bounded MCU backend |
| L4 graph runtime | `path`, `graph` | read / write / await, `:children[]`, `:subscribers[]` |
| net plane | `op_resolve`, `route_handle`, `fwd_router`, `transport_vertex`, `loopback` | explicit-source-routed FWD (ADR-0040); `child_registry` is header-only |
| socket transports | `transport_udp`, `transport_tcp`, `transport_ws` | lwIP BSD sockets on chips, glibc on the `linux` target |
| CAN transport | `transport_can` + **`twai_link.cpp`** (this component) | framing/advertise/reassembly are portable & host-tested; the TWAI link is the on-chip `can_link_t` |

**Platform TU selection is a build-system concern** (the no-feature-macro ruling — no `#ifdef`s in shared sources): chip targets compile `socketcan_link_stub.cpp` (SocketCAN is Linux-only) plus `twai_link.cpp`; the `linux` host target compiles the real `socketcan_link.cpp` and no TWAI.

### lwIP portability audit (what the socket transports use)

The transports compile against lwIP's BSD-socket layer **unmodified — no shim was needed**: `SO_RCVTIMEO` (the recv-loop stop-poll idiom; on by default in ESP-IDF's lwIP), `sendmsg`/`iovec` gather writes (the rope-to-wire path), `MSG_NOSIGNAL` (defined by lwIP; no SIGPIPE exists there anyway), `getsockname`, `poll`, `TCP_NODELAY`, `inet_pton`. The one MCU-relevant behavior fix landed in core (macro-free): `udp_transport_t` sizes its RX segments to `min(64 KiB, backend->max_segment_size())`, so a `pool_t` with MTU-sized slots receives datagrams instead of dropping them, and the recv thread no longer carries a 64 KiB scratch frame on its (small) pthread stack.

### TWAI `can_link_t` (`include/libtracer_esp/twai_link.hpp`)

`tr::net::twai_link_t` implements the same seam `socketcan_link_t` implements, over the `esp_driver_twai` node API: **classic CAN 2.0 only** (TWAI has no FD — configure `transport_can` as `CLASSIC`), 29-bit extended IDs (the CAN-ID *is* the path, ADR-0022). The driver's RX-done callback runs in ISR context and only copies the frame into a FreeRTOS queue; a dispatch thread pops and feeds `transport_can` — user code never runs in the ISR. The framing/advertise/reassembly layers above the seam are covered by the host test suite (`core/tests/transport_can_test.cpp` over a fake link); **bus-level validation on a real transceiver is an on-silicon checklist item on #183**, not a CI gate.

## Examples

| Example | Target(s) | What it proves |
| --- | --- | --- |
| [`examples/inprocess_mirror/`](examples/inprocess_mirror/) | chips | P0 in-process profile: register / write / read / await on FreeRTOS |
| [`examples/host_smoke/`](examples/host_smoke/) | `linux` | the component as a host_test dependency (no FreeRTOS tasks, no esp_log) |
| [`examples/full_node/`](examples/full_node/) | chips + `linux` | **the strawberry shape**: one-slab recipe, sensor vertex, config-created UDP listener via `/net:children[]` SPEC, remote subscriber fan-out |

### full_node (the #183 readiness example)

The device node wires the **one-slab recipe (ADR-0039/0042) concretely**: one static slab, front region → `pool_t` (RX datagram segments; exhaustion = backpressure, never OOM), back region → `monotonic_buffer_resource` + `synchronized_pool_resource` (the router's terminus arena and label tables). Steady state allocates from the slab, not the global heap. Connections are **config-created**: `write /net:children[] SPEC{listener, kind=udp, port}` constructs and owns the real socket (ADR-0027) — same for `tcp`/`ws` kinds via the built-in catalog.

On the `linux` target CI also **runs** it: an in-process host-peer node dials the device node over **real loopback datagrams** and drives `FWD{READ}` → reply, `:subscribers[]` subscribe → transient-local latch, and device write → remote fan-out observed via `graph.await`. On a chip it parks in the publish loop; set Wi-Fi credentials via `idf.py menuconfig` (*full_node example*) to make the same listener reachable from a LAN host (the on-silicon e2e).

```bash
cd integrations/esp-idf/examples/full_node
idf.py set-target esp32c6
idf.py build      # produces build/full_node.{elf,bin}
```

Or in the official Docker image, no local toolchain needed:

```bash
docker run --rm -v "$PWD:/p" -w /p/integrations/esp-idf/examples/full_node \
  espressif/idf:release-v6.0 bash -c "idf.py set-target esp32c6 build"
```

## Memory budget (measured)

Numbers from the `full_node` build for **esp32c6** (`espressif/idf:release-v6.0`, `-Os` defaults), via `idf.py size` — the full node: graph + fwd_router + transport_vertex + udp/tcp/ws/can transports + TWAI link, plus the example app and IDF's Wi-Fi/lwIP stack:

<!-- FOOTPRINT:BEGIN -->
| Metric | esp32c6 |
| --- | --- |
| Total image size | *(filled from the CI `Report footprint` step)* |
| Static RAM (data+bss) | *(filled from the CI `Report footprint` step)* |
<!-- FOOTPRINT:END -->

Steady-state heap is what you configure: the full_node example runs its RX segments and router tables out of a **24 KiB static slab** (12 KiB pool → 7 × 1536 B datagram slots + 12 KiB pmr arena); each live socket transport additionally owns one recv thread (stack below). The `idf.py size` breakdown is printed by the CI `Report footprint` step of the `full-node` job on every run — read current numbers there rather than trusting a stale table.

## FreeRTOS / threading notes

- **`std::thread` is pthread on FreeRTOS.** Every socket transport owns one recv thread; `transport_can`'s TWAI link owns one dispatch thread. Their stacks come from `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT` (the example sets **12 KiB**; the ESP-IDF default 3 KiB is NOT enough — the ws/tcp serve loops carry ~4 KiB chunk buffers + TLV decode frames). To differentiate per-thread, call `esp_pthread_set_cfg()` before constructing a transport (it applies to threads spawned after it). Priorities follow `CONFIG_PTHREAD_TASK_PRIO_DEFAULT` (5) — below Wi-Fi/lwIP tasks, which is the right default: recv loops are throughput, not latency-critical ISR work.
- **`await` is a condvar wait** (`std::condition_variable` → pthread condvar): it blocks the calling task only, wakes on the producer's write, and honors the timeout via the FreeRTOS tick. Don't call it from timer-service or event-loop callbacks.
- **Single-core (esp32c3) sanity**: the refcount/`<atomic>` paths and the one-recv-thread-per-socket model have no core-affinity assumptions; the esp32c3 build is CI-gated. Runtime soak on c3 silicon is an on-silicon checklist item.
- **ISR discipline**: nothing in libtracer may be called from an ISR. The TWAI link is the pattern: ISR → FreeRTOS queue → dispatch thread → libtracer.

## Requirements

- **ESP-IDF v6.0** (tested in CI; matches strawberry-fw's IDF v6.0-dev / GCC 15 toolchain) — libtracer's core is **C++23** (`std::expected`, `std::span`), which needs GCC 13+ (i.e. IDF ≥ 5.3); CI pins `release-v6.0`. The TWAI link uses the `esp_driver_twai` node API (IDF ≥ 5.5).
- `PRIV_REQUIRES pthread, lwip, esp_driver_twai` (chip targets) — all **private**: libtracer's public headers expose only libstdc++ headers, never `<pthread.h>` or lwIP/driver headers, so nothing propagates to dependents. On the `linux` target only pthread is required (sockets come from glibc).
- **Exceptions / RTTI** stay at the ESP-IDF default (**OFF**): the core's data path is exception-free and RTTI-free (it returns `std::expected`, never throws; no `typeid`/`dynamic_cast`), and the full-node profile (including the examples) links clean under `-fno-exceptions -fno-rtti`.

## Security posture

**Unsafe by default in v0.1** — no `security_*` modules exist yet (TLS/DTLS/PSK/ACL enforcement are post-MVP). The full-node component DOES open sockets (udp/tcp/ws listeners are plaintext, unauthenticated); do not expose them beyond a trusted LAN until the matching `security_*` module lands.

## Status

**Built in CI** ([`.github/workflows/esp-idf.yml`](../../.github/workflows/esp-idf.yml)):

- `inprocess_mirror` (P0 profile) — builds for **esp32c6** + **esp32c3**.
- `full_node` (full-node profile, #183) — builds for **esp32c6** + **esp32c3**, including the TWAI link; the `Report footprint` step prints `idf.py size`.
- `full_node` — builds **and runs** for the **`linux`** target: device node + host peer over real datagrams, read/subscribe/latch/fan-out/await end to end (the runtime proof CI can give without silicon).
- `host_smoke` — builds and runs for the **`linux`** target (the host_test path).

On-silicon runtime validation (Wi-Fi FWD e2e, TWAI on a real bus, conformance vectors on-target, footprint soak) is the maintainer's hardware checklist on [#183](https://github.com/avatarsd-llc/libtracer/issues/183). Report build issues there.
