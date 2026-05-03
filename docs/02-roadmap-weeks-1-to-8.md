# 02 — Roadmap (Weeks 1 – 8)

> **Status**: draft, v0.1, 2026-05-03 — week 1 starts the next working day after [doc 00](00-vision-and-reality-check.md) is approved.
> **Audience**: the maintainer doing the work; anyone tracking whether libtracer hits its MVP.
> **Reading time**: ~12 min.

---

## How to read this doc

Eight one-week slices. Each slice has: a **goal** (one sentence), a **demo** (the thing that runs at the end of the week), a **deliverables** list (what files exist on Friday), a **definition of done** (the bit-flip from in-progress to complete), and a **risks-this-week** sub-list.

The roadmap is **demo-driven, not spec-driven.** Every week ships a thing that runs. Specs (doc 03 wire format especially) freeze when demos prove they need to.

If a week slips, the rule is: **finish week N before starting week N+1.** No parallel weeks. The 8-week budget is meant to flex by ±2 weeks total without abandoning the plan.

---

## Week 1 — Core shape exists

### Goal
Stand up the core/module repo layout, fix the broken stubs, and prove read/write works in-process between two endpoints.

### Demo
A single C program creates two named endpoints (`/sensor/temp` and `/log/output`). One thread writes a TLV to `/sensor/temp`. Another thread reads from `/log/output`. A subscription written into `/sensor/temp:subscribers[0]` causes writes to fan out to `/log/output` automatically. No transport, no network, just core.

### Deliverables (files on Friday)
- `CMakeLists.txt` — top-level build with `LIBTRACER_NO_ATOMIC` and `LIBTRACER_BUILD_TESTS` options.
- `libtracer/core/` — new directory. Move and refactor:
  - `tlv.h` — keep the 13-type enum from current [libtracer/tlv.h](../libtracer/tlv.h); upgrade `crc:u16` field to `u32`; bump version bit; fix flexible-array-member doc.
  - `tlv.c` — CRC-32C software implementation (~2 cycles/byte).
  - `endpoint.h` / `endpoint.c` — the read/write API, the `subscribers[]` field plumbing, no `connect`/`disconnect`.
  - `router.h` / `router.c` — in-process fan-out from publisher to subscriber list.
  - `segment.h` / `segment.c` — buffer-chain refcount with the canonical memory orderings from doc 03.
- `libtracer/cpp/` — moved from current `libtracer/`:
  - Existing `tlv_vector.hpp`, `tlv_string.hpp` adapted to wrap the new C core.
  - Fix [libtracer/tracer.hpp](../libtracer/tracer.hpp): three `name_t` redefinitions collapsed into one templated definition; `status_t` ID corrected from `tlv_t::PATH` to `tlv_t::STATUS`; remove `connect`/`disconnect` from `point_i`.
- `tests/` — Catch2 (or Unity for pure-C) tests:
  - `test_tlv_crc.c` — round-trip CRC-32C verification.
  - `test_endpoint_rw.c` — write a TLV, read it back, assert payload matches.
  - `test_endpoint_fanout.c` — write to one endpoint with two subscribers, both observe.
- `examples/01_in_process.c` — the demo above, runnable as `./build/examples/01_in_process`.

### Definition of done
- `cmake -B build && cmake --build build && ctest --test-dir build` passes on Linux x86-64 with GCC 14 (`-std=c23`).
- The demo program exits with code 0 and prints both observed messages.
- Existing files in `libtracer/` (the C++ headers) compile cleanly after the refactor (or are formally moved; either is fine).

### Risks this week
- **R1.1**: the buffer-chain refcount in C is harder than in C++. Resolution: copy the memory-ordering pattern from doc 03 verbatim; resist any cleverness.
- **R1.2**: subscribing-via-field-write is novel — implementation may need iteration to feel right. Resolution: don't ship the fancy `ep:subscribers[self]` syntax in week 1; ship a simpler `endpoint_subscribe(ep, callback)` C call internally, with the field-write surface deferred to week 2 once the wire format is closer to frozen.
- **R1.3**: the existing `tracer.hpp` redefinitions imply this code never built. Confirm by running the existing build (there isn't one — there's no CMakeLists.txt) and treat the C++ side as starter sketches, not working code.

