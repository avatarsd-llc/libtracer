# Interface map

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
    TRANSPORT["transport<br/><small>transport_t · loopback/UDP/TCP/WS/CAN</small>"]
    FRAME["frame-codec<br/><small>tlv_t · decode/encode · crc</small>"]
    VIEWS["views<br/><small>view_t · rope_t · decode(view_t)</small>"]
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
    classDef lib fill:#dcfce7,stroke:#166534;
    class GRAPH,PATH,FWD,TVERT,TRANSPORT,FRAME,VIEWS,SEG,BACK,STATUS lib;
```

## The seams (who hands what to whom)

```{mermaid}
flowchart LR
    subgraph fwd["remote op (FWD source-routing, RFC-0004)"]
        direction TB
        W1["client: FWD{op, dst=/net/&lt;module&gt;/&lt;name&gt;/&lt;peer path&gt;, src, payload?}"] --> W2["fwd_router: peek the leading dst segment run (offset, no decode)"]
        W2 --> W3["child_registry_t::by_segments → transport"]
        W3 --> W4["strip the matched dst run · grow src (pooled head) · scatter-gather send"]
    end
    subgraph term["terminus (dst empty → this node)"]
        direction TB
        R1["transport receiver(bytes)"] --> R1a["wire::decode_into(frame, block_source_t&amp;)"]
        R1a --> R1b["tlv_arena_t (span nodes)"]
        R1b --> R2["op_resolver_t::resolve"]
        R2 --> R3["read/write/await the local vertex"]
        R3 --> R4["FWD{REPLY} direct-emitted, source-routed back via src"]
    end
    fwd -. "each hop over the wire" .-> term
