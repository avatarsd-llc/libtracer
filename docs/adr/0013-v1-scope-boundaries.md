# Protocol-v1 scope boundaries: no in-band capability negotiation; the module ABI is implementation-defined

Status: accepted

Two things sit deliberately **outside** the protocol-v1 standard, because the standard is the wire format and addressing alone ([00-overview.md](../reference/00-overview.md) §the standard, [01-data-format.md](../reference/01-data-format.md), [03-addressing.md](../reference/03-addressing.md)):

1. **No in-band capability negotiation.** A conforming receiver MUST already accept every `LL`/`CW`/`TF` variant and can pre-allocate worst-case buffers ([01-data-format.md](../reference/01-data-format.md) §interop); senders SHOULD default to the compact variants. So per-peer "what do you support?" negotiation buys nothing within protocol v1 — there are no other negotiable wire features (the reserved `opt` bits are forever-frozen, and any wire-incompatible change is a different protocol version selected at the discovery layer, [ADR-0002](0002-versioning-protocol-vs-release-no-per-frame-version.md)). There is no capability-discovery handshake, and none is needed.

2. **The module ABI is implementation-defined.** The C contracts between modules (`mem_backend_t`, `transport_vtable_t`, the `abi_version` field) are an implementation concern, not a protocol property; two conforming implementations need not share them — they interoperate over the wire regardless. Within one implementation the ABI is semver-stable (`tracer_abi_version`; loaders refuse mismatches). This completes [ADR-0012](0012-modular-memory-binding-transparent-router.md)'s "[backend-trait] bits are an ABI detail, not frozen here."

## Considered options

- **A capability/feature-negotiation handshake** (the `01:409` "deferred to v1.0" idea). Rejected: it would add a protocol mechanism with no function — universal-acceptance + compact-default already deliver the only benefit (fewer bytes on the wire), and a frozen v1 has nothing else to negotiate.
- **A standardized cross-implementation module ABI.** Rejected: it would force heap / MMIO / DMA / pbuf / SHM backends and every transport into one C contract for the sake of binary module portability the project does not need; the wire format already delivers cross-implementation interop, and per-implementation flexibility is worth more than binary module interchange.

## Consequences

- The `01:409` deferred-mechanism note is removed: the reference states negotiation is **unnecessary**, not pending.
- Cross-version compatibility remains entirely a discovery-layer concern ([ADR-0002](0002-versioning-protocol-vs-release-no-per-frame-version.md)): incompatible peers never establish a session, so there is nothing to negotiate per-frame.
- A future contributor proposing a capability handshake or a standardized module ABI is redirected here.
- Pairs with [ADR-0010](0010-closed-protocol-error-boundary.md) (protocol-only errors) and [ADR-0012](0012-modular-memory-binding-transparent-router.md) (transparent memory binding) — all three draw the same line: the wire is the standard; everything around it is the user's or the implementer's.