---

## Week 2 — TCP transport module + wire format frozen

### Goal
First transport module exists; wire format v0.1 is byte-exact and locked.

### Demo
**Button → relay over LAN.** Host A (Linux laptop) runs a publisher that watches a GPIO-mocked file descriptor (or just stdin) and writes a 1-byte TLV to `/control/button`. Host B (separate Linux machine, or same machine on a different port) runs a subscriber on `/control/button:subscribers[0]` that toggles a GPIO-mocked LED (or stdout). Both hosts connect via the new `transport_tcp` module.

### Deliverables
- `libtracer/modules/transport_tcp/` — new directory:
  - `transport_tcp.h` — the public init/bind/send/recv ABI.
  - `transport_tcp.c` — implementation using BSD sockets, blocking accept loop in v0.1 (epoll / kqueue / IOCP comes post-MVP).
  - Conforms to the transport-module ABI specified in doc 05.
- `libtracer/core/bridge.h` / `bridge.c` — the core mechanism that re-publishes incoming TLVs from a transport module onto the local graph. Per [doc 04](04-graph-and-endpoint-api.md).
- `examples/02_button_relay.c` — two roles in one binary, selected by command line: `./button_relay --pub --bind 0.0.0.0:7700` and `./button_relay --sub --connect HOST:7700`.
- **`docs/03-wire-format-and-data-model.md` promoted to "stable"**: every byte position locked, version byte set to 0x01, no further changes without a version bump.

### Definition of done
- The two-host demo runs reliably (1000 button presses, 1000 LED toggles, zero loss on loopback).
- Wire-byte capture (`tcpdump -X port 7700`) matches the byte tables in doc 03 exactly.
- A second, independent reader of doc 03 could write a parser that interoperates.

### Risks this week
- **R2.1**: the wire format reveals an issue when actually sent over a real socket (e.g., header alignment requires a struct attribute we forgot). Resolution: this is the entire point of doing the demo first — patch the spec before locking it.
- **R2.2**: TCP framing bugs. libtracer's TLV is self-delimiting (length in header), but a partial-read from `recv()` is the classic mistake. Resolution: explicit "read-N-bytes-or-error" helper, tested with a small kernel buffer.

---

## Week 3 — Discovery module (mDNS)

### Goal
Devices find each other on a LAN without static IP/port configuration.

### Demo
**Plug-and-play.** Three devices on the same Wi-Fi: a Linux laptop, an ESP32, and a second Linux machine. Each starts a libtracer node with a unique node name. Within seconds, each node knows the others' names, transports, and root paths, **without any host:port configured**. The button-relay demo from week 2 now uses discovered addresses.

### Deliverables
- `libtracer/modules/discovery_mdns/` — new directory:
  - `discovery_mdns.h` / `discovery_mdns.c` — uses an existing mDNS library on Linux (Avahi or a small embeddable one — pick a vendored single-file lib like `mjansson/mdns`); on ESP32 uses the ESP-IDF mDNS component.
  - Service type: `_libtracer._tcp.local` for TCP-based nodes; `_libtracer._udp.local` reserved for week-2's-not-yet UDP module.
  - TXT records: `version`, `node-name`, `root-path`, optional `transports` list.
- `libtracer/core/discovery.h` — generic discovery-module ABI (the same shape every discovery module implements).
- `libtracer/modules/discovery_static/` — fallback for environments without multicast (e.g., Docker without bridge). Reads a TOML or JSON config file with peer addresses.
- `examples/03_discovery.c` — demo above.

### Definition of done
- Plugging a third node into the LAN appears in the others' discovery tables within 5 seconds.
- A node restart triggers re-announcement; stale entries time out within 30 seconds.

### Risks this week
- **R3.1**: ESP-IDF mDNS and the Linux mDNS library may interpret edge cases differently (unicast queries vs multicast, IPv6 link-local). Resolution: stick to the IPv4 multicast happy path; document the corners as "non-goals for v0.1."
- **R3.2**: container/cloud environments where multicast doesn't work — that's why `discovery_static` is shipped in the same week, not deferred.

---

## Week 4 — C ABI extraction + size sentinel

