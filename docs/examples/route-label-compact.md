# A repeating flow buys its route back (L4 routing)

*"A delivery IS a `FWD` WRITE"* is the right model and the wrong bill. Taken literally it makes
every streamed sample re-carry its full return route — roughly 16× overhead on a small,
high-rate sample
([RFC-0004](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md)
§D, §E.1). The route-handle is header elision generalized: the producer advertises the route
once, the consumer records `label → binding`, and the steady state becomes
`COMPACT{ label, payload }`
([ADR-0035](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0035-implementing-rfc-0004-remote-operation-addressing.md),
`tr::net::route_handle_t`).

The bare word **label** always means this — RFC-0004 §E.1's per-link `u16`. RFC-0027's
per-element alias is always spelled "path label", and it is a different mechanism
(`CONTEXT.md` §Path label).

## What to notice

- **The label is per LINK, not global.** It is minted by `advertise` against one child name and
  means nothing anywhere else; a forwarding hop **swaps** it, MPLS-style, exactly as a CAN ID is
  re-resolved against each bus
  ([ADR-0030](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md)).
- **Only flagged flows pay.** A binding exists because someone advertised. A cold, one-shot or
  non-compact flow allocates no entry at all — so the stateless-forwarder property of
  [multi-hop](route-multi-hop.md) survives alongside this one, rather than being traded away.
- **Minted once per `(link, route)`, then reused** ([#913](https://github.com/avatarsd-llc/libtracer/issues/913)).
  Advertising the same route again re-sends the frame — which is the reconnect self-heal — but
  grows no table. The example checks both halves: same label back, table still at one.
- **The delivery is still a write.** The consumer's binding here is a *terminus* binding: the
  advertised route names a vertex on the consumer, so a COMPACT is written straight to it and
  reported through `on_compact_delivery`. Compaction changes the bytes, never the semantics.
- **The byte delta is measured, not asserted at a threshold.** The example checks only
  `compact < full` and *prints* the numbers, because the delta depends on how long the route is
  — a hard-coded figure would be a CI flake waiting for a longer path.
- **A `0` from `advertise` is a refusal, not an error.** No such link, or that link's label space
  is exhausted / its egress table is full under an injected bound — in which case the flow simply
  keeps using the full-route form. The example checks for it rather than assuming success.
- **This target needs the FWD net plane.** It is built only when `LIBTRACER_NET_PLANE` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/route_label_compact.cpp
:language: cpp
:linenos:
```

See also: [fwd-router module](../modules/fwd-router.md) ·
[CAN transport](../reference/14-can-transport.md) ·
[network formation](../reference/13-network-formation.md) ·
[multi-hop](route-multi-hop.md) ·
[a stale label](route-label-stale.md).
