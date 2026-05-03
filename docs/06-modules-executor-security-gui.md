# 06 — Executor, Security, and GUI Modules

> **Status**: draft, v0.1, 2026-05-03 — only `executor_c` and the CLI `tracer-top` ship in v0.1; the rest are named, sketched, and deferred.
> **Audience**: anyone wanting to add per-vertex logic, secure their network, or introspect a live graph.
> **Reading time**: ~15 min.

---

## How to read this doc

§[Executor modules](#executor-modules) is the per-vertex-logic story. §[Security modules](#security-modules) is the opt-in encryption / authentication story. §[GUI / introspection](#gui--introspection) is `tracer-top` and the future web GUI. §[What deploys, what doesn't](#what-deploys-what-doesnt) draws the safety line for "deploy logic over the network."

None of these modules are core. A node can run with zero of them and still participate in a libtracer graph.

---

## Executor modules

An **executor** binds user logic to a vertex. When a write arrives at the vertex, the executor's logic transforms it (or filters it, or aggregates it, or republishes it elsewhere). The vertex acts as a "function" in the graph.

### `executor_c` — week 7 MVP

The v0.1 baseline. Register a C callback on a vertex.

```c
#include <libtracer/modules/executor_c.h>

// Signature: takes the input TLV (ownership: read-only borrow during call),
// returns a new TLV to publish (ownership: transfers to executor).
// Return NULL to drop the message (filter behavior).
static tlv_t *celsius_to_fahrenheit(const tlv_t *in, void *user) {
    int32_t   c;
    memcpy(&c, tlv_payload(in), sizeof c);
    int32_t   f = c * 9 / 5 + 32;
    return tlv_new_value(&f, sizeof f);
}

int main(void) {
    tracer_init(NULL);

    executor_c_register("/sensor/temp_c",        // input vertex
                        "/sensor/temp_f",        // output vertex (NULL = in-place)
                        celsius_to_fahrenheit,
                        NULL);                   // user pointer

    pause();
}
```

**Properties**:
- Runs on the libtracer worker thread by default (or a per-callback thread if `executor_c_register_threaded` is used).
- Callback may be re-entered concurrently for different inputs unless registered with `_threaded` (which serializes per callback).
- Lifetime of the input TLV: valid only during the call. To keep it across calls, `tlv_acquire()` it.
- Lifetime of the returned TLV: ownership transfers to the executor; the executor publishes it via `tracer_write` and `tlv_release`s.

**When to use**: every v0.1 use case. C callback is the lowest-friction extension point.

**When to avoid**: you want to ship logic over the wire (you can't — see §[What deploys, what doesn't](#what-deploys-what-doesnt)).

### `executor_micropython` — post-MVP

Embed MicroPython on ESP32 / similar; callback is a Python function.

```python
import libtracer

def celsius_to_fahrenheit(in_tlv):
    c = in_tlv.value()
    return libtracer.value(c * 9 // 5 + 32)

libtracer.executor.register("/sensor/temp_c",
                            "/sensor/temp_f",
                            celsius_to_fahrenheit)
```

**Status**: stretch goal, not a v0.1 commitment. Adds ~100 KB flash on ESP32. Useful when over-the-air *function update* is desirable; risky for the same reason — see [§What deploys, what doesn't](#what-deploys-what-doesnt).

### `executor_python` — post-MVP

CPython on Linux. Same shape as MicroPython, but full Python ecosystem (numpy, pandas, scikit-learn for in-process feature computation).

### `executor_lua` — post-MVP

Alternative for embedded; smaller footprint than MicroPython (~50 KB), simpler language. Useful if MicroPython is too heavy and the user is willing to learn Lua.

### `executor_wasm` — post-MVP

Load a WASM module via WAMR (WebAssembly Micro Runtime). The module exports a function with the C callback signature; the executor invokes it.

```c
executor_wasm_load("/sensor/temp_c",
                   "/sensor/temp_f",
                   "control_law.wasm");
```

**Properties**:
- Sandboxed: WASM module cannot syscall, cannot access libtracer state outside what the host explicitly imports.
- Portable: same `.wasm` file runs on Linux, MCU (with WAMR), and browser (with libtracer's WASM binding from week 5 of [doc 02](02-roadmap-weeks-1-to-8.md) — though running WASM-in-WASM is silly).
- Update-safe: a misbehaving module can be unloaded; it cannot brick the host.

**Status**: post-MVP. Makes "deploy logic over the wire" plausible (see [§What deploys](#what-deploys-what-doesnt)) but is not a v0.1 commitment.

### Behavior trees — explicit non-goal

The user expressed dislike for behavior trees as conventionally implemented; libtracer agrees and does not bundle one.

A **directed acyclic dataflow graph with explicit triggers** would be the more general structure (vertices = nodes, edges = data deps, scheduler walks the DAG when sources update). Prior art: Apache Beam, GNU Radio, NumPy compute graphs, Drake's diagrams. **Out of scope for v0.1.** A future module `executor_dataflow` could compose multiple `executor_c` / `executor_wasm` callbacks into a scheduled DAG, but the spec for it isn't designed.

### FPGA executor — aspirational

The user's vision includes "FPGA math executor for ultra-low latency." A hypothetical `executor_fpga` module would:
- Compile a callback specification (similar shape to `executor_wasm` import) into FPGA bitstream via Vitis HLS or similar.
- Wire FPGA fabric to receive TLV payloads via PCIe DMA or AXI-Stream.
- Reach single-digit-µs latency for fixed-point math.

This is a research project, not a roadmap item. Mentioned here so the maintainer can point at it if asked. Not in v0.1, not in v0.2.

---

## Security modules

Security in libtracer is **opt-in per transport**. The core does not assume crypto; a node running only loopback or a trusted lab network can skip security entirely.

### `security_tls` — post-MVP for v0.1

Wraps `transport_tcp` with TLS 1.3. Backend: mbedTLS (small, portable, MCU-friendly).

```toml
[[transports]]
module = "transport_tcp"
bind   = "0.0.0.0:7700"

[[security]]
module    = "security_tls"
applies   = "transport_tcp"
cert_file = "/etc/libtracer/cert.pem"
key_file  = "/etc/libtracer/key.pem"
ca_file   = "/etc/libtracer/ca.pem"
verify    = "mutual"
```

**Status**: not in v0.1; first security module that's likely to ship post-MVP. Until then, TCP is plaintext — fine for trusted lab networks, **not fine for the open Internet.**

### `security_dtls` — post-MVP

Wraps `transport_udp` with DTLS 1.3. Same backend as `security_tls`.

### `security_psk` — post-MVP

Pre-shared key for `transport_can`, `transport_i2c`, `transport_uart`. Asymmetric crypto is too expensive on these buses; PSK + AES-CCM at 128-bit gives reasonable confidentiality and integrity within the constraints.

```toml
[[security]]
module  = "security_psk"
applies = "transport_can"
psk     = "<hex-encoded 16 byte key>"
```

### `security_acl` — post-MVP, sketched

Capability-based access control. Each vertex has an `:acl` field (a TLV of type ACL from [doc 03](03-wire-format-and-data-model.md)) listing who can read/write/subscribe. The `security_acl` module enforces the ACL on every operation.

ACL entries are capabilities — opaque tokens issued by an authority and presented in subscription requests. v0.1 does not have an authority service; ACL is **structurally defined but unenforced** in the core. The module that enforces it ships post-MVP.

```
acl := LIST of capability
capability := LIST of {
  subject     :  NAME (the holder of this capability)
  permissions :  u8   (bitfield: READ=0x1, WRITE=0x2, SUBSCRIBE=0x4)
  expires_ns  :  u64
}
```

### `security_noise` — sketched, post-MVP

Noise Protocol Framework (the Wireguard cryptographic core). Higher security than TLS for some patterns, smaller code, but no certificate ecosystem. Post-MVP.

### "No security" is a valid build

For trusted segments (a single Linux box with libtracer over Unix domain sockets, or a closed lab network), running zero security modules is the correct choice. The wire format is security-agnostic; security wraps it at the transport.

---

## GUI / introspection

### `tracer-top` — week 8 MVP

A terminal-UI tool that connects to a libtracer node and shows the graph live.

```
$ tracer-top --connect lab-bridge.local:7700

libtracer top — node: lab-bridge-1
Discovered peers: 3 (esp32-front, stm32-wheel, laptop-dev)
Local time: 2026-05-03 17:42:11   PTP-synced: yes (offset 4 µs)

Vertices (12)                                           Subs   In/s   Out/s
/sensor/temp                                              2     1.0    2.0
/sensor/humidity                                          1     1.0    1.0
/can-bridge/wheel-encoder/left                            1   100.0  100.0
/can-bridge/wheel-encoder/right                           1   100.0  100.0
/can-bridge/imu/accel                                     2   500.0 1000.0
/log/output                                               0     5.0    0.0
[Esc to quit] [s sort] [/ filter] [Enter inspect]
```

**Implementation**: pure C, ncurses or a tiny ANSI escape library. Walks the live graph by reading `:schema` and `:subscribers[]` on each vertex. Updates every 500 ms via subscription to `/.../*:liveness.last_seen_ns` (cheap because it's read-only).

**Connect**: requires a TCP transport on the target node. Authenticates via `security_tls` if loaded (post-MVP) or anonymously (v0.1).

**Filter**: filter expression supports glob-like wildcards on path (`/sensor/*`, `/can-bridge/**`).

**Inspect**: pressing Enter on a row shows the most recent N samples (configurable), the full schema, and the subscriber list for that vertex.

### Web GUI — post-MVP

A web-based diagnostic tool, served by a libtracer node with both `transport_ws` and a static-asset module. Same data as `tracer-top`, but with:
- Live D3-based graph visualization.
- Click-to-subscribe on any vertex; payload preview pane.
- Recording / playback (records subscriptions to a file, replays from file as if live).

**Status**: post-MVP. The CLI ships first because it's the smaller, more reliable answer.

### eCAL Monitor parity — explicit non-goal for v0.1

eCAL has a polished introspection ecosystem (Monitor, Recorder, Player, Sys). libtracer does not aim to replicate it in v0.1. `tracer-top` covers the live-introspection 80%; recording/playback is post-MVP; orchestration is out of scope.

---

## What deploys, what doesn't

The user's vision includes "deploy logic over the network" (e.g., MATLAB-derived control laws pushed to devices). This must be made precise to be safe.

| Artifact | Deployable over libtracer? | Mechanism | Safety story |
| ---- | ---- | ---- | ---- |
| **Graph topology** (vertices, subscriptions, QoS) | **Yes** | Write SUBSCRIBER / SETTINGS TLVs to receiving nodes | Declarative; receiving node interprets as data, not code |
| **QoS settings on existing endpoints** | **Yes** | Field-write per [doc 04](04-graph-and-endpoint-api.md) | Same as topology — declarative |
| **Pre-registered named callback IDs** | **Yes** | Receiving node has callbacks indexed by name; remote write to `:callback_id` field selects one | Code is pre-flashed; remote chooses among existing options |
| **Compiled C function** | **No** | n/a | Cannot safely interpret raw machine code; out of scope |
| **WASM module** | **Maybe, post-MVP** | Write the `.wasm` bytes to `executor_wasm`'s reload path | Sandboxed, but still a security surface; opt-in only, requires auth |
| **MicroPython source** | **Maybe, post-MVP** | Push `.py` source to the executor | Less sandboxed than WASM; risk of CPU / memory abuse |
| **MATLAB-generated control law** | **Pre-flashed, then bound by name** | MATLAB → C code → flash; libtracer binds the named callback | Code path stays out of libtracer; libtracer wires data flow |

### The default rule

> **Topology is data. Code is not.**

Topology can flow over the wire freely (it's just TLVs). Code stays pre-flashed and is selected by name. This is the safe default and the one libtracer ships in v0.1.

WASM-over-the-wire is a deliberate post-MVP option for users who explicitly want it and accept the security surface. It is gated, opt-in, and authenticated.

---

## What's NOT in this doc

- The graph / API / QoS — see [doc 04](04-graph-and-endpoint-api.md).
- Transport modules — see [doc 05](05-modules-transport-and-discovery.md).
- The wire format — see [doc 03](03-wire-format-and-data-model.md).
- The roadmap that schedules these modules — see [doc 02](02-roadmap-weeks-1-to-8.md).
- The honest assessment of whether the FPGA executor is real — see [doc 00](00-vision-and-reality-check.md) §risks.
- The full ACL / capability model design — sketched here, designed in a future security-spec doc that doesn't exist yet.
