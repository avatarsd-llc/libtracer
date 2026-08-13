# ESP-IDF integration

Packages the libtracer C++ reference core as an [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) component, so an ESP-IDF project can depend on libtracer through the component manager.

## Supported ESP-IDF versions

**ESP-IDF `>=5.5.5`** (`idf_component.yml`). This is a TX-path correctness floor, not a packaging preference ([#949](https://github.com/avatarsd-llc/libtracer/issues/949)): below it `httpd_queue_work` is a bare non-blocking `sendto` on `esp_http_server`'s loopback control socket, so an enqueue past `CONFIG_LWIP_UDP_RECVMBOX_SIZE` is discarded inside lwIP while the call still reports success — a WebSocket frame lost with nothing observable anywhere. From 5.5.5 the mbox slot is reserved through a counting semaphore before the `sendto` (`httpd_main.c`), so a full control queue is an `ESP_FAIL` the caller sees and `httpd_ws_link_t` can report every dropped frame on `enqueue_drops()`. The link is written for that guarantee and carries no compensation for its absence, so 5.3.x, 5.4.x and 5.5.0–5.5.4 are **not** supported.

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
  avatarsd-llc/libtracer: "^0.3.0"
```

## What the component packages

**The full node** ([#183](https://github.com/avatarsd-llc/libtracer/issues/183)) — everything a device shaped like the originating production firmware (an ESP32-C6 smart-agriculture node) needs to run libtracer as its MAIN transport:

| Layer | Sources | Notes |
| --- | --- | --- |
| L2/L3 wire codec | `frame`, `tlv_arena` | arena decode (ADR-0041) included |
| L0/L1 substrate | `mem_heap`, `mem_pool`, `rope` | `pool_t` = the bounded MCU backend |
| L4 graph runtime | `path`, `graph` | read / write / await, `:children[]`, `:subscribers[]` |
| net plane | `op_resolve`, `route_handle`, `fwd_router`, `transport_vertex`, `loopback` | explicit-source-routed FWD (ADR-0040); `child_registry` is header-only |
| socket transports | `transport_udp`, `transport_tcp` | lwIP BSD sockets on chips, glibc on the `linux` target |
| WebSocket | **`httpd_ws_link.cpp`**, **`esp_ws_client_link.cpp`** (this component) on chips; `transport_ws` on `linux` | ESP-IDF WebSocket is IDF-native and never POSIX — see below |
| CAN transport | `transport_can` + **`twai_link.cpp`** (this component) | framing/advertise/reassembly are portable & host-tested; the TWAI link is the on-chip `can_link_t` |

**Platform TU selection is a build-system concern** (the no-feature-macro ruling — no `#ifdef`s in shared sources): chip targets compile `socketcan_link_stub.cpp` (SocketCAN is Linux-only) plus `twai_link.cpp`; the `linux` host target compiles the real `socketcan_link.cpp` and no TWAI. The WebSocket plane follows the same rule (next section).

### The WebSocket plane: IDF-native on chips, portable on `linux`

**ESP-IDF WebSocket never uses POSIX sockets** ([#947](https://github.com/avatarsd-llc/libtracer/issues/947) ruling). Core's `transport_ws_server` / `transport_ws_client` are the **host** implementation, and a chip build does not compile them at all — not as a footprint preference but as a correctness one. Their scatter-gather egress (`posix_endpoint_t::write_all_iov`) asks `sendmsg` for `MSG_NOSIGNAL`; lwIP *defines* that flag but `lwip_sendmsg` rejects any flag outside `MSG_DONTWAIT|MSG_MORE` with `EOPNOTSUPP`, which `write_all_iov` reads as peer-gone. On silicon the portable server therefore accepts connections, completes the RFC 6455 handshake, answers PINGs — and silently discards **every** data frame ([#948](https://github.com/avatarsd-llc/libtracer/issues/948)).

So on a chip target the plane is:

| Role | Type | Backed by | Gated on |
| --- | --- | --- | --- |
| serve | `tr::net::httpd_ws_link_t` | `esp_http_server` (stand up its own, or adopt the running SPA server) | `CONFIG_LIBTRACER_TRANSPORT_WS` **and** `CONFIG_HTTPD_WS_SUPPORT` |
| dial | `tr::net::esp_ws_client_link_t` | `esp_transport_ws` over `tcp_transport` | `CONFIG_LIBTRACER_TRANSPORT_WS` |

Neither is a factory entry: the application constructs the link and hands it in with `transport_vertex_t::provide_link`. Consequently a chip build registers **no** `ws` kind in the built-in catalog, and a `:children[]` SPEC carrying `kind=ws` with no staged link answers `SCHEMA_NOT_FOUND`. The `linux` target keeps the portable pair — it has glibc's `sendmsg` and no `esp_http_server`.

For the dial link the recipe has one more step ([#1102](https://github.com/avatarsd-llc/libtracer/issues/1102), ADR-0081): construct `esp_ws_client_link_t` **with `defer_recv = true`**, `provide_link`, then issue the creating `:children[]` write. The flag holds the link's first dial until `start_receiving()` — which the creating write's `make_connection` calls once the receiver sink is installed — so a peer's push-on-connect cannot arrive before a sink exists and be dropped silently. There is no `ws` factory on a chip target to pass the flag for you (the way the core `tcp`/`ws` factories pass theirs), so opting in is the application's move; a link constructed with the flag and never armed **never dials**. The historical dial-at-once default is unchanged for embedders that wire the receiver themselves before any peer can push.

`tools/check_esp_ws_plane.py` is the gate: zero `transport_ws_server` / `transport_ws_client` symbols in the linked chip ELF (`nm`), the portable TUs uncompiled, and both native links still built.

### lwIP portability audit (what the socket transports use)

The transports compile against lwIP's BSD-socket layer **unmodified — no shim was needed**: `SO_RCVTIMEO` (the recv-loop stop-poll idiom; on by default in ESP-IDF's lwIP), `sendmsg`/`iovec` gather writes (the rope-to-wire path), `getsockname`, `poll`, `TCP_NODELAY`, `inet_pton`. The one MCU-relevant behavior fix landed in core (macro-free): `udp_transport_t` sizes its RX segments to `min(64 KiB, backend->max_segment_size())`, so a `pool_t` with MTU-sized slots receives datagrams instead of dropping them, and the recv thread no longer carries a 64 KiB scratch frame on its (small) pthread stack.

**One audited item was wrong, and is now known-wrong:** this list used to claim `MSG_NOSIGNAL` was fine because lwIP defines it and has no SIGPIPE. It defines it and then *refuses* it — `lwip_sendmsg` returns `EOPNOTSUPP` for it. `lwip_send` ignores unknown flags, so single-buffer writes are unaffected and only the **gather** path breaks. That is what removed WebSocket from this list; `transport_tcp` still rides the same `write_all_iov` and remains on ESP-IDF pending a separate ruling ([#948](https://github.com/avatarsd-llc/libtracer/issues/948)).

### TWAI `can_link_t` (`include/libtracer_esp/twai_link.hpp`)

`tr::net::twai_link_t` implements the same seam `socketcan_link_t` implements, over the `esp_driver_twai` node API: **classic CAN 2.0 only** (TWAI has no FD — configure `transport_can` as `CLASSIC`), 29-bit extended IDs (the CAN-ID *is* the path, ADR-0022). The driver's RX-done callback runs in ISR context and only copies the frame into a FreeRTOS queue; a dispatch thread pops and feeds `transport_can` — user code never runs in the ISR. The framing/advertise/reassembly layers above the seam are covered by the host test suite (`core/tests/transport_can_test.cpp` over a fake link); **bus-level validation on a real transceiver is an on-silicon checklist item on #183**, not a CI gate.

## Examples

| Example | Target(s) | What it proves |
| --- | --- | --- |
| [`examples/inprocess_mirror/`](examples/inprocess_mirror/) | chips | P0 in-process profile: register / write / read / await on FreeRTOS |
| [`examples/host_smoke/`](examples/host_smoke/) | `linux` | the component as a host_test dependency (no FreeRTOS tasks, no esp_log) |
| [`examples/full_node/`](examples/full_node/) | chips + `linux` | **the origin-firmware shape**: one-slab recipe, sensor vertex, config-created UDP listener via `/net:children[]` SPEC, remote subscriber fan-out |

### full_node (the #183 readiness example)

The device node wires the **one-slab recipe (ADR-0039/0042) concretely**: one static slab, front region → `pool_t` (RX datagram segments; exhaustion = backpressure, never OOM), back region → `monotonic_buffer_resource` + `synchronized_pool_resource` (the router's **label tables**). Since #588 the terminus **arena** is not among them — it draws from the router's nothrow `rx` block source, which this example leaves at the default heap, as are the graph's own three seams (the example default-constructs the graph). So the slab bounds the RX segments and the label tables, not yet the whole steady state. The **recycling** `block_source_t` that bounds the rest now exists — `tr::mem::pool_source_t` (#597 / ADR-0067) — and wiring it into this example is a follow-on: it wants a per-child source on the router rather than one shared across receive threads (ADR-0067 §3). See `docs/reference/09-memory-substrate.md`. Connections are **config-created**: `write /net:children[] SPEC{listener, kind=udp, port}` constructs and owns the real socket (ADR-0027) — same for the `tcp` kind via the built-in catalog. `ws` is not in that catalog on a chip target (see [the WebSocket plane](#the-websocket-plane-idf-native-on-chips-portable-on-linux)); it is staged with `provide_link`.

On the `linux` target CI also **runs** it: an in-process host-peer node dials the device node over **real loopback datagrams** and drives `FWD{READ}` → reply, `:subscribers[]` subscribe carrying a `durability_request` → latch (RFC-0022 §3.A), and device write → remote fan-out observed via `graph.await`. On a chip it parks in the publish loop; set Wi-Fi credentials via `idf.py menuconfig` (*full_node example*) to make the same listener reachable from a LAN host (the on-silicon e2e).

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

## Memory budget (measured per CI run)

Footprint numbers are **measured and published on every run of the `full-node` CI job** — this README deliberately carries no copied-in numbers (they would go stale the next time a source file changes). To read the current numbers:

1. Open the latest [`esp-idf` workflow run](https://github.com/avatarsd-llc/libtracer/actions/workflows/esp-idf.yml) → the **`full-node (esp32c6)`** (or `esp32c3`) job.
2. The **job step summary** carries a markdown table with the **libtracer component's own contribution** (flash = text+rodata, static RAM = data+bss+iram, from `esp-idf-size --archives` on `libtracer.a`) next to the whole-image totals, plus a per-memory-type breakdown.
3. The same numbers are attached as a machine-readable **`footprint-<target>` JSON artifact**, and the raw `idf.py size` / `idf.py size-components` tables are in the `Report footprint` step log.

What is measured: the `full_node` build (`espressif/idf:release-v6.0`, built `-Os` — the example sets `CONFIG_COMPILER_OPTIMIZATION_SIZE`, overriding IDF's default `-Og`, so the footprint reflects a size-optimized deployment) — graph + fwd_router + transport_vertex + udp/tcp/ws/can transports + TWAI link, plus the example app and IDF's Wi-Fi/lwIP stack.

The footprint is **tracked, never gated**: [`tools/esp_size_gate.py`](../../tools/esp_size_gate.py) reports the libtracer component's flash and static-RAM contribution and sets no ceiling on either (it fails the job only if it cannot parse the size output). That is deliberate — a ceiling in this repo would police a number nobody deploys, and the library must stay free to serve the thinnest possible client; what has to fit is *your* image on *your* target, so arm a ceiling in your own build if you have one (pass `--max-flash-bytes` / `--max-static-ram-bytes`). Regressions are caught by reading the published numbers run to run — the current figures live in the `footprint-<target>` CI artifact and the job's step summary, and are not copied here (they would go stale). The library holds no internal buffers: its static RAM is a few hundred bytes of function-local bookkeeping, a property tracked and reviewed rather than enforced by a ceiling.

Steady-state heap is what you configure: the full_node example runs its RX segments and router tables out of a **24 KiB static slab** (12 KiB pool → 7 × 1536 B datagram slots + 12 KiB pmr arena); each live socket transport additionally owns one recv thread (stack below).

## Configuration

The component exposes libtracer's build-time knobs through **Kconfig** (`idf.py menuconfig` → *libtracer*, or set them in `sdkconfig` / `sdkconfig.defaults` like any IDF option):

| Option | Type | Default | Effect |
| ------ | ---- | ------- | ------ |
| `CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES` | int (1–256) | 16 | Process-wide vertex lock-stripe count — the **only** global mutable buffer libtracer links into a node. N stripes cost N lazily-allocated FreeRTOS mutexes (~90 B heap each) + N condvars, paid once a `graph_t` exists (independent of how many vertices you register). A vertex's stripe is chosen by hashing its pinned address, so lowering N only raises **control-plane** lock contention (ring trim, edge/ACL mutation, `await` wake) — the lock-free LKV read/write hot path is unaffected. On a single-core target (esp32c3/c6) **4–8** reclaims RAM at negligible cost; the multi-core default stays 16. |

The value is delivered as the ordinary constexpr `tr::graph::kVertexLockStripes` (ADR-0068). The component writes a generated `<libtracer/config_override.hpp>` — a fragment that inherits `tr::graph::default_config_t` and states only this knob and the three other ESP-specific ones — into a directory listed before `core/include` in the component's public include dirs, where the checked-in `config.hpp` finds it with `__has_include`. One shared header per build, so the `inline constinit` stripe table's size can never diverge across TUs. (A bare `-D` no longer does anything; the Kconfig option is the knob.)

## FreeRTOS / threading notes

- **The transports spawn one recv thread each (a FreeRTOS task via `pthread_create`).** Every socket transport owns one recv thread; `transport_can`'s TWAI link owns one dispatch thread. **Right-size each one directly**: pass `recv_stack` (bytes) to the transport constructor, or set `twai_link_config_t::stack_size`, and it sizes just that thread — no need to inflate the **global** `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT`, which is paid by *every* pthread in the system (#486). A `0` (the default) keeps the global default. Size to the measured high-water mark (`uxTaskGetStackHighWaterMark`, surfaced in the `/system/tasks` census) plus margin — the ws/tcp serve loops carry ~4 KiB chunk buffers + TLV decode frames, so the ESP-IDF default 3 KiB is NOT enough for an un-sized global default (the example raises the global to **12 KiB** as the blunt fallback). Priorities follow `CONFIG_PTHREAD_TASK_PRIO_DEFAULT` (5) — below Wi-Fi/lwIP tasks, which is the right default: recv loops are throughput, not latency-critical ISR work.
- **`await` is a condvar wait** (`std::condition_variable` → pthread condvar): it blocks the calling task only, wakes on the producer's write, and honors the timeout via the FreeRTOS tick. Don't call it from timer-service or event-loop callbacks.
- **Single-core (esp32c3) sanity**: the refcount/`<atomic>` paths and the one-recv-thread-per-socket model have no core-affinity assumptions; the esp32c3 build is CI-gated. Runtime soak on c3 silicon is an on-silicon checklist item.
- **ISR discipline**: nothing in libtracer may be called from an ISR. The TWAI link is the pattern: ISR → FreeRTOS queue → dispatch thread → libtracer.

## Requirements

- **ESP-IDF v6.0** (tested in CI; matches the origin firmware's IDF v6.0-dev / GCC 15 toolchain) — libtracer's core is **C++23** (`std::expected`, `std::span`), which needs GCC 13+; CI pins `release-v6.0` for the chip targets and `release-v5.5` for the two `linux`-target jobs. The **component's** floor is `idf >=5.5.5` (see *Supported ESP-IDF versions* above) — above both the C++23 compiler floor and the `esp_driver_twai` node API's IDF ≥ 5.5.
- `PRIV_REQUIRES pthread, lwip, esp_driver_twai` (chip targets) — all **private**: libtracer's public headers expose only libstdc++ headers, never `<pthread.h>` or lwIP/driver headers, so nothing propagates to dependents. On the `linux` target only pthread is required (sockets come from glibc).
- **Exceptions / RTTI** stay at the ESP-IDF default (**OFF**). Two of the three parts of that are true and one is not, and the distinction matters most on exactly this target:
  - **RTTI-free — true.** No `typeid`, no `dynamic_cast` anywhere in `core/include` or `core/src`.
  - **Links clean under `-fno-exceptions -fno-rtti` — true**, for the full-node profile including the examples; CI builds it for esp32c6, esp32c3 and `linux`.
  - **"Never throws" — FALSE.** The data path *reports* by value (`std::expected` / `status_t`) and draws peer-driven allocations from injected **nothrow** seams (ADR-0065), which is the shape worth relying on — but the tree is not throw-free, and under `-fno-exceptions` a throw is not an exception, it is `abort()` — a reboot. See the table below.

### The three throwing sites this component compiles in

Each of these still reports exhaustion by throwing and is reachable by a peer or by the local sender, so price them on a node that must not reboot. They are the same two rows [docs/design/allocation-and-backpressure.md](../../docs/design/allocation-and-backpressure.md) §"Where the rule is not met today" names; what this table adds is *why each one is in this component's archive*.

| Site | Code | Provoked by | In this component because |
| ---- | ---- | ----------- | ------------------------- |
| Label-table binds (#603) | `core/src/route_handle.cpp:82`, `:182`, `:247` — `std::pmr::vector::push_back` and the route copy beside it | a **peer**: an ingress `ADVERTISE` binds a label. `max_label_bindings_per_link` bounds the entry *count*, not the allocation's failure mode | `route_handle.cpp` is unconditional in the source list (`libtracer/CMakeLists.txt:61`) |
| `try_reserve` on `-fno-exceptions` (#923, #850) | `core/include/libtracer/mem_heap.hpp:157-171` — `try_grow` catches the container's own allocation failure on a hosted build; **this component is not one**, so IDF's `-fno-exceptions` build keeps the probe-then-commit fallback | anything concurrent — and single-core is not single-threaded: a FreeRTOS context switch between the probe's free and the `reserve` opens the window. Every `try_*` helper inherits it on this profile | header-only, reached from the graph and every transport |

A third row — the CAN egress window table — used to head this list; #1110 removed it for every build, CAN on or off, by deleting the window vector rather than bounding it. The two that remain are structural until #603 lands and until the remaining `try_*` sites move to the ADR-0065 failable seam.

## Security posture

**Unsafe by default — but "by default" is the operative word, not "unavailable".** The distinction the previous wording erased:

- **ACL enforcement ships and this component compiles it in.** `core/include/libtracer/security_acl.hpp` is real: `graph_t::acl_allows` gates READ / WRITE / CREATE (`core/src/graph.cpp:994`, called at `:588`, `:770`, `:1071` and the write paths), and the component builds against the MCU profile `allow_only_policy_t` — core's default binding (`core/include/libtracer/config.hpp:246`), which the component's override fragment inherits rather than restating — first-match-per-bit ALLOW, DENY rejected at parse time, no full-ACL Kconfig yet. See [docs/modules/security-acl.md](../../docs/modules/security-acl.md).
- **It is OFF until you turn it on.** `acl_allows` returns `true` immediately when no `subject_resolver_` is installed (`graph.cpp:995`), returns `true` for the EMPTY caller context (a local API call — settled *before* the resolver is consulted, #905), and returns `true` when no ancestor up the chain bears an ACE — *open by default*, by design. A node is enforced only once the host calls `graph_t::set_subject_resolver` **and** writes `:acl` ACEs. What it does **not** do any more is treat an unresolvable caller as trusted: the resolver's `std::expected` error arm is a **deny**, so a resolver that cannot name a peer refuses it at every gate rather than granting it `WRITE_ACL`. Nor may your resolver return `EVERYONE@`: that spelling is the wildcard subject and is reserved against the resolver's output (#908), so a resolver that hands it back names no principal and the caller is refused — a pass-through of caller-supplied identity cannot mint a peer that impersonates the wildcard.
- **Link security does not exist here.** No TLS/DTLS/PSK module is compiled into this component: the udp/tcp/ws listeners the full-node profile opens are **plaintext and unauthenticated**, and QUIC (the one transport with TLS 1.3, ADR-0043) is a separate `libtracer_quic` target that this component does not build. Do not expose the listeners beyond a trusted LAN.

So: ACL is an opt-in you can turn on today; transport security is genuinely absent and still post-MVP.

## Status

**Built in CI** ([`.github/workflows/esp-idf.yml`](../../.github/workflows/esp-idf.yml)):

- `inprocess_mirror` (P0 profile) — builds for **esp32c6** + **esp32c3**.
- `full_node` (full-node profile, #183) — builds for **esp32c6** + **esp32c3**, including the TWAI link; the footprint steps publish the component-size table to the job step summary, and upload the `footprint-<target>` JSON artifact — tracked, not gated (see [Memory budget](#memory-budget-measured-per-ci-run)).
- `full_node` — builds **and runs** for the **`linux`** target: device node + host peer over real datagrams, read/subscribe/latch/fan-out/await end to end (the runtime proof CI can give without silicon).
- `host_smoke` — builds and runs for the **`linux`** target (the host_test path).

On-silicon runtime validation (Wi-Fi FWD e2e, TWAI on a real bus, conformance vectors on-target, footprint soak) is the maintainer's hardware checklist on [#183](https://github.com/avatarsd-llc/libtracer/issues/183). Report build issues there.
