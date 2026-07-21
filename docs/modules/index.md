# Modules

A module-by-module guide to the **reference C++ implementation** (`core/`). Each
page gives a one-paragraph summary, an extended description, the public interface
(real C++ signatures), and where the module sits. For the cross-cutting view see
the **[Interface Map](interface-map.md)**; for the bytes on the wire see the
**[bit-level walkthrough](wire-format-bits.md)**.

A libtracer node is **a set of modules linked together** — there is no monolithic
"core". The implemented modules form a clean six-layer stack:

```{mermaid}
flowchart TB
    subgraph L5["L5 · application"]
        APP[your data / handlers]
    end
    subgraph L4["L4 · graph + transport"]
        GRAPH["graph — vertices, read/write/await"]
        DISP["dispatcher — fan-out + field-write"]
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
    classDef done fill:#dcfce7,stroke:#166534;
    class FRAME,VIEWS,SEG,BACK,GRAPH,DISP,FWD,TRANSPORT done;
```

The load-bearing idea: a **TLV at L2 is a cast from an L1 `view_t`**, and an L1 `view_t` is
a refcounted window onto L0 bytes. So the wire encoding, the in-memory value, and
the graph node are **the same bytes** — moving data in-process is a refcount bump,
not a copy.

Each module has its own page in the sidebar, grouped by layer:

- **L0 substrate** — [segment](segment.md) (refcounted bytes), [backends](backends.md) (allocators)
- **L1 views** — [views](views.md) (`view_t` / `rope_t` / the L1→L2 cast)
- **L2/L3 wire codec** — [frame-codec](frame-codec.md) (TLV decode/encode + CRC)
- **L4 graph** — [path](path.md) (addressing), [graph](graph.md) (vertices, read/write/await, dispatch)
- **L4 transport** — [transport](transport.md) (loopback · UDP · TCP · WS · CAN; QUIC / WebTransport opt-in)

For the cross-cutting view of how they compose, see the **[interface map](interface-map.md)**;
for the exact bytes on the wire, the **[bit-level walkthrough](wire-format-bits.md)**.

```{toctree}
:caption: Cross-cutting
:hidden:
:maxdepth: 1

Interface map <interface-map>
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
transport — the wire <transport>
```
