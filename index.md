# libtracer

**libtracer** is a spec-first, high-performance protocol for distributed state
and messaging over a single zero-copy wire format — pub/sub streams,
read/write/await, live configuration, and introspection across one graph of
addressable **vertices**, from Cortex-M microcontrollers to Linux gateways. The
same TLV bytes are the wire encoding, the in-memory representation, and the
graph node — so an in-process hand-off moves **zero bytes**, a steady-state
forward hop makes **zero heap allocations**, and a plain write lands in **tens
of nanoseconds**.
Measured up to **9× the delivery rate of zenoh-c** at equal payloads and
semantics, with lower latency at every point — all of it CI-gated on every
merge.

```{raw} html
<script type="application/ld+json">
{
  "@context": "https://schema.org",
  "@type": "SoftwareApplication",
  "name": "libtracer",
  "description": "A decentralized, zero-copy, high-performance protocol for distributed state and messaging — pub/sub streams, read/write/await, live configuration and introspection over one graph of addressable vertices, from a 16 KB Cortex-M0 to a GPU, across vendors and transports.",
  "applicationCategory": "DeveloperApplication",
  "operatingSystem": "Cross-platform (embedded to server)",
  "url": "https://libtracer.avatarsd.com/",
  "codeRepository": "https://github.com/avatarsd-llc/libtracer",
  "license": "https://www.apache.org/licenses/LICENSE-2.0",
  "author": { "@type": "Organization", "name": "avatarsd LLC" },
  "offers": { "@type": "Offer", "price": "0", "priceCurrency": "USD" }
}
</script>
```

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

Reproducible numbers vs Eclipse Zenoh, a live auto-generated test report
across every subsystem, and the 16KB zero-heap forward gate.
:::

:::{grid-item-card} Build a node
:link: docs/getting-started
:link-type: doc

Build the C++ node and write your first vertex, pub/sub, and a two-node exchange
in about ten minutes — then browse seven compile-tested examples and the
module-by-module guide.
:::

:::{grid-item-card} Understand the model
:link: docs/reference/00-overview
:link-type: doc

The descriptive six-layer model and load-bearing architecture — the "what it is",
read as one standard, independent of any implementation.
:::

:::{grid-item-card} Compare implementations
:link: docs/capability-matrix
:link-type: doc

Which language core or platform port does what — native codecs, ports, bindings —
and how far each is verified, in one capability matrix.
:::

:::{grid-item-card} Interoperate across vendors
:link: docs/interoperability
:link-type: doc

Interop enforcement without a certification cartel: stay byte-agnostic, be nameable,
be legible — and let a developer's agent build the integration from a node's own
self-description.
:::

:::{grid-item-card} Implement the protocol
:link: docs/spec/index
:link-type: doc

The normative v1 wire protocol: byte-level TLV framing an interoperable
implementation must honor, in any language on any platform. When the spec and any
other document disagree, the spec wins.
:::

::::

```{toctree}
:caption: Getting started
:hidden:
:maxdepth: 2

Getting started <docs/getting-started>
Examples <docs/examples/index>
```

```{toctree}
:caption: Specification
:hidden:
:maxdepth: 2

The specification <docs/spec/index>
```

```{toctree}
:caption: Reference
:hidden:
:maxdepth: 2

Reference (descriptive) <docs/reference/README>
Design notes <docs/design/README>
```

```{toctree}
:caption: C++ API reference
:hidden:
:maxdepth: 2

C++ API reference <docs/modules/index>
```

```{toctree}
:caption: Interoperate
:hidden:
:maxdepth: 2

Interoperability <docs/interoperability>
Build a custom device <docs/interop/custom-device>
A production ESP32 node <docs/interop/esp32-production-node>
Capability matrix <docs/capability-matrix>
Implementation registry <docs/implementations>
```

```{toctree}
:caption: Evidence
:hidden:
:maxdepth: 1

Performance & conformance <docs/performance>
Test report <docs/test-report>
```

```{toctree}
:caption: Glossary
:hidden:
:maxdepth: 1

Context glossary <CONTEXT>
```

```{toctree}
:caption: Start here
:hidden:
:maxdepth: 1

Route by intent <docs/start-here>
```