```

## Consolidated interface reference

| Module | Key public interface |
| --- | --- |
| [status](graph.md#status-codes) | `enum class status_t`; `template<class T> using result_t = std::expected<T, status_t>` |
| [backends](backends.md) | `class mem_backend_t { alloc(); destroy(); alignment(); … }` · `view::heap_alloc()` · `view::borrow()` / `borrow_const()` · `mem::pool_t` |
| [segment](segment.md) | `struct segment_t{ ref_count_t; mem_backend_t*; span<byte> }` · `class segment_ptr_t{ adopt/retain; copy=clone; reset() }` |
| [views](views.md) | `struct view_t{ owner; offset; length; bytes(); subview() }` · `class rope_t{ append; concat; to_iovec; materialize; flatten }` · `tr::wire::decode(view_t)→std::expected<tlv_t, err_t>` |
| [frame-codec](frame-codec.md) | `enum class type_t` · `struct opt_t` · `struct tlv_t{ type; opt; payload; children; trailer }` · `decode()` · `encode()` · `decode_into(span, mem::block_source_t&)→std::expected<tlv_arena_t, err_t>` · `struct arena_tlv_t` · `crc::crc32c/crc16_ccitt` |
| [path](path.md) | `class path_t{ parse(); key(); field() }` · `bool valid_segment(string_view)` — THE segment predicate every minting boundary shares (ADR-0073 §1; the local parser and the wire creation door both call it, so they cannot drift) · `struct path_key_t` + `path_key_hash_t` |
| [graph](graph.md) | `class graph_t{ register_vertex→vertex_handle_t; try_register_vertex; retire; read; write; assign; propagate; await; history; subscribe; unsubscribe; set_delivery_mode; set_app_fields; subscribe_wire(vertex_handle_t, view_t source_view, …) }` · `class vertex_handle_t` · `enum class role_t` · `struct settings_t` (the two storage knobs) · `struct delivery_policy_t` (the per-subscription packed policy, RFC-0022) · `struct handlers_t` — `read` and `await` return `result_t<value_ref_t>` (a reference to the published value); the folding reads `read_children_folded` / `read_children_materialized` / `read_subtree_folded` compose a new value and return `result_t<rope_t>` |
| [transport](transport.md) | `using peer_id_t = array<byte,16>` · `class transport_t{ send(span); send(iov); set_receiver() }` · `class loopback_channel_t` |
| [fwd-router](fwd-router.md) | `class fwd_router_t{ fwd_router_t(graph_t&, std::pmr::memory_resource* = default, mem::block_source_t* rx = &mem::heap_source()); add_child; remove_child; on_frame; on_reply; advertise; send_compact; subscribe_toward(producer, mount_path); registry() }` — the label tables draw from the `memory_resource`, the terminus arena from the nothrow `rx` block source (ADR-0065) · `class child_registry_t{ add; erase; by_name; by_segments }` · `class op_resolver_t` · `class route_handle_t` — FWD source-routing (RFC-0004) |
| transport-vertex | `class transport_vertex_t{ register_transport_type; register_module; provide_link; set_link_state; settings_of }` · `enum class conn_role_t` · `struct conn_settings_t{ addr; port; role; keepalive_ms; max_frame; kind; … }` — a connection as a `/net/<module>/<name>` vertex (ADR-0027); a `:children[]` SPEC whose config names a transport `kind` (built-ins `udp`/`tcp`/`ws`) CONSTRUCTS and owns the real socket — the `<module>` segment comes from the application's own `register_module` declaration, never derived by the library (ADR-0073 §4); `provide_link` is the test/manual seam |

## Two contracts hold the stack together

1. **A TLV is a cast from a `view_t`** — the `decode(view_t)` overload = `decode(view.bytes())`. The
   decoder borrows; the `view_t`'s `segment_ptr_t` owns. So L2 (bytes) and L1 (ownership)
   meet with no copy.
2. **A `segment_t` is reclaimed by its backend** — the only `mem_backend_t`→`segment_t` edge
   is `destroy`, fired by `segment_ptr_t` at refcount zero. So L0 (allocation) and L1
   (lifetime) meet with no policy baked into the core.

Everything above L1 (`graph`, `fwd-router`, transports) traffics in **`view_t`s and
`tlv_t`s**, never raw allocation — which is why a new backend, transport, or vertex
role slots in without touching the others.

## Net-plane scope

The net plane is the RFC-0004 remote-operation plane: `fwd_router_t` + `child_registry_t`,
`op_resolver_t` and `route_handle_t` carry path-addressed `read` / `write` / `await` /
`subscribe` over `FWD`, and a connection is itself a vertex at `/net/<module>/<name>`
(`transport_vertex_t`, ADR-0027). The socket transports are WebSocket, UDP, TCP and CAN;
`udp`, `tcp` and `ws` are also registered as `/net` connection kinds, while CAN is bound
as a `transport_t` directly. QUIC and WebTransport are built when
`LIBTRACER_WITH_QUIC` configures the `libtracer_quic` module, which extends the kind
catalog through `register_transport_type` rather than being compiled into
`transport_vertex.cpp`. Per-module and per-implementation coverage is enumerated in the
generated [capability matrix](../capability-matrix.md); this page does not restate it.

**The net plane is explicit-source-routed `FWD` only**
([ADR-0040 — the net plane is explicit-source-routed only](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0040-net-plane-is-explicit-source-routed-only.md)):
a remote endpoint is addressed by its full path through transport-vertices
(`/net/<module>/<name>/<peer path>`), each hop stripping the `dst` segment run that named
its own link. There is no flooding and no `(origin, ts)` dedup — parallel links to one
peer are *different explicit addresses* (deliberate redundancy), not auto-multipath.
`0x0D ROUTER` is a reserved, decodable wire code with no implemented mechanism; FWD
source-routing needs no dedup.

## Pitfalls

- **This table is a hand sketch, not the generated truth.** Each module page carries an
  `## API reference` block generated from the headers; where the two disagree, the
  generated block is the one that was compiled. A signature read off this page and not
  off the header is a signature that may have moved.
- **`fwd_router_t` takes two independent allocation seams, not one.** `mr` funds the
  `route_handle` label tables; `rx` funds the terminus arena, which is built from a
  peer's frame behind no ACL. Passing a bounded pool for `mr` and leaving `rx` at the
  default heap leaves the attacker-sized allocation unbounded — the split exists because
  a `std::pmr::memory_resource` cannot report exhaustion by value, so a nothrow
  `mem::block_source_t` is what turns an over-large frame into a
  `TLV_NESTING_TOO_DEEP` reject instead of an `abort()` under `-fno-exceptions`
  (ADR-0065).
- **A connection's routing key is the whole `net/<module>/<name>` run, not one segment.**
  `by_name` / `by_segment` resolve a single link NAME and scan globally; the forward path
  matches the multi-segment run with `by_segments`. Code that assumes the first `dst`
  segment names the link addresses a connection that no longer exists at that shape.
- **`child_registry_t::erase` tombstones, it does not compact.** The slot's `link` is
  nulled and its NAME kept, so a racing lock-free reader sees the old pointer or
  `nullptr` and never a shifted vector. A re-`add` of the same name reuses the tombstone;
  a new name appends, so the table's high-water mark is the count of distinct names ever
  registered.
