# Unsubscribing from inside a delivery (L4 graph)

A subscriber callback that retires its own subscription is the one case where "the edge is
gone" and "the `{fn, ctx}` pair is dead" are not the same moment: the fan-out that invoked
the callback is still walking a snapshot that names that pair. The default
`reclaim_local_t` policy parks the retired pair and runs its release hook when **this
thread's dispatch stack unwinds to depth 0** — before the `write()` that started the
delivery hands control back
([ADR-0080](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md)).

## What to notice

- **Retirement is immediate; release is not.** The example's second write reaches nobody —
  the next snapshot already skips the slot — yet the hook has not run when the callback
  returns. Both halves are asserted, and the run prints the flag as observed *inside* the
  callback.
- **The library decides which grace point applies, and the caller never asks.** Outside a
  delivery the same call releases inline (see [unsubscribe](sub-unsubscribe.md)); inside
  one it defers. The signal is the hook in both cases — there is no poll and no verb the
  embedder must remember.
- **The policy is a build-time type, not a runtime flag.** It is named by
  `default_config_t::reclaim_policy_t` and bound in `libtracer/config_override.hpp`, on the
  same seam pattern as the ACL policy and the LKV slot. The example prints
  `reclaim_policy_t::kName` so a run says which one it exercised.
- **`reclaim_strict_t` forbids this shape outright.** It is the opt-in zero-cost mode for a
  deployment that provably never unsubscribes from inside a dispatch; a debug build asserts,
  an `NDEBUG` build cannot see it. The example makes that a `static_assert` on
  `kReentrantUnsubscribe` rather than a run-time failure.
- **The guarantee is stated over one thread's dispatch domain — the NARROW/MCU target.** An
  embedder dispatching from several threads and unsubscribing from another needs a grace
  period spanning every thread. That is `reclaim_qsbr`, ADR-0080's third policy, which is
  **not implemented** ([#894](https://github.com/avatarsd-llc/libtracer/issues/894)) and is
  deliberately not even declared as an alias.
- **Parking is bounded.** Retired pairs sit in a per-thread array sized by
  `kDeferredReleaseSlots`; if every slot is taken the pair is dropped and its hook never
  runs — a deliberate leak in preference to a use-after-free — and
  `graph_t::deferred_release_drops()` counts it.

## Source

```{literalinclude} /core/examples/sub_unsubscribe_from_dispatch.cpp
:language: cpp
:linenos:
```

See also: [reclamation policy](../reference/17-reclamation-policy.md) ·
[configuration](../modules/config.md) · [graph module](../modules/graph.md).
