# Versioning: integer protocol version, decoupled release semver, no per-frame version field

Status: accepted

libtracer keeps two version axes separate. The **protocol** (wire format + the spec that defines it) is versioned as an **integer** — currently **v1** — and is frozen-immutable once finalized; a wire-incompatible change becomes **protocol v2**. The **release** version is independent semver living in git tags / `library.json` / `Cargo.toml` (currently **0.0.x**); a 0.0.x release is an early, partial implementation of **protocol v1**. The wire carries **no per-frame version field** — `opt` bit 7 is forever-reserved-MUST-be-zero — and peers learn the protocol version at the **discovery layer** (mDNS `_libtracer._tcp` = v1, `_libtracer-v2._tcp` = v2).

## Considered options

- **A per-frame `VR` version bit** (earlier drafts). Rejected: it permanently spends a bit, and a receiver cannot usefully act on a version bump without a parallel negotiation anyway. Discovery-layer versioning is cheaper and prevents incompatible peers from ever establishing a session.
- **Collapse protocol and release into one "v0.1" (or "v1") label.** Rejected: conflated three distinct concepts and made conformance language ambiguous.

## Consequences

- `0x06 VERSION_MISMATCH` is a discovery/bridge-level error, not a frame-parse outcome (see [0003](0003-retire-list-type-code-0x05.md) sibling registry decisions and `CONTEXT.md`).
- Every "v0.1 is the wire format" statement in `docs/reference/` and the legacy glossary is a category error and must read "protocol v1 is the wire format."
- The conformance-vector path and discovery service name are keyed to the protocol integer (`…/v1/`, `_libtracer._tcp`).
