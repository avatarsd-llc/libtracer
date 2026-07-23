# A production ESP32 node — the hardened profile

A detailed, opinionated recipe for running libtracer as the **main communication
stack** of a real ESP32-class product — not a demo. Every rule on this page was
learned the hard way on a shipped ESP32-C6 deployment (single-core RISC-V, 4 MB
flash, Wi-Fi + CAN, a web UI, OTA, and libtracer as the graph plane) and then
distilled here with the proprietary parts removed. Where the
[custom-device guide](custom-device.md) says *what* to expose, this page says *how
to run it* within an MCU's RAM, flash, and task budget.

The compile-tested starting point is the bundled
[`full_node` example](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/esp-idf/examples/full_node)
(`integrations/esp-idf/examples/full_node`) — this page is the production hardening
layered on top of it.

---

## 1. The node profile: a bounded reactor

The shape that survives production is a **bounded reactor**: every resource the
graph plane touches is an injected, fixed-capacity pool declared at init, and
every overload surfaces as **backpressure, never OOM**. On a single-core MCU there
is no second core to absorb a leak — the heap watermark only ever ratchets down.

Concretely (the `full_node` one-slab recipe, ADR-0039/0042):

```cpp
/** @brief One static slab feeds BOTH memory seams — nothing grows at runtime. */
static std::byte g_slab[24 * 1024];

// Front region: pool_t — RX segments for owning transport delivery.
// Every inbound datagram lands in a pool slot; exhaustion = backpressure.
tr::mem::pool_t rx_pool{front_region(g_slab), /*slot=*/1536};

// Back region: monotonic + synchronized arena — router terminus & label tables.
std::pmr::monotonic_buffer_resource arena{back_region(g_slab).data(),
                                          back_region(g_slab).size()};
std::pmr::synchronized_pool_resource shared{&arena};
```

Rules that follow from it:

- **Steady state allocates from the slab, not the global heap.** After init, the
  ESP-IDF heap trace should show libtracer flat.
- **Never let allocation failure abort.** ESP-IDF's default C++ `new` throws → on
  `-fno-exceptions` that is an instant `abort()`. A production node under memory
  pressure once crash-looped exactly this way until every allocation on the
  delivery path was converted to alloc-or-backpressure (drop the sample, count it,
  publish the counter — see §6). Audit any code path that calls throwing `new`.
- **Size the pool from the transport, not from hope.** `udp_transport_t` sizes RX
  segments to `min(64 KiB, backend->max_segment_size())` — give the pool MTU-sized
  slots and datagrams arrive without a 64 KiB scratch buffer on a small thread
  stack.

## 2. Compose the node for its role — transports are the RAM lever

The single biggest idle-RAM finding from production: **the core is lean; the
threads are not free.** Each socket/CAN listener a node loads costs a dedicated
FreeRTOS task — stack + TCB ≈ **12 KB apiece** — plus its protocol buffers. A node
that enables TCP-listen "just in case" and CAN "because the silicon has it" carries
~24 KB of idle RAM for capabilities nobody wired.

So compose per deployment role, and load nothing else:

| Role | Load | Don't load |
| ---- | ---- | ---- |
| Wi-Fi leaf publishing sensors | 1× WS **or** UDP listener | TCP-listen, CAN |
| CAN sensor pod | `transport_can` (TWAI link) | all socket transports |
| CAN↔IP gateway (forwarder) | CAN + one socket transport | the third transport |
| Bench/debug image | whatever you're testing | ship image ≠ debug image |

Listeners are **config-created in-band** (`write /net:children[]
SPEC{listener, kind=udp|tcp|ws, port}`), so role composition is deployment
configuration, not a firmware fork — but the *type set* you compile in is the
flash/RAM commitment, so trim `LIBTRACER_SRCS` to the kinds the product ships.

Budget honestly: a full graph plane (codec, graph, router, one socket transport)
adds **tens of KB of idle heap over a bare-metal firmware.** That is the legitimate
cost of a real comms stack, not a leak — reclaim RAM by shedding transports and
retiring the legacy stacks libtracer replaces, not by shaving the core.

## 3. Single-core tuning

- **`CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES=4`** (menuconfig → libtracer). The
  stripe table is the only global mutable buffer libtracer links; 16 stripes suit
  a multi-core host, while on a single-core chip 4–8 reclaims RAM at negligible
  contention cost (the lock-free LKV read/write hot path never touches stripes).
- **Pin task priorities deliberately**: transport RX threads just below the
  application's control loop; the publish cadence belongs to the owner (the
  producer owns cadence — no libtracer throttling exists to save you).
- The TWAI RX callback runs in ISR context and only enqueues; dispatch happens in
  a task. Keep your own handlers on the same discipline — an apply seam that does
  real work must defer, not run in the delivery path of a transport thread.

## 4. Task-stack discipline (the crash you will otherwise ship)

Two production incidents, one rule each:

1. **Size stacks from stressed high-water marks, never idle ones.** A stack that
   looks 40 % free at idle can overflow on the first deep path — the production
   crash was an HTTP-server task at ESP-IDF's default 4096 B overflowing under a
   deep WS send path, weeks after it "worked." Right-size = run the device at its
   boundary (max peers, churn of subscribe/unsubscribe, biggest frames, OTA in
   flight), then read `uxTaskGetStackHighWaterMark` per task and add margin.
   Publish the census as vertices (§6) so every future soak test re-checks it.
