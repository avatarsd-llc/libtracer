# Research — a libtracer ↔ ROS 2 adapter (interface-first)

> **Status:** research / design note (not part of the published docs). Answers
> "how far are we from a ROS adapter, and how would a libtracer-ROS compare to
> Zenoh-ROS?" — interfaces are **declared and documented here before any
> implementation**, per the project's interface-first rule.

## TL;DR — how far are we?

There are **two integration models**, mirroring exactly how Zenoh integrates with
ROS 2:

| Model | Zenoh's equivalent | Effort for libtracer | Reuses |
| --- | --- | --- | --- |
| **Bridge** — a process mirroring ROS 2 topics ↔ libtracer paths | `zenoh-plugin-ros2dds` (DDS↔Zenoh) | **Medium** — *feasible now-ish* | M3 graph pub/sub · M4 bridge/ROUTER · M5 transport |
| **Native rmw** — `rmw_libtracer`, ROS 2's middleware C API over libtracer | `rmw_zenoh` (official since ROS 2 Kilted) | **Large** | the wire + graph; everything else is new |

**Bottom line:** a **bridge is reachable** (it needs M6 + a ROS 2 build env, not
new core protocol); a **native rmw is a major undertaking** (the full rmw surface
+ rosidl type support). Recommend **bridge first**, rmw later/maybe.

## Why libtracer fits a bridge cleanly

ROS 2 messages are **CDR-serialized bytes**. libtracer treats application data as
**opaque** — the transparent-carrier principle (ADR-0010): a `VALUE` TLV's payload
is bytes libtracer never interprets. So a ROS message is simply a `VALUE` payload;
libtracer needs **no rosidl/type introspection to move it**. Only the bridge's ROS
endpoints (the `rclcpp` generic sub/pub) touch the type. A ROS topic `/scan` maps
1:1 onto a libtracer path `/scan` (a PATH TLV) — addressing lines up.

```{mermaid}
flowchart LR
    subgraph ros["ROS 2 graph (DDS)"]
        N1["node: /lidar (pub /scan)"]
        N2["node: /slam (sub /scan)"]
    end
    subgraph br["libtracer-bridge-ros2"]
        GS["rclcpp generic sub /scan"] --> MAP["topic ↔ path map"]
        MAP --> GW["graph.write(/scan, VALUE=CDR bytes)"]
        GR["graph.subscribe(/scan)"] --> GP["rclcpp generic pub /scan"]
    end
    subgraph lt["libtracer fabric"]
        V["vertex /scan"]
        T["transport (UDP/M5 · QUIC/M6)"]
    end
    N1 -. DDS .-> GS
    GW --> V --> T
    T --> V --> GR
    GP -. DDS .-> N2
```

## Interface declaration (before implementation)

A bridge is a **separate package** (under `integrations/ros2/`, not `core/`) so the
heavy `rclcpp`/`ament` build never touches the core. It reuses the core `Graph` +
`Transport`; the only new surface is the topic↔path mapping and the ROS endpoints.

```cpp
namespace tracer::ros2 {

enum class Direction { RosToLibtracer, LibtracerToRos, Bidirectional };

// One mirrored topic. The ROS type name is declared up front so the bridge can
// create an rclcpp *generic* sub/pub (no compile-time message dependency).
struct TopicMap {
    std::string  ros_topic;   // e.g. "/scan"
    graph::Path  path;        // e.g. Path::parse("/scan")  — resolved once
    std::string  ros_type;    // e.g. "sensor_msgs/msg/LaserScan"
    Direction    dir = Direction::Bidirectional;
    graph::Settings qos;      // ROS QoS ↔ libtracer :settings.* (reliability, …)
};

// Bridges a ROS 2 node's topics onto a libtracer graph and back. The CDR bytes
// pass through opaquely as VALUE payloads — no rosidl on the data path.
class Ros2Bridge {
   public:
    Ros2Bridge(graph::Graph& graph, rclcpp::Node::SharedPtr node);

    // Mirror one topic (idempotent). Resolves the Vertex* handle once.
    graph::Result<void> mirror(const TopicMap& map);

    // Discover-and-mirror by prefix (optional; like zenoh-plugin-ros2dds's
    // namespace bridging). Empty prefix = all.
    graph::Result<void> mirror_all(std::string_view ros_ns_prefix = "");
};

}  // namespace tracer::ros2
```

