# The `src` you accumulated is the way home (L4 routing)

Every hop that stripped a `dst` mount run prepended its own name for the inbound link to `src`.
By the time the request reaches the terminus, `src` spells the whole way back — in the frame, in
the hands of the node that has to answer. So the terminus builds `FWD{REPLY}` with
`dst = the request's src` and sends it back over the bidirectional link the request arrived on;
each hop retraces its own link the same way, consuming one mount run of that `dst` as it goes.
**No reply address and no correlation id are needed anywhere**
([RFC-0004](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md)
§D, `CONTEXT.md` §Path-as-route).

That is the other half of the statelessness on [the multi-hop page](route-multi-hop.md). A
forwarder with a request table would need an entry per in-flight request, a timeout to reap it,
and a policy for what happens when it overflows.

## What to notice

- **The `src`→`dst` handoff is asserted directly.** The example re-encodes the `PATH` child of
  each frame and compares: the request's `src` is `/app`, and the reply's `dst` is `/app`.
- **The reply's own `src` names the responder.** `/sensor/temp`, not the terminus node — which
  is how an answer carries its provenance without a separate field.
- **The reply is handed over rope-native.** `on_reply` receives a `rope_t`; the router performs
  no decode and no flatten
  ([ADR-0055](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0055-rope-native-reply-and-control-egress-sinks.md)).
  A sink that wants contiguous bytes calls `materialize()` once and keeps the view alive while
  it reads — a single-link reply, the common case, costs no copy at all.
- **The route terminates by running out.** At the origin, `app` names no child, so the reply is
  terminal and the sink fires. Nothing marks a frame as "the last hop"; the address does.
- **Neither node stored anything.** Both routers report zero bindings after the round trip.
- **This target needs the FWD net plane.** It is built only when `LIBTRACER_NET_PLANE` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/route_reply_home.cpp
:language: cpp
:linenos:
```

See also: [fwd-router module](../modules/fwd-router.md) ·
[communication flows](../reference/04-communication-flows.md) ·
[views and ownership](../reference/08-views-and-ownership.md) ·
[the source route](route-dst-is-source-route.md) ·
[terminus or forward](route-terminus-or-forward.md).
