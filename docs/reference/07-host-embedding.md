# Reference 07 — Host Graph Embedded in the Larger System

> **Status**: draft, v1, 2026-05-03. New material — this section synthesizes [04-communication-flows.md](04-communication-flows.md) (graph model, bridges) and [10-module-catalog.md](10-module-catalog.md) (bridges, discovery) and is the canonical home for the per-host-DAG ↔ global-topology distinction.
> **Audience**: anyone designing a multi-host deployment; anyone implementing the bridge / dedup logic.

---

## The load-bearing insight

> **Each host sees its slice of the network as a DAG; the global topology can be any graph, including cycles.**

Conforming implementations MUST handle this without livelocks or duplicate delivery, and they MUST do it transparently to application code. From the application's read/write API view, the local DAG IS the world; the bridge layer is invisible.

---

## Per-host view: a DAG of own vertices and bridge proxies

A host's local graph consists of:

- **Own vertices**: created by application code on this host, or by modules that expose hardware as vertices (e.g., `transport_i2c` exposing peripherals as `/i2c-bus/0xNN/...`).
- **Bridge proxies**: one local vertex per remote vertex this host has bound to via some bridge.

Both kinds are **first-class** to subscribers: they have schemas, settings, liveness state, an `:acl` field — everything described in [02-graph-model.md](02-graph-model.md). A subscriber reading `/can-bridge/wheel/left:schema` cannot tell from the response that the underlying data comes from a CAN device on a different physical board.

The local graph is a **DAG** because:

- Vertex paths are tree-shaped (`/a/b/c` is a child of `/a/b`).
- Subscriptions form edges from one vertex to another (a SUBSCRIBER's target path).
- Subscriptions can introduce structural cycles only if a subscriber writes back into a vertex it transitively listens to. This is application-level; the local graph data structure does not enforce DAG-ness on subscription edges, but the protocol does require **dedup at the bridge boundary** (see §cycle handling).

```
Local graph on linux-bridge-1:

    /
    ├── self/
    │     └── name = "linux-bridge-1"
    ├── sensor/                     ← own vertices
    │     ├── temp
    │     └── humidity
    ├── log/
    │     └── output
    ├── can-bridge/                 ← bridge proxies (incoming from CAN)
    │     ├── wheel-encoder/
    │     │     ├── left
    │     │     └── right
    │     └── imu/
    │           ├── accel
    │           └── gyro
    └── peer/                       ← bridge proxies (incoming from TCP, per-peer)
          ├── esp32-front/
          │     └── camera/frame[0..N]
          └── stm32-wheel/
                └── battery/voltage
```

A subscriber on this host runs `tracer_read("/peer/esp32-front/camera/frame[7]")` and gets bytes from a remote ESP32 that arrived over TCP, were dispatched into the bridge proxy, and are now served from this host's local cache (or fetched on-demand if the proxy doesn't cache). **The subscriber's code looks identical** to a `tracer_read("/sensor/temp")` of a local vertex.

---

## Global topology: any shape including cycles

The union of all hosts' local DAGs is the **global topology**. The protocol places **no restrictions** on its shape:

- **Tree**: a star — one central monitor + leaf devices, each device bridged once into the monitor.
- **Mesh**: every host bridged to every other host, no central authority.
- **Ring**: A bridges to B bridges to C bridges to A. Cycle present.
- **Arbitrary multi-graph**: two hosts may bridge the same remote vertex via different transport modules (e.g., CAN + IP). Both bridges are valid; the recipient sees the data twice unless dedup applies.

This is **deliberate**. Production deployments are messy:

- A robot fleet with redundant LAN + CAN bridges to survive a Wi-Fi blip.
- A mesh of edge devices with multi-path routing for fault tolerance.
- A research setup where data is recorded from two angles (a sniffer host + the production path).

The protocol does not require a spanning tree, a designated root, or a routing election. **It requires that every bridged TLV is dedup-able** so cycles don't cause storms.

---

## Cycle handling (mandatory)

