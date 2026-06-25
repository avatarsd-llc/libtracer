# graph — vertices, read/write/await, dispatch (L4)

```{admonition} In one paragraph
:class: tip
The graph is the node. A **`Vertex`** is a named, addressable slot holding a value
(a `View`), a bounded history, or a user handler. The entire data API is three
calls — **`read` / `write` / `await`** — and every control surface (subscriptions,
QoS) is a **field-write** to a `:`-addressed field. `write` fans out to
subscribers by **cloning the View** (a refcount bump, no copy). Reads/writes of the
last-known-value are **lock-free**.
```

## What it does

`Graph` owns the vertex map (keyed on canonical [path](path.md) bytes). Each vertex
has a **role**: *stored-value* (last-writer-wins), *stream* (a bounded ring sized by
`:settings.history_keep_last`), or *handler* (your `on_read`/`on_write` — covering
computed, proxy, sink, live-MMIO patterns). The last-known-value slot is an
`atomic<shared_ptr<const View>>` swap, so `read`/`write` never take the per-vertex
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
enum class Role { StoredValue, Stream, Handler };
struct Settings { /* reliability, durability, history_keep_last, deadline_ns, … */ };
struct Handlers { std::function<Result<View>()> on_read;
                  std::function<Result<void>(const View&)> on_write; };

class Graph {
    Result<Vertex*> register_vertex(const Path&, Role, Handlers={}, Settings={});
    Result<View>    read (Vertex*) const;            // atomic LKV load = a clone
    Result<void>    write(Vertex*, View);            // store + fan-out
    Result<View>    await(Vertex*, std::chrono::nanoseconds);   // blocks for next write
    Result<std::vector<View>> history(Vertex*) const;          // stream window

    Result<void> subscribe(const Path& src, const Path& target);          // re-dispatch
    Result<void> subscribe(const Path& src, std::function<void(const View&)>);  // callback

    Result<void> write(Vertex*, const FieldPath&, View);  // handle-based field-write
    Result<View> read (const Path&) const;           // field tail → :schema, …
    Result<void> write(const Path&, View);           // field tail → :subscribers[]/:settings.*
};
```

```{admonition} No strings on the hot path
:class: important
The hot path is **handle-typed** (the spec's rule, `reference/10` §path-handle).
`Path::parse` encodes the canonical PATH bytes **once**; `register_vertex` /
`find` resolve a **`Vertex*`** once; then `write(v, value)` and
`write(v, fieldpath, value)` reuse those handles — **no string crafting, no parse,
no map lookup per call**. The string/`Path` overloads are init-time conveniences.
```

```cpp
// idiomatic: encode the path once, reuse the handle
auto p = Path::parse("/x:settings.reliability");        // once
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
    P->>G: write(/sensor/temp, View)
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
- **Zero-copy fan-out** — N subscribers get N clones of one `View`, not N copies.
- **The value is the bytes** — a vertex stores a `View`, so what it holds is exactly
  what goes on the wire.

See: [path](path.md), [views](views.md), [bridge](bridge.md).
