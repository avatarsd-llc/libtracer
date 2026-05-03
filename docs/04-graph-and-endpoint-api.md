# 04 — Graph Model and Endpoint API

> **Status**: draft, v0.1, 2026-05-03 — companion to [doc 03](03-wire-format-and-data-model.md). The wire format makes bytes; this doc makes meaning.
> **Audience**: anyone using libtracer to build an application; module authors who need to know what the core promises.
> **Reading time**: ~20 min.

---

## How to read this doc

§[The model in one picture](#the-model-in-one-picture) is the cartoon. §[The API in three calls](#the-api-in-three-calls) shows how small the surface is. §[Subscription via field-write](#subscription-via-field-write) is the load-bearing departure from DDS/Zenoh. §[Address-shift slicing](#address-shift-slicing-replaces-fragmentation) is the load-bearing departure from MQTT/DDS. §[QoS knobs](#qos-knobs) is the deliberately small subset of DDS. §[Bridges as core mechanism](#bridges-as-core-mechanism) is what makes one address space across CAN+IP+SHM possible.

---

## The model in one picture

```
                       /
                       ├── sensor/
                       │     ├── temp        <-- POINT (vertex)
                       │     │     └── :subscribers[0]   <-- per-sub state
                       │     │     └── :subscribers[1]
                       │     │     └── :settings.reliability
                       │     │     └── :schema
                       │     └── humidity    <-- POINT
                       ├── log/
                       │     └── output      <-- POINT
                       ├── can-bridge/       <-- ROUTER (bridge)
                       │     ├── wheel-encoder/
                       │     │     └── left
                       │     │     └── right
                       │     └── imu
                       └── self/             <-- this node's own metadata
                             └── name
                             └── transports
                             └── modules
```

A **vertex** has a **path** (its full address from `/`). It exposes:
- **Data** — the value you `read` and `write`.
- **Fields** under the `:` separator — `:subscribers[N]`, `:settings.X`, `:schema`, etc. These are the control surface.

An **edge** is a subscription: a record (a SUBSCRIBER TLV from [doc 03](03-wire-format-and-data-model.md)) sitting in some vertex's `:subscribers[N]` slot. When a publisher writes to a vertex, the router walks the subscriber list and re-publishes to each subscriber's PATH.

A **router** is just a special vertex that re-publishes incoming TLVs from one transport onto the local graph (see [§Bridges](#bridges-as-core-mechanism)). To a subscriber, a router is indistinguishable from a local source.

---

## The API in three calls

```c
#include <libtracer/core/tracer.h>

// 1. Read the value at a path.
//    `out_tlv` ownership transfers to caller; caller must
//    `tlv_release()` when done. NULL on missing/error.
tlv_t *tracer_read(const char *path);

// 2. Write a value to a path. `in_tlv` is consumed (refcount transferred
//    in), caller must not use it after this call returns.
//    Returns 0 on success, non-zero error code on failure.
int tracer_write(const char *path, tlv_t *in_tlv);

// 3. Wait for the next write at a path (blocking).
//    Returns the new value, ownership to caller. NULL on shutdown/error.
//    For event-driven (non-blocking) consumption, set a callback by
//    writing into ep:subscribers[N] — see §Subscription via field-write.
tlv_t *tracer_await(const char *path, uint64_t timeout_ns);
```

That's it. There are no `connect`, `disconnect`, `subscribe`, `unsubscribe`, `pull`, `query`, `request`, or `respond` calls. Everything is a read or a write to some path.

**Auxiliary** (not part of the conceptual API, but the real C surface needs them):

```c
// Initialize the global tracer state. Call once at process start.
int tracer_init(const tracer_config_t *cfg);

// Shut down. Call once at process exit.
void tracer_shutdown(void);

// Construct a TLV for writing.
tlv_t *tlv_new_value(const void *data, size_t len);
tlv_t *tlv_new_subscriber(const char *target_path, const tlv_t *settings);
// ...one helper per core type code from doc 03.

// Refcount management (see doc 03 §Refcount memory ordering).
void tlv_acquire(tlv_t *t);   // increment refcount
void tlv_release(tlv_t *t);   // decrement; destroy if zero

// Inspect a TLV (zero-copy).
tlv_t_id  tlv_type(const tlv_t *t);
const void *tlv_payload(const tlv_t *t);
size_t      tlv_payload_len(const tlv_t *t);
const tlv_t *tlv_first_child(const tlv_t *t);   // for LIST payloads
const tlv_t *tlv_next_sibling(const tlv_t *t);
```

The auxiliary surface is small enough that bindings (C++, TypeScript) can wrap it 1:1.

---

## Naming and paths

### Path syntax

```
path     := root [ '/' segment ]* [ ':' field ]?
root     := '/'
segment  := name [ '[' index ']' ]?
field    := name ( '.' name )* [ '[' index ']' ]*
name     := UTF-8, case-sensitive, max 64 bytes, no '/' ':' '.' '[' ']' '*' '?'
index    := decimal integer, 0..65535
```

Examples:
```
/sensor/temp                           — a vertex
/sensor/temp:subscribers[0]            — a control field on a vertex
/sensor/temp:settings.reliability      — a nested control field
/can-bridge/wheel-encoder/left         — a vertex behind a bridge
/camera/frame[7]                       — element 7 of an indexed-children vertex
```

### Wildcards (subscribe-only)

Wildcards are valid only in subscription paths (the `path` field of a SUBSCRIBER TLV), not in `tracer_read`/`tracer_write` calls.

```
*       — match exactly one path segment
**      — match zero or more path segments
```

```
/sensor/*/temp                         — matches /sensor/A/temp, /sensor/B/temp
/sensor/**                             — matches everything under /sensor/
```

### Limits

- Maximum total path length: 1024 bytes (UTF-8 encoded). Returned in `:schema` for introspection.
- Maximum nesting depth: 32 segments. Matches the iterative-parser depth cap from [doc 03](03-wire-format-and-data-model.md).
- Maximum field-chain depth: 8 (e.g., `:settings.transport_tcp.tls.cipher.suite` is at the limit).

---

## Subscription via field-write

This is the load-bearing departure from DDS, Zenoh, MQTT.

**To subscribe, write a SUBSCRIBER TLV into the publisher's `:subscribers[N]` slot.**

```c
// Subscriber side.
tlv_t *settings = tlv_new_settings(/* QoS knobs */);
tlv_t *sub      = tlv_new_subscriber("/log/output", settings);  // target

// Allocate slot N (next free) by writing to the array; or pick a
// specific N if you want stable identity for unsubscribe / re-config.
tracer_write("/sensor/temp:subscribers[]", sub);
//                                  ^-- empty index = "next free"
```

After this write:
- The router walks the subscriber list on every future write to `/sensor/temp`.
- For each subscriber, it constructs a new TLV with the publisher's payload and the subscriber's address, and dispatches.
- The subscriber side, somewhere, has previously called `tracer_await("/log/output", ...)` or registered a callback via the `executor_c` module (see [doc 06](06-modules-executor-security-gui.md)).

**To unsubscribe, write a `STATUS=OK` (empty) TLV into the same slot to clear it:**

```c
tlv_t *empty = tlv_new_status_ok();
tracer_write("/sensor/temp:subscribers[3]", empty);  // slot 3 cleared
```

Or, equivalently, write a SUBSCRIBER TLV with no PATH — same effect.

**To change subscription settings (rate limit, deadline, etc.) without re-subscribing:**

```c
tlv_t *new_rate = tlv_new_value_u32(20);
tracer_write("/sensor/temp:subscribers[3].settings.rate_limit_hz", new_rate);
```

### Why this is good

- **One API surface.** The same `read`/`write` primitive controls data and control. No separate "control client" library.
- **Subscriptions are introspectable.** Reading `/sensor/temp:subscribers[]` returns the full list as a LIST TLV. Tools like `tracer-top` (week 8 of [doc 02](02-roadmap-weeks-1-to-8.md)) walk it directly — no `MGMT_GET_SUBSCRIBERS` extra primitive.
- **Subscriptions can be replayed.** A persisted snapshot of `/sensor/temp:subscribers[]` is sufficient to re-establish the subscription; no separate "subscription state" file.
- **The wire format already exists.** A subscription is just a SUBSCRIBER TLV (type `0x04` from [doc 03](03-wire-format-and-data-model.md)) written via the same path you'd use to read data.

### Why this is hard

- **Schema discipline.** If every transport module invents its own field names under `:settings.X`, the schema sprawls, third-party tools can't introspect, and the elegance dies. Mitigation: §[Schema discipline](#schema-discipline) below.
- **Atomicity of multi-field updates.** Setting two fields requires two writes; a reader between them sees a partial state. Mitigation: write a single LIST TLV containing both fields to the parent path; the router applies it atomically.
- **Permission model.** Writing to `:subscribers[]` should require permission. Mitigation: ACL is checked on the path's `:acl` field (see [doc 06](06-modules-executor-security-gui.md) security modules).

### Fallback: ioctl-style control

For cases where the field-write surface is too verbose (e.g., a CLI tool that doesn't want to construct a SUBSCRIBER TLV by hand), the C API also exposes:

```c
int tracer_ctl(const char *path, tracer_ctl_op_t op, const void *arg);

// Examples:
tracer_ctl("/sensor/temp", TRACER_CTL_SUBSCRIBE,
           &(tracer_subscribe_arg_t){
               .target = "/log/output",
               .reliability = TRACER_QOS_RELIABLE,
           });
```

Internally, `tracer_ctl(SUBSCRIBE, ...)` constructs the appropriate SUBSCRIBER TLV and calls `tracer_write` on `:subscribers[]`. **It is a convenience wrapper, not a separate primitive.** Any state visible via `tracer_ctl` is also visible via `tracer_read` on the appropriate `:` path. This keeps the introspection story honest.

---

## Schema discipline

The field-write control surface only stays elegant if the field names are disciplined. Rule:

> **Core defines a small, fixed set of field names. Modules add fields under their own namespace.**

### Core field names (frozen for v0.1)

| Field path | Type | Writable | Meaning |
| ---- | ---- | ---- | ---- |
| `:subscribers[N]` | SUBSCRIBER | yes | Subscription record N |
| `:subscribers[]` | LIST of SUBSCRIBER | read-only | Read returns full list; write to `[]` appends |
| `:settings.reliability` | u8 enum | yes | `0=best-effort, 1=reliable` |
| `:settings.durability` | u8 enum | yes | `0=volatile, 1=transient-local` |
| `:settings.history_keep_last` | u32 | yes | N samples retained for late joiners |
| `:settings.deadline_ns` | u64 | yes | Max time between writes before liveness fault |
| `:settings.priority` | u8 | yes | `0=low ... 255=critical` (transport hint) |
| `:liveness.heartbeat_hz` | u8 | yes | Subscriber heartbeat rate; 0 = no liveness check |
| `:liveness.last_seen_ns` | u64 | read-only | Wall-clock of last write observed |
| `:schema` | LIST | read-only | Self-describing schema (field names + types) |
| `:description` | UTF-8 | yes | Human-readable description |
| `:acl` | ACL | yes (with permission) | Access control list (see [doc 06](06-modules-executor-security-gui.md)) |

Reading `/sensor/temp:schema` returns a LIST describing every field the vertex exposes — including module-added fields. This is how `tracer-top` and other tools discover what's writable.

### Module-added field names

A transport module like `transport_tcp` may add per-subscriber settings like `:subscribers[N].settings.transport_tcp.send_buf_kb`. Rules:

- Module fields MUST live under their own module name (here, `transport_tcp`).
- Module names MUST match the module's directory in `libtracer/modules/`.
- Module fields MUST appear in the vertex's `:schema` output if they apply to that vertex.
- Reading a module field on a vertex where that module is not active returns `ERROR=0x0A SCHEMA_NOT_FOUND`.

This keeps the namespace navigable: any user looking at a vertex can read `:schema`, see what's there, and not be surprised by fields invented by a module they don't have loaded.

---

## Address-shift slicing (replaces fragmentation)

[doc 03](03-wire-format-and-data-model.md) deliberately omits wire-level fragmentation rules. The application-level mechanism is **address-shift slicing**.

### How it works

A publisher with a logically-large message addresses **N child endpoints** with the **same timestamp**:

```c
uint8_t   *frame_data;     // 10 MB camera frame
size_t     frame_len;
uint64_t   ts = clock_now_ns();
size_t     slice_size = 64 * 1024;     // 64 KB per slice
size_t     n_slices   = (frame_len + slice_size - 1) / slice_size;

for (size_t i = 0; i < n_slices; i++) {
    size_t   off = i * slice_size;
    size_t   len = (off + slice_size > frame_len) ? frame_len - off : slice_size;

    tlv_t   *t = tlv_new_value_with_ts(frame_data + off, len, ts);

    char     path[128];
    snprintf(path, sizeof path, "/camera/frame[%zu]", i);

    tracer_write(path, t);
}
```

Each slice is a complete, valid TLV. Each is delivered independently. The transport module sees N TLVs and ships them however it ships them — TCP segments them, CAN frames them, RDMA queues them. **There is no reassembly state in the transport.**

### How a subscriber consumes it

A subscriber subscribes once to the **parent path with a wildcard**:

```c
tlv_t *sub = tlv_new_subscriber("/local/camera-handler", settings);
tracer_write("/camera/frame[*]:subscribers[]", sub);
```

After subscription, every write to any `/camera/frame[N]` is dispatched to `/local/camera-handler` with the slice index encoded in the path. The handler can:

- **Process slices as they arrive** (streaming) — common case for camera-pipeline.
- **Buffer until all slices in a timestamp group are present** — set `:settings.address_shift.assemble = true`.
- **Wait until deadline, then surface gaps** — `:settings.deadline_ns` plus `:settings.address_shift.on_gap = error`.

The subscriber's QoS at `:settings.address_shift.*` controls the assembly policy. (Field names final at week 6 of [doc 02](02-roadmap-weeks-1-to-8.md) once the cross-bus demo exercises them.)

### Why this is good

- **Lossless transport composition.** Whatever the transport does (drop a UDP datagram, lose a CAN frame), each slice is independently lost or delivered. No reassembly state to corrupt.
- **No special "fragment" type code.** The wire format from [doc 03](03-wire-format-and-data-model.md) doesn't need a `FRAGMENT` TLV with reassembly metadata — the addressing scheme carries it.
- **Stream processing is natural.** The subscriber decides whether to assemble or to process as a stream; the publisher doesn't impose either choice.

### Why this is hard

- **Index allocation discipline.** The publisher must agree with subscribers on what `[N]` means (is it byte offset / slice_size? is it row index? is it sample index in a window?). Document at the application layer; libtracer doesn't impose semantics.
- **Wildcard matching cost.** Subscribing to `/camera/frame[*]` requires the router to walk wildcard tables on every write. Optimization: when a subscriber appears with a wildcard, the router pre-computes the matching set on every relevant `tracer_write` path. Acceptable cost for the typical fan-in being modest (≤ few dozen subscribers per topic).

---

## QoS knobs

A deliberately small subset of DDS's 22 policies. The 5 chosen are the ones most production projects actually configure:

### `RELIABILITY`

- `best-effort` (default) — fire and forget; loss is silent.
- `reliable` — transport-dependent guarantee. On TCP/QUIC, free. On UDP, requires sequence numbers + ack/nack at the libtracer layer (post-MVP). On CAN, leverages CAN ack but adds retry on bus errors. On I2C/SPI/UART, treats hardware errors as fatal.

### `DURABILITY`

- `volatile` (default) — late joiner sees only future samples.
- `transient-local` — late joiner sees the most recent N samples (where N is `history_keep_last`).

`transient` (broker-backed durability) is **not in v0.1**; libtracer has no durable storage layer.

### `HISTORY` (`history_keep_last`)

Number of samples retained per subscriber for `transient-local` durability. Default 1. Maximum bounded by `:settings.queue_max_bytes` (per-subscriber memory cap).

### `DEADLINE` (`deadline_ns`)

If a vertex's writes are expected to arrive at least this often, missing a deadline triggers a `STATUS=ERROR(TIMEOUT)` write to subscribers. Default unset (no deadline check).

### `LIVELINESS` (`heartbeat_hz`)

Subscriber heartbeat rate. The subscriber writes to `/.../publisher:subscribers[self].liveness.last_seen_ns` periodically; if the publisher doesn't observe a heartbeat for 3 intervals, the subscription is marked dead.

### Defer to post-MVP (named here so they're acknowledged):

`PRESENTATION`, `LIFESPAN`, `OWNERSHIP`, `OWNERSHIP_STRENGTH`, `LATENCY_BUDGET`, `TIME_BASED_FILTER`, `PARTITION`, `DESTINATION_ORDER`, `RESOURCE_LIMITS`, `READER_DATA_LIFECYCLE`, `WRITER_DATA_LIFECYCLE`, `ENTITY_FACTORY`, `WRITER_BATCHING`, `TYPE_CONSISTENCY_ENFORCEMENT`, `DATA_REPRESENTATION`, `DURABILITY_SERVICE`, `TRANSPORT_PRIORITY` (libtracer has the simpler `priority` knob instead).

If a user genuinely needs DDS's full QoS surface, they need DDS, not libtracer.

---

## Failure model

Three things can fail: a vertex disappears, a network partitions, a subscription's deadline expires.

### Vertex liveness

A vertex with active subscribers writes a heartbeat to its own path at `:liveness.heartbeat_hz`. Subscribers observe `:liveness.last_seen_ns`; if it stops advancing for `3 / heartbeat_hz` seconds, the subscriber emits a local `STATUS=ERROR(TRANSPORT_DOWN)` event and (by default) clears the subscription slot.

To opt out: set `heartbeat_hz = 0`. Subscriptions are best-effort with no liveness check.

### Network partition

When a transport reports a peer disconnect (TCP RST, mDNS timeout, etc.), the affected bridge vertex emits `STATUS=ERROR(TRANSPORT_DOWN)` writes to all paths it was bridging. Subscribers see it and can react (retry, fail over to another transport, give up).

When the transport reconnects, the bridge re-publishes a snapshot of any `transient-local` data it had cached. New writes resume normally. There is no "graph repartition merge" logic — last-write-wins by timestamp; if both sides wrote during the partition, the higher timestamp wins.

### Deadline expiry

If a vertex has a `deadline_ns` set and no write is observed within the window, the local liveness checker emits `STATUS=ERROR(TIMEOUT)` to subscribers and (by default) increments a missed-deadline counter at `:liveness.missed_deadlines`.

### Coherency for distributed control (one paragraph)

For control-loop applications (e.g., MATLAB-derived feedback controllers split across vertices), libtracer transports timestamped samples; PTP-synced clocks across the network make the timestamps comparable to sub-microsecond accuracy on PTP-capable hardware (STM32F7, ESP32 with HW PTP). On non-PTP hardware (STM32F4 without PTP, generic Wi-Fi), accuracy degrades to ~ms via NTP. **The deterministic compute is the user's callback's responsibility; libtracer guarantees ordered delivery within a single subscription, not synchronized causality across subscriptions.** Cluster consensus (Raft, CRDT) is explicitly out of scope; if you need it, layer it above libtracer.

---

## Bridges as core mechanism

A **bridge** is a vertex that re-publishes incoming TLVs from one transport onto the local graph. It is the mechanism that makes one address space across heterogeneous transports possible.

### How a bridge works

```
   ┌──────────────────────────────┐
   │   Linux laptop libtracer node │
   │                              │
   │   /can-bridge/  ←── ROUTER  ─┼── transport_can ── CAN bus ── STM32
   │   /tcp-bridge/  ←── ROUTER  ─┼── transport_tcp ── LAN ─────── ESP32
   │   /local/log    ←── POINT    │
   │                              │
   └──────────────────────────────┘
```

When `transport_can` receives a TLV from the CAN bus addressed (in the CAN frame's payload TLV) to path `/sensor/wheel-encoder/left`, the `can-bridge` router prepends `/can-bridge/` and re-publishes locally. A subscriber on `/can-bridge/sensor/wheel-encoder/left` sees the data; the local subscriber doesn't know it came over CAN.

If the same node also runs `transport_tcp`, an external subscriber over TCP can subscribe to `/can-bridge/sensor/wheel-encoder/left` — the TCP transport delivers the TLV verbatim. The chain `STM32 → CAN → Linux bridge → TCP → ESP32` looks to the ESP32 like one path.

### Bridge configuration

Bridges are configured at startup. Reference TOML (config format final at week 3 of [doc 02](02-roadmap-weeks-1-to-8.md) once `discovery_static` is implemented):

```toml
[node]
name = "linux-bridge-1"

[[transports]]
module = "transport_tcp"
bind   = "0.0.0.0:7700"

[[transports]]
module = "transport_can"
device = "can0"
bitrate = 1000000

[[bridges]]
mount  = "/can-bridge"
source = "transport_can"
# Republish everything from CAN under /can-bridge/

[[bridges]]
mount  = "/tcp-bridge"
source = "transport_tcp"
filter = ["/sensor/**", "/log/**"]
# Only republish matching paths
```

### Why bridges are core, not a module

If the bridge mechanism were a module, every node would have to opt in to load it before participating in cross-transport flows. That defeats the "one address space" claim. Bridges are part of the core router; the core's behavior is "if you have multiple transports loaded, you can be a bridge."

Single-transport nodes are still bridges in the trivial sense (they bridge between the wire and the local graph). The core's bridge code path is the same.

---

## I2C / CAN / SPI driver as a libtracer vertex tree

When a Linux host has an I2C bus with multiple peripherals, the `transport_i2c` module exposes the bus as a vertex tree:

```
/i2c-bus/
  ├── 0x3C/   ← OLED display at I2C address 0x3C
  │     └── data
  ├── 0x68/   ← MPU-6050 IMU at I2C address 0x68
  │     ├── accel
  │     └── gyro
  └── 0x77/   ← BMP280 pressure sensor
        ├── temperature
        └── pressure
```

A subscriber on `/i2c-bus/0x68/accel` receives accelerometer samples whenever the driver reads them (driver polls or DMA-based, configurable per-device).

`tracer_write("/i2c-bus/0x3C/data", oled_image_tlv)` causes the I2C driver to push the bytes to the OLED. The driver is a libtracer module, the bus is graph-addressable, and the user code never touches `i2c_smbus_write_byte_data()`.

**This is the "I2C/CAN device as routed vertex" pattern from the user's vision.** It only works if the bus driver authors expose the right vertex shape; a naive port of an existing driver to libtracer doesn't get this for free. [doc 05](05-modules-transport-and-discovery.md) sketches the per-bus driver layer.

---

## Worked example: subscribe via field-write, no `connect` call

Full code for the simplest pub/sub case. Compare to DDS / ROS2 / Zenoh equivalents — count the lines and primitives.

```c
#include <libtracer/core/tracer.h>
#include <stdio.h>

static void on_temp(const tlv_t *t, void *unused) {
    int32_t deg_c;
    memcpy(&deg_c, tlv_payload(t), sizeof deg_c);
    printf("temp = %d\n", deg_c);
}

int main(void) {
    tracer_init(NULL);

    // Subscribe: write a SUBSCRIBER TLV into /sensor/temp:subscribers[]
    tlv_t *sub = tlv_new_subscriber_callback(on_temp, NULL);
    tracer_write("/sensor/temp:subscribers[]", sub);

    // ... main loop, callbacks fire from the libtracer worker thread ...
    pause();
    tracer_shutdown();
    return 0;
}
```

Publisher:

```c
int main(void) {
    tracer_init(NULL);

    while (1) {
        int32_t  deg_c = read_thermometer();
        tlv_t   *t     = tlv_new_value(&deg_c, sizeof deg_c);
        tracer_write("/sensor/temp", t);
        sleep(1);
    }
}
```

Two primitives (`tracer_write`, `tracer_read`/`tracer_await`/callback). One `:` field (`subscribers[]`) for the control surface.

---

## What's NOT in this doc

- The wire bytes — see [doc 03](03-wire-format-and-data-model.md).
- The transport-module ABI — see [doc 05](05-modules-transport-and-discovery.md).
- The discovery mechanism — see [doc 05](05-modules-transport-and-discovery.md).
- Executor modules (callbacks, scripting) — see [doc 06](06-modules-executor-security-gui.md).
- Security / ACL enforcement — see [doc 06](06-modules-executor-security-gui.md).
- Diagnostic GUI / `tracer-top` — see [doc 06](06-modules-executor-security-gui.md) and week 8 of [doc 02](02-roadmap-weeks-1-to-8.md).
- The glossary of terms — see [doc 99](99-glossary.md).
