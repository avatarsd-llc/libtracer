<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: Copyright 2026 Avatar LLC -->

# `rmw_tracer` — a ROS 2 middleware backed by libtracer

A concrete **RMW** (ROS MiddleWare) implementation: select it with
`RMW_IMPLEMENTATION=rmw_tracer` and any ROS 2 node — `rclcpp`, `rclpy`, or
`rclc`/micro-ROS — runs over a libtracer graph **transparently**, with no node
code changes. Architecture and rationale: **[ADR-0023](../../docs/adr/0023-ros2-binding-via-rmw-tracer.md)**.

> **Build status.** This package requires the ROS 2 / ament toolchain (`rmw`,
> `rcutils`, `rosidl_runtime_c` headers) and is **not** built by `core/`'s CMake
> or `ctest`. It is a separate ament package built with `colcon`. The `rmw_*.c`
> translation units below are the implementation work; this directory currently
> ships the **package skeleton + the precise mapping** so the work is unambiguous.

## Concept mapping (libtracer ⇆ ROS 2)

| ROS 2 | libtracer (`core/`) |
| --- | --- |
| topic `/ns/foo` | vertex at path `/ns/foo` |
| message (CDR bytes from `rosidl` type support) | **opaque** `VALUE` payload (libtracer never parses it) |
| message type name | `:schema` POINT (introspection only) |
| `rmw_qos_profile_t` | `:settings` (reliability/durability/history/deadline/liveliness) |
| publisher | a writer handle on the topic path |
| subscription | `subscribe(path)` into a per-subscription history ring |
| service / client | a `…/_request` + `…/_response` path pair |

## RMW entry points → graph operations (the implementation checklist)

- **Init / nodes** — `rmw_init`, `rmw_create_node`/`rmw_destroy_node`: stand up a
  `tr::graph::graph_t` per context; a node is a path-namespace + a `peer_id`.
- **Publish** — `rmw_create_publisher`, `rmw_publish`: `graph.write(path, VALUE)`
  where the payload is the already-CDR-serialized message (no extra copy —
  `rcl` serialized it before calling `rmw`).
- **Subscribe / take** — `rmw_create_subscription`, `rmw_take[_with_info]`:
  `graph.subscribe(path)`; `rmw_take` pops the ring, **copying** into the user
  buffer (DDS-equivalent path).
- **Zero-copy take (MANDATORY — the edge over `rmw_zenoh`)** —
  `rmw_borrow_loaned_message`, `rmw_publish_loaned_message`,
  `rmw_take_loaned_message`, `rmw_return_loaned_message`: a libtracer `view_t`
  **is** a loaned message — hand the borrowed view to the caller, **no copy**.
  This is required, not optional ([ADR-0023 §loaned messages](../../docs/adr/0023-ros2-binding-via-rmw-tracer.md)).
- **Wait set** — `rmw_create_wait_set`, `rmw_wait`, guard conditions: driven by
  the graph's `await` + subscription callbacks signalling readiness.
- **QoS** — translate `rmw_qos_profile_t` ⇄ `:settings`; expose libtracer-only
  knobs (`delivery_mode=ON_CHANGE`) under an `rmw_tracer`-namespaced QoS extension.
- **Graph events** — `rmw_get_node_names`, `rmw_*_graph_guard_condition`: from the
  graph's structural feed (subscribing a parent's `:children[]`).
- **Services / actions** — request/response path pairs; actions compose from
  services + topics, as ROS already does.

## Transports & the differentiators

`rmw_tracer` inherits libtracer's transports, so ROS 2 runs over **CAN / UART with
header elision** ([ADR-0022](../../docs/adr/0022-transport-framing-modes-elided-full-tlv-advertise.md))
— ROS on a 16 KB MCU, which DDS/Zenoh cannot reach — and ROS messages can land in
**GPU memory** via `mem_cuda` ([ADR-0024](../../docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md)).

## Files

- `package.xml`, `CMakeLists.txt` — the ament package (build with `colcon build`).
- `src/rmw_tracer/*.c` — the entry points above (to be written against the ROS 2
  distro's `rmw` headers; one TU per RMW concern, mirroring `rmw_cyclonedds`'s layout).
