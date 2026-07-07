# Two nodes over a wire — FWD delivery (L4 + transport)

Two independent [`graph_t`](../modules/graph.md) nodes, each with a
[`fwd_router_t`](../modules/transport.md), connected by a `loopback_channel_t` — the
in-process dev "wire". A client hands node A's router an
`FWD{ op=WRITE, dst=/b/sensor/temp }` frame; A peeks the first `dst` segment, strips
`b`, and forwards `/sensor/temp` across the wire; B's terminus writes it into B's
local vertex, waking B's subscriber. The route is **explicit and loop-free by
construction** — no per-request state on any hop (RFC-0004, ADR-0040;
[network formation reference](../reference/13-network-formation.md)).

```{tip}
**Going to a real socket is a one-line swap.** Replace `channel.a()` / `channel.b()` with
two `tr::net::udp_transport_t` instances and the routers/graphs above are unchanged — that
exact swap is [`core/tests/udp_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/udp_test.cpp)'s
two-node test.
```

## What to notice

- **The frame carries its own route** — `fwd_write` builds `FWD{ op, dst, src, payload }`;
  `dst` shrinks one `NAME` per hop and `src` grows the way back, so the reply route is
  assembled for free.
- **The forward hop never touches the heap** — a router reads three headers by offset and
  scatter-gathers the shrunk-`dst` / grown-`src` heads with untouched views of the inbound
  frame.
- **Cross-wire latency is measured** — 2,000 sequential FWD writes through the loopback
  channel; the `RESULT` line reports the mean end-to-end delivery time (dispatch + the
  channel's receive-thread hand-off).
- **It self-checks** — every frame must be delivered with the exact payload, so the ctest
  smoke test guards delivery, not just a clean exit.

```{note}
The absolute nanoseconds come from whatever build ran (CI builds the examples in a debug
configuration) and include the loopback channel's thread hand-off; treat them as a *shape*,
not a spec number. The canonical network latency/throughput figures live on the
[performance page](../performance.md).
```

## Source

```{literalinclude} /core/examples/two_node_fwd.cpp
:language: cpp
:linenos:
```

See also: [transport module](../modules/transport.md) ·
[network formation reference](../reference/13-network-formation.md) ·
[communication flows](../reference/04-communication-flows.md).
