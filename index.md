# libtracer

**libtracer** is a spec-first protocol for tracing, telemetry, and pub/sub over a
single zero-copy wire format — from Cortex-M microcontrollers to Linux gateways.
A node is a graph of addressable **vertices**; the same TLV bytes are the wire
encoding, the in-memory representation, and the graph node — so an in-process
hand-off moves **zero bytes**.

```{note}
When the spec and any other document disagree, the spec wins. The reference suite
is descriptive; the design rationale (ADRs) and proposals (RFCs) live in the
[repository](https://github.com/avatarsd-llc/libtracer), not this site.
```

::::{grid} 1 1 2 2
:gutter: 3

:::{grid-item-card} Evaluate it
:link: docs/performance
:link-type: doc

Honest, reproducible numbers vs Eclipse Zenoh, a live auto-generated test report
across every subsystem, and the 16KB zero-heap forward gate.
:::

:::{grid-item-card} Build a node
:link: docs/getting-started
:link-type: doc

Build the C++ node and write your first vertex, pub/sub, and a two-node exchange
in about ten minutes — then browse six compile-tested examples and the
module-by-module guide.
:::

:::{grid-item-card} Understand the model
:link: docs/reference/00-overview
:link-type: doc

The descriptive six-layer model and load-bearing architecture — the "what it is",
read as one standard, independent of any implementation.
:::

:::{grid-item-card} Implement the protocol
:link: docs/spec/v1
:link-type: doc

The normative v1 wire protocol: byte-level TLV framing an interoperable
implementation must honor, in any language on any platform. When the spec and any
other document disagree, the spec wins.
:::

::::

```{toctree}
:caption: Evaluate
:hidden:
:maxdepth: 1

Performance & conformance <docs/performance>
Test report <docs/test-report>
```

```{toctree}
:caption: Build with libtracer
:hidden:
:maxdepth: 2

Getting started <docs/getting-started>
Examples <docs/examples/index>
C++ modules <docs/modules/index>
```

```{toctree}
:caption: Understand the model
:hidden:
:maxdepth: 2

Reference (descriptive) <docs/reference/README>
```

```{toctree}
:caption: The specification (normative)
:hidden:
:maxdepth: 1

Protocol v1 — the wire format <docs/spec/v1>
About the specification <docs/spec/README>
```

```{toctree}
:caption: Glossary
:hidden:
:maxdepth: 1

Context glossary <CONTEXT>
```
