# graph — vertices, read/write/await, dispatch (L4)

```{admonition} In one paragraph
:class: tip
The graph is the node. A **`vertex_t`** is a named, addressable slot holding a value
(a `rope_t` — a contiguous scalar is the single-link case), a bounded history, or a
user handler. The data surface is **`read` / `write` / `await`** over the value, plus
**`assign` / `propagate`** when the state transition and the edge transition are wanted
separately; every control surface (subscriptions, QoS) is a **field-write** to a
`:`-addressed field. `write` fans out to subscribers by **cloning the value** (a refcount
bump, no copy). The last-known-value path takes **no per-vertex mutex**.
```

## What it does

`graph_t` owns the vertex map (keyed on canonical [path](path.md) bytes). Each vertex
has a **role**: *stored-value* (last-writer-wins), *stream* (a bounded ring sized by
`:settings.history_keep_last`), or *handler* (`on_read` / `on_write` — covering
computed, proxy, sink, live-MMIO patterns). The last-known-value slot is an
`atomic<shared_ptr<const rope_t>>` swap, so `read` / `write` of the value take **no
per-vertex mutex**; that mutex guards the subscriber list, the history ring and the
`await` waiter accounting, and a per-vertex condvar makes `await` block until the next
write.

The slot is not free of serializing instructions. `std::atomic<std::shared_ptr<T>>` is
not lock-free on libstdc++, so both load and store take its internal pointer-lock bit —
"lock-free by contract, spin-locked in practice" (`sp_atomic_slot_t`,
`core/include/libtracer/lkv_slot.hpp:99-104`). The claim the code supports is the mutex
one, not an absence of contention; the cost of that spin and the policy that replaces it
on a host are in [design/concurrency](../design/concurrency/README.md).

**Subscriptions are field-writes, not a verb**
([ADR-0006 — read/write/await API, no connect](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0006-read-write-await-api-no-connect.md)):
subscribing *is* writing a `SUBSCRIBER` TLV into `:subscribers[]`. On each write the
dispatcher clones the value to every subscriber's target vertex and in-process callback.
A delivery **terminates at its target** — store and notify, never a re-dispatch to the
target's own `:subscribers[]` — so a dispatch-level cycle cannot form and there is no
depth cap to tune (`core/include/libtracer/graph.hpp:78-83`;
[ADR-0051 — delivery terminates at target, no dispatch limits](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0051-delivery-terminates-at-target-no-dispatch-limits.md),
[RFC-0007 — delivery terminates at target](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0007-delivery-terminates-at-target.md)).
Propagation past a target is exclusively the target's own logic — a controller
re-emitting on its execution. `:schema` reads return a `POINT` descriptor.

## Interface

