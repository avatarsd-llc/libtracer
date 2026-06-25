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
        BRIDGE["bridge — ROUTER + dedup + hop_count"]
        TRANSPORT["transport — loopback (M4) · socket (M5)"]
        ROUTER["router — ROUTER envelope"]
    end
    subgraph L23["L2/L3 · wire codec"]
        FRAME["frame-codec — TLV decode/encode + CRC"]
    end
    subgraph L1["L1 · views"]
        VIEWS["views — View / Rope / cast"]
    end
    subgraph L0["L0 · substrate"]
        SEG["segment — refcounted bytes + SegmentPtr"]
        BACK["backends — heap / borrowed / pool"]
    end

    APP --> GRAPH
    GRAPH --> DISP --> BRIDGE
    BRIDGE --> ROUTER --> FRAME
    BRIDGE --> TRANSPORT
    GRAPH -. "value IS a View" .-> VIEWS
    FRAME -- "cast, no copy" --> VIEWS
    VIEWS --> SEG --> BACK
    classDef done fill:#dcfce7,stroke:#166534;
    classDef next fill:#fef9c3,stroke:#92400e;
    class FRAME,VIEWS,SEG,BACK,GRAPH,DISP,BRIDGE,ROUTER done;
    class TRANSPORT next;
```

The load-bearing idea: a **TLV at L2 is a cast from an L1 View**, and an L1 View is
a refcounted window onto L0 bytes. So the wire encoding, the in-memory value, and
the graph node are **the same bytes** — moving data in-process is a refcount bump,
not a copy.

```{toctree}
:caption: L0/L1 — substrate
:maxdepth: 1

/docs/modules/segment
/docs/modules/backends
/docs/modules/views
```

```{toctree}
:caption: L2/L3 — wire codec
:maxdepth: 1

/docs/modules/frame-codec
```

```{toctree}
:caption: L4 — graph
:maxdepth: 1

/docs/modules/path
/docs/modules/graph
```

```{toctree}
:caption: L4 — transport + bridge
:maxdepth: 1

/docs/modules/transport
/docs/modules/router
/docs/modules/bridge
```
