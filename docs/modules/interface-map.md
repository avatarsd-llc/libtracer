# Interface Map

The cross-cutting view: **what each module exposes, and who depends on whom.** Every
type below is a real `core/` C++ symbol. For prose on each, follow the links.

## Dependency graph

Arrows point from a module to the module(s) whose interface it uses.

```{mermaid}
flowchart TD
    APP["application / bench"]:::app
    GRAPH["graph<br/><small>Graph · Vertex · Role</small>"]
    PATH["path<br/><small>Path · PathKey</small>"]
    BRIDGE["bridge<br/><small>Bridge</small>"]
    ROUTER["router<br/><small>router_wrap/unwrap</small>"]
    TRANSPORT["transport<br/><small>Transport · Loopback</small>"]
    FRAME["frame-codec<br/><small>Tlv · decode/encode · crc</small>"]
    VIEWS["views<br/><small>View · Rope · view_as_tlv</small>"]
    SEG["segment<br/><small>Segment · SegmentPtr</small>"]
    BACK["backends<br/><small>MemBackend · heap/borrow/pool</small>"]
    STATUS["status<br/><small>Status · Result&lt;T&gt;</small>"]

    APP --> GRAPH
    APP --> BRIDGE
    GRAPH --> PATH
    GRAPH --> VIEWS
    GRAPH --> STATUS
    BRIDGE --> GRAPH
    BRIDGE --> ROUTER
    BRIDGE --> TRANSPORT
    BRIDGE --> VIEWS
    ROUTER --> FRAME
    VIEWS --> FRAME
    VIEWS --> SEG
    FRAME --> STATUS
    SEG --> BACK
    BACK --> SEG
    PATH --> STATUS

    classDef app fill:#fce7f3,stroke:#9f1239;
    classDef done fill:#dcfce7,stroke:#166534;
    classDef next fill:#fef9c3,stroke:#92400e;
    class GRAPH,PATH,BRIDGE,ROUTER,FRAME,VIEWS,SEG,BACK,STATUS done;
    class TRANSPORT next;
```

## The seams (who hands what to whom)

```{mermaid}
flowchart LR
    subgraph egress["write path (egress)"]
        direction TB
        W1["app: Graph::write(Path, View)"] --> W2["dispatch: clone View per subscriber"]
        W2 --> W3["bridge cb: router_wrap(view.bytes())"]
        W3 --> W4["Transport::send(bytes)"]
    end
    subgraph ingress["receive path (ingress)"]
        direction TB
        R1["Transport receiver(bytes)"] --> R2["router_unwrap → meta + data span"]
        R2 --> R3["dedup + hop_count"]
        R3 --> R4["copy → SegmentPtr → View"]
        R4 --> R5["Graph::write(mount, View) → subscribers"]
    end
    egress -. "the wire" .-> ingress
```

## Consolidated interface reference

| Module | Key public interface |
| --- | --- |
| [status](#) | `enum class Status`; `template<class T> using Result = std::expected<T, Status>` |
| [backends](backends.md) | `class MemBackend { alloc(); destroy(); alignment(); … }` · `mem::heap_alloc()` · `mem::borrow()` · `mem::Pool` |
| [segment](segment.md) | `struct Segment{ RefCount; MemBackend*; span<byte> }` · `class SegmentPtr{ adopt/retain; copy=clone; reset() }` |
| [views](views.md) | `struct View{ owner; offset; length; bytes(); subview() }` · `class Rope{ append; concat; to_iovec; flatten }` · `view_as_tlv(View)→Result<Tlv>` |
| [frame-codec](frame-codec.md) | `enum class Type` · `struct Opt` · `struct Tlv{ type; opt; payload; children; trailer }` · `decode()` · `encode()` · `crc::crc32c/crc16_ccitt` |
| [path](path.md) | `class Path{ parse(); key(); field() }` · `struct PathKey` + `PathKeyHash` |
| [graph](graph.md) | `class Graph{ register_vertex; read; write; await; history; subscribe }` · `enum class Role` · `struct Settings` · `struct Handlers` |
| [transport](transport.md) | `using PeerId = array<byte,16>` · `class Transport{ send(); set_receiver() }` · `class LoopbackChannel` |
| [router](router.md) | `struct RouterMeta{ origin; ts; hop }` · `router_wrap()` · `router_unwrap()→Result<Unwrapped>` |
| [bridge](bridge.md) | `class Bridge{ export_vertex; set_mount; set_recent_set_capacity; counters }` |

## Two contracts hold the stack together

1. **A TLV is a cast from a View** — `view_as_tlv` = `decode(view.bytes())`. The
   decoder borrows; the View's `SegmentPtr` owns. So L2 (bytes) and L1 (ownership)
   meet with no copy.
2. **A Segment is reclaimed by its backend** — the only `MemBackend`→`Segment` edge
   is `destroy`, fired by `SegmentPtr` at refcount zero. So L0 (allocation) and L1
   (lifetime) meet with no policy baked into the core.

Everything above L1 (`graph`, `bridge`, transports) traffics in **`View`s and
`Tlv`s**, never raw allocation — which is why a new backend, transport, or vertex
role slots in without touching the others.

## What is implemented vs. planned

See the [module roadmap](../reference/10-module-catalog.md) — the green nodes above
are built (M1–M4); `transport`'s socket implementation is **M5**, and the wider
backend/view/transport catalog (pbuf, DMA, TCP, CAN, discovery, security) follows.
