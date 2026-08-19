# Unsubscribe, and when the context dies (L4 graph)

`unsubscribe(sub)` is the host-SDK counterpart of the wire `:subscribers[N]` clear: it
deactivates the slot, unwinds the subtree-listener bookkeeping, and leaves the (index-stable)
shell for a later `subscribe` to reuse. Retirement takes effect at once. What the
one-argument form structurally cannot say is *when* the subscriber's `{fn, ctx}` pair
stopped being reachable — so the two-argument form takes a release hook and the library
signals the caller
([ADR-0080](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md)).

## What to notice

- **The direction is inverted on purpose.** The embedder never polls in-flight state and
  never waits: it registers a `subscriber_release_fn_t`, and libtracer calls it exactly once,
  on the caller's own thread, outside every graph lock. A contract of the form "the callback
  may still fire until you call X" is what ADR-0080 rejects.
- **Called from outside a delivery, the hook runs inline.** Both shipped policies —
  `reclaim_local_t` (the default) and `reclaim_strict_t` — release before `unsubscribe()`
  returns in this case, so the caller may free its context on that return. The other case,
  unsubscribing from *inside* a delivery, is
  [its own example](sub-unsubscribe-from-dispatch.md).
- **A no-op retire owes no signal.** A default-constructed or already-cleared handle answers
  `NOT_FOUND` and runs no hook — the example resets its flag and re-checks, so "the hook did
  not run" is asserted rather than assumed.
- **This applies to the callback form.** A `subscribe(src, target)` edge is a wire
  `:subscribers[]` field-write and is removed through the wire clear — the empty-`STATUS`
  sentinel written at `:subscribers[N]`.
- **In-flight remote deliveries are not recalled.** Over a transport, TLVs dispatched before
  the clear but not yet consumed still arrive; a subscriber may see a few more after its
  unsubscribe returns. That is a property of the wire path, not of the in-process retire this
  example shows.

## Source

```{literalinclude} /core/examples/sub_unsubscribe.cpp
:language: cpp
:linenos:
```

See also: [reclamation policy](../reference/17-reclamation-policy.md) ·
[graph module](../modules/graph.md) ·
[communication flows](../reference/04-communication-flows.md).