### Goal
The pure-C23 core compiles cleanly without C++; a Cortex-M3-class build hits the ≤ 16 KB sentinel.

### Demo
- Run `arm-none-eabi-gcc -std=c23 -mcpu=cortex-m3 -Os -ffunction-sections -fdata-sections -Wl,--gc-sections -nostdlib -c libtracer/core/*.c` and link the resulting objects into a static archive. `arm-none-eabi-size libtracer-core.a` reports total `.text + .data + .rodata` ≤ 16 KB.
- Run the existing test suite against the C ABI from a C++ host program, proving the boundary is clean.

### Deliverables
- Verified `extern "C"` / pure-C compatibility for every header in `libtracer/core/`.
- `libtracer/cpp/` is now strictly a wrapper over the C ABI — no shared headers, no C++ in the core.
- `cmake -DLIBTRACER_PURE_C=ON` build option that excludes the C++ wrapper entirely.
- `tests/test_c_abi.c` — exercises the full read/write/subscribe flow from pure C.
- `tests/size_sentinel.sh` — script that runs the cross-compile, prints the size, and exits non-zero if > 16 KB. Wired into CI.
- README footnote with the size sentinel result for v0.1.

### Definition of done
- Size sentinel passes on Cortex-M3 (and as a stretch, Cortex-M0+ in `LIBTRACER_NO_ATOMIC` mode — same target, atomic-free codepath).
- C++ wrapper still compiles and tests pass via the C ABI.

### Risks this week
- **R4.1**: discovery and transport modules pull in OS-specific headers that bloat the core. Resolution: the size sentinel is on **core only, no modules**. That's the differentiator; modules can be larger.
- **R4.2**: `libtracer_core.a` linked but no entry point — the size measurement is `.text + .data + .rodata` summed via `arm-none-eabi-size`, not the size of an executable. Document this in the script.

---

## Week 5 — TypeScript / WASM binding + browser demo

### Goal
A browser tab participates in the libtracer graph as a first-class node.

### Demo
A web page in Chrome (localhost or LAN) shows a button. Clicking it publishes to `/control/button`. The same ESP32 from week 3 has its LED toggle. Round trip browser → WS → libtracer node → ESP32 over Wi-Fi.

### Deliverables
- `libtracer/modules/transport_ws/` — WebSocket transport module (server side). Wraps the TCP module's framing in WebSocket frames (RFC 6455).
- `libtracer/bindings/ts/` — TypeScript binding:
  - `package.json`, `tsconfig.json`.
  - `src/wasm/` — libtracer core + ws-client transport compiled to WASM via `clang --target=wasm32-wasi -O2`.
  - `src/index.ts` — TS API surface (mirror of the C ABI: `read`, `write`, `subscribe`).
  - Loaded via dynamic `import` in a browser context; falls back to Node's WS in Node.
- `examples/05_browser_button/` — index.html + a tiny TS file that calls into the binding.

### Definition of done
- The browser button toggles the ESP32 LED end-to-end, with libtracer-only protocol carrying the message.
- The TS binding's API matches the C ABI verb-for-verb (no JS-flavored convenience wrappers in v0.1).

### Risks this week
- **R5.1**: WASI in browsers is not as standardized as in Node; sockets in browsers must be WebSocket, not TCP. Resolution: the `transport_ws` module on the C side is the bridge; the WASM-in-browser only ever speaks WebSocket to a server-side libtracer node.
- **R5.2**: the WASM binary size is large (libc, libcrypto, etc.). Resolution: use `wasi-libc` minimal build, strip everything, gzip on the wire. Acceptable target: < 100 KB gzipped.

---

## Week 6 — First non-IP transport (CAN or I2C)

### Goal
Prove the graph spans bus and IP transports under one address space. This is the **mid-roadmap re-evaluation point** — if this demo is unconvincing, fold the project per [doc 00](00-vision-and-reality-check.md).

