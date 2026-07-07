# ROS 2 integration is an RMW implementation (`rmw_tracer`), not an rclcpp bridge — topics map to paths, ROS QoS to `:settings`, messages stay opaque

Status: accepted

ROS 2 is a first-class target. The decision is *where* libtracer plugs into the ROS 2 stack. ROS 2 is layered `rclcpp`/`rclpy` → `rcl` → **`rmw`** (the ROS MiddleWare abstraction) → a concrete middleware (`rmw_fastrtps`, `rmw_cyclonedds`, …). This ADR pins the binding at the **RMW layer** — `rmw_tracer` — and the mapping from ROS 2 concepts onto the libtracer graph. (The repository term *"rwx_tracer"* is read as `rmw_tracer`, the RMW-plugin naming convention.)

## Decision

**`rmw_tracer` is a concrete RMW implementation** exposing the `rmw_*` C API (`rmw/rmw.h`) backed by the libtracer graph. Any ROS 2 node runs over libtracer **transparently** by selecting `RMW_IMPLEMENTATION=rmw_tracer` — no node code changes. It lives in `bindings/ros2/`, built with ament/colcon (toolchain-gated; see Consequences).

**Concept mapping (the load-bearing part):**

| ROS 2 (`rmw`) | libtracer |
| --- | --- |
| topic `/ns/foo` | vertex at path `/ns/foo` (ROS namespace = path tree) |
| `rmw_publish(pub, msg)` | `write(path, VALUE)` — the **CDR-serialized message bytes are the opaque VALUE payload** |
| `rmw_create_subscription` + `rmw_take` | `subscribe(path)` into a per-subscription ring; `rmw_take` pops it |
| `rmw_qos_profile_t` | `:settings` + per-subscriber delivery policy (see below) |
| `rmw_wait` / guard conditions | the graph's `await` + subscription callbacks driving the wait set |
| service / client (req-rep) | a request path + a response path (write-request, await-response) |
| action | a small set of service + topic paths (ROS composes actions from these) |
| message type (`rosidl`) | the ROS type name stored in `:schema` (introspection only — libtracer never parses the payload) |

**QoS maps cleanly because both are DDS-derived:** `RELIABLE/BEST_EFFORT` → `:settings.reliability`; `TRANSIENT_LOCAL/VOLATILE` → `:settings.durability`; `KEEP_LAST(depth)` → `:settings.history_keep_last`; `deadline`/`liveliness` → `:settings.deadline_ns` + liveness; `lifespan` → a per-subscriber `keepalive`/expiry. The ROS DDS QoS profile is essentially the libtracer `:settings` block.

**Messages stay opaque.** libtracer carries CDR bytes as a `VALUE` payload and never interprets them (load-bearing claim 5); `rosidl` type support does (de)serialization on the ROS side. The `:schema` POINT records the type *name* for introspection/tools, not for parsing.

## Preserving the zero-copy edge through RMW (mandatory: loaned messages)

`rmw_zenoh` is a tier-1 ROS 2 RMW (Jazzy+), so `rmw_tracer` competes head-to-head. The `rmw` layer is **thin** — it forwards publish/take to the middleware data plane, so *the middleware, not `rmw`, decides latency and copies*; libtracer's thin per-message path (the ~4–5× latency win vs Zenoh, the zero-copy fan-out) is preserved through it. **The one place `rmw` can force a copy is `rmw_take`, which by default copies into a user buffer.** Therefore `rmw_tracer` **MUST implement the loaned-message API** (`rmw_borrow_loaned_message` / `rmw_return_loaned_message` / `rmw_take_loaned_message` / `rmw_publish_loaned_message`): a libtracer `view_t` *is* a loaned message (a borrowed view into the segment), so take is zero-copy. Omitting it would hand the take-side latency edge back to DDS/Zenoh. This is a non-negotiable requirement of the binding, not an optimization.

`rmw_tracer`'s differentiators over `rmw_zenoh`: (a) **graph-wide zero-copy loaned messages** (Zenoh's zero-copy is SHM / intra-host only); (b) **ROS 2 over CAN/UART with header elision** ([ADR-0022](0022-transport-framing-modes-elided-full-tlv-advertise.md)) — reaching a 16 KB MCU, which Zenoh cannot; (c) **ROS messages into GPU memory** via the `mem_cuda` heterogeneous rope ([ADR-0024](0024-mem-cuda-gpu-backend-heterogeneous-rope.md)).

## Why RMW, and what it buys

- **Transparent** — every existing ROS 2 node works unchanged (vs an `rclcpp` bridge that relays topics and needs per-app wiring, double-publishes, and breaks intra-process zero-copy).
- **Zero-copy fan-out** — libtracer delivers a refcounted view to each subscriber instead of DDS's per-reader serialize+copy; the same copy-elimination win as the strawberry io_layer.
- **ROS over constrained transports** — because framing is a transport-adapter concern ([ADR-0022](0022-transport-framing-modes-elided-full-tlv-advertise.md)), `rmw_tracer` runs ROS 2 over **CAN / UART with header elision** — ROS topics on a microcontroller fleet at a fraction of micro-ROS/DDS-XRCE overhead. This is the differentiator: ROS 2 reaching down to a 16 KB MCU.

## Considered options

- **An `rclcpp`-level topic bridge** (a node subscribing ROS topics and republishing to libtracer paths). Rejected as the primary binding: not transparent (needs per-application integration), double-buffers every message, defeats intra-process zero-copy, and cannot carry ROS QoS faithfully. (It remains viable as a *tool* for mixing two middlewares, not as the integration.)
- **A DDS transport that speaks libtracer underneath `rmw_fastrtps`.** Rejected: keeps the DDS wire format and its copies — none of the zero-copy or constrained-transport wins; we would be tunnelling, not replacing.
- **Pin at `rcl`.** Rejected: `rcl` is not a pluggable seam; `rmw` is the supported, stable middleware boundary.

## Consequences

- `bindings/ros2/rmw_tracer` implements the `rmw` C API against the libtracer C/C++ core; it requires the **ROS 2 / ament toolchain** to build and test — **not present in this repo's CI environment**, so it ships as a complete, buildable-elsewhere package (real `rmw_*` mapping + `package.xml`/`CMakeLists.txt`), gated behind the ROS 2 toolchain, *not* wired into `core/`'s `ctest`.
- ROS 2 gains **zero-copy fan-out** and **reach onto constrained buses** (the micro-ROS alternative); libtracer gains the entire ROS 2 ecosystem of nodes/tools.
- QoS round-trips through `:settings`; any libtracer QoS not in the ROS profile (e.g. `delivery_mode=ON_CHANGE`) is exposed as a `rmw_tracer`-namespaced QoS extension, ignored by stock ROS tools.
- Services/actions are composed from request/response paths; the exact path convention (`/foo/_request`, `/foo/_response`) is an implementation detail of `rmw_tracer`, recorded in `bindings/ros2/README.md`.
- This is a **binding**, not a protocol change: two libtracer nodes (one via `rmw_tracer`, one native) interoperate over the wire because the ROS-ness is confined to the opaque `VALUE` payload and the `:schema` type name.
