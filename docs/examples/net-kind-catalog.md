# A kind is a NAME, resolved twice (transport plane)

Nothing in the routing plane knows the word `tcp`. A connection is created from a SPEC carrying
`kind = <name>`, and that name is looked up in two registries the **application** fills:

1. the **factory catalog** — `register_transport_type(kind, factory)` — which decides what gets
   constructed;
2. the **module declaration** — `register_module(module, kind, role)` — which decides where it
   mounts, `/net/<module>/<name>`.

Both are open and both are strict. The example registers a kind that exists nowhere in the
library, creates a connection of it from an ordinary `:children[]` SPEC, watches the routing
plane wire it up — and then asks for a kind nobody registered and gets `SCHEMA_NOT_FOUND`.

## What to notice

- **The library declares no modules, not even for its own kinds**
  ([ADR-0073](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0073-naming-authority-the-application-mints-one-predicate-gates.md) §4).
  `kTcpClientSuggestedModule` is a *suggestion* a header offers, never a registration a
  constructor performs. Until the application declares it, `module_for("tcp", DIAL)` is refused.
- **An unregistered kind is REFUSED, never defaulted.** A fallback transport would be worse than
  a failure: "some link came up" is indistinguishable from the right one until traffic silently
  goes nowhere. And nothing is registered on the way to refusing.
- **`SCHEMA_NOT_FOUND` is the one verdict for both halves** — a missing module and a missing
  factory answer the same way, because from the creator's side they are the same fact: this
  catalog has no such entry.
- **`quic` and `webtransport` are the shipped out-of-tree case.** They live in a separate
  `libtracer_quic` target that needs msquic, and nothing in the core references them
  ([ADR-0043](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0043-quic-webtransport-optional-module-msquic.md)).
  What they use to join a node is this page and nothing more — `quic_transport_factory()` passed
  to `register_transport_type`, exactly like the kind invented here. `can` is registered the same
  way.
- **The factory parses its own private keys.** Universal keys (`addr`, `port`, `role`,
  `max_frame`, …) arrive already parsed in `conn_settings_t`; a kind's private config (quic's
  `cert`/`key` PEM paths) is the factory's business, read out of the raw config TLV. That split
  is what keeps `conn_settings_t` lean (ADR-0043 §5) — no kind-specific field ever lands in the
  shared record.
- **This target needs the net plane** (`LIBTRACER_NET_PLANE`, the default) for
  `transport_vertex_t`. It opens no socket: the kind it registers is an in-process one, so
  nothing here is conditional at run time.

## Source

```{literalinclude} /core/examples/net_kind_catalog.cpp
:language: cpp
:linenos:
```

See also: [connection config](../modules/connection-config.md) ·
[transport module](../modules/transport.md) ·
[transports are vertices](../reference/19-transports-are-vertices.md) ·
[module catalog reference](../reference/10-module-catalog.md) ·
[the seam a factory has to satisfy](net-transport-seam.md).
