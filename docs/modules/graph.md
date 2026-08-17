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
has a **role**: *stored-value* (last-writer-wins), *stream* (a bounded ring whose depth the
owner declares host-side with `set_history_depth`, and which no peer can read or write —
[RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.C), or *handler* (`on_read` / `on_write` — covering
computed, proxy, sink, live-MMIO patterns). The last-known-value slot is an
`atomic<shared_ptr<const rope_t>>` swap, so `read` / `write` of the value take **no
per-vertex mutex**; that mutex guards the subscriber list, the history ring and the
`await` waiter accounting, and a per-vertex condvar makes `await` block until the next
write.

The slot is not free of serializing instructions. `std::atomic<std::shared_ptr<T>>` is
not lock-free on libstdc++, so both load and store take its internal pointer-lock bit —
"lock-free by contract, spin-locked in practice" (`sp_atomic_slot_t`,
`core/include/libtracer/lkv_slot.hpp:100-105`). The claim the code supports is the mutex
one, not an absence of contention; the cost of that spin and the policy that replaces it
on a host are in [design/concurrency](../design/concurrency/README.md).

**Subscriptions are field-writes, not a verb**
([ADR-0006 — read/write/await API, no connect](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0006-read-write-await-api-no-connect.md)):
subscribing *is* writing a `SUBSCRIBER` TLV into `:subscribers[]`. On each write the
dispatcher clones the value to every subscriber's target vertex and in-process callback.
A delivery **terminates at its target** — store and notify, never a re-dispatch to the
target's own `:subscribers[]` — so a dispatch-level cycle cannot form and there is no
depth cap to tune (`core/include/libtracer/graph.hpp:85-90`;
[ADR-0051 — delivery terminates at target, no dispatch limits](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0051-delivery-terminates-at-target-no-dispatch-limits.md),
[RFC-0007 — delivery terminates at target](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0007-delivery-terminates-at-target.md)).
Propagation past a target is exclusively the target's own logic — a controller
re-emitting on its execution. `:schema` reads return a `POINT` descriptor.

## Interface

```cpp
enum class role_t { STORED_VALUE, STREAM, HANDLER };
enum class delivery_mode_t { IF_NEWER, UNCONDITIONAL, EXPLICIT };

// There is NO per-vertex settings type. RFC-0022 §3.B deleted `settings_t` outright: four
// of its seven knobs were inert, `durability` became the subscription's (below), and the two
// survivors are construction parameters an OWNER declares — see set_history_depth /
// set_pin_payload_ratio. Nothing is inherited (§3.F).

struct delivery_policy_t {  // ONE subscription's delivery policy (RFC-0022 §3.A) — 2 B packed
    std::uint16_t bits;     // 0-1 reliability | 2-4 priority | 5 durability_request | 6-15 rsvd
};

struct write_ctx_t {         // the per-call context a write carries into on_write (#375)
    std::string_view subject;  // the writer's resolved subject token; EMPTY = the local host
    bool is_local_owner() const noexcept;  // subject.empty()
};                           // BORROWED for the call, like the rope beside it — copy if retained

struct handlers_t {                                       // four seams, not two
    std::function<result_t<rope_t>()>                        on_read;
    std::function<result_t<void>(const rope_t&, const write_ctx_t&)> on_write;
    std::function<result_t<view_t>()>                        on_children;
    std::function<void(std::string_view, const view_t&)>     on_app_field_write;
};

using subscriber_fn_t = void (*)(void* ctx, const rope_t& value);
class  subscription_t { /* opaque: producer vertex + :subscribers[] slot index; graph_t is the
                           sole friend. Public: default-construct, copy, operator==. */ };

class graph_t {
    explicit graph_t(std::pmr::memory_resource* mr    = std::pmr::get_default_resource(),
                     mem::mem_backend_t* value_backend = &mem::heap_backend(),
                     mem::block_source_t* ctl          = &mem::heap_source());

    // registration and removal
    vertex_handle_t register_vertex(const path_t&, role_t, handlers_t = {});
    result_t<vertex_handle_t> try_register_vertex(const path_t&, role_t, handlers_t = {});
    result_t<void> retire(vertex_handle_t);                       // logical absence, subtree-wide
    std::uint32_t  retire_generation(vertex_handle_t) const noexcept;
    void           collect();                    // free BOTH parks (seams + retired {fn,ctx})
    std::size_t    parked_seam_count() const;    // how many seams await a collect()
    std::size_t    parked_callback_count() const;  // how many retired {fn,ctx} pairs do
    std::optional<vertex_handle_t> find(std::span<const std::byte> key) const;

    // the node-scoped vertex index — bound-path addressing (RFC-0024 §6.4)
    std::size_t vertex_slot_count() const noexcept;
    std::optional<vertex_slot_t>    vertex_slot(vertex_handle_t) const noexcept;   // mint side
    std::optional<vertex_handle_t>  deref_vertex_slot(std::uint32_t index,
                                                      std::uint32_t generation) const noexcept;
    std::optional<vertex_slot_t>    vertex_slot_at(std::uint32_t index) const noexcept;  // O(1)
    bool allows(vertex_handle_t, std::string_view caller, acl_right_t) const;  // the §6.2 check

    // value plane
    result_t<value_ref_t> read (vertex_handle_t, std::string_view caller = {}) const;
    result_t<void>        write(vertex_handle_t, rope_t, std::string_view caller = {});
    result_t<value_ref_t> await(vertex_handle_t, std::chrono::nanoseconds,
                                std::string_view caller = {});
    result_t<void>        assign(vertex_handle_t, rope_t, std::string_view caller = {});
    void                  propagate(vertex_handle_t);
    void                  set_delivery_mode(vertex_handle_t, delivery_mode_t);
    result_t<std::vector<rope_t>> history(vertex_handle_t) const;   // stream window

    // owner-side storage declarations (RFC-0022 §3.C) — host API only, NO wire surface
    void          set_history_depth     (vertex_handle_t, std::uint32_t keep);
    void          set_pin_payload_ratio (vertex_handle_t, std::uint32_t k);
    std::uint32_t pin_payload_ratio     (vertex_handle_t) const noexcept;

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
    result_t<void>           subscribe(const path_t& src, const path_t& target,
                                       delivery_policy_t policy = {});
    result_t<subscription_t> subscribe(const path_t& src, subscriber_fn_t fn, void* ctx,
                                       delivery_policy_t policy = {});
    template <typename F>
    result_t<subscription_t> subscribe(const path_t& src, F& callback);   // lvalue only
    result_t<void>           unsubscribe(const subscription_t&);
    result_t<void>           unsubscribe(const subscription_t&, subscriber_release_fn_t);
};
```

There is no `std::function` subscribe overload and no `result_t<void>` callback form. The
per-edge sink is a `{fn, ctx}` pair so the per-publish edge snapshot under the fan-out lock
is a trivial copy rather than a `std::function` clone that heap-allocates once captures
exceed the small-buffer size
([ADR-0047 — build-time closed module sets, compile-time seams](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md)).
The templated overload binds `callback` **by address**, so it takes an lvalue only — a
temporary lambda does not compile.

```{admonition} `ctx` lives until the next `collect()`, not until `unsubscribe` returns
:class: warning
Subscription edges are never destroyed while the graph lives. `unsubscribe` only
**deactivates** the slot; an in-flight delivery has already snapshotted the edge and
completes. The caller-owned `ctx` (or, for the templated overload, the callable itself)
must therefore stay alive past any delivery that may still be running, not merely past
the `unsubscribe` call (`core/include/libtracer/graph.hpp:1339-1340`, `:1363` for the
callable-by-address form, `:1377`).

The bound on "any delivery that may still be running" is stated, not left open:
**the callback may still be invoked until the first `collect()` that completes after
`unsubscribe()` returns; free the `ctx` after that**
([#894](https://github.com/avatarsd-llc/libtracer/issues/894)). `unsubscribe` swap-parks the
retired `{fn, ctx}` pair and the embedder-driven `collect()` releases it — the same
two-phase lifetime `retire` + `collect()` gives a value seam, and the same doctrine: the
application declares the quiescent point, the library never guesses one. Pass a
`subscriber_release_fn_t` to `unsubscribe(sub, release)` and `collect()` frees the context
for you, on the collecting thread and outside every graph lock. This costs the delivery
path nothing — refcounting the pair, the rejected alternative, would have cost two atomic
RMWs per callback per delivery, worst exactly on WIDE fan-out. The park has its OWN leaf
lock, never the graph's map lock: guarding it with the lock the delivery path holds shared
starves the unsubscriber against a publishing loop.
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
path_t p("/x:settings.app.setpoint");                    // once — no *-deref
auto v = *g.find(p.key());                               // once — find → optional<vertex_handle_t>
for (...) g.write(v, p.field(), setpoint_tlv);           // hot loop — zero strings
```

## What a read hands back

`read` and `await` return `result_t<value_ref_t>`, not `result_t<rope_t>`
(`core/include/libtracer/graph.hpp:1127,1214` by handle, `:1688,1694` by path;
`value_ref_t` at `core/include/libtracer/vertex.hpp:237`). A `value_ref_t` is an **owning
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

`set_delivery_mode(v, mode)` sets that per-vertex policy. It is a wiring-time host API call,
in the same family as `set_history_depth`, `set_pin_payload_ratio` and `set_app_fields` — an
owner declaration with no wire surface.

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

`status_t` (`core/include/libtracer/status.hpp:25-44`) is the error side of every
`result_t`. When the operation arrived over the wire, the FWD resolver maps it to the
registered `tr::` error code the `kind=ERROR` reply carries (`error_code(status_t)`,
`core/src/fwd_reply.cpp:33-77` — a private TU under `src/`, not part of the
public API).

The table below is a **total** map, and the compiler keeps it that way: `error_code` is a
`switch` with no `default:` label and no fall-through tail, compiled under `-Werror=switch`,
so a `status_t` gained without a row here is a red build rather than a status that goes out
under some other member's wire code. It reads as a formality only until you notice that the
two enums are deliberately separate registries — `status_t` is L4 vocabulary, `err_t` is the
wire's — which is what makes the mapping hand-written and therefore losable.

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
| `TRANSPORT_DOWN` | `TRANSPORT_DOWN` | a transport-construction failure: a dial refused, a TLS/WebTransport handshake rejected, a listener that could not bind, a CAN interface the kernel would not open |

`BACKPRESSURE` is the allocation-failure and flow-control answer. It is not a
dispatch-depth signal: no depth cap exists.

`TRANSPORT_DOWN` is the only member of this table whose point is the **disposition**, not
the name. Its wire code is TRANSIENT in the registry — *retry may succeed* — while every
other row here is PERMANENT or (for `BACKPRESSURE` / `TIMEOUT`) transient for a reason the
caller can already see. Until #929 the built-in transport factories spent `NOT_FOUND` on a
link that did not come up, so a refused connect went out as `tr::path::not_found` and a
peer reading the disposition off the code stopped retrying a link that would have come
back. Nothing before #929 could reach `err_t::TRANSPORT_DOWN` from this side: the map was
total over a `status_t` that had no member for it.

## Setup-time seams

Five installers configure a graph before frames flow. Each is set once at wiring time,
from one thread. That is the **doctrine**, and since #1049 it is stated by the API rather
than requested in a comment: the three callback seams are spelled `configure_*` and take
the ADR-0047 `{fn, ctx}` pair, never a `std::function`.

The shape is the enforcement. A `std::function` cannot be handed to a racing reader at
all — assigning one *destroys the old target*, freeing its captures while a reader may be
inside the call — whereas a bare function pointer is one word, so the pair publishes
through a [`sink_slot_t`](fwd-router.md) exactly as the router's five sinks do. A fan-out,
an ACL gate or a subscribe that races an install therefore sees the whole new pair, the
whole old one, or *no sink for that one operation*; it never sees a new `fn` beside a stale
`ctx`, and never a freed capture. The read costs what the null check it replaces cost: one
relaxed load when nothing is installed.

The two remaining seams are not callbacks and the slot does not reach them, so they take a
lock instead — which is free, because both are control-plane cold. The child-type catalog
is a `std::map` whose lookup runs from a **peer's bytes**; the identity record is a buffer
whose read is served **above the READ gate**, to a peer that has authenticated nothing
(RFC-0011 §C), and which install and clear both free. Neither lock touches a read, write or
dispatch path.

The `ctx` pointer is the caller's, and must outlive every operation that can still reach
the seam: clearing a sink does not stop a dispatch already in flight.

| Seam | Effect | Default |
| --- | --- | --- |
| `register_child_type(type, factory)` | populates the in-band creation catalog: which `type` selector a `:children[]` `SPEC` write may instantiate | only the built-in `stored_value`; an unregistered `type` answers `SCHEMA_NOT_FOUND` |
| `set_identity(kind, key)` / `clear_identity()` | installs the node-scoped record `read <vertex>:identity` serves, byte-identical from every vertex | absent — `:identity` answers `SCHEMA_NOT_FOUND` |
| `configure_remote_delivery_sink(fn, ctx)` | where the producer fan-out hands each **remote** subscriber's delivery | **null — remote subscriber slots are stored but never deliver** |
| `configure_subject_resolver(fn, ctx)` | maps a caller context to a subject token, enabling ACL evaluation | **none — enforcement is entirely off; every operation is allowed** |
| `subscribe_wire(v, source, route, link, reverse, caller)` | the inbound `:subscribers[]` append: one parse, the SUBSCRIBE gate, the slot append, the durability latch the subscriber requested. `link` is WHERE the edge delivers; `caller` is WHO subscribed (empty ⇒ the same as `link`, every pre-#375-Part-2 caller) | — (called by the FWD resolver, not a default) |

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
    std::uint64_t no_target;          // no live vertex: an edge's target PATH, or a net-plane route
    std::uint64_t denied;             // a WRITE was refused by the target's :acl — on any plane
    std::uint64_t out_of_memory;      // a nothrow delivery clone / edge-view copy could not allocate
    std::uint64_t fan_out_truncated;  // a wide fan-out's snapshot could not be widened past the
                                      // inline prefix — the capacity degrade, kept apart from OOM
};
```

The unit is a **delivery, not an event**: a write whose notify clone fails sheds every
subscriber of the vertex, and a truncated snapshot sheds every edge past the inline prefix, so
each counts once per shed delivery (`1` never stands in for `N`).

`denied` counts a refusal on **every plane the value-write path is entered from** — an API
`write`, a `FWD{WRITE}` terminus, a `COMPACT` terminus, and a subscription edge's fan-in gate
— because it is counted at the graph's own WRITE gate rather than once per deliverer (#1068).
It is therefore *refusals*, not *refusals nobody was told about*: an API caller both receives
`PERMISSION_DENIED` and counts here. A number that depended on which door a refusal came
through could not be summed. `assign`, a control-plane field write and a denied READ are each
a different right or a different path, and are deliberately not folded in.

A deliverer **outside** the graph — the net plane resolving a label to a vertex and writing it
— counts its own abandoned deliveries through `count_external_drop`, the one public door to
these counters. It names only `NO_TARGET` and `OUT_OF_MEMORY`: a denial is counted at the gate
that produces it, so offering it there would count one refusal twice.

Counted, never enforced: nothing in the library reads them, so a deployment chooses
whether to alarm. They are relaxed monotonic and incremented only **on** a drop, so the
delivering path pays nothing when nothing is dropped. The loads are individually
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
| `ctx` outlives every delivery — the bound is the next `collect()`, not the `unsubscribe` return | fan-out snapshots its edges under the producer's stripe lock and dispatches outside it, so a caller that frees `ctx` immediately after `unsubscribe` returns can be freeing it under an in-flight delivery. The bound the API now states is **the first `collect()` that completes after `unsubscribe()` returns** — `unsubscribe` swap-parks the retired `{fn, ctx}` pair and `collect()` releases it, the same two-phase lifetime the value seam has. `unsubscribe(sub, release)` has `collect()` free the context for you; `parked_callback_count()` makes an uncollected park observable ([#894](https://github.com/avatarsd-llc/libtracer/issues/894)) |
| `collect()` must NOT be called from inside a subscription callback | it is the one context where its precondition is provably false — the delivery running your callback holds a snapshot, so collecting from within it can release the context the enclosing fan-out is about to use for the next edge. It is forbidden rather than detected because detecting it needs a dispatch-depth counter, i.e. work on the delivery path, which the #894 ruling rules out. Two concurrent `collect()` calls, by contrast, are safe: each park is swapped out under its own lock, so each entry is released exactly once |
| `read` returns a reference | keeping a `value_ref_t` in long-lived state pins that allocation; under an injected pool, a handful of parked references is a pool that never drains |
| `only()` is the single-link accessor | calling it on a multi-link rope is not the general path; `materialize()` is. A value that arrived as a subview of a frame, or that was written as a rope, has more than one link |
| A retired handle stays dereferenceable | `retire` empties the vertex in place and never frees it, so a stale handle silently addresses a re-virginized slot. A holder that caches a resolution records `retire_generation` beside it and re-reads before use — and must not cache an authorization decision that way, since a generation match says the vertex is the same one, never that the caller may still act on it |
| `retire` parks a value seam; only `collect()` frees it | the seam is read lock-free, so `retire` cannot free it — it parks it on the graph. A vertex bears a seam iff a handler was installed (presence, not role: `STORED_VALUE` + `on_children` parks, `HANDLER` + empty `handlers_t` does not). Nothing frees the park until the embedder calls `collect()` at a point it knows no reader holds a seam. Connection teardown retires the `/net/<module>/<name>` identity vertex, which is seam-bearing only over a **bus** link (`link->bus() != nullptr`: CAN, or a tcp/ws server wired `peer_named = true`) — so a bus node with peer churn that never collects grows the park forever, while a point-to-point deployment parks nothing; `parked_seam_count()` is how that shows up before it matters ([#576](https://github.com/avatarsd-llc/libtracer/issues/576)) |
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

```{doxygenstruct} tr::graph::edge_block_t
:project: libtracer
:members:
```

```{doxygenenum} tr::graph::role_t
:project: libtracer
```

### The node-scoped vertex index

`graph_t` keeps one dense, append-only `vertex_t*` slot per vertex ever allocated, in
allocation order, with the structural root at slot 0. It exists so a bound path's `u32`
index means something: the vertex tree is a Composite of non-moving `unique_ptr`
allocations with no dense index of its own, and an element that named a tree position
would have to be a path again.

It costs **4 bytes per vertex on rv32**, 8 on a host — the pointer and nothing else. The
index is stored **chunked** rather than as one growing array for exactly that reason: a
geometrically-growing array holds up to twice the pointers it needs between doublings, which
measured 15 B per vertex on the 512-vertex heap probe against the 8 B the cost model
charges. Fixed blocks make live bytes track the vertex count instead of the last doubling,
and indexing stays O(1) with elements that never move. It is not a route table — its size
tracks the graph, not the traffic — and it introduces no new lifetime rule, because
registration was already insert-only. A slot is appended per **allocation**, not per
registration, which is what keeps the mapping a bijection: retirement revives a vertex by
filling the same object again, and a per-registration slot would give that object two
indices depending on which side of the revive a mint fell.

`deref_vertex_slot` is the hot side and is the whole of the check — a bounds compare and a
generation compare, both under one shared map hold. It authorizes nothing; the operation
that follows re-evaluates the ACL at the vertex it returns.

The generation compare is `bound_generation_matches`, and it refuses a **saturated**
element outright rather than comparing it. Below the ceiling, "generations only move
forward" is the whole guard — a stale element compares lower and can never come back. At
the ceiling the counter stops, so a saturated element would keep matching its slot through
every subsequent retire and revive, with staleness detection permanently dead for that
slot. "Permanently unbindable" therefore has to be enforced on the side that *honours* an
element, not only on the side that issues one.

`vertex_slot_at` is the same read the other way round — index in, generation out, in O(1) —
and it exists for the FORWARDER's mint: a hop mints for the connection vertex of the link a
reply arrived on, an index it recorded once at registration, so paying a scan of the whole
index per forwarded reply to re-derive an index it already holds would be the wrong shape in
the wrong place. It refuses a saturated slot exactly as the scanning form does.

`allows(vertex, caller, right)` publishes the ACL predicate every data op already runs, for
the one caller that reaches a vertex without performing a data op on it: the bound-path
forwarder, whose element dereferences to a **connection** vertex it will egress through
rather than read or write. Nothing is cached, so a revoked right takes effect on the very
next frame over an already-minted binding.

`vertex_slot` is the mint side. It returns the index **and** the generation together, from
one lock hold, because either alone is not a reference: read as two calls they can straddle
a retire, and the pair would then name the successor tenant's vertex while the caller
believes it bound the one its operation reached. And it **scans**. That is deliberate rather than pending: a
per-vertex index field costs 4 bytes on rv32, where `sizeof(vertex_t)` sits at
`config_t::kMaxVertexBytes32` with zero headroom, and a pointer→index side map costs
strictly more than the 4 B/vertex the slot vector does. A mint happens once per binding, on
a reply already being assembled.

### Registration and subscription

```{doxygenclass} tr::graph::vertex_handle_t
:project: libtracer
:members:
```

```{doxygenclass} tr::graph::subscription_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::subscriber_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::subscriber_remote_t
:project: libtracer
:members:
```

```{doxygentypedef} tr::graph::subscriber_fn_t
:project: libtracer
```

```{doxygenstruct} tr::graph::remote_delivery_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::sub_event_t
:project: libtracer
:members:
```

```{doxygentypedef} tr::graph::sub_observer_fn_t
:project: libtracer
```

### Handlers and delivery policy

```{doxygenstruct} tr::graph::write_ctx_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::handlers_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::value_handlers_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::delivery_policy_t
:project: libtracer
:members:
```

```{doxygenenum} tr::graph::delivery_mode_t
:project: libtracer
```

```{doxygenclass} tr::graph::value_ref_t
:project: libtracer
:members:
```

### Edges

```{doxygenstruct} tr::graph::edge_view_t
:project: libtracer
:members:
```

```{doxygenclass} tr::graph::edge_snapshot_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::edge_latch_t
:project: libtracer
:members:
```

### Owner app fields

```{doxygenenum} tr::graph::app_access_t
:project: libtracer
```

```{doxygenstruct} tr::graph::app_field_t
:project: libtracer
:members:
```

```{doxygentypedef} tr::graph::app_field_static_t
:project: libtracer
```

```{doxygenstruct} tr::graph::app_field_slot_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::app_field_group_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::app_field_table_t
:project: libtracer
:members:
```

```{doxygenclass} tr::graph::borrowed_fields_t
:project: libtracer
:members:
```

### Lock striping

```{doxygenstruct} tr::graph::vertex_stripe_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::graph::vertex_stripe_index
:project: libtracer
```

```{doxygenfunction} tr::graph::vertex_stripe_at
:project: libtracer
```

See: [path](path.md), [views](views.md), [status & errors](status.md),
[security & ACL](security-acl.md), [config](config.md),
[interface-map](interface-map.md).