```cpp
enum class role_t { STORED_VALUE, STREAM, HANDLER };
enum class delivery_mode_t { IF_NEWER, UNCONDITIONAL, EXPLICIT };

struct settings_t {  // widest-first; fixed-width, so identical on 32- and 64-bit
    std::uint64_t deadline_ns;         // 0 = off; max ns between writes before a liveness fault
    std::uint32_t history_keep_last;   // stream ring depth (>= 1)
    std::uint32_t queue_max_bytes;     // 0 = unbounded; per-subscriber back-pressure cap
    std::uint32_t store_ref_min_bytes; // 0 = off; store-by-reference threshold
    std::uint8_t  reliability;         // 0 = best-effort, 1 = reliable
    std::uint8_t  durability;          // 0 = volatile, 1 = transient-local
    std::uint8_t  priority;            // transport hint, not a wire bit
};

struct handlers_t {                                       // four seams, not two
    std::function<result_t<rope_t>()>                   on_read;
    std::function<result_t<void>(const rope_t&)>        on_write;
    std::function<result_t<view_t>()>                   on_children;
    std::function<void(std::string_view, const view_t&)> on_app_field_write;
};

using subscriber_fn_t = void (*)(void* ctx, const rope_t& value);
struct subscription_t { /* opaque: producer vertex + :subscribers[] slot index */ };

class graph_t {
    explicit graph_t(std::pmr::memory_resource* mr    = std::pmr::get_default_resource(),
                     mem::mem_backend_t* value_backend = &mem::heap_backend(),
                     mem::block_source_t* ctl          = &mem::heap_source());

    // registration and removal
    vertex_handle_t register_vertex(const path_t&, role_t, handlers_t = {}, settings_t = {});
    result_t<vertex_handle_t> try_register_vertex(const path_t&, role_t,
                                                  handlers_t = {}, settings_t = {});
    result_t<void> retire(vertex_handle_t);                       // logical absence, subtree-wide
    std::uint32_t  retire_generation(vertex_handle_t) const noexcept;
    void           collect();                    // free the parked value seams — CALLER-timed
    std::size_t    parked_seam_count() const;    // how many await a collect()
    std::optional<vertex_handle_t> find(std::span<const std::byte> key) const;

    // value plane
    result_t<value_ref_t> read (vertex_handle_t, std::string_view caller = {}) const;
    result_t<void>        write(vertex_handle_t, rope_t, std::string_view caller = {});
    result_t<value_ref_t> await(vertex_handle_t, std::chrono::nanoseconds,
                                std::string_view caller = {});
    result_t<void>        assign(vertex_handle_t, rope_t, std::string_view caller = {});
    void                  propagate(vertex_handle_t);
    void                  set_delivery_mode(vertex_handle_t, delivery_mode_t);
    result_t<std::vector<rope_t>> history(vertex_handle_t) const;   // stream window

    // composed reads — they build a value, so they return one
    result_t<rope_t> read_children_folded(vertex_handle_t) const;
    result_t<rope_t> read_children_materialized(vertex_handle_t) const;
    result_t<rope_t> read_subtree_folded(vertex_handle_t, ...) const;

    // field plane (`:`-addressed)
    result_t<rope_t> read (vertex_handle_t, const field_path_t&, ...) const;
    result_t<void>   write(vertex_handle_t, const field_path_t&, rope_t,
                           std::string_view caller = {});
    result_t<value_ref_t> read (const path_t&) const;               // field tail → :schema, …
    result_t<void>        write(const path_t&, rope_t);             // → :subscribers[], :settings.*
    result_t<value_ref_t> await(const path_t&, std::chrono::nanoseconds);

    // subscriptions
    result_t<void>           subscribe(const path_t& src, const path_t& target);
    result_t<subscription_t> subscribe(const path_t& src, subscriber_fn_t fn, void* ctx);
    template <typename F>
    result_t<subscription_t> subscribe(const path_t& src, F& callback);   // lvalue only
    result_t<void>           unsubscribe(const subscription_t&);
};
```

There is no `std::function` subscribe overload and no `result_t<void>` callback form. The
per-edge sink is a `{fn, ctx}` pair so the per-publish edge snapshot under the fan-out lock
is a trivial copy rather than a `std::function` clone that heap-allocates once captures
exceed the small-buffer size
([ADR-0047 — build-time closed module sets, compile-time seams](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md)).
The templated overload binds `callback` **by address**, so it takes an lvalue only — a
temporary lambda does not compile.

```{admonition} `ctx` must outlive every possible delivery
:class: warning
Subscription edges are never destroyed while the graph lives. `unsubscribe` only
**deactivates** the slot; an in-flight delivery has already snapshotted the edge and
completes. The caller-owned `ctx` (or, for the templated overload, the callable itself)
must therefore stay alive past any delivery that may still be running, not merely past
the `unsubscribe` call (`core/include/libtracer/graph.hpp:552-555`).
```

```{admonition} No strings on the hot path
:class: important
The hot path is **handle-typed** (the spec's rule,
[reference/03](../reference/03-addressing.md) §static path handles).
A `path_t` encodes the canonical PATH bytes **once** — the `path_t(std::string_view)`
constructor for a known-good literal
([ADR-0054 — path_t parse-once constructor](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0054-path-t-parse-once-constructor.md)),
or the fallible `path_t::parse` for a runtime string; `register_vertex` / `find` resolve a
**`vertex_handle_t`** once; then `write(v, value)` and `write(v, fieldpath, value)` reuse
those handles — **no string crafting, no parse, no map lookup per call**. The
string/`path_t` overloads are init-time conveniences.
```

