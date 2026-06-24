# Built-in error model: a `tr::` concept namespace, registered-code-or-string identity, severity + disposition in a registry

A protocol/stack error is identified within a hierarchical, **implementation-independent** namespace keyed by **stable protocol concept** — `tr::<concept>::<error>` (e.g. `tr::path::not_found`, `tr::frame::crc_fail`), where `<concept>` is one of `frame`, `tlv`, `path`, `schema`, `flow`, `access`, `transport`, `version` — **never** the raising module (which is implementation-specific and mutable). On the wire an error's **identity** takes one of two forms: a compact **registered code** (a single integer a frozen registry assigns to the whole path, for the built-in concepts) or the literal **string** path (for unregistered/third-party extensions — the unbounded case). Optional **structured detail** may attach to either form. Per-error **severity** and **disposition** (`transient` | `permanent` | `fatal`) are properties of the **registry entry**, not transmitted on the wire. The namespace is prefix-filterable like a path (`tr::flow::*`). The wire byte layout of the `ERROR` (`0x08`) TLV that carries this is specified in [RFC-0002](../spec/rfcs/0002-protocol-error-model.md).

## Considered options

- **Numeric `{module:u8, code:u16}` (a per-module byte).** Rejected: the module set is **unbounded** (third-party modules), so a fixed 1-byte module id cannot cover it, and it pins error identity to mutable internal structure.
- **Namespacing by the raising module** (`tr::graph::resolver::not_found`). Rejected: module names are **implementation-specific** — a second implementer in another language has no `resolver` module — and they change under refactor; a frozen-for-v1 namespace cannot ride on them. Stable *concepts* (the [CONTEXT.md](../../CONTEXT.md) vocabulary) are shared across all implementations.
- **Severity / disposition on the wire.** Rejected: both are fixed per error, so they belong in the registry; transmitting them wastes frozen bytes and lets a sender disagree with the registry.
- **A single identity form.** Rejected: a pure registered-code scheme can't name third-party errors (unbounded), and a pure-string scheme is wasteful for the hot built-in set. The registered-vs-string split *is* the built-in-vs-extensible split.

## Consequences

- Registered built-ins stay cheap (a `u16`); unregistered third-party errors self-identify by string with no central registry entry — extensibility without spending registry space or a wire bit.
- The `<concept>` set is small, stable, and finite by design; adding a built-in concept/error is itself an RFC-gated change to the frozen registry. Third-party additions need no RFC (string form).
- **Supersedes** the "error code as a leading child `VALUE`" shape and [RFC-0001](../spec/rfcs/0001-v01-consistency-consolidation.md) §C.1; folds in its §C.2 (`VERSION_MISMATCH` reword) and §C.3 (`INVALID`) registry items.
- Pairs with [ADR-0010](0010-closed-protocol-error-boundary.md): together they define *what* a protocol error is and *who* may raise one.
- Reuses path wildcard semantics ([03-addressing.md](../reference/03-addressing.md)) so errors are filterable/subscribable by prefix.