Every TLV that crosses a bridge SHALL carry, in a `ROUTER` TLV (type `0x0D`, see [05-protocol-tlvs.md](05-protocol-tlvs.md) §ROUTER):

- `origin_peer_id` — the UUIDv4 or device-derived ID of the host that first published the TLV.
- `origin_timestamp` — the wall-clock time (ns since epoch) at which the TLV was first published.
- `hop_count` — number of bridges traversed so far.

A bridge receiving a TLV SHALL:

1. Check the `(origin_peer_id, origin_timestamp)` pair against a **recent-set** of seen pairs.
2. If already present, **drop the TLV silently** (no error, no log spam).
3. If not present, add to the recent-set, increment `hop_count`, **strip the ROUTER**, and proceed to local-graph dispatch using the bare data TLV (the [02-graph-model.md](02-graph-model.md) §the ROUTER shedding rule).
4. If `hop_count >= MAX_HOPS` (recommended 32), drop and emit `STATUS=ERROR(NESTING_TOO_DEEP)` to the local subscribers of the bridge's status path.

When the same bridge later re-emits this data on another transport (e.g., the local subscriber that received it is itself a remote subscriber over a second transport), the bridge:

1. Looks up the saved `(origin_peer_id, origin_timestamp, hop_count)` in its per-proxy metadata table.
2. Wraps the bare data TLV back into a `ROUTER` envelope (ROUTER's children are NAME-tagged metadata followed by `NAME "data"` and the wrapped TLV as last child) with `hop_count` incremented.
3. Attaches a fresh wire trailer (`opt.TS`, `opt.CR`) per the egress transport's conventions ([01-data-format.md](01-data-format.md) §the trailer is append-only).

The `ROUTER` lives on the wire between bridges, never inside a vertex's stored value.

### Recent-set sizing

Implementation-defined. Recommended bound:

```
recent_set_capacity = expected_max_fanout × expected_max_delivery_window_seconds × expected_publish_rate_hz
```

For typical deployments:

- A small mesh (≤8 hosts, ≤100 Hz typical, ≤1s delivery window): 800 entries — a few KB of memory.
- A large fleet (≤256 hosts, ≤1 kHz, ≤2s window): 512000 entries — a few MB.

The recent-set is **per-bridge**, not global. A bridge that runs out of recent-set capacity SHOULD evict the oldest entries (LRU); a duplicate that arrives after eviction will pass through and may briefly cause a redundant local dispatch, but the cycle terminates because the next bridge will dedup it (if it has not also evicted).

### Source attribution

The `origin_peer_id` in `ROUTER` is set by the **first bridge that puts the TLV on the wire**, not by the application that called `tracer_write`. In-process publishers don't see ROUTER; it's strictly a bridge-layer artifact. This means a TLV written locally and consumed locally never gains a ROUTER, which keeps loopback paths zero-overhead.

---

## Bridge identity

Each bridge module instance has a **peer-id**: a 128-bit UUIDv4 or device-derived identifier (e.g., MAC-address-based, factory-burned ID). Generation rules:

- **UUIDv4**: random 122 bits + version + variant. Default for Linux hosts.
- **Device-derived**: hash of MAC address + serial number + boot-time entropy. Used on MCUs without a stable RNG at first boot.
- **User-supplied**: explicit peer-id in the node config TOML. Used in deployments where peer-id stability across reboots is required.

The peer-id MUST be stable across the lifetime of a node's installation (typically across reboots). Two nodes with identical peer-ids on the same network is a misconfiguration; discovery modules SHOULD emit `STATUS=ERROR(PATH_IN_USE)` (semantic stretch) on collision.

### Discovery modules announce peer-id

Discovery modules (`discovery_mdns`, `discovery_static`, `discovery_gossip`) emit `(peer_id, transport_label, transport_address)` tuples. A bridge consuming a discovery announcement may choose to bind to a peer (instantiate a bridge proxy mount point) based on:

- **Static config**: `[[bridges]]` entries that name the peer-id explicitly or use the `{peer_id}` interpolation.
- **Dynamic policy**: bind to all discovered peers automatically (default for `discovery_mdns`).
- **Filter**: bind only to peers matching a glob, advertising specific transports, or holding a capability token.

---

## "Every host is a router"

There is **no architectural distinction** between a leaf and a router. Any host with:

- Two or more transport modules loaded, AND
- A `[[bridges]]` configuration linking them

…is a bridge between those transports. The bridge code path is the same as the single-transport-loopback path; it just dispatches to multiple sinks.

A specialized **WAN router** is a host that runs:

- Multiple WAN-friendly transports (`transport_quic`, `transport_ws`).
- A discovery module to find peers.
- No application vertices — the host's job is purely to route.

This is **convention**, not a separate node type in the protocol. Such a host conforms at profile P2 (per [00-overview.md](00-overview.md) §conformance profiles); the protocol does not single it out.

A future `router_wan` "module" (in the [10-module-catalog.md](10-module-catalog.md) catalog) may package up the typical WAN-router config (multiple transports + discovery + dedup tuning + observability) for ergonomics, but it does not extend the protocol.

---

## Embedding examples

### RC car: 1 host, 1 transport, no bridges

```
[ ESP32 RC car ]
  /motor/throttle
  /motor/steering
  /battery/voltage
  /self/...
  ↑
  └ transport_uart on USB-CDC ↔ host PC running tracer-cli
```

Local DAG = entire view. The host PC is also a 1-bridge node (it bridges UART to its own local graph) but the topology has no cycles to worry about. Conformance: P1 (single-transport leaf) on the ESP32, P1 on the PC.

### Robot with CAN bus + Wi-Fi

```
[ STM32 wheel encoder ]──CAN──┐
[ STM32 IMU            ]──CAN──┤
                                │  ┌────[ Linux brain ]───WiFi───[ ground station laptop ]
[ STM32 motor driver   ]──CAN──┴──┤
                                   └ /can-bridge/wheel/left
                                     /can-bridge/wheel/right
                                     /can-bridge/imu/accel
                                     /can-bridge/imu/gyro
                                     /can-bridge/motor/cmd
                                     /control/...                  (own vertices)
```

Linux brain is a 2-transport bridge: `transport_can` and `transport_tcp`. Each STM32 device's vertices appear under `/can-bridge/...` on the Linux brain's local DAG. The ground station subscribes to `/can-bridge/imu/accel` over TCP; from its view, the accelerometer is just `/peer/linux-brain/can-bridge/imu/accel`. The chain `STM32 → CAN → Linux → TCP → Laptop` looks like one path. Conformance: P1 on each STM32, P2 on the Linux brain, P2 on the laptop.

### Fleet of robots with central monitor (star)

```
[ robot 1 ]──TCP──┐
[ robot 2 ]──TCP──┼──[ monitor station ]
[ robot 3 ]──TCP──┤
[ robot 4 ]──TCP──┘
                    /peer/robot-1/...
                    /peer/robot-2/...
                    /peer/robot-3/...
                    /peer/robot-4/...

Monitor subscribes:
    write("/peer/**:subscribers[]", SUBSCRIBER{path="/local/recorder"})
```

A wildcard subscription on `/peer/**` aggregates everything from every robot into the monitor's recorder. Conformance: P1 on each robot, P2 on the monitor.

### Bridge dispatch end-to-end

The full path of one bridged write — emphasizing that **every dispatch step uses a path handle**, no string parsing happens on the hot path:

```mermaid
sequenceDiagram
    autonumber
    participant App as App on STM32
    participant TxA as transport_can (egress)
    participant CAN as CAN bus
    participant RxB as transport_can (ingress)
    participant DT as Bridge dispatcher<br/>(PATH-TLV-keyed)
    participant Sub as Local subscriber

    Note over App: path handle h_wheel<br/>= &.rodata PATH TLV<br/>for "/wheel/left"
    App->>TxA: write(h_wheel, VALUE)
    Note over TxA: wrap ROUTER<br/>emit on CAN (hop=1)
    TxA->>CAN: framed bytes
    CAN->>RxB: framed bytes
    Note over RxB: parse frame<br/>extract ROUTER<br/>recent-set check (A, T0)<br/>strip ROUTER → bare VALUE
    RxB->>DT: dispatch(h_proxy, VALUE)
    Note over DT: h_proxy = pre-bound handle<br/>for /can-bridge/wheel/left<br/>(allocated at mount time,<br/>not per-write)
    DT->>Sub: deliver(VALUE)
    Note over Sub: subscriber holds<br/>its own .rodata handle<br/>for /can-bridge/wheel/left;<br/>byte-equality match
```

Step 6 is the key one: the bridge's dispatcher holds a **pre-allocated path handle** for each bridge proxy vertex. When the bridge mount is created (via `[[bridge]] mount = "/can-bridge"`), the implementation walks the configured proxy paths and registers handles for each — exactly once, at config time. After that, every CAN frame that arrives feeds into a dispatch keyed on those bytes; no string is parsed on the hot path.

This generalizes: any vertex that routinely receives or emits — every bridge proxy, every periodic publisher, every wildcard subscription's matched-set member — has a handle allocated at the time it becomes addressable, not at the time of each operation.

### Mesh of robots with no central node (cycles)

```
[ A ]───┬───[ B ]
   \    │      |
    \   │      |
     \  │      |
      \ │      |
       \│      |
       [ C ]───┘
```

A bridges to B and C; B bridges to A and C; C bridges to A and B. A TLV written on A reaches B directly and via C. Without dedup, B sees it twice; with dedup (`origin_peer_id, origin_timestamp` recent-set), B sees it once.

Conformance: P2 on each. The cycle is structurally fine; the protocol's dedup requirement is what makes it operationally fine.

### WAN: edge sites bridged via QUIC router

```
[ site A devices ]──LAN──[ A router (transport_quic + discovery_static) ]
                                      │
                                      │ QUIC (Internet)
                                      │
[ site B devices ]──LAN──[ B router ]─┘
```

Each router is a host with `transport_tcp` (LAN) + `transport_quic` (WAN). Sites are bridged. From site B's view, site A's devices appear under `/peer/A-router/...`. Conformance: P2 on routers, P1 on devices.

---

## What this means for application code

### Path resolution is local

Every `tracer_read` / `tracer_write` / `tracer_await` call resolves against the **local** DAG using a **path handle** (per [03-addressing.md](03-addressing.md) §static path handles): a build-time `.rodata` PATH TLV literal or an init-time-registered handle, never a string parsed on the hot path. If the path is a bridge proxy, the bridge handles forwarding transparently. The application does NOT:

- Choose a transport for a write.
- Know the network topology.
- Distinguish "is this path local or remote?" (it MAY introspect via `:transport` field in the proxy's schema, but isn't required to).
- Format strings to dispatch to a bridge proxy. The proxy's PATH TLV is encoded once when the bridge mount is created; subsequent writes pass the handle.

### Failure surfaces locally

When a remote bridge fails (transport disconnect, peer crash, network partition):

- The bridge proxy vertex emits `STATUS=ERROR(TRANSPORT_DOWN)` to its subscribers.
- This is the **same API** as a local vertex going down (e.g., a sensor driver crashing).
- Application failover logic doesn't have to distinguish remote-vs-local; it reacts to STATUS events on the paths it cares about.

### The "address space" is global; the API is local

An application written against libtracer thinks in **paths**, not in IP addresses or transport URIs. Whether `/sensor/wheel/left` is a local I²C sensor, a CAN-bridged peripheral, or a Wi-Fi-bridged remote vertex makes no difference to the read/write call. The protocol's job is to make this invisibility hold; the operator's job (via configuration) is to wire up the bridges that realize the global topology.

This is the operational consequence of the third claim in [00-overview.md](00-overview.md): **bridges are core**. Decentralization isn't an opt-in feature on top of a centralized core; it's the foundation, and a single-host node is the trivial case of it.
