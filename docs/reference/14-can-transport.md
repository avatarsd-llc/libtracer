# Reference 14 — CAN transport: header-elided framing, a structured 29-bit ID, and self-healing in-band advertise

```{admonition} In one paragraph
:class: tip
CAN is a **header-elided** transport ([ADR-0022](../adr/0022-transport-framing-modes-elided-full-tlv-advertise.md)):
the CAN frame's **native identity — its 29-bit ID — *is* the path**, so the 4-byte
TLV header never rides the constrained bus and the existing CAN frames are
byte-unchanged (zero added overhead — the constraint that makes 100 ksps-over-CAN
feasible). The ID is **structured** — `[protocol-version prefix | node | endpoint]`
— and because **lower numeric ID = higher bus arbitration priority**, assigning
the ID *also* assigns real-time priority. A payload larger than one frame (8 bytes
classic, up to 64 CAN-FD) reassembles via libtracer's own **address-shift slicing /
advertise+id-match** keyed by `(origin, ts) + index → rope`, **not** ISO-TP. The
`identity↔path` map lives **inside `transport_can`**, is **dynamic**, and
**self-establishes decentrally** from in-band **advertise** frames on (re)connect —
there is no gateway or orchestrator role ([ADR-0030](../adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md),
[13-network-formation](13-network-formation.md) §self-healing).
```

