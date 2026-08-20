# Terminus or forward — one test decides (L4 routing)

A node does not classify traffic into "local" and "remote", and a `FWD` carries no flag saying
which it is. Every inbound frame is put to the same question: **is the leading `dst` route
segment one of my registered children?** Yes ⇒ forward it. No ⇒ *I am the terminus* — resolve
the whole remaining `dst` as a local address and apply the op
([RFC-0004](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md)
§D,
[ADR-0035](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0035-implementing-rfc-0004-remote-operation-addressing.md)).

The consequence worth internalising: the **same bytes** are a forward at one node and a terminus
at the next, decided entirely by each node's own child table. `/sensor/temp` is a route while a
child is called `sensor`, and an address the moment that child is gone. Addressing and routing
share one namespace on purpose (`CONTEXT.md` §Path-as-route).

## What to notice

- **The example wires the ambiguity deliberately.** One node holds a child named `b` *and* a
  local vertex at `/sensor/temp`, and both arms are exercised against it — so the forward arm
  proves the local vertex was not touched, and the terminus arm proves it can be.
- **The terminus failure is ADDRESSED, not dropped.** A `dst` that resolves to no local vertex
  answers `FWD{REPLY}` with `kind=ERROR`, source-routed home along the `src` the request
  accumulated. A forwarder that silently swallowed unroutable frames would turn every
  addressing typo into a timeout instead of a status
  ([RFC-0002](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0002-protocol-error-model.md)'s
  model, carried on the FWD plane).
- **The reply goes out on the link the request arrived on.** That is the per-hop retrace, not a
  lookup — the terminus does not have to know where the origin is, only which link spoke to it.
- **Nothing about the frame is hop-aware.** No TTL, no hop count, no visited set. `dst` strictly
  shrinks per hop, which is what makes both planes loop-free by construction
  (`CONTEXT.md` §Loop freedom).
- **This target needs the FWD net plane.** It is built only when `LIBTRACER_NET_PLANE` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/route_terminus_or_forward.cpp
:language: cpp
:linenos:
```

See also: [fwd-router module](../modules/fwd-router.md) ·
[addressing](../reference/03-addressing.md) ·
[communication flows](../reference/04-communication-flows.md) ·
[the source route](route-dst-is-source-route.md) ·
[the child table](route-child-table.md).