```{admonition} Injected memory — no allocator baked in
:class: note
`graph_t`'s constructor takes three memory seams, all defaulted to the standard heap (a
host that passes nothing gets zero-churn, byte-identical behavior):

- a `std::pmr::memory_resource*` for the per-write **control objects** — the LKV control
  block and the `rope_t` wrapper
  ([ADR-0039 — pmr memory model, host-aligned allocation](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0039-pmr-memory-model-host-aligned-allocation.md));
- a `mem::mem_backend_t* value_backend` for the durable **value bytes** the write path
  copies into the LKV when a borrowed-delivery transport forces the copy
  ([ADR-0060 — LKV copy store, injected value backend](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0060-lkv-copy-store-injected-value-backend.md)); and
- a `mem::block_source_t* ctl` — the **nothrow** source for allocations a peer can
  provoke, which report exhaustion by value instead of throwing
  ([ADR-0065 — failable allocation gets its own seam, block_source](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0065-failable-allocation-gets-its-own-seam-block-source.md)).
  Read it back with `control_source()`.

The parameters are **appended** in that order, so an existing `graph_t{&mr}` keeps
compiling and picks up the defaults.

A bounded target points all three — and the transport-receive backend — at one static slab;
pool exhaustion surfaces as `BACKPRESSURE`, never a silent heap fallback. See
[reference/09](../reference/09-memory-substrate.md) §the injection points.
```

```cpp
// idiomatic: encode the path once (parse-once ctor), reuse the handle
path_t p("/x:settings.reliability");                     // once — no *-deref
auto v = *g.find(p.key());                               // once — find → optional<vertex_handle_t>
for (...) g.write(v, p.field(), reliable_tlv);           // hot loop — zero strings
```

## What a read hands back

`read` and `await` return `result_t<value_ref_t>`, not `result_t<rope_t>`
(`core/include/libtracer/graph.hpp:392,444` by handle, `:728,734` by path;
`value_ref_t` at `core/include/libtracer/vertex.hpp:83`). A `value_ref_t` is an **owning
reference** to the value the vertex published: the LKV slot holds it as a
`std::shared_ptr<const rope_t>`, so handing that reference back costs a refcount clone of
one control block instead of one `segment_ptr_t` clone per link.

The rule, and the reason the API is not uniform:

> **A read of a published value returns a reference to it; a read that composes a new
> value returns the value.**

`read_children_folded`, `read_children_materialized` and `read_subtree_folded` compose a
tree no vertex ever published, so there is no object to reference and they still return
`result_t<rope_t>`. The field-read overload likewise serves a control TLV as a `rope_t`.
The rejected alternative was a uniform `rope_t` return: it makes every read of a shared
vertex pay a contended refcount read-modify-write per link, on a cache line every reader
of that vertex shares, so its cost grows with links **and** with readers.

Spelling a read of a single-link value:

```cpp
auto got = g.read(v);                       // result_t<value_ref_t>
if (!got) return got.error();
std::span<const std::byte> b = (*got)->only().bytes();   // (*got) → const rope_t&
```

`operator*` yields the `rope_t`, `operator->` reaches its members; `only()` is the
single-link accessor (zero copy) and `materialize()` the general one. `(*got)->only()`
is the correct spelling — one dereference for the `result_t`, one for the reference.

```{admonition} A held reference pins the value
:class: warning
Holding a `value_ref_t` keeps the value alive, exactly as the reader's own copy did. Under
an **injected `std::pmr::memory_resource`** that is a real obligation rather than a
formality: the value was allocated from the graph's resource, so an outstanding reference
pins that allocation and defers its reclamation
([ADR-0069 — LKV slot is a compile-time policy, hazard reclamation](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0069-lkv-slot-is-a-compile-time-policy-hazard-reclamation.md)).
A reader that parks a `value_ref_t` in long-lived state holds a bounded pool's block for
that long.
```

## Assign and propagate

`write` is not irreducible. It is `assign` — the **state** transition — followed by
delivery — the **edge** transition
([RFC-0008 — vertex operations, assign and propagate](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0008-vertex-operations-assign-propagate.md) §D).
Splitting them is what makes "update many fields, notify once" expressible without a
notion of a batch:

| Call | State | Edges |
| --- | --- | --- |
| `assign(v, value)` | swaps the LKV, appends to the stream ring, bumps the write sequence (waking `await`), marks `v` for the next covering sweep | none |
| `propagate(v)` | none | delivers `v`'s current value, plus the qualifying descendants of `v`'s subtree |
| `write(v, value)` | as `assign` | delivers immediately |

