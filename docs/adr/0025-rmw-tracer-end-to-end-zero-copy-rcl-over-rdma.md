# End-to-end zero-copy ROS 2 (`rmw_tracer`): loaned POD messages over shared-memory and RDMA, and "rcl over RDMA"

[ADR-0023](0023-ros2-binding-via-rmw-tracer.md) established `rmw_tracer` and made the loaned-message API mandatory for a zero-copy *take*. That is necessary but not sufficient for **end-to-end** zero-copy: the publish side, the transport, and the cross-host case must each avoid a copy too. This ADR specifies the full chain — and shows that "**rcl over RDMA**" (ROS 2 nodes communicating over RDMA with one-sided zero-copy) is not a new mechanism but the composition of three pieces we already have.

## Decision

End-to-end zero-copy for `rmw_tracer` is the composition of three pieces; each removes one copy, and they must all hold to call the path zero-copy.

**1. Loaned POD messages (no serialize copy, no take copy).** For a **loanable** message — a fixed-size, pointerless ("plain") `rosidl` type — the publisher calls `rmw_borrow_loaned_message`, gets a libtracer **segment**, and **constructs the message in place** (no CDR serialize into a separate buffer). The subscriber calls `rmw_take_loaned_message` and receives a **borrowed view** into the same segment (no copy into a user buffer). Non-POD messages (strings, unbounded sequences) **cannot** be zero-copy — a universal ROS/DDS/Zenoh limitation — and fall back to `rcl`'s single CDR serialize + libtracer's zero-copy carry (still single-copy, better than DDS's several). The `:schema` POINT records whether a type is loanable.

**2. A zero-copy transport.**
- **Intra-host:** `mem_shared` / `transport_shm` — the loaned segment lives in shared memory both processes map; fan-out is a refcount bump. (Parity with `rmw_zenoh`/iceoryx SHM.)
- **Inter-host:** `transport_rdma` + `mem_rdma` — the loaned segment is RDMA-registered; the publisher **one-sided RDMA-writes** it directly into the remote subscriber's registered receive buffer — no remote-CPU involvement ([ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) **branch B**).

**3. Refcounted segments end to end.** The message bytes are the segment are the wire bytes (load-bearing claim 1); fan-out to N subscribers is N refcount bumps, never a copy. The bridge forwards the segment as a rope via scatter-gather (the M6 view-delivering seam), never flattening.

**"rcl over RDMA" is branch B, established by advertise+id-match.** One-sided RDMA needs the remote buffer's address (rkey + VA) known to the sender — which is exactly the [ADR-0022](0022-transport-framing-modes-elided-full-tlv-advertise.md) **advertise+id-match** mechanism generalized to RDMA buffers: the **subscriber advertises** its registered receive buffer `(rkey, VA, id)`; the **publisher RDMA-writes** the loaned message into it, matched by `id`; the subscriber's `rmw_take_loaned_message` then hands its node a view of bytes the NIC already delivered. So the chain is uniform with libtracer's model: *loaned message = a borrowed segment view; the advertise = the binding establishment; the RDMA write = the transport lowering the rope to its native DMA; the take = a refcount bump.* This is the memory-registration handshake [ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) named as branch B's cost — realized as an advertise frame, consistent with "capability negotiation does not exist" (it is binding establishment, not per-frame negotiation).

## Considered options

- **Serialize-then-zero-copy only (loaned-take but copied-publish).** Rejected as "end-to-end zero-copy": a CDR serialize on publish is still a copy. Loaned *publish* (construct-in-place) is required to close the publish side; it is the only way to beat DDS on the send path.
- **Force zero-copy for all message types.** Impossible: variable-size messages have no fixed layout to loan; ROS, DDS, and Zenoh all fall back to serialization. We document loanability per type rather than pretend.
- **RDMA without an advertise handshake (publisher guesses the remote buffer).** Rejected: one-sided RDMA fundamentally needs the remote `(rkey, VA)`; the advertise+id-match frame is the principled, already-designed way to exchange it — not a bespoke RDMA control protocol.
- **Two-sided RDMA (send/recv, remote CPU reposts).** Rejected for the zero-copy path: a remote-CPU copy from the recv queue defeats the point; one-sided WRITE into the advertised buffer is the zero-copy form. (Two-sided remains a fallback when buffers can't be pre-advertised.)

## Consequences

- `rmw_tracer` matches `rmw_zenoh` intra-host (SHM zero-copy) and **exceeds it inter-host**: Zenoh's zero-copy is SHM/intra-host only, while "rcl over RDMA" gives **sub-µs, zero-copy ROS 2 across the network** — a differentiator for HPC and large robotics fleets — plus ROS over constrained buses ([ADR-0023](0023-ros2-binding-via-rmw-tracer.md)) that Zenoh cannot reach.
- It needs `mem_shared` (v1) and `mem_rdma` + `transport_rdma` (catalogued "future"); this ADR makes them the zero-copy ROS path and reuses **advertise+id-match** for RDMA buffer registration rather than inventing an RDMA control plane.
- The loanability constraint is surfaced, not hidden: tools can see (via `:schema`) which topics are true zero-copy and which fall back to single-copy serialization.
- The whole chain is the same-substrate model end to end — loaned message, advertise, RDMA write, refcounted take — so it shares the codec, the rope, and the bridge with every other transport; RDMA is one more lowering, not a parallel stack.
