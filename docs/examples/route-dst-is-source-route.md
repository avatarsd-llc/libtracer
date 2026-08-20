# The `dst` is a source route (L4 routing)

A `FWD` frame carries its own route. There is no forwarding table keyed on destinations, no
route discovery and no per-flow entry: a hop matches the leading `dst` route segments against
its own children, and where one of its names matches it strips that name's whole **mount run**
and prepends to `src` the mount run of the link the frame arrived on
([RFC-0004](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md)
§B,
[ADR-0040](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0040-net-plane-is-explicit-source-routed-only.md),
[ADR-0061](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)).

`dst` shrinks by exactly what this hop consumed; `src` grows by exactly how to get back here.
The frame that leaves is a different frame from the one that arrived, and the difference *is*
the hop.

Both mounts in the example are one route segment wide — the trivial case, and the clearest to
read. [The qualified-mount page](route-qualified-mount.md) is the same rule at full width, and
it is the general statement; *"one route segment per hop"* is a property of this wiring and
never of the rule.

## What to notice

- **The assertion is byte-exact.** The frame the hop emits is compared against the frame a
  client one hop closer would have built from scratch, so the example says "forwarding produced
  the canonical bytes" rather than "forwarding produced something that decodes plausibly".
- **Nothing was stored.** `handles()` reports zero bindings after the forward. A forwarder is
  stateless because the route left with the frame — and so did the return route
  ([reply home](route-reply-home.md) is the other half of that sentence).
- **The match is on whole route segments.** A `dst` whose first route segment merely *starts*
  with a child's name is not that child's traffic; `"bb"` is not `"b"`. Path bytes are
  self-delimiting records precisely so a prefix in bytes cannot be mistaken for a prefix in
  addresses
  ([RFC-0018](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0018-packed-path-segments.md)).
- **The child name is local.** Nothing downstream has to agree that this link is called `b`.
  That is what lets a caller compose `/b/c/sensor` out of two nodes' private names without any
  global namespace.
- **The link is a recording stub, not a socket.** The subject is the BYTES a hop emits, so the
  example stays synchronous: no threads, no ports, no rendezvous, nothing to flake. A
  `loopback_channel_t` or a real transport is the same code with threads in between — see
  [two nodes over a wire](two-node-fwd.md).
- **This target needs the FWD net plane.** It is built only when `LIBTRACER_NET_PLANE` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/route_dst_is_source_route.cpp
:language: cpp
:linenos:
```

See also: [fwd-router module](../modules/fwd-router.md) ·
[addressing](../reference/03-addressing.md) ·
[communication flows](../reference/04-communication-flows.md) ·
[terminus or forward](route-terminus-or-forward.md) ·
[multi-hop](route-multi-hop.md).
