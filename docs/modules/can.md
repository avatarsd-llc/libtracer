# can — the header-elided CAN stack

```{admonition} In one paragraph
:class: tip
CAN gets a whole layer of its own because the bus is too narrow for a TLV header:
classic CAN carries eight data bytes per frame. libtracer elides the header
instead of shrinking it — **the 29-bit extended CAN ID *is* the address** — so a
data frame on the bus is payload and nothing else. A dynamic identity↔path map
inside the transport self-establishes through in-band `advertise` frames, an L1
splitter cuts one logical payload into id-matched frame windows with no copy, and
a reassembler chains the windows back into a rope on the far side.
```

## What it does

The narrative — why the header is elided, how a map forms and heals with no
gateway role, and what the bus looks like end to end — is
[reference §CAN transport](../reference/14-can-transport.md). This page is the
C++ surface, layer by layer.

**The ID codec** (`tr::net::can`) is pure and host-testable: it knows nothing
about sockets. A 29-bit extended identifier is a structured field — protocol
version, node, endpoint, and a per-group id — and `encode_can_id` /
`decode_can_id` / `slice_can_id` are the only places that layout exists. Because
it is pure, every ID edge case is testable without a bus.

**The advertise codec** (also `tr::net::can`) is the in-band control stream that
makes the map self-establishing. An `advertise_t` announces "this node, this
endpoint, this path"; the lean id-matched data frames that follow carry only
bytes. Endpoint slot `0` is reserved for the advertise stream, data groups start
at the next slot, and a peer that has been silent past the liveness window leaves
the enumeration on its own — there is no orchestrator to tell it to.

**The splitter** (`tr::view::view_can_frames_t`) is L1, not transport: it cuts a
view into per-frame windows, each a subview over the same segment, never a
memcpy. `can_frame_mode_t` selects the classic 8-byte or CAN-FD 64-byte data
field, and `can_fd_dlc_round_up` handles CAN-FD's non-contiguous length ladder.

**The reassembler** (`tr::net::can_reassembly_t`) is the far side: `(origin,
timestamp) + index` chains slices back into a `rope_t` — libtracer's own
address-shift slicing, not ISO-TP. Its storage comes from an injected
`std::pmr::memory_resource` and its group count is bounded by configuration, so
exhaustion on a constrained node is a bounded evict-oldest drop with a counter,
never an allocation failure. The defaults — process heap, unbounded — are what a
host gets unless it says otherwise.

**The binding** (`tr::net::transport_can`) joins all of that to a real bus
through the `can_link_t` seam. `socketcan_link_t` is the production Linux
implementation, a `PF_CAN` raw socket with a receive thread. A different platform
implements the same seam.

**The TX pool** (`tr::net::can_tx_pool_t`) exists for *asynchronous* links only.
A synchronous link needs none of it — the kernel copies the frame inside the
write call. A driver that queues the frame *pointer* and formats the buffer later,
possibly from a transmit-done interrupt, must keep the descriptor and payload
alive until completion; handing such a driver the writer's stack storage is a
use-after-free with interrupt-context corruption. The pool is the storage the
link owns instead: fixed capacity chosen at construction, non-blocking acquire,
lock-free release. Deliberately mechanism-only — what to do when it is *full*
(bounded backpressure, a counted drop) belongs to the owning link, which pairs it
with its platform's blocking primitive.

## Pitfalls

- **An advertise is not a handshake.** Nothing acknowledges it and nothing
  depends on having seen one before sending; a receiver that has not yet learned
  a mapping simply cannot attribute those frames yet, and learns on the next
  advertise.
- **Endpoint `0` is reserved.** Data groups begin at the first data endpoint;
  allocating group traffic to slot 0 collides with the control stream.
- **CAN-FD lengths are a ladder, not a range.** 8/12/16/20/24/32/48/64 — a
  payload that does not land on a rung is padded up, and the padding is on the
  wire.
- **The TX pool is per-link, not per-transport.** Sizing it is a property of how
  deep the driver's queue is, not of how many paths the node publishes.

## API reference

### The ID and advertise codecs

```{doxygenstruct} tr::net::can::can_id_fields_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::net::can::encode_can_id
:project: libtracer
```

```{doxygenfunction} tr::net::can::decode_can_id
:project: libtracer
```

```{doxygenfunction} tr::net::can::slice_can_id
:project: libtracer
```

```{doxygenstruct} tr::net::can::advertise_t
:project: libtracer
:members:
```

```{doxygenvariable} tr::net::can::kAdvertiseMagic
:project: libtracer
```

```{doxygenvariable} tr::net::can::kAdvertiseFormatVersion
:project: libtracer
```

```{doxygenvariable} tr::net::can::kAdvertiseHeaderSize
:project: libtracer
```

```{doxygenvariable} tr::net::can::kAdvertiseFlagGroup
:project: libtracer
```

```{doxygenvariable} tr::net::can::kAdvertiseMaxPathLen
:project: libtracer
```

```{doxygenvariable} tr::net::can::kCanBroadcastNode
:project: libtracer
```

```{doxygenfunction} tr::net::can::encode_advertise
:project: libtracer
```

```{doxygenfunction} tr::net::can::decode_advertise
:project: libtracer
```

```{doxygenfunction} tr::net::can::advertise_prefix_plausible
:project: libtracer
```

### The L1 splitter

```{doxygenenum} tr::view::can_frame_mode_t
:project: libtracer
```

```{doxygenclass} tr::view::view_can_frames_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::view::can_max_data
:project: libtracer
```

```{doxygenfunction} tr::view::can_fd_dlc_round_up
:project: libtracer
```

```{doxygenvariable} tr::view::kCanClassicMaxData
:project: libtracer
```

```{doxygenvariable} tr::view::kCanFdMaxData
:project: libtracer
```

### Reassembly

```{doxygenclass} tr::net::can_reassembly_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::reassembly_key_t
:project: libtracer
:members:
```

### The bus binding

```{doxygenstruct} tr::net::can_frame_data_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::can_link_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::socketcan_link_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::transport_can_config_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::transport_can
:project: libtracer
:members:
```

```{doxygenclass} tr::net::can_tx_pool_t
:project: libtracer
:members:
```

See: [reference §CAN transport](../reference/14-can-transport.md) (the narrative),
[transport](transport.md) (the seam `transport_can` implements),
[views](views.md) (the subview machinery the splitter uses),
[fwd-router](fwd-router.md).
