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
        TRANSPORT["transport — loopback · UDP · WS · CAN"]
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
    GRAPH -. "value IS a view_t" .-> VIEWS
    FRAME -- "cast, no copy" --> VIEWS
    VIEWS --> SEG --> BACK
    classDef done fill:#dcfce7,stroke:#166534;
    class FRAME,VIEWS,SEG,BACK,GRAPH,DISP,FWD,TRANSPORT done;
```

The load-bearing idea: a **TLV at L2 is a cast from an L1 `view_t`**, and an L1 `view_t` is
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
:caption: L4 — transport
:maxdepth: 1

/docs/modules/transport
```
