# ESP32 node profile

A node that runs libtracer as its **primary communication stack** on an ESP32-class
MCU is a **bounded reactor**: every resource the graph plane touches is an injected,
fixed-capacity pool declared at init, and every overload surfaces as backpressure
rather than as an allocation failure. Where the
[custom-device guide](custom-device.md) says *what* a device exposes, this page says
how to run it within an MCU's RAM, flash and task budget.

The compile-tested starting point is the bundled
[`full_node` example](https://github.com/avatarsd-llc/libtracer/tree/main/integrations/esp-idf/examples/full_node)
(`integrations/esp-idf/examples/full_node`); this page is the hardening layered on
top of it.

---

## 1. The node profile: a bounded reactor

On a single-core MCU there is no second core to absorb a leak — the heap watermark
only ever ratchets down. So the whole graph plane draws from storage the application
owns and sized, and reports exhaustion by value.

The one-slab recipe
([ADR-0039 — pmr memory model](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0039-pmr-memory-model-host-aligned-allocation.md),
[ADR-0042 — refcounted receiver seam](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0042-refcounted-receiver-seam-view-delivery.md)):

```cpp
/** @brief One static slab feeds BOTH memory seams — nothing grows at runtime. */
static std::byte g_slab[24 * 1024];

// Front region: pool_t — RX segments for owning transport delivery.
// Every inbound datagram lands in a pool slot; exhaustion = backpressure.
tr::mem::pool_t rx_pool{front_region(g_slab), /*slot_payload=*/1536};

// Back region: monotonic + synchronized arena — the pmr seam (label tables,
// LKV control blocks).
std::pmr::monotonic_buffer_resource arena{back_region(g_slab).data(),
                                          back_region(g_slab).size()};
std::pmr::synchronized_pool_resource shared{&arena};

// The FAILABLE seam is separate: everything a PEER can provoke (the terminus
// decode arena) draws from it, and it reports exhaustion by value instead of
// throwing (ADR-0065). Injecting only `shared` above leaves those allocations
// on the global heap — where they at least RECYCLE, which matters below.
//
// The two BYTE-BUFFER seams — graph_t's `value_backend` and fwd_router_t's `flat`
// — stay on heap_backend() here, and the warning below says why: a BARE pool_t is
// not thread-safe, and both seams are reached from several threads.
//
// ... graph_t graph{&shared, /*value_backend=*/&tr::mem::heap_backend(),
// ...               /*ctl=*/&blocks};
// ... fwd_router_t router{graph, &shared, /*rx=*/&blocks,
// ...                     /*flat=*/&tr::mem::heap_backend()};
```

Those are the three injection points of `graph_t`'s constructor — the pmr resource,
the value backend and the failable control source
(`core/include/libtracer/graph.hpp:187-189`) — and the matching three of
`fwd_router_t`: the pmr resource, the failable `rx` source and the `flat` byte backend
its rope flattens draw from (`core/include/libtracer/fwd_router.hpp:140-142`). The full set of
build-time and injected bounds is catalogued in
[the configuration space](../design/config/00-configuration-space.md); the failure
semantics of the third seam are in
[failable allocation and backpressure](../design/allocation-and-backpressure.md).

:::{warning}
**Do not point `value_backend` or `flat` at a bare `tr::mem::pool_t`.** Both are
`mem_backend_t` seams, and an injected `mem_backend_t` MUST be thread-safe
([ADR-0060 — LKV copy store](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0060-lkv-copy-store-injected-value-backend.md)
§2): a `segment` self-routes its reclaim on whichever thread drops the last reference,
which is not in general the thread that allocated it. `flat` adds a second reason —
three of the router's four flatten sites run on a transport child's **receive** thread
(several children receive concurrently) and the fourth runs on the **writer** thread
inside the remote-delivery fan-out. `pool_t`'s free list is a plain `std::size_t` head
and count with no lock and no atomic, so two threads can be handed the same slot and a
stored value aliases onto an outbound frame.

`sync_pool_t` (`core/include/libtracer/mem_pool.hpp:118`) is the in-tree composition of
a pool with synchronisation — but it is a **spinlock**, the multi-core-host variant, and
it is wrong for a single-core MCU, where a lower-priority task holding the lock cannot
run while a higher-priority task spins. The variant this target needs is the
interrupt-disable critical-section pool ADR-0060 §2 names, selected as an
[ADR-0047 — build-time closed module sets](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md)
§2 module-set trait — and **it is not built**
([zero-copy and flatten](../design/zero-copy-and-flatten.md) §5 records the same gap for
the receive backend, which is why `rx_pool` above feeds transport delivery and nothing
else). Until it exists these two seams stay on `heap_backend()`, which is thread-safe.
That is the honest state of "one slab, whole stack" on a single-core target, not a
licence to inject the pool anyway.
:::

Keeping `flat` on the heap means the router's four flattens — the ingress
`ADVERTISE` / `COMPACT` sub-rope flattens, the cold bus-name rejection flatten and the
per-delivery egress one — sit outside the node's slab bound (#730). Two further caveats,
so the bound is not read wider than it is: those four are the only flattens `flat` covers
— the **terminus resolver's** rope-tier flattens (`core/src/op_resolve_view.cpp:80`,
`:142`) never see it and draw from the global heap on every fragmented terminus request
(#766) — and `flat` bounds the flattened *bytes*, not the frame builds beside them.

:::{warning}
Do **not** reach for `tr::mem::bump_source_t` as `blocks`. It is scope-lifetime only:
a bump block is never reclaimed, so a long-lived bump seam fills monotonically and
then refuses every frame. An 8 KiB bump source wired as a router's `rx`, decoding a
53-byte FWD, served **six frames and rejected the next 194**
([ADR-0067 — bounded recycling source](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md)
§1; the same figure is carried on the type at
`core/include/libtracer/mem_source.hpp:178-179`). A frames-served count without the
payload size is not a measurement — 194 rejected 53-byte frames is a different fact
from 194 rejected 1 KiB frames.

Use `tr::mem::pool_source_t`, which recycles.
:::

`pool_source_t` takes the slab **and** a caller-owned span of `size_class_t` slots
(`core/include/libtracer/mem_source.hpp:325`), so both bounds belong to the caller
rather than to the library
([RFC-0006 — resource-bounded nesting depth](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0006-resource-bounded-nesting-depth.md)):

```cpp
// One region of the slab, plus a class table sized from what this node actually draws.
static tr::mem::size_class_t ctl_classes[8];
static tr::mem::pool_source_t<> blocks{ctl_region, ctl_classes};
```

:::{warning}
**Give each transport child its own source; do not share one across receive
threads.** A shared free-list pool measured at roughly **a fifteenth of its own
single-thread rate** on a 12-core host while the platform heap scaled, and a
lock-free `[index | ABA-tag]` CAS does not fix it — it replaces one contended word
with the same word
([ADR-0060 — LKV copy store](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0060-lkv-copy-store-injected-value-backend.md)
erratum 1). Confirmed at the router's own RX draw by `bench_rx_source_topology`
(median of three 300 ms runs, 12-core / 24-thread host): the shared pool falls to a
**sixty-seventh** of its single-thread rate, 244 ns → 16,428 ns per thread, while a
per-child pool tracks the scaling heap across the sweep (ADR-0067 §3).

Pass the source per child instead — the bound then also becomes per-peer, so one
noisy link cannot starve another's decode:

```cpp
router.add_child("up", up_link, /*rx=*/&up_blocks);
```

A source shared at **wiring** frequency — a graph's `ctl` — is fine with a locking
`Sync` policy (`tr::mem::sync_mutex_t` from `mem_source_sync.hpp`, or a target's own
interrupt-disable section). The policy is the `pool_source_t<Sync>` template
parameter, defaulting to `sync_none_t`, which compiles to nothing.
:::

After a soak run, `classes_used()` says how many slots the node really needed and
`overflowed()` must read zero (`core/include/libtracer/mem_source.hpp:376,380`) — a
non-zero count means the class span is too small and blocks are being lost to the
slab.

Rules that follow:

- **Steady state allocates from the slab, not the global heap.** After init, an
  ESP-IDF heap trace shows libtracer flat.
- **Allocation failure must not abort.** ESP-IDF's default C++ `new` throws; under
  `-fno-exceptions` that lowers to `abort()`. Every allocation on the delivery path
  is alloc-or-backpressure: drop the sample, count it, publish the counter (§6).
  Audit any path that calls throwing `new`.
- **Size the pool from the transport, not from hope.** `udp_transport_t` sizes RX
  segments to `min(64 KiB, backend->max_segment_size())`
  (`core/src/transport_udp.cpp:134`; `kMaxDatagram = 65536` at
  `core/include/libtracer/transport_udp.hpp:40`). Give the pool MTU-sized slots and
  datagrams arrive without a 64 KiB scratch buffer on a small thread stack.

## 2. Role composition and the transport RAM lever

**Transports, not the core, dominate idle RAM: each socket/CAN listener costs a
dedicated FreeRTOS task.** Budget stack + TCB ≈ **12 KB apiece** plus the transport's
own protocol buffers, and ~**24 KB** of idle RAM for a node that enables TCP-listen
"just in case" and CAN "because the silicon has it".

:::{note}
Those two figures are a **budgeting rule of thumb**, not a measurement — they carry
no named instrument or host. The 12 KB has a configuration basis rather than a
measured one: on ESP-IDF a `std::thread` is a pthread on FreeRTOS, so every recv
thread takes `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT`, which the bundled example
pins at `12288` (`integrations/esp-idf/examples/full_node/sdkconfig.defaults`).
Right-size against real high-water marks per §4 rather than against this number.
:::

So compose per deployment role, and load nothing else:

| Role | Load | Do not load |
| ---- | ---- | ---- |
| Wi-Fi leaf publishing sensors | 1× WS **or** UDP listener | TCP-listen, CAN |
| CAN sensor pod | `transport_can` (TWAI link) | all socket transports |
| CAN↔IP gateway (forwarder) | CAN + one socket transport | the third transport |
| Bench/debug image | whatever is under test | a ship image is not a debug image |

Listeners are **config-created in-band**: a `SPEC` write to `/net:children[]`
carrying a `kind` field creates a connection. The universal keys are `addr`, `kind`,
`port`, `role`, `keepalive` (`core/src/transport_vertex.cpp:53`, read at `:64`); the
two catalog child types are `client` and `listener` (`:99-107`); the created
connection mounts and routes at `/net/<module>/<name>`, the module **declared by the
application** via `register_module` (`:133`) — declared-only per ADR-0073 §4, so an
undeclared kind fails creation with `SCHEMA_NOT_FOUND` (`:157`). The accepted direction is
[RFC-0014 — creator endpoint, connection lifecycle and link liveness](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md),
which replaces the single global catalog with a per-module creator endpoint
`/net/<module>/conn`; that endpoint is not implemented, so a node built against this
release writes `/net:children[]`.

Role composition is therefore deployment configuration, not a firmware fork — but the
*type set* compiled in is the flash and RAM commitment, so trim `LIBTRACER_SRCS` to
the kinds the product ships (§7).

Budget for the plane itself: a full graph plane (codec, graph, router, one socket
transport) adds **tens of KB of idle heap** over a bare-metal firmware. That figure
is a hedged expectation, not a measurement. It is the cost of a real comms stack, not
a leak — reclaim RAM by shedding transports and retiring the ad-hoc stacks libtracer
replaces, not by shaving the core.

## 3. Single-core tuning

- **`CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES=4`** (menuconfig → libtracer;
  `integrations/esp-idf/libtracer/Kconfig:48`). The stripe table is the only global
  mutable buffer libtracer links: `N * sizeof(vertex_stripe_t)` bytes of `.bss`
  reserved at link time, plus the same for the condvar table. Sixteen stripes suit a
  multi-core host — that is the default (`kVertexLockStripes = 16`,
  `core/include/libtracer/config.hpp:79`) — while a single-core chip reclaims RAM at
  **4–8** (`config.hpp:70-71`). A stripe's platform mutex is lazy: on FreeRTOS it
  costs ~90 B of heap on its first lock, so an untouched stripe costs its struct and
  no heap.
- **Pin task priorities deliberately**: transport RX threads just below the
  application's control loop. Publish cadence belongs to the producer; no throttling
  exists in the library.
- **ISR handlers enqueue, they do not dispatch.** The TWAI RX callback runs in ISR
  context and only enqueues; dispatch happens in a task. An application apply seam
  that does real work defers likewise rather than running inside a transport thread's
  delivery path.

## 4. Task-stack sizing

**Size stacks from stressed high-water marks, never idle ones.** A stack that reads
40 % free at idle can overflow on the first deep path. ESP-IDF's HTTP server task
takes its stack from `httpd_config_t.stack_size`, which `HTTPD_DEFAULT_CONFIG()`
leaves at **4096 B** — enough for plain request serving and not for a deep WS send
path. Right-sizing means running the device at its boundary (max peers, churn of
subscribe/unsubscribe, biggest frames, OTA in flight), then reading
`uxTaskGetStackHighWaterMark` per task and adding margin. Publishing the census as
vertices (§6) makes every later soak test re-check it.

**A stack size is configuration: keep every override in versioned
`sdkconfig.defaults`.** An override that lives only in a local `sdkconfig` reverts on
a clean checkout, and the regression reappears weeks later on someone else's machine.

```ini
# sdkconfig.defaults — stack sizes are product decisions, not local state
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_HTTPD_TASK_STACK_SIZE=8192   # if the node serves HTTP/WS
```

`CONFIG_ESP_MAIN_TASK_STACK_SIZE` is an upstream ESP-IDF symbol (IDF default 3584).
`CONFIG_HTTPD_TASK_STACK_SIZE` is **not** — no such symbol exists in ESP-IDF, whose
httpd stack is the runtime `httpd_config_t.stack_size` field. A product that wants
the httpd stack under version control declares the symbol in its own
`Kconfig.projbuild` and assigns it into the config struct before `httpd_start`; the
`sdkconfig.defaults` line above is then the versioned record of that decision. Set
without the project-side symbol, the line is inert.

## 5. Network behavior under pressure

- **An oversized or unsendable frame is that frame's problem, not the session's.**
  Drop the one delivery and count it; never tear down a peer's session because one
  fan-out payload did not fit. A session drop turns one slow subscriber into a
  reconnect storm.
- **Egress is gather, not copy.** The rope-to-wire path lowers to an iovec `sendmsg`
  (`core/src/posix_endpoint.cpp:92,98`; the TCP assembly is at
  `core/src/transport_tcp.cpp:57-73`), and lwIP provides `sendmsg` unmodified. Do not
  flatten payloads before send; the only legitimate flatten is a substrate boundary
  DMA cannot span.
- **Backpressure beats buffering.** Where a node buffers for a slow subscriber, the
  buffer is bounded and newest-wins. An unbounded egress queue on a 512 KB-RAM chip
  is a crash with extra steps.

## 6. Observability vertices

Everything a soak test needs is readable — and subscribable — through the node
itself, described via `:schema` like any other data
([interoperability](../interoperability.md)):

```text
/system/
├── heap/free          u32 bytes — current free heap
├── heap/min_free      u32 bytes — lifetime low-watermark
├── tasks/<name>/hwm   u32 bytes — per-task stack high-water mark
└── drops/<counter>    u32 — backpressure counters (WS drops, pool exhaustion, …)
```

The backpressure counters come from `graph_t::delivery_drops()`
(`core/include/libtracer/graph.hpp:817`), which snapshots three per-cause totals —
`no_target`, `denied`, `out_of_memory` (`graph.hpp:751-758`). They are counted and
never enforced: nothing in the library reads them, so the deployment decides what to
alarm on. The three loads are individually relaxed rather than one atomic snapshot,
so their useful reading is "is this growing", not an instant total.

The `min_free` trend under stress is the most predictive health signal a fleet
dashboard can watch; per-task HWM vertices make §4's re-check a `read` rather than a
JTAG session.

## 7. ESP-IDF build-system rules

- **A `CONFIG_*`-gated `PRIV_REQUIRES` never propagates** — component requirements
  resolve before Kconfig runs. Gate **SRCS** on `CONFIG_*`, keep REQUIRES
  unconditional, and keep a CI job building each Kconfig-gated TU.
- **Every new core source is also appended to the component's `LIBTRACER_SRCS`**
  (`integrations/esp-idf/libtracer/CMakeLists.txt:41`) or the chip build fails to
  link while host builds stay green.
- **Platform TU selection is a build-system concern, not an `#ifdef`.** Chip targets
  compile `twai_link.cpp` plus a SocketCAN stub; the `linux` target compiles real
  SocketCAN and no TWAI (`integrations/esp-idf/libtracer/CMakeLists.txt:127-137`).
  Extend that pattern rather than adding macros.
- Build with `-fno-exceptions -fno-rtti` and treat any throwing construct on the
  delivery path as a defect (§1).

## 8. Flash layout

- **Two OTA app slots plus rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`): a
  new image confirms itself healthy or the bootloader reverts.
- **Web assets ship in a dedicated data partition**, not inside the app image — a UI
  change then does not burn an app slot, and the app image stays under the slot
  ceiling. The staleness footgun: a partition flashed once and forgotten serves last
  month's UI against this month's firmware, so the asset write belongs to the same
  release step as the app OTA.
- Expect the graph plane to be roughly **flash-neutral-to-negative** against the
  ad-hoc stacks it replaces — protocol handlers, bespoke framing, glue. The codec and
  graph cost is offset by the deleted legacy; whether the net is negative on a given
  product depends on how much legacy actually goes away.

## 9. Validation procedure

Measurements of an embedded node are only comparable under these conditions:

1. **The config is pinned before comparing.** Idle-heap deltas between two images
   mean nothing unless both `sdkconfig`s are diffed; config drift reads as a code
   regression.
2. **Baselines are rebuilt, not inherited.** The label on an already-flashed board
   is not evidence of what it is running; flash both images in the same session.
3. **Churn is the test.** Hundreds of rounds of subscribe/unsubscribe, create/delete
   and connect/disconnect while publishing at rate. Crashes live in the churn path,
   not the steady state.
4. **Numbers are banked from the boundary.** Min-free heap and per-task HWM are read
   *after* the stress run (§6), on the shipping image, with the shipping transport
   set (§2).

---

## Bring-up order

```text
boot ─► NVS/config ─► one-slab init (§1) ─► graph_t + vertices + :schema tables
     ─► fwd_router + transport_vertex (catalog: only the kinds this role ships, §2)
     ─► /net:children[] SPEC writes create the role's listeners
     ─► observability vertices registered (§6)
     ─► owner loop: sample hardware ─► write vertices (fan-out) ─► feed watchdog
```

A node built to this profile runs libtracer as its primary stack in a few tens of KB
of RAM, degrades under overload by dropping *data* rather than *sessions or uptime*,
and exposes everything an integrator — or a fleet dashboard, or a stranger's coding
agent — needs through the same three verbs and `:schema` as every other libtracer
device.

Related reading: [building a custom interoperable device](custom-device.md) for what
a device exposes, [failable allocation and
backpressure](../design/allocation-and-backpressure.md) for the semantics of the
`ctl` seam, and [the configuration
space](../design/config/00-configuration-space.md) for the full set of build-time
knobs and injected bounds.