`assign` is WRITE-gated like `write` and is **never** gated by `delivery_mode`. A branch
`POINT` decomposes and assigns each descendant, notifying nothing. `propagate` takes no
value — it reads the last-known-value — and always delivers the vertex named, because that
vertex is the explicit target; the mode gates only what an **ancestor's** sweep sweeps up.
Its cost is O((pending + unconditional) in subtree).

`set_delivery_mode(v, mode)` sets that per-vertex policy. It is a wiring-time call, like
settings.

| `delivery_mode_t` | An ancestor's sweep includes this vertex |
| --- | --- |
| `IF_NEWER` (default) | only if it was assigned since the last covering sweep — the structural coalescing flush |
| `UNCONDITIONAL` | always, at the sweep's rate — a sweep-driven keepalive |
| `EXPLICIT` | never; deliverable only by a direct `propagate` on the vertex itself |

The mode is a **structural** filter, not a value filter: nothing here compares bytes, and
a vertex never parses its own bytes. Numeric filtering (a deadband) is an application
filter vertex, never a field here. The protocol half of this model — what a peer sees, and
how coalescing composes across a link — is
[reference/02](../reference/02-graph-model.md).

## Write and fan-out

```{mermaid}
sequenceDiagram
    participant P as publisher
    participant G as graph
    participant V as /sensor/temp
    participant S1 as subscriber (callback)
    participant S2 as subscriber (target vertex)
    P->>G: write(/sensor/temp, rope_t)
    G->>V: atomic LKV store (no per-vertex mutex)
    G->>V: snapshot subscribers (brief lock)
    G-->>S1: fn(ctx, clone)  %% refcount bump
    G-->>S2: store + notify at the target  %% no re-dispatch from there
    Note over V: await waiters woken via condvar
    G-->>P: OK
```

The subscriber snapshot is taken under the per-vertex mutex and the sinks are called
outside it, so a callback may re-enter the graph. Because a delivery landing on a target
does not re-fan from that target, re-entry cannot build a dispatch cycle.

A remote subscriber's delivery does not go on the wire from here: the fan-out hands
`{link, return_route, delivery_compact}` and the value to the graph's injected
remote-delivery sink, which is a `tr::net` concern. See
[fwd-router](fwd-router.md) and [transport](transport.md).

## Status codes

`status_t` (`core/include/libtracer/status.hpp:24-33`) is the error side of every
`result_t`. When the operation arrived over the wire, the FWD resolver maps it to the
registered `tr::` error code the `kind=ERROR` reply carries (`error_code(status_t)`,
`core/src/op_resolve_walk.hpp:76-95` — a private header under `src/`, not part of the
public API).

| `status_t` | Wire error | What produces it |
| --- | --- | --- |
| `NOT_FOUND` | `PATH_NOT_FOUND` | the path resolves to no live vertex (never registered, or retired), or the vertex holds no last-known-value yet |
| `PERMISSION_DENIED` | `ACCESS_DENIED` | a subject resolver is installed and the target's effective ACL grants the operation's right to no matching, non-expired ACE |
| `INVALID_PATH` | `PATH_INVALID` | `path_t::parse` on a malformed path or a non-UTF-8 `NAME` segment |
| `TYPE_MISMATCH` | `SCHEMA_TYPE_MISMATCH` | a payload whose type the vertex or field cannot take; also `set_identity` with a kind outside the registry or a key length contradicting the kind |
| `BACKPRESSURE` | `FLOW_BACKPRESSURE` | an allocation a peer can provoke could not be served from the injected nothrow control seam, or a per-subscriber queue cap is exceeded |
| `TIMEOUT` | `FLOW_TIMEOUT` | an `await` deadline expired |
| `SCHEMA_NOT_FOUND` | `SCHEMA_NOT_FOUND` | a field read or write on a vertex that exposes no such field — an undeclared app field, `:identity` on a node with no key installed, a `:children[]` `SPEC` whose `type` is unregistered |
| `PATH_IN_USE` | `PATH_IN_USE` | `try_register_vertex` collided with a live vertex at that address |

`BACKPRESSURE` is the allocation-failure and flow-control answer. It is not a
dispatch-depth signal: no depth cap exists.

