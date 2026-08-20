# A mount run is consumed whole (L4 routing)

A child's name does not have to be one route segment. It is a mount **path** — `"up"`,
`"ws-server/up"`, the RFC-0014 shape `"net/<module>/<name>"`, or something deeper — and the
forward path matches it as a unit: one pass over the registry, each slot tested against the
prefix of *that slot's own* width, longest match wins
([ADR-0061](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md),
[RFC-0014](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)
S2a). There is no compile-time bound on K to raise; width is bounded only by the path-depth
budget every address already spends from
([#523](https://github.com/avatarsd-llc/libtracer/issues/523)).

The half that is easy to get wrong is the other direction. **Strip-K on `dst` must be matched by
grow-K on `src`**: the hop prepends the full mount run of the link the frame arrived on, not one
route segment and not a truncation. Prepending less produces a return route that no longer names
the inbound link once names are per-module scoped — a reply that cannot get home (the ADR-0061
erratum).

## What to notice

- **Both sides of the example are qualified.** A three-wide inbound mount and a three-wide
  outbound one, checked byte-for-byte, so the assertion covers grow-K as well as strip-K. A
  one-wide inbound link would have let a truncating `src` grow pass.
- **Longest prefix, not first match.** A deeper mount registered alongside a shallower one that
  also matches takes the frame, and strips its own width. That is what keeps two modules'
  same-named connections distinct.
- **`net/<module>/<name>` is a convention, not a parse.** The registry stores the name and
  matches it as route segments; nothing in `core/` splits it into fields.
- **"Each hop strips one route segment" is the wrong sentence** and `CONTEXT.md` §Path-as-route
  lists it as vocabulary to avoid. [The source-route page](route-dst-is-source-route.md) shows
  the one-wide case because it reads more clearly; this page is the general rule.
- **This target needs the FWD net plane.** It is built only when `LIBTRACER_NET_PLANE` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/route_qualified_mount.cpp
:language: cpp
:linenos:
```

See also: [fwd-router module](../modules/fwd-router.md) ·
[transports are vertices](../reference/19-transports-are-vertices.md) ·
[addressing](../reference/03-addressing.md) ·
[the child table](route-child-table.md) ·
[multi-hop](route-multi-hop.md).
