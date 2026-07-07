# libtracer

**libtracer** is a spec-first protocol for tracing, telemetry, and pub/sub over a
single zero-copy wire format — from Cortex-M microcontrollers to Linux gateways.
A node is a graph of addressable **vertices**; the same TLV bytes are the wire
encoding, the in-memory representation, and the graph node — so an in-process
hand-off moves **zero bytes**.

This site is the public documentation:

- **[Getting started](docs/getting-started.md)** — build the C++ node and write your
  first vertex, pub/sub, and a two-node exchange in ~10 minutes.
- **[Performance](docs/performance.md)** — honest, reproducible numbers vs Eclipse
  Zenoh (in-process zero-copy, and the network latency/throughput trade).
- **[Test report](docs/test-report.md)** — a live, auto-generated view of every test
  suite by subsystem, plus the sanitizer matrix and the 16KB zero-heap forward gate.
- **[Modules](docs/modules/index.md)** — a module-by-module guide to the reference
  C++ implementation, the [interface map](docs/modules/interface-map.md), and a
  hands-on [bit-level wire walkthrough](docs/modules/wire-format-bits.md).
- **[Examples](docs/examples/index.md)** — worked, compile-tested example programs
  (in-process pub/sub, wire codec round-trip), each built and smoke-tested in CI.
- **[Reference](docs/reference/00-overview.md)** — the descriptive six-layer model
  and architecture (the "what it is").
- **[Specification](docs/spec/v1.md)** — the normative v1 wire protocol.
- **[Glossary](CONTEXT.md)** — the canonical vocabulary.

```{note}
When the spec and any other document disagree, the spec wins. The reference suite
is descriptive; the design rationale (ADRs) and proposals (RFCs) live in the
[repository](https://github.com/avatarsd-llc/libtracer), not this site.
```

```{toctree}
:caption: Getting started
:maxdepth: 1

/docs/getting-started
/docs/performance
/docs/test-report
```

```{toctree}
:caption: Modules
:maxdepth: 2

/docs/modules/index
/docs/modules/interface-map
/docs/modules/wire-format-bits
```

```{toctree}
:caption: Examples
:maxdepth: 2

/docs/examples/index
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
/docs/reference/12-deployment-profiles
/docs/reference/13-network-formation
/docs/reference/14-can-transport
/docs/reference/README
```

```{toctree}
:caption: Specification (normative)
:maxdepth: 2

/docs/spec/v1
/docs/spec/README
```

```{toctree}
:caption: Glossary
:maxdepth: 1

/CONTEXT
```
