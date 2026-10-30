# On an MCU

This page is for a reader putting a libtracer node on a microcontroller: a Cortex-M0, an
ESP32, anything where flash and static RAM bind. It says what to build, what to leave out,
and what the node costs. The cost table is generated from the size gates' own measurements
on every docs build, so it shows what the code measured, not what someone typed.

## What to build

**Start from a packaged integration.** The
[ESP-IDF component](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/esp-idf)
and the
[PlatformIO library](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/platformio)
carry the source list and the MCU defaults. Building the core directly with CMake is
[Getting started §2](getting-started.md#2-use-it-as-a-dependency).

**Bind a config fragment.** Sizes and policies are compile-time members of
`tr::graph::config_t`, set by a `libtracer/config_override.hpp` on the include path. The
size gate's own node binds four of them, and they are the place to start:

| Member | The gate binds | What it decides |
| --- | --- | --- |
| `guard_t` | `tr::no_guard_t` | no locking: the node runs on one thread |
| `kSlabPool` | `false` | the default memory is one static arena, never a heap; an exhausted arena refuses, it does not grow |
| `kArenaBytes` | `2048` | the size of that arena |
| `kCrc32cSlices` | `1` | one CRC table instead of eight: 7,168 B less flash, a slower fold |

The fragment is
[`core/tests/footprint/config`](https://github.com/avatarsd-llc/libtracer/tree/main/core/tests/footprint/config).
Every other member, and which ones decide static RAM, is in
[the configuration space](design/config/00-configuration-space.md).

## What to leave out

The default build is the full node, and modularity is opt-out. On an MCU, turn off what the
node does not use:

| CMake option | Default | On an MCU |
| --- | --- | --- |
| `LIBTRACER_NET_PLANE` | `ON` | off for a node that only talks in-process |
| `LIBTRACER_TRANSPORT_TCP`, `_UDP`, `_WS` | `ON` | keep only the transport the node uses |
| `LIBTRACER_TRANSPORT_CAN` | `OFF` | on for a CAN node; it needs `kBusLinks = true` in the fragment |
| `LIBTRACER_SELF_HEAL_LINKS` | `OFF` | leave off unless the node needs the link-liveness engine; it also needs `kSelfHealLinks = true` |
| `LIBTRACER_WITH_QUIC` | `OFF` | leave off: QUIC needs msquic, a host library |

Each module and what it pulls in is listed in
[Reference 10, the module catalog](reference/10-module-catalog.md).

## What it costs

```{footprint-table}
```

Each row is the newest successful nightly run of a size gate on `main`:

- **Cortex-M0** is the required-modules node, `-Os`, sections garbage-collected, with the
  fragment above. It is the one gated footprint: the build fails past 16 KiB of flash.
- **RV32** is the same node built for the ESP32-C6's RISC-V core. It is reported, not
  gated, and its toolchain differs, so compare it only with itself.
- **ESP32-C6, ESP-IDF** is the libtracer archive inside the `full_node` example app, with
  the net plane and its transports. It is reported, not gated.

A row that reads "not measured in this build" had no artifact to read, as in a local build.
The figures come from `tools/cortexm0_footprint.py`, `tools/rv32_footprint.py` and
`tools/esp_size_gate.py`.

```{admonition} Open question: flash per feature
:class: warning

These rows measure whole nodes. A row per feature (a transport, the net plane, batching)
needs per-feature builds in the size gates; see
[#2401](https://github.com/avatarsd-llc/libtracer/issues/2401) (per-transport flash rows on MID and
NARROW). Until then, the concept pages say "not measured" in their flash cells.
```

## Where next

- [Reference 12, deployment profiles](reference/12-deployment-profiles.md): rung 1, the
  single-transport leaf, is the MCU shape.
- [CAN links](concepts/transports/can.md) and [datagram links](concepts/transports/datagram.md):
  the two transports an MCU node most often uses.
- [Batching](concepts/batching.md): many samples in one frame, which saves per-frame cost on a
  slow bus.
- [The ESP32 node profile](interop/esp32-production-node.md): running libtracer as the
  primary stack inside an MCU's RAM, flash and task budget.