Design choices (no boilerplate, all reuse):

- **Egress** = `graph.subscribe(path, cb)`; `cb` publishes the View's bytes via an
  `rclcpp::GenericPublisher`. **Ingress** = an `rclcpp::GenericSubscription` whose
  callback does `graph.write(vertex, View)` — *the same shape as the M4 bridge*,
  with rclcpp in place of `Transport`.
- **QoS mapping** is a small table: ROS `reliability`/`durability`/`history` ↔
  libtracer `:settings.reliability`/`durability`/`history_keep_last` (the names
  already line up — `reference/02` §core writable fields).
- **No type support on the data path** — the bridge moves CDR bytes; only the two
  `rclcpp` generic endpoints know the type string.

## libtracer-ROS vs Zenoh-ROS (honest)

| | **Zenoh-ROS** | **libtracer-ROS (proposed)** |
| --- | --- | --- |
| Maturity | **official `rmw_zenoh`** (ROS 2 Kilted+, Jazzy binaries) + production `zenoh-plugin-ros2dds` | experimental / research |
| Model available | native rmw **and** DDS bridge | **bridge first**; native rmw far-off |
| Transport | TCP/UDP/QUIC, **batched** | UDP (M5); TCP/QUIC = M6 |
| Per-msg latency (our bench) | ~65 µs | **~12–15 µs** — potential edge |
| Small-msg throughput | **high** (batching) | needs egress batching (M5.1) |
| Footprint | small | **tiny** (MCU-class) — a micro-ROS-style angle |
| Ecosystem | full ROS 2, Tier-1 binaries | none yet |

**Honest read:** Zenoh-ROS is **years ahead** and *official* — libtracer is not
going to displace `rmw_zenoh`. libtracer's only credible angle is the **embedded /
ultra-low-latency / zero-copy** niche (think micro-ROS-class targets, or a
latency-critical on-robot fabric), and only *after* M6 (reliable QoS) + egress
batching land. A bridge is a legitimate research artifact; a native rmw is not
worth it absent a concrete embedded demand.

## Prerequisites (the gap, concretely)

1. **M6 — reliable stream transport** (TCP/QUIC): ROS's `RELIABLE` QoS needs it;
   UDP alone only serves `BEST_EFFORT`.
2. **Egress batching** (M5.1): to be throughput-competitive for high-rate topics.
3. **A ROS 2 build env** (`rclcpp`, `ament_cmake`) for the bridge package — kept
   under `integrations/ros2/`, never linked into `core/`.
4. **QoS mapping table** + topic↔path canonicalization (mostly the existing
   addressing rules).

## Recommendation

- **Now:** this note (interfaces declared). No code until M6 + batching exist and
  the interfaces here are reviewed.
- **Next:** `integrations/ros2/` skeleton implementing `Ros2Bridge::mirror` over
  `GenericSubscription`/`GenericPublisher` once M6 lands — a *bridge*, P-level.
- **Maybe never:** `rmw_libtracer`, unless an embedded/micro-ROS demand justifies
  the full rmw surface.

Sources: [ros2/rmw_zenoh](https://github.com/ros2/rmw_zenoh) ·
[eclipse-zenoh/zenoh-plugin-ros2dds](https://github.com/eclipse-zenoh/zenoh-plugin-ros2dds) ·
[Creating an rmw implementation (ROS 2 docs)](https://docs.ros.org/en/rolling/Tutorials/Advanced/Creating-An-RMW-Implementation.html) ·
[Zenoh experimental support in ROS 2 (ZettaScale)](https://www.zettascale.tech/news/zenoh-experimental-support-lands-in-ros-2/)