## Setup-time seams

Five installers configure a graph before frames flow. Each is set once at wiring time and
is **not thread-safe against concurrent use afterwards** — the op paths read them without
a lock precisely because nothing writes them once traffic starts.

| Seam | Effect | Default |
| --- | --- | --- |
| `register_child_type(type, factory)` | populates the in-band creation catalog: which `type` selector a `:children[]` `SPEC` write may instantiate | only the built-in `stored_value`; an unregistered `type` answers `SCHEMA_NOT_FOUND` |
| `set_identity(kind, key)` / `clear_identity()` | installs the node-scoped record `read <vertex>:identity` serves, byte-identical from every vertex | absent — `:identity` answers `SCHEMA_NOT_FOUND` |
| `set_remote_delivery_sink(sink)` | where the producer fan-out hands each **remote** subscriber's delivery | **null — remote subscriber slots are stored but never deliver** |
| `set_subject_resolver(resolver)` | maps a caller context to a subject token, enabling ACL evaluation | **none — enforcement is entirely off; every operation is allowed** |
| `subscribe_wire(v, source, route, link)` | the inbound `:subscribers[]` append: one parse, the SUBSCRIBE gate, the slot append, the transient-local latch | — (called by the FWD resolver, not a default) |

The two defaults in bold are load-bearing and are the two failure modes a node wired by
hand hits first. A graph with no remote-delivery sink accepts remote subscribes and
records them; nothing ever leaves. A graph with no subject resolver is fully open,
whatever `:acl` bytes its vertices carry.

`set_identity` involves no cryptography. The record is a **claim**: the seam stores and
serves the bytes the owner supplies and verifies nothing. Proving a node holds the key is
authentication and lives elsewhere; a claim is nevertheless what a trust-on-first-use peer
pins and what a topology walk deduplicates by. `:identity` resolves **above** the READ
gate, so an unauthenticated peer can fetch it — a narrow, named exemption for that one
field
([RFC-0011 — node identity facet](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0011-node-identity-facet.md)).

## Delivery drops

A delivery can be lost after the write succeeded. `delivery_drops()` returns the only
record of it:

```cpp
struct delivery_drops_t {
    std::uint64_t no_target;      // the target PATH resolved to no live vertex
    std::uint64_t denied;         // the target's :acl denied WRITE to the edge's stored caller
    std::uint64_t out_of_memory;  // the nothrow delivery clone could not be allocated
};
```

Counted, never enforced: nothing in the library reads them, so a deployment chooses
whether to alarm. They are relaxed monotonic and incremented only **on** a drop, so the
delivering path pays nothing when nothing is dropped. The three loads are individually
relaxed rather than one atomic snapshot — making them coherent would put a lock on the
delivery path to serve a diagnostic, and the useful reading of a monotonic counter is "is
this growing", not an instant.

A subscriber whose target was retired, or whose caller lost the WRITE right, silently
stops receiving. There is no other instrument for that.

## Declaring owner fields

Application properties live under `:settings.app.` and are **declared by the owner**, never
invented by a peer. Declaration is a local host call with no wire operation behind it: the
field catalog is device state
([RFC-0010 — owner app fields and schema](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0010-owner-app-fields-and-schema.md) §A.1).
Every undeclared name answers `SCHEMA_NOT_FOUND`.

```cpp
enum class app_access_t { RO, RW, WO };   // constrains REMOTE callers only

struct app_field_t {                      // owning install
    std::string           name;           // key below settings.app. ("kp", "wifi.ssid")
    app_access_t          access;
    std::vector<std::byte> descriptor;    // §B.1 record served verbatim inside :schema
    std::vector<std::byte> value;         // optional initial value
};

void set_app_fields       (vertex_handle_t, std::vector<app_field_t>);  // owning
void set_app_fields_static(vertex_handle_t, borrowed_fields_t);        // borrowed, zero-copy
```

| | `set_app_fields` | `set_app_fields_static` |
| --- | --- | --- |
| Name and descriptor bytes | copied into the graph | **viewed**, never copied |
| Initial value | may carry one | declaration only; write values afterwards |
| Caller obligation | none | the table array **and** the bytes it points at outlive the vertex |

