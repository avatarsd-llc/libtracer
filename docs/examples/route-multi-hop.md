# Three nodes, and a forwarder that stores nothing (L4 routing)

A chain is the one-hop rule applied again. A holds a child called `b`, B holds a child called
`c`, C holds the vertex; a client writes `/b/c/sensor/temp`. There is no route discovery, no
flooding, no forwarding information base and no per-flow entry anywhere — each node applies the
same test, and the route consumes itself as it travels
([ADR-0040](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0040-net-plane-is-explicit-source-routed-only.md)).

The check that matters is the negative one. After the write has crossed B, B's routing plane
holds exactly what it held before: zero label bindings, zero link shells, the same
receiver-context count. That bounds a forwarder's memory by its **topology** (how many links it
has) rather than by its **traffic** (how many flows cross it) — the reason a 16 KB node can be a
forwarder at all.

## What to notice

- **The "before" values are read, not assumed.** The example snapshots B's counters and compares
  against the snapshot. Asserting `== 0` against a hard-coded zero would keep passing if the
  baseline ever moved.
- **Each hop's wire bytes are asserted.** `dst=/c/sensor/temp, src=/cli` on the A→B wire,
  `dst=/sensor/temp, src=/a/cli` on the B→C wire. The return route is visibly under
  construction, one hop at a time.
- **Hops are driven explicitly.** The frame a node emits is handed to the next node's
  `on_frame`. A `loopback_channel_t` or a socket does the same thing with threads in between —
  see [two nodes over a wire](two-node-fwd.md) — but driving it by hand keeps the example
  synchronous and puts the intermediate bytes where they can be asserted on.
- **The names are private.** `b` means something only to A and `c` means something only to B.
  `/b/c/sensor/temp` is the composition, spelled by whoever holds both mounts.
- **Statelessness is a choice with a price, and the price is on the wire.** Every frame
  re-carries its route. [The label plane](route-label-compact.md) is what buys that back for
  flows that repeat — deliberately, per flow, and only when someone asks.
- **This target needs the FWD net plane.** It is built only when `LIBTRACER_NET_PLANE` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/route_multi_hop.cpp
:language: cpp
:linenos:
```

See also: [fwd-router module](../modules/fwd-router.md) ·
[communication flows](../reference/04-communication-flows.md) ·
[composition over the network](../reference/18-composition-over-the-network.md) ·
[the source route](route-dst-is-source-route.md) ·
[the reply home](route-reply-home.md).