2. **Keep every stack override in versioned `sdkconfig.defaults`.** That 4096
   regression happened because a working 8192 override lived only in a local
   `sdkconfig` and silently reverted on a clean checkout. If a stack size matters,
   it is configuration, and configuration lives in the repo:

```ini
# sdkconfig.defaults — stack sizes are product decisions, not local state
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_HTTPD_TASK_STACK_SIZE=8192   # if the node serves HTTP/WS
```

## 5. Network behavior under pressure

- **An oversized or unsendable WS frame is that frame's problem, not the
  session's.** Drop the one delivery and count it; never tear down the peer's
  session because one fan-out payload didn't fit. (A session-drop here turns one
  slow subscriber into a reconnect storm.)
- **Egress is gather, not copy**: the rope-to-wire path lowers to `sendmsg`/iovec
  on lwIP unmodified. Don't "help" by flattening payloads before send — the only
  legitimate flatten is a substrate boundary the DMA cannot span.
- **Backpressure beats buffering.** A slow subscriber gets stale-drop (bounded
  ring, newest wins) — an unbounded egress queue on a 512 KB-RAM chip is just a
  crash with extra steps.

## 6. Observability is vertices (self-describing, per the interop model)

Everything you need during a soak test should be readable — and subscribable —
through the node itself, described via `:schema` like any other data
([interoperability](../interoperability.md)):

```text
/system/
├── heap/free          u32 bytes — current free heap
├── heap/min_free      u32 bytes — lifetime low-watermark (the number that matters)
├── tasks/<name>/hwm   u32 bytes — per-task stack high-water mark
└── drops/<counter>    u32 — backpressure counters (WS drops, pool exhaustion, …)
```

The `min_free` trend under stress is the single most predictive health signal a
fleet dashboard can watch; per-task HWM vertices make §4's re-check a `read`, not
a JTAG session.

## 7. Build-system gotchas (ESP-IDF specifics that cost days)

- **A `CONFIG_*`-gated `PRIV_REQUIRES` never propagates** — component requirements
  resolve before Kconfig runs. Gate **SRCS** on `CONFIG_*`, keep REQUIRES
  unconditional, and make sure some CI job builds each Kconfig-gated TU.
- **Every new core source must also be appended to the component's
  `LIBTRACER_SRCS`** (`integrations/esp-idf/libtracer/CMakeLists.txt`) or the
  chip build fails to link while host builds stay green.
- **Platform TU selection is a build-system concern, not `#ifdef`s**: chip targets
  compile `twai_link.cpp` + a SocketCAN stub; the `linux` target compiles real
  SocketCAN and no TWAI. Extend the pattern, don't macro around it.
- Build with `-fno-exceptions -fno-rtti` and treat any throwing construct on the
  delivery path as a bug (§1).

## 8. Flash layout

- **Two OTA app slots + rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`): a
  new image must confirm itself healthy or the bootloader reverts.
- **Ship web assets in a dedicated data partition**, not inside the app image — a
  UI tweak then doesn't burn an app slot, and the app image stays under the slot
  ceiling. Beware the staleness footgun: a partition flashed once and forgotten
  will happily serve last month's UI against this month's firmware; make the asset
  write part of the same release step as the app OTA.
- Expect the graph plane to be roughly flash-neutral-to-negative vs the ad-hoc
  stacks it replaces (protocol handlers, bespoke framing, glue): the production
  cutover measured the firmware **smaller** after migration, with the codec/graph
  cost more than offset by deleted legacy.

## 9. Validate like production, not like a demo

The measurement discipline that made the numbers above trustworthy:

1. **Pin the config before comparing.** Idle-heap deltas between two images are
   meaningless unless both `sdkconfig`s are diffed — most "regressions" were
   config drift, not code.
2. **Rebuild your own baselines.** Never trust a previously-flashed board's label;
   flash both images yourself in the same session.
3. **Churn is the real test**: hundreds of rounds of subscribe/unsubscribe,
   create/delete, connect/disconnect while publishing at rate. Crashes hide in
   the churn path, not the steady state.
4. **Bank numbers only from the boundary**: min-free heap and per-task HWM read
   *after* the stress run (§6), on the shipping image, with the shipping
   transport set (§2).

---

## Putting it together

```text
boot ─► NVS/config ─► one-slab init (§1) ─► graph_t + vertices + :schema tables
     ─► fwd_router + transport_vertex (catalog: only the kinds this role ships, §2)
     ─► /net:children[] SPEC writes create the role's listeners
     ─► observability vertices registered (§6)
     ─► owner loop: sample hardware ─► write vertices (fan-out) ─► feed watchdog
```

A node built to this profile runs libtracer as its primary stack in a few tens of
KB of RAM, degrades under overload by dropping *data* instead of *sessions or
uptime*, and exposes everything an integrator — or a fleet dashboard, or a
stranger's coding agent — needs through the same three verbs and `:schema` as
every other libtracer device.

For wiring these ideas into an *existing* ESP-IDF firmware with its own legacy
stack, see the [integration walkthrough](../getting-started.md) and the
[custom-device guide](custom-device.md).