`borrowed_fields_t` converts implicitly from the array spellings a `constexpr` table in
flash takes, and **not** from a `std::vector` — so a caller whose storage cannot satisfy
the lifetime rule fails to compile rather than dangling. A runtime-sized table opts out
explicitly via `borrowed_fields_t::unchecked`. For an MCU owner whose table is `constexpr`
in `.rodata`, the borrowed form costs zero declaration RAM
([ADR-0058 — vertex_ext storage classes, borrowed declarations and group split](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0058-vertex-ext-storage-classes-borrowed-declarations-and-group-split.md)).

`access` constrains remote callers only — the owner always reads and writes its own
declared fields. `WO` gives a secret no read surface, so it never mirrors back.

The runtime validates **addressing** only: declared or undeclared, and writability. Range
and dtype checking is the owner's, in `handlers_t::on_app_field_write`, which fires after a
declared field write has stored its bytes, with the field's key and the written TLV. That
seam runs **outside** the vertex lock, so it may re-enter the graph — apply the config,
restructure children, then announce the change with an ordinary data write. An app-field
write never wakes `await` and never propagates; a change consumers should notice is
followed by the owner's own announce write.

## Pitfalls

| Rule | The failure mode |
| --- | --- |
| `subscribe(src, F& callback)` binds by address | passing a temporary lambda does not compile — which is the intent; a caller that "fixes" it by storing the lambda in a shorter-lived scope than the graph reintroduces the dangle the signature was shaped to prevent |
| `ctx` outlives every delivery, not every `unsubscribe` | a caller that frees `ctx` immediately after `unsubscribe` returns can be freeing it under an in-flight delivery on another thread |
| `read` returns a reference | keeping a `value_ref_t` in long-lived state pins that allocation; under an injected pool, a handful of parked references is a pool that never drains |
| `only()` is the single-link accessor | calling it on a multi-link rope is not the general path; `materialize()` is. A value that arrived as a subview of a frame, or that was written as a rope, has more than one link |
| A retired handle stays dereferenceable | `retire` empties the vertex in place and never frees it, so a stale handle silently addresses a re-virginized slot. A holder that caches a resolution records `retire_generation` beside it and re-reads before use — and must not cache an authorization decision that way, since a generation match says the vertex is the same one, never that the caller may still act on it |
| `retire` parks a value seam; only `collect()` frees it | the seam is read lock-free, so `retire` cannot free it — it parks it on the graph. Nothing frees the park until the embedder calls `collect()` at a point it knows no reader holds a seam. Connection teardown retires the `/net/<name>` identity vertex, so a node with peer churn that never collects grows the park forever; `parked_seam_count()` is how that shows up before it matters ([#576](https://github.com/avatarsd-llc/libtracer/issues/576)) |
| No subject resolver means no enforcement | writing `:acl` bytes on vertices and never installing a resolver yields a node that looks protected and is fully open |
| No remote-delivery sink means no remote delivery | remote subscribes are accepted and stored; the `delivery_drops()` counters stay at zero because nothing was dropped — nothing was attempted |

## Consequences

- **Two irreducible operations, not one.** `assign` and `propagate` compose into `write`;
  splitting them expresses "update many, notify once" without a batch API, and keeps the
  coalescing policy on the vertex rather than on the edge.
- **No per-vertex mutex on the value path.** The LKV is an atomic pointer swap; the mutex
  guards the subscriber list, history and `await` accounting. Race-freedom under TSan is
  evidence about data races, not about blocking — the slot's serializing instructions are
  real and measured in [design/concurrency](../design/concurrency/README.md).
- **Zero-copy fan-out.** N subscribers get N refcount clones of one `rope_t`, not N copies.
- **No dispatch limits.** Delivery terminating at the target removes the cycle, so no depth
  counter, no hop budget, and no synthetic constant to tune per deployment.
- **The value is the bytes.** A vertex stores a `rope_t`, so what it holds is exactly what
  goes on the wire.

## API reference

```{doxygenclass} tr::graph::graph_t
:project: libtracer
:members:
```

```{doxygenclass} tr::graph::vertex_t
:project: libtracer
:members:
```

```{doxygenenum} tr::graph::role_t
:project: libtracer
```

See: [path](path.md), [views](views.md), [interface-map](interface-map.md).
