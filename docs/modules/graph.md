# graph — vertices, read/write/await, dispatch (L4)

```{admonition} In one paragraph
:class: tip
The graph is the node. A **`vertex_t`** is a named, addressable slot holding a value
(a `view_t`), a bounded history, or a user handler. The entire data API is three
calls — **`read` / `write` / `await`** — and every control surface (subscriptions,
QoS) is a **field-write** to a `:`-addressed field. `write` fans out to
subscribers by **cloning the view_t** (a refcount bump, no copy). Reads/writes of the
last-known-value are **lock-free**.
```

## What it does

`graph_t` owns the vertex map (keyed on canonical [path](path.md) bytes). Each vertex
has a **role**: *stored-value* (last-writer-wins), *stream* (a bounded ring sized by
`:settings.history_keep_last`), or *handler* (your `on_read`/`on_write` — covering
computed, proxy, sink, live-MMIO patterns). The last-known-value slot is an
`atomic<shared_ptr<const view_t>>` swap, so `read`/`write` never take the per-vertex
mutex; that mutex guards only the subscriber list, history, and the `await` waiter
accounting; a per-vertex condvar makes `await` block until the next write
([ADR-0015] in the repo).

**Subscriptions are field-writes, not a verb** ([ADR-0006]): subscribing *is*
writing a `SUBSCRIBER` TLV into `:subscribers[]`. On each write the dispatcher
clones the value to every subscriber's target vertex and/or in-process callback,
with a dispatch-depth cap (32) that terminates in-process cycles. `:schema` reads
return a `POINT` descriptor.

## Interface

```cpp
enum class role_t { STORED_VALUE, STREAM, HANDLER };
struct settings_t { /* reliability, durability, history_keep_last, deadline_ns, … */ };
struct handlers_t { std::function<result_t<view_t>()> on_read;
                  std::function<result_t<void>(const view_t&)> on_write; };

class graph_t {
    result_t<vertex_t*> register_vertex(const path_t&, role_t, handlers_t={}, settings_t={});
    result_t<view_t>    read (vertex_t*) const;        // atomic LKV load = a clone
    result_t<void>    write(vertex_t*, view_t);        // store + fan-out
    result_t<view_t>    await(vertex_t*, std::chrono::nanoseconds);   // blocks for next write
    result_t<std::vector<view_t>> history(vertex_t*) const;          // stream window

    result_t<void> subscribe(const path_t& src, const path_t& target);    // re-dispatch
    result_t<void> subscribe(const path_t& src, std::function<void(const view_t&)>);  // callback

    result_t<void> write(vertex_t*, const field_path_t&, view_t);  // handle-based field-write
    result_t<view_t> read (const path_t&) const;       // field tail → :schema, …
    result_t<void> write(const path_t&, view_t);       // field tail → :subscribers[]/:settings.*
};
```

```{admonition} No strings on the hot path
:class: important
The hot path is **handle-typed** (the spec's rule, `reference/10` §path-handle).
`path_t::parse` encodes the canonical PATH bytes **once**; `register_vertex` /
`find` resolve a **`vertex_t*`** once; then `write(v, value)` and
`write(v, fieldpath, value)` reuse those handles — **no string crafting, no parse,
no map lookup per call**. The string/`path_t` overloads are init-time conveniences.
```

```cpp
// idiomatic: encode the path once, reuse the handle
auto p = path_t::parse("/x:settings.reliability");      // once
auto* v = g.find(p->key());                             // once
for (...) g.write(v, p->field(), reliable_tlv);          // hot loop — zero strings
```

## Write → fan-out

```{mermaid}
sequenceDiagram
    participant P as publisher
    participant G as graph
    participant V as /sensor/temp
    participant S1 as subscriber (callback)
    participant S2 as subscriber (target vertex)
    P->>G: write(/sensor/temp, view_t)
    G->>V: atomic LKV store (lock-free)
    G->>V: snapshot subscribers (brief lock)
    G-->>S1: callback(clone)  %% refcount bump
    G-->>S2: write(target, clone)  %% re-dispatch, depth-capped
    Note over V: await waiters woken via condvar
    G-->>P: OK
```

## Benefits

- **Three primitives** — `read`/`write`/`await` plus field-writes cover pub/sub,
  QoS, and discovery; no `connect`/`subscribe` API to mismatch.
- **Lock-free reads** — the LKV is an atomic pointer swap; readers never block on
  writers (validated race-free under TSan).
- **Zero-copy fan-out** — N subscribers get N clones of one `view_t`, not N copies.
- **The value is the bytes** — a vertex stores a `view_t`, so what it holds is exactly
  what goes on the wire.

See: [path](path.md), [views](views.md), [bridge](bridge.md).
