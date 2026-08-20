# The one BUS kind, and where its peer names come from (transport plane, `can`)

Every other kind in the tree is point-to-point: one link, one far side, so the child NAME the
router registered for the link already addresses it. A CAN bus breaks that — one link reaches
every node on the wire — and the routing plane still needs a hop segment per peer.

`tr::net::bus_link_t` is how a kind answers that **without the graph growing per peer**
([ADR-0044](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md) §1:
no vertex is ever created for a peer).

## What to notice

- **The peer list is a snapshot of traffic, not a registry.** `enumerate_peers` walks a
  last-heard table refreshed by other nodes' own frames and seeded by the hello advertise a node
  emits at join. A node silent longer than `peer_ttl` simply stops being listed, and nothing had
  to notice it leave. No join protocol, no coordinator, no departure event.
- **Names are DERIVED, not assigned.** `n<node-id>` comes straight out of the structured CAN ID,
  so it is collision-safe by construction and a rejoining node reappears under the name it had.
  Contrast the stream servers, which name peers `p<slot>` **positionally** — see
  [the multi-peer listener](net-multi-peer-listener.md) for why that difference decides whether
  a resolved endpoint may be cached.
- **A directed send on a broadcast medium.** Every node's link sees the CAN frames; the group's
  advertise carries `target_node`, so only the addressed peer reassembles and delivers. The
  example checks both halves — node 2 got the frame byte-exact, node 3 got nothing.
- **The inbound seam speaks HANDLES, not names** (#1294). The flat `transport_t` sink is handed
  bytes and nothing else, which is complete on a point-to-point link and not on a bus;
  `bus_link_t::set_peer_receiver` tags each delivery with the sender's `peer_handle_t`, and
  `peer_name` is the one bridge — a pure function of the node id, so no lock and no lookup.
  The example resolves it **inside** the delivery, which is the only place the answer is defined.
- **The bus is in memory, and that is the point of the seam.** Raw frame I/O sits behind one
  virtual (`can_link_t`), so framing, reassembly and the peer table are exercised with no kernel
  CAN — and the ESP-IDF port drops `twai_link_t` into the same slot `socketcan_link_t` occupies
  on Linux. The real-`vcan` path has its own dedicated CI job.
- **This target needs the CAN transport,** and CAN implies the bus module: `transport_can.cpp`
  carries a `static_assert(kBusLinks)`, because a CAN link is peer-named by construction. So
  this example can never be built into a target whose subject is absent — it is present or it
  is not compiled, and it never skips at run time.

## Source

```{literalinclude} /core/examples/net_can_bus_peers.cpp
:language: cpp
:linenos:
```

See also: [CAN module](../modules/can.md) ·
[CAN transport reference](../reference/14-can-transport.md) ·
[transport module](../modules/transport.md) ·
[the positional naming regime](net-multi-peer-listener.md).
