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
- **[Examples](docs/examples/index.md)** — six worked, compile-tested programs (pub/sub and
  fan-out, the wire codec, rope scatter-gather, two-node FWD delivery), each built and
  smoke-tested in CI, several reporting live latency/throughput.
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

Getting started <docs/getting-started>
Performance & conformance <docs/performance>
Test report <docs/test-report>
```

```{toctree}
:caption: Modules (per-module reference)
:maxdepth: 1

Overview & layer map <docs/modules/index>
Interface map <docs/modules/interface-map>
segment — refcounted bytes (L0) <docs/modules/segment>
backends — allocators (L0) <docs/modules/backends>
views — view_t / rope_t (L1) <docs/modules/views>
frame-codec — TLV wire codec (L2/L3) <docs/modules/frame-codec>
path — addressing (L4) <docs/modules/path>
graph — vertices & dispatch (L4) <docs/modules/graph>
transport — the wire (L4) <docs/modules/transport>
Wire format, bit by bit <docs/modules/wire-format-bits>
```

```{toctree}
:caption: Examples
:maxdepth: 1

Overview <docs/examples/index>
In-process pub/sub <docs/examples/in-process-pubsub>
Pub/sub fan-out & dispatch cost <docs/examples/pubsub-fanout>
Wire codec round-trip <docs/examples/wire-roundtrip>
Wire codec deep-dive & throughput <docs/examples/wire-codec>
Rope scatter-gather <docs/examples/rope-scatter>
Two nodes over a wire — FWD delivery <docs/examples/two-node-fwd>
```

```{toctree}
:caption: Reference (descriptive)
:maxdepth: 1

Reading guide <docs/reference/README>
Overview — the six-layer model <docs/reference/00-overview>
Data format (L2) <docs/reference/01-data-format>
Graph model (L4) <docs/reference/02-graph-model>
Addressing (L4) <docs/reference/03-addressing>
Communication flows (L4) <docs/reference/04-communication-flows>
Protocol-defined TLVs (L3) <docs/reference/05-protocol-tlvs>
User data packing (L4/L5) <docs/reference/06-user-data-packing>
Host embedding (L4) <docs/reference/07-host-embedding>
Views & ownership (L1) <docs/reference/08-views-and-ownership>
Memory substrate (L0) <docs/reference/09-memory-substrate>
Module catalog & composition <docs/reference/10-module-catalog>
Vertex roles & aggregation (L4) <docs/reference/11-vertex-roles-and-aggregation>
Deployment profiles <docs/reference/12-deployment-profiles>
Network formation (L4) <docs/reference/13-network-formation>
CAN transport <docs/reference/14-can-transport>
```

```{toctree}
:caption: Specification (normative)
:maxdepth: 1

Protocol v1 — the wire format <docs/spec/v1>
About the specification <docs/spec/README>
```

```{toctree}
:caption: Glossary
:maxdepth: 1

Context glossary <CONTEXT>
```
