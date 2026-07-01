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
    FWD["fwd-router<br/><small>fwd_router_t · child_registry_t</small>"]
    TVERT["transport-vertex<br/><small>transport_vertex_t (/net)</small>"]
    TRANSPORT["transport<br/><small>transport_t · loopback/UDP/WS/CAN</small>"]
    FRAME["frame-codec<br/><small>tlv_t · decode/encode · crc</small>"]
    VIEWS["views<br/><small>view_t · rope_t · view_as_tlv</small>"]
    SEG["segment<br/><small>segment_t · segment_ptr_t</small>"]
    BACK["backends<br/><small>mem_backend_t · heap/borrow/pool</small>"]
    STATUS["status<br/><small>status_t · result_t&lt;T&gt;</small>"]

    APP --> GRAPH
    APP --> FWD
    GRAPH --> PATH
    GRAPH --> VIEWS
    GRAPH --> STATUS
    FWD --> GRAPH
    FWD --> FRAME
    FWD --> TRANSPORT
    TVERT --> FWD
    TVERT --> GRAPH
    VIEWS --> FRAME
    VIEWS --> SEG
    FRAME --> STATUS
    SEG --> BACK
    BACK --> SEG
    PATH --> STATUS

    classDef app fill:#fce7f3,stroke:#9f1239;
    classDef done fill:#dcfce7,stroke:#166534;
    class GRAPH,PATH,FWD,TVERT,TRANSPORT,FRAME,VIEWS,SEG,BACK,STATUS done;
```

## The seams (who hands what to whom)

```{mermaid}
flowchart LR
    subgraph fwd["remote op (FWD source-routing, RFC-0004)"]
        direction TB
        W1["client: FWD{op, dst=/net/&lt;conn&gt;/&lt;path&gt;, src, payload?}"] --> W2["fwd_router: peek first dst seg (offset, no decode)"]
        W2 --> W3["child_registry_t::by_segment → transport"]
        W3 --> W4["strip dst seg · grow src (pooled head) · scatter-gather send"]
    end
    subgraph term["terminus (dst empty → this node)"]
        direction TB
        R1["transport receiver(bytes)"] --> R2["op_resolver_t::resolve"]
        R2 --> R3["read/write/await the local vertex"]
        R3 --> R4["FWD{REPLY} source-routed back via src"]
    end
    fwd -. "each hop over the wire" .-> term
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
| fwd-router | `class fwd_router_t{ add_child; on_frame; on_reply; advertise; send_compact; registry() }` · `class child_registry_t{ add; by_name; by_segment }` · `struct op_resolver_t` — FWD source-routing (RFC-0004) |
| transport-vertex | `class transport_vertex_t{ provide_link; set_link_state; settings_of }` · `enum class conn_role_t` · `struct conn_settings_t` — a connection as a `/net/<conn>` vertex (ADR-0027) |

## Two contracts hold the stack together

1. **A TLV is a cast from a `view_t`** — `view_as_tlv` = `decode(view.bytes())`. The
   decoder borrows; the `view_t`'s `segment_ptr_t` owns. So L2 (bytes) and L1 (ownership)
   meet with no copy.
2. **A `segment_t` is reclaimed by its backend** — the only `mem_backend_t`→`segment_t` edge
   is `destroy`, fired by `segment_ptr_t` at refcount zero. So L0 (allocation) and L1
   (lifetime) meet with no policy baked into the core.

Everything above L1 (`graph`, `fwd-router`, transports) traffics in **`view_t`s and
`tlv_t`s**, never raw allocation — which is why a new backend, transport, or vertex
role slots in without touching the others.

## What is implemented vs. planned

See the [module roadmap](../reference/10-module-catalog.md). The graph core (M1–M4) is
built; the socket transports landed too — **WebSocket** (`transport_ws`, the
browser↔robot keystone), **UDP** (`udp_transport_t`), and **CAN** (`transport_can`,
SocketCAN). The **RFC-0004 remote-operation plane** (`fwd_router_t` + `child_registry_t`,
`op_resolver_t`, `route_handle_t` — path-addressed `read`/`write`/`await`/`subscribe`
over `FWD`) is the net plane, with connections exposed as `/net/<conn>` vertices
(`transport_vertex_t`, ADR-0027). Still ahead: a reliable byte-stream transport
(TCP/QUIC), and the wider backend/discovery/security catalog (pbuf, DMA, mDNS, TLS).

**The net plane is explicit-source-routed `FWD` only**
([ADR-0040](../adr/0040-net-plane-is-explicit-source-routed-only.md)): a remote endpoint
is addressed by its full path through transport-vertices (`/net/<conn>/<peer path>`),
each hop stripping its `dst` segment. There is no flooding and no `(origin, ts)` dedup —
parallel links to one peer are *different explicit addresses* (deliberate redundancy),
not auto-multipath. The M4 `bridge_t` + ROUTER-flood model was **retired** with ADR-0040;
the `0x0D ROUTER` wire code stays reserved for a possible future flooding profile.
