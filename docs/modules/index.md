# C++ API reference

The per-layer API reference for the **reference C++ implementation** (`core/`).
Each page pairs a usage narrative — what the module is for, how it is used, and
what goes wrong — with the declarations rendered from the headers themselves.
For the cross-cutting view see the **[interface map](interface-map.md)**; for the
bytes on the wire see the **[bit-level walkthrough](wire-format-bits.md)**.

Pages are ordered by dependency, which is also the layer order: the cross-cutting
taxonomy and configuration first, then L0 up to the transport plane. Namespaces
mirror that model — `tr::mem` is L0, `tr::view` is L1, `tr::wire` is the L2/L3
codec, `tr::graph` is L4, and `tr::net` is the transport plane. Dependencies point
*up* the layers only.

This reference describes the C++ implementation. What an implementation in any
language must do is [the specification](../spec/index.md); the
implementation-independent model is [the reference suite](../reference/README.md).

A libtracer node is **a set of modules linked together** — there is no monolithic
"core". The implemented modules form a clean six-layer stack:

```{mermaid}
flowchart TB
    subgraph L5["L5 · application"]
        APP[your data / handlers]
    end
    subgraph L4["L4 · graph + transport"]
        GRAPH["graph — vertices, read/write/await"]
        DISP["dispatch — fan-out + field write<br/>(graph_t, not a separate module)"]
        FWD["fwd-router — FWD source-routing (RFC-0004)"]
        TRANSPORT["transport — loopback · UDP · TCP · WS · CAN"]
    end
    subgraph L23["L2/L3 · wire codec"]
        FRAME["frame-codec — TLV decode/encode + CRC"]
    end
    subgraph L1["L1 · views"]
        VIEWS["views — view_t / rope_t / cast"]
    end
    subgraph L0["L0 · substrate"]
        SEG["segment — refcounted bytes + segment_ptr_t"]
        BACK["backends — heap / borrowed / pool"]
    end

    APP --> GRAPH
    GRAPH --> DISP --> FWD
    FWD --> FRAME
    FWD --> TRANSPORT
    GRAPH -. "value IS a rope_t (scalar = 1 link)" .-> VIEWS
    FRAME -- "cast, no copy" --> VIEWS
    VIEWS --> SEG --> BACK
    classDef impl fill:#dcfce7,stroke:#166534;
    class FRAME,VIEWS,SEG,BACK,GRAPH,DISP,FWD,TRANSPORT impl;
```

The load-bearing idea: a **TLV at L2 is a cast from an L1 `view_t`**, and an L1 `view_t` is
a refcounted window onto L0 bytes. So the wire encoding, the in-memory value, and
the graph node are **the same bytes** — moving data in-process is a refcount bump,
not a copy.

Each module has its own page in the sidebar, grouped by layer:

- **L0 substrate** — [segment](segment.md) (refcounted bytes; `segment_t` is the sanctioned
  L0↔L1 boundary object — L0 owns the buffer, L1 owns the count over it, which is why that
  page is tagged L0↔L1 while the nav groups it under its allocating layer),
  [backends](backends.md) (the `mem_backend_t` byte-buffer seam **and** the `block_source_t`
  failable-block seam)
- **L1 views** — [views](views.md) (`view_t` / `rope_t` / the L1→L2 cast)
- **L2/L3 wire codec** — [frame-codec](frame-codec.md) (TLV decode/encode + CRC)
- **L4 graph** — [path](path.md) (addressing), [graph](graph.md) (vertices, read/write/await,
  dispatch), [fwd-router](fwd-router.md) (FWD source-routing and the `/net` plane)
- **L4 transport** — [transport](transport.md) (loopback · UDP · TCP · WS; QUIC / WebTransport opt-in),
  [connection config](connection-config.md) (the SPEC `config` keys a `/net:children[]` creation
  carries — universal and kind-private), [can](can.md) (the header-elided CAN stack: ID codec,
  advertise, splitter, reassembly, binding)
- **Cross-cutting** — [status & errors](status.md) (`status_t` / `result_t<T>` / `err_t`),
  [config](config.md) (the named-traits type and the policies it selects),
  [security & ACL](security-acl.md) (typed entries and the policy seam),
  [instrumentation](instrumentation.md) (the optional reachability counters)

## The dispatcher module boundary

The diagram draws dispatch as a box because it is a distinct *function*: fan-out to an
edge set and the colon-field write plane. It is not a distinct translation unit. Both
live in the graph module — `graph_t::fan_out` (`core/src/graph.cpp:1800`) and
`graph_t::field_write` (`core/src/graph.cpp:2891`) — so [graph](graph.md) is the page
that documents them, and there is no `dispatcher` source file to look for.

At the standard level the split is real: `dispatcher` is one of the **required modules**
a conforming node must supply, listed alongside the frame codec, the view machinery and
the forwarder ([../reference/10-module-catalog.md](../reference/10-module-catalog.md)).
A second implementation is free to make it its own compilation unit. This implementation
folds it into the graph runtime; the module boundary is a contract, not a file layout.

## Where to see the calls in use

Every page here describes a seam; the [worked examples](../examples/index.md) are
the same seams driven end to end in compile-tested programs — in-process pub/sub
and dispatch cost for [graph](graph.md), the codec round-trip for
[frame-codec](frame-codec.md), rope scatter-gather for [views](views.md), and a
two-node forward for [fwd-router](fwd-router.md) and [transport](transport.md).
Read a module page for the contract and an example for the shape of the call.

## Generated API reference blocks

Each page carries two descriptions of the same interface, and only one of them is
authoritative. The hand-written `## Interface` sketch is a reading aid: it names the
calls in the order a reader meets them and can lag the header. The `## API reference`
block below it is generated — `docs/conf.py` runs `doxygen core/Doxyfile` at build time
and Breathe renders the resulting XML in place — so it is the declaration as the compiler
sees it and cannot drift from `core/`. When the two disagree, the generated block is right.

```{toctree}
:caption: Cross-cutting
:hidden:
:maxdepth: 1

Interface map <interface-map>
status & errors — result taxonomy <status>
config — the build's traits type <config>
instrumentation — reachability counters <instrumentation>
```

```{toctree}
:caption: L0 — substrate
:hidden:
:maxdepth: 1

segment — refcounted bytes <segment>
backends — allocators <backends>
```

```{toctree}
:caption: L1 — views
:hidden:
:maxdepth: 1

views — view_t / rope_t / cast <views>
```

```{toctree}
:caption: L2/L3 — wire codec
:hidden:
:maxdepth: 1

frame-codec — TLV codec + CRC <frame-codec>
Wire format, bit by bit <wire-format-bits>
```

```{toctree}
:caption: L4 — graph & transport
:hidden:
:maxdepth: 1

path — addressing <path>
graph — vertices & dispatch <graph>
security & ACL — access control <security-acl>
fwd-router — FWD routing and the /net plane <fwd-router>
transport — the wire <transport>
connection config — the SPEC config keys <connection-config>
can — the header-elided CAN stack <can>
```
