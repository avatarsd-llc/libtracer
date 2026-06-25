# libtracer

**libtracer** is a spec-first protocol for tracing, telemetry, and pub/sub over a
single zero-copy wire format — from Cortex-M microcontrollers to Linux gateways.
A node is a graph of addressable **vertices**; the same TLV bytes are the wire
encoding, the in-memory representation, and the graph node.

This site renders the canonical project material. The **normative wire protocol**
is [the v1 spec](docs/spec/v1.md); the **descriptive** companion is the
[reference suite](docs/reference/00-overview.md); the **why** lives in the
[ADRs](docs/adr/0001-extract-reference-implementation-from-strawberry-fw.md); and
the canonical vocabulary is the [glossary](CONTEXT.md).

```{note}
When the spec and any other document disagree, the spec wins. The reference suite
is descriptive; the ADRs record decisions and rationale.
```

```{toctree}
:caption: Reference (descriptive)
:maxdepth: 2

/docs/reference/00-overview
/docs/reference/01-data-format
/docs/reference/02-graph-model
/docs/reference/03-addressing
/docs/reference/04-communication-flows
/docs/reference/05-protocol-tlvs
/docs/reference/06-user-data-packing
/docs/reference/07-host-embedding
/docs/reference/08-views-and-ownership
/docs/reference/09-memory-substrate
/docs/reference/10-module-catalog
/docs/reference/11-vertex-roles-and-aggregation
/docs/reference/README
```

```{toctree}
:caption: Specification (normative)
:maxdepth: 2

/docs/spec/v1
/docs/spec/README
/docs/spec/rfcs/0000-template
/docs/spec/rfcs/0001-v01-consistency-consolidation
/docs/spec/rfcs/0002-protocol-error-model
/docs/spec/rfcs/0003-bridged-wildcard-delivery-path
```

```{toctree}
:caption: Architecture Decisions
:maxdepth: 1

/docs/adr/0001-extract-reference-implementation-from-strawberry-fw
/docs/adr/0002-versioning-protocol-vs-release-no-per-frame-version
/docs/adr/0003-retire-list-type-code-0x05
/docs/adr/0004-crc-in-optional-trailer
/docs/adr/0005-fixed-width-length-opt-ll
/docs/adr/0006-read-write-await-api-no-connect
/docs/adr/0007-normative-wire-format-by-incorporation
/docs/adr/0008-schema-driven-array-indexing
/docs/adr/0009-built-in-error-model-tr-concept-namespace
/docs/adr/0010-closed-protocol-error-boundary
/docs/adr/0011-address-shift-totality-opt-in
/docs/adr/0012-modular-memory-binding-transparent-router
/docs/adr/0013-v1-scope-boundaries
/docs/adr/0014-router-cycle-termination-hop-count
/docs/adr/0015-graph-runtime-concurrency-and-in-process-cycle-cap
```

```{toctree}
:caption: Project
:maxdepth: 1

/README
/CONTEXT
/GOVERNANCE
/CONTRIBUTING
/MAINTAINERS
/TRADEMARKS
```
