# The protocol error namespace is closed: applications signal failure as data, not as protocol errors

The `tr::` error namespace ([ADR-0009](0009-built-in-error-model-tr-concept-namespace.md)) is **protocol-only**. Application code never emits a protocol `ERROR`, and there is **no user/application error-code range**. An application that needs to signal a failure does so in its **own data** — an ordinary value written into the graph, whose meaning is described by the **application's schema** — exactly as the protocol defines no application *data types* (a payload's format is described at the user level, not by the wire; see [06-user-data-packing.md](../reference/06-user-data-packing.md)). Protocol errors describe failures of the libtracer **stack itself** (framing, TLV structure, addressing, schema, flow, access, transport, version); application failures are application data.

## Considered options

- **A user/application error-code range** (`0x80–0xFF`, mirroring the type-code user range). Rejected: it would invite applications to encode domain failures in a protocol registry the protocol can neither define nor version, duplicating the data plane. libtracer is a transport for application data, not a definer of application semantics — the same reason it carries opaque, schema-described payloads rather than typed values.

## Consequences

- The error registry is **entirely built-in** and finite by concept; no reserved user range exists or is needed.
- **Symmetry with the data plane:** the protocol carries opaque, schema-described application payloads and does not interpret them — errors included. "Is it an app error?" is answered at the user level by format/schema, never by a protocol code (the "we don't define a JSON type" principle).
- A future contributor proposing a "user error code" is redirected to the data plane; this ADR is the standing answer.
- Bounds [ADR-0009](0009-built-in-error-model-tr-concept-namespace.md): the registered-code space need only cover protocol concepts, and the string form covers third-party *stack* extensions — neither is for application errors.
