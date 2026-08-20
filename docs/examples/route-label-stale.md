# A stale label is dropped, NACK'd, and re-advertised (L4 routing)

[Compaction](route-label-compact.md) makes a delivery depend on state the two ends must agree
about. Anything that can desynchronise it — a reconnect, a restart, an eviction to stay inside a
bounded table — leaves an upstream happily streaming onto a label the downstream has forgotten.

The design answer is that a stale label is **never dereferenced and never guessed at**: the frame
is dropped, a `HANDLE_NACK` goes back, and receiving that NACK makes the producer re-advertise
([RFC-0004](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md)
§E.1,
[ADR-0035](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0035-implementing-rfc-0004-remote-operation-addressing.md),
[ADR-0030](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md)).

**Re-advertising IS the self-heal.** There is no separate repair protocol, no sequence numbers to
reconcile and no teardown handshake — which is why `clear_link` is a safe thing for a transport
to call on every (re)connect: the worst case is one dropped frame and one round trip.

## What to notice

- **The whole repair is counted, not narrated.** One frame lost, one NACK, one re-advertise, and
  the delivery count resumes climbing. Those are the four assertions.
- **The refusal is observable.** `on_stale_label` carries the inbound link name and the refused
  label, so an operator can see a desynchronised flow instead of inferring it from a gap in the
  data. Dropping silently would be the same behaviour with none of the evidence.
- **The refused frame changed nothing.** The delivery count does not move — a dropped frame, not
  a misrouted one. That distinction is the entire reason the label is not looked up
  best-effort.
- **`clear_link` on an unknown link is a no-op**, deliberately, so a transport can call it
  unconditionally from its connect path. The example checks that too.
- **On a mid-chain node `clear_link` reaches further than one link.** It also drops every ingress
  binding whose downstream half crossed the cleared link
  ([#716](https://github.com/avatarsd-llc/libtracer/issues/716)) — without that, an upstream
  that never saw the reconnect keeps streaming onto a dead out-label and the flow drops silently
  forever. The two-node example cannot show that; it is the reason the hook is not simply
  "forget my own table".
- **`link_down` is the bigger hammer.** It runs the subscriber-edge eviction as well as this
  label clear, and it is what `add_child` installs behind every child's departure notifier.
- **This target needs the FWD net plane.** It is built only when `LIBTRACER_NET_PLANE` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/route_label_stale.cpp
:language: cpp
:linenos:
```

See also: [fwd-router module](../modules/fwd-router.md) ·
[network formation](../reference/13-network-formation.md) ·
[CAN transport](../reference/14-can-transport.md) ·
[the label plane](route-label-compact.md) ·
[the child table](route-child-table.md).
