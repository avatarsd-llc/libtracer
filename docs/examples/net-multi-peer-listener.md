# One listener, many slots — and why `p<slot>` is not an identity (transport plane)

`tcp_transport_t`'s LISTEN constructor accepts one peer at a time — the board↔board shape. A
node that fans out to browser tabs or to a fleet needs the other one: `transport_tcp_server`
(and its RFC 6455 sibling `transport_ws_server`, sharing the same slot/poll machinery since
#871) runs **one** poll thread over a slot table, so steady-state memory is bounded by the
concurrent-peer high-water mark or by `max_peers`, whichever is smaller.

With `peer_named`, that slot table is exposed through the same `bus_link_t` facet CAN uses — and
that is where the two naming regimes part company.

## What to notice

- **CAN names by IDENTITY, a slot server names by POSITION.** `n<node-id>` is derived from the
  peer's own bus id, so the name, the table key and the endpoint are one thing no other peer can
  inherit. `p<slot>` is a *position*: after that peer departs, a pointer resolved for it
  addresses whatever session inherits the slot, and the endpoint's own liveness check is
  satisfied by that stranger. The pointer never dangles; it silently changes who it means
  ([#1153](https://github.com/avatarsd-llc/libtracer/issues/1153)).
- **So: resolve per use.** `child_registry_t` resolves and sends in one expression, which is why
  no shipping caller is exposed, and a remote subscriber edge stores the peer NAME rather than
  the pointer.
- **`max_peers` is an injected bound, not a backlog** (RFC-0006). A connection past the cap is
  accepted and immediately closed — a clean refusal rather than a hung SYN. It is also resolved
  once at construction (a request of `0` takes the liveness window's own ceiling), so the
  example reads back what the server *enforces* instead of assuming it got what it asked for.
- **One object, two addressing surfaces.** `server.send(frame)` fans out to every open peer;
  `facet.peer_link("p1")->send(frame)` reaches exactly one. The example checks the directed send
  arrived at p1 **and** that p0 and p2 got nothing.
- **A name outside the live slot set resolves to `nullptr`.** Endpoints are not synthesized on
  demand, so a stale name cannot be sent to.
- **This target needs the TCP transport** (`LIBTRACER_TRANSPORT_TCP`, the default).

## The one run-time skip in this group, and why it is not silent

The subject here is the ADR-0044 peer-named tier, and a target can compile that out with
`kBusLinks = false` — a C++ binding in `config_override.hpp`
([ADR-0068](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0068-build-configuration-is-plain-cpp-config-header.md)),
invisible to CMake, so no build guard can express it. Neither move the earlier domains used is
available: there is no second arm to name (with the module closed out there is no peer-named
tier at all), and a `return 0` would make ctest record a **pass for an example that ran
nothing** — the exact defect the [index](index.md) warns about.

So the example states the skip and exits **77**, and its `add_test` carries
`SKIP_RETURN_CODE 77`, which makes ctest report it as **Skipped**. That is strictly better than
the two older run-time skips in this tree, which exit `0` and are therefore indistinguishable
from a real pass. Verified: in a `kBusLinks = false` build `ctest -R example_` reports
`example_net_multi_peer_listener (Skipped)` and passes the rest.

## Source

```{literalinclude} /core/examples/net_multi_peer_listener.cpp
:language: cpp
:linenos:
```

See also: [transport module](../modules/transport.md) ·
[concurrency and scaling](../reference/15-concurrency-and-scaling.md) ·
[the identity-named bus](net-can-bus-peers.md) ·
[the single-peer pair](net-dial-and-listen.md).
