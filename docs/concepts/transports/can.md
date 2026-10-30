# CAN links

## In one paragraph

A CAN data frame holds 8 bytes (64 on CAN-FD), too few to spend 4 of them on a TLV header. So
the CAN link elides the header into the bus itself: the 29-bit extended ID is a structured
`version | node | endpoint` field, and an ID that a node has advertised stands for a path. A
frame larger than one data field is cut into slices with no copy, announced by a manifest on the
node's control ID, and copied back into one segment on the far side. Because a lower ID wins
arbitration, assigning IDs also assigns bus priority. The map from ID to path forms itself from
in-band advertise frames, with no gateway or coordinator.

## The picture

```{mermaid}
:caption: "Legend: green = vertex, blue = frame, vermillion = peer."
flowchart LR
    accTitle: One frame crossing a CAN bus
    accDescr: The CAN link vertex splits a frame into a manifest on its control ID and data slices on consecutive endpoint IDs. The peer node's CAN link reassembles the slices into one segment and delivers the frame.
    L("/net/can/can0"):::vertex -->|"manifest: total, slice count"| M[/"advertise on control ID"/]:::frame
    L -->|"slices 0..n-1"| S[/"data frames on endpoint IDs"/]:::frame
    M ==>|CAN bus| R[[peer CAN link]]:::peer
    S ==>|CAN bus| R
    R -->|"one segment"| F[/"the frame, reassembled"/]:::frame
```

## When you need it

When the wire is a CAN or CAN-FD bus: motor controllers, sensors and actuators on a vehicle or
machine bus, where arbitration priority matters and a frame's data field is small. For frames
of kilobytes the bus spends many frames per message; see the Cost section before you send them.

## How it works

**The ID is the address.** `encode_can_id` and `decode_can_id` own the 29-bit layout: a 4-bit
protocol-version prefix, a 13-bit node and a 12-bit endpoint. A receiver ignores any prefix
that is not its own, so two protocol generations share one bus.
[Reference 14](../../reference/14-can-transport.md#the-structured-29-bit-extended-id) has the
layout and why the widths are a modelling choice.

**A send is a group.** `send(frame)` splits the frame into data-field windows, each a subview
of the frame, never a copy. The link first emits a manifest on its control ID, the lowest
endpoint and so the highest priority, carrying the exact total and the slice count. It always
uses classic 8-byte windows for the manifest. The data slices follow on consecutive endpoint
IDs; on CAN-FD the short last slice is padded up the DLC ladder with zeros.

**A receive is bounded and counted.** The receiver checks the announced total against the slice
geometry, its `max_message` and its in-flight budget before it draws anything. It then draws one
segment of the total from the link's receive memory and writes each slice at its offset. Slices
may arrive in any order; a missing one leaves the group incomplete. A refused or aged-out group
is counted on `dropped_groups`, and a full budget refuses the newcomer rather than evicting an
open group. Age-out reads no clock: the caller stamps `set_now` and sweeps on its own cadence.

**Peers come from traffic.** The link is a bus: `:children[]` lists the peers heard within
`peer_ttl`, synthesized from the wire, with no vertex per peer. A directed group carries a
target node, and every other node consumes its slices without delivering them.

## Cost

### Wire bytes

A send of a `P`-byte frame spends `ceil((18 + L) / 8)` classic frames of manifest, where `L` is
the link's path length (3 frames for a path of up to 6 bytes), then the data frames:

| frame bytes | classic data frames | CAN-FD data frames |
| ---: | ---: | ---: |
| 64 B | 8 | 1 |
| 1 KiB | 128 | 16 |
| 4 KiB | 512 | 64 |
| 16 KiB | 2,048 | 256 |

The default `max_message` is 64 KiB (`config_t::kCanMaxMessageBytes`). Above a few hundred
bytes the bus time, not the library, is the cost to plan for.

### Allocations

None per send: the slices are subviews. One segment per received group, of the total plus one
presence bit per slice, drawn from the link's receive memory. An asynchronous driver adds a
`can_tx_pool_t` of fixed capacity, chosen at construction.

### Latency

Not benched end to end: the bus needs a privileged `vcan0` the bench host lacks.
`bench_can_reassembly` measures the receive path, and `transport_can_vcan_test` in
`can-vcan-e2e.yml` would carry a latency arm.

### Flash and RAM per target

| Target | Flash | RAM per link |
| --- | --- | --- |
| NARROW | not measured | in-flight receive budget, `config_t::kCanRxInflightBytes` (default two maximum messages) |
| MID | not measured | same |
| WIDE | not measured | same; not pinned by `bench_conn_ram`, whose CAN arms need `vcan0` |

A per-transport row in `tools/esp_size_gate.py` would measure flash on MID, and the CAN arms of
`bench_conn_ram` on a host with `vcan0` would pin the RAM.

## What the library does not do

- It does not use ISO-TP. Reassembly is libtracer's own address-shift slicing.
- It does not acknowledge an advertise. A receiver that missed one learns on the next.
- It keeps no vertex and no state per peer.
- It runs no timer for reassembly or liveness; the caller drives the sweep.

## Pitfalls

- **Symptom:** data frames arrive but nothing is delivered. **Cause:** the receiver never heard
  the manifest, so it cannot attribute the slices. **Fix:** nothing to configure; the next
  advertise binds them. If it persists, check the version prefix matches.
- **Symptom:** a payload costs more bus time on CAN-FD than its length suggests. **Cause:** a
  slice that does not land on the DLC ladder (8, 12, 16, 20, 24, 32, 48, 64) is padded up.
  **Fix:** size values so the last slice lands on a rung.
- **Symptom:** large messages are dropped with `dropped_groups` rising. **Cause:** the
  announced total exceeds `max_message`, or the in-flight budget is full. **Fix:** raise the
  compile-time bound for this build, or send smaller values.
- **Symptom:** group traffic collides with the control stream. **Cause:** a group used endpoint
  0, which is reserved for advertises. **Fix:** let the link allocate endpoints.

## Where the rules live

**Normative source:** [v1 §1](../../spec/v1.md#1-scope) leaves a transport's framing below the
TLV to its binding; reference 14 describes this one and is not normative. The frames it carries are
[v1 §3](../../spec/v1.md#3-wire-format).

**Exercised by:** `transport_can_test`, `transport_can_peers_test`, `can_frames_test`,
`can_tx_pool_test`, `transport_can_vcan_test`.

Next concept: [stream links](stream.md). Go deeper:
[reference 14](../../reference/14-can-transport.md), then the
[can module page](../../modules/can.md), then [v1 §1](../../spec/v1.md#1-scope).