### Demo
**Cross-bus subscription.** STM32F4 with a CAN transceiver publishes a sensor reading every 10 ms onto its local libtracer graph at `/local/wheel-encoder`. A USB-CAN dongle on the Linux laptop runs a libtracer node with `transport_can` and `transport_tcp` both loaded; it acts as a bridge — `/local/wheel-encoder` from the STM32 appears at `/can-bridge/wheel-encoder` on the Linux node. An ESP32 over Wi-Fi subscribes to `/can-bridge/wheel-encoder` (path resolved via mDNS to the Linux laptop's TCP transport) and prints values.

### Deliverables
- `libtracer/modules/transport_can/` — uses SocketCAN on Linux, the STM32 HAL CAN driver on the embedded side. Hand-written framing because CAN frames are ≤ 8 bytes (CAN-FD ≤ 64 bytes); a TLV will span multiple frames.
- The bridge mechanism in core gets exercised end-to-end for the first time.
- `examples/06_can_bridge/` — three programs (STM32 firmware, Linux bridge, ESP32 firmware) plus a brief wiring diagram.

### Definition of done
- The full chain runs for 5 minutes without dropping samples (modulo CAN bus errors, which surface as `STATUS` TLVs to the subscriber).
- The subscriber sees no metadata that betrays which transport carried any given sample. Path is the same; payload is the same; no per-transport flags surface.

### Risks this week
- **R6.1**: CAN-FD is needed for reasonable TLV throughput; classic CAN at 1 Mbps maxes around 100 messages/sec for the libtracer wire framing overhead. Document this; offer CAN-FD config option.
- **R6.2**: hardware availability — the user must have a CAN dongle and an STM32 with CAN. If not, fall back to two ESP32s connected via UART, which proves the same point with `transport_uart` instead. Document the fallback in the demo.
- **R6.3**: this week is **the worth-it gate.** If the bridge story is ugly or fragile, don't paper over it — re-open [doc 00](00-vision-and-reality-check.md) verdict.

---

## Week 7 — Executor module v0 (C callback)

### Goal
A vertex can run user logic that transforms incoming TLVs and republishes.

### Demo
On the Linux laptop, register a C callback on `/sensor/temp` that converts incoming Celsius readings into Fahrenheit and republishes to `/sensor/temp_f`. A subscriber on `/sensor/temp_f` (anywhere in the graph) sees Fahrenheit values without knowing the transformation happened.

### Deliverables
- `libtracer/modules/executor_c/`:
  - `executor_c.h` / `executor_c.c` — registers callbacks of signature `tlv_t * (*fn)(const tlv_t *in, void *user)` on a vertex; the return value is the new TLV to publish (NULL = drop).
  - Lifetime: the callback owns the input TLV during the call; ownership of the returned TLV transfers to the executor, which publishes it via `tracer_write`.
- `examples/07_celsius_to_fahrenheit.c` — the demo.
- A second example showing **filter** semantics: a callback that returns NULL for samples outside a configured range.

### Definition of done
- The demo runs; subscribers on `/sensor/temp_f` see correct values.
- The callback can be unregistered and re-registered cleanly without crashing.

### Risks this week
- **R7.1**: ownership semantics around the input/output TLVs are easy to get wrong in C. Resolution: explicit doc on who refcounts what, and a Valgrind / ASan run as part of CI.
- **R7.2**: scope creep into MicroPython / Lua / WASM executor modules. **Hold the line — those are post-MVP.** Week 7 ships only `executor_c`.

---

## Week 8 — Benchmark + diagnostic CLI tool

### Goal
Honest, reproducible numbers against Zenoh and NNG on the three workloads from [doc 01](01-comparison-to-existing-protocols.md). A CLI tool to introspect a live graph.

### Demo
- `bench/run_w1_w2_w3.sh` — produces a CSV of p50, p99, p99.9, throughput, CPU%, RSS for libtracer / Zenoh / NNG on each of W1 / W2 / W3.
- `tools/tracer-top` — terminal UI (ncurses or just ANSI) showing live: vertex list, edge list, sample rate per edge, per-subscriber QoS state. Updates every 500 ms. Connect to a libtracer node like `tracer-top --connect node-name.local`.

### Deliverables
- `bench/` — benchmark harness:
  - `bench_libtracer.c`, `bench_zenoh.c`, `bench_nng.c` — three identical-shape programs differing only in the messaging library.
  - `run_w1_w2_w3.sh` — wraps them with `taskset`, warmup, output to CSV.
  - `analyze.py` — produces a Markdown table from the CSV.
- `tools/tracer-top/` — the CLI tool. Connects via TCP transport.
- `docs/benchmarks-v0.1.md` — published numbers, harness description, machine specs (CPU model, kernel version, OS, library versions). Honest disclaimers for the loopback-vs-LAN distinction.

### Definition of done
- Numbers exist, are reproducible (running the script twice produces ±5% results), are committed to the repo.
- `tracer-top` works against the week-3 mDNS demo (3 nodes visible, edges between them shown live).
- [doc 00](00-vision-and-reality-check.md)'s verdict is held accountable: the five differentiators each have evidence (per the table in doc 00 §[Five Differentiators](00-vision-and-reality-check.md#five-differentiators-that-justify-existing)).

### Risks this week
- **R8.1**: a fair benchmark is genuinely hard. The first numbers may show libtracer losing in places we expected to win. Resolution: publish them anyway. The doc trail makes the project honest, not pretty.
- **R8.2**: web GUI ambition crept back in. **Don't.** CLI `tracer-top` is the deliverable; the web GUI is post-MVP and lives in [doc 06](06-modules-executor-security-gui.md).

---

## Explicit non-deliverables for the 8 weeks

To prevent scope creep, the following are **named and deferred** (not silently dropped):

- **FPGA executor** — aspiration only, see [doc 06](06-modules-executor-security-gui.md).
- **RDMA / UCX / libfabric / NCCL plugin** — control-plane vs data-plane distinction handled in [doc 00](00-vision-and-reality-check.md); a future `transport_rdma` module sketched in [doc 05](05-modules-transport-and-discovery.md).
- **CRDT or Raft-style consensus** — out of scope per [doc 04](04-graph-and-endpoint-api.md). Last-write-wins by timestamp.
- **Noise Protocol Framework** — sketched in [doc 06](06-modules-executor-security-gui.md), not implemented in v0.1.
- **Behavior trees or behavior-tree replacement** — explicit non-goal.
- **Multi-language bindings beyond C / C++ / TypeScript** — Python, Go, Rust, Java are post-MVP.
- **Full DDS QoS surface** — 5 of 22 policies in v0.1; the rest are post-MVP, see [doc 04](04-graph-and-endpoint-api.md).
- **Web GUI diagnostic tool** — CLI `tracer-top` ships; web is post-MVP, see [doc 06](06-modules-executor-security-gui.md).
- **MicroPython / Lua / WASM executor modules** — `executor_c` ships; the others are sketched in [doc 06](06-modules-executor-security-gui.md), not implemented.
- **Schema / IDL language** — no `.tlv` / `.proto` style schema files in v0.1. Endpoint schemas are discoverable at runtime via `ep:schema`. A real IDL is post-MVP.
- **`transport_quic`, `transport_shm`, `transport_ble_gatt`** — listed in doc 05's catalog as future modules.

---

## Mid-roadmap pivot triggers

If by **end of week 6** any of the following are true, do not proceed to weeks 7–8 as planned. Re-open [doc 00](00-vision-and-reality-check.md).

- The cross-bus bridge demo (week 6) requires special-cased glue for CAN that violates the transport-module ABI (week 2). → Means the abstraction is wrong; redesign before continuing.
- The size sentinel (week 4) failed even after optimization. → Means the modular-core target is wrong; either re-scope or fold.
- The mDNS discovery (week 3) is unreliable enough that the cross-bus demo can't depend on it. → Probably non-fatal; switch to static config for the demo and defer the mDNS polish.
- The user has not had any other potential consumer or contributor look at the project. → Means the bus-factor risk (R2 in doc 00) has not been mitigated; surface it explicitly in the week-6 review.

---

## What's NOT in this doc

- Week-by-week wire format details — only the freeze date is here; bytes are in [doc 03](03-wire-format-and-data-model.md).
- API code samples — see [doc 04](04-graph-and-endpoint-api.md).
- Module ABIs — see [doc 05](05-modules-transport-and-discovery.md).
- Risk register (cross-cutting risks live in [doc 00](00-vision-and-reality-check.md); only week-local risks live here).
- Anything beyond week 8. Post-MVP planning happens after the week-8 retrospective, not before.
