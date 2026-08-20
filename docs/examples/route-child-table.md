# One NAME, one slot — the whole routing table (L4 routing)

`fwd_router_t` has exactly one table: `child_registry_t`, keyed on **this node's own local name**
for each link
([ADR-0037](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0037-net-side-channels-dissolve-into-vertex-tree-compositor.md)).
Forwarding, replying and advertising all resolve through it. There is no second table, and no
destination ever appears in it.

This example routes no frames at all. It is about the table's two rules — what a *refused*
registration leaves behind, and how many slots a name can ever own.

## What to notice

- **A refusal registers NOTHING, and the `bool` is the only warning you get.** `add_child`
  returns false for a name no address could ever spell (empty, containing an empty route
  segment, wider than `graph::kMaxSegments`) and for a registry that could not grow. Discarding
  it is how [#930](https://github.com/avatarsd-llc/libtracer/issues/930) shipped: a connection
  reported UP whose every forward missed and fell through to the terminus with no error
  anywhere. `[[nodiscard]]` does not forbid ignoring the result — it makes ignoring it a
  deliberate, greppable `(void)`.
- **A name owns exactly one slot, for the router's life** ([#884](https://github.com/avatarsd-llc/libtracer/issues/884)).
  Re-adding a live name **rebinds** its slot; a second slot would shadow the first on every
  name-keyed lookup — returning the *dead* one — and churn on a stable name set would grow the
  chain every lookup walks without bound.
- **Removal is a tombstone, not an erase.** `live_size()` falls, `size()` does not. A lock-free
  reader may be walking that slot right now, so it stays put and stops answering; a later
  re-add revives it in place
  ([ADR-0063](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0063-connection-table-lock-free-reads-trait-serialized-writes.md)).
  Hence two counters: `live_size()` is what routes, `size()` is what lookups walk.
- **`remove_child` is removal, not departure.** A link that merely dropped wants `link_down`,
  because under RFC-0014 a DIAL connection's *vertex* outlives its *socket* and self-heals —
  evicting the registry entry would permanently unroute a link that is only reconnecting.
- **Every mutation is its own statement.** The example never puts a call that changes a counter
  and a read of that counter in the same `printf` argument list; C++ leaves that order
  unspecified, and an example about counters cannot afford it. (This bit the first draft.)
- **This target needs the FWD net plane.** It is built only when `LIBTRACER_NET_PLANE` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/route_child_table.cpp
:language: cpp
:linenos:
```

See also: [fwd-router module](../modules/fwd-router.md) ·
[transport module](../modules/transport.md) ·
[transports are vertices](../reference/19-transports-are-vertices.md) ·
[terminus or forward](route-terminus-or-forward.md) ·
[qualified mounts](route-qualified-mount.md).
