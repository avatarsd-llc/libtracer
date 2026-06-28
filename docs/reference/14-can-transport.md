# CAN transport — header-elided framing, a structured 29-bit ID, and self-healing in-band advertise

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
| multi-frame reassembly | `tr::mem::mem_can_reassembly_t` | `mem_can_reassembly.hpp` | L0 |
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

`mem_can_reassembly_t` reassembles a payload that spanned several CAN frames. Each
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
`mem_can_reassembly_t` is named for L0 (`tr::mem`) per [ADR-0030](../adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md)
and #55, yet it yields an L1 `rope_t`. The reassembly *bookkeeping* (which indices
arrived, totality, gap detection) is the genuine L0 concern; chaining the borrowed
slice views into a rope is zero-copy (no allocation), so it does not run afoul of
the "owning-handle helpers live in `tr::view`" rule ([ADR-0016](../adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) §2).
It is the one L0-named component that references the L1 rope it assembles.
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

On-wire layout (little-endian, a 16-byte header + the path bytes):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | magic = `0xAD` |
| 1 | 1 | format version = `0x01` |
| 2 | 1 | flags (`0x01` = group) |
| 3 | 1 | reserved, must be zero |
| 4 | 4 | `can_id` (u32 LE; a 29-bit value) |
| 8 | 4 | `group_total_len` (u32 LE; 0 if single-value) |
| 12 | 2 | `slice_count` (u16 LE; 1 if single-value) |
| 14 | 2 | `path_len` (u16 LE) |
| 16 | `path_len` | path bytes (UTF-8 libtracer path) |

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

## The SocketCAN binding (`transport_can`)

Increment 2 realizes the binding: `tr::net::transport_can` is a `transport_t` that
drives the framing above over a real Linux CAN bus. A bridge hands it a complete
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
A **data-slot** frame is reassembled by `mem_can_reassembly_t`, keyed by
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
