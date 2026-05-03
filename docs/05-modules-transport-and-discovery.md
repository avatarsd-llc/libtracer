# 05 — Transport and Discovery Modules

> **Status**: draft, v0.1, 2026-05-03 — module ABI is the contract; once frozen at week 2 of [doc 02](02-roadmap-weeks-1-to-8.md), out-of-tree modules can rely on it.
> **Audience**: anyone writing a new transport/discovery module; anyone choosing which transports to load on a given device.
> **Reading time**: ~20 min.

---

## How to read this doc

§[Module ABI](#module-abi) is the contract every transport module exports. §[Transport catalog](#transport-catalog) lists known and planned transport modules with per-transport notes. §[Discovery catalog](#discovery-catalog) lists discovery modules. §[Bridging configuration](#bridging-configuration) is how multi-transport nodes are wired up.

---

## Module ABI

A transport module is a separately-compiled translation unit (or shared object) that exports a small, stable C interface. The core loads it either as a static library at link time or as a dlopen'd `.so` at runtime (`LIBTRACER_DYNAMIC_MODULES=ON`).

### The header

```c
// libtracer/core/transport_iface.h

#include <libtracer/core/tlv.h>
#include <stddef.h>
#include <stdint.h>

typedef struct transport_module      transport_module_t;
typedef struct transport_endpoint    transport_endpoint_t;

// One-line description of the module, exported as a constant.
typedef struct {
    const char *name;          // e.g. "transport_tcp"
    const char *version;       // semver of the module
    uint32_t    abi_version;   // bumped when this struct's shape changes
} transport_meta_t;

// Configuration for module init, opaque to the core. Each module
// defines its own struct and casts in its `init` impl.
typedef struct {
    const transport_meta_t *meta;
    void                   *config;   // module-specific, may be NULL
} transport_init_args_t;

// Callback the transport module calls when it has received bytes
// the core should treat as one TLV. Ownership of `tlv` transfers
// to the core; module must not touch `tlv` after this returns.
typedef void (*transport_recv_cb_t)(transport_module_t *self,
                                    tlv_t              *tlv,
                                    const char         *peer_id,
                                    void               *user);

// The vtable a module exports.
typedef struct {
    int (*init)   (transport_module_t **out, const transport_init_args_t *args,
                   transport_recv_cb_t cb, void *cb_user);

    // Bind to a local address (e.g., listen on TCP port; open SocketCAN).
    int (*bind)   (transport_module_t *self, const char *addr_uri);

    // Connect/dial to a remote peer; returns endpoint handle or NULL.
    transport_endpoint_t *(*connect)(transport_module_t *self,
                                     const char *peer_uri);

    // Send one TLV on the given endpoint. Module is responsible for
    // any wire-level fragmentation. Returns 0 on success; non-zero
    // error code on failure (see doc 03 error codes).
    int (*send)   (transport_module_t *self, transport_endpoint_t *ep,
                   const tlv_t *tlv);

    // Hint about the largest TLV the underlying medium prefers.
    // Caller (the publisher) MAY use this to apply address-shift
    // slicing (doc 04). Returning 0 means "no preference."
    size_t (*mtu_hint)(transport_module_t *self, transport_endpoint_t *ep);

    // Per-tick poll for receive. Called by the core's run loop;
    // module dispatches received TLVs via the recv callback.
    // Should be non-blocking. Returns >0 if work was done.
    int (*poll)   (transport_module_t *self, uint32_t timeout_ns);

    // Close one endpoint without unloading the module.
    void (*close) (transport_module_t *self, transport_endpoint_t *ep);

    // Tear down the module entirely.
    void (*shutdown)(transport_module_t *self);
} transport_vtable_t;

// Each module exports one of these as `libtracer_transport_<NAME>`.
extern const transport_vtable_t  libtracer_transport_tcp;
extern const transport_vtable_t  libtracer_transport_udp;
extern const transport_vtable_t  libtracer_transport_can;
// ... etc.
```

### ABI versioning

The `abi_version` in `transport_meta_t` is bumped any time the `transport_vtable_t` shape changes. The core checks `abi_version` at module load and refuses incompatible modules with a clear error. Out-of-tree modules built against ABI version N continue to work with cores at ABI version N indefinitely; ABI version N+1 requires a rebuild.

### Notes on the ABI

- **`bind` vs `connect`**: not every transport has both. `transport_can` has `bind` (open SocketCAN, then everyone on the bus is a peer) but no meaningful `connect`. `transport_tcp` has both. Modules may return `ERR_NOT_SUPPORTED` for operations they don't model.
- **The recv callback runs on the module's poll thread** (or the IRQ context, on bare-metal — modules document their callback context). The callback MUST NOT call back into the transport's `send`/`bind`/`close` directly (recursion); it queues to the core's dispatch loop.
- **`poll` is the universal idle.** A module without an OS-thread does its work here. A module with its own thread can return immediately.
- **`mtu_hint` is advisory.** The publisher uses it for address-shift slicing decisions; it is not a hard limit. The transport's `send` MUST handle TLVs larger than `mtu_hint` (doing wire-level segmentation internally).

---

## Transport catalog

Each entry: status, footprint, when to use, when to avoid.

### `transport_tcp` — week 2 MVP

- **Status**: ships in v0.1.
- **Footprint**: ~5 KB code on Linux, ~8 KB on Cortex-M (lwIP-dependent).
- **When to use**: default for LAN connectivity between Linux/MCU nodes; the foundation for `transport_ws`.
- **When to avoid**: WAN with NAT (use `transport_quic` post-MVP); strict-latency control loops where TCP retransmit jitter is unacceptable (use `transport_udp` + reliable QoS).
- **MTU hint**: 65535. TCP framing handles arbitrary sizes.
- **Notes**: blocking accept/read in v0.1; epoll/kqueue/IOCP post-MVP.

### `transport_udp` — week 2 stretch / week 5 likely

- **Status**: planned in v0.1 if time allows; otherwise post-MVP.
- **Footprint**: ~3 KB code on Linux, ~5 KB on Cortex-M.
- **When to use**: latency-sensitive flows where TCP retransmit jitter dominates; multicast pub/sub on LAN.
- **When to avoid**: large payloads — use address-shift slicing (doc 04) since UDP IP-fragmentation is widely broken across firewalls.
- **MTU hint**: 1472 (Ethernet payload − IP header − UDP header).
- **Reliability mode**: requires app-layer ack/nack + sequence numbers (post-MVP for `RELIABILITY=reliable`).

### `transport_quic` — post-MVP

- **Status**: post-MVP. Sketched here.
- **Footprint**: ~100 KB code (msquic, ngtcp2, or quiche backend).
- **When to use**: WAN connectivity; multiplexed streams over a single connection; built-in TLS.
- **When to avoid**: MCU targets — too large.
- **Notes**: probably wraps msquic on Linux/Windows, ngtcp2 on Linux, quiche (Rust) on host. ESP32 QUIC is immature.

### `transport_ws` — week 5 MVP (browser binding)

- **Status**: ships in v0.1.
- **Footprint**: ~3 KB on top of `transport_tcp` (just RFC 6455 framing).
- **When to use**: browser ↔ libtracer node connectivity; environments where only WebSocket is allowed (corporate proxies).
- **When to avoid**: server-to-server; use raw TCP for less framing overhead.
- **MTU hint**: same as TCP (65535).
- **Notes**: WSS (WebSocket-over-TLS) gated on `security_tls` module; without it, WS is plaintext.

### `transport_unix` — likely v0.1 (cheap)

- **Status**: planned for v0.1.
- **Footprint**: ~2 KB on top of `transport_tcp` (very similar code path).
- **When to use**: intra-host process-to-process on Linux/macOS, where SHM is overkill.
- **When to avoid**: cross-host (use TCP).
- **MTU hint**: 65535.

### `transport_shm` — post-MVP

- **Status**: post-MVP. Iceoryx2-style intra-host zero-copy.
- **Footprint**: ~30 KB on Linux (mmap, refcounted ringbuffer, futex-based notification).
- **When to use**: high-throughput intra-host communication where iceoryx2 is overkill or unavailable.
- **When to avoid**: serious intra-host zero-copy on Linux — **use iceoryx2 directly**, not libtracer's SHM.
- **Notes**: this module's existence is contingent on someone needing it; if iceoryx2 covers the case, this module may stay deferred.

### `transport_can` — week 6 MVP

- **Status**: ships in v0.1.
- **Footprint**: ~5 KB on Linux (SocketCAN), ~8 KB on STM32 (HAL CAN driver).
- **When to use**: connecting MCUs over CAN/CAN-FD; classic automotive / robotics use case.
- **When to avoid**: payloads > a few hundred bytes — CAN's 8-byte frames make TLV framing chatty.
- **MTU hint**: 8 (classic CAN) or 64 (CAN-FD).
- **Notes**: TLVs span multiple CAN frames using a sequence-number convention in the CAN ID's low bits. Module documents the framing convention so foreign devices can decode.

### `transport_i2c`, `transport_spi`, `transport_uart` — pulled in by need

- **Status**: per-need v0.1 deliverables.
- **Footprint**: 2–4 KB each.
- **When to use**: connecting a peripheral or co-processor over the named bus; **as a vertex tree** (see [doc 04](04-graph-and-endpoint-api.md) §I2C as vertex tree).
- **When to avoid**: anywhere a more capable transport exists — these are point-to-point (UART) or single-master (I2C, SPI) and don't scale to graph.
- **MTU hint**: device-dependent; module configures from the device's datasheet.
- **Notes**: peripheral-as-vertex-tree pattern is per-driver. The module exposes the bus; per-peripheral vertex layout is a thin wrapper module on top (e.g., `peripheral_mpu6050` over `transport_i2c`).

### `transport_ble_gatt` — sketched only

- **Status**: post-MVP, sketched.
- **Footprint**: ~50 KB (NimBLE or BlueZ).
- **When to use**: wearable / mobile peripherals.
- **When to avoid**: anywhere with Wi-Fi.
- **Notes**: GATT characteristic per vertex; libtracer paths map to characteristic UUIDs via a reserved namespace.

### `transport_rdma` — post-MVP, aspirational

- **Status**: post-MVP. Sketched here so it has a name.
- **Footprint**: large; depends on libfabric, UCX, or raw verbs.
- **When to use**: HPC-class data plane between Linux servers with InfiniBand or RoCE; libtracer becomes the **control plane** that negotiates the queue pair, then steps out of the data path.
- **When to avoid**: anywhere not in an HPC cluster.
- **Notes**: this module is the materialization of the user's "RDMA / NCCL" vision. It is firmly post-MVP — see [doc 00](00-vision-and-reality-check.md) §control-vs-data-plane.

---

## Discovery catalog

Discovery is a separate module type with its own ABI (similar shape to transport — `init`, `start`, `stop`, callback for "found a peer").

### `discovery_mdns` — week 3 MVP

- **Status**: ships in v0.1.
- **Footprint**: ~10 KB (mjansson/mdns library or Avahi client) on Linux; ~20 KB ESP-IDF mDNS component on ESP32.
- **When to use**: LAN auto-discovery, IPv4 multicast available.
- **When to avoid**: container/cloud/Wi-Fi-isolated networks where multicast doesn't traverse.
- **Service type**: `_libtracer._tcp.local`, `_libtracer._udp.local`.
- **TXT records**: `version`, `node-name`, `root-path`, `transports`.

### `discovery_static` — week 3 MVP

- **Status**: ships in v0.1.
- **Footprint**: ~2 KB.
- **When to use**: anywhere mDNS doesn't work (containers, cloud, restrictive networks).
- **Format**: TOML or JSON file listing peers (URI + transport hint).

### `discovery_gossip` — post-MVP

- **Status**: post-MVP, sketched.
- **Footprint**: ~15 KB.
- **When to use**: WAN-scale, where mDNS doesn't and static config is unwieldy.
- **Notes**: SWIM-style gossip (membership + failure detection); seed nodes bootstrap the cluster.

### `discovery_dns_sd` — post-MVP

- **Status**: post-MVP.
- **Notes**: DNS-SD (RFC 6763) over unicast DNS; useful for cloud environments with internal DNS.

---

## Bridging configuration

Bridges (per [doc 04](04-graph-and-endpoint-api.md)) are a core mechanism, but their **configuration** lives in the node's startup config. Reference TOML format (final at week 3 of [doc 02](02-roadmap-weeks-1-to-8.md)):

```toml
[node]
name        = "linux-bridge-1"
description = "Wi-Fi to CAN bridge in the lab"

# Loaded transport modules. Order matters only for tie-break.
[[transports]]
module  = "transport_tcp"
bind    = "0.0.0.0:7700"

[[transports]]
module  = "transport_can"
device  = "can0"
bitrate = 1000000

[[transports]]
module  = "transport_uart"
device  = "/dev/ttyUSB0"
baud    = 115200

# Loaded discovery modules.
[[discovery]]
module = "discovery_mdns"

[[discovery]]
module = "discovery_static"
file   = "/etc/libtracer/peers.toml"

# Bridges. Each bridge re-publishes paths from one transport
# under a mount point on the local graph.
[[bridges]]
mount  = "/can-bridge"
source = "transport_can"
# No filter = republish everything

[[bridges]]
mount  = "/serial-bridge"
source = "transport_uart"
filter = ["/firmware-cli/**"]   # only paths under /firmware-cli/

[[bridges]]
mount  = "/peer/{peer_id}"
source = "transport_tcp"
# {peer_id} is interpolated from the connecting peer's announced node name
# This creates one mount point per remote peer, namespacing their data
```

The `{peer_id}` interpolation is how a node with multiple TCP peers keeps their graphs separated. Without it, two peers publishing `/sensor/temp` would collide on the bridge.

---

## What a transport module's day looks like

A walk-through of the lifecycle, for module authors.

### Init

```c
int my_transport_init(transport_module_t **out,
                      const transport_init_args_t *args,
                      transport_recv_cb_t cb, void *cb_user) {
    my_transport_state_t *state = calloc(1, sizeof *state);
    state->cb       = cb;
    state->cb_user  = cb_user;
    state->config   = args->config;  // module-specific, cast to your type
    // ... set up internal state ...
    *out = (transport_module_t *)state;
    return 0;
}
```

### Bind / connect

The core calls these as the application configures the node. Map "bind" to "open the local listener" (TCP listen, SocketCAN open, UART tty open). Map "connect" to "establish a session with a named peer" (TCP connect, or "subscribe to a peer's broadcasts" semantics for connectionless transports).

### The send path

```c
int my_transport_send(transport_module_t *self,
                      transport_endpoint_t *ep,
                      const tlv_t *tlv) {
    // 1. Compute total wire size (header + length-field + payload).
    size_t       n   = tlv_total_size(tlv);
    const void  *buf = tlv_raw_bytes(tlv);

    // 2. Push to the medium. Handle wire-level segmentation here
    //    (TCP: socket may accept partial; CAN: split into 8-byte frames;
    //     UART: byte stream).
    return my_transport_push(self, ep, buf, n);
}
```

### The receive path

This is where TLV reconstruction happens for stream-based transports:

```c
// Called by the module's reader thread / poll function when bytes arrive.
void my_transport_on_bytes(my_transport_state_t *state,
                           const uint8_t *bytes, size_t n,
                           const char *peer_id) {
    // 1. Append to a per-peer reassembly buffer.
    rxbuf_append(&state->rx[peer_id], bytes, n);

    // 2. Try to parse one or more complete TLVs out of the buffer.
    while (rxbuf_has_complete_tlv(&state->rx[peer_id])) {
        // Construct a TLV view directly over the rxbuf memory.
        // Refcount-bump the rxbuf segment so it survives until the
        // core releases the TLV.
        tlv_t *tlv = rxbuf_extract_tlv(&state->rx[peer_id]);

        // Hand off to the core. Ownership transfers; we don't touch
        // `tlv` after this. The core's dispatcher will route it,
        // possibly to many subscribers; the underlying rxbuf segment
        // stays alive via the refcount until all subscribers release.
        state->cb(self, tlv, peer_id, state->cb_user);
    }
}
```

The **same-substrate insight** from [doc 03](03-wire-format-and-data-model.md) makes this efficient: the TLV view is constructed over the rxbuf bytes, never copied. When the last subscriber releases, the rxbuf segment is freed.

### Shutdown

Close all open endpoints, drain the recv queue, free state.

---

## What's NOT in this doc

- Wire format details — see [doc 03](03-wire-format-and-data-model.md).
- The endpoint / subscription / QoS API — see [doc 04](04-graph-and-endpoint-api.md).
- Executor / security / GUI modules — see [doc 06](06-modules-executor-security-gui.md).
- The MCU vs Linux trade-offs — see [doc 00](00-vision-and-reality-check.md) and [doc 02](02-roadmap-weeks-1-to-8.md) for footprint targets.
- An exhaustive QoS-to-transport mapping table — that's in [doc 04](04-graph-and-endpoint-api.md).