This document describes both the CAN transport's **pure framing layer** (the
host-testable, syscall-free codecs) and — as of increment 2 of
[#55](https://github.com/avatarsd-llc/libtracer/issues/55) — the **SocketCAN
binding** that wires that framing to a live Linux CAN bus (the `transport_can :
transport_t` driving a real `PF_CAN` socket). The §[SocketCAN binding](#the-socketcan-binding-transport_can)
section below covers the binding; everything before it is the pure layer it builds on.

The reference-implementation symbols are:

| Concern | Symbol | Header | Layer |
| --- | --- | --- | --- |
| 29-bit ID + advertise codec | `tr::net::can` | `can.hpp` | transport plane |
| header-elided framing | `tr::view::view_can_frames_t` | `view_can.hpp` | L1 |
| multi-frame reassembly | `tr::net::can_reassembly_t` | `can_reassembly.hpp` | net |
| **SocketCAN binding + raw-frame seam** | **`tr::net::transport_can`, `can_link_t`, `socketcan_link_t`** | **`transport_can.hpp`** | **transport plane** |

## The structured 29-bit extended ID

A CAN 2.0B **extended** frame carries a 29-bit identifier. libtracer gives it three
fields, most-significant first:

```
 bit 28                    bit 0
  └─ version(4) ─ node(13) ─ endpoint(12) ─┘
```

| Field | Width | Range | Meaning |
| --- | --- | --- | --- |
| `version` | 4 | 0–15 | Protocol-version prefix — discovery-layer versioning on CAN (a distinct ID prefix per protocol version, [CONTEXT.md](../../CONTEXT.md) *Discovery-layer versioning*), **not** a per-frame version field. |
| `node` | 13 | 0–8191 | The originating node id. |
| `endpoint` | 12 | 0–4095 | The per-node endpoint slot (the path leaf the map resolves). |

`encode_can_id` / `decode_can_id` are exact inverses for any in-range value; an
input beyond 29 bits decodes to `nullopt`.

```{admonition} Bit widths are a reference-impl modeling choice
:class: note
[ADR-0030](../adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md)
pins the *layout* (`version | node | endpoint`) and the priority semantics but not
the exact widths. The `4 / 13 / 12` split is the reference implementation's choice:
4 version bits cover protocol generations comfortably, while 13/12 balance node
count (8192) against endpoints-per-node (4096). Other deployments may repartition
the lower 25 bits; the version prefix and the priority ordering are the invariants.
```

### ID assignment *is* priority assignment

CAN arbitration is **dominant-bit** — when two nodes transmit simultaneously, the
frame with the **numerically lower ID wins the bus**. Because `node` is more
significant than `endpoint`, a lower node id outranks a higher one regardless of
endpoint, and a lower version prefix outranks everything in a higher one. So the
path→ID assignment the map performs is also a **real-time priority assignment** — a
CAN-specific knob exposed through the identity↔path map, with no side channel.

### Two or more CAN buses on one node

The 29-bit ID deliberately carries **no bus field** — the bus is implicit (it is the
wire the frame arrived on). A node with several CAN controllers (e.g. `can0`, `can1`)
therefore distinguishes them **in the path, not in the ID**: under the path-as-route
model ([RFC-0004](../spec/rfcs/0004-remote-operation-addressing.md), [ADR-0027](../adr/0027-transport-and-connections-are-vertices.md))
**each bus is a named child vertex of the CAN transport**:

```
/net/can/
   ├─ 0/   :settings{ bitrate }  :stats{ bus_off, err_count }  :acl   ← controller can0
   │   └─ <node>/<endpoint…>      ← devices on bus 0, resolved by the 29-bit ID
   └─ 1/   :settings{ … }  :stats{ … }  :acl                          ← controller can1
       └─ <node>/<endpoint…>
```

- The bus identifier (`0`, `1`) is a **`NAME` segment**, not a `[N]` index — `NAME`
  excludes `[` `]`, and the bus is a distinct *identity*, not a slice (segment-`[N]`
  indices stay reserved for address-shift data slicing below, never for bus
  addressing). The default name is the controller index (à la SocketCAN), but it MAY
  be semantic (`/net/can/powertrain`).
- Each bus is its own vertex with independent `:settings` (bitrate), `:stats`
  (bus-off / error counters), and `:acl` — two controllers are two hardware
  identities, exactly the "distinct lifecycle ⇒ `/` vertex" rule of ADR-0027.
- The `identity↔path` map keys on **(which controller the frame arrived on) + (`node`
  | `endpoint`)** → `/net/can/<bus>/…`, so two buses carrying the **same `node` id
  never collide** — the bus segment disambiguates them while the ID stays compact.
- `read("/net/can")` enumerates the buses (vertex enumeration, [reference/04](04-communication-flows.md)),
  so an orchestrator discovers a node's bus count with no special API.

### Address-shift slice IDs

A multi-frame payload is spread across **consecutive endpoint slots** of the same
node — `endpoint[0..N]` — so slice *index* simply **shifts the endpoint sub-field**
(`slice_can_id(base, index)`). The whole group therefore stays in one
`version|node` band and keeps a single arbitration-priority class. This is exactly
[ADR-0011](../adr/0011-address-shift-totality-opt-in.md) address-shift slicing
applied to the CAN ID.

## Framing modes: classic vs CAN-FD

`view_can_frames_t::split(payload, mode)` chops one logical payload (a `view_t`)
into the CAN **data-field windows** that carry it:

| Mode | Max data field | Notes |
| --- | --- | --- |
| `can_frame_mode_t::CLASSIC` | 8 bytes | Classic CAN 2.0. |
| `can_frame_mode_t::FD` | 64 bytes | CAN-FD; valid DLC sizes are 0–8, 12, 16, 20, 24, 32, 48, 64. |

The split is **zero-copy** — each window is a `subview()` over the source segment,
mirroring the existing `view`/`rope` primitives ([08-views-and-ownership](08-views-and-ownership.md));
no payload byte is copied. `to_rope()` chains the windows back into a `rope_t`, the
reassembled payload. A payload that fits one frame yields a single window; a larger
one yields a sequence whose tail window holds the remainder.

On-wire, a CAN-FD frame can only be a valid DLC length, so an in-between window is
padded up to the next legal size — `can_fd_dlc_round_up(len)` exposes that lattice.
The framing windows themselves stay the exact logical chunk lengths (so they remain
zero-copy subviews); applying the DLC padding is the SocketCAN binding's job.

## Multi-frame reassembly — address-shift, not ISO-TP

`can_reassembly_t` reassembles a payload that spanned several CAN frames. Each
frame is a **slice**; slices are grouped by the in-flight identity `(origin, ts)`
(the same collision-free `(origin_peer_id, ts)` used for cycle-dedup and slice
grouping, [CONTEXT.md](../../CONTEXT.md) *Address-shift slicing*) and ordered by
`index`. `assemble()` chains the slices, in index order, into a `rope_t` — zero
copies.

This deliberately reuses libtracer's **one reassembly model** rather than bolting
on **ISO-TP** ([ADR-0030](../adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md)):
the same mechanism that "spans a 9-byte elided CAN sample → a GB advertised rope
group" serves CAN, UDP scatter-gather, and QUIC alike.

**Out-of-order and missing-fragment handling:**

- **Out of order.** Slices may arrive in any order; they are stored by index and
  emitted in ascending order at assembly.
- **Interior gap.** A missing slice *below* the highest received index is detected
  by `has_interior_gap()` even before the count is known.
- **Totality is opt-in.** `set_expected_count()` (the advertise manifest's slice
  count) makes the group `is_complete()` only when every index `0..count-1` is
  present, and makes a dropped **trailing** slice detectable. Without it, a trailing
  drop is invisible — exactly [ADR-0011](../adr/0011-address-shift-totality-opt-in.md)
  totality-opt-in. `assemble()` returns a rope only once the group is complete.

```{admonition} Layer placement of the reassembly buffer
:class: note
`can_reassembly_t` lives in **`tr::net`**, beside `transport_can` (header
`can_reassembly.hpp`). ADR-0030/#55 originally named it for L0 (`tr::mem::mem_can_reassembly_t`),
but that was a self-admitted layer inversion — an L0 type referencing the L1
`rope_t` it assembles. The rehome (ADR-0048 round 2) resolves it: the reassembly
is a transport-plane concern that composes L1 views into a rope, exactly as any
transport does, so no `tr::mem` type reaches up into `tr::view`. Its structure is
drawn from an injected `std::pmr::memory_resource` and the live group count is
bounded by config (evict-oldest + a `dropped_groups` counter), so a constrained
node degrades by a bounded drop rather than unbounded growth.
```

## The in-band advertise frame and the dynamic map

The `identity↔path` map is **dynamic config held inside `transport_can`** — not
static, not held by a privileged node. It self-establishes from in-band
**advertise** frames: an advertise is a full-TLV control frame that *establishes* a
header-elided binding at runtime, mapping a CAN ID to a libtracer path, after which
the **lean, id-matched** data frames carry only payload (the
`discovery_static`/`discovery_mdns`-shaped "full caps sets up non-interactive
bindings" split, [CONTEXT.md](../../CONTEXT.md) *Framing modes*).

`advertise_t` has two forms, distinguished by the `group` flag:

- **Single value** — `id ↔ path`; the lean frames that follow are values.
- **Rope group / manifest** — `group-id ↔ (path, slice structure)`; the advertise
  carries the slice count and total length, and the lean id-matched **slice** frames
  that follow are chained into a rope by id+index. This is the advertise+id-match
  generalization ([CONTEXT.md](../../CONTEXT.md) *Advertise + id-match*), the
  manifest [ADR-0011](../adr/0011-address-shift-totality-opt-in.md) otherwise carries
  statically.

Two further forms serve the [ADR-0044](../adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)
peer plane (both transport-internal framing in the same advertise family):

- **Hello / presence** — `slice_count == 0`: binds nothing and precedes no data;
  it only announces "this node is on the bus" (plus its identity path). Emitted
  once at join; any subsequent frame refreshes liveness.
- **Directed** — `target_node != 0xFFFF`: the group is addressed to ONE peer.
  Every other node recognizes and consumes its data slices but never reassembles
  or delivers them — per-peer unicast semantics on a broadcast medium, which is
  what makes transparent per-peer `FWD` forwarding possible.

On-wire layout (little-endian, an 18-byte header + the path bytes; format `0x02`
widened the v1 header with the explicit `target_node` field):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | magic = `0xAD` |
| 1 | 1 | format version = `0x02` |
| 2 | 1 | flags (`0x01` = group) |
| 3 | 1 | reserved, must be zero |
| 4 | 4 | `can_id` (u32 LE; a 29-bit value) |
| 8 | 4 | `group_total_len` (u32 LE; 0 if single-value) |
| 12 | 2 | `slice_count` (u16 LE; 1 if single-value, 0 = hello) |
| 14 | 2 | `target_node` (u16 LE; `0xFFFF` = undirected broadcast) |
| 16 | 2 | `path_len` (u16 LE) |
| 18 | `path_len` | path bytes (UTF-8 libtracer path) |

`encode_advertise` / `decode_advertise` round-trip this; decode rejects a wrong
magic, an unknown format version, a non-zero reserved byte, or a truncated buffer
(`nullopt` = need more / malformed), with an overflow-safe length check.

### Self-healing (no coordinator)

Because the map lives inside `transport_can` and is rebuilt from advertise frames,
recovery is **local and automatic** ([13-network-formation](13-network-formation.md)
§self-healing): on (re)connect a node **re-announces its own mappings**, so a
rejoining leaf or a downed forwarding hop costs only the paths through it. There is
**no central authority to lose** — the map is never a single point of truth, and no
node holds another node's wiring. A constrained CAN leaf stays dumb (a compile-time
CAN-ID scheme); the map machinery runs in `transport_can` on whatever node hosts it.

### The ws/UDP generalization — the route-handle

CAN's `identity↔path` map is **mandatory** because the ID *is* the path. On a
**full-TLV** transport (ws/UDP) the same idea is **opt-in compaction**: the
**route-handle** ([05-protocol-tlvs.md](05-protocol-tlvs.md) §route-handle, RFC-0004
§E.1, ADR-0035 slice 4) is a per-link **u16 label** that aliases an established
delivery route, advertised in-band exactly as a CAN binding is — but with the label
**swapped each hop** (MPLS-style), since a ws label is meaningful only on its link.
The mechanics mirror this section one-for-one: an `ADVERTISE` frame establishes
`label ↔ route`, lean `COMPACT` frames then carry only the label + value, a stale
label is dropped with a `HANDLE_NACK`, and **re-advertise on (re)connect is the
self-heal**. The difference is policy, not mechanism: CAN always labels (no route
fits in 8 bytes); ws labels **only** flows whose `SUBSCRIBER.qos_settings.delivery_compact`
is set, so a ws node forwarding one-shot/cold traffic holds zero label state. The ws
table lives in `tr::net::route_handle_t`, owned by `fwd_router_t`.

## The SocketCAN binding (`transport_can`)

Increment 2 realizes the binding: `tr::net::transport_can` is a `transport_t` that
drives the framing above over a real Linux CAN bus. A forwarder hands it a complete
libtracer frame via `send()`; the byte-exact frame surfaces at the peer's receiver.

### The `can_link_t` seam (testable without kernel CAN)

The raw frame I/O sits behind a small seam, `can_link_t` (`write_raw(frame)` + an
`on_receive` callback), so the transport never touches a socket directly:

- **`socketcan_link_t`** — the production impl: `socket(PF_CAN, SOCK_RAW, CAN_RAW)`,
  `CAN_RAW_FD_FRAMES` enabled best-effort (a classic-only controller still works),
  bound to a named interface (`vcan0`/`can0`), with a receive thread that translates
  each kernel `can_frame`/`canfd_frame` into a mode-agnostic `can_frame_data_t`. It is
  compiled only under `#ifdef __linux__` (a no-op stub elsewhere) so sanitizer and
  non-Linux builds stay clean. Concurrency mirrors `transport_ws`: serialized writes,
  the fd reset under the write lock before close, a bounded receive timeout polling the
  stop flag, destructor does stop→join→close.
- An **in-memory fake link** (in `core/tests/transport_can_test.cpp`) pairs two
  transports on one bus with no syscalls — this is what makes the binding fully
  testable in a plain container with no `vcan` module.

### Egress: advertise-then-data, with CAN-FD DLC padding

`send(frame)` is emitted as one **group** under a single lock (so concurrent sends
never interleave on the bus):

1. The frame is split by `view_can_frames_t` into data-field windows.
2. An **advertise manifest** is emitted first on the node's **control ID**
   (`[version|node|0]` — the lowest endpoint, hence highest bus priority, so the
   manifest out-arbitrates the data it governs). It is sliced into **classic ≤8-byte
   windows** even on an FD bus, so no DLC padding can perturb the far-side stream
   decoder. The manifest carries the **exact total length** and **slice count**.
3. The lean **data frames** follow, one per window, on consecutive endpoint slots
   (`slice_can_id` address-shift). In CAN-FD mode a short tail window is **padded up
   the DLC lattice** (`can_fd_dlc_round_up`) to a legal frame length.

### Ingress: learn, reassemble, trim

The receive thread decodes each frame's CAN ID. A **control-slot** frame feeds the
per-node advertise byte stream (`decode_advertise` pops each complete manifest),
which **learns the `id ↔ path` binding** and sets the group's expected slice count.
A **data-slot** frame is reassembled by `can_reassembly_t`, keyed by
`(node, base-endpoint) + (endpoint − base) index` — all derived from the CAN ID, so
no per-frame origin/ts ever rides the bus. On completion the slices are flattened and
**trimmed back to the advertised total length**, which is what undoes CAN-FD tail
padding so the delivered frame is byte-exact. A data frame that races ahead of its
manifest (cross-ID arbitration) is held pending and re-driven when the manifest lands.

```{admonition} Increment-2 modeling choices
:class: note
- **Advertise-per-send.** This binding emits a fresh manifest for every `send()`. It
  keeps the data plane correct and uniform (single value and multi-frame group are the
  same path) and makes DLC-padding trim unconditional. The steady-state
  *advertise-once-then-reuse* optimization (one binding, many lean values) is a future
  refinement; the learned bindings already persist and self-heal by overwrite on
  re-advertise.
- **Ordering.** Correctness relies on per-bus in-order delivery of a group's frames
  (which a single producer gets on CAN); the pending-data buffer covers control/data
  cross-ID reordering.
```

### Peer enumeration + transparent per-peer forwarding (ADR-0044)

A CAN bus reaches many peers over one wire, so `transport_can` also implements
the kind-neutral `tr::net::bus_link_t` capability (`transport_t::bus()`), which is
how a client of the node holding the bus **enumerates** the currently-reachable
peers and **forwards through** to them — with hard statelessness guarantees
([ADR-0044](../adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)):

- **No peer ever creates a vertex** — on this node or any other. The listing is
  synthesized per read; nothing persists in the graph; a peer's reboot mutates
  no listener's tree.
- **The only peer state is a last-heard table** inside the transport: refreshed
  by every valid same-version frame a peer emits (seeded by the join-time
  **hello** advertise) and expired after `peer_ttl` of silence. Insert-only, one
  entry per **distinct node id ever heard** — the same policy as the
  identity↔path map — so it grows with the bus *population* (structurally
  bounded by the 13-bit node-id space), never per request or per frame, and no
  artificial capacity is hard-wired (memory policy stays the host's).
- **The transit node keeps zero per-request state**: forwarding rides the
  RFC-0004 frame-carried routes (`dst`-shrink / `src`-grow) unchanged.

**Enumeration.** Peers appear as `n<node-id>` (decimal — the stable identity the
structured 29-bit ID already carries; collision-safe within the bus). A read of
the connection vertex's `:children[]` (e.g. `read("/net/can0:children[]")`,
locally or via a remote `FWD{READ}`) serves a `POINT` whose children are
`POINT{NAME n<id>}` members — a snapshot of who is currently audible, wired
through the vertex's `on_children` handler by `transport_vertex_t` for any link
whose `bus()` is non-null.

**Forwarding.** Each listed name doubles as a routable next-hop segment: when a
`FWD`'s first `dst` segment names no static child, the router's
`child_registry_t` asks each bus child to resolve it as a peer
(`bus_link_t::peer_link`), yielding a **directed** per-peer endpoint — the group's
advertise carries `target_node`, so on the broadcast bus only the addressed peer
delivers it. Inbound frames arrive tagged with the **sender's** peer name
(`bus_link_t::set_peer_receiver`), which the router uses as the hop's inbound
NAME — so the return route grown into `src` names the bus peer, and the reply is
itself a directed send. The whole round trip:

```{mermaid}
sequenceDiagram
    participant C as client
    participant T as transit T (CAN node 1)
    participant P as peer P (CAN node 5)
    participant Q as bystander Q (node 7)
    P->>T: hello advertise (join) — last-heard table gains n5
    C->>T: FWD{READ, dst=/net/can0, :children[]}
    T-->>C: POINT{ POINT{NAME n5}, … } (synthesized, no vertices)
    C->>T: FWD{READ, dst=/n5/a/b, src=/reply-ep}
    Note over T: "n5" = no static child → peer_link("n5")<br/>strip n5, grow src=/cli/reply-ep
    T->>P: directed group (target_node=5): FWD{READ, dst=/a/b}
    Note over Q: consumes slices, delivers nothing
    Note over P: terminus: read /a/b<br/>inbound NAME = "n1" (sender's peer name)
    P->>T: directed group (target_node=1): FWD{REPLY, dst=/cli/reply-ep}
    T->>C: FWD{REPLY} forwarded over "cli"
```

The liveness model is deliberately minimal (design (b) of the ADR-0044
implementation note): a peer is "reachable" iff it has been audible within
`peer_ttl` — an idle-but-alive node ages out until it next speaks. Probe-on-read
(a discovery probe emitted by the `:children[]` read, answered within a bounded
window) is the recorded follow-on; it needs deferred reply completion at the
`op_resolver_t` terminus, which is synchronous today.

### Tested two ways

- **Docker-local, no kernel CAN** — `core/tests/transport_can_test.cpp` pairs two
  transports over the in-memory fake link and asserts a multi-CAN-frame TLV round-trips
  byte-exact (classic **and** CAN-FD), advertise/map learning works, FD DLC padding is
  correct yet trimmed away, and the lifecycle is clean. Runs under ASan/UBSan and TSan.
- **Real `vcan0`** — `core/tests/transport_can_vcan_test.cpp` drives two
  `socketcan_link_t` over a kernel virtual-CAN device and asserts a byte-exact frame
  each way. It **self-skips** when `vcan0` cannot bind, so the required gates never
  depend on kernel CAN; the dedicated **`can-vcan-e2e`** workflow sets `vcan0` up so the
  socket path runs for real.
