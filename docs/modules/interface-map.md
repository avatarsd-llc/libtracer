# Interface Map

The cross-cutting view: **what each module exposes, and who depends on whom.** Every
type below is a real `core/` C++ symbol. For prose on each, follow the links.

## Dependency graph

Arrows point from a module to the module(s) whose interface it uses.

```{mermaid}
flowchart TD
    APP["application / bench"]:::app
    GRAPH["graph<br/><small>graph_t · vertex_t · role_t</small>"]
    PATH["path<br/><small>path_t · path_key_t</small>"]
    BRIDGE["bridge<br/><small>bridge_t</small>"]
    ROUTER["router<br/><small>router_wrap/unwrap</small>"]
    TRANSPORT["transport<br/><small>transport_t · loopback_channel_t</small>"]
    FRAME["frame-codec<br/><small>tlv_t · decode/encode · crc</small>"]
    VIEWS["views<br/><small>view_t · rope_t · view_as_tlv</small>"]
    SEG["segment<br/><small>segment_t · segment_ptr_t</small>"]
    BACK["backends<br/><small>mem_backend_t · heap/borrow/pool</small>"]
    STATUS["status<br/><small>status_t · result_t&lt;T&gt;</small>"]

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
        W1["app: graph_t::write(path_t, view_t)"] --> W2["dispatch: clone view_t per subscriber"]
        W2 --> W3["bridge cb: router_wrap(view.bytes())"]
        W3 --> W4["transport_t::send(bytes)"]
    end
    subgraph ingress["receive path (ingress)"]
        direction TB
        R1["transport_t receiver(bytes)"] --> R2["router_unwrap → meta + data span"]
        R2 --> R3["dedup + hop_count"]
        R3 --> R4["copy → segment_ptr_t → view_t"]
        R4 --> R5["graph_t::write(mount, view_t) → subscribers"]
    end
    egress -. "the wire" .-> ingress
```

## Consolidated interface reference

| Module | Key public interface |
| --- | --- |
| [status](#) | `enum class status_t`; `template<class T> using result_t = std::expected<T, status_t>` |
| [backends](backends.md) | `class mem_backend_t { alloc(); destroy(); alignment(); … }` · `view::heap_alloc()` · `view::borrow()` · `mem::pool_t` |
| [segment](segment.md) | `struct segment_t{ ref_count_t; mem_backend_t*; span<byte> }` · `class segment_ptr_t{ adopt/retain; copy=clone; reset() }` |
| [views](views.md) | `struct view_t{ owner; offset; length; bytes(); subview() }` · `class rope_t{ append; concat; to_iovec; flatten }` · `view_as_tlv(view_t)→result_t<tlv_t>` |
| [frame-codec](frame-codec.md) | `enum class type_t` · `struct opt_t` · `struct tlv_t{ type; opt; payload; children; trailer }` · `decode()` · `encode()` · `crc::crc32c/crc16_ccitt` |
| [path](path.md) | `class path_t{ parse(); key(); field() }` · `struct path_key_t` + `path_key_hash_t` |
| [graph](graph.md) | `class graph_t{ register_vertex; read; write; await; history; subscribe }` · `enum class role_t` · `struct settings_t` · `struct handlers_t` |
| [transport](transport.md) | `using peer_id_t = array<byte,16>` · `class transport_t{ send(); set_receiver() }` · `class loopback_channel_t` |
| [router](router.md) | `struct router_meta_t{ origin; ts; hop }` · `router_wrap()` · `router_unwrap()→result_t<unwrapped_t>` |
| [bridge](bridge.md) | `class bridge_t{ export_vertex; set_mount; set_recent_set_capacity; counters }` |

## Two contracts hold the stack together

1. **A TLV is a cast from a `view_t`** — `view_as_tlv` = `decode(view.bytes())`. The
   decoder borrows; the `view_t`'s `segment_ptr_t` owns. So L2 (bytes) and L1 (ownership)
   meet with no copy.
2. **A `segment_t` is reclaimed by its backend** — the only `mem_backend_t`→`segment_t` edge
   is `destroy`, fired by `segment_ptr_t` at refcount zero. So L0 (allocation) and L1
   (lifetime) meet with no policy baked into the core.

Everything above L1 (`graph`, `bridge`, transports) traffics in **`view_t`s and
`tlv_t`s**, never raw allocation — which is why a new backend, transport, or vertex
role slots in without touching the others.

## What is implemented vs. planned

See the [module roadmap](../reference/10-module-catalog.md). The graph + bridge core
(M1–M4) is built; the socket transports landed too — **WebSocket** (`transport_ws`,
the browser↔robot keystone), **UDP** (`udp_transport_t`), and **CAN** (`transport_can`,
SocketCAN). The RFC-0004 remote-operation plane (`op_resolver_t`, `fwd_router_t`,
`route_handle_t` — path-addressed `read`/`write`/`await`/`subscribe` over `FWD`) is
built on top of those. Still ahead: a reliable byte-stream transport (TCP/QUIC), and
the wider backend/discovery/security catalog (pbuf, DMA, mDNS, TLS).
